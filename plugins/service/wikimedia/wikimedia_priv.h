#ifndef BM_WIKIMEDIA_PRIV_H
#define BM_WIKIMEDIA_PRIV_H

// The wikimedia provider's own declarations: its caches, its in-flight
// registry, the shape of one piece of work, and the static prototypes of
// wikimedia.c. Nothing but wikimedia.c includes this file — the parse
// layer beside it sees none of it, which is what keeps that layer pure.

#ifdef WIKIMEDIA_INTERNAL

#include "alloc.h"
#include "clam.h"
#include "curl.h"
#include "curl_flight.h"
#include "kv.h"
#include "plugin.h"

#include "wikimedia.h"

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

// ----------------------------------------------------------------------
// Cache entries. t == 0 marks an empty slot; each cache is a fixed table
// with a round-robin cursor, since nothing here justifies eviction
// policy.
// ----------------------------------------------------------------------

typedef struct
{
  char             key[WM_QUERY_SZ];  // the normalized search term
  wm_resolve_res_t v;
  time_t           t;
} wm_resolve_ent_t;

typedef struct
{
  char          key[WM_QID_SZ];       // the class QID
  wm_menu_res_t v;
  time_t        t;
} wm_menu_ent_t;

// One resolved property word and its ranked candidates. The candidate
// list is cached rather than a single P-id because which candidate wins
// depends on the subject, not on the word.
typedef struct
{
  char          key[WM_QUERY_SZ];     // the normalized property word
  wm_property_t cand[WM_PROP_CANDIDATES];
  uint8_t       n;
  time_t        t;
} wm_prop_ent_t;

// ----------------------------------------------------------------------
// One piece of work: a public call, from entry to the caller's callback.
// Heap-owned, filed in the in-flight registry for the whole of its life,
// and freed on the single terminal path (wm_work_finish).
// ----------------------------------------------------------------------

typedef enum
{
  WM_VERB_RESOLVE,
  WM_VERB_CLAIMS,
  WM_VERB_MENU,
  WM_VERB_PROSE
} wm_verb_t;

// The caller's callback. A union so the unmap sweep can clear every arm
// at once — all-bits-zero is the null test each delivery path makes.
typedef union
{
  wm_resolve_cb_t resolve;
  wm_claims_cb_t  claims;
  wm_menu_cb_t    menu;
  wm_prose_cb_t   prose;
} wm_cb_u;

typedef struct wm_work wm_work_t;

struct wm_work
{
  wm_verb_t verb;
  wm_cb_u   cb;
  void     *user;
  uint64_t  slot[WM_WORK_SLOTS];

  // Legs still outstanding in the current fan-out phase. The thread that
  // observes the final decrement owns what happens next.
  atomic_uint_least8_t pending;

  union
  {
    struct
    {
      char    key[WM_QUERY_SZ];                    // normalized, cache key
      char    cirrus[WM_SEARCH_LIMIT][WM_QID_SZ];  // leg a, in relevance order
      uint8_t n_cirrus;
      char    wbs[WM_SEARCH_LIMIT][WM_QID_SZ];     // leg b, alias-tolerant
      uint8_t n_wbs;
      bool    reached;                             // either searcher answered
      wm_resolve_res_t res;
    } resolve;

    struct
    {
      char          word[WM_QUERY_SZ];  // the property as the caller wrote it
      wm_property_t cand[WM_PROP_CANDIDATES];
      uint8_t       n_cand;

      // One parsed answer per candidate, filled by that candidate's own
      // leg and read only after the fan-out barrier.
      wm_claims_res_t got[WM_PROP_CANDIDATES];

      wm_claims_res_t res;
    } claims;

    struct
    {
      char    ids[WM_MENU_FETCH][WM_QID_SZ];
      uint8_t n_ids;
      wm_menu_res_t res;
    } menu;

    struct
    {
      bool           full;
      wm_prose_res_t res;
    } prose;
  } u;

  wm_work_t *next_active;
};

// A fanned-out leg's own context: which work, which arm of it. Freed by
// the completion that consumes it.
typedef struct
{
  wm_work_t *work;
  uint8_t    idx;
} wm_leg_t;

// Module state

// Every request this plugin puts on the wire, so wm_stop() can cancel
// them and wait out their callbacks before wm_deinit() destroys what
// those callbacks touch (PLUGIN.md §Lifecycle Contract).
static curl_flight_t wm_flight;

// The in-flight registry: every work whose caller's callback lives in
// another mapping — which here is all of them.
static pthread_mutex_t wm_active_mutex = PTHREAD_MUTEX_INITIALIZER;
static wm_work_t      *wm_active_head  = NULL;

// The three caches, under one lock: they are read on a curl worker and
// written on the same, and no path holds this across a wire call.
static wm_resolve_ent_t wm_resolve_cache[WM_RESOLVE_CACHE_SZ];
static wm_menu_ent_t    wm_menu_cache[WM_MENU_CACHE_SZ];
static wm_prop_ent_t    wm_prop_cache[WM_PROP_CACHE_SZ];
static uint32_t         wm_resolve_cursor = 0;
static uint32_t         wm_menu_cursor    = 0;
static uint32_t         wm_prop_cursor    = 0;
static pthread_mutex_t  wm_cache_mu;

// KV schema

static const plugin_kv_entry_t wm_kv_schema[] = {
  { "plugin.wikimedia.api_base",   KV_STR,
    "https://www.wikidata.org/w/api.php",
    "Wikidata action-API endpoint. SPARQL is deliberately not used.",
    NULL, NULL },
  { "plugin.wikimedia.language",   KV_STR, "en",
    "Language code for labels, descriptions and prose. Also selects the "
    "Wikipedia host (<code>.wikipedia.org).", NULL, NULL },
  { "plugin.wikimedia.timeout",    KV_UINT32, "8",
    "Per-request HTTP timeout (seconds).", NULL, NULL },
  { "plugin.wikimedia.cache_ttl",  KV_UINT32, "86400",
    "Resolution / menu / property-word cache TTL (seconds). Entities are "
    "near-static; 0 disables caching.", NULL, NULL },
  { "plugin.wikimedia.user_agent", KV_STR, WM_UA_DEFAULT,
    "User-Agent sent to Wikimedia. Their etiquette policy asks for a "
    "descriptive one naming the project and a contact address.",
    NULL, NULL },
};

// Forward declarations

static const char  *wm_api_base(void);
static void         wm_wiki_base(char *out, size_t cap);
static curl_request_t *wm_get(const char *url, curl_done_cb_t cb, void *user);
static bool         wm_launch(wm_work_t *w, uint8_t slot, const char *url,
                        curl_done_cb_t cb, void *user);
static struct json_object *wm_body(const curl_response_t *resp,
                        wm_status_t *status, char *msg, size_t msg_cap);

static bool         wm_resolve_cache_get(const char *key, wm_resolve_res_t *out,
                        time_t now, uint32_t ttl);
static void         wm_resolve_cache_put(const char *key,
                        const wm_resolve_res_t *v);
static bool         wm_menu_cache_get(const char *key, wm_menu_res_t *out,
                        time_t now, uint32_t ttl);
static void         wm_menu_cache_put(const char *key, const wm_menu_res_t *v);
static bool         wm_prop_cache_get(const char *key, wm_property_t *out,
                        uint8_t *n, time_t now, uint32_t ttl);
static void         wm_prop_cache_put(const char *key,
                        const wm_property_t *cand, uint8_t n);

static void         wm_work_track(wm_work_t *w);
static void         wm_work_unlink(wm_work_t *w, wm_cb_u *cb_out);
static void         wm_unmap_cb(uintptr_t lo, uintptr_t hi, void *data);
static wm_work_t   *wm_work_open(wm_verb_t verb, void *user);
static void         wm_work_finish(wm_work_t *w);
static void         wm_work_fail(wm_work_t *w, wm_status_t status,
                        const char *msg);
static void         wm_work_abandon(wm_work_t *w);
static void         wm_leg_retire(wm_work_t *w);

static void         wm_resolve_cirrus_done(const curl_response_t *resp);
static void         wm_resolve_wbs_done(const curl_response_t *resp);
static void         wm_resolve_merge(wm_work_t *w);
static void         wm_resolve_window_done(const curl_response_t *resp);

static void         wm_prop_search_done(const curl_response_t *resp);
static void         wm_claims_dispatch(wm_work_t *w);
static void         wm_claims_leg_done(const curl_response_t *resp);
static void         wm_claims_choose(wm_work_t *w);
static void         wm_claims_labels_done(const curl_response_t *resp);

static void         wm_menu_props_done(const curl_response_t *resp);
static void         wm_menu_meta_done(const curl_response_t *resp);

static void         wm_prose_sitelink_done(const curl_response_t *resp);
static bool         wm_prose_fetch(wm_work_t *w, const char *title);
static void         wm_prose_done(const curl_response_t *resp);

static bool         wm_init(void);
static bool         wm_stop(void);
static void         wm_deinit(void);

#endif // WIKIMEDIA_INTERNAL

#endif // BM_WIKIMEDIA_PRIV_H
