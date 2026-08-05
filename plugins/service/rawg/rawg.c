// botmanager — MIT
// RAWG service plugin: keyed (?key= query param) access to the RAWG
// video-games database (rawg.io). Fetches and normalizes game detail,
// free-text search, and ranked lists behind the rawg_api.h mechanism
// contract. Pure connectivity — the plugin's command surface half
// (rawg_cmd.c) owns all presentation. All RAWG nuance (endpoint shapes,
// the swagger's under-documented nested arrays, date-window lists) is
// sealed here.
//
// The API key lands in the request URL (?key=…). Never clam() an assembled
// request URL — it would leak the key to the logs.
#define RAWG_INTERNAL
#include "rawg.h"

#include "rawg_cmd.h"

// ----------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------

// Percent-encode `in` into `out`, RFC-3986 unreserved set kept verbatim.
// Everything else becomes %XX. Returns the number of bytes the full
// encoding needs (may exceed cap, like snprintf).
static size_t
rawg_urlencode(const char *in, char *out, size_t cap)
{
  static const char hex[] = "0123456789ABCDEF";
  size_t n = 0;

  if(cap == 0)
    return(0);

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

// Parse a leading "YYYY" out of an ISO date; 0 when absent or malformed.
static int32_t
rawg_year_of(const char *date)
{
  int y = 0;

  if(date == NULL)
    return(0);

  for(int i = 0; i < 4; i++)
  {
    if(date[i] < '0' || date[i] > '9')
      return(0);

    y = y * 10 + (date[i] - '0');
  }

  return((int32_t)y);
}

static rawg_status_t
rawg_status_of_http(long http, int curl_code)
{
  if(curl_code != 0)
    return(RAWG_TRANSPORT);

  switch(http)
  {
    case 200: return(RAWG_OK);
    case 401: return(RAWG_AUTH);
    case 403: return(RAWG_AUTH);   // RAWG rejects a bad key with 401 or 403
    case 404: return(RAWG_NOT_FOUND);
    case 429: return(RAWG_RATE_LIMITED);
    default:  return(RAWG_TRANSPORT);
  }
}

// Accept + per-request timeout. No auth header — the key rides the URL.
static void
rawg_apply_opts(curl_request_t *cr)
{
  uint32_t to;

  curl_request_add_header(cr, "Accept: application/json");

  to = (uint32_t)kv_get_uint("plugin.rawg.timeout");
  if(to > 0)
    curl_request_set_timeout(cr, to);
}

// ----------------------------------------------------------------------
// JSON extraction helpers
// ----------------------------------------------------------------------

// Join up to `max` values of `key` from an array of objects into `out`,
// comma-separated ("Action, RPG"). Bounded; never splits mid-write.
static void
rawg_join_names(struct json_object *arr, const char *key,
    char *out, size_t cap, int max)
{
  int    len;
  int    count = 0;
  size_t pos   = 0;

  if(cap == 0)
    return;

  out[0] = '\0';

  if(arr == NULL || !json_object_is_type(arr, json_type_array))
    return;

  len = (int)json_object_array_length(arr);

  for(int i = 0; i < len && count < max; i++)
  {
    struct json_object *item = json_object_array_get_idx(arr, i);
    char name[96];
    int  need;

    if(item == NULL)
      continue;

    if(!json_get_str(item, key, name, sizeof(name)) || name[0] == '\0')
      continue;

    need = snprintf(out + pos, cap - pos, "%s%s", count > 0 ? ", " : "", name);

    if(need < 0 || (size_t)need >= cap - pos)
    {
      out[cap - 1] = '\0';
      return;
    }

    pos += (size_t)need;
    count++;
  }
}

// Like rawg_join_names, but each array element wraps a nested object:
// results[].{obj_key}.{name_key}. Used for parent_platforms[].platform.name
// and stores[].store.name. De-duplicates (RAWG repeats platform families).
static void
rawg_join_nested(struct json_object *arr, const char *obj_key,
    const char *name_key, char *out, size_t cap, int max)
{
  char   seen[8][96];
  int    have  = 0;
  int    len;
  size_t pos   = 0;

  if(cap == 0)
    return;

  out[0] = '\0';

  if(arr == NULL || !json_object_is_type(arr, json_type_array))
    return;

  len = (int)json_object_array_length(arr);

  for(int i = 0; i < len && have < max; i++)
  {
    struct json_object *item = json_object_array_get_idx(arr, i);
    struct json_object *obj;
    char name[96];
    bool dup = false;
    int  need;

    if(item == NULL)
      continue;

    obj = json_get_obj(item, obj_key);

    if(obj == NULL)
      continue;

    if(!json_get_str(obj, name_key, name, sizeof(name)) || name[0] == '\0')
      continue;

    for(int j = 0; j < have; j++)
      if(strcmp(seen[j], name) == 0)
      {
        dup = true;
        break;
      }

    if(dup)
      continue;

    need = snprintf(out + pos, cap - pos, "%s%s", have > 0 ? ", " : "", name);

    if(need < 0 || (size_t)need >= cap - pos)
    {
      out[cap - 1] = '\0';
      return;
    }

    pos += (size_t)need;

    if(have < 8)
      snprintf(seen[have], sizeof(seen[have]), "%s", name);

    have++;
  }
}

// Join up to `max` english-language tag names (tags[].name where
// tags[].language == "eng"), comma-separated.
static void
rawg_join_eng_tags(struct json_object *arr, char *out, size_t cap, int max)
{
  int    len;
  int    count = 0;
  size_t pos   = 0;

  if(cap == 0)
    return;

  out[0] = '\0';

  if(arr == NULL || !json_object_is_type(arr, json_type_array))
    return;

  len = (int)json_object_array_length(arr);

  for(int i = 0; i < len && count < max; i++)
  {
    struct json_object *item = json_object_array_get_idx(arr, i);
    char lang[8];
    char name[64];
    int  need;

    if(item == NULL)
      continue;

    if(json_get_str(item, "language", lang, sizeof(lang))
        && strcasecmp(lang, "eng") != 0)
      continue;

    if(!json_get_str(item, "name", name, sizeof(name)) || name[0] == '\0')
      continue;

    need = snprintf(out + pos, cap - pos, "%s%s", count > 0 ? ", " : "", name);

    if(need < 0 || (size_t)need >= cap - pos)
    {
      out[cap - 1] = '\0';
      return;
    }

    pos += (size_t)need;
    count++;
  }
}

// Strip HTML tags from `in` into `out`, collapsing runs of whitespace to a
// single space. A last-ditch fallback when description_raw is absent.
static void
rawg_strip_html(const char *in, char *out, size_t cap)
{
  size_t pos    = 0;
  bool   in_tag = false;
  bool   pend_sp = false;   // a pending space, emitted lazily before content

  if(cap == 0)
    return;

  out[0] = '\0';

  if(in == NULL)
    return;

  for(const char *p = in; *p != '\0' && pos + 1 < cap; p++)
  {
    if(*p == '<')
    {
      in_tag = true;
      continue;
    }

    if(*p == '>')
    {
      in_tag  = false;
      pend_sp = pos > 0;   // a tag boundary acts as a word break
      continue;
    }

    if(in_tag)
      continue;

    if(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    {
      pend_sp = pos > 0;
      continue;
    }

    if(pend_sp && pos + 1 < cap)
    {
      out[pos++] = ' ';
      pend_sp    = false;
    }

    out[pos++] = *p;
  }

  out[pos] = '\0';
}

static void
rawg_parse_game(struct json_object *root, rawg_game_t *out)
{
  struct json_object *esrb;
  int32_t idv = 0;

  memset(out, 0, sizeof(*out));

  if(root == NULL)
    return;

  json_get_int(root, "id", &idv);
  out->id = idv;

  json_get_str(root, "slug", out->slug, sizeof(out->slug));
  json_get_str(root, "name", out->name, sizeof(out->name));
  json_get_str(root, "released", out->released, sizeof(out->released));
  out->year = rawg_year_of(out->released);
  json_get_bool(root, "tba", &out->tba);

  json_get_double(root, "rating", &out->rating);
  json_get_int(root, "rating_top", &out->rating_top);
  json_get_int(root, "ratings_count", &out->ratings_count);
  json_get_int(root, "metacritic", &out->metacritic);
  json_get_int(root, "playtime", &out->playtime);
  json_get_int(root, "achievements_count", &out->achievements_count);

  esrb = json_get_obj(root, "esrb_rating");
  if(esrb != NULL)
    json_get_str(esrb, "name", out->esrb, sizeof(out->esrb));

  rawg_join_nested(json_get_array(root, "parent_platforms"), "platform",
      "name", out->platforms, sizeof(out->platforms), RAWG_PLAT_MAX);
  rawg_join_names(json_get_array(root, "genres"), "name",
      out->genres, sizeof(out->genres), RAWG_GENRES_MAX);
  rawg_join_names(json_get_array(root, "developers"), "name",
      out->developers, sizeof(out->developers), RAWG_NAMES_MAX);
  rawg_join_names(json_get_array(root, "publishers"), "name",
      out->publishers, sizeof(out->publishers), RAWG_NAMES_MAX);
  rawg_join_nested(json_get_array(root, "stores"), "store", "name",
      out->stores, sizeof(out->stores), RAWG_STORES_MAX);
  rawg_join_eng_tags(json_get_array(root, "tags"), out->tags,
      sizeof(out->tags), RAWG_TAGS_MAX);

  json_get_str(root, "website", out->website, sizeof(out->website));
  json_get_str(root, "reddit_url", out->reddit_url, sizeof(out->reddit_url));
  json_get_str(root, "background_image", out->background_image,
      sizeof(out->background_image));

  // Prefer the plaintext description_raw; fall back to stripping the HTML
  // description when RAWG omits the raw variant.
  if(!json_get_str(root, "description_raw", out->description,
      sizeof(out->description)) || out->description[0] == '\0')
  {
    char html[RAWG_DESC_SZ * 4];

    if(json_get_str(root, "description", html, sizeof(html)))
      rawg_strip_html(html, out->description, sizeof(out->description));
  }
}

static uint8_t
rawg_parse_hits(struct json_object *results, rawg_hit_t *out, uint8_t cap)
{
  int     len;
  uint8_t kept = 0;

  if(results == NULL || !json_object_is_type(results, json_type_array))
    return(0);

  len = (int)json_object_array_length(results);

  for(int i = 0; i < len && kept < cap; i++)
  {
    struct json_object *item = json_object_array_get_idx(results, i);
    rawg_hit_t *h = &out[kept];

    if(item == NULL || !json_object_is_type(item, json_type_object))
      continue;

    memset(h, 0, sizeof(*h));

    json_get_int(item, "id", &h->id);
    json_get_str(item, "slug", h->slug, sizeof(h->slug));

    if(!json_get_str(item, "name", h->name, sizeof(h->name)))
      continue;

    json_get_str(item, "released", h->released, sizeof(h->released));
    h->year = rawg_year_of(h->released);

    json_get_double(item, "rating", &h->rating);
    json_get_int(item, "metacritic", &h->metacritic);
    json_get_int(item, "added", &h->added);
    json_get_int(item, "ratings_count", &h->ratings_count);

    rawg_join_nested(json_get_array(item, "parent_platforms"), "platform",
        "name", h->platforms, sizeof(h->platforms), RAWG_PLAT_MAX);
    rawg_join_names(json_get_array(item, "genres"), "name",
        h->genres, sizeof(h->genres), RAWG_GENRES_MAX);

    if(h->id <= 0 || h->name[0] == '\0')
      continue;

    kept++;
  }

  return(kept);
}

// ----------------------------------------------------------------------
// Caches (game detail + lists). One mutex covers both; callers copy out
// under the lock and fire callbacks outside it.
// ----------------------------------------------------------------------

static bool
rawg_game_cache_get(int32_t id, rawg_game_t *out, time_t now, uint32_t ttl)
{
  bool hit = false;

  pthread_mutex_lock(&rawg_cache_mu);

  for(size_t i = 0; i < RAWG_GAME_CACHE_SZ; i++)
    if(rawg_game_cache[i].t != 0
        && (now - rawg_game_cache[i].t) < (time_t)ttl
        && rawg_game_cache[i].v.id == id)
    {
      *out = rawg_game_cache[i].v;
      hit  = true;
      break;
    }

  pthread_mutex_unlock(&rawg_cache_mu);
  return(hit);
}

static void
rawg_game_cache_put(const rawg_game_t *v)
{
  size_t slot = RAWG_GAME_CACHE_SZ;

  if(v->id <= 0)
    return;

  pthread_mutex_lock(&rawg_cache_mu);

  for(size_t i = 0; i < RAWG_GAME_CACHE_SZ; i++)
    if(rawg_game_cache[i].t != 0 && rawg_game_cache[i].v.id == v->id)
    {
      slot = i;
      break;
    }

  if(slot == RAWG_GAME_CACHE_SZ)
  {
    slot             = rawg_game_cursor;
    rawg_game_cursor = (rawg_game_cursor + 1) % RAWG_GAME_CACHE_SZ;
  }

  rawg_game_cache[slot].v = *v;
  rawg_game_cache[slot].t = time(NULL);

  pthread_mutex_unlock(&rawg_cache_mu);
}

static bool
rawg_list_cache_get(const char *key, rawg_hit_t *out, uint8_t *n,
    time_t now, uint32_t ttl)
{
  bool hit = false;

  pthread_mutex_lock(&rawg_cache_mu);

  for(size_t i = 0; i < RAWG_LIST_CACHE_SZ; i++)
    if(rawg_list_cache[i].t != 0
        && (now - rawg_list_cache[i].t) < (time_t)ttl
        && strcmp(rawg_list_cache[i].key, key) == 0)
    {
      uint8_t cnt = rawg_list_cache[i].n;

      if(cnt > RAWG_HITS_MAX)
        cnt = RAWG_HITS_MAX;

      for(uint8_t j = 0; j < cnt; j++)
        out[j] = rawg_list_cache[i].hits[j];

      *n  = cnt;
      hit = true;
      break;
    }

  pthread_mutex_unlock(&rawg_cache_mu);
  return(hit);
}

static void
rawg_list_cache_put(const char *key, const rawg_hit_t *hits, uint8_t n)
{
  size_t slot = RAWG_LIST_CACHE_SZ;

  if(n > RAWG_HITS_MAX)
    n = RAWG_HITS_MAX;

  pthread_mutex_lock(&rawg_cache_mu);

  for(size_t i = 0; i < RAWG_LIST_CACHE_SZ; i++)
    if(rawg_list_cache[i].t != 0 && strcmp(rawg_list_cache[i].key, key) == 0)
    {
      slot = i;
      break;
    }

  if(slot == RAWG_LIST_CACHE_SZ)
  {
    slot             = rawg_list_cursor;
    rawg_list_cursor = (rawg_list_cursor + 1) % RAWG_LIST_CACHE_SZ;
  }

  snprintf(rawg_list_cache[slot].key, sizeof(rawg_list_cache[slot].key),
      "%s", key);

  for(uint8_t j = 0; j < n; j++)
    rawg_list_cache[slot].hits[j] = hits[j];

  rawg_list_cache[slot].n = n;
  rawg_list_cache[slot].t = time(NULL);

  pthread_mutex_unlock(&rawg_cache_mu);
}

// ----------------------------------------------------------------------
// Completion callbacks (fire on the curl worker thread)
// ----------------------------------------------------------------------

static void
rawg_search_done(const curl_response_t *resp)
{
  rawg_req_t         *req     = (rawg_req_t *)resp->user_data;
  struct json_object *root    = NULL;
  struct json_object *results = NULL;
  rawg_search_res_t   res;
  rawg_hit_t          hits[RAWG_HITS_MAX];

  memset(&res, 0, sizeof(res));
  res.status = rawg_status_of_http(resp->status, resp->curl_code);

  if(res.status != RAWG_OK)
    goto emit;

  root = json_parse_buf(resp->body, resp->body_len, RAWG_CTX);

  if(root == NULL)
  {
    res.status = RAWG_TRANSPORT;
    goto emit;
  }

  results  = json_get_array(root, "results");
  res.n    = rawg_parse_hits(results, hits, RAWG_HITS_MAX);
  res.hits = res.n > 0 ? hits : NULL;

  if(req->kind == RAWG_REQ_LIST && res.n > 0)
    rawg_list_cache_put(req->list_key, hits, res.n);

emit:
  if(req->search_cb != NULL)
    req->search_cb(&res, req->user);

  if(root != NULL)
    json_object_put(root);

  mem_free(req);
}

static void
rawg_game_done(const curl_response_t *resp)
{
  rawg_req_t         *req  = (rawg_req_t *)resp->user_data;
  struct json_object *root = NULL;
  rawg_game_res_t     res;

  memset(&res, 0, sizeof(res));
  res.status = rawg_status_of_http(resp->status, resp->curl_code);

  if(res.status != RAWG_OK)
    goto emit;

  root = json_parse_buf(resp->body, resp->body_len, RAWG_CTX);

  if(root == NULL)
  {
    res.status = RAWG_TRANSPORT;
    goto emit;
  }

  rawg_parse_game(root, &res.game);
  rawg_game_cache_put(&res.game);

emit:
  if(req->game_cb != NULL)
    req->game_cb(&res, req->user);

  if(root != NULL)
    json_object_put(root);

  mem_free(req);
}

// ----------------------------------------------------------------------
// URL builders (private; the key is appended last and never logged)
// ----------------------------------------------------------------------

// Effective page_size: KV clamped to [1, RAWG_HITS_MAX].
static uint32_t
rawg_page_size(void)
{
  uint32_t ps = (uint32_t)kv_get_uint("plugin.rawg.page_size");

  if(ps == 0)
    ps = 10;

  if(ps > RAWG_HITS_MAX)
    ps = RAWG_HITS_MAX;

  return(ps);
}

// Fill `dates` with the "from,to" window for the requested list, or leave it
// empty ("") to request no date filter at all. `year > 0` windows to that
// whole calendar year. With no year: POPULAR spans this year to date, NEW
// spans the trailing 30 days, and BEST spans ALL TIME (empty) — a partial
// current year carries almost no Metacritic scores, so a this-year default
// would come back empty; "best reviewed" reads as all-time anyway.
static void
rawg_list_dates(rawg_list_kind_t kind, int32_t year, char *dates, size_t cap)
{
  time_t    now = time(NULL);
  struct tm tmv;
  char      today[RAWG_DATE_SZ];

  if(cap == 0)
    return;

  dates[0] = '\0';

  gmtime_r(&now, &tmv);
  strftime(today, sizeof(today), "%Y-%m-%d", &tmv);

  if(year > 0)
  {
    snprintf(dates, cap, "%04d-01-01,%04d-12-31", year, year);
    return;
  }

  if(kind == RAWG_LIST_BEST)   // all-time best by Metacritic
    return;

  if(kind == RAWG_LIST_NEW)
  {
    time_t    from_t = now - (time_t)30 * 24 * 60 * 60;
    struct tm fv;
    char      from[RAWG_DATE_SZ];

    gmtime_r(&from_t, &fv);
    strftime(from, sizeof(from), "%Y-%m-%d", &fv);
    snprintf(dates, cap, "%s,%s", from, today);
    return;
  }

  snprintf(dates, cap, "%04d-01-01,%s", tmv.tm_year + 1900, today);
}

// ----------------------------------------------------------------------
// Provider API — the mechanism contract
// ----------------------------------------------------------------------

bool
rawg_configured(void)
{
  const char *tok = kv_get_creds("plugin.rawg.creds.apikey");

  return(tok != NULL && tok[0] != '\0');
}

bool
rawg_search_async(const char *query, rawg_search_cb_t cb, void *user)
{
  const char     *tok;
  char            enc[RAWG_ENC_SZ];
  char            url[RAWG_REQ_URL_SZ];
  int             need;
  curl_request_t *cr;
  rawg_req_t     *req;

  if(cb == NULL || query == NULL || query[0] == '\0')
    return(FAIL);

  tok = kv_get_creds("plugin.rawg.creds.apikey");

  if(tok == NULL || tok[0] == '\0')
    return(FAIL);

  if(rawg_urlencode(query, enc, sizeof(enc)) >= sizeof(enc))
    return(FAIL);

  need = snprintf(url, sizeof(url),
      "%s/games?search=%s&search_precise=true&page_size=%u&key=%s",
      RAWG_API_BASE, enc, rawg_page_size(), tok);

  if(need < 0 || (size_t)need >= sizeof(url))
    return(FAIL);

  req = mem_alloc(RAWG_CTX, "req", sizeof(*req));
  memset(req, 0, sizeof(*req));
  req->kind      = RAWG_REQ_SEARCH;
  req->search_cb = cb;
  req->user      = user;

  cr = curl_request_create(CURL_METHOD_GET, url, rawg_search_done, req);

  if(cr == NULL)
  {
    mem_free(req);
    return(FAIL);
  }

  rawg_apply_opts(cr);

  if(curl_request_submit(cr) != SUCCESS)
  {
    mem_free(req);
    return(FAIL);
  }

  return(SUCCESS);
}

bool
rawg_game_async(int32_t id, rawg_game_cb_t cb, void *user)
{
  const char     *tok;
  char            url[RAWG_REQ_URL_SZ];
  int             need;
  uint32_t        ttl;
  time_t          now;
  rawg_game_t     cached_v;
  curl_request_t *cr;
  rawg_req_t     *req;

  if(cb == NULL || id <= 0)
    return(FAIL);

  tok = kv_get_creds("plugin.rawg.creds.apikey");

  if(tok == NULL || tok[0] == '\0')
    return(FAIL);

  ttl = (uint32_t)kv_get_uint("plugin.rawg.cache_ttl");
  now = time(NULL);

  if(ttl > 0 && rawg_game_cache_get(id, &cached_v, now, ttl))
  {
    rawg_game_res_t res;

    memset(&res, 0, sizeof(res));
    res.status = RAWG_OK;
    res.game   = cached_v;
    cb(&res, user);
    return(SUCCESS);
  }

  need = snprintf(url, sizeof(url), "%s/games/%d?key=%s",
      RAWG_API_BASE, id, tok);

  if(need < 0 || (size_t)need >= sizeof(url))
    return(FAIL);

  req = mem_alloc(RAWG_CTX, "req", sizeof(*req));
  memset(req, 0, sizeof(*req));
  req->kind    = RAWG_REQ_GAME;
  req->id      = id;
  req->game_cb = cb;
  req->user    = user;

  cr = curl_request_create(CURL_METHOD_GET, url, rawg_game_done, req);

  if(cr == NULL)
  {
    mem_free(req);
    return(FAIL);
  }

  rawg_apply_opts(cr);

  if(curl_request_submit(cr) != SUCCESS)
  {
    mem_free(req);
    return(FAIL);
  }

  return(SUCCESS);
}

bool
rawg_list_async(rawg_list_kind_t kind, int32_t year, rawg_search_cb_t cb,
    void *user)
{
  const char     *tok;
  const char     *ord;
  const char     *prefix;
  const char     *extra;
  char            key[RAWG_LIST_KEY_SZ];
  char            dates[RAWG_DATES_SZ];
  char            dates_clause[RAWG_DATES_SZ + 8];
  char            url[RAWG_REQ_URL_SZ];
  int             need;
  uint32_t        ttl;
  time_t          now;
  rawg_hit_t      hits[RAWG_HITS_MAX];
  uint8_t         n = 0;
  curl_request_t *cr;
  rawg_req_t     *req;

  if(cb == NULL)
    return(FAIL);

  tok = kv_get_creds("plugin.rawg.creds.apikey");

  if(tok == NULL || tok[0] == '\0')
    return(FAIL);

  switch(kind)
  {
    case RAWG_LIST_BEST:
      ord    = "-metacritic";
      prefix = "best";
      extra  = "&metacritic=1,100";
      break;

    case RAWG_LIST_NEW:
      ord    = "-released";
      prefix = "new";
      extra  = "";
      break;

    case RAWG_LIST_POPULAR:
    default:
      ord    = "-added";
      prefix = "popular";
      extra  = "";
      break;
  }

  snprintf(key, sizeof(key), "%s:%d", prefix, year);

  ttl = (uint32_t)kv_get_uint("plugin.rawg.trending_ttl");
  now = time(NULL);

  if(ttl > 0 && rawg_list_cache_get(key, hits, &n, now, ttl))
  {
    rawg_search_res_t res;

    memset(&res, 0, sizeof(res));
    res.status = RAWG_OK;
    res.hits   = n > 0 ? hits : NULL;
    res.n      = n;
    cb(&res, user);
    return(SUCCESS);
  }

  rawg_list_dates(kind, year, dates, sizeof(dates));

  if(dates[0] != '\0')
    snprintf(dates_clause, sizeof(dates_clause), "&dates=%s", dates);
  else
    dates_clause[0] = '\0';

  need = snprintf(url, sizeof(url),
      "%s/games?ordering=%s%s%s&page_size=%u&key=%s",
      RAWG_API_BASE, ord, dates_clause, extra, rawg_page_size(), tok);

  if(need < 0 || (size_t)need >= sizeof(url))
    return(FAIL);

  req = mem_alloc(RAWG_CTX, "req", sizeof(*req));
  memset(req, 0, sizeof(*req));
  req->kind      = RAWG_REQ_LIST;
  req->search_cb = cb;
  req->user      = user;
  snprintf(req->list_key, sizeof(req->list_key), "%s", key);

  cr = curl_request_create(CURL_METHOD_GET, url, rawg_search_done, req);

  if(cr == NULL)
  {
    mem_free(req);
    return(FAIL);
  }

  rawg_apply_opts(cr);

  if(curl_request_submit(cr) != SUCCESS)
  {
    mem_free(req);
    return(FAIL);
  }

  return(SUCCESS);
}

// ----------------------------------------------------------------------
// Plugin lifecycle
// ----------------------------------------------------------------------

static bool
rawg_init(void)
{
  pthread_mutex_init(&rawg_cache_mu, NULL);
  memset(rawg_game_cache, 0, sizeof(rawg_game_cache));
  memset(rawg_list_cache, 0, sizeof(rawg_list_cache));
  rawg_game_cursor = 0;
  rawg_list_cursor = 0;

  // A plugin that cannot raise its own command surface is half-loaded,
  // which is worse than absent — fail the init and let the loader skip it.
  if(rawg_cmd_register() != SUCCESS)
  {
    pthread_mutex_destroy(&rawg_cache_mu);
    return(FAIL);
  }

  clam(CLAM_INFO, RAWG_CTX, "rawg plugin initialized");
  return(SUCCESS);
}

static void
rawg_deinit(void)
{
  rawg_cmd_unregister();
  pthread_mutex_destroy(&rawg_cache_mu);
  clam(CLAM_INFO, RAWG_CTX, "rawg plugin deinitialized");
}

// The command half's upward method_command dependency rides on this
// descriptor. Sound only because rawg is a dependency-graph leaf —
// nothing requires service_rawg, so nothing inherits it. See
// `PLUGIN.md §Layer Rules` Rule 1, leaf exception.
const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = RAWG_CTX,
  .version         = "1.0",
  .type            = PLUGIN_SERVICE,
  .kind            = RAWG_CTX,
  .provides        = { { .name = "service_rawg" } },
  .provides_count  = 1,
  .requires        = { { .name = "bot_chat" } },
  .requires_count  = 1,
  .kv_schema       = rawg_kv_schema,
  .kv_schema_count = sizeof(rawg_kv_schema) / sizeof(rawg_kv_schema[0]),
  .init            = rawg_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = rawg_deinit,
  .ext             = NULL,
};
