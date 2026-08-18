// botmanager — MIT
// National Weather Service (weather.gov) client: point → forecast grid.
#define WEATHERGOV_INTERNAL
#define WXG_CORE_TU
#include "weathergov.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

// ----------------------------------------------------------------------
// Request freelist
// ----------------------------------------------------------------------

wxg_request_t *
wxg_req_alloc(void)
{
  wxg_request_t *r = NULL;

  pthread_mutex_lock(&wxg_free_mu);

  if(wxg_free != NULL)
  {
    r = wxg_free;
    wxg_free = r->next;
  }

  pthread_mutex_unlock(&wxg_free_mu);

  if(r == NULL)
    r = mem_alloc(WXG_CTX, "request", sizeof(*r));

  memset(r, 0, sizeof(*r));

  return(r);
}

void
wxg_req_release(wxg_request_t *r)
{
  uint64_t slot = r->slot;

  pthread_mutex_lock(&wxg_active_mutex);
  wxg_req_untrack_locked(r);
  pthread_mutex_unlock(&wxg_active_mutex);

  pthread_mutex_lock(&wxg_free_mu);
  r->next = wxg_free;
  wxg_free = r;
  pthread_mutex_unlock(&wxg_free_mu);

  // Last, always: every terminal path in this plugin ends here, so this
  // is where the drain in wxg_stop() learns the work is over — and
  // everything above it touches state wxg_deinit() is about to free.
  curl_flight_close(&wxg_flight, slot);
}

// ----------------------------------------------------------------------
// The in-flight registry — every request borrows a foreign callback
// ----------------------------------------------------------------------

// Each weathergov_* entry point stores its caller's completion in a
// request of ours and hands core's curl layer one of our own wxg_*_done
// functions instead. That is one indirection more than core can see:
// plugin_quiesce and plugin_audit both range-test curl_iter_req_t.cb,
// which for our transfers names THIS mapping, never the caller's. Reload
// the `weather` feature with a request airborne and the stored pointer
// aims into freed .text.
//
// The registry therefore brackets a request's entire lifetime rather
// than a single transfer — filed the moment the caller's callback is
// installed, dropped by wxg_req_release, which every terminal path
// already goes through. That the point lookup is one round trip today is
// not something to lean on: WX-2 onward chain legs behind the same
// caller half.
//
// A request whose caller left still runs to the end; it simply delivers
// to nobody. The caller's `user` is dropped with the callback and
// whatever it points at is leaked. Nothing else is possible: only the
// caller knows how to free its own context, and the caller is precisely
// what is no longer there. A bounded leak on an operator action beats a
// SIGSEGV. See PLUGIN.md §Lifecycle Contract.

bool
wxg_req_track(wxg_request_t *r)
{
  // The flight is what stop() waits on, so the slot opens before the
  // registry entry does. A grounded flight refuses here, and the entry
  // point hands the work straight back to its caller.
  if(curl_flight_open(&wxg_flight, &r->slot) != SUCCESS)
    return(FAIL);

  pthread_mutex_lock(&wxg_active_mutex);

  r->next_active = wxg_active_head;
  wxg_active_head = r;
  wxg_active_count++;

  pthread_mutex_unlock(&wxg_active_mutex);

  return(SUCCESS);
}

// Caller holds wxg_active_mutex. A no-op for a request that is not on
// the list, which is what makes release-after-delivery safe.
static void
wxg_req_untrack_locked(wxg_request_t *r)
{
  wxg_request_t **pp;

  for(pp = &wxg_active_head; *pp != NULL; pp = &(*pp)->next_active)
  {
    if(*pp != r)
      continue;

    *pp = r->next_active;
    r->next_active = NULL;
    wxg_active_count--;
    return;
  }
}

// Lift the caller's half off `r` and unlink it, both under the registry
// lock. The read has to happen in the same critical section the sweep
// would null it in — read it afterwards and the two interleave, which is
// the whole bug. Clearing the arms as we go also makes a second delivery
// on the same request structurally impossible.
void
wxg_req_take_caller(wxg_request_t *r, wxg_caller_t *out)
{
  pthread_mutex_lock(&wxg_active_mutex);

  wxg_req_untrack_locked(r);

  out->cb   = r->cb;
  out->user = r->user;

  memset(&r->cb, 0, sizeof(r->cb));
  r->user = NULL;

  pthread_mutex_unlock(&wxg_active_mutex);
}

// A mapping is going away (core is between the plugin's deinit() and its
// residual audit, so nothing of it runs any more). Drop every callback
// that lives inside it.
//
// Residual race: a request that has already taken its caller half holds
// it on the stack and is a few instructions from calling it. The window
// is bounded above by the quiescence poll plus the audit that follow
// this broadcast, and below by two stores — against an operator-timescale
// unload. Closing it would need core to wait on a lock a curl worker
// holds.
static void
wxg_unmap_cb(uintptr_t lo, uintptr_t hi, void *data)
{
  uint32_t orphaned = 0;

  (void)data;

  pthread_mutex_lock(&wxg_active_mutex);

  for(wxg_request_t *r = wxg_active_head; r != NULL; r = r->next_active)
  {
    uintptr_t cb = (uintptr_t)fn_addr(&r->cb.point);

    if(cb == 0 || cb < lo || cb >= hi)
      continue;

    // memset rather than one arm's NULL: the arms are a union, and
    // all-bits-zero is the null test every delivery path makes.
    memset(&r->cb, 0, sizeof(r->cb));
    r->user = NULL;
    orphaned++;
  }

  pthread_mutex_unlock(&wxg_active_mutex);

  if(orphaned > 0)
    clam(CLAM_WARN, WXG_CTX, "%u weather.gov request(s) lost their caller "
        "to an unload; they will complete and deliver nothing", orphaned);
}

// ----------------------------------------------------------------------
// Shared plumbing — HTTP, JSON, time
// ----------------------------------------------------------------------

// weather.gov authenticates on the User-Agent alone: an empty one is
// answered 403. There is no key and no credential tier, which is why
// this is a plain kv_get_str.
void
wxg_ua(char *out, size_t sz)
{
  const char *ua = kv_get_str("plugin.weathergov.user_agent");

  if(ua[0] == '\0')
    ua = WXG_UA_DEFAULT;

  snprintf(out, sz, "%s", ua);
}

// The one place a weather.gov transfer is built. curl_get() cannot set a
// User-Agent, so it can never be used against this API.
bool
wxg_http_get(const char *url, wxg_request_t *r, curl_done_cb_t cb)
{
  curl_request_t *req = curl_request_create(CURL_METHOD_GET, url, cb, r);

  if(req == NULL)
    return(FAIL);

  curl_request_set_user_agent(req, r->ua);
  curl_request_add_header(req, "Accept: application/geo+json");
  curl_request_set_timeout(req,
      (uint32_t)kv_get_uint("plugin.weathergov.timeout_secs"));

  // The slot moves to this leg's id before the submit, so a completion
  // that beats the return still finds something to close.
  return(curl_flight_relay(&wxg_flight, req, &r->slot));
}

// The single response classifier. Three outcomes, and only the first is
// a failure anybody hears about:
//
//   transport error  -> false, `err` populated, coverage untouched
//   non-2xx status   -> false, `err` EMPTY, *covered = false
//   2xx              -> true
//
// The second case is deliberately not an error. An uncovered coordinate
// is reported as 404 by /points and 400 by /alerts — two codes for one
// condition — so coverage is decided by "not 2xx", never by switching on
// a specific status.
bool
wxg_http_status_ok(const curl_response_t *resp, char *err, size_t err_sz,
    bool *covered)
{
  if(resp->curl_code != 0)
  {
    snprintf(err, err_sz, "weather.gov request failed: %s",
        resp->error != NULL ? resp->error : "transport error");
    return(false);
  }

  if(resp->status < 200 || resp->status >= 300)
  {
    *covered = false;
    return(false);
  }

  return(true);
}

// ISO-8601 with an inline offset ("2026-08-09T06:45:09-04:00") to a true
// UTC time_t, reporting the offset it carried. weather.gov's own
// timeZone field is an IANA name we would need a tz-database walk to
// resolve, but every timestamp states its offset, so this is the only
// timezone reader the plugin needs.
//
// Returns 0 on any malformed input — callers must treat 0 as "absent",
// never as the epoch.
time_t
wxg_parse_iso8601(const char *s, int32_t *tz_off_out)
{
  struct tm    tm = {0};
  int          y, mo, d, h, mi, sec;
  int32_t      off = 0;
  const char  *p;
  time_t       t;

  if(s == NULL || s[0] == '\0')
    return(0);

  if(sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &sec) != 6)
    return(0);

  tm.tm_year = y - 1900;
  tm.tm_mon  = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min  = mi;
  tm.tm_sec  = sec;

  t = timegm(&tm);

  if(t == (time_t)-1)
    return(0);

  // The offset trails the seconds field: 'Z', or ±HH:MM. Scan from the
  // 'T' so a date-side '-' can never be mistaken for a sign.
  p = strchr(s, 'T');

  if(p != NULL)
  {
    for(; *p != '\0'; p++)
    {
      int oh, om;

      if(*p != '+' && *p != '-')
        continue;

      if(sscanf(p + 1, "%2d:%2d", &oh, &om) == 2)
        off = (int32_t)((oh * 3600) + (om * 60)) * (*p == '-' ? -1 : 1);

      break;
    }
  }

  if(tz_off_out != NULL)
    *tz_off_out = off;

  return(t - (time_t)off);
}

// ----------------------------------------------------------------------
// Point cache
// ----------------------------------------------------------------------

// returns: cache entry if found and not expired, NULL otherwise.
//          expired entries are freed. must be called under lock.
static wxg_pointcache_t *
wxg_point_lookup(const char *coord)
{
  uint32_t idx;
  wxg_pointcache_t **pp;
  uint32_t ttl = (uint32_t)kv_get_uint("plugin.weathergov.point_cache_ttl");

  if(ttl == 0)
    return(NULL);

  idx = util_fnv1a(coord) % WXG_POINT_CACHE_BUCKETS;
  pp = &wxg_point_cache[idx];

  while(*pp != NULL)
  {
    wxg_pointcache_t *e = *pp;

    if(strcmp(e->coord, coord) == 0)
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

// Insert or update a point cache entry. Must be called under lock. `pt`
// may be NULL for a negative (uncovered) entry.
static void
wxg_point_insert(const char *coord, bool covered, const weathergov_point_t *pt)
{
  wxg_pointcache_t *e;
  uint32_t idx = util_fnv1a(coord) % WXG_POINT_CACHE_BUCKETS;

  for(e = wxg_point_cache[idx]; e != NULL; e = e->next)
  {
    if(strcmp(e->coord, coord) != 0)
      continue;

    e->covered   = covered;
    e->cached_at = time(NULL);

    if(pt != NULL)
      e->point = *pt;
    else
      memset(&e->point, 0, sizeof(e->point));

    return;
  }

  e = mem_alloc(WXG_CTX, "pointcache", sizeof(*e));

  memset(e, 0, sizeof(*e));
  snprintf(e->coord, sizeof(e->coord), "%s", coord);
  e->covered   = covered;
  e->cached_at = time(NULL);

  if(pt != NULL)
    e->point = *pt;

  e->next = wxg_point_cache[idx];
  wxg_point_cache[idx] = e;
}

// ----------------------------------------------------------------------
// Point resolution
// ----------------------------------------------------------------------

// The grid triple is the only part that is required: it is what every
// forecast URL is built from. Everything else — the place label, the
// timezone, the sun times — is enrichment the same response happens to
// carry, and its absence must not fail the lookup.
static bool
wxg_point_parse(struct json_object *root, weathergov_point_t *out)
{
  struct json_object *props;
  struct json_object *astro;
  struct json_object *rel;
  char state[8];

  props = json_get_obj(root, "properties");

  if(props == NULL)
    return(FAIL);

  if(!json_get_str(props, "gridId", out->grid_id, sizeof(out->grid_id))
      || !json_get_int(props, "gridX", &out->grid_x)
      || !json_get_int(props, "gridY", &out->grid_y))
    return(FAIL);

  json_get_str(props, "timeZone", out->tz, sizeof(out->tz));

  // relativeLocation is a GeoJSON feature: the city and state sit one
  // level down, under its own `properties`.
  rel = json_get_obj(props, "relativeLocation");

  if(rel != NULL)
    rel = json_get_obj(rel, "properties");

  if(rel != NULL)
  {
    state[0] = '\0';
    json_get_str(rel, "city", out->place, sizeof(out->place));
    json_get_str(rel, "state", state, sizeof(state));

    if(out->place[0] != '\0' && state[0] != '\0')
    {
      size_t len = strlen(out->place);

      if(len < sizeof(out->place))
        snprintf(out->place + len, sizeof(out->place) - len, ", %s", state);
    }
  }

  // Sunrise and sunset arrive with the point, so nothing here costs a
  // second call. The table is the current UTC day's and can read one day
  // old out of upstream's cache — good to the minute, not to the second.
  astro = json_get_obj(props, "astronomicalData");

  if(astro != NULL)
  {
    char ts[40];

    if(json_get_str(astro, "sunrise", ts, sizeof(ts)))
      out->sunrise = wxg_parse_iso8601(ts, &out->tz_offset);

    if(json_get_str(astro, "sunset", ts, sizeof(ts)))
      out->sunset = wxg_parse_iso8601(ts, &out->tz_offset);
  }

  return(SUCCESS);
}

// Takes the caller's half off the request first, so what runs is a stack
// copy that is either the caller's or NULL because the caller was
// unloaded mid-request.
static void
wxg_point_deliver(wxg_request_t *r)
{
  wxg_caller_t caller;

  wxg_req_take_caller(r, &caller);

  if(caller.cb.point != NULL)
    caller.cb.point(&r->acc.point, caller.user);

  wxg_req_release(r);
}

static void
wxg_point_deliver_err(wxg_request_t *r, const char *msg)
{
  snprintf(r->acc.point.err, sizeof(r->acc.point.err), "%s", msg);
  r->acc.point.covered = false;

  wxg_point_deliver(r);
}

static void
wxg_point_done(const curl_response_t *resp)
{
  wxg_request_t *r = (wxg_request_t *)resp->user_data;
  weathergov_point_result_t *res = &r->acc.point;
  struct json_object *root;

  res->covered = true;

  if(!wxg_http_status_ok(resp, res->err, sizeof(res->err), &res->covered))
  {
    if(res->err[0] != '\0')
    {
      wxg_point_deliver(r);
      return;
    }

    // Not covered — the expected answer outside the US, and cheap enough
    // to remember that a repeat costs nothing at all.
    clam(CLAM_DEBUG2, WXG_CTX, "point %s: outside NWS coverage (HTTP %ld)",
        r->coord, resp->status);

    pthread_mutex_lock(&wxg_point_cache_mu);
    wxg_point_insert(r->coord, false, NULL);
    pthread_mutex_unlock(&wxg_point_cache_mu);

    wxg_point_deliver(r);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, WXG_CTX);

  if(root == NULL)
  {
    wxg_point_deliver_err(r, "weather.gov returned an unreadable response.");
    return;
  }

  if(wxg_point_parse(root, &res->point) != SUCCESS)
  {
    json_object_put(root);
    wxg_point_deliver_err(r, "weather.gov returned no forecast grid.");
    return;
  }

  json_object_put(root);

  clam(CLAM_DEBUG2, WXG_CTX, "point %s -> %s/%d,%d (%s)",
      r->coord, res->point.grid_id, res->point.grid_x, res->point.grid_y,
      res->point.place);

  pthread_mutex_lock(&wxg_point_cache_mu);
  wxg_point_insert(r->coord, true, &res->point);
  pthread_mutex_unlock(&wxg_point_cache_mu);

  wxg_point_deliver(r);
}

// ----------------------------------------------------------------------
// Public mechanism API
// ----------------------------------------------------------------------

bool
weathergov_enabled(void)
{
  return(kv_get_uint("plugin.weathergov.enabled") != 0);
}

async_rc_t
weathergov_point_async(double lat, double lon,
    weathergov_point_cb_t cb, void *user)
{
  char url[WXG_URL_SZ];
  wxg_pointcache_t *cached;
  wxg_request_t *r;

  if(cb == NULL || !weathergov_enabled())
    return(ASYNC_FAILED_UNDELIVERED);

  r = wxg_req_alloc();
  r->type     = WXG_REQ_POINT;
  r->lat      = lat;
  r->lon      = lon;
  r->cb.point = cb;
  r->user     = user;

  // Rounded once, then used as both the URL tail and the cache key: the
  // full-precision form answers 301 to exactly this string.
  snprintf(r->coord, sizeof(r->coord), "%.4f,%.4f", lat, lon);
  wxg_ua(r->ua, sizeof(r->ua));

  // File it before anything can be submitted, never after: a completion
  // can run on a curl worker before the submitting call has returned.
  if(wxg_req_track(r) != SUCCESS)
  {
    wxg_req_release(r);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  pthread_mutex_lock(&wxg_point_cache_mu);
  cached = wxg_point_lookup(r->coord);

  if(cached != NULL)
  {
    r->acc.point.covered = cached->covered;
    r->acc.point.point   = cached->point;
    pthread_mutex_unlock(&wxg_point_cache_mu);

    clam(CLAM_DEBUG2, WXG_CTX, "point %s: %s [cache hit]", r->coord,
        r->acc.point.covered ? r->acc.point.point.grid_id : "uncovered");

    wxg_point_deliver(r);
    return(ASYNC_AIRBORNE);
  }

  pthread_mutex_unlock(&wxg_point_cache_mu);

  snprintf(url, sizeof(url), "%s/%s", WXG_POINTS_URL, r->coord);

  if(wxg_http_get(url, r, wxg_point_done) != SUCCESS)
  {
    wxg_req_release(r);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

// ----------------------------------------------------------------------
// Plugin lifecycle
// ----------------------------------------------------------------------

static bool
wxg_init(void)
{
  pthread_mutex_init(&wxg_free_mu, NULL);
  pthread_mutex_init(&wxg_point_cache_mu, NULL);
  memset(wxg_point_cache, 0, sizeof(wxg_point_cache));
  curl_flight_init(&wxg_flight);

  plugin_unmap_notify_register(wxg_unmap_cb, NULL);

  clam(CLAM_INFO, WXG_CTX, "weathergov plugin initialized");

  return(SUCCESS);
}

// The plugin's whole Class-B holding is the work in wxg_flight: no
// threads, no tasks, no bound vtable. A cancelled request still
// delivers, so what this waits for is weathergov's own callbacks
// finishing — not the transfers.
static bool
wxg_stop(void)
{
  uint32_t left = curl_flight_drain(&wxg_flight, WXG_STOP_DRAIN_MS);

  if(left == 0)
    return(SUCCESS);

  clam(CLAM_WARN, WXG_CTX, "%u weather.gov request(s) still airborne after "
      "a %u ms cancel-and-drain; refusing the unload rather than "
      "deinitializing under their callbacks", left,
      (uint32_t)WXG_STOP_DRAIN_MS);

  return(FAIL);
}

static void
wxg_deinit(void)
{
  uint32_t stranded;

  plugin_unmap_notify_unregister(wxg_unmap_cb);

  pthread_mutex_lock(&wxg_active_mutex);
  stranded = wxg_active_count;
  pthread_mutex_unlock(&wxg_active_mutex);

  // Nothing to free here, and since wxg_stop() this is normally zero:
  // the drain has already cancelled every leg and waited out its
  // callback. A survivor means the drain timed out, stop() refused the
  // unload, and we are on the shutdown path instead — where core's
  // residual audit sees them, since wxg_*_done is curl_iter_req_t.cb for
  // every one. Naming the count is what makes that legible.
  if(stranded > 0)
    clam(CLAM_WARN, WXG_CTX, "%u weather.gov request(s) still in flight at "
        "deinit", stranded);

  pthread_mutex_lock(&wxg_free_mu);

  while(wxg_free != NULL)
  {
    wxg_request_t *r = wxg_free;

    wxg_free = r->next;
    mem_free(r);
  }

  pthread_mutex_unlock(&wxg_free_mu);
  pthread_mutex_destroy(&wxg_free_mu);

  for(uint32_t i = 0; i < WXG_POINT_CACHE_BUCKETS; i++)
  {
    wxg_pointcache_t *e = wxg_point_cache[i];

    while(e != NULL)
    {
      wxg_pointcache_t *next = e->next;

      mem_free(e);
      e = next;
    }

    wxg_point_cache[i] = NULL;
  }

  pthread_mutex_destroy(&wxg_point_cache_mu);

  curl_flight_destroy(&wxg_flight);

  // Owned by the observation chain, freed here: one plugin, one place
  // that gives its memory back.
  wxg_station_cache_clear();

  clam(CLAM_INFO, WXG_CTX, "weathergov plugin deinitialized");
}

// Plugin descriptor

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "weathergov",
  .version         = "1.0",
  .type            = PLUGIN_SERVICE,
  .kind            = "weathergov",
  .provides        = { { .name = "service_weathergov" } },
  .provides_count  = 1,
  .requires_count  = 0,
  .kv_schema       = wxg_kv_schema,
  .kv_schema_count = 4,
  .init            = wxg_init,
  .start           = NULL,
  .stop            = wxg_stop,
  .deinit          = wxg_deinit,
  .ext             = NULL,
};
