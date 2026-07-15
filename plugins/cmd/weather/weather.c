// botmanager — MIT
// Weather command-surface plugin: /weather + /forecast via openweather.
#define WEATHER_INTERNAL
#include "weather.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "colors.h"

// Day-name tables, shared by forecast formatters.

static const char *const weather_day_names_full[] = {
  "Sunday", "Monday", "Tuesday", "Wednesday",
  "Thursday", "Friday", "Saturday",
};

static const char *const weather_day_names_abbr[] = {
  "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
};

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

// Unit + display helpers

static const char *
weather_temp_unit(const char *units)
{
  if(strcmp(units, "metric") == 0)
    return("C");

  if(strcmp(units, "standard") == 0)
    return("K");

  return("F");
}

static const char *
weather_speed_unit(const char *units)
{
  if(strcmp(units, "imperial") == 0)
    return("mph");

  return("m/s");
}

static const char *
weather_wind_dir(double deg)
{
  static const char *dirs[] = {
    "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
    "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"
  };
  int idx = ((int)((deg + 11.25) / 22.5)) % 16;

  return(dirs[idx]);
}

static double
weather_to_fahrenheit(double temp, const char *units)
{
  if(strcmp(units, "metric") == 0)
    return(temp * 9.0 / 5.0 + 32.0);

  if(strcmp(units, "standard") == 0)
    return((temp - 273.15) * 9.0 / 5.0 + 32.0);

  return(temp);
}

static const char *
weather_temp_color(double temp_f)
{
  if(temp_f >= 100.0) return(CLR_BOLD CLR_RED);
  if(temp_f >=  90.0) return(CLR_RED);
  if(temp_f >=  80.0) return(CLR_ORANGE);
  if(temp_f >=  70.0) return(CLR_YELLOW);
  if(temp_f <    0.0) return(CLR_BOLD CLR_PURPLE);
  if(temp_f <   25.0) return(CLR_BOLD CLR_BLUE);
  if(temp_f <   40.0) return(CLR_BLUE);
  if(temp_f <   55.0) return(CLR_CYAN);

  return("");
}

static int
weather_fmt_temp(char *buf, size_t sz, double temp, const char *units)
{
  const char *clr = weather_temp_color(weather_to_fahrenheit(temp, units));

  // The bold toggle pair (CLR_BOLD CLR_BOLD) between the color code
  // and the digit prevents IRC clients from consuming temperature
  // digits as part of the \003NN color parameter sequence.
  if(*clr != '\0')
    return(snprintf(buf, sz, "%s" CLR_BOLD CLR_BOLD "%.0f%s",
        clr, temp, CLR_RESET));

  return(snprintf(buf, sz, "%.0f", temp));
}

// As weather_fmt_temp, but right-justifies the numeric part to a fixed
// visible width so temperatures line up as a column. The padding lands
// *inside* the colour run (leading spaces, never a digit), which also
// sidesteps the \003NN digit-eating hazard the bold toggle guards.
static int
weather_fmt_temp_w(char *buf, size_t sz, double temp, const char *units,
    int width)
{
  const char *clr = weather_temp_color(weather_to_fahrenheit(temp, units));

  if(*clr != '\0')
    return(snprintf(buf, sz, "%s" CLR_BOLD CLR_BOLD "%*.0f%s",
        clr, width, temp, CLR_RESET));

  return(snprintf(buf, sz, "%*.0f", width, temp));
}

// Map a weather condition ID to a Unicode weather emoji (UTF-8). Must
// be placed at the START of each line so variable emoji width does
// not break column alignment.
static const char *
weather_condition_icon(int id)
{
  if(id >= 200 && id < 300)
    return("\xf0\x9f\x8c\xa9\xef\xb8\x8f");  // 🌩️ thunderstorm

  if(id >= 300 && id < 400)
    return("\xf0\x9f\x8c\xa7\xef\xb8\x8f");  // 🌧️ drizzle

  if(id >= 500 && id < 600)
    return("\xf0\x9f\x8c\xa7\xef\xb8\x8f");  // 🌧️ rain

  if(id >= 600 && id < 700)
    return("\xf0\x9f\x8c\xa8\xef\xb8\x8f");  // 🌨️ snow

  if(id >= 700 && id < 800)
    return("\xf0\x9f\x8c\xab\xef\xb8\x8f");  // 🌫️ fog/mist/haze

  if(id == 800)
    return("\xe2\x98\x80\xef\xb8\x8f");       // ☀️  clear

  if(id == 801 || id == 802)
    return("\xf0\x9f\x8c\xa5\xef\xb8\x8f");  // 🌥️ few/scattered

  if(id >= 803)
    return("\xf0\x9f\x8c\xa5\xef\xb8\x8f");  // 🌥️ overcast

  return("\xf0\x9f\x8c\xa1\xef\xb8\x8f");    // 🌡️ fallback
}

static const char *
weather_condition_color(int id)
{
  if(id >= 200 && id < 300) return(CLR_RED);        // thunderstorm
  if(id >= 300 && id < 400) return(CLR_CYAN);       // drizzle
  if(id >= 500 && id < 600) return(CLR_CYAN);       // rain
  if(id >= 600 && id < 700) return(CLR_BOLD);       // snow
  if(id >= 700 && id < 800) return(CLR_PURPLE);     // fog/mist/haze
  if(id == 800)             return(CLR_YELLOW);      // clear sky
  if(id == 801)             return(CLR_YELLOW);      // few clouds
  if(id == 802)             return(CLR_GRAY);        // scattered clouds
  if(id >= 803)             return(CLR_GRAY);        // overcast

  return("");
}

static void
weather_format_time_ampm(time_t ts, int tz_offset, char *buf, size_t sz)
{
  int h;
  time_t local = ts + tz_offset;
  struct tm tm;

  gmtime_r(&local, &tm);

  h = tm.tm_hour % 12;

  if(h == 0)
    h = 12;

  snprintf(buf, sz, "%d:%02d%s", h, tm.tm_min,
      tm.tm_hour < 12 ? "am" : "pm");
}

// Synthetic zipcodes take the shape G+8 hex digits; they arrive from
// the city-name path's lat/lon fallback when OpenWeather has coords
// but no postcode. They're purely internal cache keys and must not
// leak into human-facing replies.
static bool
weather_zip_is_synth(const char *zip)
{
  size_t i;

  if(zip == NULL || zip[0] != 'G')
    return(false);

  for(i = 1; i < 9; i++)
  {
    if(!((zip[i] >= '0' && zip[i] <= '9')
        || (zip[i] >= 'A' && zip[i] <= 'F')))
      return(false);
  }

  return(zip[9] == '\0');
}

static void
weather_fmt_precip(char *buf, size_t sz, int pop)
{
  if(pop > 0)
    snprintf(buf, sz, " " CLR_CYAN "%d%% precip" CLR_RESET, pop);
  else
    buf[0] = '\0';
}

static void
weather_fmt_desc_pad(char *buf, size_t sz, const char *desc, int width)
{
  snprintf(buf, sz, "%-*.*s", width, width, desc);
}

static void
weather_reply_header(const cmd_ctx_t *ctx, const char *place,
    const char *zip, const char *subtitle)
{
  char buf[WEATHER_REPLY_SZ];

  if(weather_zip_is_synth(zip))
    snprintf(buf, sizeof(buf),
        CLR_BOLD "%s" CLR_RESET " "
        "\xe2\x80\x94 " CLR_BOLD "%s" CLR_RESET,
        place, subtitle);
  else
    snprintf(buf, sizeof(buf),
        CLR_BOLD "%s" CLR_RESET " (%s) "
        "\xe2\x80\x94 " CLR_BOLD "%s" CLR_RESET,
        place, zip, subtitle);

  cmd_reply(ctx, buf);
}

static void
weather_reply_alerts(const cmd_ctx_t *ctx, const openweather_alert_set_t *a)
{
  char buf[WEATHER_REPLY_SZ];
  uint8_t i;

  for(i = 0; i < a->count && i < 3; i++)
  {
    if(a->alerts[i].event[0] == '\0')
      continue;

    snprintf(buf, sizeof(buf),
        "  " CLR_BOLD CLR_RED "\xe2\x9a\xa0" CLR_RESET " "
        CLR_BOLD CLR_YELLOW "ALERT:" CLR_RESET " %s",
        a->alerts[i].event);

    cmd_reply(ctx, buf);
  }
}

// Reply formatters (consume typed payloads)

static void
weather_reply_current(const cmd_ctx_t *ctx,
    const openweather_current_t *cur,
    const openweather_alert_set_t *alerts)
{
  char ct[32], cf[32];
  char buf[WEATHER_REPLY_SZ];
  char sunrise[12], sunset[12];
  const char *tu = weather_temp_unit(cur->units);
  const char *su = weather_speed_unit(cur->units);
  const char *icon = weather_condition_icon(cur->condition_id);
  const char *dclr = weather_condition_color(cur->condition_id);

  sunrise[0] = sunset[0] = '?';
  sunrise[1] = sunset[1] = '\0';

  if(cur->sunrise > 0)
    weather_format_time_ampm(cur->sunrise, cur->tz_offset,
        sunrise, sizeof(sunrise));

  if(cur->sunset > 0)
    weather_format_time_ampm(cur->sunset, cur->tz_offset,
        sunset, sizeof(sunset));

  weather_fmt_temp(ct, sizeof(ct), cur->temp,       cur->units);
  weather_fmt_temp(cf, sizeof(cf), cur->feels_like, cur->units);

  // Line 1: icon + location + condition + temperature.
  if(weather_zip_is_synth(cur->zipcode))
    snprintf(buf, sizeof(buf),
        "%s " CLR_BOLD "%s" CLR_RESET " "
        "\xe2\x80\x94 %s%s" CLR_RESET
        " " CLR_GRAY "\xc2\xb7" CLR_RESET " "
        "%s\xc2\xb0%s (feels %s\xc2\xb0%s)",
        icon, cur->place_name,
        dclr, cur->condition_desc,
        ct, tu, cf, tu);
  else
    snprintf(buf, sizeof(buf),
        "%s " CLR_BOLD "%s" CLR_RESET " (%s) "
        "\xe2\x80\x94 %s%s" CLR_RESET
        " " CLR_GRAY "\xc2\xb7" CLR_RESET " "
        "%s\xc2\xb0%s (feels %s\xc2\xb0%s)",
        icon, cur->place_name, cur->zipcode,
        dclr, cur->condition_desc,
        ct, tu, cf, tu);

  cmd_reply(ctx, buf);

  // Line 2: hi/lo + humidity + wind + sunrise/sunset.
  if(cur->have_hilo)
  {
    char chi[32], clo[32];

    weather_fmt_temp(chi, sizeof(chi), cur->temp_hi, cur->units);
    weather_fmt_temp(clo, sizeof(clo), cur->temp_lo, cur->units);

    snprintf(buf, sizeof(buf),
        "  Hi %s\xc2\xb0/Lo %s\xc2\xb0%s"
        " " CLR_GRAY "\xc2\xb7" CLR_RESET " "
        CLR_CYAN "Humidity" CLR_RESET " %d%%"
        " " CLR_GRAY "\xc2\xb7" CLR_RESET " "
        CLR_GREEN "Wind" CLR_RESET " %.0f%s %s"
        " " CLR_GRAY "\xc2\xb7" CLR_RESET " "
        CLR_YELLOW "Rise" CLR_RESET " %s "
        CLR_PURPLE "Set" CLR_RESET " %s",
        chi, clo, tu,
        cur->humidity,
        cur->wind_speed, su, weather_wind_dir(cur->wind_deg),
        sunrise, sunset);
  }

  else
    snprintf(buf, sizeof(buf),
        "  "
        CLR_CYAN "Humidity" CLR_RESET " %d%%"
        " " CLR_GRAY "\xc2\xb7" CLR_RESET " "
        CLR_GREEN "Wind" CLR_RESET " %.0f%s %s"
        " " CLR_GRAY "\xc2\xb7" CLR_RESET " "
        CLR_YELLOW "Rise" CLR_RESET " %s "
        CLR_PURPLE "Set" CLR_RESET " %s",
        cur->humidity,
        cur->wind_speed, su, weather_wind_dir(cur->wind_deg),
        sunrise, sunset);

  cmd_reply(ctx, buf);

  weather_reply_alerts(ctx, alerts);
}

static void
weather_reply_forecast_daily(const cmd_ctx_t *ctx,
    const openweather_forecast_t *f,
    const openweather_alert_set_t *alerts)
{
  const char *tu = weather_temp_unit(f->units);
  const char *su = weather_speed_unit(f->units);
  uint8_t i;

  weather_reply_header(ctx, f->place_name, f->zipcode, "7-day forecast");

  for(i = 0; i < f->day_count && i < 7; i++)
  {
    const openweather_forecast_day_t *d = &f->days[i];
    const char *day_name = "???";
    const char *icon;
    const char *dclr;
    char chi[32], clo[32];
    char desc_pad[24];
    char precip[24];
    char buf[WEATHER_REPLY_SZ];
    int pop = (int)(d->pop * 100);

    if(d->dt > 0)
    {
      time_t dt = d->dt + f->tz_offset;
      struct tm tm;

      gmtime_r(&dt, &tm);
      day_name = weather_day_names_full[tm.tm_wday];
    }

    icon = weather_condition_icon(d->condition_id);
    dclr = weather_condition_color(d->condition_id);

    weather_fmt_temp(chi, sizeof(chi), d->temp_hi, f->units);
    weather_fmt_temp(clo, sizeof(clo), d->temp_lo, f->units);

    weather_fmt_desc_pad(desc_pad, sizeof(desc_pad),
        d->condition_desc, 22);

    weather_fmt_precip(precip, sizeof(precip), pop);

    snprintf(buf, sizeof(buf),
        "%s %-9.9s  %s/%s\xc2\xb0%s"
        "  %s%s" CLR_RESET
        "  %2d%%"
        "  %2.0f%s %-3s"
        "%s",
        icon, day_name, chi, clo, tu,
        dclr, desc_pad,
        d->humidity,
        d->wind_speed, su, weather_wind_dir(d->wind_deg),
        precip);

    cmd_reply(ctx, buf);
  }

  weather_reply_alerts(ctx, alerts);
}

// Render one hour into a fixed-visible-width cell for the two-column
// hourly view. Every field is padded on its *raw* text (colour codes
// wrap already-padded content), so a cell's on-screen width is constant
// and the second column lands at a predictable position. The layout is
//
//   {icon} {Day} {time}  {temp}°{u}  {condition:16}  {pop}
//
// which measures ~42 display columns; two cells plus a two-space gutter
// stay under the 100-column budget with room to spare.
//
// The trailing precipitation-probability field is shown only when there
// *is* a chance (mirroring the daily view): a dry hour leaves the
// 4-column slot blank so the eye locks onto the hours that carry rain
// odds instead of a wall of "0%". The slot keeps its fixed width either
// way, preserving column alignment.
static void
weather_hour_cell(char *buf, size_t sz, const openweather_forecast_hour_t *h,
    const char *units, const char *tu, int tz_offset)
{
  const char *icon = weather_condition_icon(h->condition_id);
  const char *dclr = weather_condition_color(h->condition_id);
  const char *day_name = "???";
  char temp[40];
  char desc_pad[24];
  char time_str[8];
  char pop_str[24];
  int pop = (int)(h->pop * 100);

  time_str[0] = time_str[1] = '?';
  time_str[2] = '\0';

  if(h->dt > 0)
  {
    int hour12;
    time_t dt = h->dt + tz_offset;
    struct tm tm;

    gmtime_r(&dt, &tm);
    day_name = weather_day_names_abbr[tm.tm_wday];

    hour12 = tm.tm_hour % 12;

    if(hour12 == 0)
      hour12 = 12;

    snprintf(time_str, sizeof(time_str), "%d%s",
        hour12, tm.tm_hour < 12 ? "am" : "pm");
  }

  weather_fmt_temp_w(temp, sizeof(temp), h->temp, units, 3);
  weather_fmt_desc_pad(desc_pad, sizeof(desc_pad), h->condition_desc, 16);

  // Fixed 4-column precip slot: coloured "NN%" when there's a chance,
  // blank otherwise.
  if(pop > 0)
    snprintf(pop_str, sizeof(pop_str), CLR_CYAN "%3d%%" CLR_RESET, pop);
  else
    snprintf(pop_str, sizeof(pop_str), "    ");

  snprintf(buf, sz,
      "%s %-3s %4s  %s\xc2\xb0%s  %s%s" CLR_RESET "  %s",
      icon, day_name, time_str, temp, tu, dclr, desc_pad, pop_str);
}

// Double-column hourly forecast: 24 hours collapse into ~12 reply
// lines, two cells each, so IRC clients aren't flooded. Hourly
// deliberately drops wind/humidity (kept in the daily view) to stay
// within a sane line width; the essentials — time, temperature, sky,
// precip odds — remain.
//
// Cells are laid out COLUMN-MAJOR: the left column holds the earlier
// half of the hours top-to-bottom, the right column the later half, so
// a human reads each column straight down rather than zig-zagging
// left↔right across every row.
static void
weather_reply_forecast_hourly(const cmd_ctx_t *ctx,
    const openweather_forecast_t *f,
    const openweather_alert_set_t *alerts)
{
  const char *tu = weather_temp_unit(f->units);
  char cells[24][WEATHER_CELL_SZ];
  uint8_t n;
  uint8_t rows;
  uint8_t r;
  uint8_t i;

  weather_reply_header(ctx, f->place_name, f->zipcode, "24-hour forecast");

  n = (f->hour_count < 24) ? f->hour_count : 24;

  for(i = 0; i < n; i++)
    weather_hour_cell(cells[i], sizeof(cells[i]), &f->hours[i],
        f->units, tu, f->tz_offset);

  // Split point: the left column takes the first ceil(n/2) hours, so an
  // odd count leaves the lone trailing cell alone on the last row.
  rows = (uint8_t)((n + 1) / 2);

  for(r = 0; r < rows; r++)
  {
    char buf[WEATHER_REPLY_SZ];
    uint8_t right = (uint8_t)(rows + r);

    // Precision bounds each cell to its buffer so the compiler can see
    // the join stays well within WEATHER_REPLY_SZ (the runtime-indexed
    // cells[r] otherwise reads as reaching the end of the 2-D array).
    if(right < n)
      snprintf(buf, sizeof(buf), "%.*s  %.*s",
          WEATHER_CELL_SZ - 1, cells[r],
          WEATHER_CELL_SZ - 1, cells[right]);
    else
      snprintf(buf, sizeof(buf), "%.*s", WEATHER_CELL_SZ - 1, cells[r]);

    cmd_reply(ctx, buf);
  }

  weather_reply_alerts(ctx, alerts);
}

// Async completion callbacks. Fire on the openweather curl-multi worker
// thread; cmd_reply is thread-safe. Each frees the per-request closure.

static void
weather_done_current(const openweather_current_result_t *res, void *user)
{
  weather_req_t *r = (weather_req_t *)user;
  cmd_ctx_t ctx = r->ctx;

  ctx.msg = &r->msg;

  if(res->err[0] != '\0')
    cmd_reply(&ctx, res->err);
  else
    weather_reply_current(&ctx, &res->current, &res->alerts);

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

  if(r->kind == WEATHER_REQ_FORECAST_HOURLY)
    weather_reply_forecast_hourly(&ctx, &res->forecast, &res->alerts);
  else
    weather_reply_forecast_daily(&ctx, &res->forecast, &res->alerts);

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
  char zip[OPENWEATHER_ZIPCODE_SZ];
  weather_req_kind_t kind = WEATHER_REQ_CURRENT;
  bool has_digit;
  bool submitted;
  weather_req_t *r;
  size_t i;

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

  // Any ASCII digit routes through the zipcode path; pure-alpha
  // inputs go through the city geocoder.
  has_digit = false;
  for(i = 0; input[i] != '\0'; i++)
  {
    if(input[i] >= '0' && input[i] <= '9')
    {
      has_digit = true;
      break;
    }
  }

  zip[0] = '\0';

  if(has_digit)
  {
    snprintf(zip, sizeof(zip), "%s", input);
  }

  else if(openweather_geocode_city_sync(input, zip, sizeof(zip)) != SUCCESS)
  {
    cmd_reply(ctx,
        "I don't recognize that location. Try a US zipcode or a "
        "city name.");
    return;
  }

  r = weather_req_new(ctx, kind);

  switch(kind)
  {
    case WEATHER_REQ_FORECAST_HOURLY:
      submitted = (openweather_fetch_forecast_hourly(zip,
          weather_done_forecast, r) == SUCCESS);
      break;

    case WEATHER_REQ_FORECAST_DAILY:
      submitted = (openweather_fetch_forecast_daily(zip,
          weather_done_forecast, r) == SUCCESS);
      break;

    case WEATHER_REQ_CURRENT:
    default:
      submitted = (openweather_fetch_current(zip,
          weather_done_current, r) == SUCCESS);
      break;
  }

  if(!submitted)
  {
    cmd_reply(ctx,
        "Error: failed to submit weather request. "
        "Check plugin.openweather.apikey.");
    mem_free(r);
  }
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
  { .utterance  = "hourly forecast for Cincinnati",
    .invocation = "/weather -h Cincinnati" },
  { .utterance  = "what's the 7-day forecast for 90210?",
    .invocation = "/weather -d 90210" },
};

static const cmd_nl_t weather_weather_nl = {
  .when          = "User asks about current or forecast weather.",
  .syntax        = "/weather [-h | -d] <zipcode | city>",
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
      "Requires plugin.openweather.apikey to be set.\n"
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
  cmd_unregister("weather");

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
    { .name = "method_command" },
    { .name = "service_openweather" },
  },
  .requires_count  = 2,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = weather_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = weather_deinit,
  .ext             = NULL,
};
