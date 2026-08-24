#ifndef BM_WIKIMEDIA_H
#define BM_WIKIMEDIA_H

// Internal header shared by the wikimedia plugin's two translation
// units: the provider (wikimedia.c), which owns the wire and every piece
// of module state, and the parse layer (wikimedia_parse.c), which turns
// Wikimedia's wire formats into this plugin's types and touches nothing
// else. The public mechanism API is wikimedia_api.h; nothing outside the
// plugin (and tests/test_wikimedia_parse.c) pulls this file.
//
// Gated by WIKIMEDIA_INTERNAL, which both TUs define before including
// wikimedia_api.h so that header exposes the real provider prototypes
// rather than the dlsym shims.

#ifdef WIKIMEDIA_INTERNAL

#include "common.h"
#include "json.h"

#include "wikimedia_api.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Constants

// WIKIMEDIA_CTX (the plugin name / clam-context root) lives in
// wikimedia_api.h.

#define WM_URL_SZ        2048  // an assembled request URL: ids= is the long one
#define WM_ENC_SZ        384   // one percent-encoded query segment
#define WM_QUERY_SZ      128   // a caller's search term, verbatim
#define WM_IDLIST_SZ     1024  // "Q1%7CQ2%7C…" — bounded by WM_IDS_MAX
#define WM_UA_SZ         KV_STR_SZ

// How many hits each searcher is asked for. The merged window is capped
// at WM_CANDIDATES_MAX, which is deliberately larger than both: the two
// searchers agree on the prominent items and diverge on the tail, and
// the tail is what the alternates line is for.
#define WM_SEARCH_LIMIT  5

// Property words resolve to a small ranked set, not a single answer, and
// the first candidate with a statement on the subject wins. Three is
// where the measured misses live: `height` puts the wanted P2048 at rank
// 2, behind P2044 (elevation).
#define WM_PROP_CANDIDATES 3

// A P1963 menu can run past a hundred entries (Q5 human has 103) and is
// priority-ordered, so the head is the useful part. We fetch metadata
// for the first WM_MENU_FETCH — the action API's ids= ceiling is 50 —
// and keep the first WM_MENU_MAX that survive the external-id filter.
#define WM_MENU_FETCH    50

// The action API refuses more than 50 ids in one wbgetentities call, and
// every batch this plugin builds is bounded by that rather than by its
// own arithmetic.
#define WM_IDS_MAX       50

// Flight slots one piece of work may hold at once: three concurrent legs
// (the resolver's two searchers, or a property fan-out) plus the batched
// label lookup that follows them. A verb opens only what it uses, and an
// unopened handle reads 0, which curl_flight_close ignores.
#define WM_WORK_SLOTS    4

// How long wm_stop() waits for its own completion callbacks after
// cancelling them. The same budget every flighted plugin uses.
#define WM_STOP_DRAIN_MS 3000

#define WM_UA_DEFAULT \
  "botmanager/1.0 (+set plugin.wikimedia.user_agent; contact your operator)"

// Cache sizing. Entities, their menus and the property vocabulary are
// all near-static; what varies is how many distinct ones one channel
// asks about in a session.
#define WM_RESOLVE_CACHE_SZ 24
#define WM_MENU_CACHE_SZ    12
#define WM_PROP_CACHE_SZ    48

// ----------------------------------------------------------------------
// The parse layer (wikimedia_parse.c). Every function here is a pure
// function of its arguments — no KV, no curl, no module state.
// ----------------------------------------------------------------------

// True for a Wikidata id of the given series, 'Q' or 'P', in either
// case; wm_qid_norm writes the form the API will accept.
bool    wm_is_qid(const char *s, char kind);
void    wm_qid_norm(const char *in, char *out, size_t cap);

// Lowercase, trim and collapse whitespace — the form a cache key and a
// property word are both held in.
void    wm_normalize(const char *in, char *out, size_t cap);

// snprintf-style: returns the bytes the full encoding needs, so
// truncation is `ret >= cap` at the call site.
size_t  wm_urlencode(const char *in, char *out, size_t cap);

void    wm_time_render(struct json_object *val, wm_time_t *out, char *text,
            size_t cap);
void    wm_uri_qid(const char *uri, char *out, size_t cap);
void    wm_snak_parse(struct json_object *snak, wm_value_t *out);
bool    wm_value_same(const wm_value_t *a, const wm_value_t *b);
uint8_t wm_quals_parse(struct json_object *st, wm_qualifier_t *out,
            uint8_t cap);

// One property's statements, filtered and ordered the way a reader
// expects: deprecated dropped, preferred first, duplicates collapsed.
uint8_t wm_claims_parse(struct json_object *claims, const char *property,
            wm_claim_t *out, uint8_t cap);

void    wm_id_push(char (*ids)[WM_QID_SZ], uint8_t *n, uint8_t cap,
            const char *id);
uint8_t wm_ids_collect(const wm_claims_res_t *res, const char *extra,
            char (*out)[WM_QID_SZ], uint8_t cap);
bool    wm_ids_join(char (*ids)[WM_QID_SZ], uint8_t n, char *out, size_t cap);

const char *wm_label_of(struct json_object *entities, const char *id,
            const char *lang);
void    wm_labels_apply(struct json_object *entities, wm_claims_res_t *res,
            const char *lang);

// Sitelink count is the ranking key; hits[0] is the winner afterwards.
void    wm_rank_window(wm_candidate_t *hits, uint8_t n);

#endif // WIKIMEDIA_INTERNAL

#endif // BM_WIKIMEDIA_H
