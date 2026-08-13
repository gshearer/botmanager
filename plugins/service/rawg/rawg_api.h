#ifndef BM_RAWG_API_H
#define BM_RAWG_API_H

// Public mechanism API for the rawg service plugin — keyed (?key= query
// param) access to the RAWG video-games database (rawg.io, 350k+ games).
// Consumers include this header and resolve the symbols at runtime via
// plugin_dlsym_cached(RAWG_CTX, …, (void **)&cached) — the plugin is loaded
// RTLD_LOCAL. Pure mechanism: this plugin fetches and normalizes; the rawg
// command plugin owns every byte of user-facing presentation.
//
// Shim shape mirrors plugins/service/tmdb/tmdb_api.h: an atomic-guarded
// static cache per symbol, a union to launder void*↔function-pointer,
// FATAL + abort on a lookup miss. Inside the rawg plugin itself the
// static-inline shims would collide with the real definitions, so its
// own translation units (rawg.c, rawg_cmd.c) define RAWG_INTERNAL before
// including this header to skip them and link directly instead.
//
// Normalization conventions (the service normalizes; the consumer only
// presents): `rating` is RAWG's community average on a 0–5 scale and is 0
// when `ratings_count` is 0 (unrated). `metacritic` is 0–100 and 0 when
// absent. `year` is parsed from the release date (0 if absent). All strings
// are "" when the source omits them. Every payload — the envelope and any
// array it points at — is valid only for the duration of the callback;
// consumers that must outlive it deep-copy.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "async.h"
#include "common.h"  // SUCCESS/FAIL

// The plugin's name, and so both its dlsym handle and the root of its
// clam contexts. Public because the shims below resolve against it.
#define RAWG_CTX "rawg"

// ----------------------------------------------------------------------
// Fixed sizes. Fixed-width strings let consumers stack-allocate results
// and sidestep inner-string lifetime ambiguity.
// ----------------------------------------------------------------------

#define RAWG_NAME_SZ      128
#define RAWG_SLUG_SZ      96
#define RAWG_DATE_SZ      12    // "YYYY-MM-DD"
#define RAWG_DESC_SZ      512
#define RAWG_GENRES_SZ    128
#define RAWG_PLATFORMS_SZ 160
#define RAWG_NAMES_SZ     160   // developers / publishers, comma-joined
#define RAWG_STORES_SZ    160
#define RAWG_TAGS_SZ      192
#define RAWG_URL_SZ       256
#define RAWG_ESRB_SZ      32
#define RAWG_HITS_MAX     20    // ceiling on a search / list result set

typedef enum
{
  RAWG_OK = 0,
  RAWG_NOT_FOUND,
  RAWG_RATE_LIMITED,
  RAWG_AUTH,          // 401 — key missing/invalid
  RAWG_TRANSPORT,
  RAWG_UNAVAILABLE    // no key configured, or provider not loaded
} rawg_status_t;

typedef enum
{
  RAWG_LIST_POPULAR = 0,  // -added, recent window (default)
  RAWG_LIST_BEST,         // -metacritic, metacritic=1,100
  RAWG_LIST_NEW           // -released, last ~30 days
} rawg_list_kind_t;

// ----------------------------------------------------------------------
// A full game detail card.
// ----------------------------------------------------------------------

typedef struct
{
  int32_t id;
  char    slug[RAWG_SLUG_SZ];
  char    name[RAWG_NAME_SZ];
  char    released[RAWG_DATE_SZ];
  int32_t year;                         // parsed from released (0 if absent)
  bool    tba;                          // "to be announced"
  double  rating;                       // 0–5 community avg (0 = unrated)
  int32_t rating_top;                   // usual max on the scale (typically 5)
  int32_t ratings_count;
  int32_t metacritic;                   // 0–100 (0 = none)
  int32_t playtime;                     // typical hours to beat (0 = unknown)
  char    esrb[RAWG_ESRB_SZ];
  char    genres[RAWG_GENRES_SZ];
  char    platforms[RAWG_PLATFORMS_SZ]; // parent-platform families, joined
  char    developers[RAWG_NAMES_SZ];
  char    publishers[RAWG_NAMES_SZ];
  char    stores[RAWG_STORES_SZ];
  char    tags[RAWG_TAGS_SZ];
  char    description[RAWG_DESC_SZ];     // description_raw, trimmed
  char    website[RAWG_URL_SZ];
  char    reddit_url[RAWG_URL_SZ];
  char    background_image[RAWG_URL_SZ];
  int32_t achievements_count;
} rawg_game_t;

// ----------------------------------------------------------------------
// A lightweight search / list row (ranked, pre-detail).
// ----------------------------------------------------------------------

typedef struct
{
  int32_t id;
  char    slug[RAWG_SLUG_SZ];
  char    name[RAWG_NAME_SZ];
  int32_t year;                         // 0 for undated titles
  double  rating;                       // 0–5 (0 = unrated)
  int32_t metacritic;                   // 0–100 (0 = none)
  int32_t added;                        // library-adds popularity proxy
  int32_t ratings_count;
  char    platforms[RAWG_PLATFORMS_SZ]; // parent-platform families, joined
  char    genres[RAWG_GENRES_SZ];
  char    released[RAWG_DATE_SZ];       // shown for RAWG_LIST_NEW
} rawg_hit_t;

// ----------------------------------------------------------------------
// Result envelopes + callbacks. `message` carries a human-readable detail
// on failure ("" on success).
// ----------------------------------------------------------------------

typedef struct
{
  rawg_status_t status;
  char          message[128];
  rawg_hit_t   *hits;   // n entries, ranked; NULL when n == 0
  uint8_t       n;
} rawg_search_res_t;

typedef struct
{
  rawg_status_t status;
  char          message[128];
  rawg_game_t   game;   // populated only when status == RAWG_OK
} rawg_game_res_t;

// Callbacks run on the curl-multi worker thread owned by the rawg plugin
// — they must be fast and non-blocking.
//
// This plugin registers NO plugin_unmap_notify listener, and is the one
// shape of async service that legitimately needs none: it is a single
// .so whose only caller is its own command surface (rawg_cmd.c), so a
// stored completion always names THIS mapping. Core sees it — quiesce
// and audit range-test curl_iter_req_t.cb, which for our transfers
// points here too — and refuses the dlclose while a request is
// airborne. ⚠ That holds only while nothing else calls in. The first
// consumer in another plugin makes this the defect written up in
// PLUGIN.md §Lifecycle Contract ("hand back pointers other plugins gave
// you"); copy reachyapi.c's registry before adding one.
typedef void (*rawg_search_cb_t)(const rawg_search_res_t *, void *user);
typedef void (*rawg_game_cb_t)(const rawg_game_res_t *, void *user);

// ----------------------------------------------------------------------
// Real function declarations — visible only inside the rawg plugin (where
// RAWG_INTERNAL is defined). External consumers go through the shims.
//
// Every async call here returns ASYNC_AIRBORNE or
// ASYNC_FAILED_UNDELIVERED and never ASYNC_FAILED_DELIVERED — a
// refusal (bad args, no key, transport refusal) always leaves `user`
// yours. See include/async.h for what the values oblige you to do.
// ----------------------------------------------------------------------

#ifdef RAWG_INTERNAL

// True when plugin.rawg.creds.apikey is configured.
bool rawg_configured(void);

// ----------------------------------------------------------------------
// All three below return only ASYNC_AIRBORNE or
// ASYNC_FAILED_UNDELIVERED. The undelivered causes are all pre-flight:
// NULL/empty argument, no API key configured, URL overflow, or the curl
// request could not be created or submitted.
//
// ⚠ ASYNC_AIRBORNE does not mean "later". Two of them answer a warm
// cache **in place, before this call returns** (`rawg_game_async`,
// `rawg_list_async`; `rawg_search_async` has no cache and is always
// deferred), so a caller holding a lock across the call can re-enter
// itself through its own callback. Take that seriously or call from a
// task worker.
// ----------------------------------------------------------------------

// Free-text search over the games catalogue. No cache: the callback
// always runs later, on the curl worker.
async_rc_t rawg_search_async(const char *query, rawg_search_cb_t cb, void *user);

// Full detail for one game by numeric id. ⚠ Cached: the callback may
// already have run, inside this call.
async_rc_t rawg_game_async(int32_t id, rawg_game_cb_t cb, void *user);

// A ranked list. `year` == 0 uses the kind's default date window; a
// positive year windows to that whole calendar year. ⚠ Cached: the
// callback may already have run, inside this call.
async_rc_t rawg_list_async(rawg_list_kind_t kind, int32_t year,
    rawg_search_cb_t cb, void *user);

#endif // RAWG_INTERNAL

// ----------------------------------------------------------------------
// dlsym shim helpers (consumer side)
// ----------------------------------------------------------------------

#ifndef RAWG_INTERNAL

#include "clam.h"
#include "plugin.h"

#include <stdlib.h>  // abort

static inline bool
rawg_configured(void)
{
  typedef bool (*fn_t)(void);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(RAWG_CTX, "rawg_configured", (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, RAWG_CTX, "dlsym failed: rawg_configured");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn());
}

static inline async_rc_t
rawg_search_async(const char *query, rawg_search_cb_t cb, void *user)
{
  typedef async_rc_t (*fn_t)(const char *, rawg_search_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(RAWG_CTX, "rawg_search_async",
        (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, RAWG_CTX, "dlsym failed: rawg_search_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(query, cb, user));
}

static inline async_rc_t
rawg_game_async(int32_t id, rawg_game_cb_t cb, void *user)
{
  typedef async_rc_t (*fn_t)(int32_t, rawg_game_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(RAWG_CTX, "rawg_game_async", (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, RAWG_CTX, "dlsym failed: rawg_game_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(id, cb, user));
}

static inline async_rc_t
rawg_list_async(rawg_list_kind_t kind, int32_t year,
    rawg_search_cb_t cb, void *user)
{
  typedef async_rc_t (*fn_t)(rawg_list_kind_t, int32_t, rawg_search_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(RAWG_CTX, "rawg_list_async", (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, RAWG_CTX, "dlsym failed: rawg_list_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(kind, year, cb, user));
}

#endif // !RAWG_INTERNAL

#endif // BM_RAWG_API_H
