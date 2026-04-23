#ifndef BM_OPENWEATHER_H
#define BM_OPENWEATHER_H

// Public mechanism API lives in openweather_api.h — consumers include
// that header. This header is service-plugin-internal and is only
// visible inside openweather.c (which defines OW_INTERNAL).

#ifdef OW_INTERNAL

#include "clam.h"
#include "common.h"
#include "curl.h"
#include "kv.h"
#include "alloc.h"
#include "json.h"
#include "plugin.h"

#include "openweather_api.h"

// Constants

#define OW_CTX          "openweather"

// API base URLs.
#define OW_GEO_URL          "https://api.openweathermap.org/geo/1.0/zip"
#define OW_GEO_DIRECT_URL   "https://api.openweathermap.org/geo/1.0/direct"
#define OW_GEO_REVERSE_URL  "https://api.openweathermap.org/geo/1.0/reverse"
#define OW_ONECALL_URL      "https://api.openweathermap.org/data/3.0/onecall"

// Size limits.
#define OW_ZIPCODE_SZ   OPENWEATHER_ZIPCODE_SZ
#define OW_APIKEY_SZ    64
#define OW_UNITS_SZ     OPENWEATHER_UNITS_SZ
#define OW_URL_SZ       512
#define OW_NAME_SZ      OPENWEATHER_NAME_SZ
#define OW_CITY_SZ      96

// Timeout budget for the sync city→zip geocode path (seconds). The
// /weather command body runs on a task-worker thread; bounding the
// wait keeps a flaky upstream from pinning the worker.
#define OW_CITY_TIMEOUT_SECS 5

// Request types and structures

typedef enum
{
  OW_REQ_WEATHER,
  OW_REQ_FORECAST_DAILY,
  OW_REQ_FORECAST_HOURLY
} ow_req_type_t;

// Request context: carries fetch parameters and the caller-supplied
// completion callback through the async geocode → onecall chain. No
// command-surface state (no cmd_ctx_t, no method_msg_t) — consumers
// in plugins/cmd/weather/ own their own per-request structs.
typedef struct ow_request
{
  ow_req_type_t       type;
  char                zipcode[OW_ZIPCODE_SZ];
  char                apikey[OW_APIKEY_SZ];
  char                units[OW_UNITS_SZ];

  // Geocode results.
  double              lat;
  double              lon;
  char                location_name[OW_NAME_SZ];

  // Caller callback (union on done-callback shape).
  union
  {
    openweather_done_current_cb_t   current;
    openweather_done_forecast_cb_t  forecast;
  }                   cb;
  void               *user;

  // Freelist linkage.
  struct ow_request  *next;
} ow_request_t;

// Geocode cache

typedef struct ow_geocache
{
  char                zipcode[OW_ZIPCODE_SZ];
  double              lat;
  double              lon;
  char                name[OW_NAME_SZ];
  time_t              cached_at;
  struct ow_geocache *next;
} ow_geocache_t;

#define OW_GEO_CACHE_BUCKETS  64

typedef struct ow_citycache
{
  char                 city[OW_CITY_SZ];
  char                 zipcode[OW_ZIPCODE_SZ];
  time_t               cached_at;
  struct ow_citycache *next;
} ow_citycache_t;

#define OW_CITY_CACHE_BUCKETS 32

// Module state

static ow_request_t    *ow_free     = NULL;
static pthread_mutex_t  ow_free_mu;

static ow_geocache_t   *ow_geo_cache[OW_GEO_CACHE_BUCKETS];
static pthread_mutex_t  ow_geo_cache_mu;

static ow_citycache_t  *ow_city_cache[OW_CITY_CACHE_BUCKETS];

// Forward declarations

static bool             ow_validate_zipcode(const char *s);

static const plugin_kv_entry_t ow_kv_schema[] = {
  { "plugin.openweather.apikey",         KV_STR,    "",
    "OpenWeatherMap API key" },
  { "plugin.openweather.units",          KV_STR,    "imperial",
    "Temperature units: imperial, metric, or standard" },
  { "plugin.openweather.geo_cache_ttl",  KV_UINT32, "86400",
    "Geocoding cache time-to-live in seconds" },
};

static ow_geocache_t   *ow_geo_lookup(const char *zipcode);
static void             ow_geo_insert(const char *zipcode, double lat,
                            double lon, const char *name);
static ow_request_t    *ow_req_alloc(void);
static void             ow_req_release(ow_request_t *r);
static void             ow_submit_onecall(ow_request_t *r);
static void             ow_onecall_done(const curl_response_t *resp);
static void             ow_geocode_done(const curl_response_t *resp);
static void             ow_deliver_current_err(ow_request_t *r,
                            const char *msg);
static void             ow_deliver_forecast_err(ow_request_t *r,
                            const char *msg);

static void             ow_parse_alerts(struct json_object *root,
                            openweather_alert_set_t *out);
static void             ow_parse_current(ow_request_t *r,
                            struct json_object *root,
                            openweather_current_result_t *out);
static void             ow_parse_forecast_daily(ow_request_t *r,
                            struct json_object *root,
                            openweather_forecast_result_t *out);
static void             ow_parse_forecast_hourly(ow_request_t *r,
                            struct json_object *root,
                            openweather_forecast_result_t *out);

static void             ow_canon_query(const char *in, char *out,
                            size_t out_sz);

static bool             ow_init(void);
static void             ow_deinit(void);

#endif // OW_INTERNAL

#endif // BM_OPENWEATHER_H
