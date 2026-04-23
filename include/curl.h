#ifndef BM_CURL_H
#define BM_CURL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
  CURL_METHOD_GET,
  CURL_METHOD_POST,
  CURL_METHOD_PUT,
  CURL_METHOD_DELETE,
  CURL_METHOD_PATCH
} curl_method_t;

typedef struct curl_request curl_request_t;

// Delivered to completion callback. Valid for the duration of the
// callback only -- caller must copy any data they need.
typedef struct
{
  curl_request_t *request;      // originating request
  long            status;       // HTTP status code (0 on transport error)
  const char     *body;         // response body (NUL-terminated)
  size_t          body_len;     // response body length in bytes
  const char     *content_type; // Content-Type header value (or NULL)
  const char     *etag;         // ETag header value (or NULL)
  const char     *last_modified;// Last-Modified header value (or NULL)
  int             curl_code;    // CURLcode (0 = CURLE_OK)
  const char     *error;        // human-readable error (NULL on success)
  void           *user_data;    // caller's opaque pointer
} curl_response_t;

// Invoked on the curl multi worker thread. Callbacks must be fast and
// non-blocking.
typedef void (*curl_done_cb_t)(const curl_response_t *resp);

// Per-chunk callback for streaming responses. Invoked on the curl multi
// worker thread as body bytes arrive. partial->body/body_len reflect
// what has accumulated so far (NULL/0 if accumulate disabled). chunk
// points to the newly received bytes only. Must be fast, non-blocking.
typedef void (*curl_chunk_cb_t)(const curl_response_t *partial,
    const char *chunk, size_t chunk_len, void *user_data);

// url is copied internally.
curl_request_t *curl_request_create(curl_method_t method, const char *url,
    curl_done_cb_t cb, void *user_data);

// Must be called before curl_request_submit(). body is copied internally.
bool curl_request_set_body(curl_request_t *req, const char *content_type,
    const char *body, size_t body_len);

// May be called multiple times before submit.
bool curl_request_add_header(curl_request_t *req, const char *header);

// timeout_secs of 0 uses the KV default.
bool curl_request_set_timeout(curl_request_t *req, uint32_t timeout_secs);

// ua is copied internally; NULL uses the KV default.
bool curl_request_set_user_agent(curl_request_t *req, const char *ua);

// The done callback still fires at transfer end with final status.
// Safe to call before submit only. cb NULL clears.
bool curl_request_set_chunk_cb(curl_request_t *req,
    curl_chunk_cb_t cb, void *user_data);

// Default true. Set false to skip growing resp_body — the chunk
// callback becomes the sole data sink, and the 4 MiB response cap is
// bypassed (useful for long streams). The done callback will still
// fire with partial->body == NULL / len == 0. Request must be in
// CREATED state.
bool curl_request_set_accumulate(curl_request_t *req, bool accumulate);

// Default: true (follow Location: redirects). Set false for a
// security-sensitive fetch that must not be redirected to a new
// origin. Safe to call before submit only.
bool curl_request_set_follow_redirects(curl_request_t *req, bool follow);

// Thread-safe; may be called from any thread. Ownership transfers to
// the curl subsystem -- the caller must not use req after this call.
bool curl_request_submit(curl_request_t *req);

// Blocking variant of curl_request_submit. When the submit queue is
// full, waits on an internal condition variable instead of returning
// FAIL. Wakes when the drain thread frees slots. Designed for bulk
// pipelines (e.g. knowledge corpus ingest) where the caller prefers
// backpressure to silent drops and has the luxury of a dedicated
// worker thread.
//
// Does NOT log a queue-full WARN -- the whole point is to wait
// quietly. On successful enqueue, ownership transfers to the curl
// subsystem as usual. Returns FAIL only when the subsystem itself is
// shutting down (curl_ready goes false); on FAIL the request is
// released internally.
//
// Thread-safe. May be called from any thread except the curl multi
// loop's own worker (that would deadlock).
bool curl_request_submit_wait(curl_request_t *req);

bool curl_get(const char *url, curl_done_cb_t cb, void *user_data);

bool curl_post(const char *url, const char *content_type,
    const char *body, size_t body_len,
    curl_done_cb_t cb, void *user_data);

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

void curl_get_stats(curl_stats_t *out);

const char *curl_method_name(curl_method_t method);

// Must be called after pool_init().
void curl_init(void);

// Must be called after kv_init() and plugin_init_all().
void curl_register_config(void);

// The multi worker thread must already be joined (via pool_exit)
// before calling this.
void curl_exit(void);

// Invoked once per in-flight request while the submit queue lock is
// held — must be fast.
typedef void (*curl_iter_cb_t)(const char *url, curl_method_t method,
    uint32_t elapsed_secs, void *data);

void curl_iterate_active(curl_iter_cb_t cb, void *data);

#ifdef CURL_INTERNAL

#include "common.h"
#include "clam.h"
#include "kv.h"
#include "alloc.h"
#include "pool.h"
#include "task.h"

#include <curl/curl.h>
#include <errno.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define CURL_DEF_TIMEOUT         30
#define CURL_DEF_CONNECT_TIMEOUT 10
#define CURL_DEF_MAX_ACTIVE      32
#define CURL_DEF_MAX_QUEUED      256
#define CURL_DEF_MAX_RESP_SZ     (10 * 1024 * 1024)  // 10 MiB
#define CURL_DEF_POLL_TIMEOUT    500
#define CURL_DEF_MAX_CONNS       64
#define CURL_DEF_MAX_HOST_CONNS  8
#define CURL_DEF_VERBOSE         0

#define CURL_DEF_USER_AGENT      "libp0ada/2.0"

#define CURL_RESP_INIT_CAP       4096

#define CURL_URL_SZ           2048
#define CURL_CT_SZ            128
#define CURL_UA_SZ            128

typedef enum
{
  CURL_REQ_CREATED,       // created, not yet submitted
  CURL_REQ_QUEUED,        // in the submit queue, awaiting multi_add
  CURL_REQ_ACTIVE,        // added to curl_multi, transfer in progress
  CURL_REQ_DONE           // transfer complete, callback delivered
} curl_req_state_t;

typedef struct curl_hdr
{
  char            *value;
  struct curl_hdr *next;
} curl_hdr_t;

struct curl_request
{
  curl_method_t       method;
  curl_req_state_t    state;
  char                url[CURL_URL_SZ];
  uint32_t            timeout_secs;     // 0 = use default
  char                user_agent[CURL_UA_SZ]; // empty = use default

  // Heap-allocated, owned by request.
  char               *req_body;
  size_t              req_body_len;
  char                content_type[CURL_CT_SZ];

  curl_hdr_t         *headers;
  struct curl_slist  *curl_headers;     // built at submit time

  // Grown via mem_realloc.
  char               *resp_body;
  size_t              resp_body_len;
  size_t              resp_body_cap;

  // Captured response headers of interest. Populated by the header
  // callback during transfer; surfaced on the delivered curl_response_t
  // as `etag` / `last_modified`. Empty string means "not present".
  char                resp_etag          [128];
  char                resp_last_modified [64];

  curl_done_cb_t      cb;
  void               *cb_data;

  curl_chunk_cb_t     chunk_cb;
  void               *chunk_user;
  bool                accumulate;   // default true
  bool                follow_redirects; // default true

  // Created at submit, owned by multi.
  CURL               *easy;
  char                curl_errbuf[CURL_ERROR_SIZE];

  struct curl_request *next;
};

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
// Signalled whenever the submit queue's fill drops (drain thread runs,
// subsystem shuts down) so threads blocked in curl_request_submit_wait
// can re-check capacity without polling or generating log noise.
static pthread_cond_t     curl_slot_cond;

// Only touched by multi loop thread.
static uint32_t           curl_active_count = 0;

static uint64_t           curl_stat_total   = 0;
static uint64_t           curl_stat_errors  = 0;
static uint64_t           curl_stat_in      = 0;
static uint64_t           curl_stat_out     = 0;
static uint64_t           curl_stat_time_ms = 0;

static curl_request_t    *curl_req_free     = NULL;
static pthread_mutex_t    curl_req_mutex;

static void    curl_multi_loop(task_t *t);
static void    curl_drain_queue(void);
static void    curl_finish_request(curl_request_t *req, CURLcode result);
static void    curl_request_release(curl_request_t *req);
static size_t  curl_write_cb(char *ptr, size_t size, size_t nmemb,
                   void *userdata);
static size_t  curl_header_cb(char *buf, size_t size, size_t nitems,
                   void *userdata);
static void    curl_register_kv(void);
static void    curl_load_config(void);

#endif // CURL_INTERNAL

#endif // BM_CURL_H
