#ifndef BM_OPENWEATHER_H
#define BM_OPENWEATHER_H

// No public API — this plugin is loaded via dlopen and interacts
// with the core exclusively through cmd_register / cmd_reply /
// curl_get.  All declarations are internal.

#ifdef OW_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "curl.h"
#include "kv.h"
#include "mem.h"
#include "method.h"
#include "plugin.h"

#include <json-c/json.h>

// -----------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------

#define OW_CTX          "openweather"

// API base URLs.
#define OW_GEO_URL      "https://api.openweathermap.org/geo/1.0/zip"
#define OW_ONECALL_URL  "https://api.openweathermap.org/data/3.0/onecall"

// Size limits.
#define OW_ZIPCODE_SZ   16
#define OW_APIKEY_SZ    64
#define OW_UNITS_SZ     16
#define OW_URL_SZ       512
#define OW_REPLY_SZ     640
#define OW_NAME_SZ      64

// -----------------------------------------------------------------------
// Request types and structures
// -----------------------------------------------------------------------

// Command request types.
typedef enum
{
  OW_REQ_WEATHER,
  OW_REQ_FORECAST,
  OW_REQ_FORECAST_HOURLY
} ow_req_type_t;

// Request context: carries cmd_ctx_t data and parameters through
// the async geocode -> onecall chain.
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

  // Saved command context for reply.
  cmd_ctx_t           ctx;
  method_msg_t        msg;        // backing storage for ctx.msg

  // Freelist linkage.
  struct ow_request  *next;
} ow_request_t;

// -----------------------------------------------------------------------
// Geocode cache
// -----------------------------------------------------------------------

// Cached geocode result: maps zipcode -> lat/lon/name.
typedef struct ow_geocache
{
  char                zipcode[OW_ZIPCODE_SZ];
  double              lat;
  double              lon;
  char                name[OW_NAME_SZ];
  time_t              cached_at;
  struct ow_geocache *next;       // hash chain
} ow_geocache_t;

#define OW_GEO_CACHE_BUCKETS  64

// -----------------------------------------------------------------------
// Module state
// -----------------------------------------------------------------------

// Request freelist.
static ow_request_t    *ow_free     = NULL;
static pthread_mutex_t  ow_free_mu;

// Geocode cache.
static ow_geocache_t   *ow_geo_cache[OW_GEO_CACHE_BUCKETS];
static pthread_mutex_t  ow_geo_cache_mu;

// -----------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------

static bool             ow_validate_zipcode(const char *s);

// Argument descriptors for weather commands.
static const cmd_arg_desc_t ow_ad_weather[] = {
  { "zipcode", CMD_ARG_CUSTOM, CMD_ARG_REQUIRED, 0, ow_validate_zipcode },
};

// KV schema for the openweather plugin.
static const plugin_kv_entry_t ow_kv_schema[] = {
  { "plugin.openweather.apikey",         KV_STR,    ""         },
  { "plugin.openweather.units",          KV_STR,    "imperial" },
  { "plugin.openweather.geo_cache_ttl",  KV_UINT32, "86400"    },
};

static ow_geocache_t   *ow_geo_lookup(const char *zipcode);
static void             ow_geo_insert(const char *zipcode, double lat,
                            double lon, const char *name);
static ow_request_t    *ow_req_alloc(void);
static void             ow_req_release(ow_request_t *r);
static void             ow_reply(ow_request_t *r, const char *text);
static const char      *ow_temp_unit(const char *units);
static const char      *ow_speed_unit(const char *units);
static const char      *ow_wind_dir(double deg);
static void             ow_format_time(time_t ts, int tz_offset,
                            char *buf, size_t sz);
static void             ow_format_time_ampm(time_t ts, int tz_offset,
                            char *buf, size_t sz);
static double           ow_to_fahrenheit(double temp, const char *units);
static const char      *ow_temp_color(double temp_f);
static int              ow_fmt_temp(char *buf, size_t sz,
                            double temp, const char *units);
static const char      *ow_condition_icon(int id);
static const char      *ow_condition_color(int id);
static int              ow_get_condition_id(struct json_object *jw);
static int              ow_get_tz_offset(struct json_object *root);
static void             ow_reply_header(ow_request_t *r,
                            const char *subtitle);
static void             ow_fmt_precip(char *buf, size_t sz, int pop);
static void             ow_fmt_desc_pad(char *buf, size_t sz,
                            const char *desc, int width);
static bool             ow_get_hilo(struct json_object *jtemp_obj,
                            double *hi, double *lo);
static void             ow_reply_alerts(ow_request_t *r,
                            struct json_object *root);
static void             ow_handle_weather(ow_request_t *r,
                            struct json_object *root);
static void             ow_handle_forecast(ow_request_t *r,
                            struct json_object *root);
static void             ow_handle_forecast_hourly(ow_request_t *r,
                            struct json_object *root);
static void             ow_submit_onecall(ow_request_t *r);
static void             ow_onecall_done(const curl_response_t *resp);
static void             ow_geocode_done(const curl_response_t *resp);
static void             ow_cmd_common(const cmd_ctx_t *ctx,
                            ow_req_type_t type);
static void             ow_cmd_weather(const cmd_ctx_t *ctx);
static void             ow_cmd_forecast(const cmd_ctx_t *ctx);
static bool             ow_init(void);
static void             ow_deinit(void);

#endif // OW_INTERNAL

#endif // BM_OPENWEATHER_H
