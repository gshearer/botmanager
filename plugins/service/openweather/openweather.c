// botmanager — MIT
// OpenWeather service plugin: One Call 4.0 fetch + geocode cache.
#define OW_INTERNAL
#include "openweather.h"
#include "util.h"

#include <ctype.h>
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

// Compose a human-facing place label from geocoder fields into `out`:
//   "<name>, <state>, <country>"
// The state is appended when present and not a duplicate of the name
// (so "Singapore, Singapore" collapses to "Singapore"); the country is
// appended only when it is not the US — a US state already
// disambiguates, and the bare city reads cleaner for the common case.
// `out` must be a distinct buffer from `name`.
static void
ow_compose_place(const char *name, const char *state, const char *country,
    char *out, size_t out_sz)
{
  size_t len;

  if(out_sz == 0)
    return;

  snprintf(out, out_sz, "%s", (name != NULL) ? name : "");

  if(state != NULL && state[0] != '\0' && strcasecmp(state, out) != 0)
  {
    len = strlen(out);

    if(len < out_sz)
      snprintf(out + len, out_sz - len, ", %s", state);
  }

  if(country != NULL && country[0] != '\0' && strcasecmp(country, "US") != 0)
  {
    len = strlen(out);

    if(len < out_sz)
      snprintf(out + len, out_sz - len, ", %s", country);
  }
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
  pthread_mutex_lock(&ow_active_mutex);
  ow_req_untrack_locked(r);
  pthread_mutex_unlock(&ow_active_mutex);

  pthread_mutex_lock(&ow_free_mu);
  r->next = ow_free;
  ow_free = r;
  pthread_mutex_unlock(&ow_free_mu);
}

// ----------------------------------------------------------------------
// The in-flight registry — every fetch borrows a foreign callback
// ----------------------------------------------------------------------

// Each openweather_fetch_* stores its caller's completion in a request
// of ours and hands core's curl layer one of our own ow_*_done functions
// instead. That is one indirection more than core can see: plugin_quiesce
// and plugin_audit both range-test curl_iter_req_t.cb, which for our
// transfers names THIS mapping, never the caller's. The only caller is
// the `weather` feature plugin, in a mapping of its own — reload it with
// a fetch airborne and the stored pointer aims into freed .text.
//
// The exposure here is wider than one HTTP round trip. A request is a
// CHAIN — geocode, then the One Call datatype, then hi/lo, then one GET
// per weather alert — and the caller's callback sits in it, unread, for
// the whole sequence. So the registry brackets the request's entire
// lifetime rather than a single transfer: filed the moment the caller's
// callback is installed, dropped by ow_req_release, which every terminal
// path already goes through.
//
// A request whose caller left still runs its chain to the end; it simply
// delivers to nobody. The mechanics are commented in full in
// reachyapi.c and written up in PLUGIN.md.
//
// The caller's `user` is dropped with the callback and whatever it
// points at is leaked. Nothing else is possible: only the caller knows
// how to free its own context, and the caller is precisely what is no
// longer there. A bounded leak on an operator action beats a SIGSEGV.

static void
ow_req_track(ow_request_t *r)
{
  pthread_mutex_lock(&ow_active_mutex);

  r->next_active = ow_active_head;
  ow_active_head = r;
  ow_active_count++;

  pthread_mutex_unlock(&ow_active_mutex);
}

// Caller holds ow_active_mutex. A no-op for a request that is not on the
// list, which is what makes release-after-delivery safe.
static void
ow_req_untrack_locked(ow_request_t *r)
{
  ow_request_t **pp;

  for(pp = &ow_active_head; *pp != NULL; pp = &(*pp)->next_active)
  {
    if(*pp != r)
      continue;

    *pp = r->next_active;
    r->next_active = NULL;
    ow_active_count--;
    return;
  }
}

// Lift the caller's half off `r` and unlink it, both under the registry
// lock. The read has to happen in the same critical section the sweep
// would null it in — read it afterwards and the two interleave, which is
// the whole bug. Clearing the arms as we go also makes a second delivery
// on the same request structurally impossible.
static void
ow_req_take_caller(ow_request_t *r, ow_caller_t *out)
{
  pthread_mutex_lock(&ow_active_mutex);

  ow_req_untrack_locked(r);

  out->cb   = r->cb;
  out->user = r->user;

  memset(&r->cb, 0, sizeof(r->cb));
  r->user = NULL;

  pthread_mutex_unlock(&ow_active_mutex);
}

// A mapping is going away (core is between the plugin's deinit() and its
// residual audit, so nothing of it runs any more). Drop every callback
// that lives inside it.
//
// Residual race: a chain that has already taken its caller half holds it
// on the stack and is a few instructions from calling it. The window is
// bounded above by the quiescence poll plus the audit that follow this
// broadcast, and below by two stores — against an operator-timescale
// unload. Closing it would need core to wait on a lock a curl worker
// holds.
static void
ow_unmap_cb(uintptr_t lo, uintptr_t hi, void *data)
{
  uint32_t orphaned = 0;

  (void)data;

  pthread_mutex_lock(&ow_active_mutex);

  for(ow_request_t *r = ow_active_head; r != NULL; r = r->next_active)
  {
    uintptr_t cb = r->type == OW_REQ_WEATHER
        ? (uintptr_t)fn_addr(&r->cb.current)
        : (uintptr_t)fn_addr(&r->cb.forecast);

    if(cb == 0 || cb < lo || cb >= hi)
      continue;

    // memset rather than one arm's NULL: the arms are a union, and
    // all-bits-zero is the null test every delivery path makes.
    memset(&r->cb, 0, sizeof(r->cb));
    r->user = NULL;
    orphaned++;
  }

  pthread_mutex_unlock(&ow_active_mutex);

  if(orphaned > 0)
    clam(CLAM_WARN, OW_CTX, "%u weather request(s) lost their caller to "
        "an unload; they will complete and deliver nothing", orphaned);
}

// ----------------------------------------------------------------------
// Callback delivery helpers
// ----------------------------------------------------------------------

// Each takes the caller's half off the request first, so what runs is a
// stack copy that is either the caller's or NULL because the caller was
// unloaded mid-chain.

static void
ow_deliver_current_err(ow_request_t *r, const char *msg)
{
  openweather_current_result_t res;
  ow_caller_t                  c;

  ow_req_take_caller(r, &c);

  memset(&res, 0, sizeof(res));
  snprintf(res.err, sizeof(res.err), "%s", msg);

  if(c.cb.current != NULL)
    c.cb.current(&res, c.user);
}

static void
ow_deliver_forecast_err(ow_request_t *r, const char *msg)
{
  openweather_forecast_result_t res;
  ow_caller_t                   c;

  ow_req_take_caller(r, &c);

  memset(&res, 0, sizeof(res));
  snprintf(res.err, sizeof(res.err), "%s", msg);

  if(c.cb.forecast != NULL)
    c.cb.forecast(&res, c.user);
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

// Copy the alert-id URN strings from an "alerts" JSON array — One Call
// 4.0 returns bare ids here, not fleshed-out objects — into the
// request's enrichment worklist for later per-id resolution.
static void
ow_collect_alert_ids(struct json_object *jalerts, ow_request_t *r)
{
  int n;
  int i;

  r->alert_id_count = 0;
  r->alert_idx      = 0;

  // The caller holds a better alert set than anything we could build
  // out of these ids. With none collected, ow_start_alert_enrich falls
  // straight through to delivery — no enrichment GETs at all.
  if(r->alerts_mode == OPENWEATHER_ALERTS_SKIP)
    return;

  if(jalerts == NULL || !json_object_is_type(jalerts, json_type_array))
    return;

  n = (int)json_object_array_length(jalerts);

  for(i = 0; i < n && r->alert_id_count < OPENWEATHER_ALERT_MAX; i++)
  {
    struct json_object *e = json_object_array_get_idx(jalerts, i);
    const char *s;

    if(e == NULL)
      continue;

    s = json_object_get_string(e);

    if(s == NULL || s[0] == '\0')
      continue;

    snprintf(r->alert_ids[r->alert_id_count], OW_ALERT_ID_SZ, "%s", s);
    r->alert_id_count++;
  }
}

// Pick the English body out of an alert's `description` array. Most
// agencies publish one entry; the docs warn that some issue only a
// local language, so an unmatched language still yields the first entry
// rather than nothing.
static struct json_object *
ow_alert_desc_en(struct json_object *root)
{
  struct json_object *descs = json_get_array(root, "description");
  int n;
  int i;

  if(descs == NULL)
    return(NULL);

  n = (int)json_object_array_length(descs);

  for(i = 0; i < n; i++)
  {
    struct json_object *d = json_object_array_get_idx(descs, i);
    char lang[16];

    if(d == NULL)
      continue;

    lang[0] = '\0';
    json_get_str(d, "language", lang, sizeof(lang));

    if(strncasecmp(lang, "en", 2) == 0)
      return(d);
  }

  return(n > 0 ? json_object_array_get_idx(descs, 0) : NULL);
}

// The nouns that end a weather product's name. A match is what anchors
// the scan below; everything before it is the qualifier run.
static bool
ow_is_product_word(const char *w, size_t len)
{
  static const char *const products[] = {
    "warning", "watch", "advisory", "statement", "emergency", NULL
  };
  int i;

  for(i = 0; products[i] != NULL; i++)
    if(strlen(products[i]) == len && strncasecmp(w, products[i], len) == 0)
      return(true);

  return(false);
}

// Normalize an ALL-CAPS product name to title case. Bulletins shout
// their continuation headers ("SEVERE THUNDERSTORM WATCH 552 REMAINS
// VALID…") while issuance lines are already mixed case, so a string
// carrying any lowercase letter is left in the issuer's own casing.
static void
ow_title_case(char *s)
{
  bool at_word = true;
  char *p;

  for(p = s; *p != '\0'; p++)
    if(islower((unsigned char)*p))
      return;

  for(p = s; *p != '\0'; p++)
  {
    if(isalpha((unsigned char)*p))
    {
      *p      = (char)(at_word ? toupper((unsigned char)*p)
                               : tolower((unsigned char)*p));
      at_word = false;
    }

    else
      at_word = true;
  }
}

// Mine the product name out of an advisory body: scan for a product
// keyword and claim the run of capitalized words immediately before it,
// bounded to one line. This lifts "Flash Flood Warning" out of
//
//   FFWPBZ
//
//   The National Weather Service in Pittsburgh has issued a
//
//   * Flash Flood Warning for...
//
// and "SEVERE THUNDERSTORM WATCH" out of a shouted continuation header,
// where the old first-line heuristic yielded the WMO product code and a
// truncated sentence respectively. Returns false when the body names no
// product — common on the follow-up statements that carry only radar
// narrative — which hands the label to the tag fallback.
static bool
ow_alert_product_name(const char *text, char *out, size_t out_sz)
{
  ow_word_t   ring[OW_ALERT_NAME_WORDS];
  const char *p    = text;
  uint8_t     held = 0;

  out[0] = '\0';

  while(*p != '\0')
  {
    const char *w;
    size_t      len;
    size_t      span;
    uint8_t     first;

    // A name never spans lines: forget the qualifier run at each break.
    if(*p == '\n' || *p == '\r')
    {
      held = 0;
      p++;
      continue;
    }

    if(!isalpha((unsigned char)*p))
    {
      p++;
      continue;
    }

    w = p;

    while(isalpha((unsigned char)*p))
      p++;

    len = (size_t)(p - w);

    if(!ow_is_product_word(w, len))
    {
      if(held == OW_ALERT_NAME_WORDS)
      {
        memmove(&ring[0], &ring[1], sizeof(ring) - sizeof(ring[0]));
        held--;
      }

      ring[held].start    = w;
      ring[held].namelike = isupper((unsigned char)w[0]) != 0;
      held++;
      continue;
    }

    first = held;

    while(first > 0 && ring[first - 1].namelike)
      first--;

    // A bare "Warning" names nothing — keep scanning for a qualified one.
    if(first == held)
    {
      held = 0;
      continue;
    }

    // The name is contiguous in the source, so copy the span rather than
    // rejoining words: the issuer's own spacing survives intact.
    span = (size_t)((w + len) - ring[first].start);

    if(span >= out_sz)
      span = out_sz - 1;

    memcpy(out, ring[first].start, span);
    out[span] = '\0';
    ow_title_case(out);

    return(true);
  }

  return(false);
}

// Last resort before the issuer name: 4.0 ships an undocumented `tags`
// array ("Thunderstorm", "Flood", "Other dangers") on bulletins whose
// body is pure narrative. Coarse, but never garbage — and qualified with
// the issuer it reads as a real advisory rather than a bare noun.
static bool
ow_alert_tag_label(struct json_object *root, const char *sender, char *out,
    size_t out_sz)
{
  struct json_object *tags = json_get_array(root, "tags");
  struct json_object *t0;
  const char *s;

  if(tags == NULL || json_object_array_length(tags) == 0)
    return(false);

  t0 = json_object_array_get_idx(tags, 0);
  s  = (t0 != NULL) ? json_object_get_string(t0) : NULL;

  if(s == NULL || s[0] == '\0')
    return(false);

  if(sender[0] != '\0')
    snprintf(out, out_sz, "%s (%s)", s, sender);

  else
    snprintf(out, out_sz, "%s", s);

  return(true);
}

// Distil one /alert/{id} document into a single human-facing label, best
// source first. `event` is the issuer's own name for the product and is
// what non-US agencies populate; across every US NWS bulletin sampled it
// was empty, so the body and tags carry the real work.
static void
ow_alert_label(struct json_object *root, char *out, size_t out_sz)
{
  char text[OW_ALERT_SCAN_SZ];
  char sender[96];
  struct json_object *desc;

  out[0]    = '\0';
  sender[0] = '\0';

  json_get_str(root, "sender_name", sender, sizeof(sender));

  if(json_get_str(root, "event", out, out_sz) && out[0] != '\0')
    return;

  out[0] = '\0';
  desc   = ow_alert_desc_en(root);

  if(desc != NULL)
  {
    text[0] = '\0';
    json_get_str(desc, "description", text, sizeof(text));

    if(ow_alert_product_name(text, out, out_sz))
      return;
  }

  if(ow_alert_tag_label(root, sender, out, out_sz))
    return;

  if(sender[0] != '\0')
    snprintf(out, out_sz, "%s advisory", sender);
}

// One storm produces a burst of bulletins — an issuance plus a radar
// update every ten minutes — that all distil to the same label. Without
// this the user reads "Severe Thunderstorm Warning" four times and the
// three-line display cap hides everything else that is happening.
static bool
ow_label_seen(const openweather_alert_set_t *set, const char *label)
{
  uint8_t i;

  for(i = 0; i < set->count; i++)
    if(strcasecmp(set->alerts[i].event, label) == 0)
      return(true);

  return(false);
}

// Synthesize an OpenWeather condition code + description from the
// coverage fields the daily timeline *does* carry. 4.0's daily rows omit
// the `weather` array entirely, so without this every day renders as
// "unknown"; mapping cloud cover (and any precipitation) onto the
// canonical id keeps the command-side icon/color tables working untouched.
static void
ow_synth_condition(int clouds, bool has_rain, bool has_snow,
    int32_t *id_out, char *desc, size_t desc_sz)
{
  int32_t id;
  const char *text;

  if(has_snow)          { id = 601; text = "snow";             }
  else if(has_rain)     { id = 501; text = "rain";             }
  else if(clouds >= 85) { id = 804; text = "overcast clouds";  }
  else if(clouds >= 51) { id = 803; text = "broken clouds";    }
  else if(clouds >= 25) { id = 802; text = "scattered clouds"; }
  else if(clouds >= 11) { id = 801; text = "few clouds";       }
  else                  { id = 800; text = "clear sky";        }

  *id_out = id;
  snprintf(desc, desc_sz, "%s", text);
}

// Pull the OpenWeather "message" field out of an error body so the user
// sees the API's own explanation (e.g. the One Call subscription notice)
// instead of a guessed-at generic string. `fallback` may be NULL.
static void
ow_api_message(const curl_response_t *resp, char *out, size_t out_sz,
    const char *fallback)
{
  struct json_object *root;
  char msg[192];

  if(fallback != NULL)
    snprintf(out, out_sz, "%s", fallback);
  else
    out[0] = '\0';

  if(resp->body == NULL || resp->body_len == 0)
    return;

  root = json_parse_buf(resp->body, resp->body_len, OW_CTX);

  if(root == NULL)
    return;

  msg[0] = '\0';

  if(json_get_str(root, "message", msg, sizeof(msg)) && msg[0] != '\0')
    snprintf(out, out_sz, "Error: %s", msg);

  json_object_put(root);
}

// Classify a completed transfer. Returns true when the body is a usable
// 2xx payload; otherwise fills errbuf with a forward-ready message.
static bool
ow_http_ok(const curl_response_t *resp, char *errbuf, size_t sz)
{
  if(resp->curl_code != 0)
  {
    snprintf(errbuf, sz, "Weather API error: %s",
        resp->error != NULL ? resp->error : "transport failure");
    return(false);
  }

  if(resp->status == 401)
  {
    ow_api_message(resp, errbuf, sz,
        "Error: OpenWeather rejected the request (HTTP 401) — check the "
        "API key and that the One Call subscription is active");
    return(false);
  }

  if(resp->status == 429)
  {
    snprintf(errbuf, sz, "Error: API rate limit exceeded, try again later");
    return(false);
  }

  if(resp->status != 200)
  {
    ow_api_message(resp, errbuf, sz, NULL);

    if(errbuf[0] == '\0')
      snprintf(errbuf, sz, "Weather API returned HTTP %ld", resp->status);

    return(false);
  }

  if(resp->body == NULL)
  {
    snprintf(errbuf, sz, "Error: empty response from weather API");
    return(false);
  }

  return(true);
}

// Parse the One Call 4.0 /current document (the payload lives in
// data[0]) into the request accumulator. Alert ids are lifted for later
// enrichment; hi/lo is filled by a follow-up daily call. Returns false
// when the response carries no usable current-conditions object.
static bool
ow_parse_current(ow_request_t *r, struct json_object *root)
{
  openweather_current_result_t *out = &r->acc.current;
  struct json_object *data = json_get_array(root, "data");
  struct json_object *d0;
  int64_t sunrise_ts = 0;
  int64_t sunset_ts = 0;
  double temp = 0.0;
  double feels = 0.0;
  double wind = 0.0;
  double wind_d = 0.0;
  int32_t humidity = 0;

  memset(out, 0, sizeof(*out));

  if(data == NULL || json_object_array_length(data) == 0)
    return(false);

  d0 = json_object_array_get_idx(data, 0);

  if(d0 == NULL)
    return(false);

  json_get_double(d0, "temp",       &temp);
  json_get_double(d0, "feels_like", &feels);
  json_get_int   (d0, "humidity",   &humidity);
  json_get_double(d0, "wind_speed", &wind);
  json_get_double(d0, "wind_deg",   &wind_d);
  json_get_int64 (d0, "sunrise",    &sunrise_ts);
  json_get_int64 (d0, "sunset",     &sunset_ts);

  out->current.temp       = temp;
  out->current.feels_like = feels;
  out->current.wind_speed = wind;
  out->current.wind_deg   = wind_d;
  out->current.humidity   = humidity;
  out->current.sunrise    = (time_t)sunrise_ts;
  out->current.sunset     = (time_t)sunset_ts;
  out->current.tz_offset  = ow_get_tz_offset(root);
  out->current.have_hilo  = false;   // filled by ow_daily_hilo_done

  ow_fill_desc(json_get_array(d0, "weather"), out->current.condition_desc,
      sizeof(out->current.condition_desc), &out->current.condition_id);

  snprintf(out->current.place_name, sizeof(out->current.place_name),
      "%s", r->location_name);
  snprintf(out->current.zipcode, sizeof(out->current.zipcode),
      "%s", r->zipcode);
  snprintf(out->current.units, sizeof(out->current.units),
      "%s", r->units);

  ow_collect_alert_ids(json_get_array(d0, "alerts"), r);

  return(true);
}

// Parse a One Call 4.0 daily timeline (…/timeline/1day) into the
// accumulator. The rows carry temp.max/min but no `weather` array and no
// `pop`, so the condition is synthesized from cloud cover + rain/snow
// and precipitation probability is left unset. Returns false if no day
// rows were produced.
static bool
ow_parse_daily(ow_request_t *r, struct json_object *root)
{
  openweather_forecast_result_t *out = &r->acc.forecast;
  struct json_object *data = json_get_array(root, "data");
  struct json_object *d0;
  int n;
  int i;

  memset(out, 0, sizeof(*out));

  if(data == NULL)
    return(false);

  out->forecast.tz_offset = ow_get_tz_offset(root);
  snprintf(out->forecast.place_name, sizeof(out->forecast.place_name),
      "%s", r->location_name);
  snprintf(out->forecast.zipcode, sizeof(out->forecast.zipcode),
      "%s", r->zipcode);
  snprintf(out->forecast.units, sizeof(out->forecast.units),
      "%s", r->units);

  n = (int)json_object_array_length(data);

  for(i = 0; i < n && out->forecast.day_count < OPENWEATHER_FCAST_DAYS; i++)
  {
    int64_t dt_ts = 0;
    double wind = 0.0;
    double wdir = 0.0;
    double rain = 0.0;
    double snow = 0.0;
    int32_t humid = 0;
    int32_t clouds = 0;
    double hi = 0.0;
    double lo = 0.0;
    bool has_rain;
    bool has_snow;
    openweather_forecast_day_t *slot;
    struct json_object *day = json_object_array_get_idx(data, i);
    struct json_object *jtemp_obj;

    if(day == NULL)
      continue;

    json_get_int64 (day, "dt",         &dt_ts);
    json_get_int   (day, "humidity",   &humid);
    json_get_int   (day, "clouds",     &clouds);
    json_get_double(day, "wind_speed", &wind);
    json_get_double(day, "wind_deg",   &wdir);

    has_rain = json_get_double(day, "rain", &rain) && rain > 0.0;
    has_snow = json_get_double(day, "snow", &snow) && snow > 0.0;

    jtemp_obj = json_get_obj(day, "temp");

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
    slot->pop        = 0.0;   // 4.0 daily carries rain (mm), not pop
    slot->humidity   = humid;

    ow_synth_condition((int)clouds, has_rain, has_snow,
        &slot->condition_id, slot->condition_desc,
        sizeof(slot->condition_desc));

    out->forecast.day_count++;
  }

  d0 = (n > 0) ? json_object_array_get_idx(data, 0) : NULL;
  ow_collect_alert_ids(d0 != NULL ? json_get_array(d0, "alerts") : NULL, r);

  return(out->forecast.day_count > 0);
}

// Parse a One Call 4.0 hourly timeline (…/timeline/1h) into the
// accumulator. Hourly rows keep the full `weather` array and `pop`, so
// they map straight across. Returns false if no hour rows were produced.
static bool
ow_parse_hourly(ow_request_t *r, struct json_object *root)
{
  openweather_forecast_result_t *out = &r->acc.forecast;
  struct json_object *data = json_get_array(root, "data");
  struct json_object *d0;
  int n;
  int i;

  memset(out, 0, sizeof(*out));

  if(data == NULL)
    return(false);

  out->forecast.tz_offset = ow_get_tz_offset(root);
  snprintf(out->forecast.place_name, sizeof(out->forecast.place_name),
      "%s", r->location_name);
  snprintf(out->forecast.zipcode, sizeof(out->forecast.zipcode),
      "%s", r->zipcode);
  snprintf(out->forecast.units, sizeof(out->forecast.units),
      "%s", r->units);

  n = (int)json_object_array_length(data);

  for(i = 0; i < n && out->forecast.hour_count < OPENWEATHER_FCAST_HOURS; i++)
  {
    int64_t dt_ts = 0;
    double temp = 0.0;
    double wind = 0.0;
    double wdir = 0.0;
    double pop = 0.0;
    int32_t humid = 0;
    openweather_forecast_hour_t *slot;
    struct json_object *hour = json_object_array_get_idx(data, i);

    if(hour == NULL)
      continue;

    json_get_int64 (hour, "dt",         &dt_ts);
    json_get_double(hour, "temp",       &temp);
    json_get_int   (hour, "humidity",   &humid);
    json_get_double(hour, "wind_speed", &wind);
    json_get_double(hour, "wind_deg",   &wdir);
    json_get_double(hour, "pop",        &pop);

    slot = &out->forecast.hours[out->forecast.hour_count];

    slot->dt         = (time_t)dt_ts;
    slot->temp       = temp;
    slot->wind_speed = wind;
    slot->wind_deg   = wdir;
    slot->pop        = pop;
    slot->humidity   = humid;

    ow_fill_desc(json_get_array(hour, "weather"), slot->condition_desc,
        sizeof(slot->condition_desc), &slot->condition_id);

    out->forecast.hour_count++;
  }

  d0 = (n > 0) ? json_object_array_get_idx(data, 0) : NULL;
  ow_collect_alert_ids(d0 != NULL ? json_get_array(d0, "alerts") : NULL, r);

  return(out->forecast.hour_count > 0);
}

// One Call 4.0 request chain
//
// A command fans out into a short, strictly-sequential chain of GETs:
// the primary datatype first, then one call per active alert to resolve
// its label, and finally delivery. Exactly one transfer is ever
// outstanding per request, so the accumulator in ow_request_t needs no
// locking — each callback either schedules the next leg or delivers.

static void
ow_submit_primary(ow_request_t *r)
{
  switch(r->type)
  {
    case OW_REQ_WEATHER:          ow_submit_current(r); break;
    case OW_REQ_FORECAST_DAILY:   ow_submit_daily(r);   break;
    case OW_REQ_FORECAST_HOURLY:  ow_submit_hourly(r);  break;
  }
}

// Leg 1a: current conditions.

static void
ow_submit_current(ow_request_t *r)
{
  char url[OW_URL_SZ];

  snprintf(url, sizeof(url),
      "%s?lat=%.6f&lon=%.6f&units=%s&appid=%s",
      OW_ONECALL_CURRENT_URL, r->lat, r->lon, r->units, r->apikey);

  if(curl_get(url, ow_current_done, r) != SUCCESS)
  {
    ow_deliver_current_err(r, "Error: failed to submit weather request");
    ow_req_release(r);
  }
}

static void
ow_current_done(const curl_response_t *resp)
{
  ow_request_t *r = (ow_request_t *)resp->user_data;
  struct json_object *root;
  char errbuf[192];
  bool ok;

  if(!ow_http_ok(resp, errbuf, sizeof(errbuf)))
  {
    ow_deliver_current_err(r, errbuf);
    ow_req_release(r);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, OW_CTX);

  if(root == NULL)
  {
    ow_deliver_current_err(r, "Error: malformed JSON from weather API");
    ow_req_release(r);
    return;
  }

  ok = ow_parse_current(r, root);
  json_object_put(root);

  if(!ok)
  {
    ow_deliver_current_err(r, "Error: no current weather data in response");
    ow_req_release(r);
    return;
  }

  // Enrich: resolve any active alerts, then deliver. The current view
  // deliberately does not chain a daily call for hi/lo — 4.0's daily
  // timeline is cursor-paginated and returns inconsistent (often empty)
  // results for the current day, which would make hi/lo flicker between
  // identical queries and add unpredictable latency to /weather. Hi/lo
  // stays available through /forecast, which needs the daily feed anyway.
  ow_start_alert_enrich(r);
}

// Leg 1 (forecast): daily timeline.

static void
ow_submit_daily(ow_request_t *r)
{
  char url[OW_URL_SZ];

  snprintf(url, sizeof(url),
      "%s/1day?lat=%.6f&lon=%.6f&cnt=%d&units=%s&appid=%s",
      OW_ONECALL_TIMELINE_URL, r->lat, r->lon,
      OW_FCAST_DAILY_CNT, r->units, r->apikey);

  if(curl_get(url, ow_daily_done, r) != SUCCESS)
  {
    ow_deliver_forecast_err(r, "Error: failed to submit forecast request");
    ow_req_release(r);
  }
}

static void
ow_daily_done(const curl_response_t *resp)
{
  ow_request_t *r = (ow_request_t *)resp->user_data;
  struct json_object *root;
  char errbuf[192];
  bool ok;

  if(!ow_http_ok(resp, errbuf, sizeof(errbuf)))
  {
    ow_deliver_forecast_err(r, errbuf);
    ow_req_release(r);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, OW_CTX);

  if(root == NULL)
  {
    ow_deliver_forecast_err(r, "Error: malformed JSON from weather API");
    ow_req_release(r);
    return;
  }

  ok = ow_parse_daily(r, root);
  json_object_put(root);

  if(!ok)
  {
    // 4.0's daily timeline periodically answers 200 with an empty data
    // array (its aggregation lags the current/hourly feeds). Steer the
    // user to the hourly view rather than surfacing a bare error.
    ow_deliver_forecast_err(r,
        "Daily forecast is momentarily unavailable upstream. "
        "Try the hourly view: weather -h <zipcode>");
    ow_req_release(r);
    return;
  }

  ow_start_alert_enrich(r);
}

// Leg 1 (forecast): hourly timeline.

static void
ow_submit_hourly(ow_request_t *r)
{
  char url[OW_URL_SZ];

  snprintf(url, sizeof(url),
      "%s/1h?lat=%.6f&lon=%.6f&cnt=%d&units=%s&appid=%s",
      OW_ONECALL_TIMELINE_URL, r->lat, r->lon,
      OW_FCAST_HOURLY_CNT, r->units, r->apikey);

  if(curl_get(url, ow_hourly_done, r) != SUCCESS)
  {
    ow_deliver_forecast_err(r, "Error: failed to submit forecast request");
    ow_req_release(r);
  }
}

static void
ow_hourly_done(const curl_response_t *resp)
{
  ow_request_t *r = (ow_request_t *)resp->user_data;
  struct json_object *root;
  char errbuf[192];
  bool ok;

  if(!ow_http_ok(resp, errbuf, sizeof(errbuf)))
  {
    ow_deliver_forecast_err(r, errbuf);
    ow_req_release(r);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, OW_CTX);

  if(root == NULL)
  {
    ow_deliver_forecast_err(r, "Error: malformed JSON from weather API");
    ow_req_release(r);
    return;
  }

  ok = ow_parse_hourly(r, root);
  json_object_put(root);

  if(!ok)
  {
    ow_deliver_forecast_err(r, "Error: no hourly forecast data in response");
    ow_req_release(r);
    return;
  }

  ow_start_alert_enrich(r);
}

// Leg 2: resolve each active alert id to a human label, one GET at a
// time. Also best-effort: a failed lookup is skipped, never fatal.

static void
ow_start_alert_enrich(ow_request_t *r)
{
  r->alert_idx = 0;
  ow_submit_next_alert(r);
}

static void
ow_submit_next_alert(ow_request_t *r)
{
  char url[OW_ALERT_URL_SZ];
  char enc[OW_ALERT_ID_SZ * 3 + 1];

  if(r->alert_idx >= r->alert_id_count)
  {
    ow_deliver_final(r);
    return;
  }

  ow_url_escape(r->alert_ids[r->alert_idx], enc, sizeof(enc));

  snprintf(url, sizeof(url), "%s/%s?appid=%s",
      OW_ONECALL_ALERT_URL, enc, r->apikey);

  if(curl_get(url, ow_alert_done, r) != SUCCESS)
  {
    // Skip this id and continue; recursion depth is bounded by
    // OPENWEATHER_ALERT_MAX.
    r->alert_idx++;
    ow_submit_next_alert(r);
  }
}

static void
ow_alert_done(const curl_response_t *resp)
{
  ow_request_t *r = (ow_request_t *)resp->user_data;
  openweather_alert_set_t *set = (r->type == OW_REQ_WEATHER)
      ? &r->acc.current.alerts
      : &r->acc.forecast.alerts;

  if(resp->curl_code == 0 && resp->status == 200 && resp->body != NULL)
  {
    struct json_object *root = json_parse_buf(resp->body, resp->body_len,
        OW_CTX);

    if(root != NULL)
    {
      char label[OPENWEATHER_ALERT_SZ];

      ow_alert_label(root, label, sizeof(label));

      if(label[0] != '\0' && set->count < OPENWEATHER_ALERT_MAX
          && !ow_label_seen(set, label))
      {
        snprintf(set->alerts[set->count].event,
            sizeof(set->alerts[set->count].event), "%s", label);
        set->count++;
      }

      json_object_put(root);
    }
  }

  r->alert_idx++;
  ow_submit_next_alert(r);
}

// Leg 3: hand the fully-assembled accumulator to the caller's callback.

static void
ow_deliver_final(ow_request_t *r)
{
  ow_caller_t c;

  ow_req_take_caller(r, &c);

  if(r->type == OW_REQ_WEATHER)
  {
    if(c.cb.current != NULL)
      c.cb.current(&r->acc.current, c.user);
  }

  else
  {
    if(c.cb.forecast != NULL)
      c.cb.forecast(&r->acc.forecast, c.user);
  }

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

  {
    char zname[OW_NAME_SZ];
    char zcountry[OW_NAME_SZ];

    zname[0] = '\0';
    zcountry[0] = '\0';
    json_get_str(root, "name", zname, sizeof(zname));
    json_get_str(root, "country", zcountry, sizeof(zcountry));

    // The zip geocoder carries no state field, so only the country
    // (when non-US) can enrich the label here.
    ow_compose_place(zname, NULL, zcountry,
        r->location_name, sizeof(r->location_name));
  }

  json_object_put(root);

  pthread_mutex_lock(&ow_geo_cache_mu);
  ow_geo_insert(r->zipcode, r->lat, r->lon, r->location_name);
  pthread_mutex_unlock(&ow_geo_cache_mu);

  clam(CLAM_DEBUG2, OW_CTX, "geocode %s -> %s (%.4f, %.4f) [cached]",
      r->zipcode, r->location_name, r->lat, r->lon);

  ow_submit_primary(r);
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

  apikey = kv_get_creds("plugin.openweather.creds.apikey");

  if(apikey == NULL || apikey[0] == '\0')
  {
    snprintf(errbuf, errsz,
        "Error: OpenWeather API key not configured. "
        "Set plugin.openweather.creds.apikey via /set");
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
    openweather_alerts_t alerts,
    openweather_done_current_cb_t done_cb, void *user)
{
  char errbuf[128];
  ow_prep_result_t pr;
  ow_request_t *r;

  if(done_cb == NULL)
    return(FAIL);

  r = ow_req_alloc();
  r->type        = OW_REQ_WEATHER;
  r->alerts_mode = alerts;
  r->cb.current  = done_cb;
  r->user        = user;

  // File it before anything can be submitted, never after: a completion
  // can run on a curl worker before the submitting call has returned.
  ow_req_track(r);

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
    ow_submit_primary(r);
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
    openweather_alerts_t alerts,
    openweather_done_forecast_cb_t done_cb, void *user)
{
  char errbuf[128];
  ow_prep_result_t pr;
  ow_request_t *r;

  if(done_cb == NULL)
    return(FAIL);

  r = ow_req_alloc();
  r->type        = type;
  r->alerts_mode = alerts;
  r->cb.forecast = done_cb;
  r->user        = user;

  ow_req_track(r);

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
    ow_submit_primary(r);
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
    openweather_alerts_t alerts,
    openweather_done_forecast_cb_t done_cb, void *user)
{
  return(ow_fetch_forecast_common(OW_REQ_FORECAST_DAILY,
      zipcode, alerts, done_cb, user));
}

bool
openweather_fetch_forecast_hourly(const char *zipcode,
    openweather_alerts_t alerts,
    openweather_done_forecast_cb_t done_cb, void *user)
{
  return(ow_fetch_forecast_common(OW_REQ_FORECAST_HOURLY,
      zipcode, alerts, done_cb, user));
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
    char *state_out, size_t state_sz,
    char *country_out, size_t country_sz,
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

  if(state_out != NULL && state_sz > 0)
  {
    state_out[0] = '\0';
    json_get_str(e0, "state", state_out, state_sz);
  }

  if(country_out != NULL && country_sz > 0)
  {
    country_out[0] = '\0';
    json_get_str(e0, "country", country_out, country_sz);
  }

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
  char state[OW_NAME_SZ];
  char country[OW_NAME_SZ];
  char place[OW_NAME_SZ];
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

  apikey = kv_get_creds("plugin.openweather.creds.apikey");

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
  state[0] = '\0';
  country[0] = '\0';

  parsed = ow_parse_direct_geo(body, body_len,
      name, sizeof(name), state, sizeof(state),
      country, sizeof(country), zip, sizeof(zip), &lat, &lon);

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
    char rev_state[OW_NAME_SZ];
    char rev_country[OW_NAME_SZ];
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
    rev_state[0] = '\0';
    rev_country[0] = '\0';

    rparsed = ow_parse_direct_geo(rb, rb_len,
        rev_name, sizeof(rev_name),
        rev_state, sizeof(rev_state),
        rev_country, sizeof(rev_country), zip, sizeof(zip),
        &rev_lat, &rev_lon);

    mem_free(rb);

    // The reverse point backfills state/country when the direct hit
    // omitted them (and its name if the direct name was blank).
    if(rparsed == SUCCESS)
    {
      if(name[0] == '\0' && rev_name[0] != '\0')
        snprintf(name, sizeof(name), "%s", rev_name);
      if(state[0] == '\0' && rev_state[0] != '\0')
        snprintf(state, sizeof(state), "%s", rev_state);
      if(country[0] == '\0' && rev_country[0] != '\0')
        snprintf(country, sizeof(country), "%s", rev_country);
    }

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

  ow_compose_place(name[0] != '\0' ? name : city, state, country,
      place, sizeof(place));

  pthread_mutex_lock(&ow_geo_cache_mu);
  ow_city_insert_locked(city_lc, zip);
  ow_geo_insert(zip, lat, lon, place);
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

  apikey = kv_get_creds("plugin.openweather.creds.apikey");

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

  {
    char zname[OW_NAME_SZ];
    char zcountry[OW_NAME_SZ];

    zname[0] = '\0';
    zcountry[0] = '\0';
    json_get_str(root, "name", zname, sizeof(zname));
    json_get_str(root, "country", zcountry, sizeof(zcountry));

    // The zip geocoder carries no state field; enrich with the country
    // only (when non-US).
    ow_compose_place(zname, NULL, zcountry, name, sizeof(name));
  }

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

  plugin_unmap_notify_register(ow_unmap_cb, NULL);

  clam(CLAM_INFO, OW_CTX, "openweather plugin initialized");

  return(SUCCESS);
}

static void
ow_deinit(void)
{
  uint32_t stranded;

  plugin_unmap_notify_unregister(ow_unmap_cb);

  pthread_mutex_lock(&ow_active_mutex);
  stranded = ow_active_count;
  pthread_mutex_unlock(&ow_active_mutex);

  // Nothing to free here: a stranded request is still owned by a live
  // curl transfer whose completion lives in the mapping now going away.
  // Core's residual audit sees those — ow_*_done is curl_iter_req_t.cb
  // for every one — so it is the audit that refuses the dlclose, not us.
  // Naming the count here is what makes that refusal legible.
  if(stranded > 0)
    clam(CLAM_WARN, OW_CTX, "%u weather request(s) still in flight at "
        "deinit", stranded);

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
  .version         = "4.0",
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
