#ifndef BM_ODDSAPI_CMD_H
#define BM_ODDSAPI_CMD_H

// The oddsapi plugin's command surface: `show odds` and `odds`, with
// `scores` still to land. Pure presentation — every byte the user sees
// is shaped here, while fetching, caching and pricing stay in the
// service half of the same .so.
//
// Both halves ship as one plugin because oddsapi is a dependency-graph
// leaf (nothing requires service_oddsapi), so the command's upward
// bot_chat dependency is inherited by nobody. See `PLUGIN.md §Layer
// Rules` Rule 1, leaf exception.

#include <stdbool.h>

// Driven by oddsapi.c's lifecycle: the service half owns the plugin
// descriptor, so it raises and lowers the command surface with it.
bool oddsapi_cmd_register(void);
void oddsapi_cmd_unregister(void);

#ifdef ODDSAPI_CMD_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "colors.h"
#include "display.h"
#include "kv.h"
#include "method.h"
#include "userns.h"
#include "util.h"

#include <time.h>

#define ODDSAPI_CMD_CTX      ODDSAPI_CTX ".cmd"
#define ODDSAPI_CMD_REPLY_SZ 512

// The `show odds` cache grid. The key column takes whatever the other
// three leave of DISPLAY_COLS.
#define ODDSAPI_SHOW_AGE_W   12
#define ODDSAPI_SHOW_ITEMS_W 8
#define ODDSAPI_SHOW_STATE_W 10
#define ODDSAPI_SHOW_KEY_W   (DISPLAY_COLS - ODDSAPI_SHOW_AGE_W \
                              - ODDSAPI_SHOW_ITEMS_W - ODDSAPI_SHOW_STATE_W)

// The `odds` slate grid. The two team columns share whatever the rest
// leave of DISPLAY_COLS, and the money-line column is sized from the
// slate, since a three-way sport adds a draw price to every row.
#define ODDSAPI_SLATE_WHEN_W 11   // "Sun 12:00p" and one gutter
#define ODDSAPI_SLATE_SPR_W  10
#define ODDSAPI_SLATE_TOT_W  7
#define ODDSAPI_PX_W         4    // "-110"; a wider price widens its column

#define ODDSAPI_CELL_SZ      80
#define ODDSAPI_PX_SZ        24   // "+1000000 " and a book code
#define ODDSAPI_CODE_SZ      8    // four columns of a book's title
#define ODDSAPI_WHEN_SZ      48
#define ODDSAPI_SEARCH_MAX   16   // leagues one bare-team search fans out over
#define ODDSAPI_HITS_MAX     4    // the game carded, and the "also:" lines

// The team card's grid: one row per book, then consensus and best.
#define ODDSAPI_CARD_ROWS    (ODDSAPI_BOOKS_MAX + 2)
#define ODDSAPI_CARD_COLS    6
#define ODDSAPI_CARD_BOOK_W  14

typedef enum
{
  ODDSAPI_MODE_SLATE,       // a league's games, one row each
  ODDSAPI_MODE_TEAM         // one team's next game, one row per book
} oddsapi_mode_t;

// Parsed argument line (stack-only). `league` is NULL when the first
// word named none, and the whole line is then a team searched across
// plugin.oddsapi.search_leagues.
typedef struct
{
  oddsapi_mode_t          mode;
  const oddsapi_league_t *league;
  char                    query[ODDSAPI_TEAM_SZ];
} oddsapi_args_t;

typedef struct
{
  const oddsapi_league_t *league;
  oddsapi_event_t         ev;
} oddsapi_hit_t;

typedef struct oddsapi_req oddsapi_req_t;

// One league's share of a team search. Its events callback writes only
// this part and the last release reads every part, so the join needs no
// lock: the acq_rel decrement orders each part's writes before the merge.
typedef struct
{
  oddsapi_req_t          *req;
  const oddsapi_league_t *league;
  uint8_t                 nhits;
  oddsapi_hit_t           hits[ODDSAPI_HITS_MAX];
} oddsapi_part_t;

// Per-request heap closure, the rawg_req_t shape: a deep copy of the
// command context and message — the message carries reply_route, without
// which a botmanctl reply vanishes — so a completion on a curl worker can
// still answer. Every terminal path frees it exactly once. A team search
// is held by `pending` until its last league answers, then handed on to
// the one odds pull the winning league needs.
struct oddsapi_req
{
  cmd_ctx_t               ctx;
  method_msg_t            msg;
  oddsapi_mode_t          mode;
  const oddsapi_league_t *league;           // the slate's, then the card's
  char                    query[ODDSAPI_TEAM_SZ];
  uint32_t                pending;          // atomic; the submitter holds one
  uint8_t                 nparts;
  oddsapi_part_t          parts[ODDSAPI_SEARCH_MAX];
  uint8_t                 nhits;
  oddsapi_hit_t           hits[ODDSAPI_HITS_MAX];  // soonest first
};

// Short codes for the best-price row, keyed by bookmaker key. A book
// not listed shows the first four columns of its title.
typedef struct
{
  const char *key;
  const char *code;
} oddsapi_book_code_t;

static const oddsapi_book_code_t oddsapi_book_codes[] = {
  { "draftkings",     "DK"   },
  { "fanduel",        "FD"   },
  { "betmgm",         "MGM"  },
  { "williamhill_us", "CZR"  },
  { "fanatics",       "FAN"  },
  { "betrivers",      "BR"   },
  { "espnbet",        "TSB"  },   // ESPN BET, retitled theScore Bet
  { "hardrockbet",    "HR"   },
};

static void     oddsapi_cell(char *line, size_t cap, const char *text,
    int width, bool right);
static void     oddsapi_age(time_t then, time_t now, char *out, size_t cap);
static uint32_t oddsapi_csv_count(const char *csv);
static void     oddsapi_show_cmd(const cmd_ctx_t *ctx);

static void     oddsapi_as_of(const oddsapi_res_t *res, time_t now,
    char *out, size_t cap);
static void     oddsapi_when(time_t t, time_t now, bool full, char *out,
    size_t cap);
static void     oddsapi_px(int32_t px, const char *code, char *out,
    size_t cap);
static void     oddsapi_spread_pt(float pt, char *out, size_t cap);
static const char *oddsapi_book_code(const oddsapi_line_t *l, char *buf,
    size_t cap);
static const char *oddsapi_status_note(oddsapi_status_t st);

static void     oddsapi_render_slate(const cmd_ctx_t *ctx,
    const oddsapi_league_t *league, const oddsapi_res_t *res);
static void     oddsapi_slate_ml(const oddsapi_consensus_t *c, bool draw,
    int pw, char *out, size_t cap);
static void     oddsapi_render_game(const cmd_ctx_t *ctx,
    const oddsapi_league_t *league, const oddsapi_game_t *g,
    const oddsapi_res_t *res);
static uint8_t  oddsapi_card_row(const oddsapi_line_t *l,
    const char *const *codes, bool draw,
    char cells[ODDSAPI_CARD_COLS][ODDSAPI_CELL_SZ]);
static int      oddsapi_card_width(
    char cells[][ODDSAPI_CARD_COLS][ODDSAPI_CELL_SZ],
    char hdr[ODDSAPI_CARD_COLS][ODDSAPI_CELL_SZ], uint8_t nrows, uint8_t k,
    uint8_t ncols);
static void     oddsapi_card_drop_empty(
    char cells[][ODDSAPI_CARD_COLS][ODDSAPI_CELL_SZ],
    char hdr[ODDSAPI_CARD_COLS][ODDSAPI_CELL_SZ], uint8_t nrows,
    uint8_t first, uint8_t *ncols);
static void     oddsapi_render_title(const cmd_ctx_t *ctx,
    const oddsapi_league_t *league, const char *away, const char *home,
    time_t commence, const char *tail);
static void     oddsapi_render_also(const oddsapi_req_t *r);

static oddsapi_req_t *oddsapi_req_new(const cmd_ctx_t *ctx);
static bool     oddsapi_args_parse(const char *args, oddsapi_args_t *out);
static void     oddsapi_usage(const cmd_ctx_t *ctx);
static void     oddsapi_on_slate(const oddsapi_res_t *res, void *user);
static void     oddsapi_on_sports(const oddsapi_res_t *res, void *user);
static void     oddsapi_search(oddsapi_req_t *r, const oddsapi_sports_t *sp);
static void     oddsapi_on_events(const oddsapi_res_t *res, void *user);
static void     oddsapi_hit_insert(oddsapi_hit_t *hits, uint8_t *n,
    const oddsapi_hit_t *hit);
static void     oddsapi_search_release(oddsapi_req_t *r);
static void     oddsapi_on_card(const oddsapi_res_t *res, void *user);
static void     oddsapi_odds_cmd(const cmd_ctx_t *ctx);

#endif // ODDSAPI_CMD_INTERNAL

#endif // BM_ODDSAPI_CMD_H
