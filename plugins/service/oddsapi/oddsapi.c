// botmanager — MIT
// The Odds API service plugin: betting lines, futures and scores from
// the-odds-api.com (v4), behind the oddsapi_api.h contract. The wire,
// the credit quota and a refcounted single-flight cache live here; the
// replies are read by oddsapi_parse.c and presented by oddsapi_cmd.c.
//
// The free tier is 500 credits a month and a league's lines cost 3, so
// the cache is the product: a reply is pulled once per TTL however many
// people ask, a request already on the wire is joined rather than
// repeated, and below the quota reserve the cached copy is served with
// its age instead of spending.
//
// The API key lands in the request URL (?apiKey=…). Never clam() an
// assembled request URL — it would leak the key to the logs.
#define ODDSAPI_INTERNAL
#define ODDSAPI_PROVIDER_INTERNAL
#include "oddsapi.h"

#include "oddsapi_cmd.h"

// ----------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------

static int64_t
oddsapi_mono(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return((int64_t)ts.tv_sec);
}

// The boundary for a sport key: it is pasted into a URL path unescaped,
// so it is held to the alphabet the API's keys are spelled in.
static bool
oddsapi_key_ok(const char *s)
{
  size_t n;

  for(n = 0; s[n] != '\0'; n++)
  {
    if(!((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= '0' && s[n] <= '9')
        || s[n] == '_'))
      return(false);
  }

  return(n > 0 && n < ODDSAPI_SPORT_KEY_SZ);
}

// Percent-encode `in` into `out`, RFC-3986 unreserved set kept verbatim.
// Returns the bytes the whole encoding needs, like snprintf, so a return
// >= cap is a truncation.
static size_t
oddsapi_urlencode(const char *in, char *out, size_t cap)
{
  static const char hex[] = "0123456789ABCDEF";
  size_t            n     = 0;

  for(const unsigned char *p = (const unsigned char *)in; *p != '\0'; p++)
  {
    unsigned char c = *p;
    bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
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

// The request URL for one kind. Every call asks for unix timestamps, so
// nothing downstream parses a date. Lines and futures name their books;
// with none configured they ask for the us region instead.
static bool
oddsapi_url(oddsapi_kind_t kind, const char *arg, const char *apikey,
    char *url, size_t cap)
{
  const char *books = kv_get_str("plugin.oddsapi.bookmakers");
  char        key[128];
  char        enc[512];
  char        sel[sizeof(enc) + 16];
  int         need = -1;

  if(oddsapi_urlencode(apikey, key, sizeof(key)) >= sizeof(key))
    return(false);

  if(books != NULL && books[0] != '\0')
  {
    if(oddsapi_urlencode(books, enc, sizeof(enc)) >= sizeof(enc))
      return(false);

    snprintf(sel, sizeof(sel), "bookmakers=%s", enc);
  }

  else
    strlcpy(sel, "regions=us", sizeof(sel));

  switch(kind)
  {
    case ODDSAPI_K_SPORTS:
      need = snprintf(url, cap, "%s?all=true&dateFormat=unix&apiKey=%s",
          ODDSAPI_API_BASE, key);
      break;

    case ODDSAPI_K_EVENTS:
      need = snprintf(url, cap, "%s/%s/events?dateFormat=unix&apiKey=%s",
          ODDSAPI_API_BASE, arg, key);
      break;

    case ODDSAPI_K_ODDS:
      need = snprintf(url, cap, "%s/%s/odds?markets=h2h,spreads,totals"
          "&oddsFormat=american&dateFormat=unix&%s&apiKey=%s",
          ODDSAPI_API_BASE, arg, sel, key);
      break;

    case ODDSAPI_K_FUTURES:
      need = snprintf(url, cap, "%s/%s/odds?markets=outrights"
          "&oddsFormat=american&dateFormat=unix&%s&apiKey=%s",
          ODDSAPI_API_BASE, arg, sel, key);
      break;

    case ODDSAPI_K_SCORES:
      need = snprintf(url, cap, "%s/%s/scores?daysFrom=1&dateFormat=unix"
          "&apiKey=%s", ODDSAPI_API_BASE, arg, key);
      break;

    case ODDSAPI_K_COUNT:
      break;
  }

  return(need > 0 && (size_t)need < cap);
}

// A quota counter from the reply's headers; -1 when it was not sent or
// does not read as a count.
static int32_t
oddsapi_hdr_int(const curl_response_t *resp, const char *name)
{
  const char *v = curl_response_header(resp, name);
  char       *end;
  long        n;

  if(v == NULL)
    return(-1);

  errno = 0;
  n     = strtol(v, &end, 10);

  if(errno != 0 || end == v || n < 0)
    return(-1);

  return(n > INT32_MAX ? INT32_MAX : (int32_t)n);
}

// ----------------------------------------------------------------------
// Snapshots
// ----------------------------------------------------------------------

static uint32_t
oddsapi_snap_items(const oddsapi_snap_t *snap)
{
  switch(snap->kind)
  {
    case ODDSAPI_K_SPORTS:  return(snap->u.sports.n);
    case ODDSAPI_K_EVENTS:  return(snap->u.events.n);
    case ODDSAPI_K_ODDS:    return(snap->u.slate.ngames);
    case ODDSAPI_K_FUTURES: return(snap->u.futures.n);
    case ODDSAPI_K_SCORES:  return(snap->u.scores.n);
    case ODDSAPI_K_COUNT:   break;
  }

  return(0);
}

static void
oddsapi_snap_free(oddsapi_snap_t *snap)
{
  switch(snap->kind)
  {
    case ODDSAPI_K_SPORTS:  oddsapi_sports_clear(&snap->u.sports);   break;
    case ODDSAPI_K_EVENTS:  oddsapi_events_clear(&snap->u.events);   break;
    case ODDSAPI_K_ODDS:    oddsapi_slate_clear(&snap->u.slate);     break;
    case ODDSAPI_K_FUTURES: oddsapi_futures_clear(&snap->u.futures); break;
    case ODDSAPI_K_SCORES:  oddsapi_scores_clear(&snap->u.scores);   break;
    case ODDSAPI_K_COUNT:   break;
  }

  mem_free(snap);
}

// Under oddsapi_mu. Freeing here is safe: it reaches only the allocator.
static void
oddsapi_snap_unref_locked(oddsapi_snap_t *snap)
{
  if(--snap->refs == 0)
    oddsapi_snap_free(snap);
}

static void
oddsapi_snap_drop(oddsapi_snap_t *snap)
{
  pthread_mutex_lock(&oddsapi_mu);
  oddsapi_snap_unref_locked(snap);
  pthread_mutex_unlock(&oddsapi_mu);
}

// A new snapshot holding the cache's one ref, or NULL when the body is
// not the shape `kind` documents.
static oddsapi_snap_t *
oddsapi_snap_parse(oddsapi_kind_t kind, const char *body, size_t len)
{
  oddsapi_snap_t *snap = mem_alloc(ODDSAPI_CTX, "snap", sizeof(*snap));
  bool            ok   = false;

  switch(kind)
  {
    case ODDSAPI_K_SPORTS:
      ok = oddsapi_parse_sports(body, len, &snap->u.sports);
      break;

    case ODDSAPI_K_EVENTS:
      ok = oddsapi_parse_events(body, len, &snap->u.events);
      break;

    case ODDSAPI_K_ODDS:
      ok = oddsapi_parse_odds(body, len, &snap->u.slate);
      break;

    case ODDSAPI_K_FUTURES:
      ok = oddsapi_parse_futures(body, len, &snap->u.futures);
      break;

    case ODDSAPI_K_SCORES:
      ok = oddsapi_parse_scores(body, len, &snap->u.scores);
      break;

    case ODDSAPI_K_COUNT:
      break;
  }

  // A parser that refuses has allocated nothing.
  if(!ok)
  {
    mem_free(snap);
    return(NULL);
  }

  snap->refs    = 1;
  snap->kind    = kind;
  snap->fetched = time(NULL);
  return(snap);
}

// ----------------------------------------------------------------------
// Delivery
// ----------------------------------------------------------------------

// One callback. `snap` is read only when `status` is ODDSAPI_OK, and the
// caller holds a ref on it for the length of the call.
static void
oddsapi_deliver(oddsapi_cb_t cb, void *user, oddsapi_kind_t kind,
    oddsapi_status_t status, const char *message, const oddsapi_snap_t *snap,
    bool stale)
{
  oddsapi_res_t res;

  memset(&res, 0, sizeof(res));
  res.status = status;
  res.kind   = kind;
  res.stale  = stale;
  strlcpy(res.message, message, sizeof(res.message));

  if(status == ODDSAPI_OK && snap != NULL)
  {
    res.fetched = snap->fetched;

    switch(kind)
    {
      case ODDSAPI_K_SPORTS:  res.sports  = &snap->u.sports;  break;
      case ODDSAPI_K_EVENTS:  res.events  = &snap->u.events;  break;
      case ODDSAPI_K_ODDS:    res.slate   = &snap->u.slate;   break;
      case ODDSAPI_K_FUTURES: res.futures = &snap->u.futures; break;
      case ODDSAPI_K_SCORES:  res.scores  = &snap->u.scores;  break;
      case ODDSAPI_K_COUNT:   break;
    }
  }

  cb(&res, user);
}

// The one funnel a flight ends through, answered or refused. Publishes
// `fresh` (which arrives holding the cache's ref), takes the whole
// waiter list off the entry, and answers each waiter outside the lock:
// the fresh copy, else the old copy marked stale with `message`, else
// the failure. `skip` is a waiter whose caller is being told
// ASYNC_FAILED_UNDELIVERED instead — it is freed, never called.
//
// Nothing touches `e` after the unlock: it is no longer in flight, so
// another fetch may claim it from here on.
static void
oddsapi_settle(oddsapi_entry_t *e, oddsapi_snap_t *fresh,
    oddsapi_status_t status, const char *message,
    const oddsapi_waiter_t *skip)
{
  oddsapi_waiter_t *list;
  oddsapi_snap_t   *serve;
  oddsapi_kind_t    kind;

  pthread_mutex_lock(&oddsapi_mu);

  if(fresh != NULL)
  {
    if(e->snap != NULL)
      oddsapi_snap_unref_locked(e->snap);

    e->snap = fresh;
  }

  list        = e->waiters;
  kind        = e->kind;
  serve       = e->snap;
  e->waiters  = NULL;
  e->tail     = NULL;
  e->inflight = false;

  if(serve != NULL)
    serve->refs++;

  pthread_mutex_unlock(&oddsapi_mu);

  while(list != NULL)
  {
    oddsapi_waiter_t *next = list->next;

    if(list != skip)
    {
      if(fresh != NULL)
        oddsapi_deliver(list->cb, list->user, kind, ODDSAPI_OK, "", serve,
            false);

      else if(serve != NULL)
        oddsapi_deliver(list->cb, list->user, kind, ODDSAPI_OK, message,
            serve, true);

      else
        oddsapi_deliver(list->cb, list->user, kind, status, message, NULL,
            false);
    }

    mem_free(list);
    list = next;
  }

  if(serve != NULL)
    oddsapi_snap_drop(serve);
}

// ----------------------------------------------------------------------
// The cache
// ----------------------------------------------------------------------

// A slot for a key not yet cached: a free one, else the least recently
// fetched one not in flight — an entry that never answered counts as
// oldest. NULL when every slot is in flight.
static oddsapi_entry_t *
oddsapi_entry_claim_locked(const char *key, oddsapi_kind_t kind)
{
  oddsapi_entry_t *e = NULL;

  for(uint32_t i = 0; i < ODDSAPI_CACHE_SZ; i++)
  {
    oddsapi_entry_t *c = &oddsapi_cache[i];

    if(c->key[0] == '\0')
    {
      e = c;
      break;
    }

    if(c->inflight)
      continue;

    if(e == NULL || (c->snap != NULL ? c->snap->fetched : 0)
        < (e->snap != NULL ? e->snap->fetched : 0))
      e = c;
  }

  if(e == NULL)
    return(NULL);

  if(e->snap != NULL)
    oddsapi_snap_unref_locked(e->snap);

  memset(e, 0, sizeof(*e));
  strlcpy(e->key, key, sizeof(e->key));
  e->kind = kind;
  return(e);
}

// Why a paid pull must not be made right now, or NULL when it may.
static const char *
oddsapi_hold_reason_locked(uint32_t reserve, oddsapi_status_t *st)
{
  if(oddsapi_backoff_until > oddsapi_mono())
  {
    *st = ODDSAPI_RATE_LIMITED;
    return("rate limited");
  }

  if(oddsapi_quota.remaining >= 0
      && (uint32_t)oddsapi_quota.remaining < reserve)
  {
    *st = ODDSAPI_QUOTA;
    return("quota low");
  }

  return(NULL);
}

// ----------------------------------------------------------------------
// The fetch path
// ----------------------------------------------------------------------

// Behind all five entry points. Every KV read happens before the lock;
// under it the request is answered from cache, joined to its twin,
// held back by the quota, or committed to the wire — and the wire is
// reached only after the lock is dropped.
static async_rc_t
oddsapi_fetch(oddsapi_kind_t kind, const char *arg, oddsapi_cb_t cb,
    void *user)
{
  const oddsapi_kind_info_t *ki      = &oddsapi_kinds[kind];
  const char                *apikey  = kv_get_creds("plugin.oddsapi.creds.apikey");
  time_t                     ttl     = (time_t)kv_get_uint(ki->ttl_key);
  uint32_t                   reserve = (uint32_t)kv_get_uint("plugin.oddsapi.quota_reserve");
  uint32_t                   to      = (uint32_t)kv_get_uint("plugin.oddsapi.timeout");
  time_t                     now     = time(NULL);
  char                       key[ODDSAPI_CACHE_KEY_SZ];
  char                       url[ODDSAPI_URL_SZ];
  oddsapi_entry_t           *e       = NULL;
  oddsapi_snap_t            *snap;
  oddsapi_waiter_t          *w;
  oddsapi_status_t           held    = ODDSAPI_OK;
  const char                *why;
  curl_request_t            *cr;

  if(cb == NULL || apikey == NULL || apikey[0] == '\0')
    return(ASYNC_FAILED_UNDELIVERED);

  if(kind == ODDSAPI_K_SPORTS)
    arg = "";

  else if(arg == NULL || !oddsapi_key_ok(arg))
    return(ASYNC_FAILED_UNDELIVERED);

  snprintf(key, sizeof(key), "%s:%s", ki->name, arg);

  if(!oddsapi_url(kind, arg, apikey, url, sizeof(url)))
    return(ASYNC_FAILED_UNDELIVERED);

  w       = mem_alloc(ODDSAPI_CTX, "waiter", sizeof(*w));
  w->cb   = cb;
  w->user = user;

  pthread_mutex_lock(&oddsapi_mu);

  for(uint32_t i = 0; i < ODDSAPI_CACHE_SZ && e == NULL; i++)
  {
    if(strcmp(oddsapi_cache[i].key, key) == 0)
      e = &oddsapi_cache[i];
  }

  // Fresh: answer in place.
  if(e != NULL && e->snap != NULL && now - e->snap->fetched < ttl)
  {
    snap = e->snap;
    snap->refs++;
    pthread_mutex_unlock(&oddsapi_mu);

    mem_free(w);
    oddsapi_deliver(cb, user, kind, ODDSAPI_OK, "", snap, false);
    oddsapi_snap_drop(snap);
    return(ASYNC_AIRBORNE);
  }

  // Already on the wire: ride along.
  if(e != NULL && e->inflight)
  {
    e->tail->next = w;
    e->tail       = w;
    pthread_mutex_unlock(&oddsapi_mu);
    return(ASYNC_AIRBORNE);
  }

  // A paid pull the quota or the backoff forbids: serve what we have.
  if(ki->paid && (why = oddsapi_hold_reason_locked(reserve, &held)) != NULL)
  {
    snap = e != NULL ? e->snap : NULL;

    if(snap != NULL)
      snap->refs++;

    pthread_mutex_unlock(&oddsapi_mu);
    mem_free(w);

    if(snap == NULL)
    {
      oddsapi_deliver(cb, user, kind, held, why, NULL, false);
      return(ASYNC_FAILED_DELIVERED);
    }

    oddsapi_deliver(cb, user, kind, ODDSAPI_OK, why, snap, true);
    oddsapi_snap_drop(snap);
    return(ASYNC_AIRBORNE);
  }

  if(e == NULL && (e = oddsapi_entry_claim_locked(key, kind)) == NULL)
  {
    pthread_mutex_unlock(&oddsapi_mu);
    mem_free(w);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  e->inflight = true;
  e->waiters  = w;
  e->tail     = w;
  pthread_mutex_unlock(&oddsapi_mu);

  // Opened before the request exists: a completion can run on a curl
  // worker before the submit has returned, so the slot has to be there
  // for it to close. FAIL means the plugin is stopping.
  if(curl_flight_open(&oddsapi_flight, &e->slot) != SUCCESS)
    goto refused;

  cr = curl_request_create(CURL_METHOD_GET, url, oddsapi_done, e);

  if(cr == NULL)
  {
    curl_flight_close(&oddsapi_flight, e->slot);
    goto refused;
  }

  curl_request_add_header(cr, "Accept: application/json");
  curl_request_capture_header(cr, "x-requests-remaining");
  curl_request_capture_header(cr, "x-requests-used");
  curl_request_capture_header(cr, "x-requests-last");

  if(to > 0)
    curl_request_set_timeout(cr, to);

  if(curl_flight_relay(&oddsapi_flight, cr, &e->slot) != SUCCESS)
  {
    curl_flight_close(&oddsapi_flight, e->slot);
    goto refused;
  }

  return(ASYNC_AIRBORNE);

refused:
  // Anyone who joined while the lock was down was promised a callback;
  // the caller who started the flight was not, and keeps its closure.
  oddsapi_settle(e, NULL, ODDSAPI_TRANSPORT, "the request could not be sent",
      w);
  return(ASYNC_FAILED_UNDELIVERED);
}

// Curl worker. The quota counters are read on every status, because a
// refusal reports the account just as an answer does.
static void
oddsapi_done(const curl_response_t *resp)
{
  oddsapi_entry_t  *e         = (oddsapi_entry_t *)resp->user_data;
  uint64_t          slot      = e->slot;
  oddsapi_kind_t    kind      = e->kind;
  int32_t           remaining = oddsapi_hdr_int(resp, "x-requests-remaining");
  int32_t           used      = oddsapi_hdr_int(resp, "x-requests-used");
  int32_t           last      = oddsapi_hdr_int(resp, "x-requests-last");
  oddsapi_snap_t   *fresh     = NULL;
  oddsapi_status_t  st        = ODDSAPI_OK;
  bool              backoff   = false;
  char              msg[128]  = "";
  char              key[ODDSAPI_CACHE_KEY_SZ];

  // Ours to read while the entry is in flight; copied for the log line,
  // which runs after settle has let go of it.
  strlcpy(key, e->key, sizeof(key));

  if(resp->cancelled || resp->curl_code != 0 || resp->status == 0)
  {
    st = ODDSAPI_TRANSPORT;
    strlcpy(msg, "no answer from the-odds-api.com", sizeof(msg));
  }

  else if(resp->status == 200)
  {
    if((fresh = oddsapi_snap_parse(kind, resp->body, resp->body_len)) == NULL)
    {
      st = ODDSAPI_PARSE;
      strlcpy(msg, "an unreadable reply from the-odds-api.com", sizeof(msg));
    }
  }

  else if(resp->status == 401)
  {
    bool spent = resp->body != NULL
        && memmem(resp->body, resp->body_len, ODDSAPI_OUT_OF_CREDITS,
            sizeof(ODDSAPI_OUT_OF_CREDITS) - 1) != NULL;

    st = spent ? ODDSAPI_QUOTA : ODDSAPI_AUTH;
    strlcpy(msg, spent ? "out of credits this month" : "the API key was refused",
        sizeof(msg));
  }

  else if(resp->status == 404 || resp->status == 422)
  {
    st = ODDSAPI_NOT_FOUND;
    strlcpy(msg, "the-odds-api.com does not know that sport", sizeof(msg));
  }

  else if(resp->status == 429)
  {
    st      = ODDSAPI_RATE_LIMITED;
    backoff = true;
    strlcpy(msg, "rate limited", sizeof(msg));
  }

  else
  {
    st = ODDSAPI_TRANSPORT;
    snprintf(msg, sizeof(msg), "the-odds-api.com answered HTTP %ld",
        resp->status);
  }

  pthread_mutex_lock(&oddsapi_mu);

  if(remaining >= 0)
  {
    oddsapi_quota.remaining = remaining;
    oddsapi_quota.seen      = time(NULL);
  }

  if(used >= 0)
    oddsapi_quota.used = used;

  if(last >= 0)
    oddsapi_quota.last = last;

  // Only ever extended: a late refusal must not cut an earlier one short.
  if(backoff && oddsapi_backoff_until < oddsapi_mono() + ODDSAPI_BACKOFF_S)
    oddsapi_backoff_until = oddsapi_mono() + ODDSAPI_BACKOFF_S;

  pthread_mutex_unlock(&oddsapi_mu);

  // One line per paid pull is how a later reader audits the month's
  // spend; a free call is logged only when it fails.
  if(st != ODDSAPI_OK)
    clam(CLAM_WARN, ODDSAPI_CTX, "%s: %s (HTTP %ld, %d credits left)", key,
        msg, resp->status, remaining);

  else if(oddsapi_kinds[kind].paid)
    clam(CLAM_INFO, ODDSAPI_CTX, "%s: %u rows, cost %d, %d credits left", key,
        oddsapi_snap_items(fresh), last, remaining);

  oddsapi_settle(e, fresh, st, msg, NULL);

  // Last, always: the drain in oddsapi_stop() is waiting on this line,
  // and everything above it reads state oddsapi_deinit() tears down.
  curl_flight_close(&oddsapi_flight, slot);
}

// ----------------------------------------------------------------------
// Provider API — the mechanism contract
// ----------------------------------------------------------------------

bool
oddsapi_configured(void)
{
  const char *apikey = kv_get_creds("plugin.oddsapi.creds.apikey");

  return(apikey != NULL && apikey[0] != '\0');
}

async_rc_t
oddsapi_sports_async(oddsapi_cb_t cb, void *user)
{
  return(oddsapi_fetch(ODDSAPI_K_SPORTS, NULL, cb, user));
}

async_rc_t
oddsapi_events_async(const char *sport_key, oddsapi_cb_t cb, void *user)
{
  return(oddsapi_fetch(ODDSAPI_K_EVENTS, sport_key, cb, user));
}

async_rc_t
oddsapi_odds_async(const char *sport_key, oddsapi_cb_t cb, void *user)
{
  return(oddsapi_fetch(ODDSAPI_K_ODDS, sport_key, cb, user));
}

async_rc_t
oddsapi_futures_async(const char *outright_key, oddsapi_cb_t cb, void *user)
{
  return(oddsapi_fetch(ODDSAPI_K_FUTURES, outright_key, cb, user));
}

async_rc_t
oddsapi_scores_async(const char *sport_key, oddsapi_cb_t cb, void *user)
{
  return(oddsapi_fetch(ODDSAPI_K_SCORES, sport_key, cb, user));
}

void
oddsapi_quota_get(oddsapi_quota_t *out)
{
  int64_t left;

  pthread_mutex_lock(&oddsapi_mu);
  *out = oddsapi_quota;
  left = oddsapi_backoff_until - oddsapi_mono();
  pthread_mutex_unlock(&oddsapi_mu);

  out->backoff_left = left > 0 ? (uint32_t)left : 0;
}

uint32_t
oddsapi_cache_rows(oddsapi_cache_row_t *rows, uint32_t cap)
{
  uint32_t n = 0;

  pthread_mutex_lock(&oddsapi_mu);

  for(uint32_t i = 0; i < ODDSAPI_CACHE_SZ && n < cap; i++)
  {
    const oddsapi_entry_t *e = &oddsapi_cache[i];

    if(e->key[0] == '\0')
      continue;

    strlcpy(rows[n].key, e->key, sizeof(rows[n].key));
    rows[n].fetched  = e->snap != NULL ? e->snap->fetched : 0;
    rows[n].items    = e->snap != NULL ? oddsapi_snap_items(e->snap) : 0;
    rows[n].inflight = e->inflight;
    n++;
  }

  pthread_mutex_unlock(&oddsapi_mu);
  return(n);
}

// ----------------------------------------------------------------------
// Plugin lifecycle
// ----------------------------------------------------------------------

// The quota picture starts unknown on every load — a reload forgets it
// — and the first reply of any kind, free or paid, fills it in.
static bool
oddsapi_init(void)
{
  pthread_mutex_init(&oddsapi_mu, NULL);
  curl_flight_init(&oddsapi_flight);
  memset(oddsapi_cache, 0, sizeof(oddsapi_cache));
  oddsapi_quota         = (oddsapi_quota_t){ .remaining = -1, .used = -1,
                                             .last = -1 };
  oddsapi_backoff_until = 0;

  // A plugin that cannot raise its own command surface is half-loaded,
  // which is worse than absent — fail the init and let the loader skip it.
  if(oddsapi_cmd_register() != SUCCESS)
  {
    curl_flight_destroy(&oddsapi_flight);
    pthread_mutex_destroy(&oddsapi_mu);
    return(FAIL);
  }

  clam(CLAM_INFO, ODDSAPI_CTX, "oddsapi plugin initialized");
  return(SUCCESS);
}

// The plugin's whole Class-B holding is its in-flight curl set: no
// threads, no tasks, no bound vtable. A cancelled request still
// delivers, so what this waits for is oddsapi's own callbacks finishing
// — not the transfers.
static bool
oddsapi_stop(void)
{
  uint32_t left = curl_flight_drain(&oddsapi_flight, ODDSAPI_STOP_DRAIN_MS);

  if(left == 0)
    return(SUCCESS);

  clam(CLAM_WARN, ODDSAPI_CTX, "%u request(s) still airborne after a %u ms "
      "cancel-and-drain; refusing the unload rather than deinitializing "
      "under their callbacks", left, (uint32_t)ODDSAPI_STOP_DRAIN_MS);

  return(FAIL);
}

// After a complete drain every flight has settled, so no entry has a
// waiter and the cache's ref is the only one any snapshot still has.
static void
oddsapi_deinit(void)
{
  oddsapi_cmd_unregister();

  for(uint32_t i = 0; i < ODDSAPI_CACHE_SZ; i++)
  {
    if(oddsapi_cache[i].snap != NULL)
      oddsapi_snap_free(oddsapi_cache[i].snap);
  }

  memset(oddsapi_cache, 0, sizeof(oddsapi_cache));
  curl_flight_destroy(&oddsapi_flight);
  pthread_mutex_destroy(&oddsapi_mu);
  clam(CLAM_INFO, ODDSAPI_CTX, "oddsapi plugin deinitialized");
}

// The command half's upward bot_chat dependency rides on this
// descriptor. Sound only because oddsapi is a dependency-graph leaf —
// nothing requires service_oddsapi, so nothing inherits it. See
// `PLUGIN.md §Layer Rules` Rule 1, leaf exception.
const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = ODDSAPI_CTX,
  .version         = "1.0",
  .type            = PLUGIN_SERVICE,
  .kind            = ODDSAPI_CTX,
  .provides        = { { .name = "service_oddsapi" } },
  .provides_count  = 1,
  .requires        = { { .name = "bot_chat" } },
  .requires_count  = 1,
  .kv_schema       = oddsapi_kv_schema,
  .kv_schema_count = sizeof(oddsapi_kv_schema) / sizeof(oddsapi_kv_schema[0]),
  .init            = oddsapi_init,
  .start           = NULL,
  .stop            = oddsapi_stop,
  .deinit          = oddsapi_deinit,
  .ext             = NULL,
};
