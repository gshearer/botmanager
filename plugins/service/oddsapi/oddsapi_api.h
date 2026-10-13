#ifndef BM_ODDSAPI_API_H
#define BM_ODDSAPI_API_H

// Public mechanism API for the oddsapi service plugin — betting lines,
// futures and scores from The Odds API (the-odds-api.com, v4). Pure
// mechanism: this plugin fetches and normalizes; its command TU owns
// every byte of presentation.
//
// Normalization conventions (the service normalizes; the consumer only
// presents): a price is an American integer and 0 means absent — a real
// American price is never inside ±100, so 0 cannot collide with one. A
// spread or total point means something only beside a non-zero price.
// Sides are matched by name against the game's home and away teams,
// never by their position in the reply. Every timestamp is unix
// seconds (the plugin asks with dateFormat=unix). All strings are ""
// when the source omits them. Every payload — the envelope and any
// array it points at — is valid only for the duration of the callback;
// consumers that must outlive it deep-copy.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// The plugin's name, and so its dlsym handle, the root of its clam
// contexts and the module its commands register under.
#define ODDSAPI_CTX "oddsapi"

// ----------------------------------------------------------------------
// Fixed sizes. Fixed-width strings let consumers stack-allocate rows and
// sidestep inner-string lifetime ambiguity.
// ----------------------------------------------------------------------

#define ODDSAPI_BOOKS_MAX     10   // the API bills ≤10 bookmakers as one region
#define ODDSAPI_ID_SZ         40   // event ids are 32 hex digits
#define ODDSAPI_TEAM_SZ       64
#define ODDSAPI_SPORT_KEY_SZ  64
#define ODDSAPI_TITLE_SZ      64
#define ODDSAPI_BOOK_KEY_SZ   32
#define ODDSAPI_SCORE_SZ      8    // the API sends a score as a string
#define ODDSAPI_MARKET_SZ     40   // "player_reception_yds_alternate"

// ----------------------------------------------------------------------
// Results
// ----------------------------------------------------------------------

// One book's three markets for one game. Prices are int32_t rather than
// anything narrower because a futures long shot is quoted past +32767.
typedef struct
{
  char    key[ODDSAPI_BOOK_KEY_SZ];         // "draftkings"
  char    title[ODDSAPI_TITLE_SZ];          // "DraftKings"
  int32_t ml_away, ml_home, ml_draw;        // h2h; ml_draw only in 3-way sports
  float   spr_home_pt;                      // home team's spread point
  int32_t spr_home_px, spr_away_px;         // the away point is -spr_home_pt
  float   tot_pt;
  int32_t over_px, under_px;
} oddsapi_line_t;

typedef struct
{
  char           id[ODDSAPI_ID_SZ];
  char           home[ODDSAPI_TEAM_SZ];
  char           away[ODDSAPI_TEAM_SZ];
  time_t         commence;
  uint8_t        nbooks;
  oddsapi_line_t books[ODDSAPI_BOOKS_MAX];
} oddsapi_game_t;

// One league's odds pull. `games` is NULL when ngames is 0.
typedef struct
{
  char            sport_key[ODDSAPI_SPORT_KEY_SZ];
  char            sport_title[ODDSAPI_TITLE_SZ];
  uint32_t        ngames;
  oddsapi_game_t *games;                    // sorted by commence, then id
} oddsapi_slate_t;

// The free game list a team search runs over.
typedef struct
{
  char   id[ODDSAPI_ID_SZ];
  char   home[ODDSAPI_TEAM_SZ];
  char   away[ODDSAPI_TEAM_SZ];
  time_t commence;
} oddsapi_event_t;

typedef struct
{
  char             sport_key[ODDSAPI_SPORT_KEY_SZ];
  uint32_t         n;
  oddsapi_event_t *events;                  // sorted by commence, then id
} oddsapi_events_t;

// The API carries no clock or period, so a live game is known only as
// `!completed && has_scores`.
typedef struct
{
  char   id[ODDSAPI_ID_SZ];
  char   home[ODDSAPI_TEAM_SZ];
  char   away[ODDSAPI_TEAM_SZ];
  time_t commence;
  bool   completed;
  bool   has_scores;                        // false while the API sends null
  char   home_score[ODDSAPI_SCORE_SZ];
  char   away_score[ODDSAPI_SCORE_SZ];
  time_t last_update;                       // 0 when null
} oddsapi_score_t;

typedef struct
{
  char             sport_key[ODDSAPI_SPORT_KEY_SZ];
  char             sport_title[ODDSAPI_TITLE_SZ];
  uint32_t         n;
  oddsapi_score_t *scores;                  // sorted by commence, then id
} oddsapi_scores_t;

typedef struct
{
  char key[ODDSAPI_SPORT_KEY_SZ];
  char group[ODDSAPI_TITLE_SZ];
  char title[ODDSAPI_TITLE_SZ];
  bool active;
  bool has_outrights;
} oddsapi_sport_t;

typedef struct
{
  uint32_t         n;
  oddsapi_sport_t *sports;                  // in the API's order
} oddsapi_sports_t;

// One outright contender. `book[i]` indexes the futures' book table and
// names who quoted `px[i]`.
typedef struct
{
  char    name[ODDSAPI_TEAM_SZ];
  uint8_t n;
  int32_t px[ODDSAPI_BOOKS_MAX];
  uint8_t book[ODDSAPI_BOOKS_MAX];
} oddsapi_contender_t;

typedef struct
{
  char                 sport_key[ODDSAPI_SPORT_KEY_SZ];
  char                 sport_title[ODDSAPI_TITLE_SZ];
  uint8_t              nbooks;
  char                 book_key[ODDSAPI_BOOKS_MAX][ODDSAPI_BOOK_KEY_SZ];
  char                 book_title[ODDSAPI_BOOKS_MAX][ODDSAPI_TITLE_SZ];
  uint32_t             n;
  oddsapi_contender_t *contenders;          // one per distinct outcome name
} oddsapi_futures_t;

// One player's line in one prop market, across the books that quote it.
// `over` holds Over — or Yes, in a market with no line such as anytime
// TD — and `under` Under or No; 0 = that book does not offer the side.
// Slot i of book/pt/over/under is one book; `book[i]` indexes the
// props' book table.
typedef struct
{
  char    market[ODDSAPI_MARKET_SZ];        // "player_pass_yds"
  char    player[ODDSAPI_TEAM_SZ];
  bool    has_point;                        // false for a Yes/No market
  uint8_t n;
  uint8_t book[ODDSAPI_BOOKS_MAX];
  float   pt[ODDSAPI_BOOKS_MAX];
  int32_t over[ODDSAPI_BOOKS_MAX];
  int32_t under[ODDSAPI_BOOKS_MAX];
} oddsapi_prop_t;

// One game's player props. `props` is NULL when n is 0.
typedef struct
{
  char            id[ODDSAPI_ID_SZ];
  char            sport_key[ODDSAPI_SPORT_KEY_SZ];
  char            home[ODDSAPI_TEAM_SZ];
  char            away[ODDSAPI_TEAM_SZ];
  time_t          commence;
  uint8_t         nbooks;
  char            book_key[ODDSAPI_BOOKS_MAX][ODDSAPI_BOOK_KEY_SZ];
  char            book_title[ODDSAPI_BOOKS_MAX][ODDSAPI_TITLE_SZ];
  uint32_t        n;
  oddsapi_prop_t *props;                    // by market, then oddsapi_props_sort
} oddsapi_props_t;

// ----------------------------------------------------------------------
// Consensus and best price
// ----------------------------------------------------------------------

typedef enum
{
  ODDSAPI_COL_ML_AWAY = 0,
  ODDSAPI_COL_ML_HOME,
  ODDSAPI_COL_ML_DRAW,
  ODDSAPI_COL_SPR_HOME,
  ODDSAPI_COL_SPR_AWAY,
  ODDSAPI_COL_OVER,
  ODDSAPI_COL_UNDER,
  ODDSAPI_COL_COUNT
} oddsapi_col_t;

// The spread and total columns are priced only at the modal point, so a
// consensus price always belongs to the point printed beside it.
typedef struct
{
  int32_t px[ODDSAPI_COL_COUNT];            // consensus price per column, 0 = none
  float   spr_home_pt;                      // modal point the spread prices sit at
  float   tot_pt;                           // modal total
  uint8_t n[ODDSAPI_COL_COUNT];             // books that contributed
} oddsapi_consensus_t;

typedef struct
{
  int32_t px;                               // 0 = no book offered it
  uint8_t book;                             // index into the game's books
} oddsapi_best_t;

// A prop priced like a spread: the modal line, then the median and the
// best of each side among the books standing on it.
typedef struct
{
  float          pt;                        // 0 in a Yes/No market
  int32_t        over;                      // median Over (or Yes), 0 = none
  int32_t        under;
  oddsapi_best_t best_over;                 // .book indexes the props' books
  oddsapi_best_t best_under;
  uint8_t        n;                         // books standing on the line
} oddsapi_prop_line_t;

// A player-prop market the commands ask for, and how a reply labels it.
typedef struct
{
  const char *key;                          // "player_pass_yds"
  const char *label;                        // "pass yds"
} oddsapi_prop_mkt_t;

// A league the commands know by name. `alt` is a second alias or NULL;
// `outright_key` is NULL when the league has no futures market, and
// `props` NULL when it has no player props — else the curated markets
// one props ask pulls, ended by a NULL key.
typedef struct
{
  const char               *alias;          // "ncaaf"
  const char               *alt;            // "cfb"
  const char               *sport_key;      // "americanfootball_ncaaf"
  const char               *outright_key;
  const char               *label;          // "NCAAF"
  const oddsapi_prop_mkt_t *props;
} oddsapi_league_t;

// ----------------------------------------------------------------------
// The result envelope
// ----------------------------------------------------------------------

// ODDSAPI_OK is 0, so a two-state test reads success correctly. What it
// loses is the reason, and every failure value obliges the same thing —
// say `message` and stop — so nothing is lost that a caller acts on.
typedef enum
{
  ODDSAPI_OK = 0,
  ODDSAPI_NOT_FOUND,        // the API does not know that sport key
  ODDSAPI_RATE_LIMITED,     // 429, or our own backoff after one
  ODDSAPI_AUTH,             // the key was refused
  ODDSAPI_QUOTA,            // out of credits, or below the reserve
  ODDSAPI_TRANSPORT,        // no answer, or an HTTP status we do not map
  ODDSAPI_PARSE             // an answer that was not the documented shape
} oddsapi_status_t;

typedef enum
{
  ODDSAPI_K_SPORTS = 0,     // free
  ODDSAPI_K_EVENTS,         // free
  ODDSAPI_K_ODDS,           // 3 credits: h2h, spreads, totals
  ODDSAPI_K_FUTURES,        // 1 credit
  ODDSAPI_K_SCORES,         // 2 credits
  ODDSAPI_K_PROPS,          // 1 credit per market the reply carries
  ODDSAPI_K_COUNT
} oddsapi_kind_t;

// The account as the API last described it. Every reply carries the
// three counters — free calls included — so `seen` is the last reply of
// any kind. -1 = no reply yet since the plugin loaded.
typedef struct
{
  int32_t  remaining;
  int32_t  used;
  int32_t  last;                            // credits the last call cost
  time_t   seen;
  uint32_t backoff_left;                    // seconds of 429 stand-down left
} oddsapi_quota_t;

#define ODDSAPI_CACHE_KEY_SZ 96

typedef struct
{
  char     key[ODDSAPI_CACHE_KEY_SZ];       // "odds:americanfootball_nfl"
  time_t   fetched;                         // 0 = never answered
  uint32_t items;
  bool     inflight;
} oddsapi_cache_row_t;

// `stale` is true when the data is older than its TTL and was served
// anyway — the quota reserve, the backoff, or a failed refresh — and
// `message` then names why. On ODDSAPI_OK exactly one of the six
// pointers is non-NULL, the one `kind` names; on a failure all are NULL.
typedef struct
{
  oddsapi_status_t         status;
  oddsapi_kind_t           kind;
  char                     message[128];    // "" on a fresh success
  bool                     stale;
  time_t                   fetched;         // when the data left the API
  const oddsapi_slate_t   *slate;
  const oddsapi_events_t  *events;
  const oddsapi_scores_t  *scores;
  const oddsapi_sports_t  *sports;
  const oddsapi_futures_t *futures;
  const oddsapi_props_t   *props;
} oddsapi_res_t;

// Runs on the curl worker thread, or on the caller's own thread when a
// cache answers in place — fast and non-blocking either way.
//
// This plugin registers NO plugin_unmap_notify listener, and is the one
// shape of async service that legitimately needs none: it is a single
// .so whose only caller is its own command surface (oddsapi_cmd.c), so a
// stored completion always names THIS mapping. Core sees it — quiesce
// and audit range-test curl_iter_req_t.cb, which for our transfers
// points here too — and refuses the dlclose while a request is
// airborne. ⚠ That holds only while nothing else calls in. The first
// consumer in another plugin makes this the defect written up in
// PLUGIN.md §Lifecycle Contract ("hand back pointers other plugins gave
// you"); copy reachyapi.c's registry before adding one.
typedef void (*oddsapi_cb_t)(const oddsapi_res_t *res, void *user);

// ----------------------------------------------------------------------
// Real function declarations — visible only inside the oddsapi plugin.
//
// The six fetches share one return contract:
//
//   ASYNC_AIRBORNE            the callback owns `user`: the request is on
//                             the wire, joined an identical one already
//                             there, or was ANSWERED IN PLACE from cache
//   ASYNC_FAILED_DELIVERED    refused, and the callback already ran with
//                             the reason — the quota reserve or the 429
//                             backoff, with no cached copy to serve
//   ASYNC_FAILED_UNDELIVERED  refused before anything ran, `user` is
//                             still yours: NULL callback, a missing or
//                             malformed sport key, no API key, every
//                             cache slot busy, or curl refused the submit
//
// ⚠ ASYNC_AIRBORNE does not mean "later". A warm cache, a joined twin
// whose answer lands first, and both refusals run the callback before
// the call returns, so a caller holding a lock across the call can
// re-enter itself through its own callback.
//
// A second caller for a key already on the wire joins it rather than
// spending again: two `!odds nfl` in one second cost one pull.
// ----------------------------------------------------------------------

#ifdef ODDSAPI_INTERNAL

#include "async.h"

// True when plugin.oddsapi.creds.apikey is set.
bool oddsapi_configured(void);

async_rc_t oddsapi_sports_async(oddsapi_cb_t cb, void *user);
async_rc_t oddsapi_events_async(const char *sport_key, oddsapi_cb_t cb,
    void *user);
async_rc_t oddsapi_odds_async(const char *sport_key, oddsapi_cb_t cb,
    void *user);
async_rc_t oddsapi_futures_async(const char *outright_key, oddsapi_cb_t cb,
    void *user);
async_rc_t oddsapi_scores_async(const char *sport_key, oddsapi_cb_t cb,
    void *user);

// One game's player props, in the curated markets of the league whose
// sport key this is; refused undelivered for a league with none.
async_rc_t oddsapi_props_async(const char *sport_key, const char *event_id,
    oddsapi_cb_t cb, void *user);

void     oddsapi_quota_get(oddsapi_quota_t *out);

// Up to `cap` rows, one per occupied cache slot; returns how many.
uint32_t oddsapi_cache_rows(oddsapi_cache_row_t *rows, uint32_t cap);

#endif // ODDSAPI_INTERNAL

// ----------------------------------------------------------------------
// dlsym shims (consumer side)
// ----------------------------------------------------------------------

#ifndef ODDSAPI_INTERNAL

#include "async.h"
#include "clam.h"
#include "plugin.h"

#include <stdlib.h>  // abort

// Resolve one symbol into its shim's cache slot, or die naming it — a
// shim with nothing behind it has no honest value to return.
static inline void *
oddsapi_sym(const char *name, void **slot)
{
  void *obj = __atomic_load_n(slot, __ATOMIC_ACQUIRE);

  if(obj != NULL)
    return(obj);

  obj = plugin_dlsym_cached(ODDSAPI_CTX, name, slot);

  if(obj == NULL)
  {
    clam(CLAM_FATAL, ODDSAPI_CTX, "dlsym failed: %s", name);
    abort();
  }

  __atomic_store_n(slot, obj, __ATOMIC_RELEASE);
  return(obj);
}

// One shim body: resolve, launder void* to the function type, call.
#define ODDSAPI_SHIM(ret, name, params, args)                            \
  static inline ret                                                      \
  name params                                                            \
  {                                                                      \
    typedef ret (*fn_t) params;                                          \
    static void *cached = NULL;                                          \
    union { void *obj; fn_t fn; } u;                                     \
                                                                         \
    u.obj = oddsapi_sym(#name, &cached);                                 \
    return(u.fn args);                                                   \
  }

ODDSAPI_SHIM(bool, oddsapi_configured, (void), ())
ODDSAPI_SHIM(async_rc_t, oddsapi_sports_async,
    (oddsapi_cb_t cb, void *user), (cb, user))
ODDSAPI_SHIM(async_rc_t, oddsapi_events_async,
    (const char *sport_key, oddsapi_cb_t cb, void *user),
    (sport_key, cb, user))
ODDSAPI_SHIM(async_rc_t, oddsapi_odds_async,
    (const char *sport_key, oddsapi_cb_t cb, void *user),
    (sport_key, cb, user))
ODDSAPI_SHIM(async_rc_t, oddsapi_futures_async,
    (const char *outright_key, oddsapi_cb_t cb, void *user),
    (outright_key, cb, user))
ODDSAPI_SHIM(async_rc_t, oddsapi_scores_async,
    (const char *sport_key, oddsapi_cb_t cb, void *user),
    (sport_key, cb, user))
ODDSAPI_SHIM(async_rc_t, oddsapi_props_async,
    (const char *sport_key, const char *event_id, oddsapi_cb_t cb,
     void *user), (sport_key, event_id, cb, user))
ODDSAPI_SHIM(uint32_t, oddsapi_cache_rows,
    (oddsapi_cache_row_t *rows, uint32_t cap), (rows, cap))

#undef ODDSAPI_SHIM

// By hand: ISO C forbids `return` with a void expression.
static inline void
oddsapi_quota_get(oddsapi_quota_t *out)
{
  typedef void (*fn_t)(oddsapi_quota_t *);
  static void *cached = NULL;
  union { void *obj; fn_t fn; } u;

  u.obj = oddsapi_sym("oddsapi_quota_get", &cached);
  u.fn(out);
}

#endif // !ODDSAPI_INTERNAL

#endif // BM_ODDSAPI_API_H
