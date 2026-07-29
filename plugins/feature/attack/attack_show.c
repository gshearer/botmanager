// botmanager — MIT
// attack's read-only views: `show attack` draws the round burning in this
// room, and `show attack scores` the lifetime standings of everyone who
// has ever swung in this namespace.
//
// Both are plain SELECTs and take no lock. atk_turn_lock serialises
// *writers*; a blow lands as one transaction, so the worst a reader can
// catch is the instant between two finished turns.

#define ATTACK_INTERNAL
#include "attack.h"

#include "colors.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------------ //
// Column geometry                                                     //
// ------------------------------------------------------------------ //

// Both tables are built cell by cell through the padding helpers below,
// header included, so a width only ever has to change in one place.

#define ATK_W_NAME    13   // combatant, left-aligned
#define ATK_W_BAR     15   // the health bar, one cell per glyph
#define ATK_W_HP       9   // "74/100"
#define ATK_W_NUM      7   // dealt, taken
#define ATK_W_CRIT     6
#define ATK_W_CARD    (2 + ATK_W_NAME + ATK_W_BAR + 1 + ATK_W_HP \
                         + 2 * ATK_W_NUM + ATK_W_CRIT)

#define ATK_W_RANK     3
#define ATK_W_ROUNDS   7
#define ATK_W_KILLS    6
#define ATK_W_DEATHS   7
#define ATK_W_DEALT    8
#define ATK_W_TAKEN    7
#define ATK_W_CRITS    6
#define ATK_W_BEST     6
#define ATK_W_BOARD   (2 + ATK_W_RANK + 1 + ATK_W_NAME + ATK_W_ROUNDS \
                         + ATK_W_KILLS + ATK_W_DEATHS + ATK_W_DEALT \
                         + ATK_W_TAKEN + ATK_W_CRITS + ATK_W_BEST)

// One column's worth of text plus its color markers.
#define ATK_CELL_SZ  128

// A name already fitted to the combatant column: at most ATK_W_NAME-1
// display columns, and a display column is at most four UTF-8 bytes.
// Sized from the geometry so the cell buffer provably swallows it.
#define ATK_NAME_SZ  ((ATK_W_NAME - 1) * 4 + 1)

// ------------------------------------------------------------------ //
// Alignment                                                           //
// ------------------------------------------------------------------ //

// Visible column count: an abstract color marker ("\x01" + id) is two
// bytes of zero width, and every glyph the pit draws — block, skull,
// arrow, en-dash — is a single-column code point, so counting UTF-8 lead
// bytes is the column count. Same shape as stock.c's renderer.
static size_t
atk_vis_len(const char *s)
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
atk_pad(char *buf, size_t sz, int width)
{
  size_t vis = atk_vis_len(buf);
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
atk_padr(char *buf, size_t sz, int width)
{
  size_t vis = atk_vis_len(buf);
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
atk_fit(const char *src, int cols, char *dst, size_t sz)
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
atk_cat(char *line, size_t cap, const char *cell)
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
atk_fmt_num(char *out, size_t cap, int64_t v, int width)
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
atk_rule(char *out, size_t cap, int cols)
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
atk_hp_bar(char *out, size_t cap, int32_t hp, int32_t hp_max)
{
  const char *color;
  int32_t     pct;
  int         filled;
  int         i;
  size_t      n;

  if(hp <= 0)
  {
    snprintf(out, cap, CLR_RED "☠" CLR_RESET);
    atk_padr(out, cap, ATK_W_BAR);
    return;
  }

  if(hp_max <= 0)     // a truncated row should still draw something sane
    hp_max = hp;

  pct    = (int32_t)(((int64_t)hp * 100) / hp_max);
  filled = (int)(((int64_t)hp * ATK_W_BAR) / hp_max);

  if(filled < 1)      // a survivor always shows at least one cell
    filled = 1;

  if(filled > ATK_W_BAR)
    filled = ATK_W_BAR;

  color = (pct > 60) ? CLR_GREEN : (pct >= 25) ? CLR_YELLOW : CLR_RED;

  snprintf(out, cap, "%s", color);
  n = strlen(out);

  for(i = 0; i < ATK_W_BAR && n + 4 < cap; i++)
  {
    memcpy(out + n, (i < filled) ? "█" : "░", 3);
    n += 3;
  }

  snprintf(out + n, cap - n, "%s", CLR_RESET);
}

// ------------------------------------------------------------------ //
// show attack — the round card                                        //
// ------------------------------------------------------------------ //

static void
atk_card_header(const cmd_ctx_t *ctx)
{
  char cell[ATK_CELL_SZ];
  char line[ATK_LINE_SZ];

  snprintf(line, sizeof(line), "%s  ", CLR_GRAY);

  snprintf(cell, sizeof(cell), "combatant");
  atk_padr(cell, sizeof(cell), ATK_W_NAME);
  atk_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "health");
  atk_padr(cell, sizeof(cell), ATK_W_BAR + 1);
  atk_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "hp");
  atk_pad(cell, sizeof(cell), ATK_W_HP);
  atk_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "dealt");
  atk_pad(cell, sizeof(cell), ATK_W_NUM);
  atk_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "taken");
  atk_pad(cell, sizeof(cell), ATK_W_NUM);
  atk_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "crit");
  atk_pad(cell, sizeof(cell), ATK_W_CRIT);
  atk_cat(line, sizeof(line), cell);

  atk_cat(line, sizeof(line), CLR_RESET);
  cmd_reply(ctx, line);
}

// The affliction markers a combatant carries, colorized, or an empty
// string when they carry none. Each glyph is one display column, so the
// card grows by exactly (1 + count) columns on the widest afflicted row.
static void
atk_card_marks(char *out, size_t cap, const atk_card_row_t *row,
    const atk_dot_mark_t *marks, uint32_t n)
{
  uint32_t i;
  uint32_t k;

  out[0] = '\0';

  for(i = 0; i < n; i++)
  {
    if(strcmp(marks[i].victim, row->user) != 0)
      continue;

    atk_cat(out, cap, " ");

    for(k = 0; k < marks[i].n; k++)
    {
      char cell[32];

      snprintf(cell, sizeof(cell), "%s%s" CLR_RESET,
          atk_dot_color_of(marks[i].kinds[k]),
          atk_dot_emoji_of(marks[i].kinds[k]));
      atk_cat(out, cap, cell);
    }

    return;
  }
}

static void
atk_card_row(const cmd_ctx_t *ctx, const atk_card_row_t *row,
    const atk_dot_mark_t *marks, uint32_t n_marks)
{
  const bool alive = (row->hp > 0);
  char       name[ATK_NAME_SZ];
  char       num [32];
  char       max [32];
  char       cell[ATK_CELL_SZ];
  char       line[ATK_LINE_SZ];

  line[0] = '\0';
  atk_cat(line, sizeof(line), "  ");

  // One column of breathing room is reserved so a long name never runs
  // into the bar.
  atk_fit(row->name, ATK_W_NAME - 1, name, sizeof(name));
  snprintf(cell, sizeof(cell), "%s%s" CLR_RESET,
      alive ? CLR_CYAN : CLR_GRAY, name);
  atk_padr(cell, sizeof(cell), ATK_W_NAME);
  atk_cat(line, sizeof(line), cell);

  atk_hp_bar(cell, sizeof(cell), row->hp, row->hp_max);
  atk_cat(line, sizeof(line), cell);
  atk_cat(line, sizeof(line), " ");

  // Each half of "74/100" gets half the column, so even a pit tuned to
  // the six-figure ceiling still reads as "100K/100K".
  atk_fmt_num(num, sizeof(num), row->hp,     ATK_W_HP / 2);
  atk_fmt_num(max, sizeof(max), row->hp_max, ATK_W_HP / 2);
  snprintf(cell, sizeof(cell), "%s%s" CLR_RESET "/%s",
      alive ? CLR_WHITE : CLR_GRAY, num, max);
  atk_pad(cell, sizeof(cell), ATK_W_HP);
  atk_cat(line, sizeof(line), cell);

  atk_fmt_num(cell, sizeof(cell), row->dmg_given, ATK_W_NUM);
  atk_pad(cell, sizeof(cell), ATK_W_NUM);
  atk_cat(line, sizeof(line), cell);

  atk_fmt_num(cell, sizeof(cell), row->dmg_taken, ATK_W_NUM);
  atk_pad(cell, sizeof(cell), ATK_W_NUM);
  atk_cat(line, sizeof(line), cell);

  if(row->best_crit > 0)
  {
    atk_fmt_num(num, sizeof(num), row->best_crit, ATK_W_CRIT);
    snprintf(cell, sizeof(cell), CLR_RED "%s" CLR_RESET, num);
  }

  else
    snprintf(cell, sizeof(cell), CLR_GRAY "×" CLR_RESET);

  atk_pad(cell, sizeof(cell), ATK_W_CRIT);
  atk_cat(line, sizeof(line), cell);

  // Outside the grid, after the last padded cell: the markers are a
  // ragged tail, not a column, so no width promise is broken.
  atk_card_marks(cell, sizeof(cell), row, marks, n_marks);
  atk_cat(line, sizeof(line), cell);

  cmd_reply(ctx, line);
}

static void
atk_show_round(const cmd_ctx_t *ctx)
{
  atk_card_t     card;
  atk_card_row_t rows [ATK_MAX_PLAYERS];
  atk_dot_mark_t marks[ATK_MAX_PLAYERS];
  userns_t      *ns;
  const char    *state;
  char           line  [ATK_LINE_SZ];
  char           rule  [ATK_LINE_SZ];
  char           roster[ATK_ROSTER_SZ];
  char           dur   [32];
  uint32_t       shown;
  uint32_t       n_marks;
  uint32_t       total = 0;
  uint32_t       i;

  ns = userns_session_resolve(ctx);

  if(ns == NULL)          // the resolver already replied
    return;

  // An empty channel is a direct message, which names no room: the
  // lookup widens to the namespace and reports whatever fought last.
  if(!atk_db_card_find(ns->id, method_inst_name(ctx->msg->inst),
        ctx->msg->channel, &card))
  {
    cmd_reply(ctx, "☠ The pit is quiet. Open one with !attack <nick>.");
    return;
  }

  state = (card.state == ATK_ROUND_ACTIVE)  ? ""
        : (card.state == ATK_ROUND_ENDED)   ? " · ended"
        :                                       " · abandoned";

  util_fmt_duration((time_t)card.length, dur, sizeof(dur));
  atk_rule(rule, sizeof(rule), ATK_W_CARD);

  snprintf(line, sizeof(line),
      "⚔ " CLR_BOLD "ATTACK" CLR_RESET CLR_GRAY " — " CLR_RESET
      CLR_CYAN "%s" CLR_RESET
      CLR_GRAY " · wave %d · %d %s · %s%s" CLR_RESET,
      (card.channel[0] != '\0') ? card.channel : "the pit",
      card.wave, card.blows, (card.blows == 1) ? "blow" : "blows",
      dur, state);
  cmd_reply(ctx, line);

  cmd_reply(ctx, rule);
  atk_card_header(ctx);

  shown = atk_db_card_roster(card.id, rows, ATK_MAX_PLAYERS, &total);

  // Lock-free like everything else in this view, and optional: a failed
  // marker query draws a card without markers rather than no card.
  n_marks = atk_db_dot_marks(card.id, marks, ATK_MAX_PLAYERS);

  for(i = 0; i < shown; i++)
    atk_card_row(ctx, &rows[i], marks, n_marks);

  cmd_reply(ctx, rule);

  if(total > shown)
  {
    snprintf(line, sizeof(line),
        CLR_GRAY "… and %" PRIu32 " more in the press." CLR_RESET,
        total - shown);
    cmd_reply(ctx, line);
  }

  if(card.state == ATK_ROUND_ENDED && card.slayer[0] != '\0')
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

  if(card.state == ATK_ROUND_ACTIVE &&
     atk_db_pending(card.id, card.wave, roster, sizeof(roster)) == SUCCESS &&
     roster[0] != '\0')
  {
    snprintf(line, sizeof(line),
        "⏳ yet to swing this wave: " CLR_CYAN "%s" CLR_RESET, roster);
    cmd_reply(ctx, line);
  }
}

// ------------------------------------------------------------------ //
// show attack scores — the leaderboard                                //
// ------------------------------------------------------------------ //

static void
atk_board_header(const cmd_ctx_t *ctx)
{
  char cell[ATK_CELL_SZ];
  char line[ATK_LINE_SZ];

  snprintf(line, sizeof(line), "%s  ", CLR_GRAY);

  snprintf(cell, sizeof(cell), "#");
  atk_pad(cell, sizeof(cell), ATK_W_RANK);
  atk_cat(line, sizeof(line), cell);
  atk_cat(line, sizeof(line), " ");

  snprintf(cell, sizeof(cell), "combatant");
  atk_padr(cell, sizeof(cell), ATK_W_NAME);
  atk_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "rounds");
  atk_pad(cell, sizeof(cell), ATK_W_ROUNDS);
  atk_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "kills");
  atk_pad(cell, sizeof(cell), ATK_W_KILLS);
  atk_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "deaths");
  atk_pad(cell, sizeof(cell), ATK_W_DEATHS);
  atk_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "dealt");
  atk_pad(cell, sizeof(cell), ATK_W_DEALT);
  atk_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "taken");
  atk_pad(cell, sizeof(cell), ATK_W_TAKEN);
  atk_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "crits");
  atk_pad(cell, sizeof(cell), ATK_W_CRITS);
  atk_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "best");
  atk_pad(cell, sizeof(cell), ATK_W_BEST);
  atk_cat(line, sizeof(line), cell);

  atk_cat(line, sizeof(line), CLR_RESET);
  cmd_reply(ctx, line);
}

// The podium is tinted; everyone below it shares one color. Rank is
// 1-based and arrives already ordered by damage dealt.
static void
atk_board_row(const cmd_ctx_t *ctx, uint32_t rank,
    const atk_score_row_t *row)
{
  const char *tint = (rank == 1) ? CLR_YELLOW
                   : (rank == 2) ? CLR_WHITE
                   : (rank == 3) ? CLR_ORANGE
                   :               CLR_CYAN;
  char        name[ATK_NAME_SZ];
  char        num [32];
  char        cell[ATK_CELL_SZ];
  char        line[ATK_LINE_SZ];

  line[0] = '\0';
  atk_cat(line, sizeof(line), "  ");

  snprintf(cell, sizeof(cell), "%s%" PRIu32 CLR_RESET, tint, rank);
  atk_pad(cell, sizeof(cell), ATK_W_RANK);
  atk_cat(line, sizeof(line), cell);
  atk_cat(line, sizeof(line), " ");

  atk_fit(row->name, ATK_W_NAME - 1, name, sizeof(name));
  snprintf(cell, sizeof(cell), "%s%s" CLR_RESET, tint, name);
  atk_padr(cell, sizeof(cell), ATK_W_NAME);
  atk_cat(line, sizeof(line), cell);

  atk_fmt_num(cell, sizeof(cell), row->rounds, ATK_W_ROUNDS);
  atk_pad(cell, sizeof(cell), ATK_W_ROUNDS);
  atk_cat(line, sizeof(line), cell);

  atk_fmt_num(num, sizeof(num), row->kills, ATK_W_KILLS);
  snprintf(cell, sizeof(cell), CLR_GREEN "%s" CLR_RESET, num);
  atk_pad(cell, sizeof(cell), ATK_W_KILLS);
  atk_cat(line, sizeof(line), cell);

  atk_fmt_num(num, sizeof(num), row->deaths, ATK_W_DEATHS);
  snprintf(cell, sizeof(cell), CLR_RED "%s" CLR_RESET, num);
  atk_pad(cell, sizeof(cell), ATK_W_DEATHS);
  atk_cat(line, sizeof(line), cell);

  atk_fmt_num(cell, sizeof(cell), row->dmg_given, ATK_W_DEALT);
  atk_pad(cell, sizeof(cell), ATK_W_DEALT);
  atk_cat(line, sizeof(line), cell);

  atk_fmt_num(cell, sizeof(cell), row->dmg_taken, ATK_W_TAKEN);
  atk_pad(cell, sizeof(cell), ATK_W_TAKEN);
  atk_cat(line, sizeof(line), cell);

  atk_fmt_num(cell, sizeof(cell), row->crits, ATK_W_CRITS);
  atk_pad(cell, sizeof(cell), ATK_W_CRITS);
  atk_cat(line, sizeof(line), cell);

  if(row->best_crit > 0)
    atk_fmt_num(cell, sizeof(cell), row->best_crit, ATK_W_BEST);

  else
    snprintf(cell, sizeof(cell), CLR_GRAY "×" CLR_RESET);

  atk_pad(cell, sizeof(cell), ATK_W_BEST);
  atk_cat(line, sizeof(line), cell);

  cmd_reply(ctx, line);
}

static void
atk_show_scores(const cmd_ctx_t *ctx)
{
  atk_tunables_t  t;
  atk_score_row_t rows[ATK_MAX_SCORE_ROWS];
  userns_t       *ns;
  char            line[ATK_LINE_SZ];
  char            rule[ATK_LINE_SZ];
  char            by  [ATK_USER_SZ];
  char            on  [ATK_USER_SZ];
  int32_t         worst = 0;
  uint32_t        n;
  uint32_t        i;

  ns = userns_session_resolve(ctx);

  if(ns == NULL)          // the resolver already replied
    return;

  atk_tunables_load(&t);
  n = atk_db_scores(ns->id, t.scoreboard_rows, rows, ATK_MAX_SCORE_ROWS);

  if(n == 0)
  {
    cmd_reply(ctx, "☠ No blood has been spilled yet.");
    return;
  }

  snprintf(line, sizeof(line),
      "☠ " CLR_BOLD "ATTACK — HALL OF THE FALLEN" CLR_RESET
      CLR_GRAY " (namespace: %s)" CLR_RESET, ns->name);
  cmd_reply(ctx, line);

  atk_rule(rule, sizeof(rule), ATK_W_BOARD);
  cmd_reply(ctx, rule);
  atk_board_header(ctx);

  for(i = 0; i < n; i++)
    atk_board_row(ctx, i + 1, &rows[i]);

  cmd_reply(ctx, rule);

  if(atk_db_deadliest(ns->id, by, sizeof(by), on, sizeof(on), &worst))
  {
    snprintf(line, sizeof(line),
        "💥 deadliest single blow: " CLR_CYAN "%s" CLR_RESET " → "
        CLR_PURPLE "%s" CLR_RESET " for " CLR_BOLD CLR_RED "%d" CLR_RESET,
        by, (on[0] != '\0') ? on : "someone", worst);
    cmd_reply(ctx, line);
  }
}

// ------------------------------------------------------------------ //
// The damage bands                                                    //
// ------------------------------------------------------------------ //

static const char *const atk_flav_label[ATK_FLAV__COUNT] = {
  "minor", "medium", "major", "critical", "deaths", "decay", "decay kill"
};

// What the four damage tiers actually mean at the current tunables. The
// boundaries are not computed from the KV percentages a second time —
// they are read back out of atk_severity() itself, one damage value at
// a time, so this line can never disagree with the renderer. The ceiling
// is clamped, so the walk is free.
void
atk_flav_bands(const cmd_ctx_t *ctx, const atk_tunables_t *t)
{
  int32_t       lo[ATK_FLAV_DEATH] = { 0 };
  int32_t       hi[ATK_FLAV_DEATH] = { 0 };
  int32_t       ceiling = atk_dmg_ceiling(t);
  char          line[ATK_LINE_SZ];
  char          cell[64];
  atk_flavour_t cat;
  int32_t       dmg;

  for(dmg = 1; dmg <= ceiling; dmg++)
  {
    atk_flavour_t sev = atk_severity(t, dmg);

    if(lo[sev] == 0)
      lo[sev] = dmg;

    hi[sev] = dmg;
  }

  snprintf(line, sizeof(line), CLR_GRAY "  bands  ");

  for(cat = ATK_FLAV_MINOR; cat < ATK_FLAV_DEATH; cat++)
  {
    // A small ceiling leaves no room for four tiers, and an unreachable
    // band says "—" rather than the lie of "0-0".
    if(lo[cat] == 0)
      snprintf(cell, sizeof(cell), "%s%s —",
          cat != ATK_FLAV_MINOR ? " · " : "", atk_flav_label[cat]);

    else if(lo[cat] == hi[cat])
      snprintf(cell, sizeof(cell), "%s%s %d",
          cat != ATK_FLAV_MINOR ? " · " : "", atk_flav_label[cat],
          lo[cat]);

    else
      snprintf(cell, sizeof(cell), "%s%s %d-%d",
          cat != ATK_FLAV_MINOR ? " · " : "", atk_flav_label[cat],
          lo[cat], hi[cat]);

    atk_cat(line, sizeof(line), cell);
  }

  atk_cat(line, sizeof(line), CLR_RESET);
  cmd_reply(ctx, line);
}

// ------------------------------------------------------------------ //
// show attack classes — the roster of sheets                          //
// ------------------------------------------------------------------ //

#define ATK_W_CLASS   12
#define ATK_W_MOVES    8
#define ATK_W_DECAY    7
#define ATK_W_HEAL     6
#define ATK_W_CLASSES (2 + ATK_W_CLASS + ATK_W_MOVES + ATK_W_DECAY \
                         + ATK_W_HEAL + 1 + 40)

static void
atk_classes_header(const cmd_ctx_t *ctx)
{
  char cell[ATK_CELL_SZ];
  char line[ATK_LINE_SZ];

  snprintf(line, sizeof(line), "%s  ", CLR_GRAY);

  snprintf(cell, sizeof(cell), "class");
  atk_padr(cell, sizeof(cell), ATK_W_CLASS);
  atk_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "damage");
  atk_pad(cell, sizeof(cell), ATK_W_MOVES);
  atk_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "decay");
  atk_pad(cell, sizeof(cell), ATK_W_DECAY);
  atk_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "heal");
  atk_pad(cell, sizeof(cell), ATK_W_HEAL);
  atk_cat(line, sizeof(line), cell);

  atk_cat(line, sizeof(line), " description");
  atk_cat(line, sizeof(line), CLR_RESET);
  cmd_reply(ctx, line);
}

// `decay` counts the afflictions a class can INFLICT, not the lines it
// has for their ticks: what a reader wants to know is whether this class
// leaves anything behind at all.
static void
atk_classes_row(const cmd_ctx_t *ctx, const atk_class_info_t *info)
{
  char name[ATK_CLASS_NAME_SZ];
  char desc[ATK_CLASS_DESC_SZ];
  char cell[ATK_CELL_SZ];
  char line[ATK_LINE_SZ];

  line[0] = '\0';
  atk_cat(line, sizeof(line), "  ");

  // Fitted rather than printed straight: a stem is bounded by the
  // grammar, but the cell is what has to hold it, and one column of
  // breathing room keeps a long class name off the count beside it.
  atk_fit(info->type, ATK_W_CLASS - 1, name, sizeof(name));
  atk_fit(info->desc, ATK_CLASS_DESC_SZ - 1, desc, sizeof(desc));

  snprintf(cell, sizeof(cell), CLR_CYAN "%s" CLR_RESET, name);
  atk_padr(cell, sizeof(cell), ATK_W_CLASS);
  atk_cat(line, sizeof(line), cell);

  atk_fmt_num(cell, sizeof(cell), info->n[ATK_SEC_DAMAGE], ATK_W_MOVES);
  atk_pad(cell, sizeof(cell), ATK_W_MOVES);
  atk_cat(line, sizeof(line), cell);

  atk_fmt_num(cell, sizeof(cell), info->n[ATK_SEC_DOT], ATK_W_DECAY);
  atk_pad(cell, sizeof(cell), ATK_W_DECAY);
  atk_cat(line, sizeof(line), cell);

  atk_fmt_num(cell, sizeof(cell), info->n[ATK_SEC_HEAL], ATK_W_HEAL);
  atk_pad(cell, sizeof(cell), ATK_W_HEAL);
  atk_cat(line, sizeof(line), cell);

  // The description is the last cell on the row, so it is the one thing
  // that never needs padding — and the fallback says what it is.
  snprintf(cell, sizeof(cell), " %s%s", desc,
      info->builtin ? CLR_GRAY " (built-in fallback)" CLR_RESET : "");
  atk_cat(line, sizeof(line), cell);

  cmd_reply(ctx, line);
}

static void
atk_show_classes(const cmd_ctx_t *ctx)
{
  atk_tunables_t   t;
  atk_class_info_t info[ATK_CLASSES_MAX];
  char             line[ATK_LINE_SZ];
  char             rule[ATK_LINE_SZ];
  uint32_t         n;
  uint32_t         i;

  atk_tunables_load(&t);
  n = atk_class_list(info, ATK_CLASSES_MAX);

  if(n == 0)
  {
    cmd_reply(ctx, "☠ No character classes are loaded.");
    return;
  }

  snprintf(line, sizeof(line),
      "🎭 " CLR_BOLD "ATTACK — CHARACTER CLASSES" CLR_RESET
      CLR_GRAY " (%s)" CLR_RESET, t.classes_path);
  cmd_reply(ctx, line);

  atk_rule(rule, sizeof(rule), ATK_W_CLASSES);
  cmd_reply(ctx, rule);
  atk_classes_header(ctx);

  for(i = 0; i < n; i++)
    atk_classes_row(ctx, &info[i]);

  cmd_reply(ctx, rule);

  // The charter, in one line, right under the evidence for it. A reader
  // asking what a class's `critical` moves are worth gets the bands from
  // the renderer itself — never re-derived from the percentages.
  atk_flav_bands(ctx, &t);
  cmd_reply(ctx, CLR_GRAY
      "  every class rolls the same numbers; a sheet supplies only the "
      "words." CLR_RESET);
}

// ------------------------------------------------------------------ //
// Registration                                                        //
// ------------------------------------------------------------------ //

// everyone/0, unlike `show whenmoon`: the pit is an amusement, and
// anyone watching the fight should be able to read the board. The `show`
// parent is core-provided and already exists.
bool
atk_show_register(void)
{
  if(cmd_register("attack", "attack",
        "show attack",
        "The current round in this pit.",
        "Draws the round burning in this room: every combatant's health, "
        "what they have dealt and taken, the heaviest blow struck, and "
        "who has yet to swing this wave. In a direct message, where there "
        "is no room to speak of, it shows whichever brawl in the "
        "namespace was last touched. A finished round keeps its card "
        "until the next one opens.",
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        atk_show_round, NULL, "show", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("attack", "scores",
        "show attack scores",
        "Lifetime standings for every combatant.",
        "Everyone in this namespace who has ever fought, ordered by the "
        "damage they have dealt: rounds entered, kills, deaths, damage "
        "given and taken, critical hits, and their heaviest single blow. "
        "The row count is `plugin.attack.scoreboard_rows`.",
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        atk_show_scores, NULL, "show/attack", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("attack", "classes",
        "show attack classes",
        "The character classes a combatant may be dealt.",
        "One row per loaded character sheet: how many damage moves it "
        "carries, how many afflictions it can inflict, how many heals it "
        "knows, and what it is. Below the table, what the four damage "
        "tiers actually pay at the current tunables. A class changes "
        "only the WORDS the pit speaks — the engine rolls the same "
        "numbers for everybody, and a sheet with forty critical lines "
        "hits exactly as hard as one with a single line. Sheets live in "
        "`plugin.attack.classes_path` and are re-read by `attack "
        "reload`.",
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        atk_show_classes, NULL, "show/attack", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
