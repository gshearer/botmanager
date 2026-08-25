// botmanager — MIT
// Wikimedia service plugin: keyless access to Wikidata's structured
// claims and Wikipedia's prose, which are two APIs on two hosts joined
// by one identifier. Pure connectivity and normalization — every byte of
// presentation belongs to a consumer. All Wikimedia nuance (the merged
// resolver, the empty props= that strips reference blocks, snak
// datatypes, statement rank, date precision) is sealed here.
#define WIKIMEDIA_INTERNAL
#include "wikimedia_priv.h"

// ----------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------

static const char *
wm_api_base(void)
{
  return(kv_get_str("plugin.wikimedia.api_base"));
}

// The Wikipedia host for the configured language. Wikidata is one host
// for every language; the prose is not.
static void
wm_wiki_base(char *out, size_t cap)
{
  snprintf(out, cap, "https://%s.wikipedia.org",
      kv_get_str("plugin.wikimedia.language"));
}

// The one place a Wikimedia transfer is built. Their etiquette policy
// asks for a descriptive User-Agent, and curl_get() cannot set one — so
// it can never be used against these APIs.
static curl_request_t *
wm_get(const char *url, curl_done_cb_t cb, void *user)
{
  curl_request_t *cr = curl_request_create(CURL_METHOD_GET, url, cb, user);
  uint32_t        to;

  if(cr == NULL)
    return(NULL);

  curl_request_set_user_agent(cr, kv_get_str("plugin.wikimedia.user_agent"));
  curl_request_add_header(cr, "Accept: application/json");

  // The only header worth a capture slot here: it is how a refusal says
  // how long to stand down for, and wm_backoff_arm() is guessing without it.
  curl_request_capture_header(cr, "Retry-After");

  to = (uint32_t)kv_get_uint("plugin.wikimedia.timeout");

  if(to > 0)
    curl_request_set_timeout(cr, to);

  return(cr);
}

// Put one leg of `w` on the wire. A slot that already carries a leg is
// relayed onto the next one — a chain (resolve the id, then fetch it) is
// one piece of work wearing a second transfer — and a fresh handle is
// opened here. FAIL means the leg never flew and the caller still owes
// whatever accounting that leg would have done.
static bool
wm_launch(wm_work_t *w, uint8_t slot, const char *url, curl_done_cb_t cb,
    void *user)
{
  curl_request_t *cr;

  // The choke point every leg passes through, so this is where the
  // backoff is actually kept: a chain admitted before the window opened
  // does not get to spend the rest of its legs inside it.
  if(wm_retry_after() > 0)
    return(FAIL);

  if(w->slot[slot] == 0
      && curl_flight_open(&wm_flight, &w->slot[slot]) != SUCCESS)
    return(FAIL);

  cr = wm_get(url, cb, user);

  if(cr == NULL)
    return(FAIL);

  return(curl_flight_relay(&wm_flight, cr, &w->slot[slot]));
}

// ----------------------------------------------------------------------
// The backoff — Wikimedia's Retry-After, honoured
// ----------------------------------------------------------------------

static int64_t
wm_mono(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return((int64_t)ts.tv_sec);
}

uint32_t
wm_retry_after(void)
{
  int64_t until = atomic_load(&wm_backoff_until);
  int64_t now   = wm_mono();

  return(until > now ? (uint32_t)(until - now) : 0);
}

// Arm the backoff off a refusal that has just come back. Their
// Retry-After wins where it is the plain delta-seconds form; the
// HTTP-date form the RFC also permits has never been seen from these
// hosts, and falls back to the configured default rather than earning a
// date parser.
//
// An armed backoff is only ever extended, never shortened: several
// workers can be holding refusals from the same window, and one that
// lands late with a smaller remainder must not cut the earlier one short.
static void
wm_backoff_arm(const curl_response_t *resp)
{
  const char *hdr  = curl_response_header(resp, "Retry-After");
  uint32_t    cap  = (uint32_t)kv_get_uint("plugin.wikimedia.backoff_max");
  int64_t     secs = (int64_t)kv_get_uint("plugin.wikimedia.backoff");
  int64_t     want;
  int64_t     seen;

  if(hdr != NULL)
  {
    char *end = NULL;
    long  v;

    errno = 0;
    v     = strtol(hdr, &end, 10);

    if(errno == 0 && end != hdr && v > 0)
      secs = (int64_t)v;
  }

  if(secs < 1)
    secs = 1;

  if(cap > 0 && secs > (int64_t)cap)
    secs = (int64_t)cap;

  want = wm_mono() + secs;
  seen = atomic_load(&wm_backoff_until);

  // compare_exchange reloads `seen` on failure, so the guard re-reads.
  while(seen < want
      && !atomic_compare_exchange_weak(&wm_backoff_until, &seen, want))
    ;

  clam(CLAM_WARN, WIKIMEDIA_CTX,
      "wikimedia is rate-limiting us; standing down for %llds",
      (long long)secs);
}

// Refuse a call outright because the backoff is armed. The caller's
// callback runs with WM_RATE_LIMITED before this returns and nothing
// reaches the wire, which is the whole point of honouring a Retry-After
// — a request sent inside the window earns a longer one.
static async_rc_t
wm_backoff_refuse(wm_verb_t verb, wm_cb_u cb, void *user)
{
  wm_work_t *w = wm_work_open(verb, user);

  w->cb = cb;
  wm_work_fail(w, WM_RATE_LIMITED, "wikimedia is rate-limiting us");

  return(ASYNC_AIRBORNE);
}

// ----------------------------------------------------------------------
// Response classification
// ----------------------------------------------------------------------

// Turn a completed transfer into a parsed body, or into a status and a
// message. Returns NULL on every failure; the caller json_object_put()s
// what it gets back.
//
// ⚠ The action API answers a bad id with HTTP 200 and an `error` object,
// so the status line alone classifies nothing.
static struct json_object *
wm_body(const curl_response_t *resp, wm_status_t *status, char *msg,
    size_t msg_cap)
{
  struct json_object *root;
  struct json_object *err;
  char                code[64];

  *status = WM_TRANSPORT;
  msg[0]  = '\0';

  if(resp->cancelled)
  {
    *status = WM_UNAVAILABLE;
    snprintf(msg, msg_cap, "request cancelled");
    return(NULL);
  }

  if(resp->curl_code != 0)
  {
    snprintf(msg, msg_cap, "wikimedia request failed: %s",
        resp->error != NULL ? resp->error : "transport error");
    return(NULL);
  }

  if(resp->status == 404)
  {
    *status = WM_NOT_FOUND;
    return(NULL);
  }

  // Their edge refuses with 429 and x-envoy-ratelimited, and a 503 under
  // load carries the same Retry-After. A 503 without one is an outage
  // rather than a limit, and stays an ordinary transport failure.
  if(resp->status == 429
      || (resp->status == 503
          && curl_response_header(resp, "Retry-After") != NULL))
  {
    *status = WM_RATE_LIMITED;
    wm_backoff_arm(resp);
    snprintf(msg, msg_cap, "wikimedia is rate-limiting us");
    return(NULL);
  }

  if(resp->status < 200 || resp->status >= 300)
  {
    snprintf(msg, msg_cap, "wikimedia returned HTTP %ld", resp->status);
    return(NULL);
  }

  root = json_parse_buf(resp->body, resp->body_len, WIKIMEDIA_CTX);

  if(root == NULL)
  {
    snprintf(msg, msg_cap, "wikimedia returned unparseable JSON");
    return(NULL);
  }

  err = json_get_obj(root, "error");

  if(err == NULL)
  {
    *status = WM_OK;
    return(root);
  }

  code[0] = '\0';
  json_get_str(err, "code", code, sizeof(code));

  // An id nobody has heard of and an id that is not an id at all are the
  // same answer to a consumer: there is no such thing.
  if(strcmp(code, "no-such-entity") == 0 || strcmp(code, "param-invalid") == 0)
    *status = WM_NOT_FOUND;
  else
    snprintf(msg, msg_cap, "wikidata refused the query (%s)",
        code[0] != '\0' ? code : "unknown error");

  json_object_put(root);
  return(NULL);
}

// ----------------------------------------------------------------------
// Snak parsing — a value, its kind, and what precision it carries
// ----------------------------------------------------------------------

// ----------------------------------------------------------------------
// Batched label resolution — an id is not an answer
// ----------------------------------------------------------------------

// ----------------------------------------------------------------------
// Caches. Fixed tables with a round-robin cursor — nothing here
// justifies an eviction policy, and every entry is near-static.
// ----------------------------------------------------------------------

static bool
wm_resolve_cache_get(const char *key, wm_resolve_res_t *out, time_t now,
    uint32_t ttl)
{
  bool hit = false;

  pthread_mutex_lock(&wm_cache_mu);

  for(size_t i = 0; i < WM_RESOLVE_CACHE_SZ && !hit; i++)
  {
    if(wm_resolve_cache[i].t == 0
        || strcmp(wm_resolve_cache[i].key, key) != 0
        || now - wm_resolve_cache[i].t >= (time_t)ttl)
      continue;

    *out = wm_resolve_cache[i].v;
    hit  = true;
  }

  pthread_mutex_unlock(&wm_cache_mu);
  return(hit);
}

static void
wm_resolve_cache_put(const char *key, const wm_resolve_res_t *v)
{
  size_t slot = WM_RESOLVE_CACHE_SZ;

  pthread_mutex_lock(&wm_cache_mu);

  for(size_t i = 0; i < WM_RESOLVE_CACHE_SZ && slot == WM_RESOLVE_CACHE_SZ; i++)
  {
    if(wm_resolve_cache[i].t != 0 && strcmp(wm_resolve_cache[i].key, key) == 0)
      slot = i;
  }

  if(slot == WM_RESOLVE_CACHE_SZ)
  {
    slot              = wm_resolve_cursor;
    wm_resolve_cursor = (wm_resolve_cursor + 1) % WM_RESOLVE_CACHE_SZ;
  }

  strlcpy(wm_resolve_cache[slot].key, key, sizeof(wm_resolve_cache[slot].key));
  wm_resolve_cache[slot].v = *v;
  wm_resolve_cache[slot].t = time(NULL);

  pthread_mutex_unlock(&wm_cache_mu);
}

static bool
wm_menu_cache_get(const char *key, wm_menu_res_t *out, time_t now,
    uint32_t ttl)
{
  bool hit = false;

  pthread_mutex_lock(&wm_cache_mu);

  for(size_t i = 0; i < WM_MENU_CACHE_SZ && !hit; i++)
  {
    if(wm_menu_cache[i].t == 0
        || strcmp(wm_menu_cache[i].key, key) != 0
        || now - wm_menu_cache[i].t >= (time_t)ttl)
      continue;

    *out = wm_menu_cache[i].v;
    hit  = true;
  }

  pthread_mutex_unlock(&wm_cache_mu);
  return(hit);
}

static void
wm_menu_cache_put(const char *key, const wm_menu_res_t *v)
{
  size_t slot = WM_MENU_CACHE_SZ;

  pthread_mutex_lock(&wm_cache_mu);

  for(size_t i = 0; i < WM_MENU_CACHE_SZ && slot == WM_MENU_CACHE_SZ; i++)
  {
    if(wm_menu_cache[i].t != 0 && strcmp(wm_menu_cache[i].key, key) == 0)
      slot = i;
  }

  if(slot == WM_MENU_CACHE_SZ)
  {
    slot           = wm_menu_cursor;
    wm_menu_cursor = (wm_menu_cursor + 1) % WM_MENU_CACHE_SZ;
  }

  strlcpy(wm_menu_cache[slot].key, key, sizeof(wm_menu_cache[slot].key));
  wm_menu_cache[slot].v = *v;
  wm_menu_cache[slot].t = time(NULL);

  pthread_mutex_unlock(&wm_cache_mu);
}

static bool
wm_prop_cache_get(const char *key, wm_property_t *out, uint8_t *n, time_t now,
    uint32_t ttl)
{
  bool hit = false;

  pthread_mutex_lock(&wm_cache_mu);

  for(size_t i = 0; i < WM_PROP_CACHE_SZ && !hit; i++)
  {
    if(wm_prop_cache[i].t == 0
        || strcmp(wm_prop_cache[i].key, key) != 0
        || now - wm_prop_cache[i].t >= (time_t)ttl)
      continue;

    memcpy(out, wm_prop_cache[i].cand, sizeof(wm_prop_cache[i].cand));
    *n  = wm_prop_cache[i].n;
    hit = true;
  }

  pthread_mutex_unlock(&wm_cache_mu);
  return(hit);
}

static void
wm_prop_cache_put(const char *key, const wm_property_t *cand, uint8_t n)
{
  size_t slot = WM_PROP_CACHE_SZ;

  pthread_mutex_lock(&wm_cache_mu);

  for(size_t i = 0; i < WM_PROP_CACHE_SZ && slot == WM_PROP_CACHE_SZ; i++)
  {
    if(wm_prop_cache[i].t != 0 && strcmp(wm_prop_cache[i].key, key) == 0)
      slot = i;
  }

  if(slot == WM_PROP_CACHE_SZ)
  {
    slot           = wm_prop_cursor;
    wm_prop_cursor = (wm_prop_cursor + 1) % WM_PROP_CACHE_SZ;
  }

  strlcpy(wm_prop_cache[slot].key, key, sizeof(wm_prop_cache[slot].key));
  memcpy(wm_prop_cache[slot].cand, cand, sizeof(wm_prop_cache[slot].cand));
  wm_prop_cache[slot].n = n;
  wm_prop_cache[slot].t = time(NULL);

  pthread_mutex_unlock(&wm_cache_mu);
}

// ----------------------------------------------------------------------
// The in-flight registry — every callback here is somebody else's
// ----------------------------------------------------------------------
//
// Each call below stores its caller's completion in a heap context of
// ours and hands core's curl layer one of our own callbacks instead.
// That is one indirection more than core can see: plugin_quiesce and
// plugin_audit both range-test curl_iter_req_t.cb, which for our
// transfers points at THIS mapping, never at the caller's. Unload the
// wiki command plugin — or the inference engine, which acquires corpora
// through here — with a request airborne and the stored pointer aims
// into freed .text.
//
// So every live piece of work is filed here, and plugin_unmap_notify
// tells us when a mapping is about to go away in time to null the
// pointers that name it. Work whose caller left still completes; it
// simply delivers to nobody. reachyapi.c and llm.c hold the identical
// posture for the identical reason, and the pattern is written up in
// PLUGIN.md.
//
// The caller's `user` is dropped with the callback and whatever it
// points at is leaked. Nothing else is possible: only the caller knows
// how to free its own context, and the caller is precisely what is no
// longer there. A bounded leak on an operator action beats a SIGSEGV.

static void
wm_work_track(wm_work_t *w)
{
  pthread_mutex_lock(&wm_active_mutex);

  w->next_active = wm_active_head;
  wm_active_head = w;

  pthread_mutex_unlock(&wm_active_mutex);
}

// Unlink `w` and take its callback out under the same lock the unmap
// sweep nulls it in — read it afterwards and the two interleave, which
// is the whole bug.
static void
wm_work_unlink(wm_work_t *w, wm_cb_u *cb_out)
{
  wm_work_t **pp;

  pthread_mutex_lock(&wm_active_mutex);

  for(pp = &wm_active_head; *pp != NULL; pp = &(*pp)->next_active)
  {
    if(*pp != w)
      continue;

    *pp = w->next_active;
    break;
  }

  *cb_out        = w->cb;
  w->next_active = NULL;

  pthread_mutex_unlock(&wm_active_mutex);
}

// A mapping is going away (core is between the plugin's deinit() and its
// residual audit, so nothing of it runs any more). Drop every callback
// that lives inside it.
//
// Residual race, the one reachyapi documents too: a completion that has
// already unlinked its work holds the callback on its stack and is a few
// instructions from calling it. The window is bounded above by the
// quiescence poll and the audit that follow this broadcast, and below by
// two stores — against an operator-timescale unload.
static void
wm_unmap_cb(uintptr_t lo, uintptr_t hi, void *data)
{
  uint32_t orphaned = 0;

  (void)data;

  pthread_mutex_lock(&wm_active_mutex);

  for(wm_work_t *w = wm_active_head; w != NULL; w = w->next_active)
  {
    uintptr_t cb = 0;

    switch(w->verb)
    {
      case WM_VERB_RESOLVE: cb = (uintptr_t)fn_addr(&w->cb.resolve); break;
      case WM_VERB_CLAIMS:  cb = (uintptr_t)fn_addr(&w->cb.claims);  break;
      case WM_VERB_MENU:    cb = (uintptr_t)fn_addr(&w->cb.menu);    break;
      case WM_VERB_PROSE:   cb = (uintptr_t)fn_addr(&w->cb.prose);   break;

      case WM_VERB_PROPERTY:
        cb = (uintptr_t)fn_addr(&w->cb.property);
        break;

      case WM_VERB_FACTS:   cb = (uintptr_t)fn_addr(&w->cb.facts);   break;
      case WM_VERB_REVERSE: cb = (uintptr_t)fn_addr(&w->cb.resolve); break;
    }

    if(cb == 0 || cb < lo || cb >= hi)
      continue;

    // memset rather than one arm's NULL: the arms are a union, and
    // all-bits-zero is the null test every delivery path makes.
    memset(&w->cb, 0, sizeof(w->cb));
    w->user = NULL;
    orphaned++;
  }

  pthread_mutex_unlock(&wm_active_mutex);

  if(orphaned > 0)
    clam(CLAM_WARN, WIKIMEDIA_CTX, "%u wikimedia request(s) lost their "
        "caller to an unload; they will complete and deliver nothing",
        orphaned);
}

// ----------------------------------------------------------------------
// Work lifecycle
// ----------------------------------------------------------------------

static wm_work_t *
wm_work_open(wm_verb_t verb, void *user)
{
  wm_work_t *w = mem_alloc(WIKIMEDIA_CTX, "work", sizeof(*w));

  memset(w, 0, sizeof(*w));
  w->verb = verb;
  w->user = user;
  atomic_init(&w->pending, 0);
  wm_work_track(w);

  return(w);
}

// The single terminal path: deliver, free, and only then give the flight
// slots back — the close is what stop()'s drain waits for, so it marks
// the end of the work rather than the end of a transfer.
static void
wm_work_finish(wm_work_t *w)
{
  uint64_t slot[WM_WORK_SLOTS];
  wm_cb_u  cb;
  void    *user;

  memcpy(slot, w->slot, sizeof(slot));
  user = w->user;
  wm_work_unlink(w, &cb);

  switch(w->verb)
  {
    case WM_VERB_RESOLVE:
      if(cb.resolve != NULL)
        cb.resolve(&w->u.resolve.res, user);
      break;

    case WM_VERB_CLAIMS:
      if(cb.claims != NULL)
        cb.claims(&w->u.claims.res, user);
      break;

    case WM_VERB_MENU:
      if(cb.menu != NULL)
        cb.menu(&w->u.menu.res, user);
      break;

    case WM_VERB_PROSE:
      if(cb.prose != NULL)
        cb.prose(&w->u.prose.res, user);
      break;

    case WM_VERB_PROPERTY:
      if(cb.property != NULL)
        cb.property(&w->u.property.res, user);
      break;

    case WM_VERB_FACTS:
      if(cb.facts != NULL)
        cb.facts(&w->u.facts.res, user);
      break;

    case WM_VERB_REVERSE:
      if(cb.resolve != NULL)
        cb.resolve(&w->u.reverse.res, user);
      break;
  }

  mem_free(w);

  for(uint8_t i = 0; i < WM_WORK_SLOTS; i++)
    curl_flight_close(&wm_flight, slot[i]);
}

// Terminal: this delivers the failure and frees `w`. Nothing may touch
// the work afterwards, and in particular nothing may follow it with a
// wm_work_finish() — that reads a freed work and delivers a second time.
static void
wm_work_fail(wm_work_t *w, wm_status_t status, const char *msg)
{
  switch(w->verb)
  {
    case WM_VERB_RESOLVE:
      w->u.resolve.res.status = status;
      strlcpy(w->u.resolve.res.message, msg, WM_MSG_SZ);
      break;

    case WM_VERB_CLAIMS:
      w->u.claims.res.status = status;
      strlcpy(w->u.claims.res.message, msg, WM_MSG_SZ);
      break;

    case WM_VERB_MENU:
      w->u.menu.res.status = status;
      strlcpy(w->u.menu.res.message, msg, WM_MSG_SZ);
      break;

    case WM_VERB_PROSE:
      w->u.prose.res.status = status;
      strlcpy(w->u.prose.res.message, msg, WM_MSG_SZ);
      break;

    case WM_VERB_PROPERTY:
      w->u.property.res.status = status;
      strlcpy(w->u.property.res.message, msg, WM_MSG_SZ);
      break;

    case WM_VERB_FACTS:
      w->u.facts.res.status = status;
      strlcpy(w->u.facts.res.message, msg, WM_MSG_SZ);
      break;

    case WM_VERB_REVERSE:
      w->u.reverse.res.status = status;
      strlcpy(w->u.reverse.res.message, msg, WM_MSG_SZ);
      break;
  }

  wm_work_finish(w);
}

// Terminal, and the shape every "the wire gave us nothing" site wants:
// while a backoff is armed, that IS the reason nothing came back, and a
// leg refused locally is indistinguishable from a leg nobody answered.
// Only one of the two statuses can tell whoever asked when to come back.
static void
wm_work_fail_wire(wm_work_t *w, const char *msg)
{
  wm_work_fail(w, wm_retry_after() > 0 ? WM_RATE_LIMITED : WM_TRANSPORT, msg);
}

// A pre-flight refusal: nothing is airborne, nobody has been told, and
// the caller keeps its own context. Only legal while every launch this
// work attempted has failed.
static void
wm_work_abandon(wm_work_t *w)
{
  uint64_t slot[WM_WORK_SLOTS];
  wm_cb_u  cb;

  memcpy(slot, w->slot, sizeof(slot));
  wm_work_unlink(w, &cb);
  mem_free(w);

  for(uint8_t i = 0; i < WM_WORK_SLOTS; i++)
    curl_flight_close(&wm_flight, slot[i]);
}

// One leg of a fan-out is accounted for, whether it flew or not. The
// thread that observes the last one owns what happens next — and after
// this returns, `w` may already be freed.
static void
wm_leg_retire(wm_work_t *w)
{
  if(atomic_fetch_sub(&w->pending, 1) != 1)
    return;

  switch(w->verb)
  {
    case WM_VERB_RESOLVE: wm_resolve_merge(w);  break;
    case WM_VERB_CLAIMS:  wm_claims_choose(w);  break;
    case WM_VERB_REVERSE: wm_reverse_choose(w); break;
    default:              break;  // the rest are chains, not fan-outs
  }
}

// ----------------------------------------------------------------------
// wm_resolve_async — the merged resolver
// ----------------------------------------------------------------------

// Leg (a): CirrusSearch. Relevance-ranked and prominence-aware, and
// completely typo-intolerant — "robert duval" reaches a French alchemist
// and never reaches Robert Duvall.
static void
wm_resolve_cirrus_done(const curl_response_t *resp)
{
  wm_work_t          *w = resp->user_data;
  struct json_object *root;
  struct json_object *query;
  struct json_object *hits;
  wm_status_t         status;
  char                msg[WM_MSG_SZ];
  int                 len;

  root = wm_body(resp, &status, msg, sizeof(msg));

  if(root == NULL)
  {
    w->u.resolve.cirrus_st = status;
    clam(CLAM_WARN, WIKIMEDIA_CTX, "search leg failed: %s",
        msg[0] != '\0' ? msg : "no match");
    wm_leg_retire(w);
    return;
  }

  w->u.resolve.cirrus_st = WM_OK;
  query = json_get_obj(root, "query");
  hits  = query != NULL ? json_get_array(query, "search") : NULL;
  len   = hits != NULL ? (int)json_object_array_length(hits) : 0;

  for(int i = 0; i < len && w->u.resolve.n_cirrus < WM_SEARCH_LIMIT; i++)
  {
    struct json_object *hit = json_object_array_get_idx(hits, i);
    char                qid[WM_QID_SZ];

    qid[0] = '\0';

    if(hit == NULL || !json_get_str(hit, "title", qid, sizeof(qid)))
      continue;

    // Namespace 0 on wikidata.org is items, so a page title IS a QID —
    // but the id is about to be pasted into a URL, so it is checked
    // rather than assumed.
    if(!wm_is_qid(qid, 'Q'))
      continue;

    strlcpy(w->u.resolve.cirrus[w->u.resolve.n_cirrus++], qid, WM_QID_SZ);
  }

  json_object_put(root);
  wm_leg_retire(w);
}

// Leg (b): wbsearchentities. Alias- and typo-tolerant, and completely
// prominence-blind — a Viennese merchant outranks Ludwig Wittgenstein.
static void
wm_resolve_wbs_done(const curl_response_t *resp)
{
  wm_work_t          *w = resp->user_data;
  struct json_object *root;
  struct json_object *hits;
  wm_status_t         status;
  char                msg[WM_MSG_SZ];
  int                 len;

  root = wm_body(resp, &status, msg, sizeof(msg));

  if(root == NULL)
  {
    w->u.resolve.wbs_st = status;
    clam(CLAM_WARN, WIKIMEDIA_CTX, "alias leg failed: %s",
        msg[0] != '\0' ? msg : "no match");
    wm_leg_retire(w);
    return;
  }

  w->u.resolve.wbs_st = WM_OK;
  hits = json_get_array(root, "search");
  len  = hits != NULL ? (int)json_object_array_length(hits) : 0;

  for(int i = 0; i < len && w->u.resolve.n_wbs < WM_SEARCH_LIMIT; i++)
  {
    struct json_object *hit = json_object_array_get_idx(hits, i);
    char                qid[WM_QID_SZ];

    qid[0] = '\0';

    if(hit == NULL || !json_get_str(hit, "id", qid, sizeof(qid)))
      continue;

    if(!wm_is_qid(qid, 'Q'))
      continue;

    strlcpy(w->u.resolve.wbs[w->u.resolve.n_wbs++], qid, WM_QID_SZ);
  }

  json_object_put(root);
  wm_leg_retire(w);
}

// Which arm holds the candidate window. Two verbs produce one:
// wm_resolve_async from a name, wm_reverse_async from a statement, and
// everything downstream of the ids — weighing, labelling, ranking — is
// the same work for both.
static wm_resolve_res_t *
wm_window_of(wm_work_t *w)
{
  return(w->verb == WM_VERB_REVERSE ? &w->u.reverse.res
                                    : &w->u.resolve.res);
}

// Weigh a window whose hits already carry their ids. FAIL means the
// work has NOT been failed and the caller still owes that.
static bool
wm_window_fetch(wm_work_t *w, uint8_t slot)
{
  wm_resolve_res_t *res = wm_window_of(w);
  char              ids[WM_IDLIST_SZ];
  char              url[WM_URL_SZ];
  char              set[WM_CANDIDATES_MAX][WM_QID_SZ];
  uint8_t           n = 0;
  int               need;

  for(uint8_t i = 0; i < res->n; i++)
    wm_id_push(set, &n, WM_CANDIDATES_MAX, res->hits[i].qid);

  if(n == 0 || wm_ids_join(set, n, ids, sizeof(ids)) != SUCCESS)
    return(FAIL);

  // props=sitelinks returns every sitelink rather than a count — Paris
  // has 366 — and there is no count-only endpoint. ~33 KB is the
  // ranker's price, and the operator has accepted it.
  //
  // languagefallback=1 is not optional and its absence is silent.
  // Wikidata moved the label of anything spelled the same everywhere
  // into the `mul` pseudo-language, so `languages=en` alone answers
  // Albert Einstein with an EMPTY label object and a perfectly good
  // English description beside it. With the fallback on, the value
  // arrives still keyed `en` and nothing downstream has to know.
  need = snprintf(url, sizeof(url),
      "%s?action=wbgetentities&ids=%s&props=labels%%7Cdescriptions%%7C"
      "sitelinks&languages=%s&languagefallback=1&format=json&formatversion=2",
      wm_api_base(), ids, kv_get_str("plugin.wikimedia.language"));

  if(need < 0 || (size_t)need >= sizeof(url))
    return(FAIL);

  return(wm_launch(w, slot, url, wm_resolve_window_done, w));
}

// Both searchers are in. Union them — relevance order first, then the
// aliases nobody else found — and go and weigh the result.
static void
wm_resolve_merge(wm_work_t *w)
{
  wm_resolve_res_t *res = &w->u.resolve.res;
  char              joined[WM_CANDIDATES_MAX][WM_QID_SZ];
  uint8_t           n = 0;

  for(uint8_t i = 0; i < w->u.resolve.n_cirrus; i++)
    wm_id_push(joined, &n, WM_CANDIDATES_MAX, w->u.resolve.cirrus[i]);

  for(uint8_t i = 0; i < w->u.resolve.n_wbs; i++)
    wm_id_push(joined, &n, WM_CANDIDATES_MAX, w->u.resolve.wbs[i]);

  if(n == 0)
  {
    // A nonsense query answers cleanly with nothing, and that is an
    // empty rather than a bad guess — but only when BOTH searchers
    // answered. A refused leg looked at nothing, so its silence says
    // nothing about whether the subject exists, and one of the two is
    // enough to make the whole window's emptiness meaningless.
    if(w->u.resolve.cirrus_st == WM_OK && w->u.resolve.wbs_st == WM_OK)
      wm_work_fail(w, WM_NOT_FOUND, "");
    else
      wm_work_fail_wire(w, "a wikidata searcher was refused");

    return;
  }

  // The window's ids are its own order; the fetch fills in the rest.
  for(uint8_t i = 0; i < n; i++)
    strlcpy(res->hits[i].qid, joined[i], WM_QID_SZ);

  res->n = n;

  if(wm_window_fetch(w, 2) != SUCCESS)
    wm_work_fail_wire(w, "could not weigh the candidate window");
}

// Leg (c): label, description, article title and sitelink count for the
// whole window, then the re-rank that fixes both searchers at once —
// PSG (102) behind Paris (366), the colour gold (61) behind the element
// (277).
static void
wm_resolve_window_done(const curl_response_t *resp)
{
  wm_work_t          *w   = resp->user_data;
  wm_resolve_res_t   *res = wm_window_of(w);
  struct json_object *root;
  struct json_object *entities;
  const char         *lang;
  wm_status_t         status;
  char                msg[WM_MSG_SZ];
  uint8_t             kept = 0;

  root = wm_body(resp, &status, msg, sizeof(msg));

  if(root == NULL)
  {
    wm_work_fail(w, status, msg);
    return;
  }

  lang     = kv_get_str("plugin.wikimedia.language");
  entities = json_get_obj(root, "entities");

  for(uint8_t i = 0; i < res->n; i++)
  {
    wm_candidate_t     *c   = &res->hits[i];
    struct json_object *ent = NULL;
    struct json_object *sub;
    struct json_object *loc;
    struct json_object *sitelinks;
    char                site[32];

    if(entities == NULL
        || !json_object_object_get_ex(entities, c->qid, &ent))
      continue;

    sub = json_get_obj(ent, "labels");
    loc = sub != NULL ? json_get_obj(sub, lang) : NULL;

    if(loc != NULL)
      json_get_str(loc, "value", c->label, sizeof(c->label));

    sub = json_get_obj(ent, "descriptions");
    loc = sub != NULL ? json_get_obj(sub, lang) : NULL;

    if(loc != NULL)
      json_get_str(loc, "value", c->description, sizeof(c->description));

    sitelinks = json_get_obj(ent, "sitelinks");

    if(sitelinks != NULL)
    {
      c->sitelinks = (int32_t)json_object_object_length(sitelinks);

      snprintf(site, sizeof(site), "%swiki", lang);
      loc = json_get_obj(sitelinks, site);

      if(loc != NULL)
        json_get_str(loc, "title", c->title, sizeof(c->title));
    }

    // An entity the window named but the fetch could not produce (a
    // redirect, a deletion between the two calls) is dropped rather
    // than offered as a labelless id.
    res->hits[kept++] = *c;
  }

  res->n = kept;
  json_object_put(root);

  if(res->n == 0)
  {
    wm_work_fail(w, WM_NOT_FOUND, "");
    return;
  }

  wm_rank_window(res->hits, res->n);
  res->status = WM_OK;

  // A reverse window is a different question every time — the property
  // and the value both vary — so only the by-name resolver caches.
  if(w->verb == WM_VERB_RESOLVE)
    wm_resolve_cache_put(w->u.resolve.key, res);

  wm_work_finish(w);
}

// ----------------------------------------------------------------------
// wm_claims_async — a property word, then the item that answers it
// ----------------------------------------------------------------------

// Ask each candidate property for its statements. One round trip for
// all of them, because which one wins is decided by which one has
// something to say.
static void
wm_claims_dispatch(wm_work_t *w)
{
  uint8_t launched = 0;
  uint8_t missing  = 0;

  // The +1 guard keeps `w` alive across the whole submit loop even when
  // every leg completes before the loop returns.
  atomic_store(&w->pending, (uint_least8_t)(w->u.claims.n_cand + 1));

  // Before the first launch, never after one: WM_OK is 0 and the work is
  // zeroed, so a candidate that never flies would read as one that
  // answered with nothing — and a leg can complete on a curl worker
  // between its own wm_launch() and the next statement here, so a seed
  // written after the launch would race its own answer back to unread.
  for(uint8_t i = 0; i < w->u.claims.n_cand; i++)
    w->u.claims.got[i].status = WM_TRANSPORT;

  for(uint8_t i = 0; i < w->u.claims.n_cand; i++)
  {
    char      url[WM_URL_SZ];
    wm_leg_t *leg;
    int       need;

    // props= empty is the whole trick: it drops the reference blocks
    // that dominate a claim payload (12,957 bytes -> 402 for one
    // property) and qualifiers survive it, which is what makes "current
    // members" answerable at all.
    need = snprintf(url, sizeof(url),
        "%s?action=wbgetclaims&entity=%s&property=%s&props=&format=json"
        "&formatversion=2",
        wm_api_base(), w->u.claims.res.entity, w->u.claims.cand[i].property);

    if(need < 0 || (size_t)need >= sizeof(url))
    {
      missing++;
      continue;
    }

    leg       = mem_alloc(WIKIMEDIA_CTX, "leg", sizeof(*leg));
    leg->work = w;
    leg->idx  = i;

    if(wm_launch(w, i, url, wm_claims_leg_done, leg) != SUCCESS)
    {
      mem_free(leg);
      missing++;
      continue;
    }

    launched++;
  }

  if(launched == 0)
  {
    wm_work_fail_wire(w, "could not query wikidata");
    return;
  }

  for(uint8_t i = 0; i < missing; i++)
    wm_leg_retire(w);

  wm_leg_retire(w);  // the guard; `w` may be gone from here on
}

// ----------------------------------------------------------------------
// The property word — one step, three verbs
//
// Turning "date of birth" into a ranked set of P-ids is the first half
// of wm_claims_async, the first half of wm_reverse_async, and the whole
// of wm_property_async. It is written once here and each verb says what
// to do with the answer.
// ----------------------------------------------------------------------

static const char *
wm_word_of(const wm_work_t *w)
{
  switch(w->verb)
  {
    case WM_VERB_CLAIMS:   return(w->u.claims.word);
    case WM_VERB_REVERSE:  return(w->u.reverse.word);
    case WM_VERB_PROPERTY: return(w->u.property.word);
    default:               return("");
  }
}

// The word resolved to `n` candidates. From here on `w` belongs to
// whichever verb asked, and may already be freed when this returns.
static void
wm_prop_adopt(wm_work_t *w, const wm_property_t *cand, uint8_t n)
{
  switch(w->verb)
  {
    case WM_VERB_CLAIMS:
      memcpy(w->u.claims.cand, cand, n * sizeof(*cand));
      w->u.claims.n_cand = n;
      wm_claims_dispatch(w);
      break;

    case WM_VERB_REVERSE:
      memcpy(w->u.reverse.cand, cand, n * sizeof(*cand));
      w->u.reverse.n_cand = n;
      wm_reverse_dispatch(w);
      break;

    case WM_VERB_PROPERTY:
      memcpy(w->u.property.res.cand, cand, n * sizeof(*cand));
      w->u.property.res.n      = n;
      w->u.property.res.status = WM_OK;
      wm_work_finish(w);
      break;

    default:
      wm_work_fail(w, WM_TRANSPORT, "property lookup on the wrong verb");
      break;
  }
}

// wbsearchentities hands back the label and the datatype with the id,
// so this leg is the only one that has to run for a word rather than a
// P-id.
static void
wm_prop_search_done(const curl_response_t *resp)
{
  wm_work_t          *w = resp->user_data;
  struct json_object *root;
  wm_property_t       cand[WM_PROP_CANDIDATES];
  wm_status_t         status;
  char                msg[WM_MSG_SZ];
  uint8_t             n;

  root = wm_body(resp, &status, msg, sizeof(msg));

  if(root == NULL)
  {
    wm_work_fail(w, status, msg);
    return;
  }

  n = wm_props_parse(root, cand, WM_PROP_CANDIDATES);
  json_object_put(root);

  if(n == 0)
  {
    wm_work_fail(w, WM_NOT_FOUND, "");
    return;
  }

  wm_prop_cache_put(wm_word_of(w), cand, n);
  wm_prop_adopt(w, cand, n);
}

// Put `word` in front of whichever verb owns `w`, from the cache where
// it is warm and from the wire where it is not. SUCCESS means the work
// has moved on and `w` is no longer the caller's to touch; FAIL means
// nothing flew and the caller still owes the abandon.
static bool
wm_prop_resolve(wm_work_t *w, const char *word)
{
  wm_property_t cand[WM_PROP_CANDIDATES];
  char          enc[WM_ENC_SZ];
  char          url[WM_URL_SZ];
  uint32_t      ttl = (uint32_t)kv_get_uint("plugin.wikimedia.cache_ttl");
  uint8_t       n;
  int           need;

  if(ttl > 0 && wm_prop_cache_get(word, cand, &n, time(NULL), ttl))
  {
    wm_prop_adopt(w, cand, n);
    return(SUCCESS);
  }

  if(wm_urlencode(word, enc, sizeof(enc)) >= sizeof(enc))
    return(FAIL);

  need = snprintf(url, sizeof(url),
      "%s?action=wbsearchentities&search=%s&type=property&language=%s"
      "&uselang=%s&limit=%d&format=json&formatversion=2",
      wm_api_base(), enc, kv_get_str("plugin.wikimedia.language"),
      kv_get_str("plugin.wikimedia.language"), WM_PROP_CANDIDATES);

  if(need < 0 || (size_t)need >= sizeof(url))
    return(FAIL);

  return(wm_launch(w, 0, url, wm_prop_search_done, w));
}

// One candidate property's statements. Writes only its own arm of the
// result set, so no lock is needed until the barrier.
static void
wm_claims_leg_done(const curl_response_t *resp)
{
  wm_leg_t           *leg = resp->user_data;
  wm_work_t          *w   = leg->work;
  uint8_t             idx = leg->idx;
  wm_claims_res_t    *got = &w->u.claims.got[idx];
  struct json_object *root;
  wm_status_t         status;
  char                msg[WM_MSG_SZ];

  // The arm is read out of the leg before the leg goes: everything
  // below wants the index, and it is this completion that frees it.
  mem_free(leg);
  root = wm_body(resp, &status, msg, sizeof(msg));

  if(root == NULL)
  {
    got->status = status;
    strlcpy(got->message, msg, sizeof(got->message));
    wm_leg_retire(w);
    return;
  }

  got->status = WM_OK;
  got->n      = wm_claims_parse(json_get_obj(root, "claims"),
      w->u.claims.cand[idx].property, got->claims, WM_CLAIMS_MAX);

  json_object_put(root);
  wm_leg_retire(w);
}

// Every candidate has answered. The first one — in the order the
// resolver ranked them — that actually has a statement on this item is
// the answer: that fall-through is what separates a person's height
// (P2048) from the elevation (P2044) that wins the literal label match.
static void
wm_claims_choose(wm_work_t *w)
{
  wm_claims_res_t *res = &w->u.claims.res;
  char             ids[WM_IDLIST_SZ];
  char             set[WM_IDS_MAX][WM_QID_SZ];
  char             url[WM_URL_SZ];
  uint8_t          n_ids;
  int              need;
  bool             answered = false;
  bool             all_in   = true;

  // The fall-through below stops at the first candidate that HAS a
  // statement, so the reachability sweep is its own loop over every
  // candidate — a later one being refused is exactly the case that must
  // not be reported as "this item records no such property".
  for(uint8_t i = 0; i < w->u.claims.n_cand; i++)
  {
    if(w->u.claims.got[i].status != WM_OK)
      all_in = false;
  }

  for(uint8_t i = 0; i < w->u.claims.n_cand && !answered; i++)
  {
    const wm_claims_res_t *got  = &w->u.claims.got[i];
    const wm_property_t   *cand = &w->u.claims.cand[i];

    if(got->status != WM_OK || got->n == 0)
      continue;

    strlcpy(res->property, cand->property, sizeof(res->property));
    strlcpy(res->label, cand->label, sizeof(res->label));
    strlcpy(res->datatype, cand->datatype, sizeof(res->datatype));
    memcpy(res->claims, got->claims, sizeof(res->claims));
    res->n   = got->n;
    answered = true;
  }

  if(!answered)
  {
    // Nothing recorded, and nothing wrong either — but only when EVERY
    // candidate answered. One refused leg and this is not "the item
    // records no date of death", it is "we did not get to look".
    if(all_in)
      wm_work_fail(w, WM_NOT_FOUND, "");
    else
      wm_work_fail_wire(w, "wikidata did not answer for every property");

    return;
  }

  res->status = WM_OK;

  n_ids = wm_ids_collect(res, res->label[0] == '\0' ? res->property : NULL,
      set, WM_IDS_MAX);

  if(n_ids == 0 || wm_ids_join(set, n_ids, ids, sizeof(ids)) != SUCCESS)
  {
    wm_work_finish(w);
    return;
  }

  need = snprintf(url, sizeof(url),
      "%s?action=wbgetentities&ids=%s&props=labels%%7Cdatatype&languages=%s&languagefallback=1"
      "&format=json&formatversion=2",
      wm_api_base(), ids, kv_get_str("plugin.wikimedia.language"));

  // A label lookup that cannot be built or flown is cosmetic: the answer
  // is already correct, it just reads as ids.
  if(need < 0 || (size_t)need >= sizeof(url)
      || wm_launch(w, 3, url, wm_claims_labels_done, w) != SUCCESS)
    wm_work_finish(w);
}

static void
wm_claims_labels_done(const curl_response_t *resp)
{
  wm_work_t          *w = resp->user_data;
  struct json_object *root;
  wm_status_t         status;
  char                msg[WM_MSG_SZ];

  root = wm_body(resp, &status, msg, sizeof(msg));

  if(root != NULL)
  {
    wm_labels_apply(json_get_obj(root, "entities"), &w->u.claims.res,
        kv_get_str("plugin.wikimedia.language"));
    json_object_put(root);
  }

  wm_work_finish(w);
}

// ----------------------------------------------------------------------
// wm_menu_async — what can be asked about a thing of this type
// ----------------------------------------------------------------------

static void
wm_menu_props_done(const curl_response_t *resp)
{
  wm_work_t          *w = resp->user_data;
  struct json_object *root;
  struct json_object *arr;
  wm_status_t         status;
  char                msg[WM_MSG_SZ];
  char                ids[WM_IDLIST_SZ];
  char                url[WM_URL_SZ];
  int                 len;
  int                 need;

  root = wm_body(resp, &status, msg, sizeof(msg));

  if(root == NULL)
  {
    wm_work_fail(w, status, msg);
    return;
  }

  arr = json_get_array(json_get_obj(root, "claims"), "P1963");
  len = arr != NULL ? (int)json_object_array_length(arr) : 0;

  for(int i = 0; i < len && w->u.menu.n_ids < WM_MENU_FETCH; i++)
  {
    struct json_object *st = json_object_array_get_idx(arr, i);
    wm_value_t          v;

    wm_snak_parse(json_get_obj(st, "mainsnak"), &v);

    if(v.kind == WM_VAL_ITEM && wm_is_qid(v.qid, 'P'))
      strlcpy(w->u.menu.ids[w->u.menu.n_ids++], v.qid, WM_QID_SZ);
  }

  json_object_put(root);

  if(w->u.menu.n_ids == 0)
  {
    wm_work_fail(w, WM_NOT_FOUND, "");
    return;
  }

  if(wm_ids_join(w->u.menu.ids, w->u.menu.n_ids, ids, sizeof(ids)) != SUCCESS)
  {
    wm_work_fail(w, WM_TRANSPORT, "property menu would not fit a request");
    return;
  }

  need = snprintf(url, sizeof(url),
      "%s?action=wbgetentities&ids=%s&props=labels%%7Cdatatype&languages=%s&languagefallback=1"
      "&format=json&formatversion=2",
      wm_api_base(), ids, kv_get_str("plugin.wikimedia.language"));

  if(need < 0 || (size_t)need >= sizeof(url)
      || wm_launch(w, 0, url, wm_menu_meta_done, w) != SUCCESS)
    wm_work_fail_wire(w, "could not read the property menu");
}

static void
wm_menu_meta_done(const curl_response_t *resp)
{
  wm_work_t          *w   = resp->user_data;
  wm_menu_res_t      *res = &w->u.menu.res;
  struct json_object *root;
  struct json_object *entities;
  const char         *lang;
  wm_status_t         status;
  char                msg[WM_MSG_SZ];

  root = wm_body(resp, &status, msg, sizeof(msg));

  if(root == NULL)
  {
    wm_work_fail(w, status, msg);
    return;
  }

  lang     = kv_get_str("plugin.wikimedia.language");
  entities = json_get_obj(root, "entities");

  // P1963 is curated and priority-ordered, so the head of the list is
  // the useful part and truncation costs nothing.
  for(uint8_t i = 0; i < w->u.menu.n_ids && res->n < WM_MENU_MAX; i++)
  {
    struct json_object *ent = NULL;
    struct json_object *sub;
    struct json_object *loc;
    wm_property_t      *p   = &res->props[res->n];

    if(entities == NULL
        || !json_object_object_get_ex(entities, w->u.menu.ids[i], &ent))
      continue;

    memset(p, 0, sizeof(*p));
    strlcpy(p->property, w->u.menu.ids[i], sizeof(p->property));
    json_get_str(ent, "datatype", p->datatype, sizeof(p->datatype));

    // An identifier is a cross-reference key, never an answer. Every
    // menu carries a run of them (IMDb ID, PlayStation ID, Giant Bomb
    // ID) and no consumer would ever want one, so they are dropped here
    // rather than in each of them.
    if(strcmp(p->datatype, "external-id") == 0)
      continue;

    sub = json_get_obj(ent, "labels");
    loc = sub != NULL ? json_get_obj(sub, lang) : NULL;

    if(loc != NULL)
      json_get_str(loc, "value", p->label, sizeof(p->label));

    res->n++;
  }

  json_object_put(root);
  res->status = WM_OK;
  wm_menu_cache_put(res->class_qid, res);
  wm_work_finish(w);
}

// ----------------------------------------------------------------------
// wm_prose_async — the other half of Wikimedia
// ----------------------------------------------------------------------

// An article's own address, the form a human would paste: spaces are
// underscores on every wiki, and '_' survives percent-encoding
// untouched. Built from whichever title the wiki finally answered
// under, so a redirect names its target.
static void
wm_page_url(const char *title, char *out, size_t cap)
{
  char base[128];
  char under[WM_TITLE_SZ];
  char enc[WM_ENC_SZ];
  int  need;

  strlcpy(under, title, sizeof(under));

  for(char *p = under; *p != '\0'; p++)
    if(*p == ' ')
      *p = '_';

  if(wm_urlencode(under, enc, sizeof(enc)) >= sizeof(enc))
  {
    out[0] = '\0';
    return;
  }

  wm_wiki_base(base, sizeof(base));
  need = snprintf(out, cap, "%s/wiki/%s", base, enc);

  if(need < 0 || (size_t)need >= cap)
    out[0] = '\0';
}

// Fetch the article. The lead summary comes from the REST endpoint,
// which carries the description and the QID in one 2 KB answer; the
// whole article comes from the action API, where an explaintext extract
// arrives with no HTML and no infobox boilerplate.
static bool
wm_prose_fetch(wm_work_t *w, const char *title)
{
  char base[128];
  char enc[WM_ENC_SZ];
  char url[WM_URL_SZ];
  int  need;

  if(wm_urlencode(title, enc, sizeof(enc)) >= sizeof(enc))
    return(FAIL);

  wm_wiki_base(base, sizeof(base));

  if(w->u.prose.full)
    need = snprintf(url, sizeof(url),
        "%s/w/api.php?action=query&prop=extracts%%7Cpageprops"
        "&ppprop=wikibase_item&explaintext=1&redirects=1&titles=%s"
        "&format=json&formatversion=2", base, enc);
  else
    need = snprintf(url, sizeof(url), "%s/api/rest_v1/page/summary/%s",
        base, enc);

  if(need < 0 || (size_t)need >= sizeof(url))
    return(FAIL);

  return(wm_launch(w, 0, url, wm_prose_done, w));
}

// A caller may name the article by its item instead of its title, which
// is one sitelink lookup away.
static void
wm_prose_sitelink_done(const curl_response_t *resp)
{
  wm_work_t          *w = resp->user_data;
  struct json_object *root;
  struct json_object *links;
  struct json_object *loc;
  wm_status_t         status;
  char                msg[WM_MSG_SZ];
  char                site[32];
  char                title[WM_TITLE_SZ];

  root = wm_body(resp, &status, msg, sizeof(msg));

  if(root == NULL)
  {
    wm_work_fail(w, status, msg);
    return;
  }

  snprintf(site, sizeof(site), "%swiki",
      kv_get_str("plugin.wikimedia.language"));

  title[0] = '\0';
  links    = json_get_obj(json_get_obj(root, "entities"), w->u.prose.res.qid);
  links    = links != NULL ? json_get_obj(links, "sitelinks") : NULL;
  loc      = links != NULL ? json_get_obj(links, site) : NULL;

  if(loc != NULL)
    json_get_str(loc, "title", title, sizeof(title));

  json_object_put(root);

  // An item with no article in this language is not a transport
  // failure; there is simply no prose to give.
  if(title[0] == '\0')
  {
    wm_work_fail(w, WM_NOT_FOUND, "");
    return;
  }

  if(wm_prose_fetch(w, title) != SUCCESS)
    wm_work_fail_wire(w, "could not fetch the article");
}

static void
wm_prose_done(const curl_response_t *resp)
{
  wm_work_t          *w   = resp->user_data;
  wm_prose_res_t     *res = &w->u.prose.res;
  struct json_object *root;
  struct json_object *page;
  struct json_object *extract = NULL;
  wm_status_t         status;
  char                msg[WM_MSG_SZ];

  root = wm_body(resp, &status, msg, sizeof(msg));

  if(root == NULL)
  {
    wm_work_fail(w, status, msg);
    return;
  }

  if(w->u.prose.full)
  {
    struct json_object *pages = json_get_array(json_get_obj(root, "query"),
        "pages");

    page = pages != NULL && json_object_array_length(pages) > 0
        ? json_object_array_get_idx(pages, 0) : NULL;

    if(page != NULL)
    {
      struct json_object *props = json_get_obj(page, "pageprops");

      json_get_str(page, "title", res->title, sizeof(res->title));

      if(props != NULL)
        json_get_str(props, "wikibase_item", res->qid, sizeof(res->qid));

      json_object_object_get_ex(page, "extract", &extract);
    }
  }

  else
  {
    page = root;
    json_get_str(page, "title", res->title, sizeof(res->title));
    json_get_str(page, "description", res->description,
        sizeof(res->description));
    json_get_str(page, "wikibase_item", res->qid, sizeof(res->qid));
    json_object_object_get_ex(page, "extract", &extract);
  }

  res->text = extract != NULL ? json_object_get_string(extract) : NULL;

  if(res->text == NULL || res->text[0] == '\0')
  {
    json_object_put(root);
    wm_work_fail(w, WM_NOT_FOUND, "");
    return;
  }

  res->len    = strlen(res->text);
  res->status = WM_OK;

  wm_page_url(res->title, res->url, sizeof(res->url));

  // The text is borrowed from the parsed body, so the delivery has to
  // happen while that body is still alive — the one place in this file
  // where the put comes after the finish.
  wm_work_finish(w);
  json_object_put(root);
}

// ----------------------------------------------------------------------
// wm_facts_async — what is worth reading about this item
//
// Four steps and no held payload: the type, the type's menu, the whole
// claims object parsed straight against that menu, and one batched
// lookup that turns the ids into words.
// ----------------------------------------------------------------------

static void
wm_facts_p31_done(const curl_response_t *resp)
{
  wm_work_t          *w   = resp->user_data;
  wm_facts_res_t     *res = &w->u.facts.res;
  struct json_object *root;
  wm_claim_t          got[WM_CLAIMS_MAX];
  wm_status_t         status;
  char                msg[WM_MSG_SZ];
  uint8_t             n;

  root = wm_body(resp, &status, msg, sizeof(msg));

  if(root == NULL)
  {
    wm_work_fail(w, status, msg);
    return;
  }

  n = wm_claims_parse(json_get_obj(root, "claims"), "P31", got,
      WM_CLAIMS_MAX);
  json_object_put(root);

  for(uint8_t i = 0; i < n && w->u.facts.n_class < WM_P31_TRIES; i++)
  {
    if(got[i].value.kind == WM_VAL_ITEM)
      strlcpy(w->u.facts.class[w->u.facts.n_class++], got[i].value.qid,
          WM_QID_SZ);
  }

  if(w->u.facts.n_class == 0)
  {
    wm_work_fail(w, WM_NOT_FOUND, "");
    return;
  }

  // The narrowest type is listed first and is the least likely to
  // publish a menu, so the types are tried in order and the first one
  // that answers decides the card's shape.
  strlcpy(res->class_qid, w->u.facts.class[0], sizeof(res->class_qid));

  if(wm_facts_menu_next(w) != SUCCESS)
    wm_work_fail_wire(w, "could not read the property menu");
}

static bool
wm_facts_menu_next(wm_work_t *w)
{
  return(wm_menu_async(w->u.facts.class[w->u.facts.at_class],
      wm_facts_menu_done, w) == ASYNC_AIRBORNE ? SUCCESS : FAIL);
}

// The menu is in — warm from its own cache, or a moment ago off the
// wire. Now the item itself, whole: props= empty drops the reference
// blocks, and what is left is still the largest payload this plugin
// fetches (a heavily-edited person runs past 250 KB).
static void
wm_facts_menu_done(const wm_menu_res_t *menu, void *user)
{
  wm_work_t *w = user;
  char       url[WM_URL_SZ];
  int        need;

  // A menu is the preferred ordering, not the only one. Try each of the
  // item's types in turn, then fall back to its own statement order —
  // Wikidata curates P1963 for broad classes and not for narrow ones,
  // so an absent menu is ordinary rather than exceptional.
  if(menu->status == WM_OK)
  {
    w->u.facts.menu = *menu;
    strlcpy(w->u.facts.res.class_qid, menu->class_qid,
        sizeof(w->u.facts.res.class_qid));
  }

  else if(++w->u.facts.at_class < w->u.facts.n_class)
  {
    if(wm_facts_menu_next(w) != SUCCESS)
      wm_work_fail_wire(w, "could not read the property menu");

    return;
  }


  need = snprintf(url, sizeof(url),
      "%s?action=wbgetclaims&entity=%s&props=&format=json&formatversion=2",
      wm_api_base(), w->u.facts.res.qid);

  if(need < 0 || (size_t)need >= sizeof(url)
      || wm_launch(w, 1, url, wm_facts_claims_done, w) != SUCCESS)
    wm_work_fail_wire(w, "could not read the item");
}

static void
wm_facts_claims_done(const curl_response_t *resp)
{
  wm_work_t          *w   = resp->user_data;
  wm_facts_res_t     *res = &w->u.facts.res;
  struct json_object *root;
  wm_status_t         status;
  char                msg[WM_MSG_SZ];
  char                ids[WM_IDLIST_SZ];
  char                set[WM_IDS_MAX][WM_QID_SZ];
  char                url[WM_URL_SZ];
  uint8_t             n_ids;
  int                 need;

  root = wm_body(resp, &status, msg, sizeof(msg));

  if(root == NULL)
  {
    wm_work_fail(w, status, msg);
    return;
  }

  res->n = w->u.facts.menu.n > 0
      ? wm_facts_parse(json_get_obj(root, "claims"), &w->u.facts.menu,
            res->facts, WM_FACTS_MAX)
      : wm_facts_parse_any(json_get_obj(root, "claims"), res->facts,
            WM_FACTS_MAX);
  json_object_put(root);
  res->status = WM_OK;

  // The batch is bounded and the facts are in priority order, so what
  // it cannot reach is the tail of the card rather than its head.
  n_ids = wm_fact_ids_collect(res, set, WM_IDS_MAX);

  if(n_ids == 0 || wm_ids_join(set, n_ids, ids, sizeof(ids)) != SUCCESS)
  {
    wm_work_finish(w);
    return;
  }

  need = snprintf(url, sizeof(url),
      "%s?action=wbgetentities&ids=%s&props=labels%%7Cdescriptions"
      "%%7Cdatatype&languages=%s&languagefallback=1&format=json"
      "&formatversion=2",
      wm_api_base(), ids, kv_get_str("plugin.wikimedia.language"));

  // Not cosmetic here, unlike the claims path: a statement-ordered
  // block has no labels and no datatypes until this lands, so a card
  // without it is a column of P-ids and a run of identifiers.
  if(need < 0 || (size_t)need >= sizeof(url)
      || wm_launch(w, 2, url, wm_facts_labels_done, w) != SUCCESS)
    wm_work_finish(w);
}

static void
wm_facts_labels_done(const curl_response_t *resp)
{
  wm_work_t          *w = resp->user_data;
  struct json_object *root;
  wm_status_t         status;
  char                msg[WM_MSG_SZ];

  root = wm_body(resp, &status, msg, sizeof(msg));

  if(root != NULL)
  {
    wm_fact_labels_apply(json_get_obj(root, "entities"), &w->u.facts.res,
        kv_get_str("plugin.wikimedia.language"));
    json_object_put(root);
  }

  wm_work_finish(w);
}

// ----------------------------------------------------------------------
// wm_reverse_async — the statement read the other way round
//
// Same shape as wm_claims_async: fan the property candidates out, and
// answer from the first that has anything to say. What lands is a
// candidate window, so the tail of the work is the resolver's.
// ----------------------------------------------------------------------

static void
wm_reverse_dispatch(wm_work_t *w)
{
  uint8_t launched = 0;
  uint8_t missing  = 0;

  // The +1 guard keeps `w` alive across the whole submit loop even when
  // every leg completes before the loop returns.
  atomic_store(&w->pending, (uint_least8_t)(w->u.reverse.n_cand + 1));

  // Before the first launch, for the reason wm_claims_run gives.
  for(uint8_t i = 0; i < w->u.reverse.n_cand; i++)
    w->u.reverse.st[i] = WM_TRANSPORT;

  for(uint8_t i = 0; i < w->u.reverse.n_cand; i++)
  {
    char      url[WM_URL_SZ];
    wm_leg_t *leg;
    int       need;

    // haswbstatement is CirrusSearch's structured filter: it is the one
    // way to ask this question without SPARQL, which this plugin
    // refuses on latency variance.
    need = snprintf(url, sizeof(url),
        "%s?action=query&list=search&srsearch=haswbstatement%%3A%s%%3D%s"
        "&srlimit=%d&srnamespace=0&format=json&formatversion=2",
        wm_api_base(), w->u.reverse.cand[i].property, w->u.reverse.value,
        WM_REVERSE_LIMIT);

    if(need < 0 || (size_t)need >= sizeof(url))
    {
      missing++;
      continue;
    }

    leg       = mem_alloc(WIKIMEDIA_CTX, "leg", sizeof(*leg));
    leg->work = w;
    leg->idx  = i;

    if(wm_launch(w, i, url, wm_reverse_leg_done, leg) != SUCCESS)
    {
      mem_free(leg);
      missing++;
      continue;
    }

    launched++;
  }

  if(launched == 0)
  {
    wm_work_fail_wire(w, "could not query wikidata");
    return;
  }

  for(uint8_t i = 0; i < missing; i++)
    wm_leg_retire(w);

  wm_leg_retire(w);  // the guard; `w` may be gone from here on
}

static void
wm_reverse_leg_done(const curl_response_t *resp)
{
  wm_leg_t           *leg = resp->user_data;
  wm_work_t          *w   = leg->work;
  uint8_t             idx = leg->idx;
  struct json_object *root;
  struct json_object *query;
  struct json_object *hits;
  struct json_object *info;
  wm_status_t         status;
  char                msg[WM_MSG_SZ];
  int                 len;

  mem_free(leg);
  root = wm_body(resp, &status, msg, sizeof(msg));

  if(root == NULL)
  {
    w->u.reverse.st[idx] = status;
    clam(CLAM_WARN, WIKIMEDIA_CTX, "reverse leg failed: %s",
        msg[0] != '\0' ? msg : "no match");
    wm_leg_retire(w);
    return;
  }

  w->u.reverse.st[idx] = WM_OK;
  query = json_get_obj(root, "query");
  info  = query != NULL ? json_get_obj(query, "searchinfo") : NULL;
  hits  = query != NULL ? json_get_array(query, "search") : NULL;
  len   = hits != NULL ? (int)json_object_array_length(hits) : 0;

  if(info != NULL)
    json_get_int(info, "totalhits", &w->u.reverse.total[idx]);

  for(int i = 0; i < len && w->u.reverse.n_got[idx] < WM_REVERSE_LIMIT; i++)
  {
    struct json_object *hit = json_object_array_get_idx(hits, i);
    char                qid[WM_QID_SZ];

    qid[0] = '\0';

    if(hit == NULL || !json_get_str(hit, "title", qid, sizeof(qid)))
      continue;

    // Namespace 0 on wikidata.org is items, so a page title IS a QID —
    // but the id is about to be pasted into a URL, so it is checked
    // rather than assumed.
    if(!wm_is_qid(qid, 'Q'))
      continue;

    strlcpy(w->u.reverse.got[idx][w->u.reverse.n_got[idx]++], qid,
        WM_QID_SZ);
  }

  json_object_put(root);
  wm_leg_retire(w);
}

static void
wm_reverse_choose(wm_work_t *w)
{
  wm_resolve_res_t *res = &w->u.reverse.res;
  uint8_t           pick;
  bool              answered = false;

  for(pick = 0; pick < w->u.reverse.n_cand && !answered; pick++)
    answered = w->u.reverse.n_got[pick] > 0;

  if(!answered)
  {
    bool all_in = true;

    for(uint8_t i = 0; i < w->u.reverse.n_cand; i++)
    {
      if(w->u.reverse.st[i] != WM_OK)
        all_in = false;
    }

    // Nothing holds that statement, and nothing is wrong either — but
    // only when EVERY candidate answered. One refused leg and "nobody
    // holds this statement" is a claim about pages nothing looked at.
    if(all_in)
      wm_work_fail(w, WM_NOT_FOUND, "");
    else
      wm_work_fail_wire(w, "wikidata did not answer for every property");

    return;
  }

  pick--;

  for(uint8_t i = 0; i < w->u.reverse.n_got[pick] && i < WM_CANDIDATES_MAX;
      i++)
    strlcpy(res->hits[res->n++].qid, w->u.reverse.got[pick][i], WM_QID_SZ);

  res->total = w->u.reverse.total[pick];

  if(wm_window_fetch(w, 3) != SUCCESS)
    wm_work_fail_wire(w, "could not weigh the matches");
}

// ----------------------------------------------------------------------
// Provider API — the mechanism contract
// ----------------------------------------------------------------------

async_rc_t
wm_resolve_async(const char *name, wm_resolve_cb_t cb, void *user)
{
  wm_work_t *w;
  char       key[WM_QUERY_SZ];
  char       enc[WM_ENC_SZ];
  char       url[WM_URL_SZ];
  uint32_t   ttl;
  bool       flew_a;
  bool       flew_b;
  int        need;

  if(cb == NULL || name == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  wm_normalize(name, key, sizeof(key));

  if(key[0] == '\0' || wm_urlencode(key, enc, sizeof(enc)) >= sizeof(enc))
    return(ASYNC_FAILED_UNDELIVERED);

  if(wm_retry_after() > 0)
    return(wm_backoff_refuse(WM_VERB_RESOLVE,
        (wm_cb_u){ .resolve = cb }, user));

  ttl = (uint32_t)kv_get_uint("plugin.wikimedia.cache_ttl");

  if(ttl > 0)
  {
    wm_resolve_res_t cached;

    if(wm_resolve_cache_get(key, &cached, time(NULL), ttl))
    {
      cb(&cached, user);
      return(ASYNC_AIRBORNE);
    }
  }

  w = wm_work_open(WM_VERB_RESOLVE, user);
  w->cb.resolve = cb;
  strlcpy(w->u.resolve.key, key, sizeof(w->u.resolve.key));

  // Neither leg has answered yet, and a leg whose launch fails never
  // will — WM_OK is 0, so "nobody answered" has to be written down.
  w->u.resolve.cirrus_st = WM_TRANSPORT;
  w->u.resolve.wbs_st    = WM_TRANSPORT;

  // The +1 guard keeps `w` alive across both submits even when the
  // first leg completes before the second is built.
  atomic_store(&w->pending, 3);

  need = snprintf(url, sizeof(url),
      "%s?action=query&list=search&srsearch=%s&srlimit=%d&srnamespace=0"
      "&format=json&formatversion=2",
      wm_api_base(), enc, WM_SEARCH_LIMIT);

  flew_a = need > 0 && (size_t)need < sizeof(url)
      && wm_launch(w, 0, url, wm_resolve_cirrus_done, w) == SUCCESS;

  need = snprintf(url, sizeof(url),
      "%s?action=wbsearchentities&search=%s&language=%s&uselang=%s&limit=%d"
      "&format=json&formatversion=2",
      wm_api_base(), enc, kv_get_str("plugin.wikimedia.language"),
      kv_get_str("plugin.wikimedia.language"), WM_SEARCH_LIMIT);

  flew_b = need > 0 && (size_t)need < sizeof(url)
      && wm_launch(w, 1, url, wm_resolve_wbs_done, w) == SUCCESS;

  if(!flew_a && !flew_b)
  {
    wm_work_abandon(w);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  if(!flew_a)
    wm_leg_retire(w);

  if(!flew_b)
    wm_leg_retire(w);

  wm_leg_retire(w);  // the guard; `w` may be gone from here on
  return(ASYNC_AIRBORNE);
}

async_rc_t
wm_claims_async(const char *qid, const char *property, wm_claims_cb_t cb,
    void *user)
{
  wm_work_t *w;
  char       word[WM_QUERY_SZ];

  if(cb == NULL || !wm_is_qid(qid, 'Q') || property == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  wm_normalize(property, word, sizeof(word));

  if(word[0] == '\0')
    return(ASYNC_FAILED_UNDELIVERED);

  if(wm_retry_after() > 0)
    return(wm_backoff_refuse(WM_VERB_CLAIMS,
        (wm_cb_u){ .claims = cb }, user));

  w = wm_work_open(WM_VERB_CLAIMS, user);
  w->cb.claims = cb;
  strlcpy(w->u.claims.word, word, sizeof(w->u.claims.word));
  wm_qid_norm(qid, w->u.claims.res.entity, WM_QID_SZ);

  // A P-id names its property outright; its label and datatype are
  // filled in by the batched lookup that follows the statements.
  if(wm_is_qid(property, 'P'))
  {
    wm_qid_norm(property, w->u.claims.cand[0].property, WM_QID_SZ);
    w->u.claims.n_cand = 1;
    wm_claims_dispatch(w);
    return(ASYNC_AIRBORNE);
  }

  if(wm_prop_resolve(w, word) != SUCCESS)
  {
    wm_work_abandon(w);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

async_rc_t
wm_menu_async(const char *class_qid, wm_menu_cb_t cb, void *user)
{
  wm_work_t *w;
  char       id[WM_QID_SZ];
  char       url[WM_URL_SZ];
  uint32_t   ttl;
  int        need;

  if(cb == NULL || !wm_is_qid(class_qid, 'Q'))
    return(ASYNC_FAILED_UNDELIVERED);

  if(wm_retry_after() > 0)
    return(wm_backoff_refuse(WM_VERB_MENU, (wm_cb_u){ .menu = cb }, user));

  wm_qid_norm(class_qid, id, sizeof(id));
  ttl = (uint32_t)kv_get_uint("plugin.wikimedia.cache_ttl");

  if(ttl > 0)
  {
    wm_menu_res_t cached;

    if(wm_menu_cache_get(id, &cached, time(NULL), ttl))
    {
      cb(&cached, user);
      return(ASYNC_AIRBORNE);
    }
  }

  w = wm_work_open(WM_VERB_MENU, user);
  w->cb.menu = cb;
  strlcpy(w->u.menu.res.class_qid, id, WM_QID_SZ);

  need = snprintf(url, sizeof(url),
      "%s?action=wbgetclaims&entity=%s&property=P1963&props=&format=json"
      "&formatversion=2", wm_api_base(), id);

  if(need < 0 || (size_t)need >= sizeof(url)
      || wm_launch(w, 0, url, wm_menu_props_done, w) != SUCCESS)
  {
    wm_work_abandon(w);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

async_rc_t
wm_prose_async(const char *title, bool full, wm_prose_cb_t cb, void *user)
{
  wm_work_t *w;
  char       url[WM_URL_SZ];
  int        need;

  if(cb == NULL || title == NULL || title[0] == '\0')
    return(ASYNC_FAILED_UNDELIVERED);

  if(wm_retry_after() > 0)
    return(wm_backoff_refuse(WM_VERB_PROSE, (wm_cb_u){ .prose = cb }, user));

  w = wm_work_open(WM_VERB_PROSE, user);
  w->cb.prose    = cb;
  w->u.prose.full = full;

  if(!wm_is_qid(title, 'Q'))
  {
    if(wm_prose_fetch(w, title) != SUCCESS)
    {
      wm_work_abandon(w);
      return(ASYNC_FAILED_UNDELIVERED);
    }

    return(ASYNC_AIRBORNE);
  }

  wm_qid_norm(title, w->u.prose.res.qid, WM_QID_SZ);

  need = snprintf(url, sizeof(url),
      "%s?action=wbgetentities&ids=%s&props=sitelinks&sitefilter=%swiki"
      "&format=json&formatversion=2",
      wm_api_base(), w->u.prose.res.qid,
      kv_get_str("plugin.wikimedia.language"));

  if(need < 0 || (size_t)need >= sizeof(url)
      || wm_launch(w, 0, url, wm_prose_sitelink_done, w) != SUCCESS)
  {
    wm_work_abandon(w);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

async_rc_t
wm_property_async(const char *word, wm_property_cb_t cb, void *user)
{
  wm_work_t *w;
  char       norm[WM_QUERY_SZ];

  if(cb == NULL || word == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  wm_normalize(word, norm, sizeof(norm));

  if(norm[0] == '\0')
    return(ASYNC_FAILED_UNDELIVERED);

  if(wm_retry_after() > 0)
    return(wm_backoff_refuse(WM_VERB_PROPERTY,
        (wm_cb_u){ .property = cb }, user));

  w = wm_work_open(WM_VERB_PROPERTY, user);
  w->cb.property = cb;
  strlcpy(w->u.property.word, norm, sizeof(w->u.property.word));
  strlcpy(w->u.property.res.word, norm, sizeof(w->u.property.res.word));

  // A P-id needs no search: it is the answer, and the label and
  // datatype a caller may want are one wbgetentities away if it ever
  // does. Nobody has asked, so nothing is spent.
  if(wm_is_qid(word, 'P'))
  {
    wm_qid_norm(word, w->u.property.res.cand[0].property, WM_QID_SZ);
    w->u.property.res.n      = 1;
    w->u.property.res.status = WM_OK;
    wm_work_finish(w);
    return(ASYNC_AIRBORNE);
  }

  if(wm_prop_resolve(w, norm) != SUCCESS)
  {
    wm_work_abandon(w);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

async_rc_t
wm_facts_async(const char *qid, wm_facts_cb_t cb, void *user)
{
  wm_work_t *w;
  char       url[WM_URL_SZ];
  int        need;

  if(cb == NULL || !wm_is_qid(qid, 'Q'))
    return(ASYNC_FAILED_UNDELIVERED);

  if(wm_retry_after() > 0)
    return(wm_backoff_refuse(WM_VERB_FACTS,
        (wm_cb_u){ .facts = cb }, user));

  w = wm_work_open(WM_VERB_FACTS, user);
  w->cb.facts = cb;
  wm_qid_norm(qid, w->u.facts.res.qid, WM_QID_SZ);

  need = snprintf(url, sizeof(url),
      "%s?action=wbgetclaims&entity=%s&property=P31&props=&format=json"
      "&formatversion=2", wm_api_base(), w->u.facts.res.qid);

  if(need < 0 || (size_t)need >= sizeof(url)
      || wm_launch(w, 0, url, wm_facts_p31_done, w) != SUCCESS)
  {
    wm_work_abandon(w);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

async_rc_t
wm_reverse_async(const char *property, const char *value_qid,
    wm_resolve_cb_t cb, void *user)
{
  wm_work_t *w;
  char       word[WM_QUERY_SZ];

  if(cb == NULL || property == NULL || !wm_is_qid(value_qid, 'Q'))
    return(ASYNC_FAILED_UNDELIVERED);

  wm_normalize(property, word, sizeof(word));

  if(word[0] == '\0')
    return(ASYNC_FAILED_UNDELIVERED);

  if(wm_retry_after() > 0)
    return(wm_backoff_refuse(WM_VERB_REVERSE,
        (wm_cb_u){ .resolve = cb }, user));

  w = wm_work_open(WM_VERB_REVERSE, user);
  w->cb.resolve = cb;
  strlcpy(w->u.reverse.word, word, sizeof(w->u.reverse.word));
  wm_qid_norm(value_qid, w->u.reverse.value, WM_QID_SZ);

  if(wm_is_qid(property, 'P'))
  {
    wm_qid_norm(property, w->u.reverse.cand[0].property, WM_QID_SZ);
    w->u.reverse.n_cand = 1;
    wm_reverse_dispatch(w);
    return(ASYNC_AIRBORNE);
  }

  if(wm_prop_resolve(w, word) != SUCCESS)
  {
    wm_work_abandon(w);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

// ----------------------------------------------------------------------
// Plugin lifecycle
// ----------------------------------------------------------------------

static bool
wm_init(void)
{
  pthread_mutex_init(&wm_cache_mu, NULL);
  curl_flight_init(&wm_flight);
  atomic_init(&wm_backoff_until, 0);
  memset(wm_resolve_cache, 0, sizeof(wm_resolve_cache));
  memset(wm_menu_cache, 0, sizeof(wm_menu_cache));
  memset(wm_prop_cache, 0, sizeof(wm_prop_cache));
  wm_resolve_cursor = 0;
  wm_menu_cursor    = 0;
  wm_prop_cursor    = 0;

  plugin_unmap_notify_register(wm_unmap_cb, NULL);

  clam(CLAM_INFO, WIKIMEDIA_CTX, "wikimedia plugin initialized");
  return(SUCCESS);
}

// The plugin's Class-B holding is its in-flight curl set. A cancelled
// request still delivers, so what this waits for is our own callbacks
// finishing — not the transfers.
static bool
wm_stop(void)
{
  uint32_t left = curl_flight_drain(&wm_flight, WM_STOP_DRAIN_MS);

  if(left == 0)
    return(SUCCESS);

  clam(CLAM_WARN, WIKIMEDIA_CTX, "%u request(s) still airborne after a %u ms "
      "cancel-and-drain; refusing the unload rather than deinitializing "
      "under their callbacks", left, (uint32_t)WM_STOP_DRAIN_MS);

  return(FAIL);
}

static void
wm_deinit(void)
{
  plugin_unmap_notify_unregister(wm_unmap_cb);
  curl_flight_destroy(&wm_flight);
  pthread_mutex_destroy(&wm_cache_mu);
  clam(CLAM_INFO, WIKIMEDIA_CTX, "wikimedia plugin deinitialized");
}

// It requires nothing and registers no user command. This service is
// non-leaf — the inference engine acquires corpora through it — so its
// command surface lives in a plugin of its own, `PLUGIN.md §Layer Rules`
// Rule 1. (Said without the symbol's name, so the leafness grep audit in
// plugins/service/AGENTS.md does not read this comment as a violation.)
const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = WIKIMEDIA_CTX,
  .version         = "1.0",
  .type            = PLUGIN_SERVICE,
  .kind            = WIKIMEDIA_CTX,
  .provides        = { { .name = "service_wikimedia" } },
  .provides_count  = 1,
  .requires_count  = 0,
  .kv_schema       = wm_kv_schema,
  .kv_schema_count = sizeof(wm_kv_schema) / sizeof(wm_kv_schema[0]),
  .init            = wm_init,
  .start           = NULL,
  .stop            = wm_stop,
  .deinit          = wm_deinit,
  .ext             = NULL,
};
