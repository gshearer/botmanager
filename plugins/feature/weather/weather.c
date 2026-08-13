// botmanager — MIT
// Weather command surface: resolve a location, route it, fetch, render.
#define WEATHER_INTERNAL
#define WEATHER_CMD_TU
#include "weather.h"

#include <stdio.h>
#include <string.h>

// Input validation — any non-empty, printable string. The command
// body picks zipcode vs city at dispatch time.
static bool
weather_valid_location(const char *s)
{
  size_t i;

  if(s == NULL || s[0] == '\0')
    return(false);

  for(i = 0; s[i] != '\0'; i++)
  {
    unsigned char c = (unsigned char)s[i];

    if(c < 0x20 || c == 0x7f)
      return(false);
  }

  return(true);
}

// Async completion callbacks. Fire on the openweather curl-multi worker
// thread; cmd_reply is thread-safe. Each frees the per-request closure.

// The alert set to render. weather.gov's answer wins outright when we
// have one — openweather was asked to skip enrichment in that case and
// its own set is empty by construction.
static void
weather_alerts_adopt(weather_req_t *r, const openweather_alert_set_t *ow)
{
  if(r->have_alerts)
    return;

  weather_view_from_ow_alerts(ow, &r->alerts);
}

static void
weather_done_current(const openweather_current_result_t *res, void *user)
{
  weather_req_t *r = (weather_req_t *)user;
  cmd_ctx_t ctx = r->ctx;

  ctx.msg = &r->msg;

  if(res->err[0] != '\0')
    cmd_reply(&ctx, res->err);

  else
  {
    weather_view_current_t view;

    weather_alerts_adopt(r, &res->alerts);
    weather_view_from_ow_current(&res->current, &view);
    weather_reply_current(&ctx, &view, &r->alerts);
  }

  mem_free(r);
}

static void
weather_done_forecast(const openweather_forecast_result_t *res, void *user)
{
  weather_req_t *r = (weather_req_t *)user;
  cmd_ctx_t ctx = r->ctx;

  ctx.msg = &r->msg;

  if(res->err[0] != '\0')
  {
    cmd_reply(&ctx, res->err);
    mem_free(r);
    return;
  }

  weather_alerts_adopt(r, &res->alerts);

  if(r->kind == WEATHER_REQ_FORECAST_HOURLY)
    weather_reply_forecast_hourly(&ctx, &res->forecast, &r->alerts);

  else
  {
    weather_view_forecast_t view;

    weather_view_from_ow_forecast(&res->forecast, &view);
    weather_reply_forecast_daily(&ctx, &view, &r->alerts);
  }

  mem_free(r);
}

// Request factory — deep-copies the command context so it survives
// beyond the command callback return.
static weather_req_t *
weather_req_new(const cmd_ctx_t *ctx, weather_req_kind_t kind)
{
  weather_req_t *r = mem_alloc(WEATHER_CTX, "req", sizeof(*r));

  memset(r, 0, sizeof(*r));
  r->ctx = *ctx;

  if(ctx->msg != NULL)
    r->msg = *ctx->msg;

  r->ctx.msg      = &r->msg;
  r->ctx.args     = NULL;
  r->ctx.username = NULL;
  r->ctx.parsed   = NULL;
  r->kind         = kind;

  return(r);
}

// Location resolution
//
// Whatever the user typed becomes {zip, place, lat, lon} exactly once,
// on the task-worker thread, before the first submit. Every later leg
// carries the coordinate on the request rather than resolving again —
// which it could not do anyway: openweather's geocoders block on a
// condvar, and a completion callback runs on the curl multi loop, where
// blocking on a transfer that loop must itself dispatch is a deadlock.
//
// TASK-WORKER THREAD ONLY.
static bool
weather_resolve_location(const char *input, weather_loc_t *out)
{
  // Any ASCII digit routes through the zipcode path; pure-alpha inputs
  // go through the city geocoder, which resolves to a zipcode (real or
  // synthetic) and caches the coordinate on the way.
  for(size_t i = 0; input[i] != '\0'; i++)
  {
    if(input[i] < '0' || input[i] > '9')
      continue;

    snprintf(out->zip, sizeof(out->zip), "%s", input);
    break;
  }

  if(out->zip[0] == '\0'
      && openweather_geocode_city_sync(input, out->zip,
          sizeof(out->zip)) != SUCCESS)
    return(FAIL);

  // A geo-cache hit in every case but a cold zipcode: the city path has
  // just inserted this very entry.
  if(openweather_geocode_zip_sync(out->zip, &out->lat, &out->lon,
      out->place, sizeof(out->place)) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

// Hands the request to openweather. Owns the failure path: on a refused
// submit the user is told and `r` is freed, so no caller may touch it
// afterwards either way. Safe on any thread — every fetch is async.
static void
weather_dispatch_openweather(weather_req_t *r)
{
  // Holding weather.gov's CAP set, we spend nothing on One Call's
  // alert ids — which cost one serial GET each, up to eight of them.
  openweather_alerts_t alerts = r->have_alerts ? OPENWEATHER_ALERTS_SKIP
                                               : OPENWEATHER_ALERTS_FETCH;
  bool submitted;

  switch(r->kind)
  {
    case WEATHER_REQ_FORECAST_HOURLY:
      submitted = (openweather_fetch_forecast_hourly(r->loc.zip, alerts,
          weather_done_forecast, r) == SUCCESS);
      break;

    case WEATHER_REQ_FORECAST_DAILY:
      submitted = (openweather_fetch_forecast_daily(r->loc.zip, alerts,
          weather_done_forecast, r) == SUCCESS);
      break;

    case WEATHER_REQ_CURRENT:
    default:
      submitted = (openweather_fetch_current(r->loc.zip, alerts,
          weather_done_current, r) == SUCCESS);
      break;
  }

  if(submitted)
    return;

  cmd_reply(&r->ctx,
      "Error: failed to submit weather request. "
      "Check plugin.openweather.creds.apikey.");
  mem_free(r);
}

// weather.gov negotiates units server-side, in two flavours where our
// own knob has three. There is no Kelvin upstream at all, so "standard"
// asks for Celsius and the adapter adds the 273.15 — the one unit
// conversion this feature owns (root TODO §WX-DESIGN §5).
static const char *
weather_units_wxg(const char *kv)
{
  return(strcmp(kv, "imperial") == 0 ? "us" : "si");
}

// Leg C, and the last of them: the forecast the local Weather Forecast
// Office wrote. Anything short of real periods — uncovered, non-2xx,
// unparseable, empty — falls back to openweather's daily view, which is
// exactly what shipped before this leg existed.
static void
weather_forecast_done(const weathergov_forecast_result_t *res, void *user)
{
  weather_req_t *r = (weather_req_t *)user;
  weather_view_forecast_t view;
  cmd_ctx_t ctx = r->ctx;

  if(res->err[0] != '\0' || !res->covered || res->forecast.count == 0)
  {
    clam(CLAM_DEBUG, WEATHER_CTX, "forecast %s: weathergov gave nothing (%s)",
        r->loc.zip, res->err[0] != '\0' ? res->err : "no periods");

    weather_dispatch_openweather(r);
    return;
  }

  ctx.msg = &r->msg;

  weather_view_from_wxg_forecast(&res->forecast, r->loc.place, r->loc.zip,
      openweather_units_kv_value(), &view);

  clam(CLAM_DEBUG, WEATHER_CTX, "forecast %s: %u period(s) -> %u day row(s)",
      r->loc.zip, res->forecast.count, view.count);

  weather_reply_forecast_daily(&ctx, &view, &r->alerts);

  mem_free(r);
}

// Leg C for the current view: what the nearest station measured, plus the
// two periods that state today's extremes and the office's prose. The
// observation is the point of it — without one there is no line to
// print that openweather does not print better, so anything short of it
// falls back whole.
static void
weather_current_done(const weathergov_current_result_t *res, void *user)
{
  weather_req_t *r = (weather_req_t *)user;
  weather_view_current_t view;
  cmd_ctx_t ctx = r->ctx;

  if(res->err[0] != '\0' || !res->covered || !res->obs.have_temp)
  {
    clam(CLAM_DEBUG, WEATHER_CTX, "current %s: weathergov gave nothing (%s)",
        r->loc.zip, res->err[0] != '\0' ? res->err : "no observation");

    weather_dispatch_openweather(r);
    return;
  }

  ctx.msg = &r->msg;

  weather_view_from_wxg_current(&res->obs, &res->forecast, &r->point,
      r->loc.place, r->loc.zip, openweather_units_kv_value(), &view);

  clam(CLAM_DEBUG, WEATHER_CTX, "current %s: station %s, %u period(s)",
      r->loc.zip, res->obs.station, res->forecast.count);

  weather_reply_current(&ctx, &view, &r->alerts);

  mem_free(r);
}

// Leg B, and the fork: the forecast grid this coordinate sits in. Alerts
// took raw coordinates, but every gridpoint URL is built from the
// office/x/y triple, so both views need this one lookup first — from the
// point cache in every case but the first (7 days, and a grid does not
// move).
//
// The point is kept rather than consumed: it carries the sunrise and
// sunset the current view prints, at no further cost.
static void
weather_point_done(const weathergov_point_result_t *res, void *user)
{
  weather_req_t *r = (weather_req_t *)user;
  const char *units = weather_units_wxg(openweather_units_kv_value());
  bool submitted;

  if(res->err[0] != '\0' || !res->covered)
  {
    clam(CLAM_DEBUG, WEATHER_CTX, "route %s: no grid (%s)", r->loc.zip,
        res->err[0] != '\0' ? res->err : "uncovered");

    weather_dispatch_openweather(r);
    return;
  }

  r->point = res->point;

  // Only ASYNC_FAILED_UNDELIVERED leaves the request ours to hand on to
  // openweather — never a reply and never a free.
  if(r->kind == WEATHER_REQ_CURRENT)
    submitted = (weathergov_current_async(&res->point, units,
        weather_current_done, r) != ASYNC_FAILED_UNDELIVERED);
  else
    submitted = (weathergov_forecast_async(&res->point, units,
        weather_forecast_done, r) != ASYNC_FAILED_UNDELIVERED);

  if(submitted)
    return;

  clam(CLAM_DEBUG, WEATHER_CTX, "route %s: weathergov refused the "
      "conditions leg", r->loc.zip);

  weather_dispatch_openweather(r);
}

// Leg A of the chain: weather.gov's alerts, and with them its verdict
// on whether this coordinate is American at all. Runs on the curl multi
// worker, and hands straight on to leg B — openweather, for the
// conditions — without a join, because both completions land on that
// same single thread and a concurrent fetch would only be theatre.
//
// EVERY failure here is soft. Timeout, non-2xx, unparseable, uncovered:
// all of them leave `have_alerts` false and openweather's own alert
// path switched on, which is exactly the behaviour that shipped before
// weather.gov existed. An outage must degrade !weather, never break it.
static void
weather_alerts_done(const weathergov_alert_result_t *res, void *user)
{
  weather_req_t *r = (weather_req_t *)user;

  if(res->err[0] != '\0')
    clam(CLAM_DEBUG, WEATHER_CTX, "route %s: weathergov unavailable (%s)",
        r->loc.zip, res->err);

  else if(!res->covered)
    clam(CLAM_DEBUG, WEATHER_CTX, "route %s: us=no", r->loc.zip);

  else
  {
    // Covered and quiet is an answer too: it is what lets a US location
    // with no active alert skip openweather's enrichment entirely.
    weather_view_from_wxg_alerts(&res->alerts, &r->alerts);
    r->have_alerts = true;

    clam(CLAM_DEBUG, WEATHER_CTX, "route %s: us=yes alerts=%u (%u carried)",
        r->loc.zip, res->alerts.total, res->alerts.count);
  }

  // A covered coordinate is the only thing that earns the legs below:
  // the current and daily views are the two weather.gov answers better
  // than OpenWeather in every column that matters, and `have_alerts` is
  // already the coverage verdict — there is no second coverage test.
  // Everything else — the hourly view, and every failure downstream of
  // here — goes to openweather.
  if(r->have_alerts && (r->kind == WEATHER_REQ_FORECAST_DAILY
      || r->kind == WEATHER_REQ_CURRENT))
  {
    if(weathergov_point_async(r->loc.lat, r->loc.lon,
        weather_point_done, r) != ASYNC_FAILED_UNDELIVERED)
      return;

    clam(CLAM_DEBUG, WEATHER_CTX, "route %s: weathergov refused the point "
        "leg", r->loc.zip);
  }

  weather_dispatch_openweather(r);
}

// Command callbacks

// /weather [-h | -d] <zipcode | city> — the single entry point for
// every weather view:
//   (no flag)  current conditions
//   -h         24-hour forecast (two-column)
//   -d         7-day forecast
// A city-name location is synchronously geocoded to a zipcode and then
// reuses the zipcode path, so all three views accept either form. Runs
// on a task-worker thread.
static void
weather_cmd_weather(const cmd_ctx_t *ctx)
{
  const char *input;
  weather_req_kind_t kind = WEATHER_REQ_CURRENT;
  weather_loc_t loc;
  weather_req_t *r;

  if(ctx->parsed != NULL && ctx->parsed->argc > 0)
    input = ctx->parsed->argv[0];
  else
    input = ctx->args;

  // Optional leading mode flag: -h hourly, -d daily. Anything else is
  // taken verbatim as the location.
  if(input != NULL && input[0] == '-'
      && (input[1] == 'h' || input[1] == 'd')
      && (input[2] == ' ' || input[2] == '\0'))
  {
    kind = (input[1] == 'h') ? WEATHER_REQ_FORECAST_HOURLY
                             : WEATHER_REQ_FORECAST_DAILY;
    input += 2;

    while(*input == ' ')
      input++;

    if(*input == '\0')
      input = NULL;
  }

  if(input == NULL || input[0] == '\0')
  {
    cmd_reply(ctx, "Usage: weather [-h | -d] <zipcode | city>");
    return;
  }

  memset(&loc, 0, sizeof(loc));

  if(weather_resolve_location(input, &loc) != SUCCESS)
  {
    cmd_reply(ctx,
        "I don't recognize that location. Try a US zipcode or a "
        "city name.");
    return;
  }

  r = weather_req_new(ctx, kind);
  r->loc = loc;

  if(weathergov_enabled())
  {
    clam(CLAM_DEBUG, WEATHER_CTX, "route %s (%.4f,%.4f): asking weathergov",
        loc.zip, loc.lat, loc.lon);

    // A FAIL here means the callback did NOT fire, so the request is
    // still ours to hand on — never a reply and never a free.
    if(weathergov_alerts_async(loc.lat, loc.lon,
        weather_alerts_done, r) != ASYNC_FAILED_UNDELIVERED)
      return;

    clam(CLAM_DEBUG, WEATHER_CTX, "route %s: weathergov refused the alerts "
        "leg", loc.zip);
  }

  weather_dispatch_openweather(r);
}

// NL hints

static const cmd_nl_slot_t weather_weather_slots[] = {
  { .name  = "location",
    .type  = CMD_NL_ARG_LOCATION,
    .flags = CMD_NL_SLOT_OPTIONAL | CMD_NL_SLOT_USER_DEFAULT },
};

static const cmd_nl_example_t weather_weather_examples[] = {
  { .utterance  = "what's the weather like today?",
    .invocation = "/weather" },
  { .utterance  = "tell me the weather in 45069",
    .invocation = "/weather 45069" },
  { .utterance  = "how's the weather in london?",
    .invocation = "/weather London" },
  { .utterance  = "what's it like in santa fe right now?",
    .invocation = "/weather Santa Fe" },
  { .utterance  = "hourly forecast for Cincinnati",
    .invocation = "/weather -h Cincinnati" },
  { .utterance  = "what's the 7-day forecast for 90210?",
    .invocation = "/weather -d 90210" },
};

static const cmd_nl_t weather_weather_nl = {
  .when          = "User asks about current or forecast weather for any "
                   "place in the world.",
  .syntax        = "/weather [-h | -d] <zipcode | city name — bare "
                   "(\"Santa Fe\", \"London\") or comma-qualified "
                   "(\"Santa Fe, NM\"); never a bare state suffix>",
  .slots         = weather_weather_slots,
  .slot_count    = (uint8_t)(sizeof(weather_weather_slots)
                             / sizeof(weather_weather_slots[0])),
  .examples      = weather_weather_examples,
  .example_count = (uint8_t)(sizeof(weather_weather_examples)
                             / sizeof(weather_weather_examples[0])),
};

// Plugin lifecycle

static bool
weather_init(void)
{
  if(cmd_register(WEATHER_CTX, "weather",
      "weather [-h | -d] <zipcode | city>",
      "Show weather for a US zipcode or city (current / hourly / daily)",
      "Queries the OpenWeather One Call 4.0 API. Accepts either a US\n"
      "zipcode or a city name (geocoded via OpenWeather's\n"
      "direct-geocoding endpoint).\n"
      "\n"
      "  !weather <location>       current conditions\n"
      "  !weather -h <location>    24-hour forecast (two-column)\n"
      "  !weather -d <location>    7-day forecast\n"
      "\n"
      "Current conditions show temperature, humidity, wind,\n"
      "sunrise/sunset, and any active alerts.\n"
      "\n"
      "Requires plugin.openweather.creds.apikey to be set.\n"
      "Units controlled by plugin.openweather.units (imperial/metric).\n"
      "\n"
      "Example: !weather 90210\n"
      "         !weather -h Cincinnati\n"
      "         !weather -d 10001",
      "everyone", 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      weather_cmd_weather, NULL, NULL, "w",
      weather_ad_weather, 1, NULL, &weather_weather_nl) != SUCCESS)
    return(FAIL);

  clam(CLAM_INFO, WEATHER_CTX, "weather command plugin initialized");

  return(SUCCESS);
}

static void
weather_deinit(void)
{
  cmd_unregister_path("weather");

  clam(CLAM_INFO, WEATHER_CTX, "weather command plugin deinitialized");
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "weather",
  .version         = "1.0",
  .type            = PLUGIN_MISC,
  .kind            = "weather",
  .provides        = { { .name = "cmd_weather" } },
  .provides_count  = 1,
  .requires        = {
    { .name = "bot_chat" },
    { .name = "service_openweather" },
    { .name = "service_weathergov" },
  },
  .requires_count  = 3,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = weather_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = weather_deinit,
  .ext             = NULL,
};
