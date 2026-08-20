#ifndef BM_WEATHERGOV_API_H
#define BM_WEATHERGOV_API_H

// Public mechanism API for the weathergov service plugin — a client for
// the US National Weather Service web API (api.weather.gov). Consumers
// include this header and resolve the symbols at runtime via
// plugin_dlsym_cached("weathergov", …, (void **)&cached) — the plugin is
// loaded RTLD_LOCAL.
//
// Shim shape mirrors plugins/service/openweather/openweather_api.h: an
// atomic-guarded static cache per symbol, union to launder
// void*↔function-pointer conversion, FATAL + abort on lookup miss
// (which implies a broken plugin-dependency graph).
//
// Inside the weathergov plugin itself the static-inline shims below
// would collide with the real definitions, so weathergov.c defines
// WEATHERGOV_INTERNAL before including this header to skip them.
//
// This service holds no credential and never will: weather.gov
// authenticates on the User-Agent header alone. It also has no
// geocoder — coordinates come from openweather, which stays the only
// place-name oracle in the tree.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "async.h"
#include "common.h"  // SUCCESS/FAIL

// Size limits used by the result structs. Fixed sizes let callers
// stack-allocate a result and avoid any lifetime ambiguity about the
// strings inside.

#define WEATHERGOV_GRID_SZ      8    // "ILN"
#define WEATHERGOV_PLACE_SZ     64   // "Olde West Chester, OH"
#define WEATHERGOV_TZ_SZ        48   // "America/New_York"

#define WEATHERGOV_ID_SZ        200  // urn:oid:… measured 96
#define WEATHERGOV_EVENT_SZ     64   // "Severe Thunderstorm Warning" = 27
#define WEATHERGOV_AREA_SZ      256
#define WEATHERGOV_HEADLINE_SZ  256
#define WEATHERGOV_DESC_SZ      512  // excerpt; full bodies run ~900 B
#define WEATHERGOV_INSTR_SZ     256
#define WEATHERGOV_IMPACT_SZ    128
#define WEATHERGOV_SENDER_SZ    64   // "NWS Wilmington OH"
#define WEATHERGOV_RESPONSE_SZ  16   // "Shelter"
#define WEATHERGOV_STATE_SZ     4
#define WEATHERGOV_ALERT_MAX    8    // matches OPENWEATHER_ALERT_MAX

#define WEATHERGOV_PERIOD_NAME_SZ 24   // "Wednesday Night" = 15
#define WEATHERGOV_COND_SZ      96   // shortForecast, verbatim; measured 77
#define WEATHERGOV_WIND_SZ      24   // "7-12mph"
#define WEATHERGOV_DETAIL_SZ    512  // detailedForecast
#define WEATHERGOV_PERIOD_MAX   14   // measured exactly 14, day/night alternating

#define WEATHERGOV_STATION_SZ   12   // "KHAO"; mesonet identifiers run longer

// A point resolved to its NWS forecast grid. The grid triple is what
// every forecast and gridpoint URL is built from; the rest is what the
// same response hands over for free.
typedef struct
{
  char    grid_id[WEATHERGOV_GRID_SZ];   // "ILN"
  int32_t grid_x;
  int32_t grid_y;
  char    place[WEATHERGOV_PLACE_SZ];    // "Olde West Chester, OH"
  char    tz[WEATHERGOV_TZ_SZ];          // IANA name, informational only
  int32_t tz_offset;                     // seconds east of UTC
  time_t  sunrise;                       // 0 when astronomicalData absent
  time_t  sunset;                        // 0 when astronomicalData absent
} weathergov_point_t;

// `covered == false` with an empty `err` is the NORMAL answer for any
// coordinate outside NWS coverage (London, Reykjavík, mid-ocean). It is
// not an error and must never reach a user — it is the caller's cue to
// use its other provider.
//
// `tz_offset` is read out of the astronomicalData timestamps, so it is 0
// when that object is missing. 0 is also a legal offset (UTC), so it
// cannot serve as a presence flag: test `sunrise != 0` instead.
typedef struct
{
  char                err[128];
  bool                covered;
  weathergov_point_t  point;
} weathergov_point_result_t;

// Callbacks run on the curl-multi worker thread owned by core — do not
// block; if substantial work is needed, enqueue a task. cmd_reply is
// thread-safe.
//
// The plugin files every live request on an in-flight list and listens
// for mapping unloads (plugin_unmap_notify_register). If your plugin is
// unloaded with a request airborne, that request still runs to the end
// but this callback is NOT invoked — and `user` is dropped, so whatever
// it points at LEAKS. That is the accepted outcome: only you could free
// your own context and you are exactly what is no longer there. See
// PLUGIN.md §Lifecycle Contract.
typedef void (*weathergov_point_cb_t)(
    const weathergov_point_result_t *res, void *user);

// ----------------------------------------------------------------------
// Active alerts (Common Alerting Protocol)
// ----------------------------------------------------------------------
//
// One GET answers every active alert for a coordinate, and every CAP
// field a human cares about arrives with it — the product name is
// stated rather than mined out of prose, and an update names the
// bulletin it supersedes.
//
// The three graded enums are ordered ASCENDING so a comparison means
// what it reads as: WEATHERGOV_SEV_SEVERE > WEATHERGOV_SEV_MINOR.

typedef enum
{
  WEATHERGOV_SEV_UNKNOWN = 0, WEATHERGOV_SEV_MINOR,
  WEATHERGOV_SEV_MODERATE, WEATHERGOV_SEV_SEVERE, WEATHERGOV_SEV_EXTREME
} weathergov_severity_t;

typedef enum
{
  WEATHERGOV_URG_UNKNOWN = 0, WEATHERGOV_URG_PAST,
  WEATHERGOV_URG_FUTURE, WEATHERGOV_URG_EXPECTED, WEATHERGOV_URG_IMMEDIATE
} weathergov_urgency_t;

typedef enum
{
  WEATHERGOV_CRT_UNKNOWN = 0, WEATHERGOV_CRT_UNLIKELY,
  WEATHERGOV_CRT_POSSIBLE, WEATHERGOV_CRT_LIKELY, WEATHERGOV_CRT_OBSERVED
} weathergov_certainty_t;

// One active alert.
//
// `impact` is assembled here rather than by the caller because it is a
// distillation of the CAP `parameters` map — wind gust, hail size,
// damage threat, else the protective-action phrase — and no renderer
// should have to know that map exists.
//
// ⚠ `desc` and `instruction` are parsed and carried but rendered
// NOWHERE. They run 250–900 bytes of prose and would flood a channel.
// They exist for the proactive alerter (the soul's weather chore, SOUL-3); do
// not delete them as dead weight.
typedef struct
{
  char                    id      [WEATHERGOV_ID_SZ];
  char                    event   [WEATHERGOV_EVENT_SZ];
  char                    area    [WEATHERGOV_AREA_SZ];      // areaDesc verbatim
  char                    state   [WEATHERGOV_STATE_SZ];     // "" unless all areas agree
  char                    headline[WEATHERGOV_HEADLINE_SZ];
  char                    desc    [WEATHERGOV_DESC_SZ];      // NOT rendered
  char                    instruction[WEATHERGOV_INSTR_SZ];  // NOT rendered
  char                    impact  [WEATHERGOV_IMPACT_SZ];    // pre-assembled
  char                    sender  [WEATHERGOV_SENDER_SZ];
  char                    response[WEATHERGOV_RESPONSE_SZ];
  weathergov_severity_t   severity;
  weathergov_urgency_t    urgency;
  weathergov_certainty_t  certainty;
  time_t                  onset;
  time_t                  ends;      // 0 when null upstream — fall back to expires
  time_t                  expires;
  time_t                  sent;
  int32_t                 tz_offset; // seconds east of UTC, off the ISO strings
  uint8_t                 area_count;
} weathergov_alert_t;

// Sorted severity-descending, then soonest-expiring first, so the ones
// that fit a reply are the ones that matter.
//
// `total` counts every distinct active alert after supersession and
// label dedup; `count` is how many of them fit below. total - count is
// exactly what an "…and N more" line reports.
typedef struct
{
  uint8_t             count;
  uint8_t             total;
  weathergov_alert_t  alerts[WEATHERGOV_ALERT_MAX];
} weathergov_alert_set_t;

// `covered == false` with an empty `err` carries the same meaning as it
// does for a point lookup: the coordinate is outside NWS coverage. The
// alerts endpoint reports that as HTTP 400 where /points reports 404,
// which is why neither is switched on.
typedef struct
{
  char                    err[128];
  bool                    covered;
  weathergov_alert_set_t  alerts;
} weathergov_alert_result_t;

typedef void (*weathergov_alerts_cb_t)(
    const weathergov_alert_result_t *res, void *user);

// ----------------------------------------------------------------------
// The forecast
// ----------------------------------------------------------------------
//
// weather.gov forecasts in day/night PERIODS, not days: 14 of them,
// alternating, each written by the local Weather Forecast Office. The
// service hands them over exactly as issued — pairing a day with its
// night is a presentation decision and belongs to the consumer.
//
// Two fields are deliberately carried and rendered nowhere, on the same
// terms as an alert's `desc`: `start` is a period's identity, and
// `detail` is the meteorologist's own prose — the quality argument for
// this provider in the first place, and what the proactive alerter
// (the soul's weather chore, SOUL-3) will read. Neither is dead weight.
//
// There is no timezone here on purpose. Every period arrives already
// LABELLED by upstream ("Today", "Tonight", "Wednesday Night"), so no
// consumer has to turn an instant back into a weekday, and a second
// offset beside weathergov_point_t.tz_offset would only be a way for
// the two to disagree.
typedef struct
{
  char    name[WEATHERGOV_PERIOD_NAME_SZ];  // "Tonight", "Wednesday Night"
  time_t  start;                            // NOT rendered
  bool    is_daytime;                       // pairs a night onto its day
  double  temp;                             // high on a day, low on a night
  char    temp_unit[4];                     // "F" | "C", as requested
  int32_t pop;                              // %, -1 when null upstream
  char    wind [WEATHERGOV_WIND_SZ];        // tidied: "7-12mph", "19km/h"
  char    wind_dir[8];                      // "SSW" — already a compass point
  int32_t condition_id;                     // OpenWeather numbering; see below
  char    cond [WEATHERGOV_COND_SZ];        // shortForecast, verbatim
  char    detail[WEATHERGOV_DETAIL_SZ];     // NOT rendered
} weathergov_period_t;

// `condition_id` is OpenWeather's condition numbering, which the whole
// tree's icon and colour tables already key off. The five sky states
// NWS names and OpenWeather has no code for take a private range at
// 900+: 900 tornado, 901 hurricane, 902 blizzard, 903 tropical storm,
// 904 hot, 905 cold. 0 means the icon token was unrecognised.

typedef struct
{
  uint8_t             count;
  weathergov_period_t periods[WEATHERGOV_PERIOD_MAX];
} weathergov_forecast_t;

// `covered == false` with an empty `err` means the same thing it does
// everywhere else in this API: outside NWS coverage, ask the other
// provider. A grid that answers 200 with zero periods lands here as
// `count == 0`, which a consumer must treat the same way.
typedef struct
{
  char                   err[128];
  bool                   covered;
  weathergov_forecast_t  forecast;
} weathergov_forecast_result_t;

typedef void (*weathergov_forecast_cb_t)(
    const weathergov_forecast_result_t *res, void *user);

// ----------------------------------------------------------------------
// Current conditions
// ----------------------------------------------------------------------
//
// A forecast is a model's opinion of right now; an observation is what
// an instrument at a named airport actually measured, minutes ago. This
// is the one view where the two providers differ in kind rather than in
// quality.
//
// ⚠ EVERY numeric an observation carries is nullable, and a station
// that reports temperature may report nothing else — `windGust`,
// `windChill`, `seaLevelPressure` were all null in the 08-10 probe of
// KHAO. Each optional field therefore has its own presence flag and a
// zero must never be read as a measurement.
//
// `have_temp` is the validity flag for the whole observation: false
// means the nearest station is not reporting and the caller should use
// its other provider rather than print a half-empty line.
typedef struct
{
  char    station[WEATHERGOV_STATION_SZ];  // "KHAO"
  char    text   [WEATHERGOV_COND_SZ];     // textDescription: "Partly Cloudy"
  int32_t condition_id;                    // OpenWeather numbering; see above
  time_t  observed;
  bool    have_temp;
  double  temp;
  bool    have_feels;                      // heatIndex, else windChill
  double  feels_like;
  bool    have_humidity;
  int32_t humidity;                        // %
  bool    have_wind;
  double  wind_speed;
  double  wind_dir_deg;                    // degrees, as issued
  bool    have_gust;
  double  wind_gust;
} weathergov_obs_t;

// One caller needs both halves of "right now" — what was measured and
// what the office says about the rest of the day — so they arrive
// together rather than as two callbacks a consumer would have to join
// (root TODO §WX-DESIGN §6: chains, never joins).
//
// The forecast half is enrichment and degrades on its own: `count == 0`
// costs the caller its high/low and its prose line, nothing more.
typedef struct
{
  char                   err[128];
  bool                   covered;
  weathergov_obs_t       obs;
  weathergov_forecast_t  forecast;
} weathergov_current_result_t;

typedef void (*weathergov_current_cb_t)(
    const weathergov_current_result_t *res, void *user);

// Real function declarations — visible only inside the weathergov
// plugin (where WEATHERGOV_INTERNAL is defined). External consumers go
// through the static-inline dlsym shims defined further down.
#ifdef WEATHERGOV_INTERNAL

// All four *_async below return only ASYNC_AIRBORNE or
// ASYNC_FAILED_UNDELIVERED, never ASYNC_FAILED_DELIVERED. Undelivered
// means the request could not be queued at all — a NULL callback or
// point, the service switched off via plugin.weathergov.enabled, or
// curl refusing the submit — and `user` is still entirely the caller's
// to reply on and free. Each notes below only what is particular to it.

// Resolve a coordinate to its NWS forecast grid, from cache when warm.
//
// ⚠ On a warm cache the callback has already fired by the time this
// returns, so a caller holding a lock can re-enter itself.
async_rc_t weathergov_point_async(double lat, double lon,
    weathergov_point_cb_t cb, void *user);

// Every active alert for a coordinate, in one round trip. Needs no
// point lookup, no grid and no prior call — the endpoint takes raw
// coordinates and its own non-2xx is the coverage answer.
async_rc_t weathergov_alerts_async(double lat, double lon,
    weathergov_alerts_cb_t cb, void *user);

// The 14-period forecast for an already-resolved grid. `pt` is borrowed
// for the duration of the call only — the grid triple is copied into the
// URL before this returns. `units` is "us" (°F, mph) or "si" (°C, km/h);
// NULL means "us". There is no Kelvin upstream, so a caller wanting it
// asks for "si" and converts.
async_rc_t weathergov_forecast_async(const weathergov_point_t *pt,
    const char *units, weathergov_forecast_cb_t cb, void *user);

// Current conditions for an already-resolved grid: the latest
// observation from the station nearest it, together with the forecast
// periods that state today's high and low. `pt` is borrowed for the
// duration of the call only. `units` is "us" (°F, mph) or "si" (°C,
// m/s); NULL means "us".
//
// ⚠ The forecast half is enrichment: a result whose `obs.have_temp` is
// true but whose `forecast.count` is 0 is a good answer with two fields
// missing, not a failure.
async_rc_t weathergov_current_async(const weathergov_point_t *pt,
    const char *units, weathergov_current_cb_t cb, void *user);

// The plugin.weathergov.enabled master switch. A caller checks this to
// skip the weather.gov leg outright rather than paying for a refused
// submit; weathergov_point_async enforces it regardless.
bool weathergov_enabled(void);

#endif // WEATHERGOV_INTERNAL

// ----------------------------------------------------------------------
// dlsym shim helpers
// ----------------------------------------------------------------------
//
// Each shim caches the resolved function pointer in a static
// atomic-guarded slot so subsequent calls take one relaxed-acquire load.
// On a cold cache the loader calls plugin_dlsym; a NULL return means the
// weathergov plugin was not loaded — a programming error, fatal.

#ifndef WEATHERGOV_INTERNAL

#include "clam.h"
#include "plugin.h"

#include <stdlib.h>  // abort

static inline async_rc_t
weathergov_point_async(double lat, double lon,
    weathergov_point_cb_t cb, void *user)
{
  typedef async_rc_t (*fn_t)(double, double, weathergov_point_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached("weathergov", "weathergov_point_async", (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "weathergov",
          "dlsym failed: weathergov_point_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(lat, lon, cb, user));
}

static inline async_rc_t
weathergov_alerts_async(double lat, double lon,
    weathergov_alerts_cb_t cb, void *user)
{
  typedef async_rc_t (*fn_t)(double, double, weathergov_alerts_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached("weathergov", "weathergov_alerts_async", (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "weathergov",
          "dlsym failed: weathergov_alerts_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(lat, lon, cb, user));
}

static inline async_rc_t
weathergov_forecast_async(const weathergov_point_t *pt, const char *units,
    weathergov_forecast_cb_t cb, void *user)
{
  typedef async_rc_t (*fn_t)(const weathergov_point_t *, const char *,
      weathergov_forecast_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached("weathergov", "weathergov_forecast_async", (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "weathergov",
          "dlsym failed: weathergov_forecast_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(pt, units, cb, user));
}

static inline async_rc_t
weathergov_current_async(const weathergov_point_t *pt, const char *units,
    weathergov_current_cb_t cb, void *user)
{
  typedef async_rc_t (*fn_t)(const weathergov_point_t *, const char *,
      weathergov_current_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached("weathergov", "weathergov_current_async", (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "weathergov",
          "dlsym failed: weathergov_current_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(pt, units, cb, user));
}

static inline bool
weathergov_enabled(void)
{
  typedef bool (*fn_t)(void);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached("weathergov", "weathergov_enabled", (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "weathergov",
          "dlsym failed: weathergov_enabled");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn());
}

#endif // !WEATHERGOV_INTERNAL

#endif // BM_WEATHERGOV_API_H
