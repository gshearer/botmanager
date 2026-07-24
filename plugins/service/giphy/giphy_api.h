#ifndef BM_GIPHY_API_H
#define BM_GIPHY_API_H

// Public mechanism API for the giphy service plugin. Consumers include
// this header and resolve the exported symbols at runtime via
// plugin_dlsym("giphy", …) — the plugin is loaded RTLD_LOCAL.
//
// Three include modes:
//   - default: types + static-inline dlsym shims (abort on plugin miss).
//     Use this in command-surface or behaviour plugins that consider a
//     missing service plugin a startup misconfiguration.
//   - GIPHY_INTERNAL: types + real prototypes, no shims. Used by the
//     plugin's own translation units (giphy.c, giphy_cmd.c), which link
//     against the definitions directly rather than dlsym-ing themselves.
//   - GIPHY_TYPES_ONLY: types only. For consumers that want to handle
//     "plugin not loaded" gracefully through their own dlsym path.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <strings.h>  // strcasecmp

#include "common.h"  // SUCCESS/FAIL

// The plugin's name, and so both its dlsym handle and the root of its
// clam contexts. Public because the shims below resolve against it.
#define GIPHY_CTX "giphy"

// Which Giphy endpoint a request targets. SEARCH and TRANSLATE consume a
// query; RANDOM treats it as an optional tag; TRENDING ignores it.
typedef enum
{
  GIPHY_MODE_SEARCH = 0,  // /v1/gifs/search   — relevance-ranked list
  GIPHY_MODE_RANDOM,      // /v1/gifs/random   — one GIF, optional tag
  GIPHY_MODE_TRENDING,    // /v1/gifs/trending — current trending list
  GIPHY_MODE_TRANSLATE,   // /v1/gifs/translate— one "best mood" match
  GIPHY_MODE__COUNT
} giphy_mode_t;

// Wire/log names, indexed by giphy_mode_t.
static const char *const giphy_mode_names[GIPHY_MODE__COUNT] = {
  [GIPHY_MODE_SEARCH]    = "search",
  [GIPHY_MODE_RANDOM]    = "random",
  [GIPHY_MODE_TRENDING]  = "trending",
  [GIPHY_MODE_TRANSLATE] = "translate",
};

static inline const char *
giphy_mode_name(giphy_mode_t m)
{
  if((unsigned)m >= GIPHY_MODE__COUNT)
    return("search");

  return(giphy_mode_names[m]);
}

// Case-insensitive parse; unknown/NULL falls back to SEARCH so callers
// can pass user input through without a separate validation step.
static inline giphy_mode_t
giphy_mode_from_name(const char *s)
{
  if(s == NULL)
    return(GIPHY_MODE_SEARCH);

  for(size_t i = 0; i < GIPHY_MODE__COUNT; i++)
    if(strcasecmp(giphy_mode_names[i], s) == 0)
      return((giphy_mode_t)i);

  return(GIPHY_MODE_SEARCH);
}

// Outcome of a completed request. Anything other than GIPHY_OK means the
// result array is empty.
typedef enum
{
  GIPHY_OK = 0,
  GIPHY_NO_KEY,        // plugin.giphy.creds.apikey unset
  GIPHY_AUTH,          // HTTP 401/403 — key rejected
  GIPHY_RATE_LIMITED,  // HTTP 429
  GIPHY_NOT_FOUND,     // HTTP 404, or an empty data payload
  GIPHY_MALFORMED,     // body was not the JSON we expect
  GIPHY_TRANSPORT      // curl-level failure or an unmapped HTTP status
} giphy_status_t;

static inline const char *
giphy_status_str(giphy_status_t s)
{
  switch(s)
  {
    case GIPHY_OK:           return("ok");
    case GIPHY_NO_KEY:       return("no API key configured");
    case GIPHY_AUTH:         return("API key rejected");
    case GIPHY_RATE_LIMITED: return("rate limited");
    case GIPHY_NOT_FOUND:    return("no results");
    case GIPHY_MALFORMED:    return("malformed response");
    case GIPHY_TRANSPORT:    return("transport error");
  }

  return("unknown error");
}

// Result-row sizing — fixed buffers let callers stack- or array-copy
// without lifetime questions about inner strings.

#define GIPHY_ID_SZ       64
#define GIPHY_TITLE_SZ    256
#define GIPHY_URL_SZ      512   // asset URLs carry a ?cid=…&ct=… tail
#define GIPHY_NAME_SZ     64
#define GIPHY_RATING_SZ   8     // "g" / "pg" / "pg-13" / "r"
#define GIPHY_DATE_SZ     32    // "YYYY-MM-DD HH:MM:SS"
#define GIPHY_MAX_RESULTS 25    // hard ceiling regardless of KV

// One GIF. `media_url` is the canonical, query-string-free permalink the
// service synthesises from the id — prefer it for display; `gif_url` is
// whatever the API handed back and may expire or carry tracking params.
// Fields the API omitted are empty strings / zero.
typedef struct
{
  char     id       [GIPHY_ID_SZ];
  char     title    [GIPHY_TITLE_SZ];
  char     page_url [GIPHY_URL_SZ];   // giphy.com landing page
  char     media_url[GIPHY_URL_SZ];   // media.giphy.com/media/<id>/giphy.gif
  char     gif_url  [GIPHY_URL_SZ];   // images.original.url
  char     mp4_url  [GIPHY_URL_SZ];   // images.original.mp4 (may be empty)
  char     webp_url [GIPHY_URL_SZ];   // images.original.webp (may be empty)
  char     username [GIPHY_NAME_SZ];  // uploader, when attributed
  char     source   [GIPHY_NAME_SZ];  // originating domain (source_tld)
  char     rating   [GIPHY_RATING_SZ];
  char     imported [GIPHY_DATE_SZ];
  uint32_t width_px;
  uint32_t height_px;
  uint32_t size_bytes;                // original asset weight
} giphy_result_t;

// Completion payload. `results` is valid for the duration of the
// callback only — the caller copies anything it needs to keep.
typedef struct
{
  giphy_status_t        status;
  giphy_mode_t          mode;         // mirrors the request
  const giphy_result_t *results;
  size_t                n_results;
  void                 *user_data;
} giphy_response_t;

// Completion callback. Invoked on the curl worker thread; must be fast
// and non-blocking.
typedef void (*giphy_done_cb_t)(const giphy_response_t *resp);

// Real declarations — visible only inside the giphy plugin. External
// consumers go through the static-inline dlsym shims below.
#ifdef GIPHY_INTERNAL

// True when plugin.giphy.creds.apikey holds a non-empty value.
bool giphy_configured(void);

// Submit a Giphy request. `query` is the search phrase (SEARCH /
// TRANSLATE), the tag (RANDOM, may be NULL or empty), or ignored
// (TRENDING). `n_wanted` of 0 means "use plugin.giphy.max_results"; the
// final count is clamped to [1, GIPHY_MAX_RESULTS] and to 1 for the
// single-result modes.
//
// returns: SUCCESS once the request is queued, FAIL if the key is
//          unset, the arguments are unusable, or curl refused the
//          submit. On FAIL the callback is NOT invoked.
bool giphy_fetch(giphy_mode_t mode, const char *query, size_t n_wanted,
    giphy_done_cb_t cb, void *user_data);

#endif // GIPHY_INTERNAL

#if !defined(GIPHY_INTERNAL) && !defined(GIPHY_TYPES_ONLY)

#include "clam.h"
#include "plugin.h"

#include <stdlib.h>  // abort

static inline bool
giphy_configured(void)
{
  typedef bool (*fn_t)(void);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(GIPHY_CTX, "giphy_configured",
        (void **)&cached);

    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, GIPHY_CTX, "dlsym failed: giphy_configured");
      abort();
    }

    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }

  return(fn());
}

static inline bool
giphy_fetch(giphy_mode_t mode, const char *query, size_t n_wanted,
    giphy_done_cb_t cb, void *user_data)
{
  typedef bool (*fn_t)(giphy_mode_t, const char *, size_t,
      giphy_done_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(GIPHY_CTX, "giphy_fetch", (void **)&cached);

    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, GIPHY_CTX, "dlsym failed: giphy_fetch");
      abort();
    }

    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }

  return(fn(mode, query, n_wanted, cb, user_data));
}

#endif // !GIPHY_INTERNAL && !GIPHY_TYPES_ONLY

#endif // BM_GIPHY_API_H
