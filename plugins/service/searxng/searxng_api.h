#ifndef BM_SEARXNG_API_H
#define BM_SEARXNG_API_H

// Public mechanism API for the searxng service plugin. Consumers include
// this header and resolve sxng_search at runtime via plugin_dlsym
// ("searxng", "sxng_search") — the plugin is loaded RTLD_LOCAL.
//
// Three include modes:
//   - default: types + static-inline dlsym shim (aborts on plugin miss).
//     Use this in command-surface or behaviour plugins that consider a
//     missing service plugin a startup misconfiguration.
//   - SEARXNG_INTERNAL: types + real prototype, no shim. Used by
//     searxng.c itself.
//   - SEARXNG_TYPES_ONLY: types only, no shim, no prototype. Used by
//     consumers (e.g. the inference layer's acquire engine) that want
//     to handle "plugin not loaded" gracefully via their own dlsym
//     path with WARN-and-skip semantics.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <strings.h>  // strcasecmp

#include "common.h"  // SUCCESS/FAIL

// Search categories. Matches SearXNG's `categories=` query parameter.
// Wire names live in sxng_category_names[].
typedef enum
{
  SXNG_CAT_GENERAL = 0,  // default web search
  SXNG_CAT_IMAGES,
  SXNG_CAT_NEWS,
  SXNG_CAT_VIDEOS,
  SXNG_CAT_MUSIC,
  SXNG_CAT__COUNT
} sxng_category_t;

// Result row sizing — fixed buffers let callers stack- or array-copy
// without lifetime questions about inner strings.

#define SXNG_RESULT_TITLE_SZ      256
#define SXNG_RESULT_URL_SZ        2048
#define SXNG_RESULT_SNIPPET_SZ    1024
#define SXNG_ENGINE_SZ            64
#define SXNG_AUTHOR_SZ            128
#define SXNG_DATE_SZ              32   // ISO-8601 or whatever the engine returns
#define SXNG_LENGTH_SZ            16   // "MM:SS" or "HH:MM:SS"
#define SXNG_RESOLUTION_SZ        16   // "WIDTHxHEIGHT"
#define SXNG_MAX_RESULTS          32

// Wire-name table. Indexed by sxng_category_t; SXNG_CAT__COUNT fixes the
// length so a missed entry trips a translation-unit-local zero-init and
// the helpers below catch it.
static const char *const sxng_category_names[SXNG_CAT__COUNT] = {
  [SXNG_CAT_GENERAL] = "general",
  [SXNG_CAT_IMAGES]  = "images",
  [SXNG_CAT_NEWS]    = "news",
  [SXNG_CAT_VIDEOS]  = "videos",
  [SXNG_CAT_MUSIC]   = "music",
};

static inline const char *
sxng_category_name(sxng_category_t c)
{
  if((unsigned)c >= SXNG_CAT__COUNT)
    return("general");

  return(sxng_category_names[c]);
}

// Case-insensitive parse. Returns SXNG_CAT_GENERAL on NULL/unknown so
// callers can pass user input straight through without a separate
// validation step (the GENERAL fallback is the safe default).
static inline sxng_category_t
sxng_category_from_name(const char *s)
{
  if(s == NULL)
    return(SXNG_CAT_GENERAL);

  for(size_t i = 0; i < SXNG_CAT__COUNT; i++)
    if(strcasecmp(sxng_category_names[i], s) == 0)
      return((sxng_category_t)i);

  return(SXNG_CAT_GENERAL);
}

// One result row returned by the SearXNG endpoint. The common fields
// (title, url, snippet, engine, score) are populated for every category;
// the `extras` union carries category-specific metadata. Unused union
// members are zero-initialised.
typedef struct
{
  char  title  [SXNG_RESULT_TITLE_SZ];
  char  url    [SXNG_RESULT_URL_SZ];   // page URL (always populated)
  char  snippet[SXNG_RESULT_SNIPPET_SZ];
  char  engine [SXNG_ENGINE_SZ];
  float score;

  // Mirrors the request category so consumers iterating a heterogeneous
  // mix (future multi-category queries) can dispatch per row without
  // tracking the request side-band.
  sxng_category_t category;

  union
  {
    struct
    {
      char     src       [SXNG_RESULT_URL_SZ];   // direct image URL
      char     thumbnail [SXNG_RESULT_URL_SZ];
      char     resolution[SXNG_RESOLUTION_SZ];   // e.g. "1920x1080"
      uint32_t width_px;                          // parsed from resolution
      uint32_t height_px;
    } image;

    struct
    {
      char     img_src   [SXNG_RESULT_URL_SZ];   // article hero image
      char     published [SXNG_DATE_SZ];
      char     source    [SXNG_AUTHOR_SZ];        // publication name
    } news;

    struct
    {
      char     thumbnail [SXNG_RESULT_URL_SZ];
      char     iframe_src[SXNG_RESULT_URL_SZ];   // embeddable player
      char     author    [SXNG_AUTHOR_SZ];        // channel / uploader
      char     published [SXNG_DATE_SZ];
      char     length    [SXNG_LENGTH_SZ];        // "MM:SS" / "HH:MM:SS"
    } video;

    struct
    {
      char     author    [SXNG_AUTHOR_SZ];        // artist
      char     published [SXNG_DATE_SZ];
    } music;
  } extras;
} sxng_result_t;

// Completion payload. `results` and `error` are valid for the duration
// of the callback only — caller copies anything needed.
typedef struct
{
  bool                 ok;
  const char          *error;
  sxng_category_t      category;       // mirrors the request
  const sxng_result_t *results;
  size_t               n_results;
  void                *user_data;
} sxng_response_t;

// Completion callback. Invoked on the curl worker thread; must be fast
// and non-blocking.
typedef void (*sxng_done_cb_t)(const sxng_response_t *resp);

// Real function declaration — visible only inside the searxng plugin.
// External consumers go through the static-inline dlsym shim below.
#ifdef SEARXNG_INTERNAL

// Submit a SearXNG search. Caps n_wanted at SXNG_MAX_RESULTS and at the
// plugin-configured per-request limit. `category` selects the SearXNG
// category bucket; SXNG_CAT_GENERAL is the no-op default.
//
// On success the callback fires with ok=true and 0+ results. On
// transport, HTTP, or parse failure the callback fires with ok=false
// and error set.
//
// returns: SUCCESS on submit, FAIL if the subsystem is not ready, the
//          query is empty, or the request could not be queued. On FAIL
//          the callback is NOT invoked.
bool sxng_search(const char *query, sxng_category_t category,
    size_t n_wanted, sxng_done_cb_t cb, void *user_data);

#endif // SEARXNG_INTERNAL

#if !defined(SEARXNG_INTERNAL) && !defined(SEARXNG_TYPES_ONLY)

#include "clam.h"
#include "plugin.h"

#include <stdlib.h>  // abort

static inline bool
sxng_search(const char *query, sxng_category_t category, size_t n_wanted,
    sxng_done_cb_t cb, void *user_data)
{
  typedef bool (*fn_t)(const char *, sxng_category_t, size_t,
      sxng_done_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("searxng", "sxng_search");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "searxng", "dlsym failed: sxng_search");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(query, category, n_wanted, cb, user_data));
}

#endif // !SEARXNG_INTERNAL && !SEARXNG_TYPES_ONLY

#endif // BM_SEARXNG_API_H
