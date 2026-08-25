// botmanager — MIT
// featreq read surface: `show feature` draws the board as a table, and
// `show feature <id>` opens one request as a card. Both are built cell
// by cell through include/display.h, so a column width lives in exactly
// one place and a nickname full of multi-byte glyphs still lines up.

#define FEATREQ_INTERNAL
#include "featreq.h"

#include "colors.h"
#include "display.h"
#include "kv.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// ------------------------------------------------------------------ //
// Column geometry                                                     //
// ------------------------------------------------------------------ //

// Header and rows are drawn through the same widths, so the two cannot
// drift. No cell carries an emoji: display.h counts a code point as one
// column and most emoji occupy two, which would shear the grid.
//
// The columns are measured, not declared: every row is sized to the
// widest thing actually in THIS result — its header label included —
// and the description takes every column the others did not need. A
// board of `doc` on `hedgehogg` spends twelve columns on identity where
// a fixed grid reserved twenty-three for names that might have been
// there and were not, and the description is where those twelve go.
//
// Everything is left-justified for the same reason: a right-aligned
// cell buys its alignment with a gap on the left, and there is no gap
// here to spare. Columns are separated by exactly one space, so the
// widths and FR_TABLE_GAPS below are the whole of the arithmetic.
//
// The ceilings bound only the two open-ended columns plus the two that
// grow with the clock and the row count; type and status need none,
// their vocabulary being closed and short. Because the ceilings sum to
// less than DISPLAY_COLS, the description can never be squeezed to
// nothing and there is no floor to enforce.
#define FR_MAX_ID      8
#define FR_MAX_USER   16
#define FR_MAX_BOT    14
#define FR_MAX_AGE     8

// Six single-space separators between seven columns.
#define FR_TABLE_GAPS  6

typedef struct
{
  int id;
  int type;
  int status;
  int user;
  int bot;
  int age;
  int desc;
} fr_cols_t;

// The card frames to the same width as the table so the two views read
// as one system. What differs is the treatment, not the measure: the
// card is the view that shows a description WHOLE — recognising a
// request is the table's job, reading it is the card's — so nothing
// here truncates and fr_wrap() folds instead, inside a margin that
// keeps prose off the frame.
#define FR_W_CARD    DISPLAY_COLS
#define FR_W_WRAP   (DISPLAY_COLS - 4)

// The card's label gutter — "  %-8s " — and the widest the username on
// its provenance line may grow before the rest of that line starts
// paying for it. Three lines share the gutter and the provenance line
// budgets the remaining frame from it.
#define FR_W_CARD_LABEL   8
#define FR_W_CARD_GUTTER (2 + FR_W_CARD_LABEL + 1)
#define FR_W_CARD_USER    24

// A cell's text before its colour markers. No column can be wider than
// the table itself, so DISPLAY_COLS at four bytes to a display column
// bounds every one, plus the ellipsis display_fit reserves out of the
// same buffer. FR_CELL_SZ is that cell once it is wearing its colour:
// a two-byte marker in front and a two-byte reset behind.
#define FR_FIT_SZ    (DISPLAY_COLS * 4 + 8)
#define FR_CELL_SZ   (FR_FIT_SZ + 8)

// A dated line's buffers. Each is sized for what it can actually hold —
// "2026-08-21 22:22", and UTIL_DURATION_SZ for the age — because slack
// in an intermediate is not free here: the compiler prices an snprintf
// by its arguments' DECLARED sizes, so a 64-byte date and a 32-byte age
// joined into a stamp read as a possible truncation of a line that in
// practice runs to forty.
#define FR_DATE_SZ   24
#define FR_STAMP_SZ  96

// ------------------------------------------------------------------ //
// Knobs                                                               //
// ------------------------------------------------------------------ //

static uint32_t
fr_list_rows(void)
{
  uint64_t n = kv_get_uint_or_default(FR_KV_LIST_ROWS);

  // A ceiling, not a second default: past FR_LIST_ROWS_MAX the reply
  // stops being a table and becomes a flood on whatever method asked.
  return((n > FR_LIST_ROWS_MAX) ? FR_LIST_ROWS_MAX : (uint32_t)n);
}

// ------------------------------------------------------------------ //
// Small renderers                                                     //
// ------------------------------------------------------------------ //

static void
fr_rule(char *out, size_t cap, int cols)
{
  size_t n;
  int    i;

  n = strlcpy(out, CLR_GRAY, cap);

  for(i = 0; i < cols && n + 4 < cap; i++)
  {
    memcpy(out + n, "─", 3);
    n += 3;
  }

  strlcpy(out + n, CLR_RESET, cap - n);
}

// One cell of the result as plain text, never NULL. Both the measuring
// pass and the render read through this, so neither can disagree with
// the other about what an absent cell looks like.
static const char *
fr_cell_text(const db_result_t *res, uint32_t row, uint32_t col)
{
  const char *s = db_result_get(res, row, col);

  return((s != NULL && s[0] != '\0') ? s : "—");
}

// How long ago, in the compact form the rest of the tree uses. Plain
// text, because it is measured before it is coloured.
static void
fr_age_text(char *out, size_t cap, time_t then, time_t now)
{
  if(then <= 0)
  {
    strlcpy(out, "—", cap);
    return;
  }

  util_fmt_duration((now > then) ? now - then : 0, out, cap);
}

// "2026-08-20 22:40  (10h55m ago)", the form both dated lines of the
// card carry. Local time, because the operator reading the board is
// the one who has to remember when they were sitting here; an absent
// stamp is an em dash and no age at all.
static void
fr_stamp_text(char *out, size_t cap, time_t when, time_t now)
{
  struct tm tm;
  char      date[FR_DATE_SZ];
  char      age [UTIL_DURATION_SZ];

  if(when <= 0 || localtime_r(&when, &tm) == NULL)
  {
    strlcpy(out, "—", cap);
    return;
  }

  strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &tm);
  util_fmt_duration((now > when) ? now - when : 0, age, sizeof(age));

  snprintf(out, cap, "%s" CLR_GRAY "  (%s ago)" CLR_RESET, date, age);
}

// A colorized cell holding at most `width` columns of `text`.
//
// display_fit appends its mark OUTSIDE the column budget, so a cell
// that will carry one has to be budgeted a column short — and a cell
// that will not must NOT be, or a value that fits exactly loses its
// last character to an ellipsis it never needed. `hedgehogg` in a bot
// column measured at nine is the case that catches it, and it is why
// the length is tested rather than the budget simply lowered.
static void
fr_text_cell(char *out, size_t cap, const char *color, const char *text,
    int width)
{
  char fitted[FR_FIT_SZ];

  if((int)display_vis_len(text) <= width)
    strlcpy(fitted, text, sizeof(fitted));

  else
    display_fit(text, width - 1, fitted, sizeof(fitted), "…");

  snprintf(out, cap, "%s%s" CLR_RESET, color, fitted);
}

// ------------------------------------------------------------------ //
// The table                                                           //
// ------------------------------------------------------------------ //

// Widen `*w` to hold `s`, up to `ceiling`. A ceiling of 0 is no
// ceiling — the closed vocabularies need none.
static void
fr_col_fit(int *w, const char *s, int ceiling)
{
  int n = (int)display_vis_len(s);

  if(ceiling > 0 && n > ceiling)
    n = ceiling;

  if(n > *w)
    *w = n;
}

// Size every column to the widest thing in this result, header labels
// included, and hand the description what is left. Called once per
// table, before a single row is drawn.
static void
fr_cols_measure(const db_result_t *res, time_t now, fr_cols_t *w)
{
  uint32_t i;

  // The labels are the floor: a column is never narrower than its own
  // heading, however short the values under it.
  w->id     = (int)display_vis_len("id");
  w->type   = (int)display_vis_len("type");
  w->status = (int)display_vis_len("status");
  w->user   = (int)display_vis_len("from");
  w->bot    = (int)display_vis_len("bot");
  w->age    = (int)display_vis_len("age");

  for(i = 0; i < res->rows; i++)
  {
    fr_type_t   type   = FR_TYPE_FEAT;
    fr_status_t status = FR_ST_NEW;
    char        buf[FR_WORD_SZ];
    char        age[UTIL_DURATION_SZ];

    snprintf(buf, sizeof(buf), "#%" PRId64,
        db_result_get_i64(res, i, FR_COL_ID, 0));
    fr_col_fit(&w->id, buf, FR_MAX_ID);

    db_result_copy(buf, sizeof(buf), res, i, FR_COL_TYPE);
    (void)fr_type_parse(buf, &type);
    fr_col_fit(&w->type, fr_type_word(type), 0);

    db_result_copy(buf, sizeof(buf), res, i, FR_COL_STATUS);
    (void)fr_status_parse(buf, &status);
    fr_col_fit(&w->status, fr_status_word(status), 0);

    fr_col_fit(&w->user, fr_cell_text(res, i, FR_COL_USER), FR_MAX_USER);
    fr_col_fit(&w->bot,  fr_cell_text(res, i, FR_COL_BOT),  FR_MAX_BOT);

    fr_age_text(age, sizeof(age),
        (time_t)db_result_get_i64(res, i, FR_COL_CREATED, 0), now);
    fr_col_fit(&w->age, age, FR_MAX_AGE);
  }

  w->desc = DISPLAY_COLS - FR_TABLE_GAPS - w->id - w->type - w->status
      - w->user - w->bot - w->age;
}

// One padded, left-justified header cell, then the separator.
static void
fr_hdr_cell(char *line, size_t cap, const char *label, int width, bool last)
{
  char cell[FR_CELL_SZ];

  strlcpy(cell, label, sizeof(cell));

  if(!last)
    display_align_left(cell, sizeof(cell), width);

  display_cat(line, cap, cell);

  if(!last)
    display_cat(line, cap, " ");
}

static void
fr_table_header(const cmd_ctx_t *ctx, const fr_cols_t *w)
{
  char line[FR_LINE_SZ];

  strlcpy(line, CLR_GRAY, sizeof(line));

  fr_hdr_cell(line, sizeof(line), "id",     w->id,     false);
  fr_hdr_cell(line, sizeof(line), "type",   w->type,   false);
  fr_hdr_cell(line, sizeof(line), "status", w->status, false);
  fr_hdr_cell(line, sizeof(line), "from",   w->user,   false);
  fr_hdr_cell(line, sizeof(line), "bot",    w->bot,    false);
  fr_hdr_cell(line, sizeof(line), "age",    w->age,    false);
  fr_hdr_cell(line, sizeof(line), "description", w->desc, true);

  display_cat(line, sizeof(line), CLR_RESET);
  cmd_reply(ctx, line);
}

// Append one already-colorized cell, padded and followed by the
// separator. The last column of a row is neither: trailing blanks help
// nobody read it.
static void
fr_row_cell(char *line, size_t cap, char *cell, size_t cell_cap, int width,
    bool last)
{
  if(!last)
  {
    display_align_left(cell, cell_cap, width);
    display_cat(line, cap, cell);
    display_cat(line, cap, " ");
    return;
  }

  display_cat(line, cap, cell);
}

static void
fr_table_row(const cmd_ctx_t *ctx, const db_result_t *res, uint32_t row,
    time_t now, const fr_cols_t *w)
{
  fr_type_t   type   = FR_TYPE_FEAT;
  fr_status_t status = FR_ST_NEW;
  char        word[FR_WORD_SZ];
  char        age [UTIL_DURATION_SZ];
  char        cell[FR_CELL_SZ];
  char        line[FR_LINE_SZ];

  db_result_copy(word, sizeof(word), res, row, FR_COL_TYPE);
  (void)fr_type_parse(word, &type);

  db_result_copy(word, sizeof(word), res, row, FR_COL_STATUS);
  (void)fr_status_parse(word, &status);

  line[0] = '\0';

  snprintf(cell, sizeof(cell), CLR_BOLD "#%" PRId64 CLR_RESET,
      db_result_get_i64(res, row, FR_COL_ID, 0));
  fr_row_cell(line, sizeof(line), cell, sizeof(cell), w->id, false);

  snprintf(cell, sizeof(cell), "%s%s" CLR_RESET,
      fr_type_color(type), fr_type_word(type));
  fr_row_cell(line, sizeof(line), cell, sizeof(cell), w->type, false);

  snprintf(cell, sizeof(cell), "%s%s" CLR_RESET,
      fr_status_color(status), fr_status_word(status));
  fr_row_cell(line, sizeof(line), cell, sizeof(cell), w->status, false);

  fr_text_cell(cell, sizeof(cell), CLR_CYAN,
      fr_cell_text(res, row, FR_COL_USER), w->user);
  fr_row_cell(line, sizeof(line), cell, sizeof(cell), w->user, false);

  fr_text_cell(cell, sizeof(cell), CLR_GRAY,
      fr_cell_text(res, row, FR_COL_BOT), w->bot);
  fr_row_cell(line, sizeof(line), cell, sizeof(cell), w->bot, false);

  fr_age_text(age, sizeof(age),
      (time_t)db_result_get_i64(res, row, FR_COL_CREATED, 0), now);
  snprintf(cell, sizeof(cell), CLR_GRAY "%s" CLR_RESET, age);
  fr_row_cell(line, sizeof(line), cell, sizeof(cell), w->age, false);

  fr_text_cell(cell, sizeof(cell), CLR_WHITE,
      fr_cell_text(res, row, FR_COL_DESC), w->desc);
  fr_row_cell(line, sizeof(line), cell, sizeof(cell), w->desc, true);

  cmd_reply(ctx, line);
}

// What the caller asked to be shown, in words, for the title line.
//
// "open only" is said out loud on the DEFAULT board, because that is
// the view holding rows back: a reader who cannot find #3 needs to be
// told it is finished rather than missing. --all, which holds nothing
// back, is the case with nothing to say.
static void
fr_filter_words(const fr_filter_t *f, char *out, size_t cap)
{
  size_t used = 0;

  out[0] = '\0';

  if(f->by_type)
    used += (size_t)snprintf(out + used, cap - used, "%s only",
        fr_type_word(f->type));

  if(f->by_status && used < cap)
    used += (size_t)snprintf(out + used, cap - used, "%s%s only",
        (used > 0) ? ", " : "", fr_status_word(f->status));

  else if(!f->all && used < cap)
    used += (size_t)snprintf(out + used, cap - used, "%sopen only",
        (used > 0) ? ", " : "");

  if(f->sort != FR_SORT_NEW && used < cap)
    snprintf(out + used, cap - used, "%s%s",
        (used > 0) ? ", " : "",
        (f->sort == FR_SORT_OLD) ? "oldest first" : "by status");
}

static void
fr_table(const cmd_ctx_t *ctx, const fr_filter_t *f)
{
  db_result_t *res   = db_result_alloc();
  time_t       now   = time(NULL);
  uint32_t     limit = fr_list_rows();
  fr_cols_t    w;
  char         words[128];
  char         line[FR_LINE_SZ];
  char         rule[FR_LINE_SZ];
  uint32_t     i;
  int          total;

  if(fr_db_list(f, limit, res) != SUCCESS)
  {
    cmd_reply(ctx, "I couldn't reach the board. :~(");
    db_result_free(res);
    return;
  }

  // Three different silences: nothing matched what was asked for,
  // nothing is left to work on, and nothing was ever filed. Only the
  // last is an empty board, and only the middle one is worth pointing
  // at --all for.
  if(res->rows == 0)
  {
    if(f->by_type || f->by_status)
      cmd_reply(ctx, "Nothing on the board matches that.");

    else if(f->all)
      cmd_reply(ctx, "The board is empty. `feature <description>` "
          "starts it.");

    else
      cmd_reply(ctx, "Nothing open on the board. `show feature --all` "
          "includes what's been closed.");

    db_result_free(res);
    return;
  }

  fr_filter_words(f, words, sizeof(words));

  snprintf(line, sizeof(line),
      "📋 " CLR_BOLD "FEATURE REQUESTS" CLR_RESET "%s%s%s",
      (words[0] != '\0') ? CLR_GRAY " — " : "",
      words, (words[0] != '\0') ? CLR_RESET : "");
  cmd_reply(ctx, line);

  fr_cols_measure(res, now, &w);

  fr_rule(rule, sizeof(rule), DISPLAY_COLS);
  cmd_reply(ctx, rule);
  fr_table_header(ctx, &w);

  for(i = 0; i < res->rows; i++)
    fr_table_row(ctx, res, i, now, &w);

  cmd_reply(ctx, rule);

  // Only say how many were held back when some were. A count that
  // always reads "12 of 12" is noise on every line of the board.
  total = fr_db_count(f);

  if(total > (int)res->rows)
  {
    snprintf(line, sizeof(line),
        CLR_GRAY "  showing %" PRIu32 " of %d — plugin.featreq.list_rows"
        " sets the depth." CLR_RESET, res->rows, total);
    cmd_reply(ctx, line);
  }

  db_result_free(res);
}

// ------------------------------------------------------------------ //
// The card                                                            //
// ------------------------------------------------------------------ //

// Emit `text` as indented lines of at most `cols` display columns,
// broken on a space where there is one and mid-word only when a single
// word is wider than the column itself. `color` is what separates the
// request from the answer to it — the card folds both.
static void
fr_wrap(const cmd_ctx_t *ctx, const char *text, int cols, const char *color)
{
  char line[FR_LINE_SZ];

  while(*text != '\0')
  {
    const char *brk = NULL;
    const char *p   = text;
    int         n   = 0;

    while(*p != '\0' && n < cols)
    {
      if(*p == ' ')
        brk = p;

      // One display column is a lead byte and its continuations.
      p++;

      while(((unsigned char)*p & 0xc0) == 0x80)
        p++;

      n++;
    }

    if(*p != '\0' && brk != NULL && brk > text)
      p = brk;

    snprintf(line, sizeof(line), "  %s%.*s" CLR_RESET,
        color, (int)(p - text), text);
    cmd_reply(ctx, line);

    text = p;

    while(*text == ' ')
      text++;
  }
}

static void
fr_card(const cmd_ctx_t *ctx, const db_result_t *res)
{
  fr_type_t   type    = FR_TYPE_FEAT;
  fr_status_t status  = FR_ST_NEW;
  time_t      created = (time_t)db_result_get_i64(res, 0, FR_COL_CREATED, 0);
  time_t      changed = (time_t)db_result_get_i64(res, 0, FR_COL_CHANGED, 0);
  time_t      now     = time(NULL);
  time_t      noted   = (time_t)db_result_get_i64(res, 0, FR_COL_NOTED, 0);
  const char *desc    = db_result_get(res, 0, FR_COL_DESC);
  const char *note    = db_result_get(res, 0, FR_COL_NOTE);
  char        word[FR_WORD_SZ];
  char        nick[FR_NAME_SZ];
  char        user[FR_NAME_SZ];
  char        bot [FR_NAME_SZ];
  char        meth[FR_NAME_SZ];
  char        via [FR_NAME_SZ * 3 + 64];
  char        who [FR_NAME_SZ * 4];
  char        tail[FR_NAME_SZ * 4];
  char        stamp[FR_STAMP_SZ];
  char        age  [UTIL_DURATION_SZ];
  char        line[FR_LINE_SZ];
  char        rule[FR_LINE_SZ];

  db_result_copy(word, sizeof(word), res, 0, FR_COL_TYPE);
  (void)fr_type_parse(word, &type);

  db_result_copy(word, sizeof(word), res, 0, FR_COL_STATUS);
  (void)fr_status_parse(word, &status);

  db_result_copy(nick, sizeof(nick), res, 0, FR_COL_NICK);
  db_result_copy(user, sizeof(user), res, 0, FR_COL_USER);
  db_result_copy(bot,  sizeof(bot),  res, 0, FR_COL_BOT);
  db_result_copy(meth, sizeof(meth), res, 0, FR_COL_METHOD);

  fr_rule(rule, sizeof(rule), FR_W_CARD);

  snprintf(line, sizeof(line),
      "📋 " CLR_BOLD "REQUEST #%" PRId64 CLR_RESET "   %s%s" CLR_RESET
      "   %s%s" CLR_RESET,
      db_result_get_i64(res, 0, FR_COL_ID, 0),
      fr_type_color(type), fr_type_word(type),
      fr_status_color(status), fr_status_word(status));
  cmd_reply(ctx, line);
  cmd_reply(ctx, rule);

  // "as doc_ on irc, heard by hedgehogg" where a bot on a method heard
  // it. A request typed at the control socket has neither a nickname
  // nor a bot, and naming only the way in is what says so.
  if(nick[0] != '\0')
    snprintf(via, sizeof(via), "as %s on %s", nick, meth);

  else
    snprintf(via, sizeof(via), "over %s", meth);

  if(bot[0] != '\0')
  {
    size_t n = strlen(via);

    snprintf(via + n, sizeof(via) - n, ", heard by %s", bot);
  }

  // Every name on this line came from outside, and three of them in one
  // sentence can outrun a frame that promises DISPLAY_COLS. The
  // username is the identity that matters, so it is fitted first and
  // the provenance clause takes whatever the frame has left. Real names
  // reach neither bound; this only ever fires on the pathological one.
  //
  // Both budgets are one short of the room available, because
  // display_fit appends its mark outside the columns it was given.
  display_fit((user[0] != '\0') ? user : "—", FR_W_CARD_USER - 1, who,
      sizeof(who), "…");

  display_fit(via, DISPLAY_COLS - FR_W_CARD_GUTTER - 2
      - (int)display_vis_len(who) - 1, tail, sizeof(tail), "…");

  snprintf(line, sizeof(line),
      CLR_GRAY "  %-*s" CLR_RESET " " CLR_CYAN "%s" CLR_RESET
      CLR_GRAY "  %s" CLR_RESET,
      FR_W_CARD_LABEL, "from", who, tail);
  cmd_reply(ctx, line);

  fr_stamp_text(stamp, sizeof(stamp), created, now);

  snprintf(line, sizeof(line), CLR_GRAY "  %-*s" CLR_RESET " %s",
      FR_W_CARD_LABEL, "filed", stamp);
  cmd_reply(ctx, line);

  util_fmt_duration((now > changed) ? now - changed : 0, age, sizeof(age));

  snprintf(line, sizeof(line),
      CLR_GRAY "  %-*s" CLR_RESET " %s%s" CLR_RESET CLR_GRAY
      "  (since %s ago)" CLR_RESET,
      FR_W_CARD_LABEL, "status", fr_status_color(status),
      fr_status_word(status), age);
  cmd_reply(ctx, line);

  cmd_reply(ctx, " ");

  fr_wrap(ctx, (desc != NULL) ? desc : "", FR_W_WRAP, CLR_WHITE);

  // The answer, where one has been written. A request that has none
  // ends at its description exactly as it always did: an empty `note`
  // line would say only that nobody has been here yet, which is what
  // its absence already says.
  //
  // This is the whole of what the card gained — the table has no room
  // for a note and no need of one, since recognising a request is its
  // job and reading the answer is the card's.
  if(note != NULL && note[0] != '\0')
  {
    cmd_reply(ctx, " ");

    fr_stamp_text(stamp, sizeof(stamp), noted, now);

    snprintf(line, sizeof(line), CLR_GRAY "  %-*s" CLR_RESET " %s",
        FR_W_CARD_LABEL, "note", stamp);
    cmd_reply(ctx, line);

    fr_wrap(ctx, note, FR_W_WRAP, CLR_CYAN);
  }

  cmd_reply(ctx, rule);
}

static void
fr_show_one(const cmd_ctx_t *ctx, int64_t id)
{
  db_result_t *res = db_result_alloc();
  char         line[FR_LINE_SZ];

  if(fr_db_fetch(id, res) != SUCCESS)
    cmd_reply(ctx, "I couldn't reach the board. :~(");

  else if(res->rows == 0)
  {
    snprintf(line, sizeof(line),
        "Nothing on the board carries id " CLR_YELLOW "%" PRId64 CLR_RESET
        ".", id);
    cmd_reply(ctx, line);
  }

  else
    fr_card(ctx, res);

  db_result_free(res);
}

// ------------------------------------------------------------------ //
// show feature                                                        //
// ------------------------------------------------------------------ //

// Parse `[id] [--type X] [--status Y] [--sort Z]` in any order. `id`
// stays -1 when no bare number was given, which is what selects the
// table over the card. Returns false having already replied.
static bool
fr_show_parse(const cmd_ctx_t *ctx, const char *args, fr_filter_t *f,
    int64_t *id)
{
  const char *p = (args != NULL) ? args : "";
  char        tok[64];
  char        val[64];
  char        line[FR_LINE_SZ];
  char        tokens[128] = "";

  memset(f, 0, sizeof(*f));
  f->sort = FR_SORT_NEW;
  *id     = -1;

  for(;;)
  {
    bool ok = false;

    p = fr_skip_ws(p);

    if(*p == '\0')
      return(true);

    p = fr_token(p, tok, sizeof(tok));

    if(fr_all_digits(tok))
    {
      *id = (int64_t)strtoll(tok, NULL, 10);
      continue;
    }

    if(tok[0] != '-' || tok[1] != '-')
    {
      snprintf(line, sizeof(line),
          "I don't know what to do with " CLR_YELLOW "%s" CLR_RESET
          ". Try: show feature [id] [--all] [--type ...] "
          "[--status ...] [--sort new|old|status]", tok);
      cmd_reply(ctx, line);
      return(false);
    }

    // --all is answered BEFORE the value scan below, because it is the
    // one flag here that takes no value: read past it and it eats the
    // next token, which is usually the following flag.
    if(strcasecmp(tok, "--all") == 0)
    {
      f->all = true;
      continue;
    }

    p = fr_token(fr_skip_ws(p), val, sizeof(val));

    if(strcasecmp(tok, "--type") == 0)
    {
      ok = fr_type_parse(val, &f->type);
      f->by_type = ok;

      if(!ok)
        fr_type_tokens(tokens, sizeof(tokens));
    }

    else if(strcasecmp(tok, "--status") == 0)
    {
      ok = fr_status_parse(val, &f->status);
      f->by_status = ok;

      if(!ok)
        fr_status_tokens(tokens, sizeof(tokens));
    }

    else if(strcasecmp(tok, "--sort") == 0)
    {
      ok = fr_sort_parse(val, &f->sort);

      if(!ok)
        fr_sort_tokens(tokens, sizeof(tokens));
    }

    else
    {
      snprintf(line, sizeof(line),
          "I don't know the option " CLR_YELLOW "%s" CLR_RESET
          ". Try --all, --type, --status or --sort.", tok);
      cmd_reply(ctx, line);
      return(false);
    }

    if(!ok)
    {
      snprintf(line, sizeof(line), "%s takes one of: " CLR_CYAN "%s"
          CLR_RESET ".", tok, tokens);
      cmd_reply(ctx, line);
      return(false);
    }
  }
}

static void
fr_show_handler(const cmd_ctx_t *ctx)
{
  fr_filter_t f;
  int64_t     id;

  if(!fr_show_parse(ctx, ctx->args, &f, &id))
    return;

  if(id >= 0)
    fr_show_one(ctx, id);

  else
    fr_table(ctx, &f);
}

// ------------------------------------------------------------------ //
// Registration                                                        //
// ------------------------------------------------------------------ //

static const cmd_decl_t show_feature_decl = {
  .module      = "featreq",
  .name        = "feature",
  .usage       = "show feature [id] [--all] [--type <t>] [--status <s>] "
                 "[--sort <new|old|status>]",
  .description = "The feature-request board.",
  .help_long   =
      "With no argument, draws every request still OPEN, newest "
      "first: id, what kind it is, where it has got to, who asked, "
      "which bot heard it, how long ago, and the request itself. "
      "Completed and canceled requests are left out, because the "
      "default board is a worklist — --all puts them back, and "
      "naming one with --status asks for it directly. Give an id for "
      "one request in full, whatever its status. --type narrows to "
      "feat / bug / change, --status to new / in-prog / on-hold / "
      "completed / canceled. --sort old is queue order, oldest first — what to "
      "work on next — and --sort status groups the board by where "
      "each request has got to rather than by when it arrived. The "
      "board is global — every bot and every namespace reads the "
      "same one.",
  .group       = USERNS_GROUP_USER,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = fr_show_handler,
  .parent_path = "show",
  .abbrev      = "feat",
};

bool
fr_show_register(void)
{
  if(cmd_register(&show_feature_decl) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

void
fr_show_unregister(void)
{
  cmd_unregister_path("show/feature");
}
