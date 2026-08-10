// botmanager — MIT
// Provider structs in, neutral view out. The only file that knows both.
#define WEATHER_INTERNAL
#define WEATHER_ADAPT_TU
#include "weather.h"

#include <stdio.h>
#include <string.h>

// One area entry, stripped of its state suffix: "Fairfield, OH" is the
// county Fairfield, and printing the state on every one of three
// entries says "OH" three times.
static void
weather_area_bare(const char *part, size_t len, char *out, size_t sz)
{
  size_t i;

  for(i = 0; i + 1 < len; i++)
  {
    if(part[i] == ',' && part[i + 1] == ' ')
    {
      len = i;
      break;
    }
  }

  if(len >= sz)
    len = sz - 1;

  memcpy(out, part, len);
  out[len] = '\0';
}

// The area segment, print-ready.
//
// Up to three areas are named outright, with the state stated once at
// the end when every one of them shares it. Beyond three the names stop
// being information and become a wall, so they collapse to a count.
static void
weather_area_compose(const weathergov_alert_t *a, char *out, size_t sz)
{
  const char *p   = a->area;
  size_t      len = 0;

  out[0] = '\0';

  if(a->area_count == 0 || p[0] == '\0')
    return;

  if(a->area_count > 3)
  {
    snprintf(out, sz, "%u counties%s%s", (unsigned)a->area_count,
        a->state[0] != '\0' ? " " : "", a->state);
    return;
  }

  while(*p != '\0' && len + 1 < sz)
  {
    const char *end  = strstr(p, "; ");
    size_t      plen = (end != NULL) ? (size_t)(end - p) : strlen(p);
    char        bare[64];

    weather_area_bare(p, plen, bare, sizeof(bare));

    len += (size_t)snprintf(out + len, sz - len, "%s%s",
        len > 0 ? ", " : "", bare);

    if(end == NULL || len + 1 >= sz)
      break;

    p = end + 2;
  }

  if(a->state[0] != '\0' && len + 1 < sz)
    snprintf(out + len, sz - len, " %s", a->state);
}

void
weather_view_from_wxg_alerts(const weathergov_alert_set_t *src,
    weather_view_alert_set_t *out)
{
  uint8_t i;

  memset(out, 0, sizeof(*out));

  out->total = src->total;

  for(i = 0; i < src->count
      && i < (uint8_t)(sizeof(out->alerts) / sizeof(out->alerts[0])); i++)
  {
    const weathergov_alert_t *a = &src->alerts[i];
    weather_view_alert_t     *v = &out->alerts[i];

    snprintf(v->event,  sizeof(v->event),  "%s", a->event);
    snprintf(v->impact, sizeof(v->impact), "%s", a->impact);
    snprintf(v->state,  sizeof(v->state),  "%s", a->state);

    weather_area_compose(a, v->area, sizeof(v->area));

    v->severity   = (uint8_t)a->severity;
    v->area_count = a->area_count;
    v->tz_offset  = a->tz_offset;

    // `ends` is null on every Special Weather Statement; `expires` is
    // the fallback CAP always carries.
    v->until = (a->ends != 0) ? a->ends : a->expires;

    out->count++;
  }
}

// OpenWeather carries the product name and nothing else — no area, no
// end time, no impact — so most of the view stays empty and the line
// grammar omits each missing segment along with its separator.
//
// Severity is pinned at Severe rather than Unknown on purpose: One Call
// only ever surfaces real warnings, and Unknown would demote every
// non-US alert to a grey ⚑ when today it is a red ⚠.
void
weather_view_from_ow_alerts(const openweather_alert_set_t *src,
    weather_view_alert_set_t *out)
{
  uint8_t i;

  memset(out, 0, sizeof(*out));

  for(i = 0; i < src->count
      && i < (uint8_t)(sizeof(out->alerts) / sizeof(out->alerts[0])); i++)
  {
    if(src->alerts[i].event[0] == '\0')
      continue;

    snprintf(out->alerts[out->count].event,
        sizeof(out->alerts[out->count].event), "%s", src->alerts[i].event);

    out->alerts[out->count].severity = 3;   // Severe
    out->count++;
  }

  out->total = out->count;
}

// ----------------------------------------------------------------------
// The daily forecast
// ----------------------------------------------------------------------

// shortForecast is prose written for a web page — "Slight Chance Showers
// And Thunderstorms then Chance Showers And Thunderstorms" is 76
// characters and a 22-column truncation of it says "Slight Chance Showe".
//
// Two rules recover a column's worth of meaning. The clause after "then"
// goes: the icon already carries the dual condition, and what a period
// IS matters more than what it becomes. Then the phrases a forecaster
// repeats are abbreviated the way a forecaster abbreviates them.
static void
weather_cond_abbrev(const char *src, char *out, size_t sz)
{
  static const struct
  {
    const char *find;
    const char *repl;
  } table[] = {
    { "Showers And Thunderstorms", "Showers/T-storms" },
    { "Thunderstorms",             "T-storms"         },
    { "Slight Chance ",            "Sl "              },
    { "Chance ",                   "Chc "             },
    { " Likely",                   " Lkly"            },
    { "Isolated ",                 "Iso "             },
    { "Scattered ",                "Sct "             },
    { "Areas Of ",                 ""                 },
    { "Patchy ",                   ""                 },
    { "Precipitation",             "Precip"           },
  };
  // Abbreviated at full length and cut afterwards — the other order
  // would truncate "Slight Chance Showers And Thunderstorms" to
  // "Slight Chance Showers A" and then abbreviate the wreckage.
  char        work[WEATHERGOV_COND_SZ];
  char       *then;
  size_t      t;
  size_t      len;

  snprintf(work, sizeof(work), "%s", src);

  then = strstr(work, " then ");

  if(then != NULL)
    *then = '\0';

  // Longest match first, which the table is already ordered for: every
  // pass rewrites the whole string, so a later shorter entry can only
  // reach what an earlier longer one did not claim.
  for(t = 0; t < sizeof(table) / sizeof(table[0]); t++)
  {
    char       *at;
    size_t      flen = strlen(table[t].find);
    size_t      rlen = strlen(table[t].repl);

    while((at = strstr(work, table[t].find)) != NULL)
    {
      len = strlen(at + flen);

      // Every replacement in the table is shorter than what it replaces,
      // so this can only ever shrink the string — the guard is here
      // because a future entry that grows one must not be able to run
      // off the end quietly.
      if((size_t)(at - work) + rlen + len + 1 > sizeof(work))
        break;

      memmove(at + rlen, at + flen, len + 1);
      memcpy(at, table[t].repl, rlen);
    }
  }

  snprintf(out, sz, "%.*s", (int)(sz - 1), work);
}

// One row, from a daytime period and the night that follows it.
//
// `night` is NULL for the last row of an odd walk; `day` is NULL for the
// leading-night row a command issued after dark opens with, which has no
// daytime half and renders its high slot empty. Exactly one of them may
// be absent.
//
// `kelvin` is 273.15 when the operator asked for absolute temperatures
// and 0 otherwise — weather.gov has no Kelvin option, so the conversion
// is ours (root TODO §WX-DESIGN §5).
static void
weather_day_from_wxg(const weathergov_period_t *day,
    const weathergov_period_t *night, double kelvin, weather_view_day_t *out)
{
  // The daytime half is the one people read: its condition, its wind and
  // its precipitation odds are the row's. A leading night row has only
  // its own.
  const weathergov_period_t *face = (day != NULL) ? day : night;

  memset(out, 0, sizeof(*out));

  // Upstream labels its own periods and the labels are better than a
  // weekday: "Today", "Tonight", "Overnight". The exception is the
  // partial period a forecast issued mid-day or late opens with — "This
  // Afternoon" is fourteen characters into a nine-column slot and
  // truncates to "This Afte". Anything that will not fit says which half
  // of today it is instead, which is all the long form was saying.
  if(strlen(face->name) > WEATHER_DAY_COLS)
    snprintf(out->day_name, sizeof(out->day_name), "%s",
        face->is_daytime ? "Today" : "Tonight");
  else
    snprintf(out->day_name, sizeof(out->day_name), "%.*s", WEATHER_DAY_COLS,
        face->name);

  // Explicit precision: the view's fields are deliberately narrower than
  // the service's, and a cut that is stated is a cut the compiler stops
  // warning about.
  snprintf(out->wind, sizeof(out->wind), "%.*s",
      (int)(sizeof(out->wind) - 1), face->wind);
  snprintf(out->wind_dir, sizeof(out->wind_dir), "%.*s",
      (int)(sizeof(out->wind_dir) - 1), face->wind_dir);

  weather_cond_abbrev(face->cond, out->cond, sizeof(out->cond));

  out->condition_id = face->condition_id;
  out->have_pop     = (face->pop >= 0);
  out->pop          = face->pop;

  // No relative humidity in a forecast period at all — the slot stays
  // blank and the precipitation slot beside it carries the row.
  out->have_humidity = false;

  // A period states one temperature and means the extreme of its own
  // half: the day's is the high, the night's is the low. A row missing
  // half of itself is missing that number outright — the other half
  // standing in for it would read as a measurement rather than a gap.
  if(day != NULL)
  {
    out->have_hi = true;
    out->temp_hi = day->temp + kelvin;
  }

  if(night != NULL)
  {
    out->have_lo = true;
    out->temp_lo = night->temp + kelvin;
  }
}

void
weather_view_from_wxg_forecast(const weathergov_forecast_t *src,
    const char *place, const char *zip, const char *units,
    weather_view_forecast_t *out)
{
  double  kelvin = (strcmp(units, "standard") == 0) ? 273.15 : 0.0;
  uint8_t i      = 0;

  memset(out, 0, sizeof(*out));

  snprintf(out->place, sizeof(out->place), "%s", place);
  snprintf(out->zip,   sizeof(out->zip),   "%s", zip);
  snprintf(out->units, sizeof(out->units), "%s", units);

  // The pairing walk. weather.gov issues 14 alternating day/night
  // periods, not 7 days: a day opens a row and the night that follows it
  // closes it. Issued after about six in the evening the sequence opens
  // on "Tonight" instead, which is a row of its own with no high.
  while(i < src->count && out->count < WEATHER_FORECAST_DAYS)
  {
    const weathergov_period_t *day   = NULL;
    const weathergov_period_t *night = NULL;

    if(src->periods[i].is_daytime)
    {
      day = &src->periods[i++];

      if(i < src->count && !src->periods[i].is_daytime)
        night = &src->periods[i++];
    }

    else
      night = &src->periods[i++];

    weather_day_from_wxg(day, night, kelvin, &out->days[out->count]);
    out->count++;
  }
}

// ----------------------------------------------------------------------
// Current conditions
// ----------------------------------------------------------------------

void
weather_view_from_wxg_current(const weathergov_obs_t *obs,
    const weathergov_forecast_t *fc, const weathergov_point_t *pt,
    const char *place, const char *zip, const char *units,
    weather_view_current_t *out)
{
  double kelvin = (strcmp(units, "standard") == 0) ? 273.15 : 0.0;

  memset(out, 0, sizeof(*out));

  snprintf(out->place, sizeof(out->place), "%s", place);
  snprintf(out->zip,   sizeof(out->zip),   "%s", zip);
  snprintf(out->units, sizeof(out->units), "%s", units);

  snprintf(out->cond, sizeof(out->cond), "%s", obs->text);
  out->condition_id = obs->condition_id;

  out->temp          = obs->temp + kelvin;
  out->have_feels    = obs->have_feels;
  out->feels_like    = obs->feels_like + kelvin;
  out->have_humidity = obs->have_humidity;
  out->humidity      = obs->humidity;
  out->have_wind     = obs->have_wind;
  out->wind_speed    = obs->wind_speed;
  out->wind_deg      = obs->wind_dir_deg;
  out->have_gust     = obs->have_gust;
  out->wind_gust     = obs->wind_gust;

  // Sunrise and sunset came with the grid lookup and cost nothing here —
  // the same cached /points response the forecast URL was built from.
  out->sunrise   = pt->sunrise;
  out->sunset    = pt->sunset;
  out->tz_offset = pt->tz_offset;

  if(fc->count == 0)
    return;

  // ⚠ A reporting station may still say nothing about the sky: PHTO in
  // Hilo sends a temperature with an empty textDescription and no icon
  // at all. The forecast office's own word for the same hour is already
  // in hand, so the sky falls back to it rather than to a blank line and
  // the thermometer glyph.
  if(out->cond[0] == '\0')
  {
    snprintf(out->cond, sizeof(out->cond), "%s", fc->periods[0].cond);
    out->condition_id = fc->periods[0].condition_id;
  }

  // Today's extremes are the first two periods — a high issued for the
  // daylight half and the low for the night that follows it. Issued after
  // dark the sequence opens on the night instead, and today's high is
  // simply over: printing tomorrow's in its place would read as a
  // measurement of a day that has not happened.
  if(fc->periods[0].is_daytime && fc->count >= 2)
  {
    out->have_hilo = true;
    out->temp_hi   = fc->periods[0].temp + kelvin;
    out->temp_lo   = fc->periods[1].temp + kelvin;
  }

  if(fc->periods[0].detail[0] == '\0')
    return;

  out->have_detail = true;
  snprintf(out->detail_name, sizeof(out->detail_name), "%s",
      fc->periods[0].name);
  snprintf(out->detail, sizeof(out->detail), "%s", fc->periods[0].detail);
}

// One Call states a feels-like, a humidity and a wind for every location
// on earth and never a gust, a station or a paragraph of prose — so
// three presence flags stay false here and the line grammar renders
// exactly the two lines this view has always had.
void
weather_view_from_ow_current(const openweather_current_t *src,
    weather_view_current_t *out)
{
  memset(out, 0, sizeof(*out));

  snprintf(out->place, sizeof(out->place), "%s", src->place_name);
  snprintf(out->zip,   sizeof(out->zip),   "%s", src->zipcode);
  snprintf(out->units, sizeof(out->units), "%s", src->units);
  snprintf(out->cond,  sizeof(out->cond),  "%s", src->condition_desc);

  out->condition_id  = src->condition_id;
  out->temp          = src->temp;
  out->have_feels    = true;
  out->feels_like    = src->feels_like;
  out->have_hilo     = src->have_hilo;
  out->temp_hi       = src->temp_hi;
  out->temp_lo       = src->temp_lo;
  out->have_humidity = true;
  out->humidity      = src->humidity;
  out->have_wind     = true;
  out->wind_speed    = src->wind_speed;
  out->wind_deg      = src->wind_deg;
  out->sunrise       = src->sunrise;
  out->sunset        = src->sunset;
  out->tz_offset     = src->tz_offset;
}

// One Call 4.0's daily rows are the mirror image of a weather.gov
// period: a real relative humidity, and no precipitation probability at
// all — 4.0 dropped `pop` from daily, which is why the column beside it
// exists. The condition text arrives synthesized from cloud cover rather
// than written, and is passed through as-is: it is already terse.
void
weather_view_from_ow_forecast(const openweather_forecast_t *src,
    weather_view_forecast_t *out)
{
  uint8_t i;

  memset(out, 0, sizeof(*out));

  snprintf(out->place, sizeof(out->place), "%s", src->place_name);
  snprintf(out->zip,   sizeof(out->zip),   "%s", src->zipcode);
  snprintf(out->units, sizeof(out->units), "%s", src->units);

  for(i = 0; i < src->day_count && out->count < WEATHER_FORECAST_DAYS; i++)
  {
    const openweather_forecast_day_t *d = &src->days[i];
    weather_view_day_t               *v = &out->days[out->count];

    memset(v, 0, sizeof(*v));

    // A daily row's `dt` is a calendar-day marker, not an instant: One
    // Call pins it to exactly 00:00:00 UTC of the day it represents,
    // identical in every timezone. It is therefore read in UTC as-is —
    // adding tz_offset would push negative-offset locales (the Americas)
    // back over midnight and mislabel the weekday.
    if(d->dt > 0)
    {
      struct tm tm;

      gmtime_r(&d->dt, &tm);
      snprintf(v->day_name, sizeof(v->day_name), "%s",
          weather_day_name_full(tm.tm_wday));
    }

    else
      snprintf(v->day_name, sizeof(v->day_name), "???");

    snprintf(v->cond, sizeof(v->cond), "%s", d->condition_desc);
    snprintf(v->wind, sizeof(v->wind), "%.0f%s", d->wind_speed,
        weather_speed_unit(src->units));
    snprintf(v->wind_dir, sizeof(v->wind_dir), "%s",
        weather_wind_dir(d->wind_deg));

    // A One Call daily row is a whole day and always states both ends of
    // it; only weather.gov's half-periods can be missing one.
    v->have_hi       = true;
    v->temp_hi       = d->temp_hi;
    v->have_lo       = true;
    v->temp_lo       = d->temp_lo;
    v->condition_id  = d->condition_id;
    v->have_humidity = true;
    v->humidity      = d->humidity;
    v->have_pop      = false;

    out->count++;
  }
}
