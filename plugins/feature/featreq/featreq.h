#ifndef BM_FEATREQ_H
#define BM_FEATREQ_H

// featreq: a suggestion box. `feature [--type <bug|change>] <description>`
// files a request, `bug <description>` files the commonest kind of one,
// `show feature` reads the board back, `feature status` moves a row
// along and `feature note` writes the answer onto it. The table is
// GLOBAL — one board for the whole daemon, not one
// per userns — because a request is about the software, not about the
// room it was mentioned in.

// ---- Public-facing: nothing. Everything below is internal, gated on
// FEATREQ_INTERNAL, exactly as note.h does it.

#ifdef FEATREQ_INTERNAL

#include "bot.h"
#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "db.h"
#include "method.h"
#include "plugin.h"
#include "userns.h"

#include <stddef.h>
#include <stdint.h>

// CLAM context (registered in CLAM.md).
#define FR_CTX             "featreq"

// KV keys (registered under the plugin schema).
#define FR_KV_TABLE        "plugin.featreq.table"
#define FR_KV_MAX_DESC     "plugin.featreq.max_desc_cols"
#define FR_KV_LIST_ROWS    "plugin.featreq.list_rows"
#define FR_KV_MAX_NOTE     "plugin.featreq.max_note_cols"

// The table name is a SQL identifier, validated strict alnum/underscore
// before it is ever pasted into a statement.
#define FR_TABLE_SZ        64

// A description as stored. Sized from the widest thing that can reach
// us — one method line — so sanitising never truncates and the only
// bound the user meets is the configured column count.
#define FR_DESC_SZ         METHOD_TEXT_SZ

// The answer written back onto a request. Sized like a description and
// for the same reason: cleaning only ever drops bytes, so a buffer as
// wide as the widest line that can reach us cannot truncate one, and
// the only bound a writer meets is the configured column count.
#define FR_NOTE_SZ         METHOD_TEXT_SZ

// One rendered, colorized line. A row is at most DISPLAY_COLS columns
// and a column at most four UTF-8 bytes, so this holds the widest row
// drawable plus its colour markers. The cell buffers within it are
// sized in featreq_show.c, the only file that draws one.
#define FR_LINE_SZ         640

// A short name read back out of the board — nickname, username, bot,
// method kind — sized from the VARCHAR(64) columns that hold them.
#define FR_NAME_SZ         64

// One vocabulary word read back out of the board.
#define FR_WORD_SZ         32

// Most rows one `show feature` may draw, whatever the knob says. The
// board is read over a chat method: past this the reply is a flood
// rather than a table.
#define FR_LIST_ROWS_MAX   100

// ------------------------------------------------------------------ //
// Vocabulary (featreq_vocab.c)                                        //
// ------------------------------------------------------------------ //

// What was asked for. FR_TYPE_FEAT is `feature`'s default: a request
// with no --type is a feature somebody would like. It is spelled `feat`
// rather than `new` because `new` is also where every row STARTS on the
// status axis, and a board reading `new  new` says nothing twice.
typedef enum
{
  FR_TYPE_FEAT = 0,
  FR_TYPE_BUG,
  FR_TYPE_CHANGE,
} fr_type_t;

// Where it has got to. FR_ST_NEW is where every row starts.
typedef enum
{
  FR_ST_NEW = 0,
  FR_ST_PROG,
  FR_ST_DONE,
  FR_ST_CANCEL,
} fr_status_t;

typedef enum
{
  FR_SORT_NEW = 0,   // newest first
  FR_SORT_OLD,       // oldest first — the queue order work is taken in
  FR_SORT_STATUS,    // by where it has got to, newest first within
} fr_sort_t;

// The stored word is also the displayed word; the token is what the
// command line accepts in its place ("in-prog" for "in progress").
const char *fr_type_word   (fr_type_t   t);
const char *fr_type_color  (fr_type_t   t);
const char *fr_status_word (fr_status_t s);
const char *fr_status_token(fr_status_t s);
const char *fr_status_color(fr_status_t s);

// Parse one user-supplied word. Both spellings of a status are accepted
// ("in progress" and "in-prog"), as is "cancelled". True on a match.
bool fr_type_parse  (const char *s, fr_type_t   *out);
bool fr_status_parse(const char *s, fr_status_t *out);
bool fr_sort_parse  (const char *s, fr_sort_t   *out);
void fr_sort_tokens (char *out, size_t cap);

// The accepted spellings, comma-joined, for an error reply.
void fr_type_tokens  (char *out, size_t cap);
void fr_status_tokens(char *out, size_t cap);

// The same list as a usage-line placeholder: `<new|in-prog|…>`. Both
// vocabularies are closed and short, so FR_SYNTAX_SZ bounds either.
#define FR_SYNTAX_SZ  64

void fr_type_syntax  (char *out, size_t cap);
void fr_status_syntax(char *out, size_t cap);

// `CASE status WHEN … END`, yielding each status's ordinal — the ORDER
// BY for FR_SORT_STATUS, built from the same table the words come from
// so the two can never disagree. Returns `out`.
const char *fr_status_case_sql(char *out, size_t cap);

// `status NOT IN ('completed', 'canceled')` — the default board's WHERE
// clause, naming every status the vocabulary marks CLOSED. Built from
// the same table for the same reason. Returns `out`.
const char *fr_status_open_sql(char *out, size_t cap);

// Cutting a command line into the words above. fr_token() copies the
// whitespace-delimited token at `p` into `out` and returns the byte
// after it, consuming a token longer than `out` whole so an over-long
// flag reads as one bad flag rather than as a flag plus a fragment.
const char *fr_skip_ws(const char *p);
const char *fr_token(const char *p, char *out, size_t cap);
bool fr_all_digits(const char *s);

// ------------------------------------------------------------------ //
// DB layer (featreq_db.c)                                             //
// ------------------------------------------------------------------ //

// Every SELECT here projects these columns in this order and every
// reader indexes them by the enum below, so a projection and its reader
// cannot drift apart.
#define FR_SELECT_COLS \
    "id, req_type, status, nickname, method, username, botname," \
    " description, EXTRACT(EPOCH FROM created_at)::BIGINT," \
    " EXTRACT(EPOCH FROM status_at)::BIGINT, note," \
    " EXTRACT(EPOCH FROM note_at)::BIGINT"

enum
{
  FR_COL_ID = 0,
  FR_COL_TYPE,
  FR_COL_STATUS,
  FR_COL_NICK,
  FR_COL_METHOD,
  FR_COL_USER,
  FR_COL_BOT,
  FR_COL_DESC,
  FR_COL_CREATED,
  FR_COL_CHANGED,
  FR_COL_NOTE,
  FR_COL_NOTED,
};

// Who filed it and from where. Every member is borrowed for the call.
typedef struct
{
  const char *nickname;
  const char *method;     // the METHOD KIND — "irc", "reachy", "text"
  const char *username;
  const char *botname;
  const char *desc;
  fr_type_t   type;
} fr_new_t;

// `all` is the only member that WIDENS the board: without it a request
// the operator has finished with drops out of the default view, which
// is what keeps the board a worklist rather than an archive. Naming a
// status explicitly reaches a closed one without it.
typedef struct
{
  bool        by_type;
  fr_type_t   type;
  bool        by_status;
  fr_status_t status;
  bool        all;
  fr_sort_t   sort;
} fr_filter_t;

// Three outcomes because the caller says something different about
// each: the row moved, no such row exists, or the write itself failed.
typedef enum
{
  FR_UPD_OK = 0,
  FR_UPD_NO_ROW,
  FR_UPD_ERROR,
} fr_upd_t;

// Resolve + validate the configured table name into `out`.
bool fr_table_name(char *out, size_t cap);

// Ensure the table + index exist. Idempotent; runs once.
bool fr_schema_ensure(void);

// File a request. Returns the new row id (>0), or -1 on error.
int64_t fr_db_add(const fr_new_t *req);

// Move a request to `status`, stamping status_at.
fr_upd_t fr_db_set_status(int64_t id, fr_status_t status);

// Write `note` onto a request, stamping note_at. An empty note CLEARS
// both — a note written onto the wrong id is the one mistake here that
// replacing it cannot undo.
fr_upd_t fr_db_set_note(int64_t id, const char *note);

// Fill `res` with the matching rows, ordered and capped. `res` is the
// caller's, from db_result_alloc().
bool fr_db_list(const fr_filter_t *f, uint32_t limit, db_result_t *res);

// One row by id. `res` holds zero rows when nothing carries that id.
bool fr_db_fetch(int64_t id, db_result_t *res);

// How many rows the filter matches in total, ignoring the row cap.
// Returns -1 on error.
int fr_db_count(const fr_filter_t *f);

// ------------------------------------------------------------------ //
// Command surfaces (featreq_cmds.c, featreq_show.c)                   //
// ------------------------------------------------------------------ //

bool fr_commands_register(void);
void fr_commands_unregister(void);

bool fr_show_register(void);
void fr_show_unregister(void);

#endif // FEATREQ_INTERNAL

#endif // BM_FEATREQ_H
