// botmanager — MIT
// OpenWeather service plugin: OneCall 3.0 fetch + geocode cache.
#define OW_INTERNAL
#include "openweather.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>

// Input validation

static bool
ow_validate_zipcode(const char *s)
{
  int i;

  if(s == NULL || s[0] == '\0')
    return(false);

  i = 0;

  // Accept 1-10 alphanumeric characters.
  while(i < 10 && ((s[i] >= '0' && s[i] <= '9')
      || (s[i] >= 'A' && s[i] <= 'Z')
      || (s[i] >= 'a' && s[i] <= 'z')))
    i++;

  if(i == 0)
    return(false);

  // End of string — valid.
  if(s[i] == '\0')
    return(true);

  // Optional comma + 2-letter country code.
  if(s[i] != ',')
    return(false);

  i++;

  if(!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')))
    return(false);

  i++;

  if(!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')))
    return(false);

  i++;

  return(s[i] == '\0');
}

// Geocode cache helpers

// returns: cache entry if found and not expired, NULL otherwise.
//          expired entries are freed. must be called under lock.
static ow_geocache_t *
ow_geo_lookup(const char *zipcode)
{
  uint32_t idx;
  ow_geocache_t **pp;
  uint32_t ttl = (uint32_t)kv_get_uint("plugin.openweather.geo_cache_ttl");

  if(ttl == 0)
    return(NULL);

  idx = util_fnv1a(zipcode) % OW_GEO_CACHE_BUCKETS;
  pp = &ow_geo_cache[idx];

  while(*pp != NULL)
  {
    ow_geocache_t *e = *pp;

    if(strcmp(e->zipcode, zipcode) == 0)
    {
      if((time(NULL) - e->cached_at) < (time_t)ttl)
        return(e);

      *pp = e->next;
      mem_free(e);
      return(NULL);
    }

    pp = &e->next;
  }

  return(NULL);
}

// Insert or update a geocode cache entry. Must be called under lock.
static void
ow_geo_insert(const char *zipcode, double lat, double lon, const char *name)
{
  ow_geocache_t *e;
  uint32_t idx = util_fnv1a(zipcode) % OW_GEO_CACHE_BUCKETS;

  for(ow_geocache_t *e = ow_geo_cache[idx]; e != NULL; e = e->next)
  {
    if(strcmp(e->zipcode, zipcode) == 0)
    {
      e->lat = lat;
      e->lon = lon;
      snprintf(e->name, sizeof(e->name), "%s", name);
      e->cached_at = time(NULL);
      return;
    }
  }

  e = mem_alloc("openweather", "geocache", sizeof(*e));

  snprintf(e->zipcode, sizeof(e->zipcode), "%s", zipcode);
  e->lat       = lat;
  e->lon       = lon;
  snprintf(e->name, sizeof(e->name), "%s", name);
  e->cached_at = time(NULL);
  e->next      = ow_geo_cache[idx];

  ow_geo_cache[idx] = e;
}

// Request freelist helpers

static ow_request_t *
ow_req_alloc(void)
{
  ow_request_t *r = NULL;

  pthread_mutex_lock(&ow_free_mu);

  if(ow_free != NULL)
  {
    r = ow_free;
    ow_free = r->next;
  }

  pthread_mutex_unlock(&ow_free_mu);

  if(r == NULL)
    r = mem_alloc("openweather", "request", sizeof(*r));

  memset(r, 0, sizeof(*r));

  return(r);
}

static void
ow_req_release(ow_request_t *r)
{
  pthread_mutex_lock(&ow_free_mu);
  r->next = ow_free;
  ow_free = r;
  pthread_mutex_unlock(&ow_free_mu);
}

// Callback delivery helpers

static void
ow_deliver_current_err(ow_request_t *r, const char *msg)
{
  openweather_current_result_t res;

  memset(&res, 0, sizeof(res));
  snprintf(res.err, sizeof(res.err), "%s", msg);

  if(r->cb.current != NULL)
    r->cb.current(&res, r->user);
}

static void
ow_deliver_forecast_err(ow_request_t *r, const char *msg)
{
  openweather_forecast_result_t res;

  memset(&res, 0, sizeof(res));
  snprintf(res.err, sizeof(res.err), "%s", msg);

  if(r->cb.forecast != NULL)
    r->cb.forecast(&res, r->user);
}

// JSON → typed payload parsers

// Weather description from a "weather" JSON array (uses [0]).
static void
ow_fill_desc(struct json_object *jweather, char *desc_out, size_t desc_sz,
    int32_t *cond_id_out)
{
  int32_t id = 0;
  struct json_object *w0;

  desc_out[0] = '\0';
  *cond_id_out = 0;

  if(jweather == NULL
      || !json_object_is_type(jweather, json_type_array)
      || json_object_array_length(jweather) == 0)
  {
    snprintf(desc_out, desc_sz, "unknown");
    return;
  }

  w0 = json_object_array_get_idx(jweather, 0);

  if(w0 == NULL)
  {
    snprintf(desc_out, desc_sz, "unknown");
    return;
  }

  json_get_int(w0, "id", &id);
  *cond_id_out = id;

  if(!json_get_str(w0, "description", desc_out, desc_sz))
    snprintf(desc_out, desc_sz, "unknown");
}

static int32_t
ow_get_tz_offset(struct json_object *root)
{
  int32_t tz = 0;

  json_get_int(root, "timezone_offset", &tz);
  return(tz);
}

static bool
ow_get_hilo(struct json_object *jtemp_obj, double *hi, double *lo)
{
  return(json_get_double(jtemp_obj, "max", hi)
      && json_get_double(jtemp_obj, "min", lo));
}

static void
ow_parse_alerts(struct json_object *root, openweather_alert_set_t *out)
{
  int n;
  int i;
  struct json_object *jalerts = json_get_array(root, "alerts");

  out->count = 0;

  if(jalerts == NULL)
    return;

  n = (int)json_object_array_length(jalerts);

  for(i = 0; i < n && out->count < OPENWEATHER_ALERT_MAX; i++)
  {
    struct json_object *alert = json_object_array_get_idx(jalerts, i);
    openweather_alert_t *slot = &out->alerts[out->count];

    if(alert == NULL)
      continue;

    slot->event[0] = '\0';

    if(json_get_str(alert, "event", slot->event, sizeof(slot->event)))
      out->count++;
  }
}

static void
ow_parse_current(ow_request_t *r, struct json_object *root,
    openweather_current_result_t *out)
{
  int64_t sunrise_ts = 0;
  int64_t sunset_ts = 0;
  double temp = 0.0;
  double feels = 0.0;
  double wind = 0.0;
  double wind_d = 0.0;
  int32_t humidity = 0;
  int32_t tz_offset;
  double hi = 0.0;
  double lo = 0.0;
  bool have_hilo = false;
  struct json_object *current = json_get_obj(root, "current");
  struct json_object *jweather;
  struct json_object *daily;

  memset(out, 0, sizeof(*out));

  if(current == NULL)
  {
    snprintf(out->err, sizeof(out->err),
        "Error: no current weather data in response");
    return;
  }

  json_get_double(current, "temp",       &temp);
  json_get_double(current, "feels_like", &feels);
  json_get_int   (current, "humidity",   &humidity);
  json_get_double(current, "wind_speed", &wind);
  json_get_double(current, "wind_deg",   &wind_d);
  json_get_int64 (current, "sunrise",    &sunrise_ts);
  json_get_int64 (current, "sunset",     &sunset_ts);

  jweather = json_get_array(current, "weather");
  tz_offset = ow_get_tz_offset(root);

  // Try to extract today's hi/lo from daily[0].
  daily = json_get_array(root, "daily");

  if(daily != NULL && json_object_array_length(daily) > 0)
  {
    struct json_object *today = json_object_array_get_idx(daily, 0);
    struct json_object *jtemp_obj = json_get_obj(today, "temp");

    have_hilo = ow_get_hilo(jtemp_obj, &hi, &lo);
  }

  out->current.temp       = temp;
  out->current.feels_like = feels;
  out->current.wind_speed = wind;
  out->current.wind_deg   = wind_d;
  out->current.humidity   = humidity;
  out->current.sunrise    = (time_t)sunrise_ts;
  out->current.sunset     = (time_t)sunset_ts;
  out->current.tz_offset  = tz_offset;
  out->current.have_hilo  = have_hilo;
  out->current.temp_hi    = hi;
  out->current.temp_lo    = lo;

  ow_fill_desc(jweather, out->current.condition_desc,
      sizeof(out->current.condition_desc), &out->current.condition_id);

  snprintf(out->current.place_name, sizeof(out->current.place_name),
      "%s", r->location_name);
  snprintf(out->current.zipcode, sizeof(out->current.zipcode),
      "%s", r->zipcode);
  snprintf(out->current.units, sizeof(out->current.units),
      "%s", r->units);

  ow_parse_alerts(root, &out->alerts);
}

static void
ow_parse_forecast_daily(ow_request_t *r, struct json_object *root,
    openweather_forecast_result_t *out)
{
  int n;
  int i;
  int32_t tz_offset;
  struct json_object *daily = json_get_array(root, "daily");

  memset(out, 0, sizeof(*out));

  if(daily == NULL)
  {
    snprintf(out->err, sizeof(out->err),
        "Error: no daily forecast data in response");
    return;
  }

  tz_offset = ow_get_tz_offset(root);
  n = (int)json_object_array_length(daily);

  out->forecast.tz_offset = tz_offset;
  snprintf(out->forecast.place_name, sizeof(out->forecast.place_name),
      "%s", r->location_name);
  snprintf(out->forecast.zipcode, sizeof(out->forecast.zipcode),
      "%s", r->zipcode);
  snprintf(out->forecast.units, sizeof(out->forecast.units),
      "%s", r->units);

  for(i = 0; i < n && out->forecast.day_count < OPENWEATHER_FCAST_DAYS; i++)
  {
    int64_t dt_ts = 0;
    double wind = 0.0;
    double wdir = 0.0;
    double pop = 0.0;
    int32_t humid = 0;
    double hi = 0.0;
    double lo = 0.0;
    openweather_forecast_day_t *slot;
    struct json_object *day = json_object_array_get_idx(daily, i);
    struct json_object *jtemp_obj;
    struct json_object *jweather;

    if(day == NULL)
      continue;

    json_get_int64 (day, "dt",         &dt_ts);
    json_get_int   (day, "humidity",   &humid);
    json_get_double(day, "wind_speed", &wind);
    json_get_double(day, "wind_deg",   &wdir);
    json_get_double(day, "pop",        &pop);

    jtemp_obj = json_get_obj  (day, "temp");
    jweather  = json_get_array(day, "weather");

    if(!ow_get_hilo(jtemp_obj, &hi, &lo))
    {
      hi = 0.0;
      lo = 0.0;
    }

    slot = &out->forecast.days[out->forecast.day_count];

    slot->dt         = (time_t)dt_ts;
    slot->temp_hi    = hi;
    slot->temp_lo    = lo;
    slot->wind_speed = wind;
    slot->wind_deg   = wdir;
    slot->pop        = pop;
    slot->humidity   = humid;

    ow_fill_desc(jweather, slot->condition_desc,
        sizeof(slot->condition_desc), &slot->condition_id);

    out->forecast.day_count++;
  }

  ow_parse_alerts(root, &out->alerts);
}

static void
ow_parse_forecast_hourly(ow_request_t *r, struct json_object *root,
    openweather_forecast_result_t *out)
{
  int n;
  int i;
  int32_t tz_offset;
  struct json_object *hourly = json_get_array(root, "hourly");

  memset(out, 0, sizeof(*out));

  if(hourly == NULL)
  {
    snprintf(out->err, sizeof(out->err),
        "Error: no hourly forecast data in response");
    return;
  }

  tz_offset = ow_get_tz_offset(root);
  n = (int)json_object_array_length(hourly);

  out->forecast.tz_offset = tz_offset;
  snprintf(out->forecast.place_name, sizeof(out->forecast.place_name),
      "%s", r->location_name);
  snprintf(out->forecast.zipcode, sizeof(out->forecast.zipcode),
      "%s", r->zipcode);
  snprintf(out->forecast.units, sizeof(out->forecast.units),
      "%s", r->units);

  for(i = 0; i < n && out->forecast.hour_count < OPENWEATHER_FCAST_HOURS; i++)
  {
    int64_t dt_ts = 0;
    double temp = 0.0;
    double wind = 0.0;
    double wdir = 0.0;
    double pop = 0.0;
    int32_t humid = 0;
    openweather_forecast_hour_t *slot;
    struct json_object *hour = json_object_array_get_idx(hourly, i);
    struct json_object *jweather;

    if(hour == NULL)
      continue;

    json_get_int64 (hour, "dt",         &dt_ts);
    json_get_double(hour, "temp",       &temp);
    json_get_int   (hour, "humidity",   &humid);
    json_get_double(hour, "wind_speed", &wind);
    json_get_double(hour, "wind_deg",   &wdir);
    json_get_double(hour, "pop",        &pop);

    jweather = json_get_array(hour, "weather");

    slot = &out->forecast.hours[out->forecast.hour_count];

    slot->dt         = (time_t)dt_ts;
    slot->temp       = temp;
    slot->wind_speed = wind;
    slot->wind_deg   = wdir;
    slot->pop        = pop;
    slot->humidity   = humid;

    ow_fill_desc(jweather, slot->condition_desc,
        sizeof(slot->condition_desc), &slot->condition_id);

    out->forecast.hour_count++;
  }

  ow_parse_alerts(root, &out->alerts);
}

// OneCall submit + completion

static void
ow_submit_onecall(ow_request_t *r)
{
  char url[OW_URL_SZ];
  const char *exclude = "minutely,hourly";

  switch(r->type)
  {
    case OW_REQ_WEATHER:          exclude = "minutely,hourly";         break;
    case OW_REQ_FORECAST_DAILY:   exclude = "minutely,hourly,current"; break;
    case OW_REQ_FORECAST_HOURLY:  exclude = "minutely,daily,current";  break;
  }

  snprintf(url, sizeof(url),
      "%s?lat=%.6f&lon=%.6f&exclude=%s&units=%s&appid=%s",
      OW_ONECALL_URL, r->lat, r->lon, exclude,
      r->units, r->apikey);

  if(curl_get(url, ow_onecall_done, r) != SUCCESS)
  {
    if(r->type == OW_REQ_WEATHER)
      ow_deliver_current_err(r, "Error: failed to submit weather request");
    else
      ow_deliver_forecast_err(r, "Error: failed to submit weather request");

    ow_req_release(r);
  }
}

static void
ow_onecall_done(const curl_response_t *resp)
{
  struct json_object *root;
  char errbuf[128];
  ow_request_t *r = (ow_request_t *)resp->user_data;
  bool is_current = (r->type == OW_REQ_WEATHER);

  if(resp->curl_code != 0)
  {
    snprintf(errbuf, sizeof(errbuf), "Weather API error: %s", resp->error);

    if(is_current)
      ow_deliver_current_err(r, errbuf);
    else
      ow_deliver_forecast_err(r, errbuf);

    ow_req_release(r);
    return;
  }

  if(resp->status == 401)
  {
    if(is_current)
      ow_deliver_current_err(r, "Error: invalid API key");
    else
      ow_deliver_forecast_err(r, "Error: invalid API key");

    ow_req_release(r);
    return;
  }

  if(resp->status == 429)
  {
    if(is_current)
      ow_deliver_current_err(r,
          "Error: API rate limit exceeded, try again later");
    else
      ow_deliver_forecast_err(r,
          "Error: API rate limit exceeded, try again later");

    ow_req_release(r);
    return;
  }

  if(resp->status != 200)
  {
    snprintf(errbuf, sizeof(errbuf),
        "Weather API returned HTTP %ld", resp->status);

    if(is_current)
      ow_deliver_current_err(r, errbuf);
    else
      ow_deliver_forecast_err(r, errbuf);

    ow_req_release(r);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, OW_CTX);

  if(root == NULL)
  {
    if(is_current)
      ow_deliver_current_err(r, "Error: malformed JSON from weather API");
    else
      ow_deliver_forecast_err(r, "Error: malformed JSON from weather API");

    ow_req_release(r);
    return;
  }

  switch(r->type)
  {
    case OW_REQ_WEATHER:
    {
      openweather_current_result_t res;

      ow_parse_current(r, root, &res);

      if(r->cb.current != NULL)
        r->cb.current(&res, r->user);
      break;
    }

    case OW_REQ_FORECAST_DAILY:
    {
      openweather_forecast_result_t res;

      ow_parse_forecast_daily(r, root, &res);

      if(r->cb.forecast != NULL)
        r->cb.forecast(&res, r->user);
      break;
    }

    case OW_REQ_FORECAST_HOURLY:
    {
      openweather_forecast_result_t res;

      ow_parse_forecast_hourly(r, root, &res);

      if(r->cb.forecast != NULL)
        r->cb.forecast(&res, r->user);
      break;
    }
  }

  json_object_put(root);
  ow_req_release(r);
}

static void
ow_geocode_done(const curl_response_t *resp)
{
  struct json_object *root;
  bool have_lat;
  bool have_lon;
  char errbuf[128];
  ow_request_t *r = (ow_request_t *)resp->user_data;
  bool is_current = (r->type == OW_REQ_WEATHER);

  if(resp->curl_code != 0)
  {
    snprintf(errbuf, sizeof(errbuf), "Geocoding error: %s", resp->error);

    if(is_current)
      ow_deliver_current_err(r, errbuf);
    else
      ow_deliver_forecast_err(r, errbuf);

    ow_req_release(r);
    return;
  }

  if(resp->status == 401)
  {
    if(is_current)
      ow_deliver_current_err(r, "Error: invalid API key");
    else
      ow_deliver_forecast_err(r, "Error: invalid API key");

    ow_req_release(r);
    return;
  }

  if(resp->status == 404)
  {
    snprintf(errbuf, sizeof(errbuf),
        "Zipcode %s not found", r->zipcode);

    if(is_current)
      ow_deliver_current_err(r, errbuf);
    else
      ow_deliver_forecast_err(r, errbuf);

    ow_req_release(r);
    return;
  }

  if(resp->status != 200)
  {
    snprintf(errbuf, sizeof(errbuf),
        "Geocoding API returned HTTP %ld", resp->status);

    if(is_current)
      ow_deliver_current_err(r, errbuf);
    else
      ow_deliver_forecast_err(r, errbuf);

    ow_req_release(r);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, OW_CTX);

  if(root == NULL)
  {
    if(is_current)
      ow_deliver_current_err(r, "Error: malformed JSON from geocoding API");
    else
      ow_deliver_forecast_err(r, "Error: malformed JSON from geocoding API");

    ow_req_release(r);
    return;
  }

  have_lat = json_get_double(root, "lat", &r->lat);
  have_lon = json_get_double(root, "lon", &r->lon);

  if(!have_lat || !have_lon)
  {
    json_object_put(root);

    if(is_current)
      ow_deliver_current_err(r,
          "Error: geocoding response missing lat/lon");
    else
      ow_deliver_forecast_err(r,
          "Error: geocoding response missing lat/lon");

    ow_req_release(r);
    return;
  }

  json_get_str(root, "name", r->location_name, sizeof(r->location_name));

  json_object_put(root);

  pthread_mutex_lock(&ow_geo_cache_mu);
  ow_geo_insert(r->zipcode, r->lat, r->lon, r->location_name);
  pthread_mutex_unlock(&ow_geo_cache_mu);

  clam(CLAM_DEBUG2, OW_CTX, "geocode %s -> %s (%.4f, %.4f) [cached]",
      r->zipcode, r->location_name, r->lat, r->lon);

  ow_submit_onecall(r);
}

// Populate a freshly-allocated request with api key / units / cached
// or fresh geocode. Returns SUCCESS if ow_submit_onecall has already
// been scheduled (cache hit), FAIL on any setup error, and leaves the
// caller to kick off the geocode leg on a cache miss.
//
// Protocol:
//   SUCCESS + r scheduled  — onecall already in flight
//   FAIL                   — caller MUST release r and report error
//   other (returns bool)   — geocode needed, caller submits it
//
// Simpler to express as three explicit states; see public fetch fns.
typedef enum
{
  OW_PREP_ERR,
  OW_PREP_CACHE_HIT,
  OW_PREP_NEED_GEOCODE
} ow_prep_result_t;

static ow_prep_result_t
ow_prepare_request(ow_request_t *r, const char *zipcode,
    char *errbuf, size_t errsz)
{
  const char *apikey;
  const char *units;
  ow_geocache_t *cached;

  if(zipcode == NULL || zipcode[0] == '\0')
  {
    snprintf(errbuf, errsz, "Error: missing zipcode");
    return(OW_PREP_ERR);
  }

  if(!ow_validate_zipcode(zipcode))
  {
    snprintf(errbuf, errsz, "Error: invalid zipcode");
    return(OW_PREP_ERR);
  }

  apikey = kv_get_str("plugin.openweather.apikey");

  if(apikey == NULL || apikey[0] == '\0')
  {
    snprintf(errbuf, errsz,
        "Error: OpenWeather API key not configured. "
        "Set plugin.openweather.apikey via /set");
    return(OW_PREP_ERR);
  }

  snprintf(r->zipcode, sizeof(r->zipcode), "%s", zipcode);
  snprintf(r->apikey,  sizeof(r->apikey),  "%s", apikey);

  units = kv_get_str("plugin.openweather.units");

  snprintf(r->units, sizeof(r->units), "%s",
      (units != NULL && units[0] != '\0') ? units : "imperial");

  pthread_mutex_lock(&ow_geo_cache_mu);
  cached = ow_geo_lookup(r->zipcode);

  if(cached != NULL)
  {
    r->lat = cached->lat;
    r->lon = cached->lon;
    snprintf(r->location_name, sizeof(r->location_name), "%s",
        cached->name);
    pthread_mutex_unlock(&ow_geo_cache_mu);

    clam(CLAM_DEBUG2, OW_CTX, "geocode %s -> %s (%.4f, %.4f) [cache hit]",
        r->zipcode, r->location_name, r->lat, r->lon);

    return(OW_PREP_CACHE_HIT);
  }

  pthread_mutex_unlock(&ow_geo_cache_mu);
  return(OW_PREP_NEED_GEOCODE);
}

static bool
ow_kick_off(ow_request_t *r)
{
  char url[OW_URL_SZ];

  snprintf(url, sizeof(url), "%s?zip=%s&appid=%s",
      OW_GEO_URL, r->zipcode, r->apikey);

  if(curl_get(url, ow_geocode_done, r) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

// Public async fetches

bool
openweather_fetch_current(const char *zipcode,
    openweather_done_current_cb_t done_cb, void *user)
{
  char errbuf[128];
  ow_prep_result_t pr;
  ow_request_t *r;

  if(done_cb == NULL)
    return(FAIL);

  r = ow_req_alloc();
  r->type       = OW_REQ_WEATHER;
  r->cb.current = done_cb;
  r->user       = user;

  pr = ow_prepare_request(r, zipcode, errbuf, sizeof(errbuf));

  if(pr == OW_PREP_ERR)
  {
    clam(CLAM_DEBUG, OW_CTX, "fetch_current(%s): %s",
        zipcode != NULL ? zipcode : "(null)", errbuf);
    ow_req_release(r);
    return(FAIL);
  }

  if(pr == OW_PREP_CACHE_HIT)
  {
    ow_submit_onecall(r);
    return(SUCCESS);
  }

  if(ow_kick_off(r) != SUCCESS)
  {
    ow_req_release(r);
    return(FAIL);
  }

  return(SUCCESS);
}

static bool
ow_fetch_forecast_common(ow_req_type_t type, const char *zipcode,
    openweather_done_forecast_cb_t done_cb, void *user)
{
  char errbuf[128];
  ow_prep_result_t pr;
  ow_request_t *r;

  if(done_cb == NULL)
    return(FAIL);

  r = ow_req_alloc();
  r->type        = type;
  r->cb.forecast = done_cb;
  r->user        = user;

  pr = ow_prepare_request(r, zipcode, errbuf, sizeof(errbuf));

  if(pr == OW_PREP_ERR)
  {
    clam(CLAM_DEBUG, OW_CTX, "fetch_forecast(%s): %s",
        zipcode != NULL ? zipcode : "(null)", errbuf);
    ow_req_release(r);
    return(FAIL);
  }

  if(pr == OW_PREP_CACHE_HIT)
  {
    ow_submit_onecall(r);
    return(SUCCESS);
  }

  if(ow_kick_off(r) != SUCCESS)
  {
    ow_req_release(r);
    return(FAIL);
  }

  return(SUCCESS);
}

bool
openweather_fetch_forecast_daily(const char *zipcode,
    openweather_done_forecast_cb_t done_cb, void *user)
{
  return(ow_fetch_forecast_common(OW_REQ_FORECAST_DAILY,
      zipcode, done_cb, user));
}

bool
openweather_fetch_forecast_hourly(const char *zipcode,
    openweather_done_forecast_cb_t done_cb, void *user)
{
  return(ow_fetch_forecast_common(OW_REQ_FORECAST_HOURLY,
      zipcode, done_cb, user));
}

const char *
openweather_units_kv_value(void)
{
  const char *units = kv_get_str("plugin.openweather.units");

  if(units == NULL || units[0] == '\0')
    return("imperial");

  return(units);
}

// String helpers (local to the city-name path)

static void
ow_str_lower(char *dst, size_t cap, const char *src)
{
  size_t i;

  if(cap == 0)
    return;

  i = 0;

  for(; src[i] != '\0' && i + 1 < cap; i++)
  {
    unsigned char c = (unsigned char)src[i];

    dst[i] = (char)tolower(c);
  }

  dst[i] = '\0';
}

static size_t
ow_url_escape(const char *in, char *out, size_t cap)
{
  static const char hex[] = "0123456789ABCDEF";
  size_t n = 0;

  if(cap == 0)
    return(0);

  for(const unsigned char *p = (const unsigned char *)in;
      *p != '\0';
      p++)
  {
    unsigned char c = *p;
    bool unreserved = (c >= 'A' && c <= 'Z')
                   || (c >= 'a' && c <= 'z')
                   || (c >= '0' && c <= '9')
                   || c == '-' || c == '_' || c == '.' || c == '~';

    if(unreserved)
    {
      if(n + 1 < cap)
        out[n] = (char)c;
      n++;
      continue;
    }

    if(n + 3 < cap)
    {
      out[n]     = '%';
      out[n + 1] = hex[c >> 4];
      out[n + 2] = hex[c & 0x0f];
    }

    n += 3;
  }

  out[n < cap ? n : cap - 1] = '\0';

  return(n);
}

// US state name -> ISO 3166-2 subdivision code.
static const struct
{
  const char *name;
  const char *code;
} ow_us_states[] = {
  { "alabama",              "AL" },
  { "alaska",               "AK" },
  { "arizona",              "AZ" },
  { "arkansas",             "AR" },
  { "california",           "CA" },
  { "colorado",             "CO" },
  { "connecticut",          "CT" },
  { "delaware",             "DE" },
  { "district of columbia", "DC" },
  { "florida",              "FL" },
  { "georgia",              "GA" },
  { "hawaii",               "HI" },
  { "idaho",                "ID" },
  { "illinois",             "IL" },
  { "indiana",              "IN" },
  { "iowa",                 "IA" },
  { "kansas",               "KS" },
  { "kentucky",             "KY" },
  { "louisiana",            "LA" },
  { "maine",                "ME" },
  { "maryland",             "MD" },
  { "massachusetts",        "MA" },
  { "michigan",             "MI" },
  { "minnesota",            "MN" },
  { "mississippi",          "MS" },
  { "missouri",             "MO" },
  { "montana",              "MT" },
  { "nebraska",             "NE" },
  { "nevada",               "NV" },
  { "new hampshire",        "NH" },
  { "new jersey",           "NJ" },
  { "new mexico",           "NM" },
  { "new york",             "NY" },
  { "north carolina",       "NC" },
  { "north dakota",         "ND" },
  { "ohio",                 "OH" },
  { "oklahoma",             "OK" },
  { "oregon",               "OR" },
  { "pennsylvania",         "PA" },
  { "rhode island",         "RI" },
  { "south carolina",       "SC" },
  { "south dakota",         "SD" },
  { "tennessee",            "TN" },
  { "texas",                "TX" },
  { "utah",                 "UT" },
  { "vermont",              "VT" },
  { "virginia",             "VA" },
  { "washington",           "WA" },
  { "west virginia",        "WV" },
  { "wisconsin",            "WI" },
  { "wyoming",              "WY" },
};

static const char *
ow_us_state_code(const char *s)
{
  size_t i;
  size_t n = sizeof(ow_us_states) / sizeof(ow_us_states[0]);

  if(s == NULL || s[0] == '\0')
    return(NULL);

  if(s[1] != '\0' && s[2] == '\0'
      && isalpha((unsigned char)s[0])
      && isalpha((unsigned char)s[1]))
  {
    for(i = 0; i < n; i++)
    {
      if(toupper((unsigned char)s[0]) == ow_us_states[i].code[0]
          && toupper((unsigned char)s[1]) == ow_us_states[i].code[1])
        return(ow_us_states[i].code);
    }

    return(NULL);
  }

  for(i = 0; i < n; i++)
  {
    if(strcasecmp(s, ow_us_states[i].name) == 0)
      return(ow_us_states[i].code);
  }

  return(NULL);
}

static char *
ow_str_trim(char *s)
{
  size_t n;
  char *p;

  if(s == NULL)
    return(s);

  p = s;

  while(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
    p++;

  if(p != s)
    memmove(s, p, strlen(p) + 1);

  n = strlen(s);

  while(n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'
      || s[n - 1] == '\r' || s[n - 1] == '\n'))
    s[--n] = '\0';

  return(s);
}

// Canonicalize a user-provided location query for OpenWeather's
// direct-geocode endpoint. See the old inline comment for the full
// rationale; the short version is: OpenWeather's `q` parameter takes
// {city},{state_code},{country_code} with no spaces. Fuzzy values
// silently resolve to unrelated locales.
static void
ow_canon_query(const char *in, char *out, size_t out_sz)
{
  char city[OW_CITY_SZ];
  char part2[OW_CITY_SZ];
  char part3[OW_CITY_SZ];
  const char *state_code;
  const char *comma;
  size_t city_len;

  if(out_sz == 0)
    return;

  out[0] = '\0';

  if(in == NULL)
    return;

  comma = strchr(in, ',');

  if(comma == NULL)
  {
    snprintf(out, out_sz, "%s", in);
    ow_str_trim(out);
    return;
  }

  city_len = (size_t)(comma - in);

  if(city_len >= sizeof(city))
    city_len = sizeof(city) - 1;

  memcpy(city, in, city_len);
  city[city_len] = '\0';
  ow_str_trim(city);

  {
    const char *rest = comma + 1;
    const char *comma2 = strchr(rest, ',');

    part2[0] = '\0';
    part3[0] = '\0';

    if(comma2 == NULL)
    {
      snprintf(part2, sizeof(part2), "%s", rest);
    }

    else
    {
      size_t p2_len = (size_t)(comma2 - rest);

      if(p2_len >= sizeof(part2))
        p2_len = sizeof(part2) - 1;

      memcpy(part2, rest, p2_len);
      part2[p2_len] = '\0';
      snprintf(part3, sizeof(part3), "%s", comma2 + 1);
    }

    ow_str_trim(part2);
    ow_str_trim(part3);
  }

  if(strcasecmp(part3, "united states") == 0
      || strcasecmp(part3, "usa") == 0
      || strcasecmp(part3, "u.s.a.") == 0
      || strcasecmp(part3, "u.s.") == 0
      || strcasecmp(part3, "us") == 0)
    snprintf(part3, sizeof(part3), "US");

  state_code = ow_us_state_code(part2);

  // Precision specifiers cap each field so gcc can see the total output
  // stays under OW_CITY_SZ even when all three parts are maxed out.
  if(state_code != NULL
      && (part3[0] == '\0' || strcasecmp(part3, "US") == 0))
  {
    snprintf(out, out_sz, "%.24s,%.3s,US", city, state_code);
    return;
  }

  if(part3[0] != '\0')
    snprintf(out, out_sz, "%.24s,%.24s,%.24s", city, part2, part3);
  else
    snprintf(out, out_sz, "%.40s,%.40s", city, part2);
}

// City cache (shares ow_geo_cache_mu)

static const ow_citycache_t *
ow_city_lookup_locked(const char *city_lc)
{
  uint32_t idx;
  ow_citycache_t **pp;
  uint32_t ttl = (uint32_t)kv_get_uint("plugin.openweather.geo_cache_ttl");

  if(ttl == 0)
    return(NULL);

  idx = util_fnv1a(city_lc) % OW_CITY_CACHE_BUCKETS;
  pp = &ow_city_cache[idx];

  while(*pp != NULL)
  {
    ow_citycache_t *e = *pp;

    if(strcmp(e->city, city_lc) == 0)
    {
      if((time(NULL) - e->cached_at) < (time_t)ttl)
        return(e);

      *pp = e->next;
      mem_free(e);
      return(NULL);
    }

    pp = &e->next;
  }

  return(NULL);
}

static void
ow_city_insert_locked(const char *city_lc, const char *zipcode)
{
  ow_citycache_t *e;
  uint32_t idx = util_fnv1a(city_lc) % OW_CITY_CACHE_BUCKETS;

  for(ow_citycache_t *e = ow_city_cache[idx]; e != NULL; e = e->next)
  {
    if(strcmp(e->city, city_lc) == 0)
    {
      snprintf(e->zipcode, sizeof(e->zipcode), "%s", zipcode);
      e->cached_at = time(NULL);
      return;
    }
  }

  e = mem_alloc("openweather", "citycache", sizeof(*e));

  snprintf(e->city,    sizeof(e->city),    "%s", city_lc);
  snprintf(e->zipcode, sizeof(e->zipcode), "%s", zipcode);
  e->cached_at = time(NULL);
  e->next      = ow_city_cache[idx];

  ow_city_cache[idx] = e;
}

// Sync curl wrapper (plugin-local)
//
// The city-name path must block until the geocode round-trip completes,
// and include/curl.h offers only async submission, so we wrap one GET
// in a refcounted condvar rendezvous. MUST be called from a worker-pool
// thread; calling from the curl multi-loop thread itself self-deadlocks
// against the request it submits.

typedef struct ow_sync_slot
{
  pthread_mutex_t  mu;
  pthread_cond_t   cv;
  int              refcount;
  bool             done;
  bool             abandoned;
  long             status;
  char            *body;
  size_t           body_len;
} ow_sync_slot_t;

static void
ow_slot_unref(ow_sync_slot_t *slot)
{
  int remaining;

  pthread_mutex_lock(&slot->mu);
  remaining = --slot->refcount;
  pthread_mutex_unlock(&slot->mu);

  if(remaining > 0)
    return;

  if(slot->body != NULL)
    mem_free(slot->body);

  pthread_cond_destroy(&slot->cv);
  pthread_mutex_destroy(&slot->mu);
  mem_free(slot);
}

static void
ow_sync_cb(const curl_response_t *resp)
{
  ow_sync_slot_t *slot = (ow_sync_slot_t *)resp->user_data;

  pthread_mutex_lock(&slot->mu);

  if(!slot->abandoned)
  {
    slot->status = resp->status;

    if(resp->body != NULL && resp->body_len > 0)
    {
      slot->body = mem_alloc("openweather", "sync_body",
          resp->body_len + 1);
      memcpy(slot->body, resp->body, resp->body_len);
      slot->body[resp->body_len] = '\0';
      slot->body_len = resp->body_len;
    }

    slot->done = true;
    pthread_cond_signal(&slot->cv);
  }

  pthread_mutex_unlock(&slot->mu);

  ow_slot_unref(slot);
}

static char *
ow_http_get_sync(const char *url, uint32_t timeout_secs,
    size_t *body_len, long *http_status)
{
  struct timespec deadline;
  int rc;
  char *body;
  ow_sync_slot_t *slot = mem_alloc("openweather", "sync_slot",
      sizeof(*slot));

  memset(slot, 0, sizeof(*slot));
  pthread_mutex_init(&slot->mu, NULL);
  pthread_cond_init(&slot->cv, NULL);
  slot->refcount = 2;

  if(curl_get(url, ow_sync_cb, slot) != SUCCESS)
  {
    ow_slot_unref(slot);
    ow_slot_unref(slot);
    return(NULL);
  }

  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += (time_t)timeout_secs;

  pthread_mutex_lock(&slot->mu);

  rc = 0;
  while(!slot->done && rc == 0)
    rc = pthread_cond_timedwait(&slot->cv, &slot->mu, &deadline);

  body = NULL;

  if(slot->done)
  {
    body       = slot->body;
    slot->body = NULL;

    if(body_len != NULL)
      *body_len = slot->body_len;
    if(http_status != NULL)
      *http_status = slot->status;
  }

  else
  {
    slot->abandoned = true;
    clam(CLAM_WARN, OW_CTX,
        "sync GET timed out after %us: %s (abandoned, cb will clean)",
        timeout_secs, url);
  }

  pthread_mutex_unlock(&slot->mu);
  ow_slot_unref(slot);

  if(body != NULL && http_status != NULL
      && (*http_status < 200 || *http_status >= 300))
  {
    mem_free(body);
    return(NULL);
  }

  return(body);
}

static bool
ow_parse_direct_geo(const char *body, size_t body_len,
    char *name_out, size_t name_sz,
    char *zip_out, size_t zip_sz,
    double *lat, double *lon)
{
  bool have_lon;
  struct json_object *e0;
  bool have_lat;
  struct json_object *root = json_parse_buf(body, body_len, OW_CTX);

  if(root == NULL)
    return(FAIL);

  if(!json_object_is_type(root, json_type_array)
      || json_object_array_length(root) == 0)
  {
    json_object_put(root);
    return(FAIL);
  }

  e0 = json_object_array_get_idx(root, 0);

  if(e0 == NULL)
  {
    json_object_put(root);
    return(FAIL);
  }

  have_lat = json_get_double(e0, "lat", lat);
  have_lon = json_get_double(e0, "lon", lon);

  if(!have_lat || !have_lon)
  {
    json_object_put(root);
    return(FAIL);
  }

  name_out[0] = '\0';
  json_get_str(e0, "name", name_out, name_sz);

  zip_out[0] = '\0';

  if(!json_get_str(e0, "zip", zip_out, zip_sz))
    json_get_str(e0, "postcode", zip_out, zip_sz);

  json_object_put(root);

  return(SUCCESS);
}

// Public sync geocode: CITY → ZIP.
bool
openweather_geocode_city_sync(const char *city, char *zip_out, size_t zip_sz)
{
  char canon[OW_CITY_SZ];
  char city_lc[OW_CITY_SZ];
  char encoded[OW_CITY_SZ * 3 + 1];
  bool parsed;
  char url[OW_URL_SZ];
  double lon;
  char *body;
  double lat;
  char zip[OW_ZIPCODE_SZ];
  char name[OW_NAME_SZ];
  long http_stat;
  const ow_citycache_t *hit;
  const char *apikey;
  size_t body_len;

  if(city == NULL || city[0] == '\0' || zip_out == NULL || zip_sz < 2)
    return(FAIL);

  ow_canon_query(city, canon, sizeof(canon));

  if(canon[0] == '\0')
    return(FAIL);

  ow_str_lower(city_lc, sizeof(city_lc), canon);

  pthread_mutex_lock(&ow_geo_cache_mu);

  hit = ow_city_lookup_locked(city_lc);

  if(hit != NULL)
  {
    snprintf(zip_out, zip_sz, "%s", hit->zipcode);
    pthread_mutex_unlock(&ow_geo_cache_mu);
    clam(CLAM_DEBUG2, OW_CTX, "city geocode %s -> %s [cache hit]",
        city_lc, zip_out);
    return(SUCCESS);
  }

  pthread_mutex_unlock(&ow_geo_cache_mu);

  apikey = kv_get_str("plugin.openweather.apikey");

  if(apikey == NULL || apikey[0] == '\0')
  {
    clam(CLAM_DEBUG, OW_CTX,
        "city geocode '%s': apikey not configured; abort",
        city_lc);
    return(FAIL);
  }

  ow_url_escape(canon, encoded, sizeof(encoded));

  snprintf(url, sizeof(url), "%s?q=%s&limit=1&appid=%s",
      OW_GEO_DIRECT_URL, encoded, apikey);

  body_len = 0;
  http_stat = 0;
  body = ow_http_get_sync(url, OW_CITY_TIMEOUT_SECS,
                          &body_len, &http_stat);

  if(body == NULL)
  {
    clam(CLAM_DEBUG, OW_CTX,
        "city geocode '%s': direct GET failed (status=%ld)",
        city_lc, http_stat);
    return(FAIL);
  }

  lat = 0.0;
  lon = 0.0;

  parsed = ow_parse_direct_geo(body, body_len,
      name, sizeof(name), zip, sizeof(zip), &lat, &lon);

  mem_free(body);

  if(parsed != SUCCESS)
  {
    clam(CLAM_DEBUG, OW_CTX,
        "city geocode '%s': direct response unparsable / empty",
        city_lc);
    return(FAIL);
  }

  if(zip[0] == '\0')
  {
    bool rparsed;
    double rev_lon;
    char *rb;
    double rev_lat;
    char rev_name[OW_NAME_SZ];
    long rb_stat;
    size_t rb_len;
    char rev_url[OW_URL_SZ];

    snprintf(rev_url, sizeof(rev_url),
        "%s?lat=%.6f&lon=%.6f&limit=1&appid=%s",
        OW_GEO_REVERSE_URL, lat, lon, apikey);

    rb_len = 0;
    rb_stat = 0;
    rb = ow_http_get_sync(rev_url, OW_CITY_TIMEOUT_SECS,
                         &rb_len, &rb_stat);

    if(rb == NULL)
    {
      clam(CLAM_DEBUG, OW_CTX,
          "city geocode '%s': reverse GET failed (status=%ld)",
          city_lc, rb_stat);
      return(FAIL);
    }

    rev_lat = 0.0;
    rev_lon = 0.0;

    rparsed = ow_parse_direct_geo(rb, rb_len,
        rev_name, sizeof(rev_name), zip, sizeof(zip),
        &rev_lat, &rev_lon);

    mem_free(rb);

    // Neither endpoint returned a postcode — synthesize a stable
    // 9-char key so the zip→lat/lon cache short-circuits the next
    // lookup. Passes ow_validate_zipcode.
    if(rparsed != SUCCESS || zip[0] == '\0')
    {
      char   latlon[48];
      uint32_t h;

      snprintf(latlon, sizeof(latlon), "%.6f,%.6f", lat, lon);
      h = util_fnv1a(latlon);
      snprintf(zip, sizeof(zip), "G%08X", (unsigned)h);

      clam(CLAM_DEBUG, OW_CTX,
          "city geocode '%s': no postcode, synthesized '%s' "
          "(lat=%.4f lon=%.4f)",
          city_lc, zip, lat, lon);
    }
  }

  pthread_mutex_lock(&ow_geo_cache_mu);
  ow_city_insert_locked(city_lc, zip);
  ow_geo_insert(zip, lat, lon, name[0] != '\0' ? name : city);
  pthread_mutex_unlock(&ow_geo_cache_mu);

  snprintf(zip_out, zip_sz, "%s", zip);

  clam(CLAM_DEBUG, OW_CTX,
      "city geocode %s -> %s (%.4f, %.4f) [cached]",
      city_lc, zip, lat, lon);

  return(SUCCESS);
}

// Public sync geocode: ZIP → lat/lon/name. Uses the cache when warm;
// on a miss issues one sync GET against the zip geocoder. name_out
// may be NULL.
bool
openweather_geocode_zip_sync(const char *zipcode,
    double *lat_out, double *lon_out,
    char *name_out, size_t name_sz)
{
  ow_geocache_t *cached;
  const char *apikey;
  char url[OW_URL_SZ];
  char *body;
  size_t body_len = 0;
  long http_stat = 0;
  struct json_object *root;
  double lat;
  double lon;
  char name[OW_NAME_SZ];

  if(zipcode == NULL || zipcode[0] == '\0'
      || lat_out == NULL || lon_out == NULL)
    return(FAIL);

  if(!ow_validate_zipcode(zipcode))
    return(FAIL);

  pthread_mutex_lock(&ow_geo_cache_mu);
  cached = ow_geo_lookup(zipcode);

  if(cached != NULL)
  {
    *lat_out = cached->lat;
    *lon_out = cached->lon;

    if(name_out != NULL && name_sz > 0)
      snprintf(name_out, name_sz, "%s", cached->name);

    pthread_mutex_unlock(&ow_geo_cache_mu);
    return(SUCCESS);
  }

  pthread_mutex_unlock(&ow_geo_cache_mu);

  apikey = kv_get_str("plugin.openweather.apikey");

  if(apikey == NULL || apikey[0] == '\0')
    return(FAIL);

  snprintf(url, sizeof(url), "%s?zip=%s&appid=%s",
      OW_GEO_URL, zipcode, apikey);

  body = ow_http_get_sync(url, OW_CITY_TIMEOUT_SECS,
                          &body_len, &http_stat);

  if(body == NULL)
    return(FAIL);

  root = json_parse_buf(body, body_len, OW_CTX);
  mem_free(body);

  if(root == NULL)
    return(FAIL);

  lat = 0.0;
  lon = 0.0;
  name[0] = '\0';

  if(!json_get_double(root, "lat", &lat)
      || !json_get_double(root, "lon", &lon))
  {
    json_object_put(root);
    return(FAIL);
  }

  json_get_str(root, "name", name, sizeof(name));
  json_object_put(root);

  pthread_mutex_lock(&ow_geo_cache_mu);
  ow_geo_insert(zipcode, lat, lon, name);
  pthread_mutex_unlock(&ow_geo_cache_mu);

  *lat_out = lat;
  *lon_out = lon;

  if(name_out != NULL && name_sz > 0)
    snprintf(name_out, name_sz, "%s", name);

  return(SUCCESS);
}

// Plugin lifecycle

static bool
ow_init(void)
{
  pthread_mutex_init(&ow_free_mu, NULL);
  pthread_mutex_init(&ow_geo_cache_mu, NULL);
  memset(ow_geo_cache,  0, sizeof(ow_geo_cache));
  memset(ow_city_cache, 0, sizeof(ow_city_cache));

  clam(CLAM_INFO, OW_CTX, "openweather plugin initialized");

  return(SUCCESS);
}

static void
ow_deinit(void)
{
  // Free the request freelist.
  pthread_mutex_lock(&ow_free_mu);

  while(ow_free != NULL)
  {
    ow_request_t *r = ow_free;

    ow_free = r->next;
    mem_free(r);
  }

  pthread_mutex_unlock(&ow_free_mu);
  pthread_mutex_destroy(&ow_free_mu);

  for(uint32_t i = 0; i < OW_GEO_CACHE_BUCKETS; i++)
  {
    ow_geocache_t *e = ow_geo_cache[i];

    while(e != NULL)
    {
      ow_geocache_t *next = e->next;

      mem_free(e);
      e = next;
    }

    ow_geo_cache[i] = NULL;
  }

  for(uint32_t i = 0; i < OW_CITY_CACHE_BUCKETS; i++)
  {
    ow_citycache_t *e = ow_city_cache[i];

    while(e != NULL)
    {
      ow_citycache_t *next = e->next;

      mem_free(e);
      e = next;
    }

    ow_city_cache[i] = NULL;
  }

  pthread_mutex_destroy(&ow_geo_cache_mu);

  clam(CLAM_INFO, OW_CTX, "openweather plugin deinitialized");
}

// Plugin descriptor

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "openweather",
  .version         = "2.0",
  .type            = PLUGIN_SERVICE,
  .kind            = "openweather",
  .provides        = { { .name = "service_openweather" } },
  .provides_count  = 1,
  .requires_count  = 0,
  .kv_schema       = ow_kv_schema,
  .kv_schema_count = 3,
  .init            = ow_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = ow_deinit,
  .ext             = NULL,
};
