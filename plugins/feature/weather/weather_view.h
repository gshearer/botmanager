#ifndef BM_WEATHER_VIEW_H
#define BM_WEATHER_VIEW_H

// The neutral view — what a renderer is allowed to see.
//
// Two providers answer !weather: openweather everywhere, weather.gov
// inside the United States. Nothing below this line knows which one
// spoke. An adapter (weather_adapt.c) turns a provider's structs into
// these; a renderer (weather_render.c) turns these into lines. Adding a
// third provider is one adapter and zero renderer edits.

#ifdef WEATHER_INTERNAL

#include "cmd.h"
#include "common.h"

#include "openweather_api.h"
#include "weathergov_api.h"

#include <stdint.h>
#include <time.h>

// The visible-width budget for one reply line. The same ~100-column
// target the two-column hourly view was laid out against; a segmented
// line (the alert grammar) measures itself against this and sheds
// detail from the right until it fits.
#define WEATHER_LINE_COLS   100

// At most this many alerts are printed however many are active — the
// rest become one overflow line. Matched to the providers' own set
// size, so everything a fetch carried gets a line and the overflow
// counts only what the provider dropped.
#define WEATHER_ALERT_LINES 8

// One active alert, already reduced to what a line needs.
//
// `area` is print-ready ("Fairfield, Hocking, Pickaway OH", or
// "24 counties" when there are too many to name). `state` and
// `area_count` are carried alongside it for one reason: when the line
// runs over budget the renderer rebuilds the area segment in its short
// form, and it cannot do that from prose.
typedef struct
{
  char    event [96];
  char    area  [96];
  char    state [WEATHERGOV_STATE_SZ];
  char    impact[128];
  uint8_t severity;    // 0 unknown … 4 extreme; mirrors weathergov_severity_t
  uint8_t area_count;
  time_t  until;       // 0 = unknown
  int32_t tz_offset;
} weather_view_alert_t;

// `total` is every distinct active alert; `count` is how many are
// carried below. The renderer prints at most WEATHER_ALERT_LINES of
// them and reports the remainder against `total`.
typedef struct
{
  uint8_t              count;
  uint8_t              total;
  weather_view_alert_t alerts[8];
} weather_view_alert_set_t;

// The daily forecast is capped at a week however many periods or days a
// provider offers: seven lines is already the most a channel will read.
#define WEATHER_FORECAST_DAYS 7

// The day-label column. Wide enough for "Wednesday", which is the widest
// weekday there is, and the width an adapter measures a provider's own
// period label against before deciding it will not fit.
#define WEATHER_DAY_COLS      9

// The wind column, which the daily view pads so the icon that CLOSES
// each row lands in a straight column. Wide enough for "10-15mph NNW";
// a wider one pushes only its own icon, since nothing is aligned
// against that.
#define WEATHER_WIND_COLS    12

// One row of the daily forecast.
//
// Every optional field is paired with a `have_` flag rather than a
// sentinel, because the two providers are optional in DIFFERENT places
// and a blank slot has to look identical whichever one answered:
// weather.gov's periods carry no humidity and One Call 4.0's daily rows
// carry no precipitation probability at all. The layout is fixed; only
// what fills it moves.
//
// `wind` is a STRING and stays one. weather.gov issues a range ("7 to 12
// mph") and the range is the honest part of the forecast; parsing it to
// a double to re-print it would throw that away for nothing.
typedef struct
{
  char    day_name[16];   // "Today", "Wednesday Night" — renderer truncates
  bool    have_hi;        // false on a leading night row: no daytime half
  double  temp_hi;
  bool    have_lo;        // false on a trailing day row: no night half yet
  double  temp_lo;
  int32_t condition_id;
  char    cond[24];
  bool    have_humidity;
  int32_t humidity;       // %
  bool    have_pop;
  int32_t pop;            // %
  char    wind[12];       // "7-12mph"
  char    wind_dir[4];    // "SSW"
} weather_view_day_t;

// `units` is the openweather KV value ("imperial" | "metric" |
// "standard") and nothing else in the tree: it is what picks the unit
// letter and drives the temperature colour ramp, and both live in the
// renderer.
typedef struct
{
  char               place[OPENWEATHER_NAME_SZ];
  char               zip  [OPENWEATHER_ZIPCODE_SZ];
  char               units[OPENWEATHER_UNITS_SZ];
  uint8_t            count;
  weather_view_day_t days[WEATHER_FORECAST_DAYS];
} weather_view_forecast_t;

// Current conditions.
//
// The two providers differ in KIND here, not merely in quality: One Call
// states a model's opinion of right now, weather.gov relays what an
// instrument at a named airport measured minutes ago. What survives into
// this struct is what both can be asked for, plus the three things only
// an observation network has — a gust, the day's high and low from the
// forecast office, and that office's own prose.
//
// Every optional field carries a presence flag because a station that
// reports temperature may report nothing else, and because the openweather
// path leaves the last three empty by construction. A zero is never a
// measurement.
typedef struct
{
  char    place[OPENWEATHER_NAME_SZ];
  char    zip  [OPENWEATHER_ZIPCODE_SZ];
  char    units[OPENWEATHER_UNITS_SZ];   // the openweather KV value

  int32_t condition_id;
  char    cond[OPENWEATHER_DESC_SZ];     // "Partly Cloudy", "broken clouds"

  double  temp;
  bool    have_feels;
  double  feels_like;

  bool    have_hilo;                     // today's extremes, when known
  double  temp_hi;
  double  temp_lo;

  bool    have_humidity;
  int32_t humidity;                      // %
  bool    have_wind;
  double  wind_speed;
  double  wind_deg;
  bool    have_gust;
  double  wind_gust;

  time_t  sunrise;                       // 0 = unknown
  time_t  sunset;
  int32_t tz_offset;

  // The local Weather Forecast Office on the rest of today — the one
  // thing in this whole feature no other API can produce.
  bool    have_detail;
  char    detail_name[WEATHERGOV_PERIOD_NAME_SZ];  // "Today", "Tonight"
  char    detail     [WEATHERGOV_DETAIL_SZ];
} weather_view_current_t;

// Adapters — weather_adapt.c. One per (provider, view), never one per
// field.
void weather_view_from_wxg_alerts(const weathergov_alert_set_t *src,
         weather_view_alert_set_t *out);
void weather_view_from_ow_alerts(const openweather_alert_set_t *src,
         weather_view_alert_set_t *out);

// weather.gov's forecast names no place and no postcode — the endpoint
// is addressed by grid — so the caller, which resolved the location in
// the first place, supplies both. `units` is the openweather KV value;
// the adapter converts the temperatures upstream returned in Celsius
// when it says "standard".
void weather_view_from_wxg_forecast(const weathergov_forecast_t *src,
         const char *place, const char *zip, const char *units,
         weather_view_forecast_t *out);
void weather_view_from_ow_forecast(const openweather_forecast_t *src,
         weather_view_forecast_t *out);

// The observation names its station and nothing else: the place label,
// the postcode and the sun times come from the caller and its cached
// point, exactly as they do for the forecast view. `fc` may be empty —
// it is what carries the high, the low and the prose, and its absence
// costs those three and no more.
void weather_view_from_wxg_current(const weathergov_obs_t *obs,
         const weathergov_forecast_t *fc, const weathergov_point_t *pt,
         const char *place, const char *zip, const char *units,
         weather_view_current_t *out);
void weather_view_from_ow_current(const openweather_current_t *src,
         weather_view_current_t *out);

// Presentation — weather_render.c. Everything below this line formats;
// nothing below it fetches, routes or decides.
const char *weather_temp_unit(const char *units);
const char *weather_speed_unit(const char *units);
const char *weather_wind_dir(double deg);
const char *weather_day_name_full(int wday);
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
void        weather_fmt_detail(char *buf, size_t sz, const char *name,
                const char *detail);
void        weather_fmt_desc_pad(char *buf, size_t sz, const char *desc,
                int width);
void        weather_reply_header(const cmd_ctx_t *ctx, const char *place,
                const char *zip, const char *subtitle);
void        weather_reply_alerts(const cmd_ctx_t *ctx,
                const weather_view_alert_set_t *a);
void        weather_reply_current(const cmd_ctx_t *ctx,
                const weather_view_current_t *cur,
                const weather_view_alert_set_t *alerts);
void        weather_reply_forecast_daily(const cmd_ctx_t *ctx,
                const weather_view_forecast_t *f,
                const weather_view_alert_set_t *alerts);
void        weather_hour_cell(char *buf, size_t sz,
                const openweather_forecast_hour_t *h, const char *units,
                const char *tu, int tz_offset);
void        weather_reply_forecast_hourly(const cmd_ctx_t *ctx,
                const openweather_forecast_t *f,
                const weather_view_alert_set_t *alerts);

// Adapter-private — weather_adapt.c alone defines WEATHER_ADAPT_TU.
#ifdef WEATHER_ADAPT_TU

static void weather_area_bare(const char *part, size_t len, char *out,
                size_t sz);
static void weather_area_compose(const weathergov_alert_t *a, char *out,
                size_t sz);
static void weather_cond_abbrev(const char *src, char *out, size_t sz);
static void weather_day_from_wxg(const weathergov_period_t *day,
                const weathergov_period_t *night, double kelvin,
                weather_view_day_t *out);

#endif // WEATHER_ADAPT_TU

// Renderer-private — weather_render.c alone defines WEATHER_RENDER_TU.
#ifdef WEATHER_RENDER_TU

// A line built out of optional segments. Each one brings its own
// separator, so a field that is absent takes its punctuation with it and
// there is never a dangling " · " to trim afterwards.
typedef struct
{
  char   *buf;
  size_t  sz;
  size_t  len;
  bool    any;
} weather_line_t;

static void weather_line_add(weather_line_t *l, const char *fmt, ...)
                __attribute__((format(printf, 2, 3)));

// Columns a line occupies on screen: colour markup is skipped, UTF-8
// continuation bytes count nothing, and a glyph followed by the
// variation selector U+FE0F counts two.
static int  weather_visible_cols(const char *s);

static const char *weather_alert_color(uint8_t severity);
static void weather_alert_until_str(const weather_view_alert_t *a,
                char *buf, size_t sz);
static bool weather_drop_last_item(char *csv);
static void weather_alert_compose(char *buf, size_t sz,
                const weather_view_alert_t *a, const char *until,
                const char *impact, bool short_area);
static void weather_alert_line(char *buf, size_t sz,
                const weather_view_alert_t *a);

#endif // WEATHER_RENDER_TU

#endif // WEATHER_INTERNAL

#endif // BM_WEATHER_VIEW_H
