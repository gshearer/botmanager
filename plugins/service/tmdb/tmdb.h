#ifndef BM_TMDB_H
#define BM_TMDB_H

// Internal header for the tmdb service plugin. The public mechanism API is
// tmdb_api.h (resolved by consumers via plugin_dlsym_cached("tmdb", …));
// nothing outside tmdb.c pulls this file. Gated by TMDB_INTERNAL. tmdb.c
// defines TMDB_INTERNAL before including tmdb_api.h, so that header exposes
// the real provider prototypes rather than the dlsym shims.

#ifdef TMDB_INTERNAL

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "curl.h"
#include "json.h"
#include "kv.h"
#include "plugin.h"

#include "tmdb_api.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// Constants

#define TMDB_CTX          "tmdb"

#define TMDB_API_BASE     "https://api.themoviedb.org/3"

#define TMDB_URL_SZ       1024  // assembled request URL
#define TMDB_ENC_SZ       256   // URL-encoded query segment
#define TMDB_HDR_SZ       512   // "Authorization: Bearer <jwt>"
#define TMDB_CAST_MAX     4     // top billed names carried per title
#define TMDB_KNOWNFOR_MAX 5     // notable titles carried per person

// Detail caches (movie/TV/person are effectively static) + a trending
// cache (refreshes on its own TTL). t == 0 marks an empty slot.
#define TMDB_TITLE_CACHE_SZ   32
#define TMDB_PERSON_CACHE_SZ  16
#define TMDB_TREND_CACHE_SZ   8
#define TMDB_TREND_KEY_SZ     16   // "movie:week" and the like

typedef struct
{
  tmdb_title_t v;
  time_t       t;
} tmdb_title_ent_t;

typedef struct
{
  tmdb_person_t v;
  time_t        t;
} tmdb_person_ent_t;

typedef struct
{
  char       key[TMDB_TREND_KEY_SZ];
  tmdb_hit_t hits[TMDB_HITS_MAX];
  uint8_t    n;
  time_t     t;
} tmdb_trend_ent_t;

// Per-request closure. Single-shot (one HTTP request per call), so unlike
// yahoofinance's fan-out there is no pending counter — the one done
// callback frees it. `media` disambiguates parsing: for a search it marks
// typed vs multi; for a title detail it is MOVIE or TV; for trending it is
// the requested window's media (UNKNOWN → per-item media_type).
typedef enum
{
  TMDB_REQ_SEARCH,
  TMDB_REQ_TITLE,
  TMDB_REQ_PERSON,
  TMDB_REQ_TRENDING
} tmdb_req_kind_t;

typedef struct
{
  tmdb_req_kind_t  kind;
  tmdb_media_t     media;
  int32_t          id;                   // TITLE / PERSON
  char             trend_key[TMDB_TREND_KEY_SZ]; // TRENDING cache key
  tmdb_search_cb_t search_cb;            // SEARCH / TRENDING
  tmdb_title_cb_t  title_cb;             // TITLE
  tmdb_person_cb_t person_cb;            // PERSON
  void            *user;
} tmdb_req_t;

// Module state: detail + trending caches, one mutex covering all three.
static tmdb_title_ent_t  tmdb_title_cache[TMDB_TITLE_CACHE_SZ];
static tmdb_person_ent_t tmdb_person_cache[TMDB_PERSON_CACHE_SZ];
static tmdb_trend_ent_t  tmdb_trend_cache[TMDB_TREND_CACHE_SZ];
static uint32_t          tmdb_title_cursor  = 0;
static uint32_t          tmdb_person_cursor = 0;
static uint32_t          tmdb_trend_cursor  = 0;
static pthread_mutex_t   tmdb_cache_mu;

// KV schema

static const plugin_kv_entry_t tmdb_kv_schema[] = {
  { "plugin.tmdb.creds.apikey",         KV_STR, "",
    "TMDB v4 Read Access Token (sent as 'Authorization: Bearer'). Get one "
    "free at themoviedb.org → Settings → API.", NULL, NULL },
  { "plugin.tmdb.language",      KV_STR, "en-US",
    "ISO language for localized titles/overviews (e.g. en-US, fr-FR).",
    NULL, NULL },
  { "plugin.tmdb.cache_ttl",     KV_UINT32, "3600",
    "Movie/TV/person detail cache TTL (seconds); details are near-static.",
    NULL, NULL },
  { "plugin.tmdb.trending_ttl",  KV_UINT32, "900",
    "Trending list cache TTL (seconds).", NULL, NULL },
  { "plugin.tmdb.timeout",       KV_UINT32, "8",
    "Per-request HTTP timeout (seconds).", NULL, NULL },
  { "plugin.tmdb.include_adult", KV_BOOL, "false",
    "Include adult results in searches (SFW default).", NULL, NULL },
};

// Forward declarations

static size_t       tmdb_urlencode(const char *in, char *out, size_t cap);
static tmdb_media_t tmdb_media_from_str(const char *s);
static int32_t      tmdb_year_of(const char *date);
static tmdb_status_t tmdb_status_of_http(long http, int curl_code);
static void         tmdb_apply_auth(curl_request_t *cr, const char *token);

static void         tmdb_join_names(struct json_object *arr, const char *key,
                        char *out, size_t cap, int max);
static void         tmdb_find_director(struct json_object *root, bool tv,
                        char *out, size_t cap);
static void         tmdb_best_trailer(struct json_object *videos,
                        char *out, size_t cap);
static void         tmdb_person_known_for(struct json_object *root,
                        char *out, size_t cap);
static void         tmdb_parse_title(struct json_object *root,
                        tmdb_media_t media, tmdb_title_t *out);
static void         tmdb_parse_person(struct json_object *root,
                        tmdb_person_t *out);
static uint8_t      tmdb_parse_hits(struct json_object *results,
                        tmdb_media_t media, tmdb_hit_t *out, uint8_t cap);

static bool         tmdb_title_cache_get(tmdb_media_t m, int32_t id,
                        tmdb_title_t *out, time_t now, uint32_t ttl);
static void         tmdb_title_cache_put(const tmdb_title_t *v);
static bool         tmdb_person_cache_get(int32_t id, tmdb_person_t *out,
                        time_t now, uint32_t ttl);
static void         tmdb_person_cache_put(const tmdb_person_t *v);
static bool         tmdb_trend_cache_get(const char *key, tmdb_hit_t *out,
                        uint8_t *n, time_t now, uint32_t ttl);
static void         tmdb_trend_cache_put(const char *key,
                        const tmdb_hit_t *hits, uint8_t n);

static void         tmdb_search_done(const curl_response_t *resp);
static void         tmdb_title_done(const curl_response_t *resp);
static void         tmdb_person_done(const curl_response_t *resp);

static bool         tmdb_init(void);
static void         tmdb_deinit(void);

#endif // TMDB_INTERNAL

#endif // BM_TMDB_H
