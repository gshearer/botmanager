#ifndef BM_WEATHER_H
#define BM_WEATHER_H

// No public API — this plugin is loaded via dlopen and interacts with
// the core exclusively through cmd_register / cmd_reply, consuming
// the openweather mechanism API via plugin_dlsym.

#ifdef WEATHER_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "plugin.h"
#include "alloc.h"
#include "method.h"

#include "openweather_api.h"

#define WEATHER_CTX       "weather"
#define WEATHER_REPLY_SZ  640

typedef enum
{
  WEATHER_REQ_CURRENT,
  WEATHER_REQ_FORECAST_DAILY,
  WEATHER_REQ_FORECAST_HOURLY
} weather_req_kind_t;

// Per-request async closure. The command callback runs on a task
// worker thread, submits an async fetch into the openweather plugin,
// then returns. The request lives on the heap until the completion
// callback fires and frees it.
typedef struct
{
  cmd_ctx_t           ctx;    // saved context (msg pointer repaired)
  method_msg_t        msg;    // backing storage for ctx.msg
  weather_req_kind_t  kind;
} weather_req_t;

static bool             weather_valid_location(const char *s);

// Accepts either a zipcode or a city name; the command body decides
// which path to take at runtime.
static const cmd_arg_desc_t weather_ad_weather[] = {
  { "location", CMD_ARG_CUSTOM, CMD_ARG_REQUIRED | CMD_ARG_REST, 0,
      weather_valid_location },
};

static void             weather_cmd_weather(const cmd_ctx_t *ctx);
static void             weather_cmd_forecast(const cmd_ctx_t *ctx);

static bool             weather_init(void);
static void             weather_deinit(void);

#endif // WEATHER_INTERNAL

#endif // BM_WEATHER_H
