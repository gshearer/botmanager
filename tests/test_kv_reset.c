// botmanager — MIT
// Cases for kv_reset(): the revert whose correctness is a negative.
//
// `set kv --delete <key>` sends a registered key back to the default its
// declaration named and drops its persisted row. The mechanism is two
// lines — kv_entry_t.def has held the declaration since registration —
// and the whole risk is in what the revert must NOT do: raise the dirty
// flag. kv_flush() persists every dirty entry, and cmd_set_kv flushes on
// success, so a revert routed through apply_val re-INSERTs the row it
// just deleted inside the same command (OBS-50 T4).
//
// Nothing reports that. The command answers "reverted", the running
// daemon reads the default, and the operator finds the old value back
// after the next restart with no line anywhere saying why — which is
// this directory's bar exactly: silent, durable wrongness.
//
// case_the_entry_is_left_clean is the row that states it, and it is the
// one to watch go red against an implementation that calls apply_val.
// Observing it needs no Postgres: the DB facade takes a driver, so these
// cases install one that runs no SQL and records what it was asked, and
// then read that record. The database the fixture cares about is the
// statements sent to it, not the rows in it.

#include "test.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "db.h"
#include "kv.h"

#include <string.h>

#define SUITE "kv_reset"

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

// persist_entry and kv_row_delete both free what this returns and treat
// NULL as a failure to build the statement, so the recorder has to hand
// back real storage — an escape that answered NULL would suppress the
// very statements these cases read.
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

// The change callback, counted rather than acted on.

static uint32_t cb_calls;
static char     cb_key[KV_KEY_SZ];

static void
count_cb(const char *key, void *data)
{
  (void)data;

  cb_calls++;
  strlcpy(cb_key, key, sizeof(cb_key));
}

// The ordinary case, and the one every knob is in: a value was tuned,
// the operator takes it back, and the key answers with its declaration
// again — the same value kv_get_uint_or_default has always read.
static void
case_a_registered_key_goes_back_to_its_declaration(void)
{
  kv_register("t.str", KV_STR, "en-US", NULL, NULL, "");
  kv_register("t.u32", KV_UINT32, "45", NULL, NULL, "");

  kv_set("t.str", "fr-FR");
  kv_set_uint("t.u32", 9);

  test_check_str(SUITE, "the tuned string is what is stored",
      "fr-FR", kv_get_str("t.str"));

  test_check_bool(SUITE, "a registered key is this verb's subject",
      true, kv_reset("t.str"));
  test_check_bool(SUITE, "and so is a numeric one",
      true, kv_reset("t.u32"));

  test_check_str(SUITE, "the string reads its declaration again",
      "en-US", kv_get_str("t.str"));
  test_check_sz(SUITE, "and the number reads its own",
      45, (size_t)kv_get_uint("t.u32"));

  test_check_bool(SUITE, "the key is still registered — it is a revert, "
      "not a removal", true, kv_exists("t.str"));
}

// The persisted row goes with the value, or the next boot restores what
// the revert just discarded.
static void
case_the_row_goes_with_the_value(void)
{
  kv_register("t.row", KV_STR, "shipped", NULL, NULL, "");
  kv_set("t.row", "tuned");

  rec_clear();
  kv_reset("t.row");

  test_check_bool(SUITE, "the row is dropped by exact key",
      true, rec_saw("DELETE FROM kv WHERE key = 't.row'"));
}

// ⭑ The OBS-50 T4 row. The entry carries an unflushed write — the state
// any plugin's kv_set_uint leaves it in — and the revert must clear it,
// not merely refrain from setting it. Then a flush, which is what
// cmd_set_kv does on every success and what kv_exit does at shutdown:
// nothing about this key may reach the database.
//
// Red against a kv_reset routed through apply_val: the flush that
// follows re-INSERTs the row the DELETE above just removed.
static void
case_the_entry_is_left_clean(void)
{
  kv_register("t.clean", KV_STR, "shipped", NULL, NULL, "");

  kv_set("t.clean", "tuned");   // dirty, and deliberately not flushed

  kv_reset("t.clean");

  rec_clear();
  kv_flush();

  test_check_bool(SUITE, "the flush after a revert writes nothing back",
      false, rec_saw("t.clean"));

  test_check_str(SUITE, "and the value is still the declaration",
      "shipped", kv_get_str("t.clean"));
}

// A flush is only evidence if it would otherwise have written. Same
// fixture, no revert — the row this states is what makes the row above
// mean something.
static void
case_the_flush_would_have_written(void)
{
  kv_register("t.dirty", KV_STR, "shipped", NULL, NULL, "");

  kv_set("t.dirty", "tuned");

  rec_clear();
  kv_flush();

  test_check_bool(SUITE, "an unreverted key does reach the database",
      true, rec_saw("t.dirty"));
}

// Same contract as apply_val's: the callback is a change notification,
// so a revert that changed nothing must not fire one. A knob whose
// callback rebuilds a subscription or reopens a socket is why.
static void
case_the_callback_fires_once_and_only_on_a_change(void)
{
  kv_register("t.cb", KV_STR, "shipped", count_cb, NULL, "");

  kv_set("t.cb", "tuned");
  cb_calls = 0;
  cb_key[0] = '\0';

  kv_reset("t.cb");

  test_check_sz(SUITE, "a real revert fires the callback once",
      1, (size_t)cb_calls);
  test_check_str(SUITE, "and names the key it reverted", "t.cb", cb_key);

  kv_reset("t.cb");

  test_check_sz(SUITE, "reverting an already-default key fires nothing",
      1, (size_t)cb_calls);
}

// The state almost every key in the tree is in. It is not an error and
// not a no-op either: the row is dropped whether or not the live value
// had drifted, because a row can outlive the value that wrote it.
static void
case_an_untouched_key_still_drops_its_row(void)
{
  kv_register("t.untouched", KV_UINT32, "7", NULL, NULL, "");

  rec_clear();

  test_check_bool(SUITE, "an untouched key is still found",
      true, kv_reset("t.untouched"));

  test_check_bool(SUITE, "and its row is dropped anyway",
      true, rec_saw("DELETE FROM kv WHERE key = 't.untouched'"));
}

// The boundary with /db delete kv, and it is a boundary in both
// directions: a key with no live entry has no declaration to revert to,
// so this refuses it and issues nothing. That refusal is why /db delete
// kv is still the only tool for a DB-only orphan.
static void
case_an_unregistered_key_is_not_this_verbs_subject(void)
{
  rec_clear();

  test_check_bool(SUITE, "an unregistered key is refused",
      false, kv_reset("t.nosuchkey"));

  test_check_bool(SUITE, "and no statement is issued for it",
      false, rec_saw("t.nosuchkey"));

  test_check_bool(SUITE, "a NULL key is refused", false, kv_reset(NULL));
  test_check_bool(SUITE, "an empty key is refused", false, kv_reset(""));
}

// --clear's whole type rule, and it was measured rather than chosen:
// str_to_val interns "" unconditionally on the KV_STR arm and every
// other arm fails on `end == str`. The command surface names the type in
// its refusal, but the refusal itself is this — which is why --clear
// needed no new primitive.
static void
case_only_str_holds_an_empty_value(void)
{
  kv_register("t.e.str", KV_STR,    "x",    NULL, NULL, "");
  kv_register("t.e.u32", KV_UINT32, "1",    NULL, NULL, "");
  kv_register("t.e.bool", KV_BOOL,  "true", NULL, NULL, "");
  kv_register("t.e.dbl", KV_DOUBLE, "1.5",  NULL, NULL, "");

  test_check_bool(SUITE, "an empty value takes on a string key",
      SUCCESS, kv_set("t.e.str", ""));
  test_check_str(SUITE, "and reads back empty",
      "", kv_get_str("t.e.str"));

  test_check_bool(SUITE, "a number refuses it",
      FAIL, kv_set("t.e.u32", ""));
  test_check_bool(SUITE, "a bool refuses it",
      FAIL, kv_set("t.e.bool", ""));
  test_check_bool(SUITE, "a double refuses it",
      FAIL, kv_set("t.e.dbl", ""));

  test_check_sz(SUITE, "and the refused ones kept their values",
      1, (size_t)kv_get_uint("t.e.u32"));
}

// The reply that names the restored value reads it through
// kv_get_val_str, and that getter answers with common.h's INVERTED
// SUCCESS while kv_exists and kv_reset either side of it answer with a
// predicate. A `!` on it compiles, inverts, and shows up only as a reply
// that says "?" — which is what the live gate found, so the convention
// gets a row of its own.
static void
case_the_serialized_value_reads_back(void)
{
  char val[KV_STR_SZ];

  kv_register("t.ser", KV_STR, "en-US", NULL, NULL, "");
  kv_set("t.ser", "fr-FR");
  kv_reset("t.ser");

  test_check_bool(SUITE, "serializing a live key answers SUCCESS",
      SUCCESS, kv_get_val_str("t.ser", val, sizeof(val)));
  test_check_str(SUITE, "and hands back the declaration", "en-US", val);

  test_check_bool(SUITE, "an absent key answers FAIL",
      FAIL, kv_get_val_str("t.nosuchkey", val, sizeof(val)));
}

int
main(void)
{
  mem_init();
  clam_init();
  kv_init();

  if(db_init(&rec_driver) != SUCCESS)
    return(test_skip(SUITE, "db_init refused the recording driver"));

  // kv_flush() refuses to persist before kv_load() has run, and the
  // cases above read what a flush sends. The recorder answers the
  // restore query with no rows, so this is the load and nothing else.
  if(kv_load() != SUCCESS)
    return(test_skip(SUITE, "kv_load refused the recording driver"));

  case_a_registered_key_goes_back_to_its_declaration();
  case_the_row_goes_with_the_value();
  case_the_entry_is_left_clean();
  case_the_flush_would_have_written();
  case_the_callback_fires_once_and_only_on_a_change();
  case_an_untouched_key_still_drops_its_row();
  case_an_unregistered_key_is_not_this_verbs_subject();
  case_only_str_holds_an_empty_value();
  case_the_serialized_value_reads_back();

  return(test_report(SUITE));
}
