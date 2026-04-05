#define OW_INTERNAL
#include "openweather.h"
#include "util.h"

// -----------------------------------------------------------------------
// Input validation
// -----------------------------------------------------------------------

// Validate a zipcode string.  Accepts 1-10 alphanumeric characters,
// optionally followed by a comma and a 2-letter ISO country code
// (e.g., "45069", "SW1A1AA,GB").  Rejects everything else.
// returns: true if valid, false otherwise
// s: NUL-terminated zipcode string
static bool
ow_validate_zipcode(const char *s)
{
  if(s == NULL || s[0] == '\0')
    return false;

  int i = 0;

  // Accept 1-10 alphanumeric characters.
  while(i < 10 && ((s[i] >= '0' && s[i] <= '9')
      || (s[i] >= 'A' && s[i] <= 'Z')
      || (s[i] >= 'a' && s[i] <= 'z')))
    i++;

  if(i == 0)
    return false;

  // End of string — valid.
  if(s[i] == '\0')
    return true;

  // Optional comma + 2-letter country code.
  if(s[i] != ',')
    return false;

  i++;

  if(!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')))
    return false;

  i++;

  if(!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')))
    return false;

  i++;

  return s[i] == '\0';
}

// -----------------------------------------------------------------------
// Geocode cache helpers
// -----------------------------------------------------------------------


// returns: cache entry if found and not expired, NULL otherwise.
//          expired entries are freed. must be called under lock.
// zipcode: zipcode string to look up
static ow_geocache_t *
ow_geo_lookup(const char *zipcode)
{
  uint32_t ttl = (uint32_t)kv_get_uint("plugin.openweather.geo_cache_ttl");

  if(ttl == 0)
    return(NULL);

  uint32_t idx = util_fnv1a(zipcode) % OW_GEO_CACHE_BUCKETS;
  ow_geocache_t **pp = &ow_geo_cache[idx];

  while(*pp != NULL)
  {
    ow_geocache_t *e = *pp;

    if(strcmp(e->zipcode, zipcode) == 0)
    {
      if((time(NULL) - e->cached_at) < (time_t)ttl)
        return(e);

      // Expired — remove and free.
      *pp = e->next;
      mem_free(e);
      return(NULL);
    }

    pp = &e->next;
  }

  return(NULL);
}

// Insert or update a geocode cache entry. Must be called under lock.
// zipcode: zipcode string key
// lat: latitude
// lon: longitude
// name: location name
static void
ow_geo_insert(const char *zipcode, double lat, double lon, const char *name)
{
  uint32_t idx = util_fnv1a(zipcode) % OW_GEO_CACHE_BUCKETS;

  // Check for existing entry to update.
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

  // New entry.
  ow_geocache_t *e = mem_alloc("openweather", "geocache", sizeof(*e));

  snprintf(e->zipcode, sizeof(e->zipcode), "%s", zipcode);
  e->lat       = lat;
  e->lon       = lon;
  snprintf(e->name, sizeof(e->name), "%s", name);
  e->cached_at = time(NULL);
  e->next      = ow_geo_cache[idx];

  ow_geo_cache[idx] = e;
}

// -----------------------------------------------------------------------
// Request freelist helpers
// -----------------------------------------------------------------------

// returns: zeroed request from freelist or fresh allocation
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

// Return a request to the freelist.
// r: request to release
static void
ow_req_release(ow_request_t *r)
{
  pthread_mutex_lock(&ow_free_mu);
  r->next = ow_free;
  ow_free = r;
  pthread_mutex_unlock(&ow_free_mu);
}

// -----------------------------------------------------------------------
// Reply helper
// -----------------------------------------------------------------------

// Send a reply using the saved command context.
// r: request carrying the context
// text: reply text
static void
ow_reply(ow_request_t *r, const char *text)
{
  cmd_reply(&r->ctx, text);
}

// -----------------------------------------------------------------------
// Unit display helpers
// -----------------------------------------------------------------------

// returns: temperature unit suffix for the given unit system
// units: "imperial", "metric", or "standard"
static const char *
ow_temp_unit(const char *units)
{
  if(strcmp(units, "metric") == 0)
    return("C");

  if(strcmp(units, "standard") == 0)
    return("K");

  return("F");
}

// returns: wind speed unit for the given unit system
// units: "imperial", "metric", or "standard"
static const char *
ow_speed_unit(const char *units)
{
  if(strcmp(units, "imperial") == 0)
    return("mph");

  return("m/s");
}

// -----------------------------------------------------------------------
// Wind direction from degrees
// -----------------------------------------------------------------------

// returns: cardinal/intercardinal direction string
// deg: wind direction in degrees (0-360)
static const char *
ow_wind_dir(double deg)
{
  static const char *dirs[] = {
    "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
    "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"
  };

  int idx = ((int)((deg + 11.25) / 22.5)) % 16;

  return(dirs[idx]);
}

// -----------------------------------------------------------------------
// Time formatting
// -----------------------------------------------------------------------

// Format a Unix timestamp to HH:MM using the given timezone offset.
// ts: Unix timestamp
// tz_offset: timezone offset in seconds from UTC
// buf: output buffer
// sz: buffer size
static void
ow_format_time(time_t ts, int tz_offset, char *buf, size_t sz)
{
  time_t local = ts + tz_offset;
  struct tm tm;

  gmtime_r(&local, &tm);
  snprintf(buf, sz, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

// -----------------------------------------------------------------------
// Temperature color helpers
// -----------------------------------------------------------------------

// Convert a temperature to Fahrenheit for color threshold comparison.
// temp: temperature in the given unit system
// units: "imperial", "metric", or "standard"
static double
ow_to_fahrenheit(double temp, const char *units)
{
  if(strcmp(units, "metric") == 0)
    return(temp * 9.0 / 5.0 + 32.0);

  if(strcmp(units, "standard") == 0)
    return((temp - 273.15) * 9.0 / 5.0 + 32.0);

  return(temp);
}

// returns: color prefix string for a temperature in Fahrenheit.
//          Uses a smooth 8-tier gradient from extreme heat to extreme cold.
// temp_f: temperature in Fahrenheit
static const char *
ow_temp_color(double temp_f)
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

// Format a colored temperature value into buf.
// returns: number of bytes written (excluding NUL)
// buf: output buffer
// sz: buffer size
// temp: temperature in the user's unit system
// units: "imperial", "metric", or "standard"
static int
ow_fmt_temp(char *buf, size_t sz, double temp, const char *units)
{
  const char *clr = ow_temp_color(ow_to_fahrenheit(temp, units));

  // The bold toggle pair (CLR_BOLD CLR_BOLD) between the color code
  // and the digit prevents IRC clients from consuming temperature
  // digits as part of the \003NN color parameter sequence.
  if(*clr != '\0')
    return(snprintf(buf, sz, "%s" CLR_BOLD CLR_BOLD "%.0f%s",
        clr, temp, CLR_RESET));

  return(snprintf(buf, sz, "%.0f", temp));
}

// -----------------------------------------------------------------------
// Weather description helper
// -----------------------------------------------------------------------

// Extract the description string from a "weather" JSON array.
// returns: description string (pointer into JSON, valid until json_object_put)
// jweather: the "weather" array from the API response
static const char *
ow_get_desc(struct json_object *jweather)
{
  if(jweather == NULL || !json_object_is_type(jweather, json_type_array)
      || json_object_array_length(jweather) == 0)
    return("unknown");

  struct json_object *w0 = json_object_array_get_idx(jweather, 0);
  struct json_object *jdesc = NULL;

  if(w0 != NULL && json_object_object_get_ex(w0, "description", &jdesc))
    return(json_object_get_string(jdesc));

  return("unknown");
}

// -----------------------------------------------------------------------
// Weather condition helpers
// -----------------------------------------------------------------------

// Extract the weather condition ID from a "weather" JSON array.
// returns: condition ID (e.g. 800 for clear), or 0 if unavailable
// jweather: the "weather" array from the API response
static int
ow_get_condition_id(struct json_object *jweather)
{
  if(jweather == NULL || !json_object_is_type(jweather, json_type_array)
      || json_object_array_length(jweather) == 0)
    return(0);

  struct json_object *w0 = json_object_array_get_idx(jweather, 0);
  struct json_object *jid = NULL;

  if(w0 != NULL && json_object_object_get_ex(w0, "id", &jid))
    return(json_object_get_int(jid));

  return(0);
}

// Map a weather condition ID to a Unicode weather emoji (UTF-8).
// All emoji include VS16 (\xef\xb8\x8f) for consistent 2-cell
// rendering. Must be placed at the START of each line so variable
// emoji width does not break column alignment.
// returns: UTF-8 emoji string
// id: OpenWeather condition ID (2xx-8xx)
static const char *
ow_condition_icon(int id)
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

// Map a weather condition ID to an abstract color prefix.
// returns: abstract color marker string (may be empty)
// id: OpenWeather condition ID
static const char *
ow_condition_color(int id)
{
  if(id >= 200 && id < 300) return(CLR_RED);        // thunderstorm
  if(id >= 300 && id < 400) return(CLR_CYAN);       // drizzle
  if(id >= 500 && id < 600) return(CLR_CYAN);       // rain
  if(id >= 600 && id < 700) return(CLR_BOLD);       // snow (bold white)
  if(id >= 700 && id < 800) return(CLR_PURPLE);     // fog/mist/haze
  if(id == 800)             return(CLR_YELLOW);      // clear sky
  if(id == 801)             return(CLR_YELLOW);      // few clouds
  if(id == 802)             return(CLR_GRAY);        // scattered clouds
  if(id >= 803)             return(CLR_GRAY);        // overcast

  return("");
}

// -----------------------------------------------------------------------
// AM/PM time formatting
// -----------------------------------------------------------------------

// Format a Unix timestamp to h:MMam/pm using the given timezone offset.
// ts: Unix timestamp
// tz_offset: timezone offset in seconds from UTC
// buf: output buffer
// sz: buffer size
static void
ow_format_time_ampm(time_t ts, int tz_offset, char *buf, size_t sz)
{
  time_t local = ts + tz_offset;
  struct tm tm;

  gmtime_r(&local, &tm);

  int h = tm.tm_hour % 12;

  if(h == 0)
    h = 12;

  snprintf(buf, sz, "%d:%02d%s", h, tm.tm_min,
      tm.tm_hour < 12 ? "am" : "pm");
}

// -----------------------------------------------------------------------
// Shared helpers for weather/forecast handlers
// -----------------------------------------------------------------------

// Extract timezone_offset from a OneCall JSON root.
// returns: offset in seconds, or 0 if absent
// root: parsed OneCall JSON root
static int
ow_get_tz_offset(struct json_object *root)
{
  struct json_object *jtz = NULL;

  json_object_object_get_ex(root, "timezone_offset", &jtz);

  return(jtz ? json_object_get_int(jtz) : 0);
}

// Reply with a bold location header line.
// r: request context
// subtitle: e.g. "7-day forecast" or "24-hour forecast"
static void
ow_reply_header(ow_request_t *r, const char *subtitle)
{
  char buf[OW_REPLY_SZ];

  snprintf(buf, sizeof(buf),
      CLR_BOLD "%s" CLR_RESET " (%s) "
      "\xe2\x80\x94 " CLR_BOLD "%s" CLR_RESET,
      r->location_name, r->zipcode, subtitle);
  ow_reply(r, buf);
}

// Format a precipitation-chance suffix into buf.
// Writes an empty string when pop is zero.
// buf: output buffer
// sz: buffer size
// pop: probability of precipitation 0-100
static void
ow_fmt_precip(char *buf, size_t sz, int pop)
{
  if(pop > 0)
    snprintf(buf, sz, " " CLR_CYAN "%d%% precip" CLR_RESET, pop);
  else
    buf[0] = '\0';
}

// Pad/truncate a description to a fixed-width column.
// buf: output buffer (should be at least width+2 bytes)
// sz: buffer size
// desc: description string
// width: column width
static void
ow_fmt_desc_pad(char *buf, size_t sz, const char *desc, int width)
{
  snprintf(buf, sz, "%-*.*s", width, width, desc);
}

// Extract hi/lo temperatures from a daily "temp" JSON object.
// returns: true if both max and min were found
// jtemp_obj: the "temp" object from a daily entry
// hi: output high temperature
// lo: output low temperature
static bool
ow_get_hilo(struct json_object *jtemp_obj, double *hi, double *lo)
{
  if(jtemp_obj == NULL)
    return(false);

  struct json_object *jhi = NULL, *jlo = NULL;

  json_object_object_get_ex(jtemp_obj, "max", &jhi);
  json_object_object_get_ex(jtemp_obj, "min", &jlo);

  if(jhi == NULL || jlo == NULL)
    return(false);

  *hi = json_object_get_double(jhi);
  *lo = json_object_get_double(jlo);

  return(true);
}

// Reply with weather alerts (up to 3) from a OneCall response.
// r: request context
// root: parsed OneCall JSON root
static void
ow_reply_alerts(ow_request_t *r, struct json_object *root)
{
  struct json_object *jalerts = NULL;

  if(!json_object_object_get_ex(root, "alerts", &jalerts)
      || !json_object_is_type(jalerts, json_type_array))
    return;

  int n = (int)json_object_array_length(jalerts);
  char buf[OW_REPLY_SZ];

  for(int i = 0; i < n && i < 3; i++)
  {
    struct json_object *alert = json_object_array_get_idx(jalerts, i);
    struct json_object *jevent = NULL;

    if(alert != NULL
        && json_object_object_get_ex(alert, "event", &jevent))
    {
      snprintf(buf, sizeof(buf),
          "  " CLR_BOLD CLR_RED "\xe2\x9a\xa0" CLR_RESET " "
          CLR_BOLD CLR_YELLOW "ALERT:" CLR_RESET " %s",
          json_object_get_string(jevent));
      ow_reply(r, buf);
    }
  }
}

// -----------------------------------------------------------------------
// OnCall response handler: !weather
// -----------------------------------------------------------------------

// Parse current weather from a OneCall response and reply.
// Two-line display with weather emoji, color-coded temperatures,
// and hi/lo from daily data when available.
// r: request context
// root: parsed JSON root object
static void
ow_handle_weather(ow_request_t *r, struct json_object *root)
{
  struct json_object *current = NULL;

  if(!json_object_object_get_ex(root, "current", &current))
  {
    ow_reply(r, "Error: no current weather data in response");
    return;
  }

  struct json_object *jtemp = NULL, *jfeels = NULL, *jhumid = NULL;
  struct json_object *jwind = NULL, *jwind_deg = NULL;
  struct json_object *jsunrise = NULL, *jsunset = NULL;
  struct json_object *jweather = NULL;

  json_object_object_get_ex(current, "temp", &jtemp);
  json_object_object_get_ex(current, "feels_like", &jfeels);
  json_object_object_get_ex(current, "humidity", &jhumid);
  json_object_object_get_ex(current, "wind_speed", &jwind);
  json_object_object_get_ex(current, "wind_deg", &jwind_deg);
  json_object_object_get_ex(current, "sunrise", &jsunrise);
  json_object_object_get_ex(current, "sunset", &jsunset);
  json_object_object_get_ex(current, "weather", &jweather);

  double temp      = jtemp     ? json_object_get_double(jtemp)     : 0.0;
  double feels     = jfeels    ? json_object_get_double(jfeels)    : 0.0;
  int    humidity  = jhumid    ? json_object_get_int(jhumid)       : 0;
  double wind      = jwind     ? json_object_get_double(jwind)     : 0.0;
  double wind_d    = jwind_deg ? json_object_get_double(jwind_deg) : 0.0;
  int    tz_offset = ow_get_tz_offset(root);

  const char *desc = ow_get_desc(jweather);
  int cond_id      = ow_get_condition_id(jweather);

  // Sunrise / sunset as AM/PM.
  char sunrise[12] = "?", sunset[12] = "?";

  if(jsunrise != NULL)
    ow_format_time_ampm(json_object_get_int64(jsunrise), tz_offset,
        sunrise, sizeof(sunrise));

  if(jsunset != NULL)
    ow_format_time_ampm(json_object_get_int64(jsunset), tz_offset,
        sunset, sizeof(sunset));

  const char *tu   = ow_temp_unit(r->units);
  const char *su   = ow_speed_unit(r->units);
  const char *icon = ow_condition_icon(cond_id);
  const char *dclr = ow_condition_color(cond_id);

  // Format colored temperatures.
  char ct[32], cf[32];

  ow_fmt_temp(ct, sizeof(ct), temp, r->units);
  ow_fmt_temp(cf, sizeof(cf), feels, r->units);

  char buf[OW_REPLY_SZ];

  // Try to extract today's hi/lo from daily data.
  double hi = 0.0, lo = 0.0;
  bool have_hilo = false;
  struct json_object *daily = NULL;

  if(json_object_object_get_ex(root, "daily", &daily)
      && json_object_is_type(daily, json_type_array)
      && json_object_array_length(daily) > 0)
  {
    struct json_object *today = json_object_array_get_idx(daily, 0);
    struct json_object *jtemp_obj = NULL;

    if(today != NULL
        && json_object_object_get_ex(today, "temp", &jtemp_obj))
      have_hilo = ow_get_hilo(jtemp_obj, &hi, &lo);
  }

  // --- Line 1: icon + location + condition + temperature ---
  //
  // ☀️  Springfield (45069) — clear sky · 79°F (feels 79°F)
  snprintf(buf, sizeof(buf),
      "%s " CLR_BOLD "%s" CLR_RESET " (%s) "
      "\xe2\x80\x94 %s%s" CLR_RESET
      " " CLR_GRAY "\xc2\xb7" CLR_RESET " "
      "%s\xc2\xb0%s (feels %s\xc2\xb0%s)",
      icon, r->location_name, r->zipcode,
      dclr, desc,
      ct, tu, cf, tu);

  ow_reply(r, buf);

  // --- Line 2: hi/lo + humidity + wind + sunrise/sunset ---
  //
  // With hi/lo:
  //   Hi 85°/Lo 62° · Humidity 45% · Wind 12mph SSW · ☀ 7:20am ☽ 8:01pm
  //
  // Without hi/lo:
  //   Humidity 45% · Wind 12mph SSW · ☀ 7:20am ☽ 8:01pm
  if(have_hilo)
  {
    char chi[32], clo[32];

    ow_fmt_temp(chi, sizeof(chi), hi, r->units);
    ow_fmt_temp(clo, sizeof(clo), lo, r->units);

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
        humidity,
        wind, su, ow_wind_dir(wind_d),
        sunrise, sunset);
  }
  else
  {
    snprintf(buf, sizeof(buf),
        "  "
        CLR_CYAN "Humidity" CLR_RESET " %d%%"
        " " CLR_GRAY "\xc2\xb7" CLR_RESET " "
        CLR_GREEN "Wind" CLR_RESET " %.0f%s %s"
        " " CLR_GRAY "\xc2\xb7" CLR_RESET " "
        CLR_YELLOW "Rise" CLR_RESET " %s "
        CLR_PURPLE "Set" CLR_RESET " %s",
        humidity,
        wind, su, ow_wind_dir(wind_d),
        sunrise, sunset);
  }

  ow_reply(r, buf);

  // --- Alerts (if any) ---
  ow_reply_alerts(r, root);
}

// -----------------------------------------------------------------------
// OnCall response handler: !forecast
// -----------------------------------------------------------------------

// Parse daily forecast from a OneCall response and reply.
// Uses weather emoji, color-coded hi/lo temps, aligned columns,
// humidity, wind, and precipitation chance.
// r: request context
// root: parsed JSON root object
static void
ow_handle_forecast(ow_request_t *r, struct json_object *root)
{
  struct json_object *daily = NULL;

  if(!json_object_object_get_ex(root, "daily", &daily)
      || !json_object_is_type(daily, json_type_array))
  {
    ow_reply(r, "Error: no daily forecast data in response");
    return;
  }

  const char *tu = ow_temp_unit(r->units);
  const char *su = ow_speed_unit(r->units);
  int n = (int)json_object_array_length(daily);

  ow_reply_header(r, "7-day forecast");

  static const char *day_names[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday",
    "Thursday", "Friday", "Saturday"
  };

  int tz_offset = ow_get_tz_offset(root);

  // Show up to 7 days.
  for(int i = 0; i < n && i < 7; i++)
  {
    struct json_object *day = json_object_array_get_idx(daily, i);

    if(day == NULL)
      continue;

    struct json_object *jdt = NULL, *jtemp_obj = NULL;
    struct json_object *jweather = NULL, *jhumid = NULL;
    struct json_object *jwind = NULL, *jwind_deg = NULL;
    struct json_object *jpop = NULL;

    json_object_object_get_ex(day, "dt", &jdt);
    json_object_object_get_ex(day, "temp", &jtemp_obj);
    json_object_object_get_ex(day, "weather", &jweather);
    json_object_object_get_ex(day, "humidity", &jhumid);
    json_object_object_get_ex(day, "wind_speed", &jwind);
    json_object_object_get_ex(day, "wind_deg", &jwind_deg);
    json_object_object_get_ex(day, "pop", &jpop);

    // Day name.
    const char *day_name = "???";

    if(jdt != NULL)
    {
      time_t dt = json_object_get_int64(jdt) + tz_offset;
      struct tm tm;

      gmtime_r(&dt, &tm);
      day_name = day_names[tm.tm_wday];
    }

    // Temperature high/low.
    double hi = 0.0, lo = 0.0;

    if(!ow_get_hilo(jtemp_obj, &hi, &lo))
    {
      hi = 0.0;
      lo = 0.0;
    }

    const char *desc  = ow_get_desc(jweather);
    int    cond_id    = ow_get_condition_id(jweather);
    int    humid      = jhumid    ? json_object_get_int(jhumid)       : 0;
    double wind       = jwind     ? json_object_get_double(jwind)     : 0.0;
    double wdir       = jwind_deg ? json_object_get_double(jwind_deg) : 0.0;
    int    pop        = jpop ? (int)(json_object_get_double(jpop) * 100) : 0;

    const char *icon  = ow_condition_icon(cond_id);
    const char *dclr  = ow_condition_color(cond_id);

    // Format colored hi/lo temperatures.
    char chi[32], clo[32];

    ow_fmt_temp(chi, sizeof(chi), hi, r->units);
    ow_fmt_temp(clo, sizeof(clo), lo, r->units);

    char desc_pad[24];

    ow_fmt_desc_pad(desc_pad, sizeof(desc_pad), desc, 22);

    char precip[24];

    ow_fmt_precip(precip, sizeof(precip), pop);

    // Icon at line start for consistent emoji width handling.
    // 🌧️ Thursday   79/49°F  light rain            60%  17mph SSW 100% precip
    char buf[OW_REPLY_SZ];

    snprintf(buf, sizeof(buf),
        "%s %-9.9s  %s/%s\xc2\xb0%s"
        "  %s%s" CLR_RESET
        "  %2d%%"
        "  %2.0f%s %-3s"
        "%s",
        icon, day_name, chi, clo, tu,
        dclr, desc_pad,
        humid,
        wind, su, ow_wind_dir(wdir),
        precip);

    ow_reply(r, buf);
  }
}

// -----------------------------------------------------------------------
// OnCall response handler: !forecast -h (hourly)
// -----------------------------------------------------------------------

// Parse hourly forecast from a OneCall response and reply.
// Uses weather emoji, color-coded temperatures, and aligned columns.
// r: request context
// root: parsed JSON root object
static void
ow_handle_forecast_hourly(ow_request_t *r, struct json_object *root)
{
  struct json_object *hourly = NULL;

  if(!json_object_object_get_ex(root, "hourly", &hourly)
      || !json_object_is_type(hourly, json_type_array))
  {
    ow_reply(r, "Error: no hourly forecast data in response");
    return;
  }

  const char *tu = ow_temp_unit(r->units);
  const char *su = ow_speed_unit(r->units);
  int n = (int)json_object_array_length(hourly);

  ow_reply_header(r, "24-hour forecast");

  static const char *day_names[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
  };

  int tz_offset = ow_get_tz_offset(root);
  int prev_wday = -1;

  // Show up to 24 hours.
  for(int i = 0; i < n && i < 24; i++)
  {
    struct json_object *hour = json_object_array_get_idx(hourly, i);

    if(hour == NULL)
      continue;

    struct json_object *jdt = NULL, *jtemp = NULL;
    struct json_object *jweather = NULL, *jhumid = NULL;
    struct json_object *jwind = NULL, *jwind_deg = NULL;
    struct json_object *jpop = NULL;

    json_object_object_get_ex(hour, "dt", &jdt);
    json_object_object_get_ex(hour, "temp", &jtemp);
    json_object_object_get_ex(hour, "weather", &jweather);
    json_object_object_get_ex(hour, "humidity", &jhumid);
    json_object_object_get_ex(hour, "wind_speed", &jwind);
    json_object_object_get_ex(hour, "wind_deg", &jwind_deg);
    json_object_object_get_ex(hour, "pop", &jpop);

    // Format local time.
    const char *day_name = "???";
    char time_str[8] = "??";

    if(jdt != NULL)
    {
      time_t dt = json_object_get_int64(jdt) + tz_offset;
      struct tm tm;

      gmtime_r(&dt, &tm);
      day_name = day_names[tm.tm_wday];

      int h = tm.tm_hour % 12;

      if(h == 0) h = 12;
      snprintf(time_str, sizeof(time_str), "%2d%s",
          h, tm.tm_hour < 12 ? "am" : "pm");

      // Insert a blank line at day boundary.
      if(prev_wday >= 0 && tm.tm_wday != prev_wday)
        ow_reply(r, " ");

      prev_wday = tm.tm_wday;
    }

    double temp    = jtemp     ? json_object_get_double(jtemp)     : 0.0;
    int    humid   = jhumid    ? json_object_get_int(jhumid)       : 0;
    double wind    = jwind     ? json_object_get_double(jwind)     : 0.0;
    double wdir    = jwind_deg ? json_object_get_double(jwind_deg) : 0.0;
    int    pop     = jpop      ? (int)(json_object_get_double(jpop) * 100) : 0;
    int    cond_id = ow_get_condition_id(jweather);

    const char *desc = ow_get_desc(jweather);
    const char *icon = ow_condition_icon(cond_id);
    const char *dclr = ow_condition_color(cond_id);

    // Format colored temperature.
    char ct[32];

    ow_fmt_temp(ct, sizeof(ct), temp, r->units);

    char desc_pad[24];

    ow_fmt_desc_pad(desc_pad, sizeof(desc_pad), desc, 20);

    char precip[24];

    ow_fmt_precip(precip, sizeof(precip), pop);

    char buf[OW_REPLY_SZ];

    snprintf(buf, sizeof(buf),
        "%s %s %s  %s\xc2\xb0%s"
        "  %s%s" CLR_RESET
        "  %2d%%"
        "  %2.0f%s %-3s"
        "%s",
        icon, day_name, time_str, ct, tu,
        dclr, desc_pad,
        humid,
        wind, su, ow_wind_dir(wdir),
        precip);

    ow_reply(r, buf);
  }
}

// -----------------------------------------------------------------------
// Step 2: submit OneCall API request
// -----------------------------------------------------------------------

// Build the OneCall URL and submit an async GET.
// r: request with lat/lon populated
static void
ow_submit_onecall(ow_request_t *r)
{
  const char *exclude;

  switch(r->type)
  {
    case OW_REQ_WEATHER:          exclude = "minutely,hourly";          break;
    case OW_REQ_FORECAST:         exclude = "minutely,hourly,current"; break;
    case OW_REQ_FORECAST_HOURLY:  exclude = "minutely,daily,current";  break;
  }

  char url[OW_URL_SZ];

  snprintf(url, sizeof(url),
      "%s?lat=%.6f&lon=%.6f&exclude=%s&units=%s&appid=%s",
      OW_ONECALL_URL, r->lat, r->lon, exclude,
      r->units, r->apikey);

  if(curl_get(url, ow_onecall_done, r) != SUCCESS)
  {
    ow_reply(r, "Error: failed to submit weather request");
    ow_req_release(r);
  }
}

// -----------------------------------------------------------------------
// Step 2 callback: OneCall API response
// -----------------------------------------------------------------------

// Handle the OneCall API response, parse JSON, dispatch to weather
// or forecast handler.
// resp: curl response
static void
ow_onecall_done(const curl_response_t *resp)
{
  ow_request_t *r = (ow_request_t *)resp->user_data;

  if(resp->curl_code != 0)
  {
    char buf[OW_REPLY_SZ];

    snprintf(buf, sizeof(buf), "Weather API error: %s", resp->error);
    ow_reply(r, buf);
    ow_req_release(r);
    return;
  }

  if(resp->status == 401)
  {
    ow_reply(r, "Error: invalid API key");
    ow_req_release(r);
    return;
  }

  if(resp->status == 429)
  {
    ow_reply(r, "Error: API rate limit exceeded, try again later");
    ow_req_release(r);
    return;
  }

  if(resp->status != 200)
  {
    char buf[OW_REPLY_SZ];

    snprintf(buf, sizeof(buf), "Weather API returned HTTP %ld",
        resp->status);
    ow_reply(r, buf);
    ow_req_release(r);
    return;
  }

  // Parse JSON.
  struct json_object *root = json_tokener_parse(resp->body);

  if(root == NULL)
  {
    ow_reply(r, "Error: malformed JSON from weather API");
    ow_req_release(r);
    return;
  }

  switch(r->type)
  {
    case OW_REQ_WEATHER:
      ow_handle_weather(r, root);
      break;

    case OW_REQ_FORECAST:
      ow_handle_forecast(r, root);
      break;

    case OW_REQ_FORECAST_HOURLY:
      ow_handle_forecast_hourly(r, root);
      break;
  }

  json_object_put(root);
  ow_req_release(r);
}

// -----------------------------------------------------------------------
// Step 1 callback: Geocoding API response
// -----------------------------------------------------------------------

// Handle the geocoding response, parse lat/lon, cache result,
// then submit the OneCall request.
// resp: curl response
static void
ow_geocode_done(const curl_response_t *resp)
{
  ow_request_t *r = (ow_request_t *)resp->user_data;

  if(resp->curl_code != 0)
  {
    char buf[OW_REPLY_SZ];

    snprintf(buf, sizeof(buf), "Geocoding error: %s", resp->error);
    ow_reply(r, buf);
    ow_req_release(r);
    return;
  }

  if(resp->status == 401)
  {
    ow_reply(r, "Error: invalid API key");
    ow_req_release(r);
    return;
  }

  if(resp->status == 404)
  {
    char buf[OW_REPLY_SZ];

    snprintf(buf, sizeof(buf), "Zipcode %s not found", r->zipcode);
    ow_reply(r, buf);
    ow_req_release(r);
    return;
  }

  if(resp->status != 200)
  {
    char buf[OW_REPLY_SZ];

    snprintf(buf, sizeof(buf), "Geocoding API returned HTTP %ld",
        resp->status);
    ow_reply(r, buf);
    ow_req_release(r);
    return;
  }

  // Parse geocoding JSON.
  struct json_object *root = json_tokener_parse(resp->body);

  if(root == NULL)
  {
    ow_reply(r, "Error: malformed JSON from geocoding API");
    ow_req_release(r);
    return;
  }

  struct json_object *jlat = NULL, *jlon = NULL, *jname = NULL;

  json_object_object_get_ex(root, "lat", &jlat);
  json_object_object_get_ex(root, "lon", &jlon);
  json_object_object_get_ex(root, "name", &jname);

  if(jlat == NULL || jlon == NULL)
  {
    ow_reply(r, "Error: geocoding response missing lat/lon");
    json_object_put(root);
    ow_req_release(r);
    return;
  }

  r->lat = json_object_get_double(jlat);
  r->lon = json_object_get_double(jlon);

  const char *name = "";

  if(jname != NULL)
    name = json_object_get_string(jname);

  snprintf(r->location_name, sizeof(r->location_name), "%s", name);

  json_object_put(root);

  // Cache the geocode result.
  pthread_mutex_lock(&ow_geo_cache_mu);
  ow_geo_insert(r->zipcode, r->lat, r->lon, r->location_name);
  pthread_mutex_unlock(&ow_geo_cache_mu);

  clam(CLAM_DEBUG2, OW_CTX, "geocode %s -> %s (%.4f, %.4f) [cached]",
      r->zipcode, r->location_name, r->lat, r->lon);

  // Step 2: query OneCall API.
  ow_submit_onecall(r);
}

// -----------------------------------------------------------------------
// Common command handler: validates args, starts geocode chain
// -----------------------------------------------------------------------

// Validate arguments, check API key, check geocode cache, and either
// skip straight to the OneCall request or start the geocode chain.
// ctx: command context (valid only for duration of this call)
// type: weather or forecast
static void
ow_cmd_common(const cmd_ctx_t *ctx, ow_req_type_t type)
{
  const char *zipcode = NULL;

  // When called from a command with an arg spec (weather), use parsed args.
  // When called from forecast (no spec), fall back to manual validation.
  if(ctx->parsed != NULL)
  {
    zipcode = ctx->parsed->argv[0];
  }
  else
  {
    if(ctx->args == NULL || ctx->args[0] == '\0')
    {
      if(type == OW_REQ_WEATHER)
        cmd_reply(ctx, "Usage: weather <zipcode>");
      else
        cmd_reply(ctx, "Usage: forecast [-h] <zipcode>");
      return;
    }

    // Validate zipcode format before any API calls.
    if(!ow_validate_zipcode(ctx->args))
    {
      if(type == OW_REQ_WEATHER)
        cmd_reply(ctx, "Invalid zipcode. Usage: weather <zipcode>");
      else
        cmd_reply(ctx, "Invalid zipcode. Usage: forecast [-h] <zipcode>");
      return;
    }

    zipcode = ctx->args;
  }

  // Check API key.
  const char *apikey = kv_get_str("plugin.openweather.apikey");

  if(apikey == NULL || apikey[0] == '\0')
  {
    cmd_reply(ctx,
        "Error: OpenWeather API key not configured. "
        "Set plugin.openweather.apikey via /set");
    return;
  }

  // Allocate request and save command context.
  ow_request_t *r = ow_req_alloc();

  r->type = type;
  snprintf(r->zipcode, sizeof(r->zipcode), "%s", zipcode);
  snprintf(r->apikey, sizeof(r->apikey), "%s", apikey);

  const char *units = kv_get_str("plugin.openweather.units");

  snprintf(r->units, sizeof(r->units), "%s",
      (units != NULL && units[0] != '\0') ? units : "imperial");

  // Deep-copy the command context.  The args and username pointers
  // reference the cmd_task_data_t which is freed after this callback
  // returns, so NULL them out (zipcode is already in r->zipcode).
  memcpy(&r->msg, ctx->msg, sizeof(r->msg));
  r->ctx.bot      = ctx->bot;
  r->ctx.msg      = &r->msg;
  r->ctx.args     = NULL;
  r->ctx.username = NULL;

  // Check geocode cache before making an API call.
  pthread_mutex_lock(&ow_geo_cache_mu);
  ow_geocache_t *cached = ow_geo_lookup(r->zipcode);

  if(cached != NULL)
  {
    r->lat = cached->lat;
    r->lon = cached->lon;
    snprintf(r->location_name, sizeof(r->location_name), "%s",
        cached->name);
    pthread_mutex_unlock(&ow_geo_cache_mu);

    clam(CLAM_DEBUG2, OW_CTX, "geocode %s -> %s (%.4f, %.4f) [cache hit]",
        r->zipcode, r->location_name, r->lat, r->lon);

    // Skip step 1, go directly to OneCall.
    ow_submit_onecall(r);
    return;
  }

  pthread_mutex_unlock(&ow_geo_cache_mu);

  // Step 1: geocode the zipcode via API.
  char url[OW_URL_SZ];

  snprintf(url, sizeof(url), "%s?zip=%s&appid=%s",
      OW_GEO_URL, r->zipcode, r->apikey);

  if(curl_get(url, ow_geocode_done, r) != SUCCESS)
  {
    cmd_reply(ctx, "Error: failed to submit geocoding request");
    ow_req_release(r);
  }
}

// -----------------------------------------------------------------------
// Command callbacks
// -----------------------------------------------------------------------

// !weather <zipcode> — current conditions.
// ctx: command context
static void
ow_cmd_weather(const cmd_ctx_t *ctx)
{
  ow_cmd_common(ctx, OW_REQ_WEATHER);
}

// !forecast [-h] <zipcode> — daily or hourly forecast.
// ctx: command context
static void
ow_cmd_forecast(const cmd_ctx_t *ctx)
{
  ow_req_type_t type = OW_REQ_FORECAST;
  const char *args = ctx->args;

  // Check for -h flag at start of args.
  if(args != NULL && strncmp(args, "-h", 2) == 0
      && (args[2] == ' ' || args[2] == '\0'))
  {
    type = OW_REQ_FORECAST_HOURLY;
    args = args + 2;

    while(*args == ' ')
      args++;

    if(*args == '\0')
      args = NULL;
  }

  cmd_ctx_t fctx = *ctx;

  fctx.args = args;
  ow_cmd_common(&fctx, type);
}

// -----------------------------------------------------------------------
// Plugin lifecycle
// -----------------------------------------------------------------------

// returns: SUCCESS or FAIL
static bool
ow_init(void)
{
  pthread_mutex_init(&ow_free_mu, NULL);
  pthread_mutex_init(&ow_geo_cache_mu, NULL);
  memset(ow_geo_cache, 0, sizeof(ow_geo_cache));

  if(cmd_register("openweather", "weather",
      "weather <zipcode>",
      "Show current weather for a US zipcode",
      "Queries the OpenWeather OneCall 3.0 API for current\n"
      "conditions at the given zipcode. Displays temperature,\n"
      "humidity, wind, sunrise/sunset, and any active alerts.\n"
      "\n"
      "Requires plugin.openweather.apikey to be set.\n"
      "Units controlled by plugin.openweather.units (imperial/metric).\n"
      "\n"
      "Example: !weather 90210",
      "everyone", 0, CMD_SCOPE_ANY, METHOD_T_ANY, ow_cmd_weather, NULL, NULL, "w",
      ow_ad_weather, 1) != SUCCESS)
    return(FAIL);

  if(cmd_register("openweather", "forecast",
      "forecast [-h] <zipcode>",
      "Show forecast for a US zipcode (daily or hourly with -h)",
      "Queries the OpenWeather OneCall 3.0 API for the forecast\n"
      "at the given zipcode.\n"
      "\n"
      "  !forecast <zipcode>      7-day daily forecast\n"
      "  !forecast -h <zipcode>   24-hour hourly forecast\n"
      "\n"
      "Requires plugin.openweather.apikey to be set.\n"
      "Units controlled by plugin.openweather.units (imperial/metric).\n"
      "\n"
      "Example: !forecast 10001\n"
      "         !forecast -h 90210",
      "everyone", 0, CMD_SCOPE_ANY, METHOD_T_ANY, ow_cmd_forecast, NULL, NULL, "f",
      NULL, 0) != SUCCESS)
  {
    cmd_unregister("weather");
    return(FAIL);
  }

  clam(CLAM_INFO, OW_CTX, "openweather plugin initialized");

  return(SUCCESS);
}

// Tear down the openweather plugin. Unregisters commands, frees the
// request freelist and geocode cache, and destroys mutexes.
static void
ow_deinit(void)
{
  cmd_unregister("forecast");
  cmd_unregister("weather");

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

  // Free geocode cache.
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

  pthread_mutex_destroy(&ow_geo_cache_mu);

  clam(CLAM_INFO, OW_CTX, "openweather plugin deinitialized");
}

// -----------------------------------------------------------------------
// Plugin descriptor
// -----------------------------------------------------------------------

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "openweather",
  .version         = "1.0",
  .type            = PLUGIN_SERVICE,
  .kind            = "openweather",
  .provides        = { { .name = "service_openweather" } },
  .provides_count  = 1,
  .requires        = { { .name = "bot_command" } },
  .requires_count  = 1,
  .kv_schema       = ow_kv_schema,
  .kv_schema_count = 3,
  .init            = ow_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = ow_deinit,
  .ext             = NULL,
};
