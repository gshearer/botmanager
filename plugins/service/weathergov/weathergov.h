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

// Alerts take the coordinate directly — no grid, no point lookup, no
// prior call. ⚠ `?limit=` is rejected by the endpoint (400): the whole
// active set arrives or nothing does.
#define WXG_ALERTS_URL  "https://api.weather.gov/alerts/active"

// Everything keyed on a forecast grid hangs off here: the 14-period
// forecast (WX-3), the 156-period hourly one and the raw 59-parameter
// timeseries (neither of which we ask for — root TODO §WX-NOTPLANNED).
#define WXG_GRID_URL    "https://api.weather.gov/gridpoints"

// Rounded "%.4f,%.4f" — the coordinate as both the URL tail and the
// point-cache key. Longer forms answer 301 with the rounded one, which
// costs a round trip and a cache key that never matches.
#define WXG_COORD_SZ    32

// A forecast is addressed by grid rather than by coordinate, so
// "ILN/39,49" is what its log lines and its cache key would name.
#define WXG_GRID_LABEL_SZ  24

#define WXG_URL_SZ      256
#define WXG_UA_SZ       128

#define WXG_POINT_CACHE_BUCKETS  64

// A single response carried 11 simultaneous alerts over Ohio on a
// storm day; the parse is bounded well above that and the reply set is
// bounded at WEATHERGOV_ALERT_MAX. Everything between the two bounds
// still counts toward `total`, it simply does not survive into the
// eight slots a caller sees.
#define WXG_ALERT_FEATURE_MAX    64   // features[] entries examined
#define WXG_ALERT_LABEL_MAX      32   // event+area fingerprints remembered
#define WXG_ALERT_REF_MAX        32   // superseded ids remembered

// Successive chunks add their own WEATHERGOV_* result sizes beside the
// ones in weathergov_api.h: observations (WX-4). §WX-NAMING in the root
// TODO pins every name.

// Request types and structures

typedef enum
{
  WXG_REQ_POINT,
  WXG_REQ_ALERTS,
  WXG_REQ_FORECAST
} wxg_req_type_t;

// The caller-supplied completion, one arm per request type. Named
// because both the request and the caller-half snapshot below hold one,
// and an anonymous union in each would be two incompatible types.
typedef union
{
  weathergov_point_cb_t    point;
  weathergov_alerts_cb_t   alerts;
  weathergov_forecast_cb_t forecast;
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
  char                grid[WXG_GRID_LABEL_SZ]; // "ILN/39,49" — grid requests
  char                ua[WXG_UA_SZ];

  // Caller callback (union on done-callback shape).
  wxg_cb_u            cb;
  void               *user;

  // Result accumulator. Only the arm matching r->type is populated; it
  // outlives the individual transfer so a chained request (WX-2 onward)
  // can fill it a leg at a time.
  union
  {
    weathergov_point_result_t     point;
    weathergov_alert_result_t     alerts;
    weathergov_forecast_result_t  forecast;
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

// ----------------------------------------------------------------------
// Shared plumbing — every translation unit in this plugin uses it, so
// none of it is static. Written once in weathergov.c; there is never a
// second HTTP path, time parser or freelist in this plugin.
// ----------------------------------------------------------------------

wxg_request_t *wxg_req_alloc(void);
void           wxg_req_release(wxg_request_t *r);

// File a request BEFORE anything can be submitted, never after: a
// completion can run on a curl worker before the submitting call has
// returned.
void           wxg_req_track(wxg_request_t *r);

// Lift the caller's half off `r` and unlink it, both under the registry
// lock. Deliver through the returned copy, never through `r->cb` — see
// the registry commentary in weathergov.c.
void           wxg_req_take_caller(wxg_request_t *r, wxg_caller_t *out);

void           wxg_ua(char *out, size_t sz);
bool           wxg_http_get(const char *url, const char *ua,
                   curl_done_cb_t cb, void *user);
bool           wxg_http_status_ok(const curl_response_t *resp, char *err,
                   size_t err_sz, bool *covered);

// Returns 0 on any malformed input — 0 means "absent", never the epoch.
time_t         wxg_parse_iso8601(const char *s, int32_t *tz_off_out);

// ----------------------------------------------------------------------
// Alerts — weathergov_alerts.c
// ----------------------------------------------------------------------

#ifdef WXG_ALERTS_TU

// One event+area fingerprint per distinct alert kept, for the weaker
// second dedup pass. A hash rather than the strings themselves: the
// comparison is exact-match only and 32 of these cost 256 bytes.
typedef struct
{
  uint64_t   label[WXG_ALERT_LABEL_MAX];
  uint8_t    count;
} wxg_alert_labels_t;

// The superseded-id set: every identifier named in any alert's
// references[] across the whole response.
typedef struct
{
  char       id[WXG_ALERT_REF_MAX][WEATHERGOV_ID_SZ];
  uint8_t    count;
} wxg_alert_refs_t;

static weathergov_severity_t   wxg_severity_of(const char *s);
static weathergov_urgency_t    wxg_urgency_of(const char *s);
static weathergov_certainty_t  wxg_certainty_of(const char *s);
static const char             *wxg_response_phrase(const char *response);

static const char *wxg_last_comma(const char *s, size_t len);
static time_t      wxg_alert_until(const weathergov_alert_t *a);
static bool        wxg_alert_precedes(const weathergov_alert_t *a,
                       const weathergov_alert_t *b);
static void        wxg_impact_append(char *out, size_t sz, const char *item);
static void        wxg_gust_phrase(const char *raw, char *out, size_t sz);
static void        wxg_hail_phrase(const char *raw, char *out, size_t sz);
static void        wxg_threat_phrase(const char *noun, const char *raw,
                       char *out, size_t sz);

static bool  wxg_param_first(struct json_object *params, const char *key,
                 char *out, size_t sz);
static void  wxg_area_summarize(const char *area_desc, char *state_out,
                 size_t state_sz, uint8_t *count_out);
static void  wxg_impact_build(struct json_object *params,
                 const char *response, char *out, size_t sz);
static void  wxg_alert_parse_one(struct json_object *props,
                 weathergov_alert_t *out);

static void  wxg_refs_collect(struct json_object *features,
                 wxg_alert_refs_t *refs);
static bool  wxg_refs_contains(const wxg_alert_refs_t *refs,
                 const char *id);
static bool  wxg_labels_add(wxg_alert_labels_t *seen,
                 const weathergov_alert_t *a);
static void  wxg_alert_set_insert(weathergov_alert_set_t *set,
                 const weathergov_alert_t *a);

static void  wxg_alerts_deliver(wxg_request_t *r);
static void  wxg_alerts_done(const curl_response_t *resp);

#endif // WXG_ALERTS_TU

// ----------------------------------------------------------------------
// Forecast — weathergov_forecast.c
// ----------------------------------------------------------------------

#ifdef WXG_FORECAST_TU

// One NWS icon token and the OpenWeather condition code it means. The
// vocabulary is closed at 34 tokens upstream (api.weather.gov/icons), so
// the table is exhaustive by construction and anything outside it is a
// protocol surprise rather than a gap.
typedef struct
{
  const char *token;
  int32_t     condition_id;
} wxg_icon_map_t;

static int32_t wxg_icon_condition_id(const char *icon_url);
static void    wxg_wind_tidy(const char *raw, char *out, size_t sz);
static void    wxg_period_parse_one(struct json_object *p,
                   weathergov_period_t *out);

static void    wxg_forecast_deliver(wxg_request_t *r);
static void    wxg_forecast_done(const curl_response_t *resp);

#endif // WXG_FORECAST_TU

// ----------------------------------------------------------------------
// Core translation unit — weathergov.c. Lifecycle, module state, the
// point lookup and its cache. Gated so a sibling TU does not inherit a
// wall of static declarations it never defines.
// ----------------------------------------------------------------------

#ifdef WXG_CORE_TU

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

static void               wxg_req_untrack_locked(wxg_request_t *r);
static void               wxg_unmap_cb(uintptr_t lo, uintptr_t hi,
                              void *data);

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

#endif // WXG_CORE_TU

#endif // WEATHERGOV_INTERNAL

#endif // BM_WEATHERGOV_H
