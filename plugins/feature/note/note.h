#ifndef BM_NOTE_H
#define BM_NOTE_H

// botmanager — MIT
// note: leave a message for another user in the same namespace, delivered
// the next time a bot witnesses that user speak. The command surface
// (`note`) is registered globally, so it works from any bot on any method;
// delivery rides an observer this plugin attaches to every running bot's
// method stream (the urlgrabber attachment model).
//
// Two halves, deliberately separated:
//   - the command writes a row and never waits for delivery
//   - the observer never touches the DB unless the in-memory pending set
//     says this namespace has an undelivered note for this user

// ---- Public-facing: nothing. The plugin talks to core exclusively
// through cmd_register / cmd_reply / method_* / db_* / kv_*. Everything
// below is internal, gated on NOTE_INTERNAL.

#ifdef NOTE_INTERNAL

#include "bot.h"
#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "method.h"
#include "plugin.h"
#include "userns.h"

#include <stdint.h>

// CLAM context (registered in CLAM.md).
#define NOTE_CTX            "note"

// KV keys (registered under the plugin schema).
#define NOTE_KV_TABLE       "plugin.note.table"
#define NOTE_KV_MIN_IDLE    "plugin.note.min_idle_secs"
#define NOTE_KV_MAX_BODY    "plugin.note.max_body_len"
#define NOTE_KV_MAX_PENDING "plugin.note.max_pending"

// Fallbacks used when a knob is absent (pre-kv_load reads, mostly).
#define NOTE_DEFAULT_MIN_IDLE     600
#define NOTE_DEFAULT_MAX_PENDING  10

// Storage bounds. The table name is a SQL identifier, so it is validated
// as strict alnum/underscore before ever touching a query string.
#define NOTE_TABLE_SZ       64
#define NOTE_BODY_SZ        512

// Most notes a single delivery pass claims and announces at once. A user
// who came back to a stack taller than this gets the rest on their next
// line — better than flooding the channel.
#define NOTE_DELIVER_MAX    5

// One claimed, about-to-be-announced note.
typedef struct
{
  int64_t id;
  char    sender[USERNS_USER_SZ];
  char    body  [NOTE_BODY_SZ];
  time_t  created;
} note_row_t;

// ---- DB layer (note_db.c) ------------------------------------------ //

// Resolve + validate the configured table name into `out`. FAIL if the
// KV value is not a safe SQL identifier.
bool note_table_name(char *out, size_t cap);

// Ensure the notes table + index exist. Idempotent; runs once.
bool note_schema_ensure(void);

// Insert a pending note. Returns the new row id (>0) or -1 on error.
int64_t note_db_add(uint32_t ns_id, const char *sender,
    const char *recipient, const char *body, const char *method,
    const char *channel);

// Count the undelivered notes already waiting for `recipient`.
// Returns -1 on error.
int note_db_pending_count(uint32_t ns_id, const char *recipient);

// Atomically claim up to `cap` of `recipient`'s undelivered notes —
// the UPDATE ... RETURNING stamps delivered_at and hands back the rows
// in one statement, so two bots witnessing the same line cannot announce
// the same note twice. Fills `out`, returns how many were claimed
// (0 when another claimant got there first), or -1 on error.
int note_db_claim(uint32_t ns_id, const char *recipient, note_row_t *out,
    uint32_t cap);

// Repopulate the pending set from the table. Called once at start() so a
// note left before a restart is still delivered. Returns rows loaded, or
// -1 on error.
int note_db_pending_reload(void);

// ---- Pending set + delivery (note_deliver.c) ----------------------- //

// The in-memory answer to "could this user have mail?" — consulted on
// every witnessed line, so it must never touch the DB. Both calls are
// thread-safe.
void note_pending_add(uint32_t ns_id, const char *recipient);
bool note_pending_maybe(uint32_t ns_id, const char *recipient);

// True when any user in this namespace has mail waiting. The cheap gate
// that keeps an idle channel from costing anything at all.
bool note_pending_any(uint32_t ns_id);

// Attach delivery observers to every running bot and to any bot that
// starts later; drop them all again at teardown.
bool note_deliver_start(void);
void note_deliver_stop(void);

// ---- Command surface (note_cmds.c) --------------------------------- //

bool note_commands_register(void);
void note_commands_unregister(void);

#endif // NOTE_INTERNAL

#endif // BM_NOTE_H
