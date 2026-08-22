// botmanager — MIT
// attack's read-only views: `show attack` draws the round burning in this
// room, `show attack scores` the lifetime standings of everyone who has
// ever swung in this namespace, `show attack classes` the character
// sheets on offer, and `show attack rules` the house rulebook.
//
// The first two are plain SELECTs and take no lock. atk_turn_lock
// serialises *writers*; a blow lands as one transaction, so the worst a
// reader can catch is the instant between two finished turns. The last
// two touch no table at all — they render the class registry and the
// tunables, and are answerable with the pit empty.

#define ATTACK_INTERNAL
#include "attack.h"

#include "colors.h"
#include "display.h"
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
// The character sheet a combatant was dealt. Shared with the class
// roster below so the two views can never disagree about how wide a
// class name is.
#define ATK_W_CLASS   12
#define ATK_W_BAR     15   // the health bar, one cell per glyph
#define ATK_W_HP       9   // "74/100"
#define ATK_W_NUM      7   // dealt, taken
// Hit points restored. Shared by both views, exactly as ATK_W_CLASS is,
// so the card and the leaderboard can never disagree about the column.
#define ATK_W_HEALED   7
#define ATK_W_CRIT     6
#define ATK_W_CARD    (2 + ATK_W_NAME + ATK_W_CLASS + ATK_W_BAR + 1 \
                         + ATK_W_HP + 2 * ATK_W_NUM + ATK_W_HEALED \
                         + ATK_W_CRIT)

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
                         + ATK_W_TAKEN + ATK_W_HEALED + ATK_W_CRITS \
                         + ATK_W_BEST)

// One column's worth of text plus its color markers.
#define ATK_CELL_SZ  128

// A name already fitted to the combatant column: at most ATK_W_NAME-1
// display columns, and a display column is at most four UTF-8 bytes.
// Sized from the geometry so the cell buffer provably swallows it.
#define ATK_NAME_SZ  ((ATK_W_NAME - 1) * 4 + 1)

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

// Hit points restored, in both views. Nought is drawn as the same gray
// `×` an unstruck critical gets rather than as a `0`: most combatants
// cannot heal at all, and a column of zeroes would read as a class
// failing at something rather than as one that never tries.
static void
atk_healed_cell(char *out, size_t cap, int64_t healed)
{
  char num[32];

  if(healed <= 0)
  {
    snprintf(out, cap, CLR_GRAY "×" CLR_RESET);
    return;
  }

  atk_fmt_num(num, sizeof(num), healed, ATK_W_HEALED);
  snprintf(out, cap, CLR_GREEN "%s" CLR_RESET, num);
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
    display_align_left(out, cap, ATK_W_BAR);
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
  display_align_left(cell, sizeof(cell), ATK_W_NAME);
  display_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "class");
  display_align_left(cell, sizeof(cell), ATK_W_CLASS);
  display_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "health");
  display_align_left(cell, sizeof(cell), ATK_W_BAR + 1);
  display_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "hp");
  display_align_right(cell, sizeof(cell), ATK_W_HP);
  display_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "dealt");
  display_align_right(cell, sizeof(cell), ATK_W_NUM);
  display_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "taken");
  display_align_right(cell, sizeof(cell), ATK_W_NUM);
  display_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "healed");
  display_align_right(cell, sizeof(cell), ATK_W_HEALED);
  display_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "crit");
  display_align_right(cell, sizeof(cell), ATK_W_CRIT);
  display_cat(line, sizeof(line), cell);

  display_cat(line, sizeof(line), CLR_RESET);
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

    display_cat(out, cap, " ");

    for(k = 0; k < marks[i].n; k++)
    {
      char cell[32];

      snprintf(cell, sizeof(cell), "%s%s" CLR_RESET,
          atk_dot_color_of(marks[i].kinds[k]),
          atk_dot_emoji_of(marks[i].kinds[k]));
      display_cat(out, cap, cell);
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
  char       type[ATK_CLASS_NAME_SZ];
  char       num [32];
  char       max [32];
  char       cell[ATK_CELL_SZ];
  char       line[ATK_LINE_SZ];

  line[0] = '\0';
  display_cat(line, sizeof(line), "  ");

  // One column of breathing room is reserved so a long name never runs
  // into the bar.
  display_fit(row->name, ATK_W_NAME - 1, name, sizeof(name), NULL);
  snprintf(cell, sizeof(cell), "%s%s" CLR_RESET,
      alive ? CLR_CYAN : CLR_GRAY, name);
  display_align_left(cell, sizeof(cell), ATK_W_NAME);
  display_cat(line, sizeof(line), cell);

  // The sheet they were dealt — a label, and never a number. A row
  // written before classes existed simply has none to show.
  display_fit(row->class, ATK_W_CLASS - 1, type, sizeof(type), NULL);
  snprintf(cell, sizeof(cell), "%s%s" CLR_RESET,
      alive ? CLR_PURPLE : CLR_GRAY,
      (type[0] != '\0') ? type : "—");
  display_align_left(cell, sizeof(cell), ATK_W_CLASS);
  display_cat(line, sizeof(line), cell);

  atk_hp_bar(cell, sizeof(cell), row->hp, row->hp_max);
  display_cat(line, sizeof(line), cell);
  display_cat(line, sizeof(line), " ");

  // Each half of "74/100" gets half the column, so even a pit tuned to
  // the six-figure ceiling still reads as "100K/100K".
  atk_fmt_num(num, sizeof(num), row->hp,     ATK_W_HP / 2);
  atk_fmt_num(max, sizeof(max), row->hp_max, ATK_W_HP / 2);
  snprintf(cell, sizeof(cell), "%s%s" CLR_RESET "/%s",
      alive ? CLR_WHITE : CLR_GRAY, num, max);
  display_align_right(cell, sizeof(cell), ATK_W_HP);
  display_cat(line, sizeof(line), cell);

  atk_fmt_num(cell, sizeof(cell), row->dmg_given, ATK_W_NUM);
  display_align_right(cell, sizeof(cell), ATK_W_NUM);
  display_cat(line, sizeof(line), cell);

  atk_fmt_num(cell, sizeof(cell), row->dmg_taken, ATK_W_NUM);
  display_align_right(cell, sizeof(cell), ATK_W_NUM);
  display_cat(line, sizeof(line), cell);

  atk_healed_cell(cell, sizeof(cell), row->heal_given);
  display_align_right(cell, sizeof(cell), ATK_W_HEALED);
  display_cat(line, sizeof(line), cell);

  if(row->best_crit > 0)
  {
    atk_fmt_num(num, sizeof(num), row->best_crit, ATK_W_CRIT);
    snprintf(cell, sizeof(cell), CLR_RED "%s" CLR_RESET, num);
  }

  else
    snprintf(cell, sizeof(cell), CLR_GRAY "×" CLR_RESET);

  display_align_right(cell, sizeof(cell), ATK_W_CRIT);
  display_cat(line, sizeof(line), cell);

  // Outside the grid, after the last padded cell: the markers are a
  // ragged tail, not a column, so no width promise is broken.
  atk_card_marks(cell, sizeof(cell), row, marks, n_marks);
  display_cat(line, sizeof(line), cell);

  // The pending deferral bonus rides in the same ragged tail, and for the
  // same reason: it is true of a minority of rows and a column of blanks
  // would cost every reader six characters of width to say nothing.
  if(row->bonus_pct > 0)
  {
    snprintf(cell, sizeof(cell),
        " " CLR_BOLD CLR_YELLOW "⚡+%d%%" CLR_RESET, row->bonus_pct);
    display_cat(line, sizeof(line), cell);
  }

  cmd_reply(ctx, line);
}

static void
atk_show_round(const cmd_ctx_t *ctx)
{
  atk_card_t     card;
  atk_card_row_t rows [ATK_MAX_PLAYERS];
  atk_dot_mark_t marks[ATK_MAX_PLAYERS];
  atk_tunables_t t;
  userns_t      *ns;
  const char    *state;
  char           line  [ATK_LINE_SZ];
  char           rule  [ATK_LINE_SZ];
  char           roster[ATK_ROSTER_SZ];
  char           dur   [UTIL_DURATION_SZ];
  int64_t        span;
  uint32_t       shown;
  uint32_t       n_marks;
  uint32_t       total = 0;
  uint32_t       i;
  bool           expired;

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

  atk_tunables_load(&t);

  // Nothing reaps a round on a timer: the row still reads ACTIVE until
  // somebody swings again, and only then does `!attack` retire it. All
  // three verbs already treat a brawl silent for longer than
  // round_max_idle_secs as over — the same comparison, in the same
  // direction — and the rulebook promises as much. A card that drew one
  // as live named combatants who owed a swing in a fight that had
  // finished.
  expired = (card.state == ATK_ROUND_ACTIVE) &&
            card.idle > (int64_t)t.round_max_idle_secs;

  state = expired                            ? " · timed out"
        : (card.state == ATK_ROUND_ACTIVE)   ? ""
        : (card.state == ATK_ROUND_ENDED)    ? " · ended"
        :                                      " · abandoned";

  // A brawl that ran out the clock ended at its last blow, not at the
  // moment somebody happened to look at it — `length` runs to NOW()
  // while ended_at is still NULL, so without this the header would set
  // "timed out" beside a duration still counting up.
  span = expired ? card.length - card.idle : card.length;

  util_fmt_duration((time_t)span, dur, sizeof(dur));
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

  // What the pit is still owed and how long it has to pay: both are
  // claims about a fight still running, so both stand or fall together.
  if(card.state == ATK_ROUND_ACTIVE && !expired)
  {
    if(atk_db_pending(card.id, card.wave, roster, sizeof(roster)) == SUCCESS &&
       roster[0] != '\0')
    {
      snprintf(line, sizeof(line),
          "⏳ yet to swing this wave: " CLR_CYAN "%s" CLR_RESET, roster);
      cmd_reply(ctx, line);
    }

    util_fmt_duration((time_t)((int64_t)t.round_max_idle_secs - card.idle),
        dur, sizeof(dur));

    snprintf(line, sizeof(line),
        "⏱ the pit goes quiet in " CLR_YELLOW "%s" CLR_RESET
        CLR_GRAY " unless somebody swings." CLR_RESET, dur);
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
  display_align_right(cell, sizeof(cell), ATK_W_RANK);
  display_cat(line, sizeof(line), cell);
  display_cat(line, sizeof(line), " ");

  snprintf(cell, sizeof(cell), "combatant");
  display_align_left(cell, sizeof(cell), ATK_W_NAME);
  display_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "rounds");
  display_align_right(cell, sizeof(cell), ATK_W_ROUNDS);
  display_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "kills");
  display_align_right(cell, sizeof(cell), ATK_W_KILLS);
  display_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "deaths");
  display_align_right(cell, sizeof(cell), ATK_W_DEATHS);
  display_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "dealt");
  display_align_right(cell, sizeof(cell), ATK_W_DEALT);
  display_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "taken");
  display_align_right(cell, sizeof(cell), ATK_W_TAKEN);
  display_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "healed");
  display_align_right(cell, sizeof(cell), ATK_W_HEALED);
  display_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "crits");
  display_align_right(cell, sizeof(cell), ATK_W_CRITS);
  display_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "best");
  display_align_right(cell, sizeof(cell), ATK_W_BEST);
  display_cat(line, sizeof(line), cell);

  display_cat(line, sizeof(line), CLR_RESET);
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
  display_cat(line, sizeof(line), "  ");

  snprintf(cell, sizeof(cell), "%s%" PRIu32 CLR_RESET, tint, rank);
  display_align_right(cell, sizeof(cell), ATK_W_RANK);
  display_cat(line, sizeof(line), cell);
  display_cat(line, sizeof(line), " ");

  display_fit(row->name, ATK_W_NAME - 1, name, sizeof(name), NULL);
  snprintf(cell, sizeof(cell), "%s%s" CLR_RESET, tint, name);
  display_align_left(cell, sizeof(cell), ATK_W_NAME);
  display_cat(line, sizeof(line), cell);

  atk_fmt_num(cell, sizeof(cell), row->rounds, ATK_W_ROUNDS);
  display_align_right(cell, sizeof(cell), ATK_W_ROUNDS);
  display_cat(line, sizeof(line), cell);

  atk_fmt_num(num, sizeof(num), row->kills, ATK_W_KILLS);
  snprintf(cell, sizeof(cell), CLR_GREEN "%s" CLR_RESET, num);
  display_align_right(cell, sizeof(cell), ATK_W_KILLS);
  display_cat(line, sizeof(line), cell);

  atk_fmt_num(num, sizeof(num), row->deaths, ATK_W_DEATHS);
  snprintf(cell, sizeof(cell), CLR_RED "%s" CLR_RESET, num);
  display_align_right(cell, sizeof(cell), ATK_W_DEATHS);
  display_cat(line, sizeof(line), cell);

  atk_fmt_num(cell, sizeof(cell), row->dmg_given, ATK_W_DEALT);
  display_align_right(cell, sizeof(cell), ATK_W_DEALT);
  display_cat(line, sizeof(line), cell);

  atk_fmt_num(cell, sizeof(cell), row->dmg_taken, ATK_W_TAKEN);
  display_align_right(cell, sizeof(cell), ATK_W_TAKEN);
  display_cat(line, sizeof(line), cell);

  atk_healed_cell(cell, sizeof(cell), row->heal_given);
  display_align_right(cell, sizeof(cell), ATK_W_HEALED);
  display_cat(line, sizeof(line), cell);

  atk_fmt_num(cell, sizeof(cell), row->crits, ATK_W_CRITS);
  display_align_right(cell, sizeof(cell), ATK_W_CRITS);
  display_cat(line, sizeof(line), cell);

  if(row->best_crit > 0)
    atk_fmt_num(cell, sizeof(cell), row->best_crit, ATK_W_BEST);

  else
    snprintf(cell, sizeof(cell), CLR_GRAY "×" CLR_RESET);

  display_align_right(cell, sizeof(cell), ATK_W_BEST);
  display_cat(line, sizeof(line), cell);

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

    display_cat(line, sizeof(line), cell);
  }

  display_cat(line, sizeof(line), CLR_RESET);
  cmd_reply(ctx, line);
}

// ------------------------------------------------------------------ //
// show attack classes — the roster of sheets                          //
// ------------------------------------------------------------------ //

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
  display_align_left(cell, sizeof(cell), ATK_W_CLASS);
  display_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "damage");
  display_align_right(cell, sizeof(cell), ATK_W_MOVES);
  display_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "decay");
  display_align_right(cell, sizeof(cell), ATK_W_DECAY);
  display_cat(line, sizeof(line), cell);

  snprintf(cell, sizeof(cell), "heal");
  display_align_right(cell, sizeof(cell), ATK_W_HEAL);
  display_cat(line, sizeof(line), cell);

  display_cat(line, sizeof(line), " description");
  display_cat(line, sizeof(line), CLR_RESET);
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
  display_cat(line, sizeof(line), "  ");

  // Fitted rather than printed straight: a stem is bounded by the
  // grammar, but the cell is what has to hold it, and one column of
  // breathing room keeps a long class name off the count beside it.
  display_fit(info->type, ATK_W_CLASS - 1, name, sizeof(name), NULL);
  display_fit(info->desc, ATK_CLASS_DESC_SZ - 1, desc, sizeof(desc), NULL);

  snprintf(cell, sizeof(cell), CLR_CYAN "%s" CLR_RESET, name);
  display_align_left(cell, sizeof(cell), ATK_W_CLASS);
  display_cat(line, sizeof(line), cell);

  atk_fmt_num(cell, sizeof(cell), info->n[ATK_SEC_DAMAGE], ATK_W_MOVES);
  display_align_right(cell, sizeof(cell), ATK_W_MOVES);
  display_cat(line, sizeof(line), cell);

  atk_fmt_num(cell, sizeof(cell), info->n[ATK_SEC_DOT], ATK_W_DECAY);
  display_align_right(cell, sizeof(cell), ATK_W_DECAY);
  display_cat(line, sizeof(line), cell);

  atk_fmt_num(cell, sizeof(cell), info->n[ATK_SEC_HEAL], ATK_W_HEAL);
  display_align_right(cell, sizeof(cell), ATK_W_HEAL);
  display_cat(line, sizeof(line), cell);

  // The description is the last cell on the row, so it is the one thing
  // that never needs padding — and the fallback says what it is.
  snprintf(cell, sizeof(cell), " %s%s", desc,
      info->builtin ? CLR_GRAY " (built-in fallback)" CLR_RESET : "");
  display_cat(line, sizeof(line), cell);

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
// show attack rules — the house rulebook                              //
// ------------------------------------------------------------------ //

// The rulebook is prose: it is read left to right rather than scanned
// down columns, so it takes the round card's width instead of
// DISPLAY_COLS. A player who has just read `show attack` should not see
// the frame move between the two. display.h sanctions a view that states
// its own total; this is that statement.
#define ATK_W_RULES  ATK_W_CARD

// Seconds in the unit a player would say them in. An idle clock is set
// in round hours or whole minutes far more often than not, and "four
// hours" reads as a rule where "14400 seconds" reads as a setting.
//
// No singular is spelled anywhere here and none can be reached: the
// clock floors at 60 seconds, and each larger unit is entered only from
// twice its own length, so every branch counts two or more of something.
static void
atk_rules_secs(char *out, size_t cap, uint32_t secs)
{
  if(secs >= 7200 && secs % 3600 == 0)
    snprintf(out, cap, "%" PRIu32 " hours", secs / 3600);

  else if(secs >= 120 && secs % 60 == 0)
    snprintf(out, cap, "%" PRIu32 " minutes", secs / 60);

  else
    snprintf(out, cap, "%" PRIu32 " seconds", secs);
}

// One stanza's heading: the glyph, the rule's name, and the words a
// player types to invoke it. `syntax` is NULL for a rule nobody invokes
// — the sweep and the clock happen TO you.
static void
atk_rules_head(const cmd_ctx_t *ctx, const char *glyph, const char *name,
    const char *syntax)
{
  char line[ATK_LINE_SZ];

  if(syntax != NULL)
    snprintf(line, sizeof(line),
        "  %s  " CLR_BOLD "%s" CLR_RESET CLR_GRAY "  ·  " CLR_RESET
        CLR_CYAN "%s" CLR_RESET, glyph, name, syntax);

  else
    snprintf(line, sizeof(line), "  %s  " CLR_BOLD "%s" CLR_RESET,
        glyph, name);

  cmd_reply(ctx, line);
}

// A stanza's body, wrapped to ATK_W_RULES and spoken one line at a time.
//
// The break is chosen on display_vis_len and never on strlen: a colour
// marker is two bytes of no width, and measuring the bytes would wrap a
// heavily coloured sentence a dozen columns early. Words are never
// split, so a single word longer than the width overhangs it — none of
// the vocabulary here comes close, and a hyphenator would be a page of
// code to prevent something that cannot happen.
static void
atk_rules_body(const cmd_ctx_t *ctx, const char *text)
{
  const char *p = text;
  char        line[ATK_LINE_SZ];
  char        word[ATK_CELL_SZ];
  size_t      used;
  bool        empty;

  strlcpy(line, "     ", sizeof(line));
  used  = display_vis_len(line);
  empty = true;

  while(*p != '\0')
  {
    const char *sp   = strchr(p, ' ');
    size_t      full = (sp != NULL) ? (size_t)(sp - p) : strlen(p);
    size_t      len  = full;
    size_t      vis;

    if(full == 0)                   // a run of spaces; nothing to place
    {
      p++;
      continue;
    }

    if(len >= sizeof(word))
      len = sizeof(word) - 1;

    memcpy(word, p, len);
    word[len] = '\0';
    vis = display_vis_len(word);

    if(!empty && used + 1 + vis > (size_t)ATK_W_RULES)
    {
      display_cat(line, sizeof(line), CLR_RESET);
      cmd_reply(ctx, line);
      strlcpy(line, "     ", sizeof(line));
      used  = display_vis_len(line);
      empty = true;
    }

    if(!empty)
    {
      display_cat(line, sizeof(line), " ");
      used++;
    }

    display_cat(line, sizeof(line), word);
    used += vis;
    empty = false;

    p += full;

    if(*p == ' ')
      p++;
  }

  if(!empty)
  {
    display_cat(line, sizeof(line), CLR_RESET);
    cmd_reply(ctx, line);
  }
}

// The pit's own rules, at the numbers it is running right now.
//
// Every figure below is lifted from the tunables this turn would use —
// nothing here is a literal, because a rulebook that has to be edited
// when a knob moves is a rulebook that will one day be wrong. The damage
// bands come from atk_flav_bands(), which reads them back out of
// atk_severity() rather than deriving them a second time.
static void
atk_show_rules(const cmd_ctx_t *ctx)
{
  atk_tunables_t t;
  char           line[ATK_LINE_SZ];
  char           rule[ATK_LINE_SZ];
  char           body[ATK_LINE_SZ * 2];
  char           clock[64];

  atk_tunables_load(&t);
  atk_rules_secs(clock, sizeof(clock), t.round_max_idle_secs);

  snprintf(line, sizeof(line),
      "📜 " CLR_BOLD "THE DUELLING PIT — HOUSE RULES" CLR_RESET
      CLR_GRAY " (as the pit is tuned right now)" CLR_RESET);
  cmd_reply(ctx, line);

  atk_rule(rule, sizeof(rule), ATK_W_RULES);
  cmd_reply(ctx, rule);

  // ---- the swing --------------------------------------------------- //

  atk_rules_head(ctx, "⚔", "THE SWING", "!attack <nick>");
  snprintf(body, sizeof(body),
      "Damage is " CLR_YELLOW "1-%" PRIu32 CLR_RESET ", rolled "
      "the same way for everybody: your class chooses the words and never "
      "the numbers, so a sheet with forty critical lines hits exactly as "
      "hard as one with a single line. You swing once per wave — when "
      "every combatant still standing has taken their turn the wave "
      "turns, and the pit comes round again. Your mark must be a "
      "registered user in the room, and it must not be you.",
      t.dmg_max);
  atk_rules_body(ctx, body);

  // ---- the mend ---------------------------------------------------- //

  atk_rules_head(ctx, "✚", "THE MEND", "!heal [nick]");
  snprintf(body, sizeof(body),
      "Only a class that knows healing may mend, and " CLR_CYAN
      "show attack classes" CLR_RESET " says which do. A minor mend "
      "restores " CLR_GREEN "%" PRIu32 "-%" PRIu32 CLR_RESET " and a "
      "major one " CLR_GREEN "%" PRIu32 "-%" PRIu32 CLR_RESET ", with "
      CLR_YELLOW "%" PRIu32 "%%" CLR_RESET " of them coming up major — "
      "the pit rolls that, not your class. Nobody is mended past the "
      CLR_GREEN "%" PRIu32 CLR_RESET " hit points they opened on, and the "
      "number announced is always the number the bar moved. Mending "
      "spends your turn exactly as a swing does: it is a trade, never a "
      "free action.",
      t.heal_minor_min, t.heal_minor_max, t.heal_major_min,
      t.heal_major_max, t.heal_major_pct, t.start_hp);
  atk_rules_body(ctx, body);

  // ---- the wait ---------------------------------------------------- //

  atk_rules_head(ctx, "⏳", "THE WAIT", "!defer");
  snprintf(body, sizeof(body),
      "Give up your turn to bank " CLR_YELLOW "%" PRIu32 "-%" PRIu32 "%%"
      CLR_RESET " on the next one. Deferrals ADD — " CLR_YELLOW "%" PRIu32
      CLR_RESET " a round, hard ceiling " CLR_YELLOW "%" PRIu32 "%%"
      CLR_RESET " — and the bonus scales the NUMBER only, so a bonused "
      "blow still speaks in the words its plain roll earned; the "
      CLR_BOLD CLR_YELLOW "⚡" CLR_RESET " on the line is what explains "
      "the size of it. It pays a mend exactly as it pays a blow. An "
      "unspent bonus dies with its round.",
      t.defer_step_lo, t.defer_step_hi, t.defer_max, t.defer_cap_pct);
  atk_rules_body(ctx, body);

  // ---- the rot ----------------------------------------------------- //

  atk_rules_head(ctx, "☣", "THE ROT", NULL);

  if(t.dot_chance_pct > 0)
    snprintf(body, sizeof(body),
        CLR_YELLOW "%" PRIu32 "%%" CLR_RESET " of swings leave a wound "
        "rather than a bruise: nothing lands at once, then " CLR_YELLOW
        "%" PRIu32 CLR_RESET " ticks " CLR_YELLOW "%" PRIu32 CLR_RESET
        " seconds apart carrying that same roll spread between them. Rot "
        "is timing and not power — an affliction and a blow of equal "
        "number cost a victim the same in the end. One combatant carries "
        CLR_YELLOW "%" PRIu32 CLR_RESET " at a time, and a swing that "
        "finds no room is spent all the same, bonus and all.",
        t.dot_chance_pct, t.dot_max_ticks, t.dot_tick_secs,
        t.dot_stack_max);

  else
    strlcpy(body, "Nothing festers here: the pit is tuned to leave no "
                  "wounds behind at all.", sizeof(body));

  atk_rules_body(ctx, body);

  // ---- the sweep --------------------------------------------------- //

  atk_rules_head(ctx, "💥", "THE SWEEP", NULL);

  if(t.aoe_pct > 0)
    snprintf(body, sizeof(body),
        CLR_YELLOW "%" PRIu32 CLR_RESET " blows in a hundred go wide and "
        "catch every combatant except the one who swung, for the same "
        "number each. It needs at least two others standing to be worth "
        "the name.", t.aoe_pct);

  else
    strlcpy(body, "No blow goes wide here: every swing lands on the one "
                  "it was aimed at.", sizeof(body));

  atk_rules_body(ctx, body);

  // ---- the end ----------------------------------------------------- //

  atk_rules_head(ctx, "☠", "THE END", "attack --end");
  snprintf(body, sizeof(body),
      "Everyone opens on " CLR_GREEN "%" PRIu32 CLR_RESET " hit points. "
      "The first to reach 0 ends the round%s A brawl left silent for "
      CLR_YELLOW "%s" CLR_RESET " is over on its own, and anyone who may "
      "swing may close one early.",
      t.start_hp,
      t.eject_on_death
          ? ", and the pit shows them the door where the room allows it."
          : ".",
      clock);
  atk_rules_body(ctx, body);

  // ---- the charter, and the numbers behind the words ---------------- //

  cmd_reply(ctx, rule);
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
        "hits exactly as hard as one with a single line. Everyone in a "
        "round is dealt a different class, and a brawl with more "
        "combatants than there are sheets starts repeating them. Sheets "
        "live in `plugin.attack.classes_path` and are re-read by `attack "
        "reload`.",
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        atk_show_classes, NULL, "show/attack", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  // No nickname collision to trade against, unlike `attack reload`: this
  // hangs off core's `show` parent, so a combatant called `rules` is as
  // attackable as anyone else.
  if(cmd_register("attack", "rules",
        "show attack rules",
        "How the pit works, at the numbers it is running now.",
        "The whole game in one card: what a swing rolls, what a mend "
        "restores, what surrendering a turn buys, how an affliction pays "
        "itself out, when a blow goes wide, and what ends a round. Every "
        "figure is read from the live tunables rather than written down, "
        "so the card cannot go stale when an operator turns a knob — the "
        "damage bands at the foot come from the same function the "
        "renderer asks, and describe the pit exactly as it will play the "
        "next blow.",
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        atk_show_rules, NULL, "show/attack", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
