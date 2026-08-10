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
} weather_req_t;

// Presentation — weather_render.c. Everything below this line formats;
// nothing below it fetches, routes or decides.
const char *weather_temp_unit(const char *units);
const char *weather_speed_unit(const char *units);
const char *weather_wind_dir(double deg);
double      weather_to_fahrenheit(double temp, const char *units);
const char *weather_temp_color(double temp_f);
int         weather_fmt_temp(char *buf, size_t sz, double temp,
                const char *units);
int         weather_fmt_temp_w(char *buf, size_t sz, double temp,
                const char *units, int width);
const char *weather_condition_icon(int id);
const char *weather_condition_color(int id);
void        weather_format_time_ampm(time_t ts, int tz_offset, char *buf,
                size_t sz);
bool        weather_zip_is_synth(const char *zip);
void        weather_fmt_precip(char *buf, size_t sz, int pop);
void        weather_fmt_desc_pad(char *buf, size_t sz, const char *desc,
                int width);
void        weather_reply_header(const cmd_ctx_t *ctx, const char *place,
                const char *zip, const char *subtitle);
void        weather_reply_alerts(const cmd_ctx_t *ctx,
                const openweather_alert_set_t *a);
void        weather_reply_current(const cmd_ctx_t *ctx,
                const openweather_current_t *cur,
                const openweather_alert_set_t *alerts);
void        weather_reply_forecast_daily(const cmd_ctx_t *ctx,
                const openweather_forecast_t *f,
                const openweather_alert_set_t *alerts);
void        weather_hour_cell(char *buf, size_t sz,
                const openweather_forecast_hour_t *h, const char *units,
                const char *tu, int tz_offset);
void        weather_reply_forecast_hourly(const cmd_ctx_t *ctx,
                const openweather_forecast_t *f,
                const openweather_alert_set_t *alerts);

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
static void             weather_dispatch_openweather(weather_req_t *r);
static void             weather_point_probe_done(
                            const weathergov_point_result_t *res,
                            void *user);
static void             weather_cmd_weather(const cmd_ctx_t *ctx);

static bool             weather_init(void);
static void             weather_deinit(void);

#endif // WEATHER_CMD_TU

#endif // WEATHER_INTERNAL

#endif // BM_WEATHER_H
