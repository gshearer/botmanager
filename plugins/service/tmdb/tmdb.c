// botmanager — MIT
// TMDB service plugin: keyed (v4 Bearer) access to The Movie Database.
// Fetches and normalizes movie/TV/person detail, multi-search, and
// trending lists behind the tmdb_api.h mechanism contract. Pure
// connectivity — the plugin's command surface half (tmdb_cmd.c) owns all
// presentation. All TMDB nuance (endpoint shapes, append_to_response,
// image/id conventions) is sealed here.
#define TMDB_INTERNAL
#include "tmdb.h"

#include "tmdb_cmd.h"

// ----------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------

// Percent-encode `in` into `out`, RFC-3986 unreserved set kept verbatim.
// Everything else (spaces, punctuation in titles) becomes %XX. Returns the
// number of bytes the full encoding needs (may exceed cap, like snprintf).
static size_t
tmdb_urlencode(const char *in, char *out, size_t cap)
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

static tmdb_media_t
tmdb_media_from_str(const char *s)
{
  if(s == NULL)
    return(TMDB_MEDIA_UNKNOWN);

  if(strcasecmp(s, "movie") == 0)
    return(TMDB_MEDIA_MOVIE);

  if(strcasecmp(s, "tv") == 0)
    return(TMDB_MEDIA_TV);

  if(strcasecmp(s, "person") == 0)
    return(TMDB_MEDIA_PERSON);

  return(TMDB_MEDIA_UNKNOWN);
}

// Parse a leading "YYYY" out of an ISO date; 0 when absent or malformed.
static int32_t
tmdb_year_of(const char *date)
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

static tmdb_status_t
tmdb_status_of_http(long http, int curl_code)
{
  if(curl_code != 0)
    return(TMDB_TRANSPORT);

  switch(http)
  {
    case 200: return(TMDB_OK);
    case 401: return(TMDB_AUTH);
    case 404: return(TMDB_NOT_FOUND);
    case 429: return(TMDB_RATE_LIMITED);
    default:  return(TMDB_TRANSPORT);
  }
}

// Attach the Bearer token, JSON Accept, and per-request timeout. The token
// is bounded by KV_STR_SZ, so the header always fits hdr[] — best-effort
// like the other services (curl_request_add_header failures are inert).
static void
tmdb_apply_auth(curl_request_t *cr, const char *token)
{
  char     hdr[TMDB_HDR_SZ];
  uint32_t to;

  snprintf(hdr, sizeof(hdr), "Authorization: Bearer %s", token);
  curl_request_add_header(cr, hdr);
  curl_request_add_header(cr, "Accept: application/json");

  to = (uint32_t)kv_get_uint("plugin.tmdb.timeout");
  if(to > 0)
    curl_request_set_timeout(cr, to);
}

// ----------------------------------------------------------------------
// JSON extraction helpers
// ----------------------------------------------------------------------

// Join up to `max` values of `key` from an array of objects into `out`,
// comma-separated ("Sci-Fi, Adventure"). Bounded; never splits mid-write.
static void
tmdb_join_names(struct json_object *arr, const char *key,
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

// Director (movie: credits.crew job == "Director") or creator (tv:
// created_by). Leaves out == "" when none is found.
static void
tmdb_find_director(struct json_object *root, bool tv, char *out, size_t cap)
{
  struct json_object *credits;
  struct json_object *crew;
  int len;

  if(cap == 0)
    return;

  out[0] = '\0';

  if(root == NULL)
    return;

  if(tv)
  {
    tmdb_join_names(json_get_array(root, "created_by"), "name", out, cap, 2);
    return;
  }

  credits = json_get_obj(root, "credits");
  crew    = credits != NULL ? json_get_array(credits, "crew") : NULL;

  if(crew == NULL)
    return;

  len = (int)json_object_array_length(crew);

  for(int i = 0; i < len; i++)
  {
    struct json_object *m = json_object_array_get_idx(crew, i);
    char job[32];

    if(m == NULL)
      continue;

    if(json_get_str(m, "job", job, sizeof(job)) && strcmp(job, "Director") == 0)
    {
      json_get_str(m, "name", out, cap);
      return;
    }
  }
}

// Pick the best YouTube video key: official trailer > trailer > teaser.
static void
tmdb_best_trailer(struct json_object *videos, char *out, size_t cap)
{
  struct json_object *results;
  int  len;
  int  best_rank = 0;   // 0 none, 1 teaser, 2 trailer, 3 official trailer
  char best_key[TMDB_TRAILER_SZ] = "";

  if(cap == 0)
    return;

  out[0] = '\0';

  if(videos == NULL)
    return;

  results = json_get_array(videos, "results");

  if(results == NULL)
    return;

  len = (int)json_object_array_length(results);

  for(int i = 0; i < len; i++)
  {
    struct json_object *v = json_object_array_get_idx(results, i);
    char site[24];
    char type[24] = "";
    char key[TMDB_TRAILER_SZ];
    bool official = false;
    int  rank;

    if(v == NULL)
      continue;

    if(!json_get_str(v, "site", site, sizeof(site))
        || strcasecmp(site, "YouTube") != 0)
      continue;

    if(!json_get_str(v, "key", key, sizeof(key)) || key[0] == '\0')
      continue;

    json_get_str(v, "type", type, sizeof(type));
    json_get_bool(v, "official", &official);

    if(strcasecmp(type, "Trailer") == 0)
      rank = official ? 3 : 2;

    else if(strcasecmp(type, "Teaser") == 0)
      rank = 1;

    else
      continue;

    if(rank > best_rank)
    {
      best_rank = rank;
      snprintf(best_key, sizeof(best_key), "%s", key);
    }
  }

  if(best_rank > 0)
    snprintf(out, cap, "%s", best_key);
}

// A person's notable titles: top TMDB_KNOWNFOR_MAX combined_credits.cast
// entries by popularity, de-duplicated, comma-joined most-popular first.
static void
tmdb_person_known_for(struct json_object *root, char *out, size_t cap)
{
  struct json_object *cc;
  struct json_object *cast;
  char   names[TMDB_KNOWNFOR_MAX][TMDB_TITLE_SZ];
  double pops[TMDB_KNOWNFOR_MAX];
  int    have = 0;
  int    len;
  size_t pos  = 0;

  if(cap == 0)
    return;

  out[0] = '\0';

  cc   = json_get_obj(root, "combined_credits");
  cast = cc != NULL ? json_get_array(cc, "cast") : NULL;

  if(cast == NULL)
    return;

  len = (int)json_object_array_length(cast);

  for(int i = 0; i < len; i++)
  {
    struct json_object *c = json_object_array_get_idx(cast, i);
    char   nm[TMDB_TITLE_SZ];
    char   ch[64];
    double pop = 0.0;
    bool   dup = false;
    int    slot;

    if(c == NULL)
      continue;

    if(!json_get_str(c, "title", nm, sizeof(nm))
        && !json_get_str(c, "name", nm, sizeof(nm)))
      continue;

    if(nm[0] == '\0')
      continue;

    // Skip "as self" credits (talk shows, award shows, documentaries) —
    // they carry huge popularity but aren't the roles a person is known for.
    if(json_get_str(c, "character", ch, sizeof(ch))
        && (strncasecmp(ch, "self", 4) == 0
            || strcasecmp(ch, "himself") == 0
            || strcasecmp(ch, "herself") == 0
            || strcasecmp(ch, "themselves") == 0))
      continue;

    for(int j = 0; j < have; j++)
      if(strcmp(names[j], nm) == 0)
      {
        dup = true;
        break;
      }

    if(dup)
      continue;

    json_get_double(c, "popularity", &pop);

    if(have < TMDB_KNOWNFOR_MAX)
      slot = have++;

    else
    {
      int minj = 0;

      for(int j = 1; j < have; j++)
        if(pops[j] < pops[minj])
          minj = j;

      if(pop <= pops[minj])
        continue;

      slot = minj;
    }

    pops[slot] = pop;
    snprintf(names[slot], sizeof(names[slot]), "%s", nm);
  }

  // Selection-sort the captured entries by popularity, descending.
  for(int a = 0; a < have; a++)
  {
    int  best = a;
    char tn[TMDB_TITLE_SZ];
    double tp;

    for(int b = a + 1; b < have; b++)
      if(pops[b] > pops[best])
        best = b;

    if(best == a)
      continue;

    tp = pops[a]; pops[a] = pops[best]; pops[best] = tp;
    snprintf(tn, sizeof(tn), "%s", names[a]);
    snprintf(names[a], sizeof(names[a]), "%s", names[best]);
    snprintf(names[best], sizeof(names[best]), "%s", tn);
  }

  for(int a = 0; a < have; a++)
  {
    int need = snprintf(out + pos, cap - pos, "%s%s",
        a > 0 ? ", " : "", names[a]);

    if(need < 0 || (size_t)need >= cap - pos)
    {
      out[cap - 1] = '\0';
      return;
    }

    pos += (size_t)need;
  }
}

static void
tmdb_parse_title(struct json_object *root, tmdb_media_t media,
    tmdb_title_t *out)
{
  struct json_object *credits;
  struct json_object *cast;
  struct json_object *videos;
  struct json_object *ext;
  const char *title_key;
  const char *orig_key;
  const char *date_key;
  int32_t     idv = 0;

  memset(out, 0, sizeof(*out));
  out->media = media;

  if(root == NULL)
    return;

  json_get_int(root, "id", &idv);
  out->id = idv;

  if(media == TMDB_MEDIA_TV)
  {
    title_key = "name";
    orig_key  = "original_name";
    date_key  = "first_air_date";
  }

  else
  {
    title_key = "title";
    orig_key  = "original_title";
    date_key  = "release_date";
  }

  json_get_str(root, title_key, out->title, sizeof(out->title));
  json_get_str(root, orig_key, out->original_title,
      sizeof(out->original_title));
  json_get_str(root, "tagline", out->tagline, sizeof(out->tagline));
  json_get_str(root, "overview", out->overview, sizeof(out->overview));
  json_get_str(root, date_key, out->release_date, sizeof(out->release_date));
  json_get_str(root, "status", out->status, sizeof(out->status));
  out->year = tmdb_year_of(out->release_date);

  json_get_double(root, "vote_average", &out->rating);
  json_get_int(root, "vote_count", &out->votes);
  json_get_double(root, "popularity", &out->popularity);

  tmdb_join_names(json_get_array(root, "genres"), "name",
      out->genres, sizeof(out->genres), 4);

  credits = json_get_obj(root, "credits");
  cast    = credits != NULL ? json_get_array(credits, "cast") : NULL;
  tmdb_join_names(cast, "name", out->cast, sizeof(out->cast), TMDB_CAST_MAX);

  tmdb_find_director(root, media == TMDB_MEDIA_TV, out->director,
      sizeof(out->director));

  videos = json_get_obj(root, "videos");
  tmdb_best_trailer(videos, out->trailer_key, sizeof(out->trailer_key));

  if(!json_get_str(root, "imdb_id", out->imdb_id, sizeof(out->imdb_id)))
  {
    ext = json_get_obj(root, "external_ids");
    if(ext != NULL)
      json_get_str(ext, "imdb_id", out->imdb_id, sizeof(out->imdb_id));
  }

  if(media == TMDB_MEDIA_TV)
  {
    struct json_object *ert;

    json_get_int(root, "number_of_seasons", &out->seasons);
    json_get_int(root, "number_of_episodes", &out->episodes);
    json_get_str(root, "last_air_date", out->last_air_date,
        sizeof(out->last_air_date));
    tmdb_join_names(json_get_array(root, "networks"), "name",
        out->networks, sizeof(out->networks), 3);

    ert = json_get_array(root, "episode_run_time");
    if(ert != NULL && json_object_array_length(ert) > 0)
    {
      struct json_object *e0 = json_object_array_get_idx(ert, 0);

      if(e0 != NULL)
        out->runtime = json_object_get_int(e0);
    }
  }

  else
  {
    json_get_int(root, "runtime", &out->runtime);
    json_get_int64(root, "budget", &out->budget);
    json_get_int64(root, "revenue", &out->revenue);
  }
}

static void
tmdb_parse_person(struct json_object *root, tmdb_person_t *out)
{
  struct json_object *ext;
  int32_t idv = 0;

  memset(out, 0, sizeof(*out));

  if(root == NULL)
    return;

  json_get_int(root, "id", &idv);
  out->id = idv;

  json_get_str(root, "name", out->name, sizeof(out->name));
  json_get_str(root, "known_for_department", out->known_for_dept,
      sizeof(out->known_for_dept));
  json_get_str(root, "birthday", out->birthday, sizeof(out->birthday));
  json_get_str(root, "deathday", out->deathday, sizeof(out->deathday));
  json_get_str(root, "place_of_birth", out->place_of_birth,
      sizeof(out->place_of_birth));
  json_get_str(root, "biography", out->biography, sizeof(out->biography));
  json_get_double(root, "popularity", &out->popularity);

  if(!json_get_str(root, "imdb_id", out->imdb_id, sizeof(out->imdb_id)))
  {
    ext = json_get_obj(root, "external_ids");
    if(ext != NULL)
      json_get_str(ext, "imdb_id", out->imdb_id, sizeof(out->imdb_id));
  }

  tmdb_person_known_for(root, out->known_for, sizeof(out->known_for));
}

static uint8_t
tmdb_parse_hits(struct json_object *results, tmdb_media_t media,
    tmdb_hit_t *out, uint8_t cap)
{
  int     len;
  uint8_t kept = 0;

  if(results == NULL || !json_object_is_type(results, json_type_array))
    return(0);

  len = (int)json_object_array_length(results);

  for(int i = 0; i < len && kept < cap; i++)
  {
    struct json_object *item = json_object_array_get_idx(results, i);
    tmdb_hit_t  *h  = &out[kept];
    tmdb_media_t im = media;
    char mt[16];
    char d[TMDB_DATE_SZ];

    if(item == NULL || !json_object_is_type(item, json_type_object))
      continue;

    if(im == TMDB_MEDIA_UNKNOWN && json_get_str(item, "media_type", mt,
        sizeof(mt)))
      im = tmdb_media_from_str(mt);

    memset(h, 0, sizeof(*h));
    h->media = im;
    json_get_int(item, "id", &h->id);
    json_get_double(item, "popularity", &h->popularity);

    if(im == TMDB_MEDIA_PERSON)
    {
      if(!json_get_str(item, "name", h->title, sizeof(h->title)))
        continue;

      json_get_str(item, "known_for_department", h->known_for_dept,
          sizeof(h->known_for_dept));
    }

    else if(im == TMDB_MEDIA_TV)
    {
      if(!json_get_str(item, "name", h->title, sizeof(h->title)))
        continue;

      json_get_double(item, "vote_average", &h->rating);

      if(json_get_str(item, "first_air_date", d, sizeof(d)))
        h->year = tmdb_year_of(d);
    }

    else if(im == TMDB_MEDIA_MOVIE)
    {
      if(!json_get_str(item, "title", h->title, sizeof(h->title)))
        continue;

      json_get_double(item, "vote_average", &h->rating);

      if(json_get_str(item, "release_date", d, sizeof(d)))
        h->year = tmdb_year_of(d);
    }

    else
      continue;   // collection / unknown media_type — skip

    if(h->id <= 0 || h->title[0] == '\0')
      continue;

    kept++;
  }

  return(kept);
}

// ----------------------------------------------------------------------
// Caches (movie/TV/person detail + trending). One mutex covers all three;
// callers copy out under the lock and fire callbacks outside it.
// ----------------------------------------------------------------------

static bool
tmdb_title_cache_get(tmdb_media_t m, int32_t id, tmdb_title_t *out,
    time_t now, uint32_t ttl)
{
  bool hit = false;

  pthread_mutex_lock(&tmdb_cache_mu);

  for(size_t i = 0; i < TMDB_TITLE_CACHE_SZ; i++)
    if(tmdb_title_cache[i].t != 0
        && (now - tmdb_title_cache[i].t) < (time_t)ttl
        && tmdb_title_cache[i].v.id == id
        && tmdb_title_cache[i].v.media == m)
    {
      *out = tmdb_title_cache[i].v;
      hit  = true;
      break;
    }

  pthread_mutex_unlock(&tmdb_cache_mu);
  return(hit);
}

static void
tmdb_title_cache_put(const tmdb_title_t *v)
{
  size_t slot = TMDB_TITLE_CACHE_SZ;

  if(v->id <= 0)
    return;

  pthread_mutex_lock(&tmdb_cache_mu);

  for(size_t i = 0; i < TMDB_TITLE_CACHE_SZ; i++)
    if(tmdb_title_cache[i].t != 0
        && tmdb_title_cache[i].v.id == v->id
        && tmdb_title_cache[i].v.media == v->media)
    {
      slot = i;
      break;
    }

  if(slot == TMDB_TITLE_CACHE_SZ)
  {
    slot              = tmdb_title_cursor;
    tmdb_title_cursor = (tmdb_title_cursor + 1) % TMDB_TITLE_CACHE_SZ;
  }

  tmdb_title_cache[slot].v = *v;
  tmdb_title_cache[slot].t = time(NULL);

  pthread_mutex_unlock(&tmdb_cache_mu);
}

static bool
tmdb_person_cache_get(int32_t id, tmdb_person_t *out, time_t now,
    uint32_t ttl)
{
  bool hit = false;

  pthread_mutex_lock(&tmdb_cache_mu);

  for(size_t i = 0; i < TMDB_PERSON_CACHE_SZ; i++)
    if(tmdb_person_cache[i].t != 0
        && (now - tmdb_person_cache[i].t) < (time_t)ttl
        && tmdb_person_cache[i].v.id == id)
    {
      *out = tmdb_person_cache[i].v;
      hit  = true;
      break;
    }

  pthread_mutex_unlock(&tmdb_cache_mu);
  return(hit);
}

static void
tmdb_person_cache_put(const tmdb_person_t *v)
{
  size_t slot = TMDB_PERSON_CACHE_SZ;

  if(v->id <= 0)
    return;

  pthread_mutex_lock(&tmdb_cache_mu);

  for(size_t i = 0; i < TMDB_PERSON_CACHE_SZ; i++)
    if(tmdb_person_cache[i].t != 0 && tmdb_person_cache[i].v.id == v->id)
    {
      slot = i;
      break;
    }

  if(slot == TMDB_PERSON_CACHE_SZ)
  {
    slot               = tmdb_person_cursor;
    tmdb_person_cursor = (tmdb_person_cursor + 1) % TMDB_PERSON_CACHE_SZ;
  }

  tmdb_person_cache[slot].v = *v;
  tmdb_person_cache[slot].t = time(NULL);

  pthread_mutex_unlock(&tmdb_cache_mu);
}

static bool
tmdb_trend_cache_get(const char *key, tmdb_hit_t *out, uint8_t *n,
    time_t now, uint32_t ttl)
{
  bool hit = false;

  pthread_mutex_lock(&tmdb_cache_mu);

  for(size_t i = 0; i < TMDB_TREND_CACHE_SZ; i++)
    if(tmdb_trend_cache[i].t != 0
        && (now - tmdb_trend_cache[i].t) < (time_t)ttl
        && strcmp(tmdb_trend_cache[i].key, key) == 0)
    {
      uint8_t cnt = tmdb_trend_cache[i].n;

      if(cnt > TMDB_HITS_MAX)
        cnt = TMDB_HITS_MAX;

      for(uint8_t j = 0; j < cnt; j++)
        out[j] = tmdb_trend_cache[i].hits[j];

      *n  = cnt;
      hit = true;
      break;
    }

  pthread_mutex_unlock(&tmdb_cache_mu);
  return(hit);
}

static void
tmdb_trend_cache_put(const char *key, const tmdb_hit_t *hits, uint8_t n)
{
  size_t slot = TMDB_TREND_CACHE_SZ;

  if(n > TMDB_HITS_MAX)
    n = TMDB_HITS_MAX;

  pthread_mutex_lock(&tmdb_cache_mu);

  for(size_t i = 0; i < TMDB_TREND_CACHE_SZ; i++)
    if(tmdb_trend_cache[i].t != 0 && strcmp(tmdb_trend_cache[i].key, key) == 0)
    {
      slot = i;
      break;
    }

  if(slot == TMDB_TREND_CACHE_SZ)
  {
    slot              = tmdb_trend_cursor;
    tmdb_trend_cursor = (tmdb_trend_cursor + 1) % TMDB_TREND_CACHE_SZ;
  }

  snprintf(tmdb_trend_cache[slot].key, sizeof(tmdb_trend_cache[slot].key),
      "%s", key);

  for(uint8_t j = 0; j < n; j++)
    tmdb_trend_cache[slot].hits[j] = hits[j];

  tmdb_trend_cache[slot].n = n;
  tmdb_trend_cache[slot].t = time(NULL);

  pthread_mutex_unlock(&tmdb_cache_mu);
}

// ----------------------------------------------------------------------
// Completion callbacks (fire on the curl worker thread)
// ----------------------------------------------------------------------

static void
tmdb_search_done(const curl_response_t *resp)
{
  tmdb_req_t         *req     = (tmdb_req_t *)resp->user_data;
  uint64_t            slot    = req->slot;
  struct json_object *root    = NULL;
  struct json_object *results = NULL;
  tmdb_search_res_t   res;
  tmdb_hit_t          hits[TMDB_HITS_MAX];

  memset(&res, 0, sizeof(res));
  res.status = tmdb_status_of_http(resp->status, resp->curl_code);

  if(res.status != TMDB_OK)
    goto emit;

  root = json_parse_buf(resp->body, resp->body_len, TMDB_CTX);

  if(root == NULL)
  {
    res.status = TMDB_TRANSPORT;
    goto emit;
  }

  results  = json_get_array(root, "results");
  res.n    = tmdb_parse_hits(results, req->media, hits, TMDB_HITS_MAX);
  res.hits = res.n > 0 ? hits : NULL;

  if(req->kind == TMDB_REQ_TRENDING && res.n > 0)
    tmdb_trend_cache_put(req->trend_key, hits, res.n);

emit:
  if(req->search_cb != NULL)
    req->search_cb(&res, req->user);

  if(root != NULL)
    json_object_put(root);

  mem_free(req);

  // Last, always: the drain in tmdb_stop() is waiting on this line, and
  // everything above it reads state tmdb_deinit() is about to tear down.
  curl_flight_close(&tmdb_flight, slot);
}

static void
tmdb_title_done(const curl_response_t *resp)
{
  tmdb_req_t         *req  = (tmdb_req_t *)resp->user_data;
  uint64_t            slot    = req->slot;
  struct json_object *root = NULL;
  tmdb_title_res_t    res;

  memset(&res, 0, sizeof(res));
  res.status = tmdb_status_of_http(resp->status, resp->curl_code);

  if(res.status != TMDB_OK)
    goto emit;

  root = json_parse_buf(resp->body, resp->body_len, TMDB_CTX);

  if(root == NULL)
  {
    res.status = TMDB_TRANSPORT;
    goto emit;
  }

  tmdb_parse_title(root, req->media, &res.title);
  tmdb_title_cache_put(&res.title);

emit:
  if(req->title_cb != NULL)
    req->title_cb(&res, req->user);

  if(root != NULL)
    json_object_put(root);

  mem_free(req);

  curl_flight_close(&tmdb_flight, slot);
}

static void
tmdb_person_done(const curl_response_t *resp)
{
  tmdb_req_t         *req  = (tmdb_req_t *)resp->user_data;
  uint64_t            slot    = req->slot;
  struct json_object *root = NULL;
  tmdb_person_res_t   res;

  memset(&res, 0, sizeof(res));
  res.status = tmdb_status_of_http(resp->status, resp->curl_code);

  if(res.status != TMDB_OK)
    goto emit;

  root = json_parse_buf(resp->body, resp->body_len, TMDB_CTX);

  if(root == NULL)
  {
    res.status = TMDB_TRANSPORT;
    goto emit;
  }

  tmdb_parse_person(root, &res.person);
  tmdb_person_cache_put(&res.person);

emit:
  if(req->person_cb != NULL)
    req->person_cb(&res, req->user);

  if(root != NULL)
    json_object_put(root);

  mem_free(req);

  curl_flight_close(&tmdb_flight, slot);
}

// ----------------------------------------------------------------------
// Provider API — the mechanism contract
// ----------------------------------------------------------------------

bool
tmdb_configured(void)
{
  const char *tok = kv_get_creds("plugin.tmdb.creds.apikey");

  return(tok != NULL && tok[0] != '\0');
}

async_rc_t
tmdb_search_async(tmdb_media_t kind, const char *query,
    tmdb_search_cb_t cb, void *user)
{
  const char     *tok;
  const char     *lang;
  const char     *path;
  char            enc[TMDB_ENC_SZ];
  char            url[TMDB_URL_SZ];
  int             need;
  curl_request_t *cr;
  tmdb_req_t     *req;

  if(cb == NULL || query == NULL || query[0] == '\0')
    return(ASYNC_FAILED_UNDELIVERED);

  tok = kv_get_creds("plugin.tmdb.creds.apikey");

  if(tok == NULL || tok[0] == '\0')
    return(ASYNC_FAILED_UNDELIVERED);

  if(tmdb_urlencode(query, enc, sizeof(enc)) >= sizeof(enc))
    return(ASYNC_FAILED_UNDELIVERED);

  lang = kv_get_str("plugin.tmdb.language");

  if(lang == NULL || lang[0] == '\0')
    lang = "en-US";

  path = kind == TMDB_MEDIA_MOVIE  ? "movie"
       : kind == TMDB_MEDIA_TV     ? "tv"
       : kind == TMDB_MEDIA_PERSON ? "person"
       : "multi";

  need = snprintf(url, sizeof(url),
      "%s/search/%s?query=%s&include_adult=%s&language=%s&page=1",
      TMDB_API_BASE, path, enc,
      kv_get_uint("plugin.tmdb.include_adult") ? "true" : "false", lang);

  if(need < 0 || (size_t)need >= sizeof(url))
    return(ASYNC_FAILED_UNDELIVERED);

  req = mem_alloc(TMDB_CTX, "req", sizeof(*req));
  memset(req, 0, sizeof(*req));
  req->kind      = TMDB_REQ_SEARCH;
  req->media     = kind;
  req->search_cb = cb;
  req->user      = user;

  // Opened before the request exists: a completion can run on a curl
  // worker before the submit has returned, so the slot has to be there
  // for it to close. FAIL means tmdb is stopping and will not start work
  // whose callback it cannot wait out.
  if(curl_flight_open(&tmdb_flight, &req->slot) != SUCCESS)
  {
    mem_free(req);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  cr = curl_request_create(CURL_METHOD_GET, url, tmdb_search_done, req);

  if(cr == NULL)
  {
    curl_flight_close(&tmdb_flight, req->slot);
    mem_free(req);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  tmdb_apply_auth(cr, tok);

  if(curl_flight_relay(&tmdb_flight, cr, &req->slot) != SUCCESS)
  {
    curl_flight_close(&tmdb_flight, req->slot);
    mem_free(req);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

async_rc_t
tmdb_title_async(tmdb_media_t kind, int32_t id, tmdb_title_cb_t cb, void *user)
{
  const char     *tok;
  const char     *lang;
  const char     *path;
  char            url[TMDB_URL_SZ];
  int             need;
  uint32_t        ttl;
  time_t          now;
  tmdb_title_t    cached_v;
  curl_request_t *cr;
  tmdb_req_t     *req;

  if(cb == NULL || id <= 0)
    return(ASYNC_FAILED_UNDELIVERED);

  if(kind != TMDB_MEDIA_MOVIE && kind != TMDB_MEDIA_TV)
    return(ASYNC_FAILED_UNDELIVERED);

  tok = kv_get_creds("plugin.tmdb.creds.apikey");

  if(tok == NULL || tok[0] == '\0')
    return(ASYNC_FAILED_UNDELIVERED);

  ttl = (uint32_t)kv_get_uint("plugin.tmdb.cache_ttl");
  now = time(NULL);

  if(ttl > 0 && tmdb_title_cache_get(kind, id, &cached_v, now, ttl))
  {
    tmdb_title_res_t res;

    memset(&res, 0, sizeof(res));
    res.status = TMDB_OK;
    res.title  = cached_v;
    cb(&res, user);
    return(ASYNC_AIRBORNE);
  }

  lang = kv_get_str("plugin.tmdb.language");

  if(lang == NULL || lang[0] == '\0')
    lang = "en-US";

  path = kind == TMDB_MEDIA_TV ? "tv" : "movie";

  need = snprintf(url, sizeof(url),
      "%s/%s/%d?append_to_response=credits,videos,external_ids&language=%s",
      TMDB_API_BASE, path, id, lang);

  if(need < 0 || (size_t)need >= sizeof(url))
    return(ASYNC_FAILED_UNDELIVERED);

  req = mem_alloc(TMDB_CTX, "req", sizeof(*req));
  memset(req, 0, sizeof(*req));
  req->kind     = TMDB_REQ_TITLE;
  req->media    = kind;
  req->id       = id;
  req->title_cb = cb;
  req->user     = user;

  if(curl_flight_open(&tmdb_flight, &req->slot) != SUCCESS)
  {
    mem_free(req);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  cr = curl_request_create(CURL_METHOD_GET, url, tmdb_title_done, req);

  if(cr == NULL)
  {
    curl_flight_close(&tmdb_flight, req->slot);
    mem_free(req);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  tmdb_apply_auth(cr, tok);

  if(curl_flight_relay(&tmdb_flight, cr, &req->slot) != SUCCESS)
  {
    curl_flight_close(&tmdb_flight, req->slot);
    mem_free(req);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

async_rc_t
tmdb_person_async(int32_t id, tmdb_person_cb_t cb, void *user)
{
  const char     *tok;
  const char     *lang;
  char            url[TMDB_URL_SZ];
  int             need;
  uint32_t        ttl;
  time_t          now;
  tmdb_person_t   cached_v;
  curl_request_t *cr;
  tmdb_req_t     *req;

  if(cb == NULL || id <= 0)
    return(ASYNC_FAILED_UNDELIVERED);

  tok = kv_get_creds("plugin.tmdb.creds.apikey");

  if(tok == NULL || tok[0] == '\0')
    return(ASYNC_FAILED_UNDELIVERED);

  ttl = (uint32_t)kv_get_uint("plugin.tmdb.cache_ttl");
  now = time(NULL);

  if(ttl > 0 && tmdb_person_cache_get(id, &cached_v, now, ttl))
  {
    tmdb_person_res_t res;

    memset(&res, 0, sizeof(res));
    res.status = TMDB_OK;
    res.person = cached_v;
    cb(&res, user);
    return(ASYNC_AIRBORNE);
  }

  lang = kv_get_str("plugin.tmdb.language");

  if(lang == NULL || lang[0] == '\0')
    lang = "en-US";

  need = snprintf(url, sizeof(url),
      "%s/person/%d?append_to_response=combined_credits,external_ids"
      "&language=%s",
      TMDB_API_BASE, id, lang);

  if(need < 0 || (size_t)need >= sizeof(url))
    return(ASYNC_FAILED_UNDELIVERED);

  req = mem_alloc(TMDB_CTX, "req", sizeof(*req));
  memset(req, 0, sizeof(*req));
  req->kind      = TMDB_REQ_PERSON;
  req->id        = id;
  req->person_cb = cb;
  req->user      = user;

  if(curl_flight_open(&tmdb_flight, &req->slot) != SUCCESS)
  {
    mem_free(req);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  cr = curl_request_create(CURL_METHOD_GET, url, tmdb_person_done, req);

  if(cr == NULL)
  {
    curl_flight_close(&tmdb_flight, req->slot);
    mem_free(req);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  tmdb_apply_auth(cr, tok);

  if(curl_flight_relay(&tmdb_flight, cr, &req->slot) != SUCCESS)
  {
    curl_flight_close(&tmdb_flight, req->slot);
    mem_free(req);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

async_rc_t
tmdb_trending_async(tmdb_media_t kind, bool weekly, tmdb_search_cb_t cb,
    void *user)
{
  const char     *tok;
  const char     *lang;
  const char     *path;
  const char     *win;
  char            key[TMDB_TREND_KEY_SZ];
  char            url[TMDB_URL_SZ];
  int             need;
  uint32_t        ttl;
  time_t          now;
  tmdb_hit_t      hits[TMDB_HITS_MAX];
  uint8_t         n = 0;
  curl_request_t *cr;
  tmdb_req_t     *req;

  if(cb == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  tok = kv_get_creds("plugin.tmdb.creds.apikey");

  if(tok == NULL || tok[0] == '\0')
    return(ASYNC_FAILED_UNDELIVERED);

  path = kind == TMDB_MEDIA_MOVIE ? "movie"
       : kind == TMDB_MEDIA_TV    ? "tv"
       : "all";
  win  = weekly ? "week" : "day";
  snprintf(key, sizeof(key), "%s:%s", path, win);

  ttl = (uint32_t)kv_get_uint("plugin.tmdb.trending_ttl");
  now = time(NULL);

  if(ttl > 0 && tmdb_trend_cache_get(key, hits, &n, now, ttl))
  {
    tmdb_search_res_t res;

    memset(&res, 0, sizeof(res));
    res.status = TMDB_OK;
    res.hits   = n > 0 ? hits : NULL;
    res.n      = n;
    cb(&res, user);
    return(ASYNC_AIRBORNE);
  }

  lang = kv_get_str("plugin.tmdb.language");

  if(lang == NULL || lang[0] == '\0')
    lang = "en-US";

  need = snprintf(url, sizeof(url), "%s/trending/%s/%s?language=%s",
      TMDB_API_BASE, path, win, lang);

  if(need < 0 || (size_t)need >= sizeof(url))
    return(ASYNC_FAILED_UNDELIVERED);

  req = mem_alloc(TMDB_CTX, "req", sizeof(*req));
  memset(req, 0, sizeof(*req));
  req->kind      = TMDB_REQ_TRENDING;
  req->media     = kind;   // UNKNOWN => per-item media_type from the response
  req->search_cb = cb;
  req->user      = user;
  snprintf(req->trend_key, sizeof(req->trend_key), "%s", key);

  if(curl_flight_open(&tmdb_flight, &req->slot) != SUCCESS)
  {
    mem_free(req);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  cr = curl_request_create(CURL_METHOD_GET, url, tmdb_search_done, req);

  if(cr == NULL)
  {
    curl_flight_close(&tmdb_flight, req->slot);
    mem_free(req);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  tmdb_apply_auth(cr, tok);

  if(curl_flight_relay(&tmdb_flight, cr, &req->slot) != SUCCESS)
  {
    curl_flight_close(&tmdb_flight, req->slot);
    mem_free(req);
    return(ASYNC_FAILED_UNDELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

// ----------------------------------------------------------------------
// Plugin lifecycle
// ----------------------------------------------------------------------

static bool
tmdb_init(void)
{
  pthread_mutex_init(&tmdb_cache_mu, NULL);
  curl_flight_init(&tmdb_flight);
  memset(tmdb_title_cache, 0, sizeof(tmdb_title_cache));
  memset(tmdb_person_cache, 0, sizeof(tmdb_person_cache));
  memset(tmdb_trend_cache, 0, sizeof(tmdb_trend_cache));
  tmdb_title_cursor  = 0;
  tmdb_person_cursor = 0;
  tmdb_trend_cursor  = 0;

  // A plugin that cannot raise its own command surface is half-loaded,
  // which is worse than absent — fail the init and let the loader skip it.
  if(tmdb_cmd_register() != SUCCESS)
  {
    curl_flight_destroy(&tmdb_flight);
    pthread_mutex_destroy(&tmdb_cache_mu);
    return(FAIL);
  }

  clam(CLAM_INFO, TMDB_CTX, "tmdb plugin initialized");
  return(SUCCESS);
}

// The plugin's whole Class-B holding is its in-flight curl set: no
// threads, no tasks, no bound vtable. A cancelled request still
// delivers, so what this waits for is tmdb's own callbacks finishing —
// not the transfers. Measured 2026-08-17: without it, a reload during
// tmdb_search_done left that callback's next trylock on tmdb_cache_mu
// returning EINVAL, and the cache write proceeded unguarded.
static bool
tmdb_stop(void)
{
  uint32_t left = curl_flight_drain(&tmdb_flight, TMDB_STOP_DRAIN_MS);

  if(left == 0)
    return(SUCCESS);

  clam(CLAM_WARN, TMDB_CTX, "%u request(s) still airborne after a %u ms "
      "cancel-and-drain; refusing the unload rather than deinitializing "
      "under their callbacks", left, (uint32_t)TMDB_STOP_DRAIN_MS);

  return(FAIL);
}

static void
tmdb_deinit(void)
{
  tmdb_cmd_unregister();
  curl_flight_destroy(&tmdb_flight);
  pthread_mutex_destroy(&tmdb_cache_mu);
  clam(CLAM_INFO, TMDB_CTX, "tmdb plugin deinitialized");
}

// The command half's upward method_command dependency rides on this
// descriptor. Sound only because tmdb is a dependency-graph leaf —
// nothing requires service_tmdb, so nothing inherits it. See
// `PLUGIN.md §Layer Rules` Rule 1, leaf exception.
const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = TMDB_CTX,
  .version         = "1.0",
  .type            = PLUGIN_SERVICE,
  .kind            = TMDB_CTX,
  .provides        = { { .name = "service_tmdb" } },
  .provides_count  = 1,
  .requires        = { { .name = "bot_chat" } },
  .requires_count  = 1,
  .kv_schema       = tmdb_kv_schema,
  .kv_schema_count = sizeof(tmdb_kv_schema) / sizeof(tmdb_kv_schema[0]),
  .init            = tmdb_init,
  .start           = NULL,
  .stop            = tmdb_stop,
  .deinit          = tmdb_deinit,
  .ext             = NULL,
};
