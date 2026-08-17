// botmanager — MIT
// weather.gov forecast: 14 day/night periods, as the local office wrote them.
#define WEATHERGOV_INTERNAL
#define WXG_FORECAST_TU
#include "weathergov.h"

#include <stdio.h>
#include <string.h>

// ----------------------------------------------------------------------
// The condition vocabulary
// ----------------------------------------------------------------------
//
// NWS states a period's sky as an icon URL and free-text prose; there is
// no numeric code. The URL's path token is the only machine-readable
// form, and its vocabulary is closed at exactly the 34 tokens
// api.weather.gov/icons enumerates — so this table is the whole language
// rather than a sample of it.
//
// The target axis is OpenWeather's numbering, because every icon and
// colour table in the tree already keys off it and there will only ever
// be one condition vocabulary here. The five states NWS names and
// OpenWeather has no code for take the private 900+ range.
static const wxg_icon_map_t wxg_icons[] = {
  { "tsra",            200 },  // thunderstorm
  { "tsra_sct",        200 },
  { "tsra_hi",         200 },
  { "rain",            500 },  // rain
  { "rain_showers",    500 },
  { "rain_showers_hi", 500 },
  { "fzra",            511 },  // freezing rain
  { "rain_fzra",       511 },
  { "snow_fzra",       511 },
  { "rain_sleet",      600 },  // wintry mix
  { "rain_snow",       600 },
  { "snow_sleet",      600 },
  { "sleet",           600 },
  { "snow",            601 },  // snow
  { "blizzard",        902 },  // private
  { "fog",             741 },  // obscuration
  { "haze",            741 },
  { "smoke",           741 },
  { "dust",            741 },
  { "skc",             800 },  // clear
  { "few",             801 },  // few clouds
  { "wind_few",        801 },
  { "sct",             802 },  // scattered
  { "wind_sct",        802 },
  { "bkn",             803 },  // broken
  { "wind_bkn",        803 },
  { "ovc",             804 },  // overcast
  { "wind_ovc",        804 },
  { "wind_skc",        804 },  // windy: no OpenWeather code, nearest is overcast
  { "tornado",         900 },  // private
  { "hurricane",       901 },  // private
  { "tropical_storm",  903 },  // private
  { "hot",             904 },  // private
  { "cold",            905 },  // private
};

// ".../icons/land/day/tsra_hi,40/tsra_hi,30?size=medium" -> 200.
//
// Two path segments after "/icons/" are the domain (land|marine) and the
// half-day (day|night); the token follows. A dual icon names one token
// per half-period and only the first is taken — it is what the period
// opens with, and shortForecast names the same thing. The ",NN" tail is
// a precipitation probability we already have as a real field.
//
// Returns 0 for anything unrecognised, which every consumer's condition
// table already renders as its own fallback.
int32_t
wxg_icon_condition_id(const char *icon_url)
{
  const char *p;
  const char *end;
  char        token[32];
  size_t      len;
  size_t      i;

  if(icon_url == NULL || icon_url[0] == '\0')
    return(0);

  p = strstr(icon_url, "/icons/");

  if(p == NULL)
    return(0);

  p += 7;

  // Skip land|marine and day|night. A URL that ends inside either is
  // malformed rather than merely unknown, and falls out here.
  for(i = 0; i < 2; i++)
  {
    p = strchr(p, '/');

    if(p == NULL)
      return(0);

    p++;
  }

  for(end = p; *end != '\0' && *end != '/' && *end != '?' && *end != ','; end++)
    ;

  len = (size_t)(end - p);

  if(len == 0 || len >= sizeof(token))
    return(0);

  memcpy(token, p, len);
  token[len] = '\0';

  for(i = 0; i < sizeof(wxg_icons) / sizeof(wxg_icons[0]); i++)
  {
    if(strcmp(wxg_icons[i].token, token) == 0)
      return(wxg_icons[i].condition_id);
  }

  clam(CLAM_DEBUG2, WXG_CTX, "unmapped icon token '%s'", token);

  return(0);
}

// ----------------------------------------------------------------------
// Wind
// ----------------------------------------------------------------------

// Wind arrives as a sentence — "7 to 12 mph", "5 mph", "19 km/h" — and
// it stays one. Parsing it to a number would throw away the range, which
// is the honest part of the forecast; all this does is close the gaps so
// the string fits a column: "7 to 12 mph" -> "7-12mph".
static void
wxg_wind_tidy(const char *raw, char *out, size_t sz)
{
  size_t i;
  size_t j = 0;

  out[0] = '\0';

  if(raw == NULL)
    return;

  for(i = 0; raw[i] != '\0' && j + 1 < sz; i++)
  {
    if(strncmp(raw + i, " to ", 4) == 0)
    {
      out[j++] = '-';
      i += 3;
      continue;
    }

    if(raw[i] == ' ')
      continue;

    out[j++] = raw[i];
  }

  out[j] = '\0';
}

// ----------------------------------------------------------------------
// One period
// ----------------------------------------------------------------------

static void
wxg_period_parse_one(struct json_object *p, weathergov_period_t *out)
{
  struct json_object *pop;
  char                buf[WXG_URL_SZ];

  memset(out, 0, sizeof(*out));

  out->pop = -1;

  json_get_str(p, "name",             out->name,     sizeof(out->name));
  json_get_str(p, "shortForecast",    out->cond,     sizeof(out->cond));
  json_get_str(p, "detailedForecast", out->detail,   sizeof(out->detail));
  json_get_str(p, "windDirection",    out->wind_dir, sizeof(out->wind_dir));
  json_get_str(p, "temperatureUnit",  out->temp_unit, sizeof(out->temp_unit));

  json_get_bool  (p, "isDaytime",   &out->is_daytime);
  json_get_double(p, "temperature", &out->temp);

  if(json_get_str(p, "startTime", buf, sizeof(buf)))
    out->start = wxg_parse_iso8601(buf, NULL);

  // probabilityOfPrecipitation is an object whose `value` is an integer
  // or JSON null; a null reads back as a missing key here, which is
  // exactly the -1 "no number was issued" case.
  pop = json_get_obj(p, "probabilityOfPrecipitation");

  if(pop != NULL)
    json_get_int(pop, "value", &out->pop);

  if(json_get_str(p, "windSpeed", buf, sizeof(buf)))
    wxg_wind_tidy(buf, out->wind, sizeof(out->wind));

  if(json_get_str(p, "icon", buf, sizeof(buf)))
    out->condition_id = wxg_icon_condition_id(buf);
}

// ----------------------------------------------------------------------
// The response
// ----------------------------------------------------------------------

// Shared with the current-conditions chain, whose last leg ends on this
// same endpoint: one gridpoint forecast, one reader of it.
bool
wxg_forecast_parse(const char *body, size_t len, weathergov_forecast_t *out)
{
  struct json_object *root;
  struct json_object *props;
  struct json_object *periods;
  int                 n;
  int                 i;

  memset(out, 0, sizeof(*out));

  root = json_parse_buf(body, len, WXG_CTX);

  if(root == NULL)
    return(FAIL);

  props   = json_get_obj(root, "properties");
  periods = (props != NULL) ? json_get_array(props, "periods") : NULL;

  if(periods == NULL)
  {
    json_object_put(root);
    return(FAIL);
  }

  n = (int)json_object_array_length(periods);

  for(i = 0; i < n && out->count < WEATHERGOV_PERIOD_MAX; i++)
  {
    struct json_object  *p = json_object_array_get_idx(periods, i);
    weathergov_period_t *period;

    if(p == NULL)
      continue;

    period = &out->periods[out->count];

    wxg_period_parse_one(p, period);

    // A period with no label is a period no consumer can render. It has
    // never been observed; dropping it keeps that true downstream.
    if(period->name[0] != '\0')
      out->count++;
  }

  json_object_put(root);

  clam(CLAM_DEBUG2, WXG_CTX, "forecast: %d period(s) -> %u kept", n,
      out->count);

  return(SUCCESS);
}

// ----------------------------------------------------------------------
// Transfer
// ----------------------------------------------------------------------

static void
wxg_forecast_deliver(wxg_request_t *r)
{
  wxg_caller_t caller;

  wxg_req_take_caller(r, &caller);

  if(caller.cb.forecast != NULL)
    caller.cb.forecast(&r->acc.forecast, caller.user);

  wxg_req_release(r);
}

static void
wxg_forecast_done(const curl_response_t *resp)
{
  wxg_request_t                *r   = (wxg_request_t *)resp->user_data;
  weathergov_forecast_result_t *res = &r->acc.forecast;

  res->covered = true;

  if(!wxg_http_status_ok(resp, res->err, sizeof(res->err), &res->covered))
  {
    // A grid we resolved a moment ago answering non-2xx is upstream
    // being upstream, not a coverage verdict — but it costs the caller
    // the same thing either way: fall through to the other provider.
    if(res->err[0] == '\0')
      clam(CLAM_DEBUG2, WXG_CTX, "forecast %s: HTTP %ld", r->grid,
          resp->status);

    wxg_forecast_deliver(r);
    return;
  }

  if(wxg_forecast_parse(resp->body, resp->body_len, &res->forecast) != SUCCESS)
    snprintf(res->err, sizeof(res->err),
        "weather.gov returned no readable forecast.");

  wxg_forecast_deliver(r);
}

// ----------------------------------------------------------------------
// Public mechanism API
// ----------------------------------------------------------------------

async_rc_t
weathergov_forecast_async(const weathergov_point_t *pt, const char *units,
    weathergov_forecast_cb_t cb, void *user)
{
  char url[WXG_URL_SZ];
  wxg_request_t *r;

  if(cb == NULL || pt == NULL || !weathergov_enabled())
    return(ASYNC_FAILED_UNDELIVERED);

  // "us" and "si" are the only values the endpoint accepts; anything
  // else is answered 400, so an unrecognised one becomes the default
  // rather than a wasted round trip.
  if(units == NULL || strcmp(units, "si") != 0)
    units = "us";

  r = wxg_req_alloc();
  r->type        = WXG_REQ_FORECAST;
  r->cb.forecast = cb;
  r->user        = user;

  snprintf(r->grid, sizeof(r->grid), "%s/%d,%d", pt->grid_id, pt->grid_x,
      pt->grid_y);
  wxg_ua(r->ua, sizeof(r->ua));

  // File it before anything can be submitted, never after: a completion
  // can run on a curl worker before the submitting call has returned.
  if(wxg_req_track(r) != SUCCESS)
  {
    wxg_req_release(r);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  snprintf(url, sizeof(url), "%s/%s/forecast?units=%s", WXG_GRID_URL,
      r->grid, units);

  if(wxg_http_get(url, r, wxg_forecast_done) != SUCCESS)
  {
    wxg_req_release(r);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  return(ASYNC_AIRBORNE);
}
