#ifndef BM_RAWG_H
#define BM_RAWG_H

// Internal header for the rawg service plugin. The public mechanism API is
// rawg_api.h (resolved by consumers via plugin_dlsym_cached("rawg", …));
// nothing outside rawg.c pulls this file. Gated by RAWG_INTERNAL. rawg.c
// defines RAWG_INTERNAL before including rawg_api.h, so that header exposes
// the real provider prototypes rather than the dlsym shims.

#ifdef RAWG_INTERNAL

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "curl.h"
#include "json.h"
#include "kv.h"
#include "plugin.h"

#include "rawg_api.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// Constants

// RAWG_CTX (the plugin name / clam-context root) lives in rawg_api.h.

#define RAWG_API_BASE   "https://api.rawg.io/api"

#define RAWG_REQ_URL_SZ 1024  // assembled request URL (holds the ?key= too)
#define RAWG_ENC_SZ     256   // URL-encoded query segment
#define RAWG_DATES_SZ   48    // "YYYY-MM-DD,YYYY-MM-DD" (int32 year-safe)
#define RAWG_GENRES_MAX 3     // genres carried per row/detail
#define RAWG_PLAT_MAX   4     // platform families carried per row/detail
#define RAWG_NAMES_MAX  3     // developers / publishers carried per detail
#define RAWG_STORES_MAX 4     // stores carried per detail
#define RAWG_TAGS_MAX   6     // english tags carried per detail

// Detail cache (games are effectively static) + a list cache (refreshes on
// its own TTL). t == 0 marks an empty slot.
#define RAWG_GAME_CACHE_SZ  32
#define RAWG_LIST_CACHE_SZ  8
#define RAWG_LIST_KEY_SZ    24   // "popular:2026", "best:0", "new:0"

typedef struct
{
  rawg_game_t v;
  time_t      t;
} rawg_game_ent_t;

typedef struct
{
  char       key[RAWG_LIST_KEY_SZ];
  rawg_hit_t hits[RAWG_HITS_MAX];
  uint8_t    n;
  time_t     t;
} rawg_list_ent_t;

// Per-request closure. Single-shot (one HTTP request per call), so unlike a
// fan-out there is no pending counter — the one done callback frees it.
typedef enum
{
  RAWG_REQ_SEARCH,
  RAWG_REQ_GAME,
  RAWG_REQ_LIST
} rawg_req_kind_t;

typedef struct
{
  rawg_req_kind_t  kind;
  int32_t          id;                       // GAME
  char             list_key[RAWG_LIST_KEY_SZ]; // LIST cache key
  rawg_search_cb_t search_cb;                // SEARCH / LIST
  rawg_game_cb_t   game_cb;                  // GAME
  void            *user;
} rawg_req_t;

// Module state: detail + list caches, one mutex covering both.
static rawg_game_ent_t rawg_game_cache[RAWG_GAME_CACHE_SZ];
static rawg_list_ent_t rawg_list_cache[RAWG_LIST_CACHE_SZ];
static uint32_t        rawg_game_cursor = 0;
static uint32_t        rawg_list_cursor = 0;
static pthread_mutex_t rawg_cache_mu;

// KV schema

static const plugin_kv_entry_t rawg_kv_schema[] = {
  { "plugin.rawg.creds.apikey",  KV_STR, "",
    "RAWG API key (sent as the ?key= query param). Get one free at "
    "rawg.io/apidocs.", NULL, NULL },
  { "plugin.rawg.cache_ttl",     KV_UINT32, "3600",
    "Game-detail cache TTL (seconds); details are near-static.",
    NULL, NULL },
  { "plugin.rawg.trending_ttl",  KV_UINT32, "900",
    "List cache TTL (seconds).", NULL, NULL },
  { "plugin.rawg.timeout",       KV_UINT32, "8",
    "Per-request HTTP timeout (seconds).", NULL, NULL },
  { "plugin.rawg.page_size",     KV_UINT32, "10",
    "Search/list rows requested (clamped to RAWG_HITS_MAX).", NULL, NULL },
};

// Forward declarations

static size_t        rawg_urlencode(const char *in, char *out, size_t cap);
static int32_t       rawg_year_of(const char *date);
static rawg_status_t rawg_status_of_http(long http, int curl_code);
static void          rawg_apply_opts(curl_request_t *cr);
static uint32_t      rawg_page_size(void);
static void          rawg_list_dates(rawg_list_kind_t kind, int32_t year,
                         char *dates, size_t cap);

static void          rawg_join_names(struct json_object *arr, const char *key,
                         char *out, size_t cap, int max);
static void          rawg_join_nested(struct json_object *arr,
                         const char *obj_key, const char *name_key,
                         char *out, size_t cap, int max);
static void          rawg_join_eng_tags(struct json_object *arr,
                         char *out, size_t cap, int max);
static void          rawg_strip_html(const char *in, char *out, size_t cap);
static void          rawg_parse_game(struct json_object *root,
                         rawg_game_t *out);
static uint8_t       rawg_parse_hits(struct json_object *results,
                         rawg_hit_t *out, uint8_t cap);

static bool          rawg_game_cache_get(int32_t id, rawg_game_t *out,
                         time_t now, uint32_t ttl);
static void          rawg_game_cache_put(const rawg_game_t *v);
static bool          rawg_list_cache_get(const char *key, rawg_hit_t *out,
                         uint8_t *n, time_t now, uint32_t ttl);
static void          rawg_list_cache_put(const char *key,
                         const rawg_hit_t *hits, uint8_t n);

static void          rawg_search_done(const curl_response_t *resp);
static void          rawg_game_done(const curl_response_t *resp);

static bool          rawg_init(void);
static void          rawg_deinit(void);

#endif // RAWG_INTERNAL

#endif // BM_RAWG_H
