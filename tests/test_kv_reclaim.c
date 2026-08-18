// botmanager — MIT
// Cases for kv_reclaim_owned(): the write a plugin unload used to skip.
//
// kv_set() does not write to the database. It marks the entry dirty and
// returns; kv_flush() is the only writer, and outside the `set kv` and
// IRC command surfaces nothing calls it. That is survivable for as long
// as the entry lives, because kv_exit() flushes before the daemon goes
// down — main.c orders kv_exit ahead of plugin_exit, and plugin_exit
// dlcloses without reclaiming.
//
// An unload is the case that is not survivable. kv_reclaim_owned() frees
// every entry naming the departing mapping, and until OBS-59 it did so
// without ever consulting `dirty` — so a value a plugin had set reached
// no row, and the free took the evidence with it. Nothing logged it, and
// the key came back at whatever the database still held.
//
// Measured on whenmoon's strategy binding, and the direction that makes
// it a defect rather than an inconvenience is the second one: an operator
// who DETACHED a strategy had it re-attach itself across the next
// `/plugin reload whenmoon`, because the clear never landed and
// kv_register adopted the older row. Silent, durable, and the wrong way
// round — which is this directory's bar.
//
// Observing it needs no Postgres: the DB facade takes a driver, so these
// cases install one that runs no SQL and records what it was asked, then
// read that record. The statements sent are the subject, not any rows.

#include "test.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "db.h"
#include "kv.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SUITE "kv_reclaim"

// A driver that runs no SQL and keeps what it was handed.

#define LOG_SZ 4096

static char rec_log[LOG_SZ];
static int  rec_handle;   // an address, never dereferenced

static void *
rec_connect(const char *host, uint16_t port, const char *dbname,
    const char *user, const char *pass)
{
  (void)host; (void)port; (void)dbname; (void)user; (void)pass;
  return(&rec_handle);
}

static void
rec_disconnect(void *handle)
{
  (void)handle;
}

static bool
rec_ping(void *handle)
{
  (void)handle;
  return(SUCCESS);
}

static bool
rec_query(void *handle, const char *sql, db_result_t *result)
{
  (void)handle;

  strlcat(rec_log, sql, sizeof(rec_log));
  strlcat(rec_log, "|", sizeof(rec_log));

  result->ok = true;
  return(SUCCESS);
}

// persist_entry frees what this returns and reads NULL as a failure to
// build the statement, so the recorder has to hand back real storage —
// an escape answering NULL would suppress the very statements these
// cases read.
static char *
rec_escape(void *handle, const char *input)
{
  (void)handle;
  return(mem_strdup("test", "escape", input));
}

static const char *
rec_error(void *handle)
{
  (void)handle;
  return("recorder");
}

static const db_driver_t rec_driver = {
  .name         = "recorder",
  .connect      = rec_connect,
  .disconnect   = rec_disconnect,
  .ping         = rec_ping,
  .reset        = NULL,
  .query        = rec_query,
  .query_stream = NULL,
  .query_params = NULL,
  .txn          = NULL,
  .escape       = rec_escape,
  .error        = rec_error,
};

static void
rec_clear(void)
{
  rec_log[0] = '\0';
}

static bool
rec_saw(const char *fragment)
{
  return(strstr(rec_log, fragment) != NULL);
}

// A mapping to be reclaimed. kv_register_owned takes any address inside
// the owner's mapping, and the range these cases reclaim is this
// object's own address — a DATA address, so it cannot collide with the
// .text return addresses the plain kv_register() attributes its entries
// to, and the "another owner" case below is a real distinction rather
// than a lucky one.
static const char departing = 0;

static uintptr_t
departing_lo(void)
{
  return((uintptr_t)&departing);
}

// The row the whole thing was filed for: a value set through the API,
// never flushed, and its owner goes away.
static void
case_a_dirty_value_is_written_before_its_owner_is_unmapped(void)
{
  kv_register_owned("t.bind", KV_STR, "", NULL, NULL, "", &departing);

  test_check_bool(SUITE, "the value takes",
      SUCCESS, kv_set("t.bind", "mako"));

  rec_clear();

  test_check_sz(SUITE, "the entry is reclaimed", 1,
      (size_t)kv_reclaim_owned(departing_lo(), departing_lo() + 1));

  test_check_bool(SUITE, "and its unsaved value was written on the way out",
      true, rec_saw("INSERT INTO kv"));
  test_check_bool(SUITE, "carrying the value that was set, by key",
      true, rec_saw("'t.bind'"));
  test_check_bool(SUITE, "and by value — the point of the write",
      true, rec_saw("'mako'"));
}

// The direction that makes it a defect. A cleared binding that never
// reaches a row is not "no binding": the next kv_register adopts
// whatever the database still holds, so the strategy an operator
// detached comes back attached.
static void
case_a_cleared_value_is_written_too(void)
{
  kv_register_owned("t.clr", KV_STR, "", NULL, NULL, "", &departing);
  kv_set("t.clr", "mako");
  kv_flush();

  test_check_bool(SUITE, "the clear takes",
      SUCCESS, kv_set("t.clr", ""));

  rec_clear();
  kv_reclaim_owned(departing_lo(), departing_lo() + 1);

  test_check_bool(SUITE, "the emptied value is written, not dropped",
      true, rec_saw("'t.clr'"));

  // Built rather than spelled: the type column is the enum's ordinal,
  // and a literal here would pass until someone adds a kv_type_t.
  {
    char expect[64];

    snprintf(expect, sizeof(expect), "'t.clr', %d, ''", (int)KV_STR);

    test_check_bool(SUITE, "and it is stored empty — a stale row here is "
        "what re-attaches a detached strategy",
        true, rec_saw(expect));
  }
}

// The reclaim is still a reclaim. A persist that left the entry behind
// would trade a silent loss for a use-after-unmap.
static void
case_the_entry_is_still_gone(void)
{
  kv_register_owned("t.gone", KV_STR, "x", NULL, NULL, "", &departing);
  kv_set("t.gone", "y");

  kv_reclaim_owned(departing_lo(), departing_lo() + 1);

  test_check_bool(SUITE, "the key does not survive its owner",
      false, kv_exists("t.gone"));
}

// A clean entry owes nothing, and an unload is not an excuse to rewrite
// every row the plugin ever read.
static void
case_a_clean_entry_costs_no_write(void)
{
  kv_register_owned("t.clean", KV_STR, "x", NULL, NULL, "", &departing);
  kv_set("t.clean", "tuned");
  kv_flush();

  rec_clear();
  kv_reclaim_owned(departing_lo(), departing_lo() + 1);

  test_check_bool(SUITE, "a flushed entry is not written again",
      false, rec_saw("'t.clean'"));
}

// The range is the whole authority on what leaves. A reclaim that
// persisted or freed beyond it would take another plugin's keys with it.
static void
case_another_owners_key_is_untouched(void)
{
  kv_register("t.other", KV_STR, "x", NULL, NULL, "");
  kv_set("t.other", "mine");

  rec_clear();
  kv_reclaim_owned(departing_lo(), departing_lo() + 1);

  test_check_bool(SUITE, "a key owned elsewhere stays registered",
      true, kv_exists("t.other"));
  test_check_str(SUITE, "with its value intact",
      "mine", kv_get_str("t.other"));
  test_check_bool(SUITE, "and nothing was written on its behalf",
      false, rec_saw("'t.other'"));
}

int
main(void)
{
  mem_init();
  clam_init();
  kv_init();

  if(db_init(&rec_driver) != SUCCESS)
    return(test_skip(SUITE, "db_init refused the recording driver"));

  // Both kv_flush() and the reclaim's own write refuse to persist before
  // kv_load() has run, and every case here reads what was sent. The
  // recorder answers the restore query with no rows, so this is the load
  // and nothing else.
  if(kv_load() != SUCCESS)
    return(test_skip(SUITE, "kv_load refused the recording driver"));

  case_a_dirty_value_is_written_before_its_owner_is_unmapped();
  case_a_cleared_value_is_written_too();
  case_the_entry_is_still_gone();
  case_a_clean_entry_costs_no_write();
  case_another_owners_key_is_untouched();

  return(test_report(SUITE));
}
