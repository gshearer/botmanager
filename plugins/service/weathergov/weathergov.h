#ifndef BM_WEATHERGOV_H
#define BM_WEATHERGOV_H

// Public mechanism API lives in weathergov_api.h — consumers include
// that header. This header is service-plugin-internal and is only
// visible inside the plugin's own translation units (which define
// WEATHERGOV_INTERNAL).

#ifdef WEATHERGOV_INTERNAL

#include "clam.h"
#include "common.h"
#include "curl.h"
#include "kv.h"
#include "alloc.h"
#include "json.h"
#include "plugin.h"

#include "weathergov_api.h"

#include <pthread.h>

// Constants

#define WXG_CTX         "weathergov"

// Every URL is built here rather than followed out of a /points
// response: the response hands over four absolute URLs, and storing
// them per cache entry costs four times what the grid triple does.
#define WXG_POINTS_URL  "https://api.weather.gov/points"

// Rounded "%.4f,%.4f" — the coordinate as both the URL tail and the
// point-cache key. Longer forms answer 301 with the rounded one, which
// costs a round trip and a cache key that never matches.
#define WXG_COORD_SZ    32

#define WXG_URL_SZ      256
#define WXG_UA_SZ       128

#define WXG_POINT_CACHE_BUCKETS  64

// Successive chunks add their own WEATHERGOV_* result sizes beside the
// three in weathergov_api.h: alerts (WX-2), forecast periods (WX-3),
// observations (WX-4). §WX-NAMING in the root TODO pins every name.

// Request types and structures

typedef enum
{
  WXG_REQ_POINT
} wxg_req_type_t;

// The caller-supplied completion, one arm per request type. Named
// because both the request and the caller-half snapshot below hold one,
// and an anonymous union in each would be two incompatible types.
typedef union
{
  weathergov_point_cb_t  point;
} wxg_cb_u;

// Request context: carries request parameters and the caller-supplied
// completion through the transfer. No command-surface state (no
// cmd_ctx_t, no method_msg_t) — consumers own their own per-request
// structs.
typedef struct wxg_request
{
  wxg_req_type_t      type;
  double              lat;
  double              lon;
  char                coord[WXG_COORD_SZ];   // rounded, "%.4f,%.4f"
  char                ua[WXG_UA_SZ];

  // Caller callback (union on done-callback shape).
  wxg_cb_u            cb;
  void               *user;

  // Result accumulator. Only the arm matching r->type is populated; it
  // outlives the individual transfer so a chained request (WX-2 onward)
  // can fill it a leg at a time.
  union
  {
    weathergov_point_result_t  point;
  }                   acc;

  // Freelist linkage.
  struct wxg_request *next;

  // In-flight registry linkage — distinct from `next` because the two
  // lists are disjoint in time but the freelist is not the thing the
  // unmap sweep walks. See the registry section in weathergov.c.
  struct wxg_request *next_active;
} wxg_request_t;

// The caller's half of a request, lifted off it under the registry lock
// so a delivery cannot interleave with an unmap sweep. Both arms are
// NULL when the caller was unloaded mid-request.
typedef struct
{
  wxg_cb_u   cb;
  void      *user;
} wxg_caller_t;

// Point cache

// A negative entry (covered == false) is cached exactly like a positive
// one, so a repeat lookup of a non-US coordinate costs nothing either.
typedef struct wxg_pointcache
{
  char                   coord[WXG_COORD_SZ];
  bool                   covered;
  weathergov_point_t     point;
  time_t                 cached_at;
  struct wxg_pointcache *next;
} wxg_pointcache_t;

// Module state

static wxg_request_t     *wxg_free = NULL;
static pthread_mutex_t    wxg_free_mu;

// In-flight registry. Guards `next_active` linkage and every read or
// write of a request's caller half.
static wxg_request_t     *wxg_active_head  = NULL;
static uint32_t           wxg_active_count = 0;
static pthread_mutex_t    wxg_active_mutex = PTHREAD_MUTEX_INITIALIZER;

static wxg_pointcache_t  *wxg_point_cache[WXG_POINT_CACHE_BUCKETS];
static pthread_mutex_t    wxg_point_cache_mu;

// Configuration schema

static const plugin_kv_entry_t wxg_kv_schema[] = {
  { "plugin.weathergov.enabled",         KV_UINT8,  "1",
    "Master switch; 0 = never call weather.gov" },
  { "plugin.weathergov.user_agent",      KV_STR,
    "botmanager/1.0 (+set plugin.weathergov.user_agent)",
    "User-Agent sent to weather.gov; NWS asks that it name a contact" },
  { "plugin.weathergov.timeout_secs",    KV_UINT32, "8",
    "Per-request timeout in seconds" },
  { "plugin.weathergov.point_cache_ttl", KV_UINT32, "604800",
    "Grid-coordinate cache time-to-live in seconds" },
};

// Forward declarations

static wxg_request_t     *wxg_req_alloc(void);
static void               wxg_req_release(wxg_request_t *r);
static void               wxg_req_track(wxg_request_t *r);
static void               wxg_req_untrack_locked(wxg_request_t *r);
static void               wxg_req_take_caller(wxg_request_t *r,
                              wxg_caller_t *out);
static void               wxg_unmap_cb(uintptr_t lo, uintptr_t hi,
                              void *data);

// Shared plumbing — HTTP, JSON, time. Every TU in this plugin uses it.
static void               wxg_ua(char *out, size_t sz);
static bool               wxg_http_get(const char *url, const char *ua,
                              curl_done_cb_t cb, void *user);
static bool               wxg_http_status_ok(const curl_response_t *resp,
                              char *err, size_t err_sz, bool *covered);
static time_t             wxg_parse_iso8601(const char *s,
                              int32_t *tz_off_out);

// Point resolution.
static wxg_pointcache_t  *wxg_point_lookup(const char *coord);
static void               wxg_point_insert(const char *coord, bool covered,
                              const weathergov_point_t *pt);
static bool               wxg_point_parse(struct json_object *root,
                              weathergov_point_t *out);
static void               wxg_point_deliver(wxg_request_t *r);
static void               wxg_point_deliver_err(wxg_request_t *r,
                              const char *msg);
static void               wxg_point_done(const curl_response_t *resp);

static bool               wxg_init(void);
static void               wxg_deinit(void);

#endif // WEATHERGOV_INTERNAL

#endif // BM_WEATHERGOV_H
