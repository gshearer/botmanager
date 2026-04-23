#ifndef BM_OPENWEATHER_API_H
#define BM_OPENWEATHER_API_H

// Public mechanism API for the openweather service plugin. Consumers
// include this header and resolve the symbols at runtime via
// plugin_dlsym("openweather", …) — the plugin is loaded RTLD_LOCAL.
//
// Shim shape mirrors plugins/inference/inference.h: an atomic-guarded
// static cache per symbol, union to launder void*↔function-pointer
// conversion, FATAL + abort on lookup miss (which implies a broken
// plugin-dependency graph).
//
// Inside the openweather plugin itself the static-inline shims below
// would collide with the real definitions, so openweather.c defines
// OW_INTERNAL before including this header to skip them.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "common.h"  // SUCCESS/FAIL

// Size limits used by the result structs. Fixed sizes let callers
// stack-allocate a result and avoid any lifetime ambiguity about the
// strings inside.

#define OPENWEATHER_NAME_SZ       64
#define OPENWEATHER_DESC_SZ       96
#define OPENWEATHER_ALERT_SZ      256
#define OPENWEATHER_UNITS_SZ      16
#define OPENWEATHER_ZIPCODE_SZ    16
#define OPENWEATHER_FCAST_DAYS    8    // OneCall returns up to 8 days
#define OPENWEATHER_FCAST_HOURS   48
#define OPENWEATHER_ALERT_MAX     8

// Current conditions. Strings are fully-contained buffers; callers may
// copy them out or consume them in place.
typedef struct
{
  double  temp;           // in configured units
  double  feels_like;
  double  wind_speed;
  double  wind_deg;
  int32_t humidity;       // %
  int32_t condition_id;   // OpenWeather condition code
  time_t  sunrise;
  time_t  sunset;
  int32_t tz_offset;      // seconds east of UTC
  bool    have_hilo;
  double  temp_hi;
  double  temp_lo;
  char    condition_desc[OPENWEATHER_DESC_SZ];
  char    place_name[OPENWEATHER_NAME_SZ];
  char    zipcode[OPENWEATHER_ZIPCODE_SZ];
  char    units[OPENWEATHER_UNITS_SZ];
} openweather_current_t;

typedef struct
{
  time_t   dt;
  double   temp_hi;
  double   temp_lo;
  double   wind_speed;
  double   wind_deg;
  double   pop;            // 0..1
  int32_t  humidity;
  int32_t  condition_id;
  char     condition_desc[OPENWEATHER_DESC_SZ];
} openweather_forecast_day_t;

typedef struct
{
  time_t   dt;
  double   temp;
  double   wind_speed;
  double   wind_deg;
  double   pop;
  int32_t  humidity;
  int32_t  condition_id;
  char     condition_desc[OPENWEATHER_DESC_SZ];
} openweather_forecast_hour_t;

typedef struct
{
  int32_t                      tz_offset;
  uint8_t                      day_count;       // 0 if hourly
  uint8_t                      hour_count;      // 0 if daily
  char                         place_name[OPENWEATHER_NAME_SZ];
  char                         zipcode[OPENWEATHER_ZIPCODE_SZ];
  char                         units[OPENWEATHER_UNITS_SZ];
  openweather_forecast_day_t   days[OPENWEATHER_FCAST_DAYS];
  openweather_forecast_hour_t  hours[OPENWEATHER_FCAST_HOURS];
} openweather_forecast_t;

typedef struct
{
  char  event[OPENWEATHER_NAME_SZ];
} openweather_alert_t;

typedef struct
{
  uint8_t              count;
  openweather_alert_t  alerts[OPENWEATHER_ALERT_MAX];
} openweather_alert_set_t;

// Async fetch result. On success, err[0] == '\0' and the payload is
// populated. On failure, err carries a human-readable reason ready to
// forward to the user.
typedef struct
{
  char                     err[128];
  openweather_current_t    current;
  openweather_alert_set_t  alerts;
} openweather_current_result_t;

typedef struct
{
  char                     err[128];
  openweather_forecast_t   forecast;
  openweather_alert_set_t  alerts;
} openweather_forecast_result_t;

// Callback signatures. Callbacks run on the curl-multi worker thread
// owned by the openweather plugin — do not block; if substantial work
// is needed, enqueue a task. cmd_reply is thread-safe.
typedef void (*openweather_done_current_cb_t)(
    const openweather_current_result_t *res, void *user);

typedef void (*openweather_done_forecast_cb_t)(
    const openweather_forecast_result_t *res, void *user);

// Real function declarations — visible only inside the openweather
// plugin (where OW_INTERNAL is defined). External consumers go through
// the static-inline dlsym shims defined further down.
#ifdef OW_INTERNAL

// Sync geocode: ZIP → (lat, lon, place_name). Returns SUCCESS/FAIL.
// name_out may be NULL to skip the place-name.
bool openweather_geocode_zip_sync(const char *zipcode,
    double *lat_out, double *lon_out,
    char *name_out, size_t name_sz);

// Sync geocode: CITY → ZIP string. Returns SUCCESS/FAIL.
bool openweather_geocode_city_sync(const char *city,
    char *zip_out, size_t zip_sz);

// Async fetches. The service plugin accepts whatever zipcode shape its
// validator admits (US zip or the extended international pattern plus
// synthetic G-prefix keys produced by the city geocoder). Callbacks
// fire on the curl-multi worker thread; callbacks MUST return quickly.
//
// Returns SUCCESS if the request was queued; FAIL if it could not be
// queued (e.g. API key unconfigured). On FAIL, done_cb is NOT invoked.
bool openweather_fetch_current(const char *zipcode,
    openweather_done_current_cb_t done_cb, void *user);

bool openweather_fetch_forecast_daily(const char *zipcode,
    openweather_done_forecast_cb_t done_cb, void *user);

bool openweather_fetch_forecast_hourly(const char *zipcode,
    openweather_done_forecast_cb_t done_cb, void *user);

// Returns the current value of plugin.openweather.units. Pointer is
// to a static buffer in the service plugin; copy if retention across
// a thread boundary is needed.
const char *openweather_units_kv_value(void);

#endif // OW_INTERNAL

// ----------------------------------------------------------------------
// dlsym shim helpers
// ----------------------------------------------------------------------
//
// Reference: plugins/inference/inference.h. Each shim caches the
// resolved function pointer in a static atomic-guarded slot so
// subsequent calls take one relaxed-acquire load. On a cold cache the
// loader calls plugin_dlsym; a NULL return means the openweather
// plugin was not loaded — a programming error, fatal.
//
// Inside the openweather plugin itself the shims would collide with
// the real definitions, so openweather.c defines OW_INTERNAL before
// including this header to skip the inline block below.

#ifndef OW_INTERNAL

#include "clam.h"
#include "plugin.h"

#include <stdlib.h>  // abort

static inline bool
openweather_geocode_zip_sync(const char *zipcode,
    double *lat_out, double *lon_out,
    char *name_out, size_t name_sz)
{
  typedef bool (*fn_t)(const char *, double *, double *, char *, size_t);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("openweather", "openweather_geocode_zip_sync");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "openweather",
          "dlsym failed: openweather_geocode_zip_sync");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(zipcode, lat_out, lon_out, name_out, name_sz));
}

static inline bool
openweather_geocode_city_sync(const char *city, char *zip_out, size_t zip_sz)
{
  typedef bool (*fn_t)(const char *, char *, size_t);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("openweather", "openweather_geocode_city_sync");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "openweather",
          "dlsym failed: openweather_geocode_city_sync");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(city, zip_out, zip_sz));
}

static inline bool
openweather_fetch_current(const char *zipcode,
    openweather_done_current_cb_t done_cb, void *user)
{
  typedef bool (*fn_t)(const char *,
      openweather_done_current_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("openweather", "openweather_fetch_current");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "openweather",
          "dlsym failed: openweather_fetch_current");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(zipcode, done_cb, user));
}

static inline bool
openweather_fetch_forecast_daily(const char *zipcode,
    openweather_done_forecast_cb_t done_cb, void *user)
{
  typedef bool (*fn_t)(const char *,
      openweather_done_forecast_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("openweather", "openweather_fetch_forecast_daily");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "openweather",
          "dlsym failed: openweather_fetch_forecast_daily");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(zipcode, done_cb, user));
}

static inline bool
openweather_fetch_forecast_hourly(const char *zipcode,
    openweather_done_forecast_cb_t done_cb, void *user)
{
  typedef bool (*fn_t)(const char *,
      openweather_done_forecast_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("openweather",
        "openweather_fetch_forecast_hourly");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "openweather",
          "dlsym failed: openweather_fetch_forecast_hourly");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(zipcode, done_cb, user));
}

static inline const char *
openweather_units_kv_value(void)
{
  typedef const char *(*fn_t)(void);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("openweather", "openweather_units_kv_value");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "openweather",
          "dlsym failed: openweather_units_kv_value");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn());
}

#endif // !OW_INTERNAL

#endif // BM_OPENWEATHER_API_H
