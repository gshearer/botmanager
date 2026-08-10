#ifndef BM_WEATHER_H
#define BM_WEATHER_H

// No public API — this plugin is loaded via dlopen and interacts with
// the core exclusively through cmd_register / cmd_reply, consuming the
// openweather and weathergov mechanism APIs via plugin_dlsym.

#ifdef WEATHER_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "plugin.h"
#include "alloc.h"
#include "method.h"

#include "openweather_api.h"
#include "weathergov_api.h"
#include "weather_view.h"

#define WEATHER_CTX       "weather"
#define WEATHER_REPLY_SZ  640

// One rendered hourly cell (colour codes + one emoji + fixed-width raw
// fields). Two of these are joined into a single ≤100-column reply line
// by the double-column hourly formatter.
#define WEATHER_CELL_SZ   256

typedef enum
{
  WEATHER_REQ_CURRENT,
  WEATHER_REQ_FORECAST_DAILY,
  WEATHER_REQ_FORECAST_HOURLY
} weather_req_kind_t;

// A resolved location: whatever the user typed, reduced to the one shape
// both providers can be asked in. openweather is keyed on the zipcode;
// weather.gov takes nothing but the coordinate.
typedef struct
{
  char   zip[OPENWEATHER_ZIPCODE_SZ];
  char   place[OPENWEATHER_NAME_SZ];
  double lat;
  double lon;
} weather_loc_t;

// Per-request async closure. The command callback runs on a task
// worker thread, submits an async fetch into the openweather plugin,
// then returns. The request lives on the heap until the completion
// callback fires and frees it.
typedef struct
{
  cmd_ctx_t           ctx;    // saved context (msg pointer repaired)
  method_msg_t        msg;    // backing storage for ctx.msg
  weather_req_kind_t  kind;
  weather_loc_t       loc;    // resolved once, on the task worker

  // The alert answer, held from the leg that produced it until the
  // renderer runs. `have_alerts` true means weather.gov spoke for this
  // coordinate and its word is final — openweather is then asked to
  // skip alert enrichment entirely, which is where the round trips go.
  weather_view_alert_set_t  alerts;
  bool                      have_alerts;
} weather_req_t;

// The command surface — weather.c only. Its argument descriptor is a
// definition, not a declaration, so a second translation unit including
// this header would get an unused copy of it and a static prototype it
// never defines. WEATHER_CMD_TU is what keeps that to the one TU that
// owns the command.
#ifdef WEATHER_CMD_TU

static bool             weather_valid_location(const char *s);

// Accepts either a zipcode or a city name; the command body decides
// which path to take at runtime.
static const cmd_arg_desc_t weather_ad_weather[] = {
  { "location", CMD_ARG_CUSTOM, CMD_ARG_REQUIRED | CMD_ARG_REST, 0,
      weather_valid_location },
};

static bool             weather_resolve_location(const char *input,
                            weather_loc_t *out);
static void             weather_alerts_adopt(weather_req_t *r,
                            const openweather_alert_set_t *ow);
static void             weather_dispatch_openweather(weather_req_t *r);
static void             weather_alerts_done(
                            const weathergov_alert_result_t *res,
                            void *user);
static void             weather_cmd_weather(const cmd_ctx_t *ctx);

static bool             weather_init(void);
static void             weather_deinit(void);

#endif // WEATHER_CMD_TU

#endif // WEATHER_INTERNAL

#endif // BM_WEATHER_H
