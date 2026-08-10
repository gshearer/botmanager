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

#include "common.h"  // SUCCESS/FAIL

// Size limits used by the result structs. Fixed sizes let callers
// stack-allocate a result and avoid any lifetime ambiguity about the
// strings inside.

#define WEATHERGOV_GRID_SZ      8    // "ILN"
#define WEATHERGOV_PLACE_SZ     64   // "Olde West Chester, OH"
#define WEATHERGOV_TZ_SZ        48   // "America/New_York"

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

// Real function declarations — visible only inside the weathergov
// plugin (where WEATHERGOV_INTERNAL is defined). External consumers go
// through the static-inline dlsym shims defined further down.
#ifdef WEATHERGOV_INTERNAL

// Resolve a coordinate to its NWS forecast grid, from cache when warm.
//
// Returns SUCCESS if the callback will fire — including the warm-cache
// case, where it has already fired by the time this returns. Returns
// FAIL when the request could not be queued at all (callback NULL, the
// service switched off via plugin.weathergov.enabled, or curl refusing
// the submit); on FAIL the callback does NOT fire and `user` is still
// entirely the caller's to reply on and free.
bool weathergov_point_async(double lat, double lon,
    weathergov_point_cb_t cb, void *user);

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

static inline bool
weathergov_point_async(double lat, double lon,
    weathergov_point_cb_t cb, void *user)
{
  typedef bool (*fn_t)(double, double, weathergov_point_cb_t, void *);
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
