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

// A league the commands know by name. `alt` is a second alias or NULL;
// `outright_key` is NULL when the league has no futures market.
typedef struct
{
  const char *alias;                        // "ncaaf"
  const char *alt;                          // "cfb"
  const char *sport_key;                    // "americanfootball_ncaaf"
  const char *outright_key;
  const char *label;                        // "NCAAF"
} oddsapi_league_t;

#endif // BM_ODDSAPI_API_H
