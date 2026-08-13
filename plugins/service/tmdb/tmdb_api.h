#ifndef BM_TMDB_API_H
#define BM_TMDB_API_H

// Public mechanism API for the tmdb service plugin — keyed (v4 Bearer
// token) access to The Movie Database (themoviedb.org). Consumers include
// this header and resolve the symbols at runtime via
// plugin_dlsym_cached(TMDB_CTX, …, (void **)&cached) — the plugin is loaded
// RTLD_LOCAL. Pure mechanism: this plugin fetches and normalizes; the
// tmdb command plugin owns every byte of user-facing presentation.
//
// Shim shape mirrors plugins/service/coinmarketcap/coinmarketcap_api.h: an
// atomic-guarded static cache per symbol, a union to launder
// void*↔function-pointer, FATAL + abort on a lookup miss. Inside the tmdb
// plugin itself the static-inline shims would collide with the real
// definitions, so its own translation units (tmdb.c, tmdb_cmd.c) define
// TMDB_INTERNAL before including this header to skip them and link
// directly instead.
//
// Normalization conventions (the service normalizes; the consumer only
// presents): `rating` is TMDB's vote_average on a 0–10 scale and is 0
// when `votes` is 0 (unrated). `year` is parsed from the release/air date
// (0 if absent). Runtime is whole minutes (0 if absent). All strings are
// "" when the source omits them; enums are the _UNKNOWN member. Every
// payload — the envelope and any array it points at — is valid only for
// the duration of the callback; consumers that must outlive it deep-copy.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"  // SUCCESS/FAIL

// The plugin's name, and so both its dlsym handle and the root of its
// clam contexts. Public because the shims below resolve against it.
#define TMDB_CTX "tmdb"

// ----------------------------------------------------------------------
// Fixed sizes. Fixed-width strings let consumers stack-allocate results
// and sidestep inner-string lifetime ambiguity.
// ----------------------------------------------------------------------

#define TMDB_TITLE_SZ      128
#define TMDB_TAGLINE_SZ    192
#define TMDB_OVERVIEW_SZ   512
#define TMDB_DATE_SZ       12    // "YYYY-MM-DD"
#define TMDB_GENRES_SZ     160
#define TMDB_NAME_SZ       96
#define TMDB_CAST_SZ       224   // top billed names, comma-joined
#define TMDB_KNOWNFOR_SZ   320   // person's notable titles, comma-joined
#define TMDB_STATUS_SZ     40
#define TMDB_NETWORKS_SZ   128
#define TMDB_PLACE_SZ      128
#define TMDB_BIO_SZ        512
#define TMDB_IMDB_SZ       16
#define TMDB_TRAILER_SZ    24    // YouTube video key
#define TMDB_HITS_MAX      16    // ceiling on a search / trending result set

typedef enum
{
  TMDB_MEDIA_UNKNOWN = 0,
  TMDB_MEDIA_MOVIE,
  TMDB_MEDIA_TV,
  TMDB_MEDIA_PERSON
} tmdb_media_t;

typedef enum
{
  TMDB_OK = 0,
  TMDB_NOT_FOUND,
  TMDB_RATE_LIMITED,
  TMDB_AUTH,          // 401 — token missing/invalid
  TMDB_TRANSPORT,
  TMDB_UNAVAILABLE    // no token configured, or provider not loaded
} tmdb_status_t;

// ----------------------------------------------------------------------
// A movie or TV title (unified). Fields the other media type never
// carries stay zeroed/"".
// ----------------------------------------------------------------------

typedef struct
{
  int32_t      id;
  tmdb_media_t media;
  char         title[TMDB_TITLE_SZ];
  char         original_title[TMDB_TITLE_SZ];
  char         tagline[TMDB_TAGLINE_SZ];
  char         overview[TMDB_OVERVIEW_SZ];
  char         release_date[TMDB_DATE_SZ];   // movie.release_date / tv.first_air_date
  int32_t      year;                         // parsed from release_date (0 if absent)
  double       rating;                       // vote_average 0–10 (0 = unrated)
  int32_t      votes;                        // vote_count
  double       popularity;
  char         status[TMDB_STATUS_SZ];       // "Released", "Returning Series", …
  char         genres[TMDB_GENRES_SZ];       // "Sci-Fi, Adventure"
  int32_t      runtime;                      // minutes (movie.runtime / tv episode run-time)
  char         director[TMDB_NAME_SZ];       // movie: Director; tv: creator
  char         cast[TMDB_CAST_SZ];           // top billed, comma-joined
  char         imdb_id[TMDB_IMDB_SZ];        // "tt…"
  char         trailer_key[TMDB_TRAILER_SZ]; // best YouTube trailer key ("" if none)

  // Movie-only enrichment (0 when absent or not a movie).
  int64_t      budget;
  int64_t      revenue;

  // TV-only enrichment (0/"" when absent or not a series).
  int32_t      seasons;
  int32_t      episodes;
  char         networks[TMDB_NETWORKS_SZ];
  char         last_air_date[TMDB_DATE_SZ];
} tmdb_title_t;

// ----------------------------------------------------------------------
// A person (actor / director / crew).
// ----------------------------------------------------------------------

typedef struct
{
  int32_t id;
  char    name[TMDB_NAME_SZ];
  char    known_for_dept[TMDB_STATUS_SZ];    // "Acting", "Directing", …
  char    birthday[TMDB_DATE_SZ];
  char    deathday[TMDB_DATE_SZ];            // "" if living
  char    place_of_birth[TMDB_PLACE_SZ];
  char    biography[TMDB_BIO_SZ];
  double  popularity;
  char    imdb_id[TMDB_IMDB_SZ];             // "nm…"
  char    known_for[TMDB_KNOWNFOR_SZ];       // notable titles, comma-joined
} tmdb_person_t;

// ----------------------------------------------------------------------
// A lightweight search / trending hit (ranked, pre-detail).
// ----------------------------------------------------------------------

typedef struct
{
  int32_t      id;
  tmdb_media_t media;
  char         title[TMDB_TITLE_SZ];         // movie.title / tv.name / person.name
  int32_t      year;                         // 0 for people or undated titles
  double       rating;                       // 0 for people / unrated
  double       popularity;
  char         known_for_dept[TMDB_STATUS_SZ]; // person hits only
} tmdb_hit_t;

// ----------------------------------------------------------------------
// Result envelopes + callbacks. `message` carries a human-readable detail
// on failure ("" on success).
// ----------------------------------------------------------------------

typedef struct
{
  tmdb_status_t status;
  char          message[128];
  tmdb_hit_t   *hits;   // n entries, ranked; NULL when n == 0
  uint8_t       n;
} tmdb_search_res_t;

typedef struct
{
  tmdb_status_t status;
  char          message[128];
  tmdb_title_t  title;  // populated only when status == TMDB_OK
} tmdb_title_res_t;

typedef struct
{
  tmdb_status_t status;
  char          message[128];
  tmdb_person_t person; // populated only when status == TMDB_OK
} tmdb_person_res_t;

// Callbacks run on the curl-multi worker thread owned by the tmdb plugin
// — they must be fast and non-blocking.
//
// This plugin registers NO plugin_unmap_notify listener, and is the one
// shape of async service that legitimately needs none: it is a single
// .so whose only caller is its own command surface (tmdb_cmd.c), so a
// stored completion always names THIS mapping. Core sees it — quiesce
// and audit range-test curl_iter_req_t.cb, which for our transfers
// points here too — and refuses the dlclose while a request is
// airborne. ⚠ That holds only while nothing else calls in. The first
// consumer in another plugin makes this the defect written up in
// PLUGIN.md §Lifecycle Contract ("hand back pointers other plugins gave
// you"); copy reachyapi.c's registry before adding one.
typedef void (*tmdb_search_cb_t)(const tmdb_search_res_t *, void *user);
typedef void (*tmdb_title_cb_t)(const tmdb_title_res_t *, void *user);
typedef void (*tmdb_person_cb_t)(const tmdb_person_res_t *, void *user);

// ----------------------------------------------------------------------
// Real function declarations — visible only inside the tmdb plugin (where
// TMDB_INTERNAL is defined). External consumers go through the shims.
//
// Every async call obeys the same ownership contract: it returns FAIL
// *before* firing the callback (bad args, no token, transport refusal) —
// the caller still owns `user`; or it returns SUCCESS and fires the
// callback exactly once (the callback then owns `user`).
// ----------------------------------------------------------------------

#ifdef TMDB_INTERNAL

// True when plugin.tmdb.creds.apikey is configured.
bool tmdb_configured(void);

// ----------------------------------------------------------------------
// Async failure contract — all four below (PLUGIN.md §Async failure
// semantics, which MUSTs this be stated and MUSTs you not guess it).
//
// tmdb is in the "FAIL ⇒ the callback did NOT fire" row, opposite the
// exchange drivers. For every `*_async` here:
//
//   FAIL    — nothing was dispatched and your callback will never run.
//             **You still own your closure: free it, and answer the
//             user yourself.** Causes are all pre-flight: NULL/empty or
//             out-of-range argument, no API key configured, URL
//             overflow, or the curl request could not be created or
//             submitted.
//   SUCCESS — the callback runs exactly once. Usually later, on the
//             curl worker.
//
// ⚠ But three of them can run it **synchronously, before this call
// returns** — a warm cache is answered in place (`tmdb_title_async`,
// `tmdb_person_async`, `tmdb_trending_async`; `tmdb_search_async` has
// no cache and is always deferred). SUCCESS therefore does NOT mean
// "later", and a caller holding a lock across the call can re-enter
// itself through its own callback. Take that seriously or call from a
// task worker.
// ----------------------------------------------------------------------

// Search. `kind` selects the endpoint: TMDB_MEDIA_UNKNOWN → /search/multi
// (hits carry per-item media); MOVIE/TV/PERSON → the typed endpoint (all
// hits take `kind`). No cache: on SUCCESS the callback always runs
// later, on the curl worker.
bool tmdb_search_async(tmdb_media_t kind, const char *query,
    tmdb_search_cb_t cb, void *user);

// Full detail for one title. `kind` must be MOVIE or TV. ⚠ Cached: on
// SUCCESS the callback may already have run, inside this call.
bool tmdb_title_async(tmdb_media_t kind, int32_t id,
    tmdb_title_cb_t cb, void *user);

// Full detail for one person. ⚠ Cached: on SUCCESS the callback may
// already have run, inside this call.
bool tmdb_person_async(int32_t id, tmdb_person_cb_t cb, void *user);

// Trending list. `kind` is UNKNOWN (all), MOVIE, or TV. `weekly` selects
// the day/week window. ⚠ Cached: on SUCCESS the callback may already
// have run, inside this call.
bool tmdb_trending_async(tmdb_media_t kind, bool weekly,
    tmdb_search_cb_t cb, void *user);

#endif // TMDB_INTERNAL

// ----------------------------------------------------------------------
// dlsym shim helpers (consumer side)
// ----------------------------------------------------------------------

#ifndef TMDB_INTERNAL

#include "clam.h"
#include "plugin.h"

#include <stdlib.h>  // abort

static inline bool
tmdb_configured(void)
{
  typedef bool (*fn_t)(void);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(TMDB_CTX, "tmdb_configured", (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, TMDB_CTX, "dlsym failed: tmdb_configured");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn());
}

static inline bool
tmdb_search_async(tmdb_media_t kind, const char *query,
    tmdb_search_cb_t cb, void *user)
{
  typedef bool (*fn_t)(tmdb_media_t, const char *, tmdb_search_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(TMDB_CTX, "tmdb_search_async",
        (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, TMDB_CTX, "dlsym failed: tmdb_search_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(kind, query, cb, user));
}

static inline bool
tmdb_title_async(tmdb_media_t kind, int32_t id,
    tmdb_title_cb_t cb, void *user)
{
  typedef bool (*fn_t)(tmdb_media_t, int32_t, tmdb_title_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(TMDB_CTX, "tmdb_title_async",
        (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, TMDB_CTX, "dlsym failed: tmdb_title_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(kind, id, cb, user));
}

static inline bool
tmdb_person_async(int32_t id, tmdb_person_cb_t cb, void *user)
{
  typedef bool (*fn_t)(int32_t, tmdb_person_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(TMDB_CTX, "tmdb_person_async",
        (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, TMDB_CTX, "dlsym failed: tmdb_person_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(id, cb, user));
}

static inline bool
tmdb_trending_async(tmdb_media_t kind, bool weekly,
    tmdb_search_cb_t cb, void *user)
{
  typedef bool (*fn_t)(tmdb_media_t, bool, tmdb_search_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(TMDB_CTX, "tmdb_trending_async",
        (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, TMDB_CTX, "dlsym failed: tmdb_trending_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(kind, weekly, cb, user));
}

#endif // !TMDB_INTERNAL

#endif // BM_TMDB_API_H
