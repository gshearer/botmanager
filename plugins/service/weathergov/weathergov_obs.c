// botmanager — MIT
// weather.gov current conditions: what a METAR station actually measured.
#define WEATHERGOV_INTERNAL
#define WXG_OBS_TU
#include "weathergov.h"

#include <stdio.h>
#include <string.h>

// ----------------------------------------------------------------------
// The station cache
// ----------------------------------------------------------------------
//
// The station list for one grid is 75 KiB of GeoJSON describing 53
// airports, of which we want the nearest — which is the one upstream
// puts first. Paying that on every !weather would be absurd, so a grid's
// answer is remembered for as long as the grid itself is: the same
// plugin.weathergov.point_cache_ttl, because a station list changes
// about as often as a forecast grid does.
//
// Flat list, no hash. The point cache next door takes a lookup per
// command and earns its buckets; this one is consulted once per US
// current-conditions request against a handful of distinct grids.

static wxg_stationcache_t *wxg_station_cache = NULL;
static uint32_t            wxg_station_count = 0;
static pthread_mutex_t     wxg_station_mu    = PTHREAD_MUTEX_INITIALIZER;

static bool
wxg_station_lookup(const char *grid, char *out, size_t sz)
{
  wxg_stationcache_t **pp;
  uint32_t             ttl;
  bool                 hit = false;

  ttl = (uint32_t)kv_get_uint("plugin.weathergov.point_cache_ttl");

  if(ttl == 0)
    return(false);

  pthread_mutex_lock(&wxg_station_mu);

  for(pp = &wxg_station_cache; *pp != NULL; pp = &(*pp)->next)
  {
    wxg_stationcache_t *e = *pp;

    if(strcmp(e->grid, grid) != 0)
      continue;

    if((time(NULL) - e->cached_at) < (time_t)ttl)
    {
      snprintf(out, sz, "%s", e->station);
      hit = true;
      break;
    }

    *pp = e->next;
    wxg_station_count--;
    mem_free(e);
    break;
  }

  pthread_mutex_unlock(&wxg_station_mu);

  return(hit);
}

static void
wxg_station_insert(const char *grid, const char *station)
{
  wxg_stationcache_t *e;

  pthread_mutex_lock(&wxg_station_mu);

  for(e = wxg_station_cache; e != NULL; e = e->next)
  {
    if(strcmp(e->grid, grid) != 0)
      continue;

    snprintf(e->station, sizeof(e->station), "%s", station);
    e->cached_at = time(NULL);

    pthread_mutex_unlock(&wxg_station_mu);
    return;
  }

  // The bound is a runaway guard, not a policy: a daemon that has seen
  // this many distinct grids is one whose oldest entries have long since
  // expired anyway, and dropping the newest insert costs one extra GET.
  if(wxg_station_count >= WXG_STATION_CACHE_MAX)
  {
    pthread_mutex_unlock(&wxg_station_mu);
    return;
  }

  e = mem_alloc(WXG_CTX, "stationcache", sizeof(*e));

  memset(e, 0, sizeof(*e));
  snprintf(e->grid,    sizeof(e->grid),    "%s", grid);
  snprintf(e->station, sizeof(e->station), "%s", station);
  e->cached_at = time(NULL);

  e->next = wxg_station_cache;
  wxg_station_cache = e;
  wxg_station_count++;

  pthread_mutex_unlock(&wxg_station_mu);
}

void
wxg_station_cache_clear(void)
{
  pthread_mutex_lock(&wxg_station_mu);

  while(wxg_station_cache != NULL)
  {
    wxg_stationcache_t *next = wxg_station_cache->next;

    mem_free(wxg_station_cache);
    wxg_station_cache = next;
  }

  wxg_station_count = 0;

  pthread_mutex_unlock(&wxg_station_mu);
}

// ----------------------------------------------------------------------
// One observation
// ----------------------------------------------------------------------

// Every measured quantity is an object — { unitCode, value, qualityControl }
// — and every one of them may carry a null value. A null reads back here
// as an absent key, which is the same thing to a renderer and is the one
// reading that must never become a zero.
static bool
wxg_obs_value(struct json_object *props, const char *key, double *out,
    char *unit, size_t unit_sz)
{
  struct json_object *obj = json_get_obj(props, key);

  if(unit != NULL && unit_sz > 0)
    unit[0] = '\0';

  if(obj == NULL)
    return(false);

  if(unit != NULL)
    json_get_str(obj, "unitCode", unit, unit_sz);

  return(json_get_double(obj, "value", out));
}

static double
wxg_to_fahrenheit(double c)
{
  return(c * 9.0 / 5.0 + 32.0);
}

// Observations are issued in km/h whatever `units` the rest of the API
// was asked for. The tree's metric speed unit is m/s (weather_speed_unit
// in the weather feature says so for every non-imperial system), so si
// converts too — a value labelled m/s that is really km/h is worse than
// no wind at all.
static double
wxg_speed_convert(double kmh, bool imperial)
{
  return(imperial ? kmh * 0.621371 : kmh / 3.6);
}

// The measured temperature, in the system the caller asked for. The unit
// code is honoured rather than assumed: every probe has said degC, and
// the day one says degF is the day an assumption would silently read 20°
// as 20°F.
static double
wxg_temp_value(double v, const char *unit, bool imperial)
{
  bool celsius = (strstr(unit, "degC") != NULL);

  if(imperial && celsius)
    return(wxg_to_fahrenheit(v));

  if(!imperial && !celsius && unit[0] != '\0')
    return((v - 32.0) * 5.0 / 9.0);

  return(v);
}

static void
wxg_obs_parse(struct json_object *props, bool imperial, weathergov_obs_t *out)
{
  char   unit[32];
  char   buf[WXG_URL_SZ];
  double v;

  memset(out, 0, sizeof(*out));

  json_get_str(props, "textDescription", out->text, sizeof(out->text));

  if(json_get_str(props, "icon", buf, sizeof(buf)))
    out->condition_id = wxg_icon_condition_id(buf);

  if(json_get_str(props, "timestamp", buf, sizeof(buf)))
    out->observed = wxg_parse_iso8601(buf, NULL);

  // Temperature is the one field that decides whether this station is
  // reporting at all: without it there is no observation, only an
  // envelope, and the caller is better served by its other provider.
  if(wxg_obs_value(props, "temperature", &v, unit, sizeof(unit)))
  {
    out->have_temp = true;
    out->temp      = wxg_temp_value(v, unit, imperial);
  }

  // What it feels like is whichever extreme applies today: a heat index
  // in August, a wind chill in January, and null for both in between.
  if(wxg_obs_value(props, "heatIndex", &v, unit, sizeof(unit))
      || wxg_obs_value(props, "windChill", &v, unit, sizeof(unit)))
  {
    out->have_feels = true;
    out->feels_like = wxg_temp_value(v, unit, imperial);
  }

  if(wxg_obs_value(props, "relativeHumidity", &v, NULL, 0))
  {
    out->have_humidity = true;
    out->humidity      = (int32_t)(v + 0.5);
  }

  if(wxg_obs_value(props, "windSpeed", &v, unit, sizeof(unit)))
  {
    out->have_wind   = true;
    out->wind_speed  = wxg_speed_convert(v, imperial);
  }

  // Direction stays in degrees: the consumer already owns a compass.
  if(wxg_obs_value(props, "windDirection", &v, NULL, 0))
    out->wind_dir_deg = v;

  if(wxg_obs_value(props, "windGust", &v, unit, sizeof(unit)))
  {
    out->have_gust = true;
    out->wind_gust = wxg_speed_convert(v, imperial);
  }
}

// ----------------------------------------------------------------------
// The chain: stations -> observation -> forecast
// ----------------------------------------------------------------------
//
// Three legs and one delivery. The observation is the point of the
// request and its failure ends the chain; the forecast behind it is
// enrichment — the day's high and low and the office's own prose — and
// its failure costs those two things and nothing else.

static void
wxg_current_deliver(wxg_request_t *r)
{
  wxg_caller_t caller;

  wxg_req_take_caller(r, &caller);

  if(caller.cb.current != NULL)
    caller.cb.current(&r->acc.current, caller.user);

  wxg_req_release(r);
}

// Leg 3. Nothing here may write the result's `err` or clear `covered`:
// an observation already in hand is the answer, and a forecast that
// failed behind it must not turn that answer into a failure.
static void
wxg_obs_forecast_done(const curl_response_t *resp)
{
  wxg_request_t               *r   = (wxg_request_t *)resp->user_data;
  weathergov_current_result_t *res = &r->acc.current;
  char                         err[128] = "";
  bool                         covered  = true;

  if(!wxg_http_status_ok(resp, err, sizeof(err), &covered)
      || wxg_forecast_parse(resp->body, resp->body_len,
          &res->forecast) != SUCCESS)
    clam(CLAM_DEBUG2, WXG_CTX, "current %s: no forecast to go with the "
        "observation", r->grid);

  wxg_current_deliver(r);
}

// Leg 2 -> leg 3.
static void
wxg_obs_done(const curl_response_t *resp)
{
  wxg_request_t               *r   = (wxg_request_t *)resp->user_data;
  weathergov_current_result_t *res = &r->acc.current;
  struct json_object          *root;
  struct json_object          *props;
  char                         url[WXG_URL_SZ];

  if(!wxg_http_status_ok(resp, res->err, sizeof(res->err), &res->covered))
  {
    // The nearest station having nothing to say is not a coverage
    // verdict, but it costs the caller the same thing: its own provider.
    if(res->err[0] == '\0')
      clam(CLAM_DEBUG2, WXG_CTX, "current %s: station %s answered HTTP %ld",
          r->grid, r->station, resp->status);

    wxg_current_deliver(r);
    return;
  }

  root  = json_parse_buf(resp->body, resp->body_len, WXG_CTX);
  props = (root != NULL) ? json_get_obj(root, "properties") : NULL;

  if(props != NULL)
    wxg_obs_parse(props, strcmp(r->units, "us") == 0, &res->obs);

  if(root != NULL)
    json_object_put(root);

  if(!res->obs.have_temp)
  {
    clam(CLAM_DEBUG2, WXG_CTX, "current %s: station %s reported no "
        "temperature", r->grid, r->station);

    wxg_current_deliver(r);
    return;
  }

  snprintf(res->obs.station, sizeof(res->obs.station), "%s", r->station);

  clam(CLAM_DEBUG2, WXG_CTX, "current %s: %s %.1f (%s)", r->grid,
      r->station, res->obs.temp, res->obs.text);

  snprintf(url, sizeof(url), "%s/%s/forecast?units=%s", WXG_GRID_URL,
      r->grid, r->units);

  if(wxg_http_get(url, r->ua, wxg_obs_forecast_done, r) != SUCCESS)
    wxg_current_deliver(r);
}

// Leg 2's submit, reached either from the station list or straight from
// the entry point on a warm cache.
static bool
wxg_obs_submit(wxg_request_t *r)
{
  char url[WXG_URL_SZ];

  snprintf(url, sizeof(url), "%s/%s/observations/latest", WXG_STATIONS_URL,
      r->station);

  return(wxg_http_get(url, r->ua, wxg_obs_done, r));
}

// Leg 1 -> leg 2. features[0] is the nearest station: the list arrives
// sorted by distance and the first entry was 11.5 km out where the
// second was 18.2.
static void
wxg_stations_done(const curl_response_t *resp)
{
  wxg_request_t               *r   = (wxg_request_t *)resp->user_data;
  weathergov_current_result_t *res = &r->acc.current;
  struct json_object          *root;
  struct json_object          *features;
  struct json_object          *first;
  struct json_object          *props;

  if(!wxg_http_status_ok(resp, res->err, sizeof(res->err), &res->covered))
  {
    if(res->err[0] == '\0')
      clam(CLAM_DEBUG2, WXG_CTX, "current %s: station list HTTP %ld", r->grid,
          resp->status);

    wxg_current_deliver(r);
    return;
  }

  root     = json_parse_buf(resp->body, resp->body_len, WXG_CTX);
  features = (root != NULL) ? json_get_array(root, "features") : NULL;
  first    = (features != NULL && json_object_array_length(features) > 0)
                 ? json_object_array_get_idx(features, 0) : NULL;
  props    = (first != NULL) ? json_get_obj(first, "properties") : NULL;

  if(props != NULL)
    json_get_str(props, "stationIdentifier", r->station, sizeof(r->station));

  if(root != NULL)
    json_object_put(root);

  if(r->station[0] == '\0')
  {
    snprintf(res->err, sizeof(res->err),
        "weather.gov lists no observing station for this area.");
    wxg_current_deliver(r);
    return;
  }

  wxg_station_insert(r->grid, r->station);

  if(wxg_obs_submit(r) != SUCCESS)
    wxg_current_deliver(r);
}

// ----------------------------------------------------------------------
// Public mechanism API
// ----------------------------------------------------------------------

async_rc_t
weathergov_current_async(const weathergov_point_t *pt, const char *units,
    weathergov_current_cb_t cb, void *user)
{
  char           url[WXG_URL_SZ];
  wxg_request_t *r;
  bool           warm;

  if(cb == NULL || pt == NULL || !weathergov_enabled())
    return(ASYNC_FAILED_UNDELIVERED);

  if(units == NULL || strcmp(units, "si") != 0)
    units = "us";

  r = wxg_req_alloc();
  r->type       = WXG_REQ_CURRENT;
  r->cb.current = cb;
  r->user       = user;

  r->acc.current.covered = true;

  snprintf(r->grid,  sizeof(r->grid),  "%s/%d,%d", pt->grid_id, pt->grid_x,
      pt->grid_y);
  snprintf(r->units, sizeof(r->units), "%s", units);
  wxg_ua(r->ua, sizeof(r->ua));

  // File it before anything can be submitted, never after: a completion
  // can run on a curl worker before the submitting call has returned.
  wxg_req_track(r);

  warm = wxg_station_lookup(r->grid, r->station, sizeof(r->station));

  if(warm)
  {
    clam(CLAM_DEBUG2, WXG_CTX, "current %s: station %s [cache hit]", r->grid,
        r->station);

    if(wxg_obs_submit(r) != SUCCESS)
    {
      wxg_req_release(r);
      return(ASYNC_FAILED_UNDELIVERED);
    }

    return(ASYNC_AIRBORNE);
  }

  snprintf(url, sizeof(url), "%s/%s/stations", WXG_GRID_URL, r->grid);

  if(wxg_http_get(url, r->ua, wxg_stations_done, r) != SUCCESS)
  {
    wxg_req_release(r);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  return(ASYNC_AIRBORNE);
}
