#ifndef BM_ODDSAPI_H
#define BM_ODDSAPI_H

// Internal header of the oddsapi plugin. The parse layer
// (oddsapi_parse.c) turns The Odds API's replies into this plugin's
// types and prices a game from its books; it touches no KV, no curl and
// no module state, which is what lets tests/test_oddsapi_parse.c drive
// it directly. The public mechanism API is oddsapi_api.h.
//
// Gated by ODDSAPI_INTERNAL, which every TU of the plugin defines before
// including this header. The parse TU's own helpers sit one gate deeper,
// behind ODDSAPI_PARSE_INTERNAL, so a sibling TU never sees a static it
// does not define.

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
// futures market. ⚠ The NCAAF, NCAAB, NHL and EPL outright keys are not
// yet confirmed against a live /v4/sports?all=true reply.
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
  { "epl",    NULL,  "soccer_epl",                 "soccer_epl_winner", "EPL" },
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

#endif // ODDSAPI_INTERNAL

#endif // BM_ODDSAPI_H
