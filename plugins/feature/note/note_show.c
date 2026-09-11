// botmanager — MIT
// `show note`: the mail still waiting in this namespace — who left it,
// for whom, how long it has sat, where it was left, and the note itself.
// Read-only and userns-scoped, the same namespace `note` writes into.
//
// Admin-gated because the last column is somebody's mail: telemetry for
// whoever keeps the namespace, not a way for one user to read another's.

#define NOTE_INTERNAL
#include "note.h"

#include "colors.h"
#include "display.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// Table geometry. Every column but the last is sized to the widest value
// in this result, never under its own label and never past its ceiling;
// the note takes what the others leave of DISPLAY_COLS. The gaps are the
// single spaces between six columns.
#define NS_GAPS       5
#define NS_MAX_ID     6
#define NS_MAX_USER  12
#define NS_MAX_AGE    9
#define NS_MAX_WHERE 14

// A cell's text before its colour markers, then the cell wearing them.
// No column is wider than the table, so four bytes to a display column
// bounds every one, plus the ellipsis display_fit reserves.
#define NS_FIT_SZ    (DISPLAY_COLS * 4 + 8)
#define NS_CELL_SZ   (NS_FIT_SZ + 8)
#define NS_LINE_SZ   (NS_CELL_SZ * 6)

typedef struct
{
  int id;
  int from;
  int to;
  int age;
  int where;
  int body;
} note_cols_t;

// ------------------------------------------------------------------ //
// Cells                                                               //
// ------------------------------------------------------------------ //

// One cell as plain text, never NULL — the measuring pass and the render
// read through the same door so they cannot disagree about a blank.
static const char *
ns_cell_text(const db_result_t *res, uint32_t row, uint32_t col)
{
  const char *s = db_result_get(res, row, col);

  return((s != NULL && s[0] != '\0') ? s : "—");
}

// Where the note was left: the channel, or the method when there was no
// channel to name. Provenance only — delivery follows the recipient.
static const char *
ns_where_text(const db_result_t *res, uint32_t row)
{
  const char *s = db_result_get(res, row, NOTE_LIST_COL_CHANNEL);

  if(s != NULL && s[0] != '\0')
    return(s);

  return(ns_cell_text(res, row, NOTE_LIST_COL_METHOD));
}

static void
ns_age_text(char *out, size_t cap, const db_result_t *res, uint32_t row,
    time_t now)
{
  time_t then = (time_t)db_result_get_i64(res, row, NOTE_LIST_COL_CREATED, 0);

  if(then <= 0)
  {
    strlcpy(out, "—", cap);
    return;
  }

  util_fmt_duration((now > then) ? now - then : 0, out, cap);
}

// A colorized cell holding at most `width` columns of `text`. The
// ellipsis lands OUTSIDE display_fit's column budget, so only a value
// that actually overflows is fitted a column short.
static void
ns_text_cell(char *out, size_t cap, const char *color, const char *text,
    int width)
{
  char fitted[NS_FIT_SZ];

  if((int)display_vis_len(text) <= width)
    strlcpy(fitted, text, sizeof(fitted));

  else
    display_fit(text, width - 1, fitted, sizeof(fitted), "…");

  snprintf(out, cap, "%s%s" CLR_RESET, color, fitted);
}

// Append a finished cell; every column but the last is padded and
// followed by the gap, and the last carries no trailing blanks.
static void
ns_row_cell(char *line, size_t cap, char *cell, size_t cell_cap, int width,
    bool last)
{
  if(!last)
    display_align_left(cell, cell_cap, width);

  display_cat(line, cap, cell);

  if(!last)
    display_cat(line, cap, " ");
}

// ------------------------------------------------------------------ //
// Measure                                                             //
// ------------------------------------------------------------------ //

static void
ns_col_fit(int *w, const char *s, int ceiling)
{
  int n = (int)display_vis_len(s);

  if(n > ceiling)
    n = ceiling;

  if(n > *w)
    *w = n;
}

static void
ns_cols_measure(const db_result_t *res, time_t now, note_cols_t *w)
{
  char     buf[32];
  uint32_t i;

  // The labels are the floor: a column is never narrower than its head.
  w->id    = (int)display_vis_len("id");
  w->from  = (int)display_vis_len("from");
  w->to    = (int)display_vis_len("to");
  w->age   = (int)display_vis_len("waiting");
  w->where = (int)display_vis_len("left in");

  for(i = 0; i < res->rows; i++)
  {
    snprintf(buf, sizeof(buf), "#%" PRId64,
        db_result_get_i64(res, i, NOTE_LIST_COL_ID, 0));
    ns_col_fit(&w->id, buf, NS_MAX_ID);

    ns_col_fit(&w->from,
        ns_cell_text(res, i, NOTE_LIST_COL_SENDER), NS_MAX_USER);
    ns_col_fit(&w->to,
        ns_cell_text(res, i, NOTE_LIST_COL_RECIPIENT), NS_MAX_USER);

    ns_age_text(buf, sizeof(buf), res, i, now);
    ns_col_fit(&w->age, buf, NS_MAX_AGE);

    ns_col_fit(&w->where, ns_where_text(res, i), NS_MAX_WHERE);
  }

  w->body = DISPLAY_COLS - NS_GAPS - w->id - w->from - w->to - w->age
      - w->where;
}

// ------------------------------------------------------------------ //
// Render                                                              //
// ------------------------------------------------------------------ //

// Unlike a row, the header pads its LAST cell too: cmd_reply_table_head
// draws its rule exactly as far as the header runs, and the rule should
// span the table, not stop at the word "note".
static void
ns_hdr_cell(char *line, size_t cap, const char *label, int width, bool last)
{
  char cell[NS_CELL_SZ];

  strlcpy(cell, label, sizeof(cell));
  display_align_left(cell, sizeof(cell), width);
  display_cat(line, cap, cell);

  if(!last)
    display_cat(line, cap, " ");
}

static void
ns_table_head(const cmd_ctx_t *ctx, const note_cols_t *w)
{
  char line[NS_LINE_SZ];

  strlcpy(line, CLR_BOLD, sizeof(line));

  ns_hdr_cell(line, sizeof(line), "id",      w->id,    false);
  ns_hdr_cell(line, sizeof(line), "from",    w->from,  false);
  ns_hdr_cell(line, sizeof(line), "to",      w->to,    false);
  ns_hdr_cell(line, sizeof(line), "waiting", w->age,   false);
  ns_hdr_cell(line, sizeof(line), "left in", w->where, false);
  ns_hdr_cell(line, sizeof(line), "note",    w->body,  true);

  display_cat(line, sizeof(line), CLR_RESET);
  cmd_reply_table_head(ctx, line);
}

static void
ns_table_row(const cmd_ctx_t *ctx, const db_result_t *res, uint32_t row,
    time_t now, const note_cols_t *w)
{
  char age [UTIL_DURATION_SZ];
  char cell[NS_CELL_SZ];
  char line[NS_LINE_SZ];

  line[0] = '\0';

  snprintf(cell, sizeof(cell), CLR_BOLD "#%" PRId64 CLR_RESET,
      db_result_get_i64(res, row, NOTE_LIST_COL_ID, 0));
  ns_row_cell(line, sizeof(line), cell, sizeof(cell), w->id, false);

  ns_text_cell(cell, sizeof(cell), CLR_CYAN,
      ns_cell_text(res, row, NOTE_LIST_COL_SENDER), w->from);
  ns_row_cell(line, sizeof(line), cell, sizeof(cell), w->from, false);

  ns_text_cell(cell, sizeof(cell), CLR_YELLOW,
      ns_cell_text(res, row, NOTE_LIST_COL_RECIPIENT), w->to);
  ns_row_cell(line, sizeof(line), cell, sizeof(cell), w->to, false);

  ns_age_text(age, sizeof(age), res, row, now);
  ns_text_cell(cell, sizeof(cell), CLR_GRAY, age, w->age);
  ns_row_cell(line, sizeof(line), cell, sizeof(cell), w->age, false);

  ns_text_cell(cell, sizeof(cell), CLR_GRAY, ns_where_text(res, row),
      w->where);
  ns_row_cell(line, sizeof(line), cell, sizeof(cell), w->where, false);

  ns_text_cell(cell, sizeof(cell), CLR_WHITE,
      ns_cell_text(res, row, NOTE_LIST_COL_BODY), w->body);
  ns_row_cell(line, sizeof(line), cell, sizeof(cell), w->body, true);

  cmd_reply(ctx, line);
}

static void
note_cmd_show(const cmd_ctx_t *ctx)
{
  userns_t    *ns  = userns_session_resolve(ctx);
  db_result_t *res;
  time_t       now = time(NULL);
  note_cols_t  w;
  char         line[NS_LINE_SZ];
  char         rule[DISPLAY_RULE_SZ(DISPLAY_COLS)];
  int64_t      total;
  uint32_t     i;

  if(ns == NULL)   // the resolver already replied
    return;

  res = db_result_alloc();

  if(note_db_list(ns->id, NOTE_SHOW_ROWS, res) != SUCCESS)
  {
    cmd_reply(ctx, "I couldn't reach my notebook. :~(");
    db_result_free(res);
    return;
  }

  if(res->rows == 0)
  {
    snprintf(line, sizeof(line),
        "No notes waiting in " CLR_CYAN "%s" CLR_RESET ".", ns->name);
    cmd_reply(ctx, line);
    db_result_free(res);
    return;
  }

  total = db_result_get_i64(res, 0, NOTE_LIST_COL_TOTAL, res->rows);

  snprintf(line, sizeof(line),
      "📬 " CLR_BOLD "QUEUED NOTES" CLR_RESET CLR_GRAY " — " CLR_RESET
      CLR_CYAN "%s" CLR_RESET CLR_GRAY " — %" PRId64 " waiting, oldest"
      " first" CLR_RESET, ns->name, total);
  cmd_reply(ctx, line);

  ns_cols_measure(res, now, &w);
  ns_table_head(ctx, &w);

  for(i = 0; i < res->rows; i++)
    ns_table_row(ctx, res, i, now, &w);

  rule[0] = '\0';
  display_rule(rule, sizeof(rule), DISPLAY_COLS);
  cmd_reply(ctx, rule);

  // Only say how many were held back when some were.
  if(total > (int64_t)res->rows)
  {
    snprintf(line, sizeof(line),
        CLR_GRAY "showing the oldest %" PRIu32 " of %" PRId64 "." CLR_RESET,
        res->rows, total);
    cmd_reply(ctx, line);
  }

  db_result_free(res);
}

// ------------------------------------------------------------------ //
// Registration                                                        //
// ------------------------------------------------------------------ //

static const cmd_decl_t show_note_decl = {
  .module      = "note",
  .name        = "note",
  .usage       = "show note",
  .description = "List the notes still waiting to be delivered.",
  .help_long   = "Oldest first: who left each note, for whom, how long it "
                 "has waited, where it was left, and the note itself. "
                 "Scoped to this namespace, and admin-only because the "
                 "last column is somebody's mail.",
  .group       = USERNS_GROUP_ADMIN,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = note_cmd_show,
  .parent_path = "show",
};

bool
note_show_register(void)
{
  return(cmd_register(&show_note_decl));
}

void
note_show_unregister(void)
{
  cmd_unregister_path("show/note");
}
