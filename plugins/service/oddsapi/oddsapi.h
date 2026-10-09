#ifndef BM_ODDSAPI_H
#define BM_ODDSAPI_H

// Internal header of the oddsapi plugin, shared by its three TUs:
//
//   oddsapi.c        the provider — the wire, the quota, the cache
//   oddsapi_parse.c  The Odds API's replies turned into this plugin's
//                    types, and a game priced from its books; no KV, no
//                    curl, no module state, which is what lets
//                    tests/test_oddsapi_parse.c drive it directly
//   oddsapi_cmd.c    the command surface (its own header, oddsapi_cmd.h)
//
// Gated by ODDSAPI_INTERNAL, which every TU defines before including
// this header. Each of the first two TUs keeps its own statics one gate
// deeper — ODDSAPI_PROVIDER_INTERNAL, ODDSAPI_PARSE_INTERNAL — so a
// sibling never sees a static it does not define. The public mechanism
// API is oddsapi_api.h.

#ifdef ODDSAPI_INTERNAL

#include "common.h"   // first: it defines _GNU_SOURCE, which strcasestr needs
#include "alloc.h"
#include "json.h"

#include "oddsapi_api.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// A price outside this magnitude is not an American price; the parser
// stores it as absent. The ceiling only keeps a hostile double out of
// lround()'s undefined range — real long shots stop near +100000.
#define ODDSAPI_PX_MIN      100
#define ODDSAPI_PX_MAX      1000000

// Points are half-integers; two closer than this are the same line.
#define ODDSAPI_PT_EPS      0.01f

// Rows kept from one reply. A list element costs this plugin ~1.4 KB
// against the ~60 bytes it costs the sender, so the array the reply
// claims is never what sizes an allocation. A real slate is tens of
// games; a college-basketball week is a few hundred.
#define ODDSAPI_ROWS_MAX    1024

// Cache slots: a league's odds, events, scores and futures, across every
// league anyone asks for. Shared because `show odds` sizes its rows by it.
#define ODDSAPI_CACHE_SZ    48

// A team query shorter than this matches too much to mean anything.
#define ODDSAPI_QUERY_MIN   3

// Price conversion. Medians are taken in decimal space, because American
// odds are discontinuous across ±100: the median of -110 and +110 is
// +100 (decimal 2.0), not 0.
double  oddsapi_am_to_dec(int32_t am);
int32_t oddsapi_dec_to_am(double dec);

// Reply parsers — the trust boundary for every byte the API sends. Each
// fills `out` from a whole reply body and returns false only when the
// body is not a JSON array; a malformed element is skipped, not stored
// half-filled. Arrays are mem_alloc'd and NULL when empty; the matching
// _clear frees them and zeroes the struct, and is safe on a zeroed one.
bool oddsapi_parse_odds(const char *body, size_t len, oddsapi_slate_t *out);
bool oddsapi_parse_events(const char *body, size_t len,
    oddsapi_events_t *out);
bool oddsapi_parse_scores(const char *body, size_t len,
    oddsapi_scores_t *out);
bool oddsapi_parse_sports(const char *body, size_t len,
    oddsapi_sports_t *out);
bool oddsapi_parse_futures(const char *body, size_t len,
    oddsapi_futures_t *out);

void oddsapi_slate_clear(oddsapi_slate_t *s);
void oddsapi_events_clear(oddsapi_events_t *e);
void oddsapi_scores_clear(oddsapi_scores_t *s);
void oddsapi_sports_clear(oddsapi_sports_t *s);
void oddsapi_futures_clear(oddsapi_futures_t *f);

// Consensus is the median per column across the books that quote it;
// the spread and total columns first settle on the modal point and
// price only the books standing on it. Best is the highest payout per
// column under the same point rule — -108 at -2.5 is not "better" than
// -110 at -3.0, it is a different bet.
void oddsapi_consensus(const oddsapi_game_t *g, oddsapi_consensus_t *out);
void oddsapi_best(const oddsapi_game_t *g, const oddsapi_consensus_t *c,
    oddsapi_best_t out[ODDSAPI_COL_COUNT]);
void oddsapi_contender_consensus(const oddsapi_contender_t *c,
    int32_t *median_px, oddsapi_best_t *best);

// Favourite first; a contender nobody prices sorts last.
void oddsapi_futures_sort(oddsapi_futures_t *f);

// Case-insensitive alias, or an exact sport key; NULL otherwise.
const oddsapi_league_t *oddsapi_league_find(const char *word);
const oddsapi_league_t *oddsapi_league_table(size_t *n);

// Substring match, case-insensitive, refused below ODDSAPI_QUERY_MIN.
// "new york" matching two teams is expected; the caller lists the rest.
bool oddsapi_team_match(const char *team, const char *query);

#ifdef ODDSAPI_PARSE_INTERNAL

// The leagues the commands know by name; a NULL outright key means no
// futures market. Every key was checked against /v4/sports?all=true
// (tests/oddsapi/live_sports.json): the API lists no EPL outright.
static const oddsapi_league_t oddsapi_leagues[] = {
  { "nfl",    NULL,  "americanfootball_nfl",
    "americanfootball_nfl_super_bowl_winner",       "NFL"     },
  { "ncaaf",  "cfb", "americanfootball_ncaaf",
    "americanfootball_ncaaf_championship_winner",   "NCAAF"   },
  { "nba",    NULL,  "basketball_nba",
    "basketball_nba_championship_winner",           "NBA"     },
  { "ncaab",  "cbb", "basketball_ncaab",
    "basketball_ncaab_championship_winner",         "NCAAB"   },
  { "wnba",   NULL,  "basketball_wnba",            NULL,     "WNBA"    },
  { "mlb",    NULL,  "baseball_mlb",
    "baseball_mlb_world_series_winner",             "MLB"     },
  { "nhl",    NULL,  "icehockey_nhl",
    "icehockey_nhl_championship_winner",            "NHL"     },
  { "mls",    NULL,  "soccer_usa_mls",             NULL,     "MLS"     },
  { "epl",    NULL,  "soccer_epl",                 NULL,     "EPL"     },
  { "laliga", NULL,  "soccer_spain_la_liga",       NULL,     "La Liga" },
  { "ucl",    NULL,  "soccer_uefa_champs_league",  NULL,     "UCL"     },
  { "ufc",    "mma", "mma_mixed_martial_arts",     NULL,     "MMA"     },
  { "boxing", NULL,  "boxing_boxing",              NULL,     "Boxing"  },
};

static struct json_object *oddsapi_root_array(const char *body, size_t len,
    size_t *n);
static bool    oddsapi_event_head(struct json_object *ev, char *id,
    char *home, char *away, time_t *commence);
static int32_t oddsapi_price(struct json_object *outcome);
static bool    oddsapi_point(struct json_object *outcome, float *out);
static double  oddsapi_median(double *v, uint8_t n);
static bool    oddsapi_modal_point(const float *pts, uint8_t n, float *out);
static bool    oddsapi_line_parse(struct json_object *book,
    const oddsapi_game_t *g, oddsapi_line_t *out);
static int     oddsapi_game_cmp(const void *a, const void *b);
static int     oddsapi_event_cmp(const void *a, const void *b);
static int     oddsapi_score_cmp(const void *a, const void *b);
static int     oddsapi_contender_cmp(const void *a, const void *b);
static int32_t oddsapi_col_px(const oddsapi_line_t *l, oddsapi_col_t col);
static bool    oddsapi_col_on_point(const oddsapi_line_t *l,
    oddsapi_col_t col, const oddsapi_consensus_t *c);
static void    oddsapi_book_futures(struct json_object *book,
    oddsapi_futures_t *out, uint32_t cap);

#endif // ODDSAPI_PARSE_INTERNAL

#ifdef ODDSAPI_PROVIDER_INTERNAL

#include "clam.h"
#include "curl.h"
#include "curl_flight.h"
#include "kv.h"
#include "plugin.h"

#include <errno.h>
#include <pthread.h>

#define ODDSAPI_API_BASE      "https://api.the-odds-api.com/v4/sports"
#define ODDSAPI_URL_SZ        1024   // holds the ?apiKey= too

// A 429 carries no Retry-After here, so the stand-down is ours to pick.
#define ODDSAPI_BACKOFF_S     60

// How long oddsapi_stop() waits for its own completion callbacks. A
// cancelled transfer is delivered on the multi loop's next pass, so this
// is a scheduling margin, not a network timeout.
#define ODDSAPI_STOP_DRAIN_MS 3000

// The 401 body that means the key is fine and the month is spent.
#define ODDSAPI_OUT_OF_CREDITS "OUT_OF_USAGE_CREDITS"

// One published reply. Immutable once it is in the cache, and shared by
// reference: delivery takes a ref under oddsapi_mu, calls back outside
// it and drops the ref after, so a 150 KB slate is never copied per
// reader. The cache holds one ref of its own; the last drop frees.
typedef struct
{
  uint32_t       refs;                      // under oddsapi_mu
  oddsapi_kind_t kind;
  time_t         fetched;
  union
  {
    oddsapi_slate_t   slate;
    oddsapi_events_t  events;
    oddsapi_scores_t  scores;
    oddsapi_sports_t  sports;
    oddsapi_futures_t futures;
  } u;
} oddsapi_snap_t;

typedef struct oddsapi_waiter
{
  oddsapi_cb_t           cb;
  void                  *user;
  struct oddsapi_waiter *next;
} oddsapi_waiter_t;

// A slot is free when key[0] is NUL. An entry in flight is never
// evicted, so the completion can hold a pointer to it as its user data.
typedef struct
{
  char              key[ODDSAPI_CACHE_KEY_SZ];
  oddsapi_kind_t    kind;
  oddsapi_snap_t   *snap;                   // NULL until the first success
  bool              inflight;
  oddsapi_waiter_t *waiters;                // FIFO, head first
  oddsapi_waiter_t *tail;
  uint64_t          slot;                   // oddsapi_flight, while inflight
} oddsapi_entry_t;

typedef struct
{
  const char *name;                         // the cache key's prefix
  const char *ttl_key;
  bool        paid;
} oddsapi_kind_info_t;

static const oddsapi_kind_info_t oddsapi_kinds[ODDSAPI_K_COUNT] = {
  [ODDSAPI_K_SPORTS]  = { "sports",  "plugin.oddsapi.sports_ttl",  false },
  [ODDSAPI_K_EVENTS]  = { "events",  "plugin.oddsapi.events_ttl",  false },
  [ODDSAPI_K_ODDS]    = { "odds",    "plugin.oddsapi.odds_ttl",    true  },
  [ODDSAPI_K_FUTURES] = { "futures", "plugin.oddsapi.futures_ttl", true  },
  [ODDSAPI_K_SCORES]  = { "scores",  "plugin.oddsapi.scores_ttl",  true  },
};

// Module state. oddsapi_mu covers the cache, every snap's refcount, the
// quota picture and the backoff. Nothing is called under it that could
// come back for it: no curl, no clam, no callback, no KV read.
static pthread_mutex_t oddsapi_mu;
static oddsapi_entry_t oddsapi_cache[ODDSAPI_CACHE_SZ];
static oddsapi_quota_t oddsapi_quota;
static int64_t         oddsapi_backoff_until;   // CLOCK_MONOTONIC seconds

// Every request on the wire, so oddsapi_stop() can wait out their
// callbacks before oddsapi_deinit() destroys the mutex they take.
static curl_flight_t   oddsapi_flight;

static const plugin_kv_entry_t oddsapi_kv_schema[] = {
  { "plugin.oddsapi.creds.apikey", KV_STR, "",
    "The Odds API key (sent as ?apiKey=). The free tier at the-odds-api.com "
    "is 500 credits a month.", NULL, NULL },
  { "plugin.oddsapi.bookmakers", KV_STR,
    "draftkings,fanduel,betmgm,williamhill_us,fanatics,betrivers,espnbet,"
    "hardrockbet",
    "Bookmaker keys asked for, comma-separated; consensus is their median. "
    "Up to 10 bill as one region; empty asks for the whole us region.",
    NULL, NULL },
  { "plugin.oddsapi.odds_ttl", KV_UINT32, "1800",
    "How long a league's lines are served before the next pull costs 3 "
    "credits (seconds).", NULL, NULL },
  { "plugin.oddsapi.futures_ttl", KV_UINT32, "21600",
    "Outright-market cache; a pull costs 1 credit (seconds).", NULL, NULL },
  { "plugin.oddsapi.scores_ttl", KV_UINT32, "600",
    "Scores cache; a pull costs 2 credits (seconds).", NULL, NULL },
  { "plugin.oddsapi.events_ttl", KV_UINT32, "3600",
    "Game-list cache, used to find a team's next game; free (seconds).",
    NULL, NULL },
  { "plugin.oddsapi.sports_ttl", KV_UINT32, "21600",
    "Sports-list cache, which says what is in season; free (seconds).",
    NULL, NULL },
  { "plugin.oddsapi.quota_reserve", KV_UINT32, "25",
    "Below this many credits left, never spend: serve the cached copy with "
    "its age, or refuse.", NULL, NULL },
  { "plugin.oddsapi.search_leagues", KV_STR, "nfl,ncaaf,nba,wnba,mlb,nhl,mls,epl",
    "Leagues a bare team name is searched in; out-of-season ones are "
    "skipped.", NULL, NULL },
  { "plugin.oddsapi.rows", KV_UINT32, "10",
    "Row cap for a slate, scores or futures list.", NULL, NULL },
  { "plugin.oddsapi.timeout", KV_UINT32, "8",
    "Per-request HTTP timeout (seconds); 0 uses the curl default.",
    NULL, NULL },
};

static int64_t          oddsapi_mono(void);
static bool             oddsapi_key_ok(const char *s);
static size_t           oddsapi_urlencode(const char *in, char *out,
    size_t cap);
static bool             oddsapi_url(oddsapi_kind_t kind, const char *arg,
    const char *apikey, char *url, size_t cap);
static int32_t          oddsapi_hdr_int(const curl_response_t *resp,
    const char *name);
static uint32_t         oddsapi_snap_items(const oddsapi_snap_t *snap);
static void             oddsapi_snap_free(oddsapi_snap_t *snap);
static void             oddsapi_snap_unref_locked(oddsapi_snap_t *snap);
static void             oddsapi_snap_drop(oddsapi_snap_t *snap);
static oddsapi_snap_t  *oddsapi_snap_parse(oddsapi_kind_t kind,
    const char *body, size_t len);
static void             oddsapi_deliver(oddsapi_cb_t cb, void *user,
    oddsapi_kind_t kind, oddsapi_status_t status, const char *message,
    const oddsapi_snap_t *snap, bool stale);
static void             oddsapi_settle(oddsapi_entry_t *e,
    oddsapi_snap_t *fresh, oddsapi_status_t status, const char *message,
    const oddsapi_waiter_t *skip);
static oddsapi_entry_t *oddsapi_entry_claim_locked(const char *key,
    oddsapi_kind_t kind);
static const char      *oddsapi_hold_reason_locked(uint32_t reserve,
    oddsapi_status_t *st);
static async_rc_t       oddsapi_fetch(oddsapi_kind_t kind, const char *arg,
    oddsapi_cb_t cb, void *user);
static void             oddsapi_done(const curl_response_t *resp);

static bool             oddsapi_init(void);
static bool             oddsapi_stop(void);
static void             oddsapi_deinit(void);

#endif // ODDSAPI_PROVIDER_INTERNAL

#endif // ODDSAPI_INTERNAL

#endif // BM_ODDSAPI_H
