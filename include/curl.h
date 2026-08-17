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

// Per-request priority. Lower value = higher priority. The byte values
// are deliberately aligned with feature_exchange's EXCHANGE_PRIO_*
// constants so a passed-through priority byte means the same thing on
// both sides (CURL-PRIO-3 will trickle exchange's priority straight
// through).
typedef enum
{
  CURL_PRIO_TRANSACTIONAL = 0,    // must-drain on shutdown
  CURL_PRIO_NORMAL        = 50,   // default; recoverable on retry
  CURL_PRIO_BULK          = 254,  // long / idempotent; first to drop
  CURL_PRIO__COUNT        = 3
} curl_prio_t;

// Bound on curl_response_t.set_cookie — public because a caller that
// keeps the value past the callback needs to size its own buffer.
#define CURL_COOKIE_SZ        1024

// Bounds on the per-request captured-header set. Public because a
// caller that keeps a captured value past the callback needs to size
// its own buffer. See curl_request_capture_header.
#define CURL_CAPTURE_MAX      4
#define CURL_CAPTURE_NAME_SZ  32
#define CURL_CAPTURE_VALUE_SZ 128

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
  const char     *set_cookie;   // every Set-Cookie cookie-pair seen on
                                // the transfer, joined as "a=1; b=2" and
                                // ready to hand back as a Cookie: request
                                // header -- attributes (Path, Expires,
                                // HttpOnly, ...) are dropped (or NULL)
  int             curl_code;    // CURLcode (0 = CURLE_OK)
  const char     *error;        // human-readable error (NULL on success)
  void           *user_data;    // caller's opaque pointer
  bool            cancelled;    // true if cancelled by curl_begin_shutdown
                                // (shutdown drain) — distinct from a
                                // generic transport error so callers
                                // don't re-enter the retry path
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

// Retain the value of response header `name` so the completion
// callback can read it back with curl_response_header(). The match is
// case-insensitive and, for a header the server repeats, last-write-
// wins; `name` is copied and must carry no colon. CREATED state only.
//
// FAIL when the request is past CREATED, the set is already
// CURL_CAPTURE_MAX deep, or `name` does not fit
// CURL_CAPTURE_NAME_SZ. Values longer than CURL_CAPTURE_VALUE_SZ are
// truncated, not refused.
//
// ETag, Last-Modified and Set-Cookie are captured unconditionally and
// surfaced as named fields on curl_response_t — never spend a slot on
// those three.
bool curl_request_capture_header(curl_request_t *req, const char *name);

// The captured value of `name` on a completed transfer. NULL when the
// header was never requested, or was requested and the server did not
// send it — so a non-NULL return always carries a real value. Valid
// for the duration of the completion callback only.
const char *curl_response_header(const curl_response_t *resp,
    const char *name);

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

// Set the request's priority. CREATED-state only. Default after
// curl_request_create is CURL_PRIO_NORMAL. Returns FAIL if state is
// not CREATED, or prio is not one of the named enum values.
bool curl_request_set_prio(curl_request_t *req, curl_prio_t prio);

// Thread-safe; may be called from any thread. Ownership transfers to
// the curl subsystem -- the caller must not use req after this call.
bool curl_request_submit(curl_request_t *req);

// Backpressure variant of curl_request_submit. When the submit queue
// is full, waits up to wait_ms for the drain thread to free a slot
// instead of returning FAIL immediately. Designed for bulk pipelines
// (e.g. knowledge corpus ingest) where the caller prefers backpressure
// to silent drops.
//
// Does NOT log a queue-full WARN -- the whole point is to wait
// quietly; only the timeout is worth a line. On successful enqueue,
// ownership transfers to the curl subsystem as usual.
//
// Returns FAIL when the wait expires, when the subsystem is shutting
// down, and on a malformed request; on FAIL the request is released
// internally. `wait_ms` of 0 does not wait at all -- it is the
// fast-fail submit without the WARN.
//
// The bound is the caller's whole protection: nothing else here
// shortens the wait, and the queue only drains as fast as the slowest
// endpoint ahead of it lets it. Never pass a bound you are not
// prepared to sit through on the thread you are on.
//
// Thread-safe. May be called from any thread except the curl multi
// loop's own worker (that would deadlock).
bool curl_request_submit_wait(curl_request_t *req, uint32_t wait_ms);

// The request's stable identity, for curl_request_cancel. Read it
// before curl_request_submit — ownership of the handle transfers
// there and the pointer stops being the caller's. Ids are never
// reused, so one that has been delivered simply matches nothing.
uint64_t curl_request_id(const curl_request_t *req);

// Give back a request that was built and will now never be sent. The
// only way to free a CREATED request without submitting it — every
// other path out of curl_request_create ends in a submit, which owns
// the handle from then on. FAIL when the request is past CREATED, where
// the subsystem owns it and this call would double-free.
bool curl_request_abandon(curl_request_t *req);

// Ask the subsystem to abandon request `id`. Thread-safe and
// non-blocking: the request is flagged and the multi loop finishes it
// with CURLE_ABORTED_BY_CALLBACK, so the completion callback still
// fires — on the multi-loop thread, with resp.cancelled true, exactly
// as the shutdown drain delivers one. A caller tearing down therefore
// waits for its own callback, not for the transfer.
//
// returns: SUCCESS when a queued or in-flight request carried `id` and
// is now flagged; FAIL when none did.
//
// A request is briefly reachable by neither walk — between leaving the
// submit queue and entering the in-flight list, while the multi loop
// builds its easy handle. A caller that must be certain re-issues the
// cancel while it waits; that is what llm's stop-drain does.
bool curl_request_cancel(uint64_t id);

bool curl_get(const char *url, curl_done_cb_t cb, void *user_data);

bool curl_post(const char *url, const char *content_type,
    const char *body, size_t body_len,
    curl_done_cb_t cb, void *user_data);

typedef struct
{
  uint32_t active;          // in-flight requests (instantaneous)
  uint32_t queued;          // requests waiting in submit queue (instantaneous)
  uint32_t active_peak;     // high-water mark of concurrent in-flight
  uint32_t queued_peak;     // high-water mark of submit-queue depth
  uint32_t max_active;      // configured concurrency ceiling
  uint32_t max_queued;      // configured submit-queue ceiling
  uint64_t submit_rejected; // submits refused because the queue was full
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

// Initiate the orderly shutdown drain. Must be called BEFORE pool_exit
// (the curl multi worker thread does the drain work). After this call:
//   - new submits are refused
//   - queued + in-flight non-TRANSACTIONAL requests are cancelled and
//     their completion callbacks fire with curl_code = CURLE_ABORTED_
//     BY_CALLBACK so plugins waiting on those callbacks (e.g. the
//     whenmoon downloader's per-page completions) can finish their
//     own teardown
//   - in-flight TRANSACTIONAL requests are given up to
//     CURL_DRAIN_DEADLINE_MS to complete; remaining are force-cancelled
//     with a CLAM_WARN when the deadline expires
// Idempotent. Returns when the drain has completed (or its deadline
// has fired).
void curl_begin_shutdown(void);

// The multi worker thread must already be joined (via pool_exit)
// before calling this.
void curl_exit(void);

// One request, as seen by curl_iterate_active. The callback pointers
// are what the request retains on the submitter's behalf: for a request
// submitted by a plugin they point into that plugin's mapping, which is
// what makes them the quiescence test (see plugin_owns_ptr).
typedef struct
{
  const char     *url;
  curl_method_t   method;
  uint32_t        elapsed_secs;
  bool            in_flight;    // false = still queued, not yet dispatched
  // True while this thread is *inside* the submitter's completion
  // callback — the only state in which the request holds the
  // submitter's code and locks, and the one an unload must wait out
  // before that plugin's deinit() tears them down.
  bool            delivering;
  curl_done_cb_t  cb;
  void           *cb_data;
  curl_chunk_cb_t chunk_cb;     // NULL unless streaming
  void           *chunk_user;
} curl_iter_req_t;

// Invoked once per in-flight and once per queued request, while the
// corresponding lock is held — must be fast and must not re-enter curl_*.
typedef void (*curl_iter_cb_t)(const curl_iter_req_t *req, void *data);

void curl_iterate_active(curl_iter_cb_t cb, void *data);

#ifdef CURL_INTERNAL

#include "common.h"
#include "clam.h"
#include "kv.h"
#include "alloc.h"
#include "pool.h"
#include "task.h"
#include "util.h"     // util_redact_url — no request URL reaches a log raw

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

// Wall-clock budget for the in-flight TRANSACTIONAL drain inside
// curl_begin_shutdown. Non-TRANSACTIONAL cancellation completes
// synchronously and isn't subject to this budget.
#define CURL_DRAIN_DEADLINE_MS   10000

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
  // Identity that outlives the pointer: the struct is freelist-backed,
  // so an address recycles and an id does not. curl_request_cancel
  // matches on this.
  uint64_t            id;

  // Set by curl_request_cancel from any thread, acted on by the multi
  // loop's cancellation sweep. Atomic because the loop reads it during
  // its own unlocked traversal of the in-flight list.
  _Atomic bool        cancel_requested;

  curl_method_t       method;

  // _Atomic on the declaration, per the tree's cross-thread-scalar
  // ruling (core/AGENTS.md §Patterns): written by the multi loop when a
  // transfer ends, read by an unloading thread that needs to know
  // whether this request is on the wire or *inside* the submitter's
  // completion callback. No reader changed to gain it.
  _Atomic curl_req_state_t state;
  curl_prio_t         prio;
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
  // as `etag` / `last_modified` / `set_cookie`. Empty string means "not
  // present". Set-Cookie is the one header that legitimately repeats, so
  // its pairs accumulate (across redirect hops too) until the buffer is
  // full; the others are last-write-wins.
  char                resp_etag          [128];
  char                resp_last_modified [64];
  char                resp_set_cookie    [CURL_COOKIE_SZ];

  // Response headers the submitter asked to keep beyond those three,
  // via curl_request_capture_header. Names are matched case-
  // insensitively and a repeated header is last-write-wins; an empty
  // value means the server never sent it. Read back through
  // curl_response_header, which is why the set is bounded and inline —
  // a captured header is a handful of small values, not a dictionary.
  struct
  {
    char name  [CURL_CAPTURE_NAME_SZ];
    char value [CURL_CAPTURE_VALUE_SZ];
  } captures[CURL_CAPTURE_MAX];
  uint8_t             capture_count;

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

// _Atomic for the same reason as sock_cfg_t: curl_load_config() runs on
// a command thread and every reader here is the multi loop or a drain
// (measured, TSan 2026-08-15). user_agent is not a scalar and cannot be
// covered this way — it is rewritten in place while the drain reads it,
// an unmeasured sibling of the same race.
typedef struct
{
  _Atomic uint32_t timeout;           // default request timeout (seconds)
  _Atomic uint32_t connect_timeout;   // connection timeout (seconds)
  _Atomic uint32_t max_active;        // max concurrent transfers
  _Atomic uint32_t max_queued;        // max pending in submit queue
  _Atomic uint32_t max_response_sz;   // max response body size (bytes)
  _Atomic uint32_t poll_timeout;      // multi poll timeout (ms)
  _Atomic uint32_t max_conns;         // max total connections in pool
  _Atomic uint32_t max_host_conns;    // max connections per host
  _Atomic uint32_t verbose;           // enable curl verbose logging
  char             user_agent[CURL_UA_SZ]; // default User-Agent string
} curl_cfg_t;

// Owned by the multi loop thread from creation to cleanup. A config
// change used to reach in and curl_multi_setopt() it from the command
// thread -- a data race on the pointer AND concurrent use of a handle
// libcurl only allows one thread to drive. The loop applies the two
// connection limits itself when this flag is set.
static CURLM             *curl_multi_handle = NULL;
static _Atomic bool       curl_conn_opts_dirty = false;
static task_handle_t      curl_task         = TASK_HANDLE_NONE;
static int                curl_wake_fd      = -1;
static bool               curl_ready        = false;
static curl_cfg_t         curl_cfg;

// Submit queue: per-priority FIFO sub-queues. Drained in priority
// order (TRANSACTIONAL first, BULK last). Indexed via
// curl_prio_to_idx; back-pressure is global (curl_submit_total
// against curl_cfg.max_queued).
typedef struct
{
  curl_request_t *head;
  curl_request_t *tail;
  uint32_t        count;
} curl_submit_q_t;

static curl_submit_q_t    curl_submit_qs[CURL_PRIO__COUNT];
static uint32_t           curl_submit_total = 0;
static pthread_mutex_t    curl_submit_mutex;
// Signalled whenever the submit queue's fill drops (drain thread runs,
// subsystem shuts down) so threads blocked in curl_request_submit_wait
// can re-check capacity without polling or generating log noise.
// Carries a CLOCK_MONOTONIC attr, unlike every other condition in this
// tree: waiters on it hold a deadline that is the caller's only bound,
// and a settime or an NTP step must not be able to extend it.
static pthread_cond_t     curl_slot_cond;

// Shutdown drain bookkeeping. Set by curl_begin_shutdown (any thread);
// observed by the multi loop, which performs the cancellation work and
// signals curl_drain_cond when finished. curl_shutting_down is checked
// by curl_request_submit / _submit_wait to refuse new traffic from
// every thread once the drain is initiated. All three are _Atomic and
// not volatile: the multi loop reads them holding nothing while the
// shutdown path writes them holding curl_submit_mutex (measured, TSan
// 2026-08-15), and volatile promises ordering to no one.
static _Atomic bool       curl_shutting_down   = false;
static _Atomic bool       curl_drain_initiated = false;
static _Atomic bool       curl_drain_complete  = false;
static pthread_cond_t     curl_drain_cond;

// In-flight bookkeeping. curl_active_qs is per-priority so the
// shutdown drain can wait on TRANSACTIONAL specifically;
// curl_active_head is the singly-linked in-flight list (chained via
// curl_request.next while the request is in CURL_REQ_ACTIVE) used by
// the drain to enumerate non-TRANSACTIONAL handles for cancellation.
//
// The multi loop thread is the only *writer* of both, but external
// readers exist (curl_iterate_active, and through it the plugin
// teardown audit), so every list mutation takes curl_active_mutex.
// The multi loop's own read-only traversals do not — it races nobody.
// Never nested with curl_submit_mutex in either direction.
static uint32_t           curl_active_qs[CURL_PRIO__COUNT] = {0};
static curl_request_t    *curl_active_head = NULL;
static pthread_mutex_t    curl_active_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t           curl_stat_total   = 0;
static uint64_t           curl_stat_errors  = 0;
static uint64_t           curl_stat_in      = 0;
static uint64_t           curl_stat_out     = 0;
static uint64_t           curl_stat_time_ms = 0;

// High-water marks + saturation telemetry (lifetime, since subsystem
// start). Peaks answer "did we ever approach the configured ceilings?";
// curl_submit_rejected counts submits dropped because the queue was
// already at max_queued. curl_active_peak is updated on the multi-loop
// thread; curl_queued_peak / curl_submit_rejected under curl_submit_mutex.
static uint32_t           curl_active_peak     = 0;
static uint32_t           curl_queued_peak     = 0;
static uint64_t           curl_submit_rejected = 0;

static curl_request_t    *curl_req_free     = NULL;
static pthread_mutex_t    curl_req_mutex;
// Handed out under curl_req_mutex, one per created request. Starts at
// 1 so a zeroed struct never carries a live id.
static uint64_t           curl_req_next_id  = 0;

static void    curl_multi_loop(task_t *t);
static void    curl_drain_queue(void);
static void    curl_finish_request(curl_request_t *req, CURLcode result);
static void    curl_finish_cancelled(curl_request_t *req);
static void    curl_run_shutdown_drain(void);
static void    curl_sweep_cancelled(void);
static void    curl_request_release(curl_request_t *req);
static size_t  curl_write_cb(char *ptr, size_t size, size_t nmemb,
                   void *userdata);
static size_t  curl_header_cb(char *buf, size_t size, size_t nitems,
                   void *userdata);
static void    curl_register_kv(void);
static void    curl_load_config(void);

#endif // CURL_INTERNAL

#endif // BM_CURL_H
