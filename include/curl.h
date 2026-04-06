#ifndef BM_CURL_H
#define BM_CURL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// HTTP methods.
typedef enum
{
  CURL_METHOD_GET,
  CURL_METHOD_POST,
  CURL_METHOD_PUT,
  CURL_METHOD_DELETE,
  CURL_METHOD_PATCH
} curl_method_t;

// Opaque request handle.
typedef struct curl_request curl_request_t;

// Response delivered to completion callback. Valid for the duration
// of the callback only -- caller must copy any data they need.
typedef struct
{
  curl_request_t *request;      // originating request
  long            status;       // HTTP status code (0 on transport error)
  const char     *body;         // response body (NUL-terminated)
  size_t          body_len;     // response body length in bytes
  const char     *content_type; // Content-Type header value (or NULL)
  int             curl_code;    // CURLcode (0 = CURLE_OK)
  const char     *error;        // human-readable error (NULL on success)
  void           *user_data;    // caller's opaque pointer
} curl_response_t;

// Completion callback type. Invoked on the curl multi worker thread.
// Callbacks must be fast and non-blocking.
typedef void (*curl_done_cb_t)(const curl_response_t *resp);

// Create a new HTTP request. Does not execute yet.
// returns: request handle, or NULL on failure
// method: HTTP method
// url: full URL (copied internally)
// cb: completion callback
// user_data: opaque data passed to callback
curl_request_t *curl_request_create(curl_method_t method, const char *url,
    curl_done_cb_t cb, void *user_data);

// Set request body. Must be called before curl_request_submit().
// returns: SUCCESS or FAIL
// req: request handle
// content_type: Content-Type header value (e.g., "application/json")
// body: request body data (copied internally)
// body_len: length of body in bytes
bool curl_request_set_body(curl_request_t *req, const char *content_type,
    const char *body, size_t body_len);

// Add a custom header. May be called multiple times before submit.
// returns: SUCCESS or FAIL
// req: request handle
// header: header line (e.g., "Authorization: Bearer xxx")
bool curl_request_add_header(curl_request_t *req, const char *header);

// Set per-request timeout, overriding the KV default.
// returns: SUCCESS or FAIL
// req: request handle
// timeout_secs: timeout in seconds (0 = use default)
bool curl_request_set_timeout(curl_request_t *req, uint32_t timeout_secs);

// Set per-request User-Agent, overriding the KV default.
// returns: SUCCESS or FAIL
// req: request handle
// ua: User-Agent string (copied internally, NULL = use default)
bool curl_request_set_user_agent(curl_request_t *req, const char *ua);

// Submit a request for async execution. Thread-safe; may be called
// from any thread. Ownership transfers to the curl subsystem -- the
// caller must not use req after this call.
// returns: SUCCESS or FAIL (subsystem not ready, or queue full)
// req: request handle
bool curl_request_submit(curl_request_t *req);

// Convenience: create and submit a GET request in one call.
// returns: SUCCESS or FAIL
// url: full URL
// cb: completion callback
// user_data: opaque data
bool curl_get(const char *url, curl_done_cb_t cb, void *user_data);

// Convenience: create and submit a POST request in one call.
// returns: SUCCESS or FAIL
// url: full URL
// content_type: Content-Type header value
// body: request body
// body_len: body length
// cb: completion callback
// user_data: opaque data
bool curl_post(const char *url, const char *content_type,
    const char *body, size_t body_len,
    curl_done_cb_t cb, void *user_data);

// Curl subsystem statistics.
typedef struct
{
  uint32_t active;          // in-flight requests
  uint32_t queued;          // requests waiting in submit queue
  uint64_t total_requests;  // lifetime total
  uint64_t total_errors;    // lifetime transport errors
  uint64_t bytes_in;        // total response bytes received
  uint64_t bytes_out;       // total request bytes sent
  uint64_t total_response_ms; // cumulative response time in milliseconds
} curl_stats_t;

// Get curl subsystem statistics (thread-safe snapshot).
// out: destination for the snapshot
void curl_get_stats(curl_stats_t *out);

// returns: human-readable name of an HTTP method
// method: method enum value
const char *curl_method_name(curl_method_t method);

// Initialize the curl subsystem. Must be called after pool_init().
void curl_init(void);

// Register KV configuration keys and load values.
// Must be called after kv_init() and plugin_init_all().
void curl_register_config(void);

// Shut down the curl subsystem. The multi worker thread must already
// be joined (via pool_exit) before calling this.
void curl_exit(void);

// Active request iteration callback type. Invoked once per in-flight
// request while the submit queue lock is held — must be fast.
typedef void (*curl_iter_cb_t)(const char *url, curl_method_t method,
    uint32_t elapsed_secs, void *data);

// Iterate active and queued curl requests. Thread-safe.
// cb: callback invoked for each request
// data: opaque user data passed to callback
void curl_iterate_active(curl_iter_cb_t cb, void *data);

#ifdef CURL_INTERNAL

#include "common.h"
#include "clam.h"
#include "kv.h"
#include "mem.h"
#include "pool.h"
#include "task.h"

#include <curl/curl.h>
#include <errno.h>
#include <sys/eventfd.h>
#include <unistd.h>

// Configuration defaults.
#define CURL_DEF_TIMEOUT         30
#define CURL_DEF_CONNECT_TIMEOUT 10
#define CURL_DEF_MAX_ACTIVE      32
#define CURL_DEF_MAX_QUEUED      256
#define CURL_DEF_MAX_RESP_SZ     (4 * 1024 * 1024)   // 4 MiB
#define CURL_DEF_POLL_TIMEOUT    500
#define CURL_DEF_MAX_CONNS       64
#define CURL_DEF_MAX_HOST_CONNS  8
#define CURL_DEF_VERBOSE         0

#define CURL_DEF_USER_AGENT      "libp0ada/2.0"

#define CURL_RESP_INIT_CAP       4096

#define CURL_URL_SZ           2048
#define CURL_CT_SZ            128
#define CURL_UA_SZ            128

// Request states (internal lifecycle).
typedef enum
{
  CURL_REQ_CREATED,       // created, not yet submitted
  CURL_REQ_QUEUED,        // in the submit queue, awaiting multi_add
  CURL_REQ_ACTIVE,        // added to curl_multi, transfer in progress
  CURL_REQ_DONE           // transfer complete, callback delivered
} curl_req_state_t;

// Custom header list node.
typedef struct curl_hdr
{
  char            *value;
  struct curl_hdr *next;
} curl_hdr_t;

// Request structure.
struct curl_request
{
  curl_method_t       method;
  curl_req_state_t    state;
  char                url[CURL_URL_SZ];
  uint32_t            timeout_secs;     // 0 = use default
  char                user_agent[CURL_UA_SZ]; // empty = use default

  // Request body (heap-allocated, owned by request).
  char               *req_body;
  size_t              req_body_len;
  char                content_type[CURL_CT_SZ];

  // Custom headers.
  curl_hdr_t         *headers;
  struct curl_slist  *curl_headers;     // built at submit time

  // Response accumulator (grown via mem_realloc).
  char               *resp_body;
  size_t              resp_body_len;
  size_t              resp_body_cap;

  // Callback.
  curl_done_cb_t      cb;
  void               *cb_data;

  // libcurl easy handle (created at submit, owned by multi).
  CURL               *easy;
  char                curl_errbuf[CURL_ERROR_SIZE];

  // Submit queue linkage.
  struct curl_request *next;
};

// Cached configuration values (from KV).
typedef struct
{
  uint32_t timeout;           // default request timeout (seconds)
  uint32_t connect_timeout;   // connection timeout (seconds)
  uint32_t max_active;        // max concurrent transfers
  uint32_t max_queued;        // max pending in submit queue
  uint32_t max_response_sz;   // max response body size (bytes)
  uint32_t poll_timeout;      // multi poll timeout (ms)
  uint32_t max_conns;         // max total connections in pool
  uint32_t max_host_conns;    // max connections per host
  uint32_t verbose;           // enable curl verbose logging
  char     user_agent[CURL_UA_SZ]; // default User-Agent string
} curl_cfg_t;

// Module state.
static CURLM             *curl_multi_handle = NULL;
static task_t            *curl_task         = NULL;
static int                curl_wake_fd      = -1;
static bool               curl_ready        = false;
static curl_cfg_t         curl_cfg;

// Submit queue: requests added from any thread, consumed by multi loop.
static curl_request_t    *curl_submit_head  = NULL;
static curl_request_t    *curl_submit_tail  = NULL;
static uint32_t           curl_submit_count = 0;
static pthread_mutex_t    curl_submit_mutex;

// Active count (only touched by multi loop thread).
static uint32_t           curl_active_count = 0;

// Statistics.
static uint64_t           curl_stat_total   = 0;
static uint64_t           curl_stat_errors  = 0;
static uint64_t           curl_stat_in      = 0;
static uint64_t           curl_stat_out     = 0;
static uint64_t           curl_stat_time_ms = 0;

// Request freelist.
static curl_request_t    *curl_req_free     = NULL;
static pthread_mutex_t    curl_req_mutex;

// Forward declarations.
static void    curl_multi_loop(task_t *t);
static void    curl_drain_queue(void);
static void    curl_finish_request(curl_request_t *req, CURLcode result);
static void    curl_request_release(curl_request_t *req);
static size_t  curl_write_cb(char *ptr, size_t size, size_t nmemb,
                   void *userdata);
static void    curl_register_kv(void);
static void    curl_load_config(void);

#endif // CURL_INTERNAL

#endif // BM_CURL_H
