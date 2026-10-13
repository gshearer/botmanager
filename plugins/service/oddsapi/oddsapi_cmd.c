// botmanager — MIT
// The oddsapi plugin's command surface. `show odds` reads only local
// state — the quota as the API last described it, and the cache — so it
// answers synchronously and costs nothing. `odds` renders a league's
// slate or one team's card from the cached league pull, so a card costs
// nothing past the slate it rides on.
#define ODDSAPI_INTERNAL
#define ODDSAPI_CMD_INTERNAL
#include "oddsapi.h"
#include "oddsapi_cmd.h"

// ----------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------

// Append one padded cell. Color markers count zero columns, so a cell
// may carry them.
static void
oddsapi_cell(char *line, size_t cap, const char *text, int width, bool right)
{
  char cell[ODDSAPI_CMD_REPLY_SZ];

  strlcpy(cell, text, sizeof(cell));

  if(right)
    display_align_right(cell, sizeof(cell), width);

  else
    display_align_left(cell, sizeof(cell), width);

  display_cat(line, cap, cell);
}

static void
oddsapi_age(time_t then, time_t now, char *out, size_t cap)
{
  if(then == 0)
    strlcpy(out, "—", cap);

  else
    util_fmt_duration(now - then, out, cap);
}

// Fields in a comma-separated list, empty ones not counted.
static uint32_t
oddsapi_csv_count(const char *csv)
{
  uint32_t n    = 0;
  bool     word = false;

  for(; *csv != '\0'; csv++)
  {
    if(*csv == ',')
      word = false;

    else if(*csv != ' ' && !word)
    {
      word = true;
      n++;
    }
  }

  return(n);
}

// ----------------------------------------------------------------------
// show odds
// ----------------------------------------------------------------------

static void
oddsapi_show_cmd(const cmd_ctx_t *ctx)
{
  oddsapi_quota_t     q;
  oddsapi_cache_row_t rows[ODDSAPI_CACHE_SZ];
  uint32_t            n     = oddsapi_cache_rows(rows, ODDSAPI_CACHE_SZ);
  const char         *books = kv_get_str("plugin.oddsapi.bookmakers");
  time_t              now   = time(NULL);
  char                line[ODDSAPI_CMD_REPLY_SZ];
  char                rule[DISPLAY_RULE_SZ(DISPLAY_COLS)];
  char                age[UTIL_DURATION_SZ];
  char                backoff[UTIL_DURATION_SZ];
  char                books_txt[24];

  oddsapi_quota_get(&q);

  if(q.remaining < 0)
    snprintf(line, sizeof(line), CLR_BOLD "🎲 ODDS API" CLR_RESET
        "   credits " CLR_GRAY "— (no call yet)" CLR_RESET);

  else
  {
    oddsapi_age(q.seen, now, age, sizeof(age));
    snprintf(line, sizeof(line), CLR_BOLD "🎲 ODDS API" CLR_RESET
        "   credits " CLR_BOLD "%d" CLR_RESET " left · %d used · "
        "last call %d · seen %s ago", q.remaining, q.used, q.last, age);
  }

  cmd_reply(ctx, line);

  if(q.backoff_left > 0)
    util_fmt_duration((time_t)q.backoff_left, backoff, sizeof(backoff));

  else
    strlcpy(backoff, "none", sizeof(backoff));

  if(books != NULL && books[0] != '\0')
    snprintf(books_txt, sizeof(books_txt), "%u", oddsapi_csv_count(books));

  else
    strlcpy(books_txt, "us region", sizeof(books_txt));

  snprintf(line, sizeof(line), "reserve %u · backoff %s · books %s",
      (uint32_t)kv_get_uint("plugin.oddsapi.quota_reserve"), backoff,
      books_txt);
  cmd_reply(ctx, line);

  if(n == 0)
  {
    cmd_reply(ctx, CLR_GRAY "cache empty" CLR_RESET);
    return;
  }

  line[0] = '\0';
  oddsapi_cell(line, sizeof(line), CLR_GRAY "cache", ODDSAPI_SHOW_KEY_W,
      false);
  oddsapi_cell(line, sizeof(line), "age", ODDSAPI_SHOW_AGE_W, true);
  oddsapi_cell(line, sizeof(line), "items", ODDSAPI_SHOW_ITEMS_W, true);
  display_cat(line, sizeof(line), "  state" CLR_RESET);
  cmd_reply(ctx, line);

  rule[0] = '\0';
  display_rule(rule, sizeof(rule), DISPLAY_COLS);
  cmd_reply(ctx, rule);

  for(uint32_t i = 0; i < n; i++)
  {
    char key[ODDSAPI_CACHE_KEY_SZ + 4];
    char items[16];

    // Budget one column for the mark and one for the gutter.
    display_fit(rows[i].key, ODDSAPI_SHOW_KEY_W - 2, key, sizeof(key), "…");
    oddsapi_age(rows[i].fetched, now, age, sizeof(age));
    snprintf(items, sizeof(items), "%u", rows[i].items);

    line[0] = '\0';
    oddsapi_cell(line, sizeof(line), key, ODDSAPI_SHOW_KEY_W, false);
    oddsapi_cell(line, sizeof(line), age, ODDSAPI_SHOW_AGE_W, true);
    oddsapi_cell(line, sizeof(line), rows[i].fetched != 0 ? items : "—",
        ODDSAPI_SHOW_ITEMS_W, true);

    if(rows[i].inflight)
      display_cat(line, sizeof(line), "  " CLR_YELLOW "fetching" CLR_RESET);

    cmd_reply(ctx, line);
  }
}

// ----------------------------------------------------------------------
// odds — formatting
// ----------------------------------------------------------------------

// "as of 4m ago" — or, when the copy was served past its TTL, the same
// in yellow with the provider's reason ("quota low", "rate limited").
static void
oddsapi_as_of(const oddsapi_res_t *res, time_t now, char *out, size_t cap)
{
  time_t d = now > res->fetched ? now - res->fetched : 0;
  char   ago[24];

  if(d < 60)
    snprintf(ago, sizeof(ago), "%lds", (long)d);

  else if(d < 3600)
    snprintf(ago, sizeof(ago), "%ldm", (long)(d / 60));

  else if(d < 86400)
    snprintf(ago, sizeof(ago), "%ldh", (long)(d / 3600));

  else
    snprintf(ago, sizeof(ago), "%ldd", (long)(d / 86400));

  if(res->stale)
    snprintf(out, cap, CLR_YELLOW "as of %s ago · %s" CLR_RESET, ago,
        res->message);

  else
    snprintf(out, cap, "as of %s ago", ago);
}

// A start time in the host zone; the daemon never touches TZ. The short
// form is a slate cell — "Sun 1:00p", "Oct 18" past six days out, a red
// LIVE once it has begun — and the full form a card's title.
static void
oddsapi_when(time_t t, time_t now, bool full, char *out, size_t cap)
{
  struct tm tm;
  char      day[16];
  char      zone[16];
  int       hour;

  if(!full && t <= now)
  {
    strlcpy(out, CLR_RED "LIVE" CLR_RESET, cap);
    return;
  }

  if(localtime_r(&t, &tm) == NULL)
  {
    strlcpy(out, "?", cap);
    return;
  }

  // strftime names the day, month and zone; the unpadded numbers are
  // ours, since its '-' flag is a GNU extension.
  hour = tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12;

  if(full)
  {
    strftime(day, sizeof(day), "%a %b", &tm);
    strftime(zone, sizeof(zone), "%Z", &tm);
    snprintf(out, cap, "%s %d %d:%02d%s %s", day, tm.tm_mday, hour,
        tm.tm_min, tm.tm_hour < 12 ? "am" : "pm", zone);
  }

  else if(t - now > 6 * 86400)
  {
    strftime(day, sizeof(day), "%b", &tm);
    snprintf(out, cap, "%s %d", day, tm.tm_mday);
  }

  else
  {
    strftime(day, sizeof(day), "%a", &tm);
    snprintf(out, cap, "%s %d:%02d%s", day, hour, tm.tm_min,
        tm.tm_hour < 12 ? "a" : "p");
  }
}

// An American price, "—" when absent, with the quoting book's code after
// it when there is one.
static void
oddsapi_px(int32_t px, const char *code, char *out, size_t cap)
{
  if(px == 0)
    strlcpy(out, "—", cap);

  else if(code != NULL)
    snprintf(out, cap, "%+d %s", (int)px, code);

  else
    snprintf(out, cap, "%+d", (int)px);
}

static void
oddsapi_spread_pt(float pt, char *out, size_t cap)
{
  if(fabsf(pt) < ODDSAPI_PT_EPS)
    strlcpy(out, "PK", cap);

  else
    snprintf(out, cap, "%+.1f", (double)pt);
}

static const char *
oddsapi_book_code(const char *key, const char *title, char *buf, size_t cap)
{
  for(size_t i = 0;
      i < sizeof(oddsapi_book_codes) / sizeof(oddsapi_book_codes[0]); i++)
  {
    if(strcmp(key, oddsapi_book_codes[i].key) == 0)
      return(oddsapi_book_codes[i].code);
  }

  display_fit(title, 4, buf, cap, NULL);
  return(buf);
}

// Every failure obliges the same thing — say why and stop — so the
// reason is phrased for whoever typed the command, not for the log.
static const char *
oddsapi_status_note(oddsapi_status_t st)
{
  switch(st)
  {
    case ODDSAPI_OK:           return("");
    case ODDSAPI_NOT_FOUND:    return("The Odds API doesn't carry that league right now.");
    case ODDSAPI_RATE_LIMITED: return(CLR_ORANGE "The Odds API is rate-limiting us — try again in a minute." CLR_RESET);
    case ODDSAPI_AUTH:         return("The Odds API refused our key (check plugin.oddsapi.creds.apikey).");
    case ODDSAPI_QUOTA:        return(CLR_YELLOW "This month's Odds API credits are spent, or nearly — nothing cached to show." CLR_RESET);
    case ODDSAPI_TRANSPORT:    return("The Odds API is unreachable right now.");
    case ODDSAPI_PARSE:        return("The Odds API sent a reply we couldn't read.");
  }

  return("The Odds API is unavailable right now.");
}

// ----------------------------------------------------------------------
// odds <league> — the slate
// ----------------------------------------------------------------------

// "+115 / -135", or away / draw / home in a three-way sport — each price
// right-aligned to `pw` so the slashes stack down the column.
static void
oddsapi_slate_ml(const oddsapi_consensus_t *c, bool draw, int pw, char *out,
    size_t cap)
{
  int32_t px[3];
  uint8_t n = 0;

  px[n++] = c->px[ODDSAPI_COL_ML_AWAY];

  if(draw)
    px[n++] = c->px[ODDSAPI_COL_ML_DRAW];

  px[n++] = c->px[ODDSAPI_COL_ML_HOME];
  out[0]  = '\0';

  for(uint8_t i = 0; i < n; i++)
  {
    char cell[ODDSAPI_CELL_SZ];

    oddsapi_px(px[i], NULL, cell, sizeof(cell));
    display_align_right(cell, sizeof(cell), pw);

    if(i > 0)
      display_cat(out, cap, " / ");

    display_cat(out, cap, cell);
  }
}

static void
oddsapi_render_slate(const cmd_ctx_t *ctx, const oddsapi_league_t *league,
    const oddsapi_res_t *res)
{
  const oddsapi_slate_t *s      = res->slate;
  uint32_t               rows   = (uint32_t)kv_get_uint("plugin.oddsapi.rows");
  uint32_t               shown  = s->ngames < rows ? s->ngames : rows;
  time_t                 now    = time(NULL);
  uint8_t                books  = 0;
  bool                   draw   = false;
  int                    pw     = ODDSAPI_PX_W;
  int                    spr_w  = 0;
  int                    tot_w  = 0;
  int                    ml_w, away_w, home_w, names;
  char                   line[ODDSAPI_CMD_REPLY_SZ];
  char                   as_of[ODDSAPI_CELL_SZ * 3];

  if(s->ngames == 0)
  {
    snprintf(line, sizeof(line), "No %s lines posted right now.",
        league->label);
    cmd_reply(ctx, line);
    return;
  }

  // The header counts the books that actually quoted, never the KV list:
  // a configured book that does not carry the league is simply absent.
  for(uint32_t i = 0; i < s->ngames; i++)
  {
    if(s->games[i].nbooks > books)
      books = s->games[i].nbooks;
  }

  // The rows it will show size the money line, and decide whether a
  // spread or total column says anything at all (`include/display.h`).
  for(uint32_t i = 0; i < shown; i++)
  {
    oddsapi_consensus_t c;

    oddsapi_consensus(&s->games[i], &c);

    if(c.px[ODDSAPI_COL_ML_DRAW] != 0)
      draw = true;

    if(c.px[ODDSAPI_COL_SPR_HOME] != 0 || c.px[ODDSAPI_COL_SPR_AWAY] != 0)
      spr_w = ODDSAPI_SLATE_SPR_W;

    if(c.px[ODDSAPI_COL_OVER] != 0 || c.px[ODDSAPI_COL_UNDER] != 0)
      tot_w = ODDSAPI_SLATE_TOT_W;

    for(int col = ODDSAPI_COL_ML_AWAY; col <= ODDSAPI_COL_ML_DRAW; col++)
    {
      char cell[ODDSAPI_CELL_SZ];
      int  w;

      oddsapi_px(c.px[col], NULL, cell, sizeof(cell));
      w = (int)display_vis_len(cell);

      if(w > pw)
        pw = w;
    }
  }

  // Two gutters lead the money line; the team names pay for the rest.
  ml_w   = draw ? 3 * pw + 6 + 2 : 2 * pw + 3 + 2;
  names  = DISPLAY_COLS - ODDSAPI_SLATE_WHEN_W - ml_w - spr_w - tot_w;
  away_w = names / 2;
  home_w = names - away_w;

  oddsapi_as_of(res, now, as_of, sizeof(as_of));
  snprintf(line, sizeof(line), CLR_BOLD "%s" CLR_RESET
      " · consensus of %u book%s · %s", league->label, books,
      books == 1 ? "" : "s", as_of);
  cmd_reply(ctx, line);

  line[0] = '\0';
  oddsapi_cell(line, sizeof(line), CLR_GRAY "when", ODDSAPI_SLATE_WHEN_W,
      false);
  oddsapi_cell(line, sizeof(line), "away", away_w, false);
  oddsapi_cell(line, sizeof(line), "  home", home_w, false);
  oddsapi_cell(line, sizeof(line), draw ? "ML away/draw/home"
      : "ML away/home", ml_w, true);
  if(spr_w > 0)
    oddsapi_cell(line, sizeof(line), "home spr", spr_w, true);

  if(tot_w > 0)
    oddsapi_cell(line, sizeof(line), "total", tot_w, true);

  display_cat(line, sizeof(line), CLR_RESET);
  cmd_reply(ctx, line);

  for(uint32_t i = 0; i < shown; i++)
  {
    const oddsapi_game_t *g = &s->games[i];
    oddsapi_consensus_t   c;
    char                  when[ODDSAPI_WHEN_SZ];
    char                  away[ODDSAPI_TEAM_SZ + 8];
    char                  home[ODDSAPI_TEAM_SZ + 8];
    char                  ml[ODDSAPI_CELL_SZ];
    char                  spr[ODDSAPI_CELL_SZ];
    char                  tot[ODDSAPI_CELL_SZ];

    oddsapi_consensus(g, &c);
    oddsapi_when(g->commence, now, false, when, sizeof(when));

    // Budget one column for the mark and one for the gutter; the home
    // cell also carries its "@ ".
    display_fit(g->away, away_w - 2, away, sizeof(away), "…");
    strlcpy(home, "@ ", sizeof(home));
    display_fit(g->home, home_w - 4, home + 2, sizeof(home) - 2, "…");
    oddsapi_slate_ml(&c, draw, pw, ml, sizeof(ml));

    if(c.px[ODDSAPI_COL_SPR_HOME] != 0 || c.px[ODDSAPI_COL_SPR_AWAY] != 0)
      oddsapi_spread_pt(c.spr_home_pt, spr, sizeof(spr));

    else
      strlcpy(spr, "—", sizeof(spr));

    if(c.px[ODDSAPI_COL_OVER] != 0 || c.px[ODDSAPI_COL_UNDER] != 0)
      snprintf(tot, sizeof(tot), "%.1f", (double)c.tot_pt);

    else
      strlcpy(tot, "—", sizeof(tot));

    line[0] = '\0';
    oddsapi_cell(line, sizeof(line), when, ODDSAPI_SLATE_WHEN_W, false);
    oddsapi_cell(line, sizeof(line), away, away_w, false);
    oddsapi_cell(line, sizeof(line), home, home_w, false);
    oddsapi_cell(line, sizeof(line), ml, ml_w, true);
    if(spr_w > 0)
      oddsapi_cell(line, sizeof(line), spr, spr_w, true);

    if(tot_w > 0)
      oddsapi_cell(line, sizeof(line), tot, tot_w, true);

    cmd_reply(ctx, line);
  }

  if(s->ngames > shown)
  {
    snprintf(line, sizeof(line), CLR_GRAY "+%u more — odds %s <team>"
        CLR_RESET, s->ngames - shown, league->alias);
    cmd_reply(ctx, line);
  }
}

// ----------------------------------------------------------------------
// odds [<league>] <team> — the card
// ----------------------------------------------------------------------

// One row's cells after the label: away, [draw,] home, spread, total —
// the slate's money-line order.
// `codes`, when given, names the book behind each column's price — the
// best row. Returns the row's cell count, label included.
static uint8_t
oddsapi_card_row(const oddsapi_line_t *l, const char *const *codes,
    bool draw, char cells[ODDSAPI_CARD_COLS][ODDSAPI_CELL_SZ])
{
  uint8_t n = 1;
  char    pt[16];
  char    a[ODDSAPI_PX_SZ];
  char    b[ODDSAPI_PX_SZ];

  oddsapi_px(l->ml_away, codes != NULL ? codes[ODDSAPI_COL_ML_AWAY] : NULL,
      cells[n++], ODDSAPI_CELL_SZ);

  if(draw)
    oddsapi_px(l->ml_draw, codes != NULL ? codes[ODDSAPI_COL_ML_DRAW] : NULL,
        cells[n++], ODDSAPI_CELL_SZ);

  oddsapi_px(l->ml_home, codes != NULL ? codes[ODDSAPI_COL_ML_HOME] : NULL,
      cells[n++], ODDSAPI_CELL_SZ);

  if(l->spr_home_px == 0 && l->spr_away_px == 0)
    strlcpy(cells[n++], "—", ODDSAPI_CELL_SZ);

  else
  {
    oddsapi_spread_pt(l->spr_home_pt, pt, sizeof(pt));
    oddsapi_px(l->spr_home_px,
        codes != NULL ? codes[ODDSAPI_COL_SPR_HOME] : NULL, a, sizeof(a));
    oddsapi_px(l->spr_away_px,
        codes != NULL ? codes[ODDSAPI_COL_SPR_AWAY] : NULL, b, sizeof(b));
    snprintf(cells[n++], ODDSAPI_CELL_SZ, "%s %s / %s", pt, a, b);
  }

  if(l->over_px == 0 && l->under_px == 0)
    strlcpy(cells[n++], "—", ODDSAPI_CELL_SZ);

  else
  {
    oddsapi_px(l->over_px, codes != NULL ? codes[ODDSAPI_COL_OVER] : NULL,
        a, sizeof(a));
    oddsapi_px(l->under_px, codes != NULL ? codes[ODDSAPI_COL_UNDER] : NULL,
        b, sizeof(b));
    snprintf(cells[n++], ODDSAPI_CELL_SZ, "%.1f o%s u%s",
        (double)l->tot_pt, a, b);
  }

  return(n);
}

// "Kansas City Chiefs @ Buffalo Bills · NFL · Sun Oct 11 1:00pm EDT",
// LIVE once begun, and `tail` (the card's freshness) when given.
static void
oddsapi_render_title(const cmd_ctx_t *ctx, const oddsapi_league_t *league,
    const char *away, const char *home, time_t commence, const char *tail)
{
  time_t now = time(NULL);
  char   when[ODDSAPI_WHEN_SZ];
  char   line[ODDSAPI_CMD_REPLY_SZ];

  oddsapi_when(commence, now, true, when, sizeof(when));
  snprintf(line, sizeof(line), CLR_BOLD "%s @ %s" CLR_RESET " · %s · %s%s%s%s",
      away, home, league->label, when,
      commence <= now ? " · " CLR_RED "LIVE" CLR_RESET : "",
      tail != NULL ? " · " : "", tail != NULL ? tail : "");
  cmd_reply(ctx, line);
}

// Column `k`'s width over the header and rows [0, nrows), with the two
// gutters every column but the last carries.
static int
oddsapi_card_width(char cells[][ODDSAPI_CARD_COLS][ODDSAPI_CELL_SZ],
    char hdr[ODDSAPI_CARD_COLS][ODDSAPI_CELL_SZ], uint8_t nrows, uint8_t k,
    uint8_t ncols)
{
  int w = (int)display_vis_len(hdr[k]);

  for(uint8_t r = 0; r < nrows; r++)
  {
    int v = (int)display_vis_len(cells[r][k]);

    if(v > w)
      w = v;
  }

  return(k + 1 < ncols ? w + 2 : w);
}

// A spread or total column no book quotes says nothing on any row —
// soccer's spread, as often as not — so it leaves the grid
// (`include/display.h`). Columns [first, ncols) are the candidates.
static void
oddsapi_card_drop_empty(char cells[][ODDSAPI_CARD_COLS][ODDSAPI_CELL_SZ],
    char hdr[ODDSAPI_CARD_COLS][ODDSAPI_CELL_SZ], uint8_t nrows,
    uint8_t first, uint8_t *ncols)
{
  for(uint8_t k = *ncols; k-- > first;)
  {
    bool empty = true;

    for(uint8_t r = 0; empty && r < nrows; r++)
      empty = strcmp(cells[r][k], "—") == 0;

    if(!empty)
      continue;

    for(uint8_t r = 0; r < nrows; r++)
      memmove(cells[r][k], cells[r][k + 1],
          (size_t)(*ncols - 1 - k) * ODDSAPI_CELL_SZ);

    memmove(hdr[k], hdr[k + 1], (size_t)(*ncols - 1 - k) * ODDSAPI_CELL_SZ);
    (*ncols)--;
  }
}

// The card: a row per book, then consensus and best. Columns are sized
// from their widest cell; when the best row — the widest, since it names
// a book beside every price — cannot fit DISPLAY_COLS, it takes two
// lines rather than squeeze the grid.
static void
oddsapi_render_game(const cmd_ctx_t *ctx, const oddsapi_league_t *league,
    const oddsapi_game_t *g, const oddsapi_res_t *res)
{
  static const char *const head[ODDSAPI_CARD_COLS] = {
    "book", "away", "draw", "home", "spread (home)", "total"
  };

  oddsapi_consensus_t c;
  oddsapi_best_t      best[ODDSAPI_COL_COUNT];
  oddsapi_line_t      cl, bl;
  const char         *codes[ODDSAPI_COL_COUNT];
  char                code_buf[ODDSAPI_COL_COUNT][ODDSAPI_CODE_SZ];
  char                cells[ODDSAPI_CARD_ROWS][ODDSAPI_CARD_COLS][ODDSAPI_CELL_SZ];
  char                hdr[ODDSAPI_CARD_COLS][ODDSAPI_CELL_SZ];
  char                line[ODDSAPI_CMD_REPLY_SZ];
  char                as_of[ODDSAPI_CELL_SZ * 3];
  int                 w[ODDSAPI_CARD_COLS] = { 0 };
  int                 total = 0;
  uint8_t             nrows = 0;
  uint8_t             ncols = 0;
  uint8_t             lines;                // first spread/total column
  uint8_t             best_row;
  bool                draw  = false;
  bool                split;

  oddsapi_consensus(g, &c);
  oddsapi_best(g, &c, best);
  draw = c.px[ODDSAPI_COL_ML_DRAW] != 0;

  oddsapi_as_of(res, time(NULL), as_of, sizeof(as_of));
  oddsapi_render_title(ctx, league, g->away, g->home, g->commence, as_of);

  // The header row is the grid's first row, so it sizes columns too.
  for(uint8_t i = 0, k = 0; i < ODDSAPI_CARD_COLS; i++)
  {
    if(i == 2 && !draw)
      continue;

    strlcpy(hdr[k++], head[i], ODDSAPI_CELL_SZ);
    ncols = k;
  }

  for(uint8_t b = 0; b < g->nbooks; b++)
  {
    display_fit(g->books[b].title, ODDSAPI_CARD_BOOK_W - 2, cells[nrows][0],
        ODDSAPI_CELL_SZ, "…");
    oddsapi_card_row(&g->books[b], NULL, draw, cells[nrows++]);
  }

  // Consensus and best are rows of the same shape as a book's, so one
  // formatter draws all three. Both price the spread and total at the
  // modal point.
  memset(&cl, 0, sizeof(cl));
  cl.ml_away     = c.px[ODDSAPI_COL_ML_AWAY];
  cl.ml_home     = c.px[ODDSAPI_COL_ML_HOME];
  cl.ml_draw     = c.px[ODDSAPI_COL_ML_DRAW];
  cl.spr_home_pt = c.spr_home_pt;
  cl.spr_home_px = c.px[ODDSAPI_COL_SPR_HOME];
  cl.spr_away_px = c.px[ODDSAPI_COL_SPR_AWAY];
  cl.tot_pt      = c.tot_pt;
  cl.over_px     = c.px[ODDSAPI_COL_OVER];
  cl.under_px    = c.px[ODDSAPI_COL_UNDER];
  strlcpy(cells[nrows][0], "consensus", ODDSAPI_CELL_SZ);
  oddsapi_card_row(&cl, NULL, draw, cells[nrows++]);

  bl = cl;
  bl.ml_away     = best[ODDSAPI_COL_ML_AWAY].px;
  bl.ml_home     = best[ODDSAPI_COL_ML_HOME].px;
  bl.ml_draw     = best[ODDSAPI_COL_ML_DRAW].px;
  bl.spr_home_px = best[ODDSAPI_COL_SPR_HOME].px;
  bl.spr_away_px = best[ODDSAPI_COL_SPR_AWAY].px;
  bl.over_px     = best[ODDSAPI_COL_OVER].px;
  bl.under_px    = best[ODDSAPI_COL_UNDER].px;

  for(int col = 0; col < ODDSAPI_COL_COUNT; col++)
    codes[col] = best[col].px != 0
        ? oddsapi_book_code(g->books[best[col].book].key,
            g->books[best[col].book].title, code_buf[col],
            sizeof(code_buf[col]))
        : NULL;

  best_row = nrows;
  strlcpy(cells[nrows][0], "best", ODDSAPI_CELL_SZ);
  oddsapi_card_row(&bl, codes, draw, cells[nrows++]);

  lines = draw ? 4 : 3;
  oddsapi_card_drop_empty(cells, hdr, nrows, lines, &ncols);

  for(uint8_t k = 0; k < ncols; k++)
  {
    w[k]   = oddsapi_card_width(cells, hdr, nrows, k, ncols);
    total += w[k];
  }

  // Too wide: the spread and total columns are re-sized without the
  // best row, whose prices there move to a second line.
  split = total > DISPLAY_COLS;

  for(uint8_t k = lines; split && k < ncols; k++)
    w[k] = oddsapi_card_width(cells, hdr, best_row, k, ncols);

  line[0] = '\0';
  display_cat(line, sizeof(line), CLR_GRAY);

  for(uint8_t k = 0; k < ncols; k++)
    oddsapi_cell(line, sizeof(line), hdr[k], k + 1 < ncols ? w[k] : 0, false);

  display_cat(line, sizeof(line), CLR_RESET);
  cmd_reply(ctx, line);

  for(uint8_t r = 0; r < nrows; r++)
  {
    uint8_t last = r == best_row && split ? lines : ncols;

    // A book's title is bold and its prices plain; consensus is bold
    // throughout, best green.
    line[0] = '\0';
    display_cat(line, sizeof(line), r == best_row ? CLR_GREEN : CLR_BOLD);

    for(uint8_t k = 0; k < last; k++)
    {
      oddsapi_cell(line, sizeof(line), cells[r][k],
          k + 1 < ncols ? w[k] : 0, false);

      if(k == 0 && r + 1 < best_row)
        display_cat(line, sizeof(line), CLR_RESET);
    }

    display_cat(line, sizeof(line), CLR_RESET);
    cmd_reply(ctx, line);
  }

  // The second line names its columns: it cannot stand under them.
  if(split)
  {
    line[0] = '\0';
    oddsapi_cell(line, sizeof(line), "", w[0], false);
    display_cat(line, sizeof(line), CLR_GREEN);

    for(uint8_t k = lines; k < ncols; k++)
    {
      display_cat(line, sizeof(line), k > lines ? "   " : "");
      display_cat(line, sizeof(line), hdr[k]);
      display_cat(line, sizeof(line), " ");
      display_cat(line, sizeof(line), cells[best_row][k]);
    }

    display_cat(line, sizeof(line), CLR_RESET);
    cmd_reply(ctx, line);
  }
}

// One gray line per other game the query matched, under the card.
static void
oddsapi_render_also(const oddsapi_req_t *r)
{
  time_t now = time(NULL);

  for(uint8_t i = 1; i < r->nhits; i++)
  {
    const oddsapi_hit_t *h = &r->hits[i];
    char                 when[ODDSAPI_WHEN_SZ];
    char                 line[ODDSAPI_CMD_REPLY_SZ];

    oddsapi_when(h->ev.commence, now, false, when, sizeof(when));
    snprintf(line, sizeof(line), CLR_GRAY "also: %s %s @ %s " CLR_RESET "%s",
        h->league->label, h->ev.away, h->ev.home, when);
    cmd_reply(&r->ctx, line);
  }
}

// ----------------------------------------------------------------------
// odds <league> --futures
// ----------------------------------------------------------------------

// `implied` is 100 over the consensus decimal price, vig and all — the
// sum down the column is past 100%, and help_long says so.
static void
oddsapi_render_futures(const cmd_ctx_t *ctx, const oddsapi_league_t *league,
    const oddsapi_res_t *res, const oddsapi_futures_t *f)
{
  uint32_t rows  = (uint32_t)kv_get_uint("plugin.oddsapi.rows");
  uint32_t shown = f->n < rows ? f->n : rows;
  char     line[ODDSAPI_CMD_REPLY_SZ];
  char     as_of[ODDSAPI_CELL_SZ * 3];

  oddsapi_as_of(res, time(NULL), as_of, sizeof(as_of));
  snprintf(line, sizeof(line), CLR_BOLD "%s" CLR_RESET
      " · consensus of %u book%s · %s",
      f->sport_title[0] != '\0' ? f->sport_title : league->label, f->nbooks,
      f->nbooks == 1 ? "" : "s", as_of);
  cmd_reply(ctx, line);

  line[0] = '\0';
  oddsapi_cell(line, sizeof(line), CLR_GRAY " #", ODDSAPI_FUT_RANK_W, false);
  oddsapi_cell(line, sizeof(line), "team", ODDSAPI_FUT_TEAM_W, false);
  oddsapi_cell(line, sizeof(line), "consensus", ODDSAPI_FUT_PX_W, false);
  oddsapi_cell(line, sizeof(line), "implied", ODDSAPI_FUT_PCT_W, false);
  display_cat(line, sizeof(line), "best" CLR_RESET);
  cmd_reply(ctx, line);

  for(uint32_t i = 0; i < shown; i++)
  {
    const oddsapi_contender_t *c = &f->contenders[i];
    oddsapi_best_t             b;
    int32_t                    med;
    char                       rank[16];
    char                       team[ODDSAPI_TEAM_SZ + 8];
    char                       px[ODDSAPI_PX_SZ];
    char                       pct[16];
    char                       best[ODDSAPI_PX_SZ];
    char                       code[ODDSAPI_CODE_SZ];

    oddsapi_contender_consensus(c, &med, &b);
    snprintf(rank, sizeof(rank), "%2u", i + 1);
    display_fit(c->name, ODDSAPI_FUT_TEAM_W - 2, team, sizeof(team), "…");
    oddsapi_px(med, NULL, px, sizeof(px));

    if(med != 0)
      snprintf(pct, sizeof(pct), "%.1f%%", 100.0 / oddsapi_am_to_dec(med));

    else
      strlcpy(pct, "—", sizeof(pct));

    oddsapi_px(b.px, b.px != 0 ? oddsapi_book_code(f->book_key[b.book],
        f->book_title[b.book], code, sizeof(code)) : NULL, best,
        sizeof(best));

    line[0] = '\0';
    oddsapi_cell(line, sizeof(line), rank, ODDSAPI_FUT_RANK_W, false);
    oddsapi_cell(line, sizeof(line), team, ODDSAPI_FUT_TEAM_W, false);
    oddsapi_cell(line, sizeof(line), px, ODDSAPI_FUT_PX_W, false);
    oddsapi_cell(line, sizeof(line), pct, ODDSAPI_FUT_PCT_W, false);
    display_cat(line, sizeof(line), best);
    cmd_reply(ctx, line);
  }

  if(f->n > shown)
  {
    snprintf(line, sizeof(line), CLR_GRAY "+%u more" CLR_RESET,
        f->n - shown);
    cmd_reply(ctx, line);
  }
}

// ----------------------------------------------------------------------
// scores
// ----------------------------------------------------------------------

// A score arrives as a string; only a whole number can name a winner.
static bool
oddsapi_score_int(const char *s, long *out)
{
  char *end;

  errno = 0;
  *out  = strtol(s, &end, 10);
  return(end != s && *end == '\0' && errno == 0);
}

// The API carries no clock or period, so LIVE is all a live row can say.
static void
oddsapi_score_row(const cmd_ctx_t *ctx, const oddsapi_score_t *s,
    time_t now)
{
  long a       = 0;
  long h       = 0;
  bool decided = s->completed && s->has_scores
                 && oddsapi_score_int(s->away_score, &a)
                 && oddsapi_score_int(s->home_score, &h) && a != h;
  char when[ODDSAPI_WHEN_SZ];
  char away[ODDSAPI_TEAM_SZ + 8];
  char home[ODDSAPI_TEAM_SZ + 8];
  char as[ODDSAPI_CELL_SZ];
  char hs[ODDSAPI_CELL_SZ];
  char line[ODDSAPI_CMD_REPLY_SZ];

  if(!s->completed && s->has_scores)
    strlcpy(when, CLR_RED "LIVE" CLR_RESET, sizeof(when));

  else if(s->completed)
    strlcpy(when, CLR_GRAY "F" CLR_RESET, sizeof(when));

  else
    oddsapi_when(s->commence, now, false, when, sizeof(when));

  // The home cell carries its gutter and "@ "; one more column is left
  // for the mark, and the home score's padding gutters the far side.
  display_fit(s->away, ODDSAPI_SCORE_AWAY_W - 2, away, sizeof(away), "…");
  strlcpy(home, "  @ ", sizeof(home));
  display_fit(s->home, ODDSAPI_SCORE_HOME_W - 5, home + 4, sizeof(home) - 4,
      "…");

  // A final's winning score is bold; a draw names nobody.
  snprintf(as, sizeof(as), "%s%s%s", decided && a > h ? CLR_BOLD : "",
      s->has_scores ? s->away_score : "", decided && a > h ? CLR_RESET : "");
  snprintf(hs, sizeof(hs), "%s%s%s", decided && h > a ? CLR_BOLD : "",
      s->has_scores ? s->home_score : "", decided && h > a ? CLR_RESET : "");

  line[0] = '\0';
  oddsapi_cell(line, sizeof(line), when, ODDSAPI_SCORE_WHEN_W, false);
  oddsapi_cell(line, sizeof(line), away, ODDSAPI_SCORE_AWAY_W, false);
  oddsapi_cell(line, sizeof(line), as, ODDSAPI_SCORE_PTS_W, true);
  oddsapi_cell(line, sizeof(line), home, ODDSAPI_SCORE_HOME_W, false);
  oddsapi_cell(line, sizeof(line), hs, ODDSAPI_SCORE_PTS_W, true);
  cmd_reply(ctx, line);
}

// "NFL scores · as of 2m ago", the team's query between when there is
// one, and no age when the pull failed.
static void
oddsapi_scores_head(const cmd_ctx_t *ctx, const oddsapi_league_t *league,
    const char *query, const oddsapi_res_t *res)
{
  char line[ODDSAPI_CMD_REPLY_SZ];
  char as_of[ODDSAPI_CELL_SZ * 3];

  as_of[0] = '\0';

  if(res->status == ODDSAPI_OK)
    oddsapi_as_of(res, time(NULL), as_of, sizeof(as_of));

  snprintf(line, sizeof(line), CLR_BOLD "%s scores" CLR_RESET "%s%s%s%s",
      league->label, query != NULL ? " · " : "", query != NULL ? query : "",
      as_of[0] != '\0' ? " · " : "", as_of);
  cmd_reply(ctx, line);
}

// Live first, then finals newest first, then what is still to come —
// three passes over a list already in start order, so nothing is sorted.
static void
oddsapi_render_scores(const cmd_ctx_t *ctx, const oddsapi_league_t *league,
    const oddsapi_res_t *res)
{
  const oddsapi_scores_t *sc    = res->scores;
  uint32_t                rows  = (uint32_t)kv_get_uint("plugin.oddsapi.rows");
  uint32_t                shown = 0;
  time_t                  now   = time(NULL);
  char                    line[ODDSAPI_CMD_REPLY_SZ];

  if(sc->n == 0)
  {
    snprintf(line, sizeof(line), "No %s games in the last day or coming up.",
        league->label);
    cmd_reply(ctx, line);
    return;
  }

  oddsapi_scores_head(ctx, league, NULL, res);

  for(int pass = 0; pass < 3; pass++)
  {
    for(uint32_t k = 0; k < sc->n && shown < rows; k++)
    {
      const oddsapi_score_t *s    = &sc->scores[pass == 1 ? sc->n - 1 - k : k];
      int                    tier = !s->completed && s->has_scores ? 0
                                    : s->completed ? 1 : 2;

      if(tier != pass)
        continue;

      oddsapi_score_row(ctx, s, now);
      shown++;
    }
  }

  if(sc->n > shown)
  {
    snprintf(line, sizeof(line), CLR_GRAY "+%u more — scores %s <team>"
        CLR_RESET, sc->n - shown, league->alias);
    cmd_reply(ctx, line);
  }
}

// ----------------------------------------------------------------------
// odds [<league>] <team> --props
// ----------------------------------------------------------------------

// The league's curated markets in its own order, the first
// plugin.oddsapi.props_rows players of each as oddsapi_props_sort left
// them. A market's label stands on its first row only, so the groups
// read down the page. Each side shows its best price and book at the
// consensus line.
static void
oddsapi_render_props(const cmd_ctx_t *ctx, const oddsapi_league_t *league,
    const oddsapi_props_t *ps, const oddsapi_res_t *res)
{
  uint32_t rows = (uint32_t)kv_get_uint("plugin.oddsapi.props_rows");
  char     line[ODDSAPI_CMD_REPLY_SZ];
  char     tail[ODDSAPI_CELL_SZ * 3 + 16];
  char     as_of[ODDSAPI_CELL_SZ * 3];

  oddsapi_as_of(res, time(NULL), as_of, sizeof(as_of));
  snprintf(tail, sizeof(tail), "props · %s", as_of);
  oddsapi_render_title(ctx, league, ps->away, ps->home, ps->commence, tail);

  line[0] = '\0';
  oddsapi_cell(line, sizeof(line), CLR_GRAY "player", ODDSAPI_PROP_NAME_W,
      false);
  oddsapi_cell(line, sizeof(line), "market", ODDSAPI_PROP_MKT_W, false);
  oddsapi_cell(line, sizeof(line), "line  ", ODDSAPI_PROP_LINE_W, true);
  oddsapi_cell(line, sizeof(line), "best over", ODDSAPI_PROP_PX_W, false);
  display_cat(line, sizeof(line), "best under" CLR_RESET);
  cmd_reply(ctx, line);

  for(const oddsapi_prop_mkt_t *m = league->props; m->key != NULL; m++)
  {
    uint32_t shown = 0;

    for(uint32_t i = 0; i < ps->n && shown < rows; i++)
    {
      const oddsapi_prop_t *p = &ps->props[i];
      oddsapi_prop_line_t   l;
      char                  name[ODDSAPI_TEAM_SZ + 8];
      char                  pt[16];
      char                  over[ODDSAPI_PX_SZ];
      char                  under[ODDSAPI_PX_SZ];
      char                  code[ODDSAPI_CODE_SZ];

      if(strcmp(p->market, m->key) != 0)
        continue;

      oddsapi_prop_consensus(p, &l);
      display_fit(p->player, ODDSAPI_PROP_NAME_W - 2, name, sizeof(name),
          "…");

      if(p->has_point)
        snprintf(pt, sizeof(pt), "%.1f  ", (double)l.pt);

      else
        strlcpy(pt, "yes  ", sizeof(pt));

      oddsapi_px(l.best_over.px, l.best_over.px != 0
          ? oddsapi_book_code(ps->book_key[l.best_over.book],
              ps->book_title[l.best_over.book], code, sizeof(code))
          : NULL, over, sizeof(over));
      oddsapi_px(l.best_under.px, l.best_under.px != 0
          ? oddsapi_book_code(ps->book_key[l.best_under.book],
              ps->book_title[l.best_under.book], code, sizeof(code))
          : NULL, under, sizeof(under));

      line[0] = '\0';
      oddsapi_cell(line, sizeof(line), name, ODDSAPI_PROP_NAME_W, false);
      oddsapi_cell(line, sizeof(line), shown == 0 ? m->label : "",
          ODDSAPI_PROP_MKT_W, false);
      oddsapi_cell(line, sizeof(line), pt, ODDSAPI_PROP_LINE_W, true);
      oddsapi_cell(line, sizeof(line), over, ODDSAPI_PROP_PX_W, false);

      // A Yes/No market quotes no "No" side: say nothing rather than "—".
      if(p->has_point || l.best_under.px != 0)
        display_cat(line, sizeof(line), under);

      cmd_reply(ctx, line);
      shown++;
    }
  }
}

// A game whose props are not posted yet still names its matchup.
static void
oddsapi_on_props(const oddsapi_res_t *res, void *user)
{
  oddsapi_req_t       *r   = user;
  const oddsapi_hit_t *hit = &r->hits[0];

  if(res->status == ODDSAPI_OK && res->props->n > 0)
    oddsapi_render_props(&r->ctx, r->league, res->props, res);

  else
  {
    oddsapi_render_title(&r->ctx, r->league, hit->ev.away, hit->ev.home,
        hit->ev.commence, NULL);

    if(res->status == ODDSAPI_OK || res->status == ODDSAPI_NOT_FOUND)
      cmd_reply(&r->ctx, CLR_GRAY "no player props posted yet" CLR_RESET);

    else
      cmd_reply(&r->ctx, oddsapi_status_note(res->status));
  }

  oddsapi_render_also(r);
  mem_free(r);
}

// ----------------------------------------------------------------------
// odds — the request and its completions
// ----------------------------------------------------------------------

static oddsapi_req_t *
oddsapi_req_new(const cmd_ctx_t *ctx)
{
  oddsapi_req_t *r = mem_alloc(ODDSAPI_CMD_CTX, "req", sizeof(*r));

  memset(r, 0, sizeof(*r));
  r->ctx = *ctx;

  if(ctx->msg != NULL)
    r->msg = *ctx->msg;

  r->ctx.msg      = &r->msg;
  r->ctx.args     = NULL;
  r->ctx.username = NULL;
  r->ctx.parsed   = NULL;

  return(r);
}

// Flags come out wherever they stand and the words left are rejoined
// single-spaced, so "new  york" is "new york". A first word naming a
// league is the league and the rest a team; otherwise all of it is a
// team. False when no word is left.
static bool
oddsapi_args_parse(const char *args, oddsapi_args_t *out)
{
  char   buf[ODDSAPI_CMD_REPLY_SZ];
  char   line[ODDSAPI_CMD_REPLY_SZ];
  char   word[ODDSAPI_SPORT_KEY_SZ];
  char  *save = NULL;
  size_t n;

  memset(out, 0, sizeof(*out));

  if(args == NULL)
    return(false);

  strlcpy(buf, args, sizeof(buf));
  line[0] = '\0';

  for(char *tok = strtok_r(buf, " \t", &save); tok != NULL;
      tok = strtok_r(NULL, " \t", &save))
  {
    if(strcmp(tok, "-f") == 0 || strcmp(tok, "--futures") == 0)
    {
      out->futures = true;
      continue;
    }

    if(strcmp(tok, "-p") == 0 || strcmp(tok, "--props") == 0)
    {
      out->props = true;
      continue;
    }

    if(line[0] != '\0')
      strlcat(line, " ", sizeof(line));

    strlcat(line, tok, sizeof(line));
  }

  if(line[0] == '\0')
    return(false);

  n = strcspn(line, " ");

  if(n < sizeof(word))
  {
    memcpy(word, line, n);
    word[n]     = '\0';
    out->league = oddsapi_league_find(word);
  }

  strlcpy(out->query, out->league == NULL ? line
      : line[n] == ' ' ? line + n + 1 : "", sizeof(out->query));
  return(true);
}

static void
oddsapi_usage(const cmd_ctx_t *ctx, const char *usage)
{
  size_t                  n;
  const oddsapi_league_t *t = oddsapi_league_table(&n);
  char                    line[ODDSAPI_CMD_REPLY_SZ];

  cmd_result_set(ctx, CMD_RESULT_REFUSED);
  snprintf(line, sizeof(line), "usage: %s", usage);
  cmd_reply(ctx, line);
  strlcpy(line, "leagues:", sizeof(line));

  for(size_t i = 0; i < n; i++)
  {
    strlcat(line, " ", sizeof(line));
    strlcat(line, t[i].alias, sizeof(line));

    if(t[i].alt != NULL)
    {
      strlcat(line, "/", sizeof(line));
      strlcat(line, t[i].alt, sizeof(line));
    }
  }

  cmd_reply(ctx, line);
}

static void
oddsapi_on_slate(const oddsapi_res_t *res, void *user)
{
  oddsapi_req_t *r = user;

  if(res->status != ODDSAPI_OK)
    cmd_reply(&r->ctx, oddsapi_status_note(res->status));

  else
    oddsapi_render_slate(&r->ctx, r->league, res);

  mem_free(r);
}

// A bare team: the free sports list says which leagues are in season.
// If it cannot be had, every configured league is searched — the events
// lists are free either way.
static void
oddsapi_on_sports(const oddsapi_res_t *res, void *user)
{
  oddsapi_search((oddsapi_req_t *)user,
      res->status == ODDSAPI_OK ? res->sports : NULL);
}

// Fan the free events call out over the leagues to search: the one
// named, or plugin.oddsapi.search_leagues less any `sp` marks out of
// season. Takes ownership of `r`.
//
// The join is wordnik's: `pending` starts at 1 for the submitter, +1
// per submit, and the submitter's own release comes after the loop —
// because a cached list answers INSIDE the submit call, and without
// that hold the first league to answer would be the last.
static void
oddsapi_search(oddsapi_req_t *r, const oddsapi_sports_t *sp)
{
  char  csv[ODDSAPI_CMD_REPLY_SZ];
  char *save = NULL;

  if(r->league != NULL)
    r->parts[r->nparts++].league = r->league;

  else
  {
    strlcpy(csv, kv_get_str("plugin.oddsapi.search_leagues"), sizeof(csv));

    for(char *tok = strtok_r(csv, ", ", &save);
        tok != NULL && r->nparts < ODDSAPI_SEARCH_MAX;
        tok = strtok_r(NULL, ", ", &save))
    {
      const oddsapi_league_t *lg   = oddsapi_league_find(tok);
      bool                    skip = lg == NULL;

      if(lg == NULL)
        clam(CLAM_WARN, ODDSAPI_CMD_CTX, "search_leagues: no league '%s'",
            tok);

      for(uint8_t i = 0; !skip && i < r->nparts; i++)
        skip = r->parts[i].league == lg;

      for(uint32_t i = 0; !skip && sp != NULL && i < sp->n; i++)
      {
        if(strcmp(sp->sports[i].key, lg->sport_key) == 0)
          skip = !sp->sports[i].active;
      }

      if(!skip)
        r->parts[r->nparts++].league = lg;
    }
  }

  if(r->nparts == 0)
  {
    snprintf(csv, sizeof(csv), "None of the leagues a team is searched in "
        "is in season right now — name one: %s <league> <team>",
        r->mode == ODDSAPI_MODE_SCORES_TEAM ? "scores" : "odds");
    cmd_result_set(&r->ctx, CMD_RESULT_REFUSED);
    cmd_reply(&r->ctx, csv);
    mem_free(r);
    return;
  }

  r->pending = 1;

  for(uint8_t i = 0; i < r->nparts; i++)
  {
    oddsapi_part_t *p = &r->parts[i];

    p->req = r;
    __atomic_fetch_add(&r->pending, 1, __ATOMIC_ACQ_REL);

    if(oddsapi_events_async(p->league->sport_key, oddsapi_on_events, p)
        == ASYNC_FAILED_UNDELIVERED)
      __atomic_fetch_sub(&r->pending, 1, __ATOMIC_ACQ_REL);
  }

  oddsapi_search_release(r);
}

static void
oddsapi_on_events(const oddsapi_res_t *res, void *user)
{
  oddsapi_part_t *p = user;
  oddsapi_req_t  *r = p->req;

  if(res->status != ODDSAPI_OK)
    clam(CLAM_DEBUG, ODDSAPI_CMD_CTX, "%s events: %s", p->league->label,
        res->message);

  for(uint32_t i = 0; res->status == ODDSAPI_OK && i < res->events->n; i++)
  {
    const oddsapi_event_t *ev = &res->events->events[i];
    oddsapi_hit_t          hit;

    if(!oddsapi_team_match(ev->home, r->query)
        && !oddsapi_team_match(ev->away, r->query))
      continue;

    hit.league = p->league;
    hit.ev     = *ev;
    oddsapi_hit_insert(p->hits, &p->nhits, &hit);
  }

  oddsapi_search_release(r);
}

// Keep the ODDSAPI_HITS_MAX soonest, in start order.
static void
oddsapi_hit_insert(oddsapi_hit_t *hits, uint8_t *n, const oddsapi_hit_t *hit)
{
  uint8_t pos = 0;

  while(pos < *n && hits[pos].ev.commence <= hit->ev.commence)
    pos++;

  if(pos >= ODDSAPI_HITS_MAX)
    return;

  if(*n < ODDSAPI_HITS_MAX)
    (*n)++;

  memmove(&hits[pos + 1], &hits[pos], (size_t)(*n - 1 - pos) * sizeof(*hits));
  hits[pos] = *hit;
}

// The last league to answer merges every part and spends the one pull
// the soonest match needs: that league's odds (3 credits cold) or its
// scores (2), and nothing on a warm cache. Never touch `r` after calling
// this.
static void
oddsapi_search_release(oddsapi_req_t *r)
{
  char       line[ODDSAPI_CMD_REPLY_SZ];
  async_rc_t rc;

  if(__atomic_fetch_sub(&r->pending, 1, __ATOMIC_ACQ_REL) != 1)
    return;

  for(uint8_t i = 0; i < r->nparts; i++)
  {
    for(uint8_t j = 0; j < r->parts[i].nhits; j++)
      oddsapi_hit_insert(r->hits, &r->nhits, &r->parts[i].hits[j]);
  }

  if(r->nhits == 0)
  {
    snprintf(line, sizeof(line), "No upcoming game found for '%s' in",
        r->query);

    for(uint8_t i = 0; i < r->nparts; i++)
    {
      strlcat(line, i == 0 ? " " : ", ", sizeof(line));
      strlcat(line, r->parts[i].league->alias, sizeof(line));
    }

    strlcat(line, ".", sizeof(line));
    cmd_reply(&r->ctx, line);
    mem_free(r);
    return;
  }

  r->league = r->hits[0].league;

  // A team's scores cost only the league its match landed in — never 2
  // credits per searched league, which a cold fan-out over the scores
  // lists themselves would spend. The price: the free events lists hold
  // only games live or ahead, so a team whose season is over is not
  // found, even with a final still inside the scores window.
  //
  // Props are pulled for the one game found, in the curated markets of
  // its league; a league with none answers here and spends nothing.
  if(r->mode == ODDSAPI_MODE_PROPS && r->league->props == NULL)
  {
    oddsapi_render_title(&r->ctx, r->league, r->hits[0].ev.away,
        r->hits[0].ev.home, r->hits[0].ev.commence, NULL);
    snprintf(line, sizeof(line), "No player props for %s.", r->league->label);
    cmd_reply(&r->ctx, line);
    oddsapi_render_also(r);
    mem_free(r);
    return;
  }

  if(r->mode == ODDSAPI_MODE_SCORES_TEAM)
    rc = oddsapi_scores_async(r->league->sport_key, oddsapi_on_score_team,
        r);

  else if(r->mode == ODDSAPI_MODE_PROPS)
    rc = oddsapi_props_async(r->league->sport_key, r->hits[0].ev.id,
        oddsapi_on_props, r);

  else
    rc = oddsapi_odds_async(r->league->sport_key, oddsapi_on_card, r);

  if(rc == ASYNC_FAILED_UNDELIVERED)
  {
    oddsapi_render_title(&r->ctx, r->league, r->hits[0].ev.away,
        r->hits[0].ev.home, r->hits[0].ev.commence, NULL);
    cmd_reply(&r->ctx, oddsapi_status_note(ODDSAPI_TRANSPORT));
    oddsapi_render_also(r);
    mem_free(r);
  }
}

// The card rides the league pull: the game is found in the slate by id,
// and a game whose lines are not posted yet is named with no credit
// spent on it beyond that pull.
static void
oddsapi_on_card(const oddsapi_res_t *res, void *user)
{
  oddsapi_req_t        *r   = user;
  const oddsapi_hit_t  *hit = &r->hits[0];
  const oddsapi_game_t *g   = NULL;
  char                  line[ODDSAPI_CMD_REPLY_SZ];

  for(uint32_t i = 0; res->status == ODDSAPI_OK && g == NULL
      && i < res->slate->ngames; i++)
  {
    if(strcmp(res->slate->games[i].id, hit->ev.id) == 0)
      g = &res->slate->games[i];
  }

  if(g != NULL)
    oddsapi_render_game(&r->ctx, r->league, g, res);

  else
  {
    oddsapi_render_title(&r->ctx, r->league, hit->ev.away, hit->ev.home,
        hit->ev.commence, NULL);

    if(res->status == ODDSAPI_OK)
    {
      snprintf(line, sizeof(line), CLR_GRAY "no lines posted yet" CLR_RESET);
      cmd_reply(&r->ctx, line);
    }

    else
      cmd_reply(&r->ctx, oddsapi_status_note(res->status));
  }

  oddsapi_render_also(r);
  mem_free(r);
}

// Live first, then the latest final, then the soonest still to come.
// The list is in start order, so the last final seen is the latest.
static const oddsapi_score_t *
oddsapi_score_pick(const oddsapi_scores_t *sc, const char *query)
{
  const oddsapi_score_t *final = NULL;
  const oddsapi_score_t *next  = NULL;

  for(uint32_t i = 0; i < sc->n; i++)
  {
    const oddsapi_score_t *s = &sc->scores[i];

    if(!oddsapi_team_match(s->home, query)
        && !oddsapi_team_match(s->away, query))
      continue;

    if(!s->completed && s->has_scores)
      return(s);

    if(s->completed)
      final = s;

    else if(next == NULL)
      next = s;
  }

  return(final != NULL ? final : next);
}

// A team's one game. When the scores window does not hold it, the game
// the search found is still the team's next one, with nothing to score.
static void
oddsapi_on_score_team(const oddsapi_res_t *res, void *user)
{
  oddsapi_req_t         *r    = user;
  const oddsapi_hit_t   *hit  = &r->hits[0];
  const oddsapi_score_t *pick = NULL;
  oddsapi_score_t        next;

  if(res->status == ODDSAPI_OK)
    pick = oddsapi_score_pick(res->scores, r->query);

  if(pick == NULL)
  {
    memset(&next, 0, sizeof(next));
    strlcpy(next.home, hit->ev.home, sizeof(next.home));
    strlcpy(next.away, hit->ev.away, sizeof(next.away));
    next.commence = hit->ev.commence;
    pick          = &next;
  }

  oddsapi_scores_head(&r->ctx, r->league, r->query, res);

  if(res->status != ODDSAPI_OK)
    cmd_reply(&r->ctx, oddsapi_status_note(res->status));

  oddsapi_score_row(&r->ctx, pick, time(NULL));
  oddsapi_render_also(r);
  mem_free(r);
}

static void
oddsapi_on_scores(const oddsapi_res_t *res, void *user)
{
  oddsapi_req_t *r = user;

  if(res->status != ODDSAPI_OK)
    cmd_reply(&r->ctx, oddsapi_status_note(res->status));

  else
    oddsapi_render_scores(&r->ctx, r->league, res);

  mem_free(r);
}

// The snapshot is shared and immutable, so the favourites are sorted on
// a copy of its contenders.
static void
oddsapi_on_futures(const oddsapi_res_t *res, void *user)
{
  oddsapi_req_t     *r = user;
  oddsapi_futures_t  f;
  char               line[ODDSAPI_CMD_REPLY_SZ];

  if(res->status != ODDSAPI_OK)
    cmd_reply(&r->ctx, oddsapi_status_note(res->status));

  else if(res->futures->n == 0)
  {
    snprintf(line, sizeof(line), "No %s futures posted right now.",
        r->league->label);
    cmd_reply(&r->ctx, line);
  }

  else
  {
    f            = *res->futures;
    f.contenders = mem_alloc(ODDSAPI_CMD_CTX, "futures",
        f.n * sizeof(*f.contenders));
    memcpy(f.contenders, res->futures->contenders,
        f.n * sizeof(*f.contenders));
    oddsapi_futures_sort(&f);
    oddsapi_render_futures(&r->ctx, r->league, res, &f);
    mem_free(f.contenders);
  }

  mem_free(r);
}

// Both commands' front door: something to look up, and a key to look it
// up with. False when it has already answered.
static bool
oddsapi_ready(const cmd_ctx_t *ctx, const oddsapi_args_t *a,
    const char *usage)
{
  if(a == NULL)
  {
    oddsapi_usage(ctx, usage);
    return(false);
  }

  if(!oddsapi_configured())
  {
    cmd_result_set(ctx, CMD_RESULT_REFUSED);
    cmd_reply(ctx, "Odds aren't set up yet — the operator needs to set "
        "plugin.oddsapi.creds.apikey.");
    return(false);
  }

  if(a->query[0] != '\0' && strlen(a->query) < ODDSAPI_QUERY_MIN)
  {
    cmd_result_set(ctx, CMD_RESULT_REFUSED);
    cmd_reply(ctx, "Give me at least three letters of a team's name.");
    return(false);
  }

  return(true);
}

static void
oddsapi_start(const cmd_ctx_t *ctx, oddsapi_mode_t mode,
    const oddsapi_args_t *a)
{
  oddsapi_req_t *r  = oddsapi_req_new(ctx);
  async_rc_t     rc = ASYNC_FAILED_UNDELIVERED;

  r->mode   = mode;
  r->league = a->league;
  strlcpy(r->query, a->query, sizeof(r->query));

  switch(mode)
  {
    case ODDSAPI_MODE_SLATE:
      rc = oddsapi_odds_async(a->league->sport_key, oddsapi_on_slate, r);
      break;

    case ODDSAPI_MODE_FUTURES:
      rc = oddsapi_futures_async(a->league->outright_key, oddsapi_on_futures,
          r);
      break;

    case ODDSAPI_MODE_SCORES:
      rc = oddsapi_scores_async(a->league->sport_key, oddsapi_on_scores, r);
      break;

    // A named league is searched without asking whether it is in
    // season: whoever typed it already decided.
    case ODDSAPI_MODE_TEAM:
    case ODDSAPI_MODE_SCORES_TEAM:
    case ODDSAPI_MODE_PROPS:
      if(a->league != NULL)
      {
        oddsapi_search(r, NULL);
        return;
      }

      rc = oddsapi_sports_async(oddsapi_on_sports, r);
      break;
  }

  if(rc == ASYNC_FAILED_UNDELIVERED)
  {
    cmd_reply(ctx, oddsapi_status_note(ODDSAPI_TRANSPORT));
    mem_free(r);
  }
}

static void
oddsapi_odds_cmd(const cmd_ctx_t *ctx)
{
  oddsapi_args_t a;
  bool           ok = oddsapi_args_parse(ctx->args, &a);
  char           line[ODDSAPI_CMD_REPLY_SZ];

  if(!oddsapi_ready(ctx, ok ? &a : NULL, ODDSAPI_ODDS_USAGE))
    return;

  if(a.props)
  {
    if(a.futures || a.query[0] == '\0')
      oddsapi_usage(ctx, ODDSAPI_ODDS_USAGE);

    else if(a.league != NULL && a.league->props == NULL)
    {
      snprintf(line, sizeof(line), "No player props for %s.",
          a.league->label);
      cmd_reply(ctx, line);
    }

    else
      oddsapi_start(ctx, ODDSAPI_MODE_PROPS, &a);

    return;
  }

  if(!a.futures)
  {
    oddsapi_start(ctx, a.query[0] == '\0' ? ODDSAPI_MODE_SLATE
        : ODDSAPI_MODE_TEAM, &a);
    return;
  }

  if(a.league == NULL || a.query[0] != '\0')
    oddsapi_usage(ctx, ODDSAPI_ODDS_USAGE);

  else if(a.league->outright_key == NULL)
  {
    snprintf(line, sizeof(line), "No futures market for %s.",
        a.league->label);
    cmd_reply(ctx, line);
  }

  else
    oddsapi_start(ctx, ODDSAPI_MODE_FUTURES, &a);
}

static void
oddsapi_scores_cmd(const cmd_ctx_t *ctx)
{
  oddsapi_args_t a;
  bool           ok = oddsapi_args_parse(ctx->args, &a);

  if(!oddsapi_ready(ctx, ok && !a.futures && !a.props ? &a : NULL,
      ODDSAPI_SCORES_USAGE))
    return;

  oddsapi_start(ctx, a.query[0] == '\0' ? ODDSAPI_MODE_SCORES
      : ODDSAPI_MODE_SCORES_TEAM, &a);
}

// ----------------------------------------------------------------------
// NL hints — a bridged ask is retold by the persona, so a slate comes
// back summarized; the grid is for whoever types the command
// ----------------------------------------------------------------------

static const cmd_nl_slot_t oddsapi_nl_slots[] = {
  { .name  = "league or team",
    .type  = CMD_NL_ARG_TOPIC,
    .flags = CMD_NL_SLOT_REQUIRED | CMD_NL_SLOT_REMAINDER },
};

// Measured, not written: on q38nv every wording variant tried moved
// routing, most for the worse, and dropping the props example dropped
// odds routing everywhere. That example also teaches its own team, so
// another player's prop pulls the Chiefs' game. AGENTS.md §Chat.
static const cmd_nl_example_t odds_nl_examples[] = {
  { .utterance  = "who's favoured in the nba tonight?",
    .invocation = "/odds nba" },
  { .utterance  = "what's the line on the lakers game?",
    .invocation = "/odds lakers" },
  { .utterance  = "are the celtics favoured tonight?",
    .invocation = "/odds celtics" },
  { .utterance  = "who's the favourite to win the world series?",
    .invocation = "/odds mlb --futures" },
  { .utterance  = "what's mahomes' passing yards line?",
    .invocation = "/odds chiefs --props" },
};

static const cmd_nl_t odds_nl = {
  .when          = "User asks who is favoured or expected to win a game, "
                   "a fight or a title, or for a point spread, an "
                   "over/under or a player's prop line — sportsbook odds. "
                   "Leagues: nfl ncaaf nba ncaab wnba mlb nhl mls epl "
                   "laliga ucl ufc boxing.",
  .syntax        = "/odds <league> | /odds [league] <team> [--props] | "
                   "/odds <league> --futures",
  .slots         = oddsapi_nl_slots,
  .slot_count    = (uint8_t)(sizeof(oddsapi_nl_slots)
                             / sizeof(oddsapi_nl_slots[0])),
  .examples      = odds_nl_examples,
  .example_count = (uint8_t)(sizeof(odds_nl_examples)
                             / sizeof(odds_nl_examples[0])),
};

static const cmd_nl_example_t scores_nl_examples[] = {
  { .utterance  = "what's the score of the yankees game?",
    .invocation = "/scores yankees" },
  { .utterance  = "nfl scores from yesterday",
    .invocation = "/scores nfl" },
  { .utterance  = "did the bruins win?",
    .invocation = "/scores bruins" },
};

static const cmd_nl_t scores_nl = {
  .when          = "User asks for the score or final result of a game "
                   "live or just played, for a league or a team. Never "
                   "for who is favoured — that is /odds. Same leagues as "
                   "/odds.",
  .syntax        = "/scores <league> | /scores [league] <team>",
  .slots         = oddsapi_nl_slots,
  .slot_count    = (uint8_t)(sizeof(oddsapi_nl_slots)
                             / sizeof(oddsapi_nl_slots[0])),
  .examples      = scores_nl_examples,
  .example_count = (uint8_t)(sizeof(scores_nl_examples)
                             / sizeof(scores_nl_examples[0])),
};

// ----------------------------------------------------------------------
// Registration
// ----------------------------------------------------------------------

static const cmd_decl_t show_odds_decl = {
  .module      = ODDSAPI_CTX,
  .name        = "odds",
  .usage       = "show odds",
  .description = "The Odds API's credit quota and what the odds cache holds.",
  .help_long   = "Credits come from the headers of the last reply of any "
                 "kind, free calls included, so they are unknown until the "
                 "plugin has made one. Each cache row is one league's lines, "
                 "game list, scores or futures, with its age. Reads only "
                 "local state; costs nothing.",
  .group       = USERNS_GROUP_EVERYONE,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = oddsapi_show_cmd,
  .parent_path = "show",
};

static const cmd_decl_t odds_decl = {
  .module      = ODDSAPI_CTX,
  .name        = "odds",
  .usage       = ODDSAPI_ODDS_USAGE,
  .description = "Betting lines — a league's slate or one team's next game, "
                 "consensus and best price across US sportsbooks "
                 "(the-odds-api.com)",
  .help_long   = "A league alone lists its games: the consensus money line, "
                 "home spread and total, each the median across the books "
                 "that quote it. A team — with or without its league — "
                 "shows its next game book by book, then the consensus and "
                 "the best price with the book offering it. Spreads and "
                 "totals are priced at the line most books agree on, so a "
                 "best price is never one from a different line. `odds` "
                 "alone lists the league names. A league's lines are cached "
                 "for plugin.oddsapi.odds_ttl and every answer says how old "
                 "it is; "
                 "when the month's credits run low, the cached copy is "
                 "served with its age instead of spending more. "
                 "--futures (-f) lists a league's outright market, "
                 "favourite first: the consensus price, the probability it "
                 "implies — vig included, so the column sums past 100% — "
                 "and the best price with its book. "
                 "--props (-p) with a team shows its next game's player "
                 "props — a few headline markets per sport, each player's "
                 "consensus line and the best price on either side; it "
                 "costs about one credit per market, once per game while "
                 "cached.",
  .group       = USERNS_GROUP_EVERYONE,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = oddsapi_odds_cmd,
  .nl          = &odds_nl,
};

static const cmd_decl_t scores_decl = {
  .module      = ODDSAPI_CTX,
  .name        = "scores",
  .usage       = ODDSAPI_SCORES_USAGE,
  .description = "Live and recent scores (the-odds-api.com)",
  .help_long   = "A league lists its games live now, then finals from the "
                 "last day, newest first, then the next to start. A team — "
                 "with or without its league — shows its live game, else "
                 "its latest final, else its next game. The source has no "
                 "game clock, so a game in progress says only LIVE. "
                 "`scores` alone lists the league names. A league's scores "
                 "are cached for plugin.oddsapi.scores_ttl.",
  .group       = USERNS_GROUP_EVERYONE,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = oddsapi_scores_cmd,
  .nl          = &scores_nl,
};

// Raised together: half a command surface is a half-loaded plugin, so a
// later failure unwinds what came before it.
bool
oddsapi_cmd_register(void)
{
  if(cmd_register(&show_odds_decl) != SUCCESS)
    return(FAIL);

  if(cmd_register(&odds_decl) != SUCCESS)
  {
    cmd_unregister_path("show/odds");
    return(FAIL);
  }

  if(cmd_register(&scores_decl) != SUCCESS)
  {
    cmd_unregister_path("odds");
    cmd_unregister_path("show/odds");
    return(FAIL);
  }

  clam(CLAM_INFO, ODDSAPI_CMD_CTX, "show odds, odds and scores registered");
  return(SUCCESS);
}

void
oddsapi_cmd_unregister(void)
{
  cmd_unregister_path("scores");
  cmd_unregister_path("odds");
  cmd_unregister_path("show/odds");
  clam(CLAM_INFO, ODDSAPI_CMD_CTX, "show odds, odds and scores unregistered");
}
