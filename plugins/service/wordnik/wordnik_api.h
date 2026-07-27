#ifndef BM_WORDNIK_API_H
#define BM_WORDNIK_API_H

// Public mechanism API for the wordnik service plugin. Consumers include
// this header and resolve the exported symbols at runtime via
// plugin_dlsym("wordnik", …) — the plugin is loaded RTLD_LOCAL.
//
// Three include modes:
//   - default: types + static-inline dlsym shims (abort on plugin miss).
//     Use this in command-surface or behaviour plugins that consider a
//     missing service plugin a startup misconfiguration.
//   - WORDNIK_INTERNAL: types + real prototypes, no shims. Used by the
//     plugin's own translation units (wordnik.c, wordnik_cmd.c), which
//     link against the definitions directly rather than dlsym-ing
//     themselves.
//   - WORDNIK_TYPES_ONLY: types only. For consumers that want to handle
//     "plugin not loaded" gracefully through their own dlsym path.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"  // SUCCESS/FAIL

// The plugin's name, and so both its dlsym handle and the root of its
// clam contexts. Public because the shims below resolve against it.
#define WORDNIK_CTX "wordnik"

// Outcome of a completed request. Anything other than WORDNIK_OK means
// the response carries no word.
typedef enum
{
  WORDNIK_OK = 0,
  WORDNIK_NO_KEY,        // plugin.wordnik.creds.apikey unset
  WORDNIK_AUTH,          // HTTP 401/403 — key rejected
  WORDNIK_RATE_LIMITED,  // HTTP 429
  WORDNIK_NOT_FOUND,     // HTTP 204/404 — no word published for that date
  WORDNIK_MALFORMED,     // body was not the JSON we expect
  WORDNIK_TRANSPORT      // curl-level failure or an unmapped HTTP status
} wordnik_status_t;

static inline const char *
wordnik_status_str(wordnik_status_t s)
{
  switch(s)
  {
    case WORDNIK_OK:           return("ok");
    case WORDNIK_NO_KEY:       return("no API key configured");
    case WORDNIK_AUTH:         return("API key rejected");
    case WORDNIK_RATE_LIMITED: return("rate limited");
    case WORDNIK_NOT_FOUND:    return("no word for that date");
    case WORDNIK_MALFORMED:    return("malformed response");
    case WORDNIK_TRANSPORT:    return("transport error");
  }

  return("unknown error");
}

// Result sizing — fixed buffers let callers stack-copy a whole word of
// the day without lifetime questions about inner strings.

#define WORDNIK_WORD_SZ      96
#define WORDNIK_DATE_SZ      16    // "YYYY-MM-DD"
#define WORDNIK_NOTE_SZ      512
#define WORDNIK_DEF_SZ       512
#define WORDNIK_POS_SZ       32    // "adjective", "transitive verb", …
#define WORDNIK_SOURCE_SZ    32    // dictionary id, e.g. "ahd-5"
#define WORDNIK_EXAMPLE_SZ   512
#define WORDNIK_TITLE_SZ     160
#define WORDNIK_URL_SZ       512

#define WORDNIK_MAX_DEFS     8     // hard ceiling regardless of KV
#define WORDNIK_MAX_EXAMPLES 4     // hard ceiling regardless of KV

// One sense of the word. `source` is Wordnik's dictionary id and may be
// empty; every field is an empty string when the API omitted it.
typedef struct
{
  char part_of_speech[WORDNIK_POS_SZ];
  char source        [WORDNIK_SOURCE_SZ];
  char text          [WORDNIK_DEF_SZ];
} wordnik_def_t;

// One citation showing the word in use.
typedef struct
{
  char title[WORDNIK_TITLE_SZ];   // work the passage was quoted from
  char url  [WORDNIK_URL_SZ];     // source page, when attributed
  char text [WORDNIK_EXAMPLE_SZ];
} wordnik_example_t;

// A word of the day. Definition and example text arrives from Wordnik
// sprinkled with inline XML markup (<xref>, <em>, …); the service strips
// it, so every string here is display-ready plain text.
typedef struct
{
  char              word[WORDNIK_WORD_SZ];
  char              date[WORDNIK_DATE_SZ];  // publication date, "pdd"
  char              note[WORDNIK_NOTE_SZ];  // etymology / usage aside
  wordnik_def_t     defs    [WORDNIK_MAX_DEFS];
  wordnik_example_t examples[WORDNIK_MAX_EXAMPLES];
  int32_t           n_defs;
  int32_t           n_examples;
} wordnik_wotd_t;

// Completion payload. `wotd` is valid for the duration of the callback
// only — the caller copies anything it needs to keep — and is NULL for
// every status other than WORDNIK_OK.
typedef struct
{
  wordnik_status_t      status;
  const wordnik_wotd_t *wotd;
  void                 *user_data;
} wordnik_response_t;

// Completion callback. Invoked on the curl worker thread; must be fast
// and non-blocking.
typedef void (*wordnik_done_cb_t)(const wordnik_response_t *resp);

// Real declarations — visible only inside the wordnik plugin. External
// consumers go through the static-inline dlsym shims below.
#ifdef WORDNIK_INTERNAL

// True when plugin.wordnik.creds.apikey holds a non-empty value.
bool wordnik_configured(void);

// Submit a word-of-the-day request. `date` is "YYYY-MM-DD" to fetch a
// specific day, or NULL/empty for today's word.
//
// returns: SUCCESS once the request is queued, FAIL if the key is unset,
//          the date is malformed, or curl refused the submit. On FAIL the
//          callback is NOT invoked.
bool wordnik_fetch_wotd(const char *date, wordnik_done_cb_t cb,
    void *user_data);

#endif // WORDNIK_INTERNAL

#if !defined(WORDNIK_INTERNAL) && !defined(WORDNIK_TYPES_ONLY)

#include "clam.h"
#include "plugin.h"

#include <stdlib.h>  // abort

static inline bool
wordnik_configured(void)
{
  typedef bool (*fn_t)(void);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(WORDNIK_CTX, "wordnik_configured",
        (void **)&cached);

    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, WORDNIK_CTX, "dlsym failed: wordnik_configured");
      abort();
    }

    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }

  return(fn());
}

static inline bool
wordnik_fetch_wotd(const char *date, wordnik_done_cb_t cb, void *user_data)
{
  typedef bool (*fn_t)(const char *, wordnik_done_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(WORDNIK_CTX, "wordnik_fetch_wotd",
        (void **)&cached);

    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, WORDNIK_CTX, "dlsym failed: wordnik_fetch_wotd");
      abort();
    }

    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }

  return(fn(date, cb, user_data));
}

#endif // !WORDNIK_INTERNAL && !WORDNIK_TYPES_ONLY

#endif // BM_WORDNIK_API_H
