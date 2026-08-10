// botmanager — MIT
// Weather presentation: colour, icon and column layout for every view.
#define WEATHER_INTERNAL
#define WEATHER_RENDER_TU
#include "weather.h"

#include <stdarg.h>
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

// The forecast adapter labels an OpenWeather daily row, which carries a
// timestamp where weather.gov carries a name. Naming a weekday is
// presentation, so the table stays here and the adapter asks for it.
const char *
weather_day_name_full(int wday)
{
  if(wday < 0 || wday > 6)
    return("???");

  return(weather_day_names_full[wday]);
}

// Unit + display helpers

const char *
weather_temp_unit(const char *units)
{
  if(strcmp(units, "metric") == 0)
    return("C");

  if(strcmp(units, "standard") == 0)
    return("K");

  return("F");
}

const char *
weather_speed_unit(const char *units)
{
  if(strcmp(units, "imperial") == 0)
    return("mph");

  return("m/s");
}

const char *
weather_wind_dir(double deg)
{
  static const char *dirs[] = {
    "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
    "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"
  };
  int idx = ((int)((deg + 11.25) / 22.5)) % 16;

  return(dirs[idx]);
}

double
weather_to_fahrenheit(double temp, const char *units)
{
  if(strcmp(units, "metric") == 0)
    return(temp * 9.0 / 5.0 + 32.0);

  if(strcmp(units, "standard") == 0)
    return((temp - 273.15) * 9.0 / 5.0 + 32.0);

  return(temp);
}

const char *
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

int
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
int
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
//
// ⭑ Codes 900–905 are a PRIVATE range, and the one place this axis
// extends past OpenWeather's numbering. weather.gov names five sky
// states OpenWeather has no code for — tornado, hurricane, blizzard,
// tropical storm — plus the two extremes it calls out as conditions in
// their own right, hot and cold. They are tested FIRST because the
// existing `id >= 803` arm would otherwise swallow every one of them.
const char *
weather_condition_icon(int id)
{
  if(id >= 900)
  {
    switch(id)
    {
      case 900: return("\xf0\x9f\x8c\xaa\xef\xb8\x8f");  // 🌪️ tornado
      case 901: return("\xf0\x9f\x8c\x80");               // 🌀 hurricane
      case 902: return("\xf0\x9f\x8c\xa8\xef\xb8\x8f");  // 🌨️ blizzard
      case 903: return("\xf0\x9f\x8c\x80");               // 🌀 tropical storm
      case 904: return("\xf0\x9f\xa5\xb5");               // 🥵 hot
      case 905: return("\xf0\x9f\xa5\xb6");               // 🥶 cold
      default:  break;
    }
  }

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

const char *
weather_condition_color(int id)
{
  // The private range, ahead of the `id >= 803` arm for the same reason.
  if(id == 900)             return(CLR_BOLD CLR_RED);  // tornado
  if(id == 901)             return(CLR_BOLD CLR_RED);  // hurricane
  if(id == 902)             return(CLR_BOLD);          // blizzard
  if(id == 903)             return(CLR_RED);           // tropical storm
  if(id == 904)             return(CLR_ORANGE);        // hot
  if(id == 905)             return(CLR_BOLD CLR_BLUE); // cold

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

void
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
bool
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

// The precipitation slot, four columns wide either way: coloured "NN%"
// when there is a chance, blank when there is not. A dry row leaves the
// slot empty rather than printing "0%", so the eye lands on the rows
// that carry odds — and the fixed width is what keeps every column to
// its right aligned across a whole forecast.
void
weather_fmt_precip(char *buf, size_t sz, int pop)
{
  if(pop > 0)
    snprintf(buf, sz, CLR_CYAN "%3d%%" CLR_RESET, pop);
  else
    snprintf(buf, sz, "    ");
}

// The forecast office's own prose, cut to one line.
//
// It runs to a paragraph upstream — "A slight chance of showers and
// thunderstorms before 11am, then a chance of showers and thunderstorms
// after 2pm. Partly sunny, with a high near 89…" — and one line of it is
// worth more than none. The cut lands on a word boundary and says so
// with an ellipsis; anything else reads as a bug.
//
// The whole line is plain: every other line in this view colours its
// field LABELS, and this line is not fields, it is a sentence.
void
weather_fmt_detail(char *buf, size_t sz, const char *name, const char *detail)
{
  // Two leading spaces to sit under the header, and the ellipsis is the
  // one multi-byte character this line can gain — the budget below is
  // therefore counted in bytes, which for ASCII prose is columns.
  size_t budget = (sz - 1 < WEATHER_LINE_COLS) ? sz - 1 : WEATHER_LINE_COLS;
  size_t len    = (size_t)snprintf(buf, sz, "  %s: ", name);
  size_t i;
  size_t cut    = 0;

  if(len >= budget)
    return;

  for(i = 0; detail[i] != '\0' && len + i < budget; i++)
  {
    buf[len + i] = detail[i];

    if(detail[i] == ' ')
      cut = i;
  }

  // The whole thing fitted: no cut, no ellipsis.
  if(detail[i] == '\0')
  {
    buf[len + i] = '\0';
    return;
  }

  // No space anywhere in the budget is one unbroken word; cutting mid-word
  // is then the only honest option left.
  if(cut == 0)
    cut = i;

  snprintf(buf + len + cut, sz - len - cut, "\xe2\x80\xa6");
}

void
weather_fmt_desc_pad(char *buf, size_t sz, const char *desc, int width)
{
  snprintf(buf, sz, "%-*.*s", width, width, desc);
}

// The interpunct that joins one segment of a line to the next. It is
// grey because it is punctuation, and it belongs to the segment that
// follows it — which is what lets an absent field vanish without leaving
// a separator behind.
#define WEATHER_SEG_DOT  " " CLR_GRAY "\xc2\xb7" CLR_RESET " "

static void
weather_line_add(weather_line_t *l, const char *fmt, ...)
{
  va_list ap;
  int     n;

  if(l->len + 1 >= l->sz)
    return;

  if(l->any)
    l->len += (size_t)snprintf(l->buf + l->len, l->sz - l->len,
        WEATHER_SEG_DOT);

  if(l->len + 1 >= l->sz)
    return;

  va_start(ap, fmt);
  n = vsnprintf(l->buf + l->len, l->sz - l->len, fmt, ap);
  va_end(ap);

  if(n > 0)
    l->len += (size_t)n;

  // snprintf reports what it WOULD have written; a truncated segment
  // must still leave the length inside the buffer.
  if(l->len >= l->sz)
    l->len = l->sz - 1;

  l->any = true;
}

void
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

// ----------------------------------------------------------------------
// Alerts
// ----------------------------------------------------------------------

// What a line costs on screen. Colour markup is a two-byte marker (see
// colors.h) that core rewrites per method and occupies no column;
// UTF-8 continuation bytes are part of a glyph already counted; and a
// glyph followed by the variation selector U+FE0F is drawn
// double-width, which is what makes the weather emoji two columns and
// the bare ⚠ one.
static int
weather_visible_cols(const char *s)
{
  int    cols = 0;
  size_t i;

  for(i = 0; s[i] != '\0'; i++)
  {
    unsigned char c = (unsigned char)s[i];

    if(c == 0x01)
    {
      if(s[i + 1] != '\0')
        i++;

      continue;
    }

    if(c == 0xef && (unsigned char)s[i + 1] == 0xb8
        && (unsigned char)s[i + 2] == 0x8f)
    {
      cols++;
      i += 2;
      continue;
    }

    if(c < 0x80 || c >= 0xc0)
      cols++;
  }

  return(cols);
}

static const char *
weather_alert_color(uint8_t severity)
{
  switch(severity)
  {
    case 4:  return(CLR_BOLD CLR_RED);   // Extreme
    case 3:  return(CLR_RED);            // Severe
    case 2:  return(CLR_ORANGE);         // Moderate
    case 1:  return(CLR_YELLOW);         // Minor
    default: return(CLR_GRAY);           // Unknown
  }
}

// "12:45pm" when the alert lifts today, "Wed 8:00am" when it does not —
// both read in the alert's own timezone, never the daemon's.
static void
weather_alert_until_str(const weather_view_alert_t *a, char *buf, size_t sz)
{
  struct tm nowtm;
  struct tm endtm;
  time_t    now_local;
  time_t    end_local;
  char      clock[12];

  buf[0] = '\0';

  if(a->until == 0)
    return;

  weather_format_time_ampm(a->until, a->tz_offset, clock, sizeof(clock));

  now_local = time(NULL) + a->tz_offset;
  end_local = a->until   + a->tz_offset;

  gmtime_r(&now_local, &nowtm);
  gmtime_r(&end_local, &endtm);

  if(nowtm.tm_year == endtm.tm_year && nowtm.tm_yday == endtm.tm_yday)
    snprintf(buf, sz, "%s", clock);
  else
    snprintf(buf, sz, "%s %s", weather_day_names_abbr[endtm.tm_wday], clock);
}

// Drop the rightmost ", "-joined item. Returns false once there is
// nothing left to drop, which is how the width loop knows to move on to
// the next thing it is allowed to shed.
static bool
weather_drop_last_item(char *csv)
{
  char *last = NULL;
  char *p;

  if(csv[0] == '\0')
    return(false);

  for(p = csv; *p != '\0'; p++)
  {
    if(p[0] == ',' && p[1] == ' ')
      last = p;
  }

  if(last != NULL)
    *last = '\0';
  else
    csv[0] = '\0';

  return(true);
}

// Every segment is omitted whole — separator included — when its source
// is absent. There is no such thing as a dangling " — " on this line.
static void
weather_alert_compose(char *buf, size_t sz, const weather_view_alert_t *a,
    const char *until, const char *impact, bool short_area)
{
  const char *clr   = weather_alert_color(a->severity);
  const char *glyph = (a->severity >= 3) ? "\xe2\x9a\xa0"   // ⚠
                                         : "\xe2\x9a\x91";  // ⚑
  char   area[96];
  size_t len;

  if(short_area && a->area_count > 0)
    snprintf(area, sizeof(area), "%u counties%s%s", (unsigned)a->area_count,
        a->state[0] != '\0' ? " " : "", a->state);
  else
    snprintf(area, sizeof(area), "%s", a->area);

  len = (size_t)snprintf(buf, sz, "  %s%s" CLR_RESET " %s%s" CLR_RESET,
      clr, glyph, clr, a->event);

  if(len >= sz)
    return;

  if(area[0] != '\0')
  {
    len += (size_t)snprintf(buf + len, sz - len, " \xe2\x80\x94 %s", area);

    if(len >= sz)
      return;
  }

  if(until[0] != '\0')
  {
    len += (size_t)snprintf(buf + len, sz - len,
        " " CLR_GRAY "\xc2\xb7" CLR_RESET " til %s", until);

    if(len >= sz)
      return;
  }

  if(impact[0] != '\0')
    snprintf(buf + len, sz - len,
        " " CLR_GRAY "\xc2\xb7" CLR_RESET " %s", impact);
}

// The line, inside its width budget.
//
// Compose, measure, and shed from the right until it fits: trailing
// impact items one at a time, then the area detail down to a count. The
// event, the time and the glyph are never dropped — a truncated alert
// must still say what it is and when it lifts.
static void
weather_alert_line(char *buf, size_t sz, const weather_view_alert_t *a)
{
  char until [24];
  char impact[128];
  bool short_area = false;

  weather_alert_until_str(a, until, sizeof(until));
  snprintf(impact, sizeof(impact), "%s", a->impact);

  for(;;)
  {
    weather_alert_compose(buf, sz, a, until, impact, short_area);

    if(weather_visible_cols(buf) <= WEATHER_LINE_COLS)
      return;

    if(weather_drop_last_item(impact))
      continue;

    if(!short_area && a->area_count > 0)
    {
      short_area = true;
      continue;
    }

    return;
  }
}

void
weather_reply_alerts(const cmd_ctx_t *ctx, const weather_view_alert_set_t *a)
{
  char    buf[WEATHER_REPLY_SZ];
  uint8_t shown = 0;
  uint8_t i;

  for(i = 0; i < a->count && shown < WEATHER_ALERT_LINES; i++)
  {
    if(a->alerts[i].event[0] == '\0')
      continue;

    weather_alert_line(buf, sizeof(buf), &a->alerts[i]);
    cmd_reply(ctx, buf);
    shown++;
  }

  if(shown == 0 || a->total <= shown)
    return;

  snprintf(buf, sizeof(buf),
      "  " CLR_GRAY "\xe2\x80\xa6" "and %u more active alert%s" CLR_RESET,
      (unsigned)(a->total - shown), (a->total - shown) == 1 ? "" : "s");

  cmd_reply(ctx, buf);
}

// Reply formatters (consume typed payloads)

// Current conditions, in two lines and sometimes three:
//
//   {icon} {Place} ({zip}) — {condition} · {temp}°{u} (feels {temp}°{u})
//     Hi {hi}°/Lo {lo}°{u} · Humidity {rh}% · Wind {n}{u} {dir} (g{n}) · Rise … Set …
//     {Today}: {the forecast office's own prose, cut to one line}
//
// Everything on line 2 is optional and every optional thing takes its own
// separator with it, so the openweather path — no gust, no prose, and a
// high/low only when One Call's daily timeline is steady enough to state
// one — renders exactly the two lines it always did.
void
weather_reply_current(const cmd_ctx_t *ctx,
    const weather_view_current_t *cur,
    const weather_view_alert_set_t *alerts)
{
  char ct[32], cf[32];
  char buf[WEATHER_REPLY_SZ];
  char sunrise[12], sunset[12];
  char zipseg[OPENWEATHER_ZIPCODE_SZ + 4];
  char feels[64];
  const char *tu = weather_temp_unit(cur->units);
  const char *su = weather_speed_unit(cur->units);
  const char *icon = weather_condition_icon(cur->condition_id);
  const char *dclr = weather_condition_color(cur->condition_id);
  weather_line_t line = { .buf = buf, .sz = sizeof(buf) };

  sunrise[0] = sunset[0] = '?';
  sunrise[1] = sunset[1] = '\0';

  if(cur->sunrise > 0)
    weather_format_time_ampm(cur->sunrise, cur->tz_offset,
        sunrise, sizeof(sunrise));

  if(cur->sunset > 0)
    weather_format_time_ampm(cur->sunset, cur->tz_offset,
        sunset, sizeof(sunset));

  weather_fmt_temp(ct, sizeof(ct), cur->temp, cur->units);

  // A synthetic zipcode is an internal cache key rather than anybody's
  // postcode, so the parentheses leave with it.
  if(weather_zip_is_synth(cur->zip))
    zipseg[0] = '\0';
  else
    snprintf(zipseg, sizeof(zipseg), " (%s)", cur->zip);

  feels[0] = '\0';

  if(cur->have_feels)
  {
    weather_fmt_temp(cf, sizeof(cf), cur->feels_like, cur->units);
    snprintf(feels, sizeof(feels), " (feels %s\xc2\xb0%s)", cf, tu);
  }

  // Line 1: icon + location + condition + temperature.
  snprintf(buf, sizeof(buf),
      "%s " CLR_BOLD "%s" CLR_RESET "%s "
      "\xe2\x80\x94 %s%s" CLR_RESET
      " " CLR_GRAY "\xc2\xb7" CLR_RESET " "
      "%s\xc2\xb0%s%s",
      icon, cur->place, zipseg,
      dclr, cur->cond,
      ct, tu, feels);

  cmd_reply(ctx, buf);

  // Line 2: whatever of hi/lo, humidity, wind and the sun is known.
  line.len = (size_t)snprintf(buf, sizeof(buf), "  ");

  if(cur->have_hilo)
  {
    char chi[32], clo[32];

    weather_fmt_temp(chi, sizeof(chi), cur->temp_hi, cur->units);
    weather_fmt_temp(clo, sizeof(clo), cur->temp_lo, cur->units);

    weather_line_add(&line, "Hi %s\xc2\xb0/Lo %s\xc2\xb0%s", chi, clo, tu);
  }

  if(cur->have_humidity)
    weather_line_add(&line, CLR_CYAN "Humidity" CLR_RESET " %d%%",
        cur->humidity);

  // A gust is reported only when there is one to report: a station
  // measuring steady wind leaves windGust null rather than repeating the
  // speed, and "(g0)" would be a reading nobody took.
  if(cur->have_wind && cur->have_gust)
    weather_line_add(&line, CLR_GREEN "Wind" CLR_RESET " %.0f%s %s (g%.0f)",
        cur->wind_speed, su, weather_wind_dir(cur->wind_deg), cur->wind_gust);

  else if(cur->have_wind)
    weather_line_add(&line, CLR_GREEN "Wind" CLR_RESET " %.0f%s %s",
        cur->wind_speed, su, weather_wind_dir(cur->wind_deg));

  weather_line_add(&line, CLR_YELLOW "Rise" CLR_RESET " %s "
      CLR_PURPLE "Set" CLR_RESET " %s", sunrise, sunset);

  cmd_reply(ctx, buf);

  // Line 3: the forecast office, in its own words.
  if(cur->have_detail)
  {
    weather_fmt_detail(buf, sizeof(buf), cur->detail_name, cur->detail);
    cmd_reply(ctx, buf);
  }

  weather_reply_alerts(ctx, alerts);
}

// The 7-day view, one row per day:
//
//   {icon} {day:9}  {hi}/{lo}°{u}  {condition:22}  {rh:3}  {pop:4}  {wind}
//
// 73 display columns, comfortably inside WEATHER_LINE_COLS. Every field
// is padded on its RAW text before colour markup wraps it — the reason
// weather_fmt_temp_w exists — so the columns line up whatever the
// temperature's digit count.
//
// Humidity and precipitation are two fixed slots rather than one field
// because the two providers are optional in opposite places: a
// weather.gov period carries a real precipitation probability and no
// relative humidity, a One Call 4.0 daily row the exact reverse. Each
// fills the slot the other cannot and both keep their width when empty,
// so the layout is identical whichever provider answered.
void
weather_reply_forecast_daily(const cmd_ctx_t *ctx,
    const weather_view_forecast_t *f,
    const weather_view_alert_set_t *alerts)
{
  const char *tu = weather_temp_unit(f->units);
  uint8_t i;

  weather_reply_header(ctx, f->place, f->zip, "7-day forecast");

  for(i = 0; i < f->count && i < WEATHER_FORECAST_DAYS; i++)
  {
    const weather_view_day_t *d = &f->days[i];
    const char *icon = weather_condition_icon(d->condition_id);
    const char *dclr = weather_condition_color(d->condition_id);
    char chi[32], clo[32];
    char desc_pad[24];
    char humid[8];
    char precip[24];
    char buf[WEATHER_REPLY_SZ];

    // Width-pad the numeric part to 3 visible chars (matching the hourly
    // view) so the hi/lo column stays fixed-width: a 3-digit temperature
    // (100+°F) would otherwise be wider than a 2-digit one and shove
    // every column to its right out of alignment. A half-row — the
    // leading "Tonight" of a forecast issued after dark — has no daytime
    // half at all, and says so rather than repeating its own low.
    if(d->have_hi)
      weather_fmt_temp_w(chi, sizeof(chi), d->temp_hi, f->units, 3);
    else
      snprintf(chi, sizeof(chi), " --");

    if(d->have_lo)
      weather_fmt_temp_w(clo, sizeof(clo), d->temp_lo, f->units, 3);
    else
      snprintf(clo, sizeof(clo), " --");

    weather_fmt_desc_pad(desc_pad, sizeof(desc_pad), d->cond, 22);

    if(d->have_humidity)
      snprintf(humid, sizeof(humid), "%2d%%", d->humidity);
    else
      snprintf(humid, sizeof(humid), "   ");

    weather_fmt_precip(precip, sizeof(precip), d->have_pop ? d->pop : 0);

    // Wind closes the line and is the one field left unpadded: nothing
    // sits to its right to fall out of alignment, and a padded "5mph"
    // only opens a gap before the direction.
    snprintf(buf, sizeof(buf),
        "%s %-*.*s  %s/%s\xc2\xb0%s"
        "  %s%s" CLR_RESET
        "  %s"
        "  %s"
        "  %s %s",
        icon, WEATHER_DAY_COLS, WEATHER_DAY_COLS, d->day_name, chi, clo, tu,
        dclr, desc_pad,
        humid,
        precip,
        d->wind, d->wind_dir);

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
void
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

  weather_fmt_precip(pop_str, sizeof(pop_str), pop);

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
void
weather_reply_forecast_hourly(const cmd_ctx_t *ctx,
    const openweather_forecast_t *f,
    const weather_view_alert_set_t *alerts)
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
