// botmanager — MIT
// melee's three read-only views: `show melee` draws the round burning in
// this room, `show melee scores` the lifetime standings of everyone who
// has ever swung in this namespace, and `show melee llm` says where the
// pit's words are coming from.
//
// The first two are plain SELECTs and take no lock. melee_turn_lock
// serialises *writers*; a blow lands as one transaction, so the worst a
// reader can catch is the instant between two finished turns. The third
// takes melee_pool_lock only, never the turn lock.

#define MELEE_INTERNAL
#include "melee.h"

#include "colors.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// ------------------------------------------------------------------ //
// Column geometry                                                     //
// ------------------------------------------------------------------ //

// Both tables are built cell by cell through the padding helpers below,
// header included, so a width only ever has to change in one place.

#define MELEE_W_NAME    13   // combatant, left-aligned
#define MELEE_W_BAR     15   // the health bar, one cell per glyph
#define MELEE_W_HP       9   // "74/100"
#define MELEE_W_NUM      7   // dealt, taken
#define MELEE_W_CRIT     6
#define MELEE_W_CARD    (2 + MELEE_W_NAME + MELEE_W_BAR + 1 + MELEE_W_HP \
                         + 2 * MELEE_W_NUM + MELEE_W_CRIT)

#define MELEE_W_RANK     3
#define MELEE_W_ROUNDS   7
#define MELEE_W_KILLS    6
#define MELEE_W_DEATHS   7
#define MELEE_W_DEALT    8
#define MELEE_W_TAKEN    7
#define MELEE_W_CRITS    6
#define MELEE_W_BEST     6
#define MELEE_W_BOARD   (2 + MELEE_W_RANK + 1 + MELEE_W_NAME + MELEE_W_ROUNDS \
                         + MELEE_W_KILLS + MELEE_W_DEATHS + MELEE_W_DEALT \
                         + MELEE_W_TAKEN + MELEE_W_CRITS + MELEE_W_BEST)

// One column's worth of text plus its color markers.
#define MELEE_CELL_SZ  128

// A name already fitted to the combatant column: at most MELEE_W_NAME-1
// display columns, and a display column is at most four UTF-8 bytes.
// Sized from the geometry so the cell buffer provably swallows it.
#define MELEE_NAME_SZ  ((MELEE_W_NAME - 1) * 4 + 1)

// ------------------------------------------------------------------ //
// Alignment                                                           //
// ------------------------------------------------------------------ //

// Visible column count: an abstract color marker ("\x01" + id) is two
// bytes of zero width, and every glyph the pit draws — block, skull,
// arrow, en-dash — is a single-column code point, so counting UTF-8 lead
// bytes is the column count. Same shape as stock.c's renderer.
static size_t
melee_vis_len(const char *s)
{
  size_t n = 0;

  while(*s != '\0')
  {
    unsigned char c = (unsigned char)*s;

    if(c == '\x01' && s[1] != '\0')
    {
      s += 2;
      continue;
    }

    if((c & 0xc0) != 0x80)
      n++;

    s++;
  }

  return(n);
}

// Right-align: shift the content up and fill the gap with spaces.
static void
melee_pad(char *buf, size_t sz, int width)
{
  size_t vis = melee_vis_len(buf);
  size_t raw = strlen(buf);
  int    pad = width - (int)vis;
  int    i;

  if(pad <= 0 || raw + (size_t)pad + 1 > sz)
    return;

  memmove(buf + pad, buf, raw + 1);

  for(i = 0; i < pad; i++)
    buf[i] = ' ';
}

// Left-align: trail spaces until the cell fills its width.
static void
melee_padr(char *buf, size_t sz, int width)
{
  size_t vis = melee_vis_len(buf);
  size_t raw = strlen(buf);
  int    pad = width - (int)vis;
  int    i;

  if(pad <= 0)
    return;

  if(raw + (size_t)pad + 1 > sz)
    pad = (int)(sz - raw - 1);

  for(i = 0; i < pad; i++)
    buf[raw + (size_t)i] = ' ';

  buf[raw + (size_t)pad] = '\0';
}

// Copy at most `cols` display columns, never splitting a UTF-8 sequence.
// `src` is a nickname straight from the database: no color markers, but
// no guarantee of ASCII either.
static void
melee_fit(const char *src, int cols, char *dst, size_t sz)
{
  size_t n = 0;
  int    w = 0;

  while(*src != '\0' && w < cols)
  {
    unsigned char c   = (unsigned char)*src;
    size_t        len = 1;
    size_t        k;

    if((c & 0xe0) == 0xc0)      len = 2;
    else if((c & 0xf0) == 0xe0) len = 3;
    else if((c & 0xf8) == 0xf0) len = 4;

    // A NUL inside the sequence (a name byte-truncated mid-glyph on its
    // way into a VARCHAR) bounds len to the bytes actually there.
    for(k = 0; k < len; k++)
      if(src[k] == '\0')
      {
        len = k;
        break;
      }

    if(len == 0 || n + len + 1 > sz)
      break;

    for(k = 0; k < len; k++)
      dst[n++] = src[k];

    src += len;
    w++;
  }

  dst[n] = '\0';
}

// Append one finished cell to a line, stopping cleanly at capacity.
static void
melee_cat(char *line, size_t cap, const char *cell)
{
  size_t n = strlen(line);

  if(n + 1 < cap)
    snprintf(line + n, cap - n, "%s", cell);
}

// Render a number into at most `width - 1` columns, so a cell always
// keeps one space between itself and its neighbour. Lifetime damage
// grows without bound; a column does not. Anything too wide is
// abbreviated K/M/B/T, and the grid survives a namespace that has been
// spilling blood for years.
static void
melee_fmt_num(char *out, size_t cap, int64_t v, int width)
{
  static const char unit[] = { 'K', 'M', 'B', 'T' };
  double            scaled = (double)v;
  int               u      = -1;

  snprintf(out, cap, "%" PRId64, v);

  if((int)strlen(out) <= width - 1)
    return;

  while(u < 3 && (scaled >= 1000.0 || scaled <= -1000.0))
  {
    scaled /= 1000.0;
    u++;
  }

  if(u < 0)               // too wide, yet under a thousand: leave it be
    return;

  // The bound is 99.95, not 100: `%.1f` *rounds*, so a combatant on
  // 99997 of 100000 hp scales to 99.997 and prints as "100.0K" — six
  // characters where the one-decimal branch promised five, shoving the
  // rest of the row two columns sideways. Anything that would round up
  // to three integer digits belongs in the no-decimal branch.
  if(scaled > -99.95 && scaled < 99.95)
    snprintf(out, cap, "%.1f%c", scaled, unit[u]);

  else
    snprintf(out, cap, "%.0f%c", scaled, unit[u]);
}

// A horizontal rule `cols` columns wide, in the dim color the tables
// frame themselves with.
static void
melee_rule(char *out, size_t cap, int cols)
{
  size_t n;
  int    i;

  snprintf(out, cap, "%s", CLR_GRAY);
  n = strlen(out);

  for(i = 0; i < cols && n + 4 < cap; i++)
  {
    memcpy(out + n, "─", 3);
    n += 3;
  }

  snprintf(out + n, cap - n, "%s", CLR_RESET);
}

// ------------------------------------------------------------------ //
// The health bar                                                      //
// ------------------------------------------------------------------ //

// Green while it is a fight, yellow while it is a worry, red while it is
// nearly over. The fallen keep their column so the grid holds its shape.
static void
melee_hp_bar(char *out, size_t cap, int32_t hp, int32_t hp_max)
{
  const char *color;
  int32_t     pct;
  int         filled;
  int         i;
  size_t      n;

  if(hp <= 0)
  {
    snprintf(out, cap, CLR_RED "☠" CLR_RESET);
    melee_padr(out, cap, MELEE_W_BAR);
    return;
  }

  if(hp_max <= 0)     // a truncated row should still draw something sane
    hp_max = hp;

  pct    = (int32_t)(((int64_t)hp * 100) / hp_max);
  filled = (int)(((int64_t)hp * MELEE_W_BAR) / hp_max);

  if(filled < 1)      // a survivor always shows at least one cell
    filled = 1;

  if(filled > MELEE_W_BAR)
    filled = MELEE_W_BAR;

  color = (pct > 60) ? CLR_GREEN : (pct >= 25) ? CLR_YELLOW : CLR_RED;

  snprintf(out, cap, "%s", color);
  n = strlen(out);

  for(i = 0; i < MELEE_W_BAR && n + 4 < cap; i++)
  {
    memcpy(out + n, (i < filled) ? "█" : "░", 3);
    n += 3;
  }

  snprintf(out + n, cap - n, "%s", CLR_RESET);
}

// ------------------------------------------------------------------ //
// show melee — the round card                                         //
// ------------------------------------------------------------------ //

static void
melee_card_header(const cmd_ctx_t *ctx)
{
  char cell[MELEE_CELL_SZ];
  char line[MELEE_LINE_SZ];

  snprintf(line, sizeof(line), "%s  ", CLR_GRAY);

  snprintf(cell, sizeof(cell), "combatant");
  melee_padr(cell, sizeof(cell), MELEE_W_NAME);
  melee_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "health");
  melee_padr(cell, sizeof(cell), MELEE_W_BAR + 1);
  melee_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "hp");
  melee_pad(cell, sizeof(cell), MELEE_W_HP);
  melee_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "dealt");
  melee_pad(cell, sizeof(cell), MELEE_W_NUM);
  melee_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "taken");
  melee_pad(cell, sizeof(cell), MELEE_W_NUM);
  melee_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "crit");
  melee_pad(cell, sizeof(cell), MELEE_W_CRIT);
  melee_cat(line, sizeof(line), cell);

  melee_cat(line, sizeof(line), CLR_RESET);
  cmd_reply(ctx, line);
}

// The affliction markers a combatant carries, colorized, or an empty
// string when they carry none. Each glyph is one display column, so the
// card grows by exactly (1 + count) columns on the widest afflicted row.
static void
melee_card_marks(char *out, size_t cap, const melee_card_row_t *row,
    const melee_dot_mark_t *marks, uint32_t n)
{
  uint32_t i;
  uint32_t k;

  out[0] = '\0';

  for(i = 0; i < n; i++)
  {
    if(strcmp(marks[i].victim, row->user) != 0)
      continue;

    melee_cat(out, cap, " ");

    for(k = 0; k < marks[i].n; k++)
    {
      char cell[32];

      snprintf(cell, sizeof(cell), "%s%s" CLR_RESET,
          melee_dot_color_of(marks[i].kinds[k]),
          melee_dot_emoji_of(marks[i].kinds[k]));
      melee_cat(out, cap, cell);
    }

    return;
  }
}

static void
melee_card_row(const cmd_ctx_t *ctx, const melee_card_row_t *row,
    const melee_dot_mark_t *marks, uint32_t n_marks)
{
  const bool alive = (row->hp > 0);
  char       name[MELEE_NAME_SZ];
  char       num [32];
  char       max [32];
  char       cell[MELEE_CELL_SZ];
  char       line[MELEE_LINE_SZ];

  line[0] = '\0';
  melee_cat(line, sizeof(line), "  ");

  // One column of breathing room is reserved so a long name never runs
  // into the bar.
  melee_fit(row->name, MELEE_W_NAME - 1, name, sizeof(name));
  snprintf(cell, sizeof(cell), "%s%s" CLR_RESET,
      alive ? CLR_CYAN : CLR_GRAY, name);
  melee_padr(cell, sizeof(cell), MELEE_W_NAME);
  melee_cat(line, sizeof(line), cell);

  melee_hp_bar(cell, sizeof(cell), row->hp, row->hp_max);
  melee_cat(line, sizeof(line), cell);
  melee_cat(line, sizeof(line), " ");

  // Each half of "74/100" gets half the column, so even a pit tuned to
  // the six-figure ceiling still reads as "100K/100K".
  melee_fmt_num(num, sizeof(num), row->hp,     MELEE_W_HP / 2);
  melee_fmt_num(max, sizeof(max), row->hp_max, MELEE_W_HP / 2);
  snprintf(cell, sizeof(cell), "%s%s" CLR_RESET "/%s",
      alive ? CLR_WHITE : CLR_GRAY, num, max);
  melee_pad(cell, sizeof(cell), MELEE_W_HP);
  melee_cat(line, sizeof(line), cell);

  melee_fmt_num(cell, sizeof(cell), row->dmg_given, MELEE_W_NUM);
  melee_pad(cell, sizeof(cell), MELEE_W_NUM);
  melee_cat(line, sizeof(line), cell);

  melee_fmt_num(cell, sizeof(cell), row->dmg_taken, MELEE_W_NUM);
  melee_pad(cell, sizeof(cell), MELEE_W_NUM);
  melee_cat(line, sizeof(line), cell);

  if(row->best_crit > 0)
  {
    melee_fmt_num(num, sizeof(num), row->best_crit, MELEE_W_CRIT);
    snprintf(cell, sizeof(cell), CLR_RED "%s" CLR_RESET, num);
  }

  else
    snprintf(cell, sizeof(cell), CLR_GRAY "×" CLR_RESET);

  melee_pad(cell, sizeof(cell), MELEE_W_CRIT);
  melee_cat(line, sizeof(line), cell);

  // Outside the grid, after the last padded cell: the markers are a
  // ragged tail, not a column, so no width promise is broken.
  melee_card_marks(cell, sizeof(cell), row, marks, n_marks);
  melee_cat(line, sizeof(line), cell);

  cmd_reply(ctx, line);
}

static void
melee_show_round(const cmd_ctx_t *ctx)
{
  melee_card_t     card;
  melee_card_row_t rows [MELEE_MAX_PLAYERS];
  melee_dot_mark_t marks[MELEE_MAX_PLAYERS];
  userns_t        *ns;
  const char      *state;
  char             line  [MELEE_LINE_SZ];
  char             rule  [MELEE_LINE_SZ];
  char             roster[MELEE_ROSTER_SZ];
  char             dur   [32];
  uint32_t         shown;
  uint32_t         n_marks;
  uint32_t         total = 0;
  uint32_t         i;

  ns = userns_session_resolve(ctx);

  if(ns == NULL)          // the resolver already replied
    return;

  // An empty channel is a direct message, which names no room: the
  // lookup widens to the namespace and reports whatever fought last.
  if(!melee_db_card_find(ns->id, method_inst_name(ctx->msg->inst),
        ctx->msg->channel, &card))
  {
    cmd_reply(ctx, "☠ The pit is quiet. Open one with !melee <nick>.");
    return;
  }

  state = (card.state == MELEE_ROUND_ACTIVE)  ? ""
        : (card.state == MELEE_ROUND_ENDED)   ? " · ended"
        :                                       " · abandoned";

  util_fmt_duration((time_t)card.length, dur, sizeof(dur));
  melee_rule(rule, sizeof(rule), MELEE_W_CARD);

  snprintf(line, sizeof(line),
      "⚔ " CLR_BOLD "MELEE" CLR_RESET CLR_GRAY " — " CLR_RESET
      CLR_CYAN "%s" CLR_RESET
      CLR_GRAY " · wave %d · %d %s · %s%s" CLR_RESET,
      (card.channel[0] != '\0') ? card.channel : "the pit",
      card.wave, card.blows, (card.blows == 1) ? "blow" : "blows",
      dur, state);
  cmd_reply(ctx, line);

  cmd_reply(ctx, rule);
  melee_card_header(ctx);

  shown = melee_db_card_roster(card.id, rows, MELEE_MAX_PLAYERS, &total);

  // Lock-free like everything else in this view, and optional: a failed
  // marker query draws a card without markers rather than no card.
  n_marks = melee_db_dot_marks(card.id, marks, MELEE_MAX_PLAYERS);

  for(i = 0; i < shown; i++)
    melee_card_row(ctx, &rows[i], marks, n_marks);

  cmd_reply(ctx, rule);

  if(total > shown)
  {
    snprintf(line, sizeof(line),
        CLR_GRAY "… and %" PRIu32 " more in the press." CLR_RESET,
        total - shown);
    cmd_reply(ctx, line);
  }

  if(card.state == MELEE_ROUND_ENDED && card.slayer[0] != '\0')
  {
    snprintf(line, sizeof(line),
        "☠ " CLR_CYAN "%s" CLR_RESET " left " CLR_PURPLE "%s" CLR_RESET
        " on the stone.", card.slayer, card.fallen);
    cmd_reply(ctx, line);
  }

  if(card.top_crit > 0)
  {
    snprintf(line, sizeof(line),
        "💥 heaviest blow: " CLR_CYAN "%s" CLR_RESET " → " CLR_PURPLE "%s"
        CLR_RESET " for " CLR_BOLD CLR_RED "%d" CLR_RESET,
        card.top_by, card.top_on, card.top_crit);
    cmd_reply(ctx, line);
  }

  if(card.state == MELEE_ROUND_ACTIVE &&
     melee_db_pending(card.id, card.wave, roster, sizeof(roster)) == SUCCESS &&
     roster[0] != '\0')
  {
    snprintf(line, sizeof(line),
        "⏳ yet to swing this wave: " CLR_CYAN "%s" CLR_RESET, roster);
    cmd_reply(ctx, line);
  }
}

// ------------------------------------------------------------------ //
// show melee scores — the leaderboard                                 //
// ------------------------------------------------------------------ //

static void
melee_board_header(const cmd_ctx_t *ctx)
{
  char cell[MELEE_CELL_SZ];
  char line[MELEE_LINE_SZ];

  snprintf(line, sizeof(line), "%s  ", CLR_GRAY);

  snprintf(cell, sizeof(cell), "#");
  melee_pad(cell, sizeof(cell), MELEE_W_RANK);
  melee_cat(line, sizeof(line), cell);
  melee_cat(line, sizeof(line), " ");

  snprintf(cell, sizeof(cell), "combatant");
  melee_padr(cell, sizeof(cell), MELEE_W_NAME);
  melee_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "rounds");
  melee_pad(cell, sizeof(cell), MELEE_W_ROUNDS);
  melee_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "kills");
  melee_pad(cell, sizeof(cell), MELEE_W_KILLS);
  melee_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "deaths");
  melee_pad(cell, sizeof(cell), MELEE_W_DEATHS);
  melee_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "dealt");
  melee_pad(cell, sizeof(cell), MELEE_W_DEALT);
  melee_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "taken");
  melee_pad(cell, sizeof(cell), MELEE_W_TAKEN);
  melee_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "crits");
  melee_pad(cell, sizeof(cell), MELEE_W_CRITS);
  melee_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "best");
  melee_pad(cell, sizeof(cell), MELEE_W_BEST);
  melee_cat(line, sizeof(line), cell);

  melee_cat(line, sizeof(line), CLR_RESET);
  cmd_reply(ctx, line);
}

// The podium is tinted; everyone below it shares one color. Rank is
// 1-based and arrives already ordered by damage dealt.
static void
melee_board_row(const cmd_ctx_t *ctx, uint32_t rank,
    const melee_score_row_t *row)
{
  const char *tint = (rank == 1) ? CLR_YELLOW
                   : (rank == 2) ? CLR_WHITE
                   : (rank == 3) ? CLR_ORANGE
                   :               CLR_CYAN;
  char        name[MELEE_NAME_SZ];
  char        num [32];
  char        cell[MELEE_CELL_SZ];
  char        line[MELEE_LINE_SZ];

  line[0] = '\0';
  melee_cat(line, sizeof(line), "  ");

  snprintf(cell, sizeof(cell), "%s%" PRIu32 CLR_RESET, tint, rank);
  melee_pad(cell, sizeof(cell), MELEE_W_RANK);
  melee_cat(line, sizeof(line), cell);
  melee_cat(line, sizeof(line), " ");

  melee_fit(row->name, MELEE_W_NAME - 1, name, sizeof(name));
  snprintf(cell, sizeof(cell), "%s%s" CLR_RESET, tint, name);
  melee_padr(cell, sizeof(cell), MELEE_W_NAME);
  melee_cat(line, sizeof(line), cell);

  melee_fmt_num(cell, sizeof(cell), row->rounds, MELEE_W_ROUNDS);
  melee_pad(cell, sizeof(cell), MELEE_W_ROUNDS);
  melee_cat(line, sizeof(line), cell);

  melee_fmt_num(num, sizeof(num), row->kills, MELEE_W_KILLS);
  snprintf(cell, sizeof(cell), CLR_GREEN "%s" CLR_RESET, num);
  melee_pad(cell, sizeof(cell), MELEE_W_KILLS);
  melee_cat(line, sizeof(line), cell);

  melee_fmt_num(num, sizeof(num), row->deaths, MELEE_W_DEATHS);
  snprintf(cell, sizeof(cell), CLR_RED "%s" CLR_RESET, num);
  melee_pad(cell, sizeof(cell), MELEE_W_DEATHS);
  melee_cat(line, sizeof(line), cell);

  melee_fmt_num(cell, sizeof(cell), row->dmg_given, MELEE_W_DEALT);
  melee_pad(cell, sizeof(cell), MELEE_W_DEALT);
  melee_cat(line, sizeof(line), cell);

  melee_fmt_num(cell, sizeof(cell), row->dmg_taken, MELEE_W_TAKEN);
  melee_pad(cell, sizeof(cell), MELEE_W_TAKEN);
  melee_cat(line, sizeof(line), cell);

  melee_fmt_num(cell, sizeof(cell), row->crits, MELEE_W_CRITS);
  melee_pad(cell, sizeof(cell), MELEE_W_CRITS);
  melee_cat(line, sizeof(line), cell);

  if(row->best_crit > 0)
    melee_fmt_num(cell, sizeof(cell), row->best_crit, MELEE_W_BEST);

  else
    snprintf(cell, sizeof(cell), CLR_GRAY "×" CLR_RESET);

  melee_pad(cell, sizeof(cell), MELEE_W_BEST);
  melee_cat(line, sizeof(line), cell);

  cmd_reply(ctx, line);
}

static void
melee_show_scores(const cmd_ctx_t *ctx)
{
  melee_tunables_t  t;
  melee_score_row_t rows[MELEE_MAX_SCORE_ROWS];
  userns_t         *ns;
  char              line[MELEE_LINE_SZ];
  char              rule[MELEE_LINE_SZ];
  char              by  [MELEE_USER_SZ];
  char              on  [MELEE_USER_SZ];
  int32_t           worst = 0;
  uint32_t          n;
  uint32_t          i;

  ns = userns_session_resolve(ctx);

  if(ns == NULL)          // the resolver already replied
    return;

  melee_tunables_load(&t);
  n = melee_db_scores(ns->id, t.scoreboard_rows, rows, MELEE_MAX_SCORE_ROWS);

  if(n == 0)
  {
    cmd_reply(ctx, "☠ No blood has been spilled yet.");
    return;
  }

  snprintf(line, sizeof(line),
      "☠ " CLR_BOLD "MELEE — HALL OF THE FALLEN" CLR_RESET
      CLR_GRAY " (namespace: %s)" CLR_RESET, ns->name);
  cmd_reply(ctx, line);

  melee_rule(rule, sizeof(rule), MELEE_W_BOARD);
  cmd_reply(ctx, rule);
  melee_board_header(ctx);

  for(i = 0; i < n; i++)
    melee_board_row(ctx, i + 1, &rows[i]);

  cmd_reply(ctx, rule);

  if(melee_db_deadliest(ns->id, by, sizeof(by), on, sizeof(on), &worst))
  {
    snprintf(line, sizeof(line),
        "💥 deadliest single blow: " CLR_CYAN "%s" CLR_RESET " → "
        CLR_PURPLE "%s" CLR_RESET " for " CLR_BOLD CLR_RED "%d" CLR_RESET,
        by, (on[0] != '\0') ? on : "someone", worst);
    cmd_reply(ctx, line);
  }
}

// ------------------------------------------------------------------ //
// Where the words come from                                           //
// ------------------------------------------------------------------ //

#define MELEE_W_FCAT    11   // category, left-aligned
#define MELEE_W_FPOOL    6
#define MELEE_W_FSERVED  9
#define MELEE_W_FREJ    10
#define MELEE_W_FLAV    (2 + MELEE_W_FCAT + MELEE_W_FPOOL + MELEE_W_FSERVED \
                         + MELEE_W_FREJ + 10)

static const char *const melee_flav_label[MELEE_FLAV__COUNT] = {
  "minor", "medium", "major", "critical", "deaths", "decay", "decay kill"
};

static void
melee_flav_header(const cmd_ctx_t *ctx)
{
  char line[MELEE_LINE_SZ];
  char cell[MELEE_CELL_SZ];

  snprintf(line, sizeof(line), "%s  ", CLR_GRAY);

  snprintf(cell, sizeof(cell), "category");
  melee_padr(cell, sizeof(cell), MELEE_W_FCAT);
  melee_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "pool");
  melee_pad(cell, sizeof(cell), MELEE_W_FPOOL);
  melee_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "served");
  melee_pad(cell, sizeof(cell), MELEE_W_FSERVED);
  melee_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "rejected");
  melee_pad(cell, sizeof(cell), MELEE_W_FREJ);
  melee_cat(line, sizeof(line), cell);

  melee_cat(line, sizeof(line), "  state");
  melee_cat(line, sizeof(line), CLR_RESET);
  cmd_reply(ctx, line);
}

static void
melee_flav_row(const cmd_ctx_t *ctx, melee_flavour_t cat,
    const melee_pool_stat_t *s)
{
  char line[MELEE_LINE_SZ];
  char cell[MELEE_CELL_SZ];
  char num [32];
  char state[48];

  snprintf(line, sizeof(line), "  ");

  snprintf(cell, sizeof(cell), CLR_CYAN "%s" CLR_RESET,
      melee_flav_label[cat]);
  melee_padr(cell, sizeof(cell), MELEE_W_FCAT);
  melee_cat(line, sizeof(line), cell);

  melee_fmt_num(num, sizeof(num), (int64_t)s->depth, MELEE_W_FPOOL);
  snprintf(cell, sizeof(cell), "%s%s" CLR_RESET,
      s->depth > 0 ? CLR_WHITE : CLR_GRAY, num);
  melee_pad(cell, sizeof(cell), MELEE_W_FPOOL);
  melee_cat(line, sizeof(line), cell);

  melee_fmt_num(num, sizeof(num), (int64_t)s->served, MELEE_W_FSERVED);
  snprintf(cell, sizeof(cell), "%s", num);
  melee_pad(cell, sizeof(cell), MELEE_W_FSERVED);
  melee_cat(line, sizeof(line), cell);

  melee_fmt_num(num, sizeof(num), (int64_t)s->rejected, MELEE_W_FREJ);
  snprintf(cell, sizeof(cell), "%s%s" CLR_RESET,
      s->rejected > 0 ? CLR_YELLOW : CLR_GRAY, num);
  melee_pad(cell, sizeof(cell), MELEE_W_FREJ);
  melee_cat(line, sizeof(line), cell);

  if(s->inflight)
    snprintf(state, sizeof(state), CLR_YELLOW "refilling" CLR_RESET);

  else if(s->wait > 0)
    snprintf(state, sizeof(state), CLR_RED "waiting %" PRId64 "s" CLR_RESET,
        s->wait);

  else
    snprintf(state, sizeof(state), CLR_GREEN "ready" CLR_RESET);

  melee_cat(line, sizeof(line), "  ");
  melee_cat(line, sizeof(line), state);
  cmd_reply(ctx, line);
}

// What the four damage tiers actually mean at the current tunables. The
// boundaries are not computed from the KV percentages a second time —
// they are read back out of melee_severity() itself, one damage value at
// a time, so this line can never disagree with the renderer. The ceiling
// is at most hit_max/crit_max's clamp, so the walk is free.
static void
melee_flav_bands(const cmd_ctx_t *ctx, const melee_tunables_t *t)
{
  int32_t         lo[MELEE_FLAV_DEATH] = { 0 };
  int32_t         hi[MELEE_FLAV_DEATH] = { 0 };
  int32_t         ceiling = melee_dmg_ceiling(t);
  char            line[MELEE_LINE_SZ];
  char            cell[64];
  melee_flavour_t cat;
  int32_t         dmg;

  for(dmg = 1; dmg <= ceiling; dmg++)
  {
    melee_flavour_t sev = melee_severity(t, dmg);

    if(lo[sev] == 0)
      lo[sev] = dmg;

    hi[sev] = dmg;
  }

  snprintf(line, sizeof(line), CLR_GRAY "  bands  ");

  for(cat = MELEE_FLAV_MINOR; cat < MELEE_FLAV_DEATH; cat++)
  {
    // A small ceiling leaves no room for four tiers, and an unreachable
    // band says "—" rather than the lie of "0-0".
    if(lo[cat] == 0)
      snprintf(cell, sizeof(cell), "%s%s —",
          cat != MELEE_FLAV_MINOR ? " · " : "", melee_flav_label[cat]);

    else if(lo[cat] == hi[cat])
      snprintf(cell, sizeof(cell), "%s%s %d",
          cat != MELEE_FLAV_MINOR ? " · " : "", melee_flav_label[cat],
          lo[cat]);

    else
      snprintf(cell, sizeof(cell), "%s%s %d-%d",
          cat != MELEE_FLAV_MINOR ? " · " : "", melee_flav_label[cat],
          lo[cat], hi[cat]);

    melee_cat(line, sizeof(line), cell);
  }

  melee_cat(line, sizeof(line), CLR_RESET);
  cmd_reply(ctx, line);
}

static void
melee_show_llm(const cmd_ctx_t *ctx)
{
  melee_tunables_t  t;
  melee_pool_stat_t s;
  const char       *why;
  char              line[MELEE_LINE_SZ];
  char              rule[MELEE_LINE_SZ];
  char              num [32];
  melee_flavour_t   cat;
  bool              errored = false;

  melee_tunables_load(&t);
  why = melee_llm_offreason(&t);

  snprintf(line, sizeof(line),
      "⚔ " CLR_BOLD "MELEE — FLAVOUR" CLR_RESET);
  cmd_reply(ctx, line);

  if(why != NULL)
  {
    // Collapsed view: the built-in lines are speaking, and the reader is
    // told which gate closed rather than left to guess.
    snprintf(line, sizeof(line),
        "  the pit speaks its own built-in lines " CLR_GRAY "(%s%s%s)"
        CLR_RESET, why,
        t.llm_model[0] != '\0' ? ": " : "",
        t.llm_model[0] != '\0' ? t.llm_model : "");
    cmd_reply(ctx, line);
    return;
  }

  snprintf(line, sizeof(line),
      "  source   " CLR_CYAN "%s" CLR_RESET CLR_GRAY " (chat)" CLR_RESET,
      t.llm_model);
  cmd_reply(ctx, line);

  // Two personas ship, so the path is worth stating — and worth probing.
  // An unreadable one is silently survivable (the model writes in its own
  // voice), which is exactly why it must not be silent here. access() at
  // view time is cheap; this is not a hot path.
  if(t.llm_prompt[0] == '\0')
    snprintf(line, sizeof(line),
        "  persona  " CLR_GRAY "(none — the model writes in its own voice)"
        CLR_RESET);

  else
    snprintf(line, sizeof(line), "  persona  %s %s", t.llm_prompt,
        access(t.llm_prompt, R_OK) == 0
          ? CLR_GREEN "✓ readable" CLR_RESET
          : CLR_RED   "✗ unreadable — the model writes in its own voice"
            CLR_RESET);

  cmd_reply(ctx, line);

  melee_rule(rule, sizeof(rule), MELEE_W_FLAV);
  cmd_reply(ctx, rule);
  melee_flav_header(ctx);

  for(cat = MELEE_FLAV_MINOR; cat < MELEE_FLAV__COUNT; cat++)
  {
    melee_pool_stats(cat, &s);
    melee_flav_row(ctx, cat, &s);

    if(s.last_error[0] != '\0')
      errored = true;
  }

  cmd_reply(ctx, rule);
  melee_flav_bands(ctx, &t);

  melee_fmt_num(num, sizeof(num), (int64_t)melee_pool_fallbacks(), 12);
  snprintf(line, sizeof(line),
      CLR_GRAY "fallbacks to the built-in lines: %s" CLR_RESET, num);
  cmd_reply(ctx, line);

  if(!errored)
    return;

  for(cat = MELEE_FLAV_MINOR; cat < MELEE_FLAV__COUNT; cat++)
  {
    melee_pool_stats(cat, &s);

    if(s.last_error[0] == '\0')
      continue;

    snprintf(line, sizeof(line), CLR_GRAY "%s: %.80s" CLR_RESET,
        melee_flav_label[cat], s.last_error);
    cmd_reply(ctx, line);
  }
}

// ------------------------------------------------------------------ //
// Registration                                                        //
// ------------------------------------------------------------------ //

// everyone/0, unlike `show whenmoon`: the pit is an amusement, and
// anyone watching the fight should be able to read the board. The `show`
// parent is core-provided and already exists.
bool
melee_show_register(void)
{
  if(cmd_register("melee", "melee",
        "show melee",
        "The current round in this pit.",
        "Draws the round burning in this room: every combatant's health, "
        "what they have dealt and taken, the heaviest blow struck, and "
        "who has yet to swing this wave. In a direct message, where there "
        "is no room to speak of, it shows whichever brawl in the "
        "namespace was last touched. A finished round keeps its card "
        "until the next one opens.",
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        melee_show_round, NULL, "show", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("melee", "scores",
        "show melee scores",
        "Lifetime standings for every combatant.",
        "Everyone in this namespace who has ever fought, ordered by the "
        "damage they have dealt: rounds entered, kills, deaths, damage "
        "given and taken, critical hits, and their heaviest single blow. "
        "The row count is `plugin.melee.scoreboard_rows`.",
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        melee_show_scores, NULL, "show/melee", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("melee", "llm",
        "show melee llm",
        "Where the pit's words come from.",
        "Reports whether a language model is authoring the combat "
        "flavour, which model, and how deep each category's pool of "
        "unspoken lines is. When no model is configured, or when a pool "
        "runs dry, the pit falls back to its built-in lines and this "
        "view says so.",
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        melee_show_llm, NULL, "show/melee", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
