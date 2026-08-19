#ifndef BM_USERQUOTE_H
#define BM_USERQUOTE_H

// botmanager — MIT
// userquote: a cosmetic "quote another user" feature. Users capture a
// memorable line into a per-userns quote book and recall it later. The
// command surface is registered globally and works from any bot on any
// method. A bare `!quote add` immortalises whoever just spoke by reading
// the dispatching bot's last-witnessed public line (bot_last_public_line
// in core) — this plugin does not observe the message stream itself.
//
// This is the successor to the old standalone "quotebot" (hedgehogg).

// ---- Public-facing: nothing. The plugin talks to the core exclusively
// through cmd_register / cmd_reply / db_* / kv_*. All declarations below
// are internal, gated on USERQUOTE_INTERNAL.

#ifdef USERQUOTE_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "method.h"
#include "plugin.h"

#include <stdint.h>

// CLAM context (registered in CLAM.md).
#define UQ_CTX              "userquote"

// KV keys (registered under the plugin schema).
#define UQ_KV_TABLE         "plugin.userquote.table"
#define UQ_KV_MAX_QUOTE     "plugin.userquote.max_quote_len"
#define UQ_KV_MAX_SAYER     "plugin.userquote.max_sayer_len"

// Storage bounds. The table name is a SQL identifier, so it is validated
// as strict alnum/underscore before ever touching a query string.
#define UQ_TABLE_SZ         64
#define UQ_SAYER_SZ         64
#define UQ_QUOTER_SZ        64
#define UQ_QUOTE_SZ         1024

// One recalled/looked-up quote row.
typedef struct
{
  int64_t id;
  char    sayer   [UQ_SAYER_SZ];
  char    quoter  [UQ_QUOTER_SZ];
  char    quote   [UQ_QUOTE_SZ];
  char    created [40];                // rendered timestamp
  char    lastview[40];                // rendered timestamp
} uq_quote_t;

// How many sayers the `show quotes` leaderboard names.
#define UQ_TOP_SAYERS       5

// One leaderboard entry: a sayer and how many of the book is theirs.
typedef struct
{
  char    name[UQ_SAYER_SZ];
  int64_t count;
} uq_tally_t;

// Aggregate telemetry for one namespace's book. Every count is a plain
// row count; `span_days` is the whole distance between the oldest and
// newest capture, so it is 0 for a book of one.
typedef struct
{
  int64_t    total;
  int64_t    sayers;             // distinct, case-folded
  int64_t    quoters;            // distinct, case-folded
  int64_t    channels;           // distinct non-empty
  char       oldest[24];         // YYYY-MM-DD, empty when the book is
  char       newest[24];         // YYYY-MM-DD, empty when the book is
  int64_t    span_days;
  int64_t    avg_len;            // bytes
  int64_t    max_len;            // bytes
  int64_t    recent;             // captured in the last 30 days
  int64_t    unseen;             // never recalled since capture
  char       busiest[8];         // busiest calendar year, or empty
  int64_t    busiest_n;
  uq_tally_t top[UQ_TOP_SAYERS];
  uint32_t   n_top;
} uq_stats_t;

// ---- DB layer (uq_db.c) -------------------------------------------- //

// Resolve + validate the configured table name into `out`. FAIL if the
// KV value is not a safe SQL identifier.
bool uq_table_name(char *out, size_t cap);

// Ensure the quotes table + indexes exist. Idempotent; runs once.
bool uq_schema_ensure(void);

// Insert a quote. Returns the new row id (>0) or -1 on error.
int64_t uq_db_add(uint32_t ns_id, const char *method, const char *channel,
    const char *sayer, const char *quoter, const char *quote);

// Recall a quote scoped to `ns_id`. If `id > 0`, fetch that id; else if
// `sayer` is non-empty, the least-recently-viewed quote by that sayer;
// else the least-recently-viewed quote in the namespace. On success the
// row's lastview is bumped. Returns true on hit.
bool uq_db_get(uint32_t ns_id, int64_t id, const char *sayer,
    uq_quote_t *out);

// Delete a quote by id within `ns_id`. Returns rows affected (0 or 1),
// or -1 on error.
int uq_db_del(uint32_t ns_id, int64_t id);

// Aggregate the whole book for `ns_id` into `out`. Three reads: the
// aggregate row, the sayer leaderboard, the busiest year. FAIL only when
// the store could not be read — an empty book is a zeroed struct and a
// SUCCESS.
bool uq_db_stats(uint32_t ns_id, uq_stats_t *out);

// ---- Command surface (uq_cmds.c) ----------------------------------- //

bool uq_commands_register(void);
void uq_commands_unregister(void);

// ---- Telemetry surface (uq_show.c) --------------------------------- //

bool uq_show_register(void);
void uq_show_unregister(void);

#endif // USERQUOTE_INTERNAL

#endif // BM_USERQUOTE_H
