// botmanager — MIT
// Cases for db.h's three silent-wrongness surfaces: a transaction that
// reports the opposite of what it did, a value that reaches the engine
// as syntax instead of data, and a pooled connection whose server has
// gone out from under it.
#include "test.h"
#include "db.h"
#include "alloc.h"
#include "common.h"

#include <string.h>

// A driver that runs no SQL. It records what the facade asked of it,
// which is the whole point: once a real engine is underneath, the
// facade's decisions — which statement it declined to send, which verb
// it sent instead of the one asked for — are invisible from the
// outside. Every case reads that record.
#define LOG_SZ  512

static char  fake_log[LOG_SZ];
static char  fake_fail_sql[128];   // the one statement that reports failure

// The server's identity. Dialling copies it into the handle, so
// bumping it is a server going away and taking every open connection
// with it — which is what happened on 2026-09-06, when the operator
// moved Postgres to a new container at the same address.
static uint32_t fake_server_gen = 1;
static bool     fake_reset_fails;  // the redial cannot find a server either

typedef struct
{
  uint32_t gen;       // the server this handle was dialled against
  bool     noticed;   // an I/O attempt has since discovered it is gone
} fake_conn_t;

static void
fake_note(const char *s)
{
  if(fake_log[0] != '\0')
    strlcat(fake_log, "|", sizeof(fake_log));

  strlcat(fake_log, s, sizeof(fake_log));
}

// True once this handle's server is gone. Nothing here notices that on
// its own — see fake_ping.
static bool
fake_dead(fake_conn_t *c)
{
  return(c->gen != fake_server_gen);
}

static void *
fake_connect(const char *host, uint16_t port, const char *dbname,
    const char *user, const char *pass)
{
  fake_conn_t *c;

  (void)host; (void)port; (void)dbname; (void)user; (void)pass;

  c = mem_alloc("test", "fake_conn", sizeof(fake_conn_t));

  c->gen     = fake_server_gen;
  c->noticed = false;

  return(c);
}

static void
fake_disconnect(void *handle)
{
  mem_free(handle);
}

// Measured against libpq on 2026-09-06 and modelled on it: a client
// library does not learn its peer is gone until an I/O attempt fails,
// and from then on it says so on every call. Both halves matter — the
// first is why the pool cannot detect an outage at acquire time alone,
// the second is why it can detect one at all.
static bool
fake_ping(void *handle)
{
  fake_conn_t *c = handle;

  return(fake_dead(c) && c->noticed ? FAIL : SUCCESS);
}

static bool
fake_reset(void *handle)
{
  fake_conn_t *c = handle;

  if(fake_reset_fails)
    return(FAIL);

  fake_note("RESET");

  c->gen     = fake_server_gen;
  c->noticed = false;

  return(SUCCESS);
}

// Every statement path funnels its outcome through here so a dead
// server means the same thing to all of them.
static bool
fake_attempt(fake_conn_t *c, db_result_t *result, const char *sql)
{
  if(fake_dead(c))
  {
    c->noticed = true;
    result->ok = false;
    strlcpy(result->error, "no connection to the server", DB_ERROR_SZ);
    return(FAIL);
  }

  if(sql != NULL && strcmp(sql, fake_fail_sql) == 0)
  {
    result->ok = false;
    strlcpy(result->error, "fake failure", DB_ERROR_SZ);
    return(FAIL);
  }

  result->ok = true;
  return(SUCCESS);
}

static bool
fake_query(void *handle, const char *sql, db_result_t *result)
{
  fake_note(sql);
  return(fake_attempt(handle, result, sql));
}

// Records the values as handed over, byte for byte. A driver that had
// quoted or escaped them would show it here.
static bool
fake_query_params(void *handle, const char *sql, const char *const *params,
    uint16_t n_params, db_result_t *result)
{
  fake_note(sql);

  for(uint16_t i = 0; i < n_params; i++)
    fake_note(params[i] != NULL ? params[i] : "(null)");

  return(fake_attempt(handle, result, sql));
}

static bool
fake_txn(void *handle, db_txn_op_t op, char *err, size_t err_cap)
{
  (void)err; (void)err_cap;

  switch(op)
  {
    case DB_TXN_BEGIN:  fake_note("BEGIN");  break;
    case DB_TXN_COMMIT: fake_note("COMMIT"); break;
    default:            fake_note("ROLLBACK");
  }

  if(fake_dead(handle))
  {
    ((fake_conn_t *)handle)->noticed = true;
    return(FAIL);
  }

  return(SUCCESS);
}

static char *
fake_escape(void *handle, const char *input)
{
  (void)handle; (void)input;
  return(NULL);
}

static const char *
fake_error(void *handle)
{
  (void)handle;
  return("fake");
}

static const db_driver_t fake_driver = {
  .name         = "fake",
  .connect      = fake_connect,
  .disconnect   = fake_disconnect,
  .ping         = fake_ping,
  .reset        = fake_reset,
  .query        = fake_query,
  .query_stream = NULL,
  .query_params = fake_query_params,
  .txn          = fake_txn,
  .escape       = fake_escape,
  .error        = fake_error,
};

// One statement through a transaction, discarding the result the cases
// never look at.
static bool
step(db_txn_t *txn, const char *sql)
{
  db_result_t *res = db_result_alloc();
  bool         ok  = db_txn_query(txn, sql, res);

  db_result_free(res);
  return(ok);
}

// The cases. Each drives the facade and returns what the facade
// returned; the table carries that plus the record the driver kept.

static bool
case_commit(void)
{
  db_txn_t *txn = db_txn_begin(NULL, 0);

  step(txn, "one");
  return(db_txn_commit(txn));
}

// The headline: PostgreSQL aborts a transaction on its first error and
// then reports the COMMIT as successful. A commit that returned SUCCESS
// here would be data loss nothing logs.
static bool
case_commit_after_failure(void)
{
  db_txn_t *txn = db_txn_begin(NULL, 0);

  strlcpy(fake_fail_sql, "two", sizeof(fake_fail_sql));

  step(txn, "one");
  step(txn, "two");
  return(db_txn_commit(txn));
}

static bool
case_statement_after_failure(void)
{
  db_txn_t *txn = db_txn_begin(NULL, 0);
  bool      ok;

  strlcpy(fake_fail_sql, "two", sizeof(fake_fail_sql));

  step(txn, "one");
  step(txn, "two");
  ok = step(txn, "three");

  db_txn_rollback(txn);
  return(ok);
}

static bool
case_rollback(void)
{
  db_txn_t *txn = db_txn_begin(NULL, 0);

  step(txn, "one");
  db_txn_rollback(txn);
  return(SUCCESS);
}

static bool
case_no_txn(void)
{
  return(step(NULL, "one"));
}

static bool
case_params_verbatim(void)
{
  const char  *params[2] = { "o'brien", "; DROP TABLE dossier --" };
  db_result_t *res       = db_result_alloc();
  bool         ok        = db_query_params("SELECT $1, $2", params, 2, res);

  db_result_free(res);
  return(ok);
}

static bool
case_params_null_element(void)
{
  const char  *params[2] = { "a", NULL };
  db_result_t *res       = db_result_alloc();
  bool         ok        = db_query_params("SELECT $1, $2", params, 2, res);

  db_result_free(res);
  return(ok);
}

static bool
case_params_too_many(void)
{
  const char  *params[DB_PARAMS_MAX + 1] = {0};
  db_result_t *res                       = db_result_alloc();
  bool         ok;

  for(uint16_t i = 0; i < DB_PARAMS_MAX + 1; i++)
    params[i] = "x";

  ok = db_query_params("SELECT 1", params, DB_PARAMS_MAX + 1, res);

  db_result_free(res);
  return(ok);
}

// The pool cases. Each first runs a statement to be sure a live
// connection is pooled, then takes the server away underneath it.

static bool
warm_pool(void)
{
  db_result_t *res = db_result_alloc();
  bool         ok  = db_query("warm", res);

  db_result_free(res);
  fake_log[0] = '\0';   // the warm-up is setup, not evidence
  return(ok);
}

static bool
run_after_outage(const char *sql)
{
  db_result_t *res;
  bool         ok;

  warm_pool();
  fake_server_gen++;

  res = db_result_alloc();
  ok  = db_query(sql, res);
  db_result_free(res);

  return(ok);
}

// The headline: before 2026-09-06 this statement failed, the slot went
// back to the pool still holding the corpse, and every statement after
// it failed the same way until a human restarted the daemon.
static bool
case_dead_redialled(void)
{
  return(run_after_outage("one"));
}

// The other half of the same rule: an ordinary SQL error must cost
// exactly one attempt. A pool that redialled on every failure would
// double the traffic of every broken statement in the tree.
static bool
case_live_failure_not_redialled(void)
{
  db_result_t *res;
  bool         ok;

  warm_pool();
  strlcpy(fake_fail_sql, "two", sizeof(fake_fail_sql));

  res = db_result_alloc();
  ok  = db_query("two", res);
  db_result_free(res);

  return(ok);
}

// A redial that cannot find a server either: the statement fails, and
// it fails once.
static bool
case_dead_redial_fails(void)
{
  bool ok;

  fake_reset_fails = true;
  ok = run_after_outage("one");
  fake_reset_fails = false;

  return(ok);
}

static bool
case_dead_params_redialled(void)
{
  const char  *params[1] = { "v" };
  db_result_t *res;
  bool         ok;

  warm_pool();
  fake_server_gen++;

  res = db_result_alloc();
  ok  = db_query_params("SELECT $1", params, 1, res);
  db_result_free(res);

  return(ok);
}

// ⛔ The one path that must never be redialled. A reset drops the
// server-side transaction, so a retry here would discard every
// statement already issued and let the COMMIT after it report success
// over nothing. No RESET may appear in this case's record.
static bool
case_txn_never_redialled(void)
{
  db_txn_t *txn;

  warm_pool();
  txn = db_txn_begin(NULL, 0);

  fake_server_gen++;
  step(txn, "one");

  return(db_txn_commit(txn));
}

static bool
case_params_absent_array(void)
{
  db_result_t *res = db_result_alloc();
  bool         ok  = db_query_params("SELECT $1", NULL, 1, res);

  db_result_free(res);
  return(ok);
}

static const struct
{
  const char *name;
  bool      (*run)(void);
  bool        want_ret;
  const char *want_log;
} cases[] = {
  { "a clean transaction commits", case_commit, SUCCESS,
    "BEGIN|one|COMMIT" },
  { "a failed statement rolls back and says so", case_commit_after_failure,
    FAIL, "BEGIN|one|two|ROLLBACK" },
  { "nothing runs after a failure", case_statement_after_failure, FAIL,
    "BEGIN|one|two|ROLLBACK" },
  { "rollback is rollback", case_rollback, SUCCESS, "BEGIN|one|ROLLBACK" },
  { "no transaction, no statement", case_no_txn, FAIL, "" },
  // Quotes and a statement terminator survive intact and separate: they
  // were never part of the SQL to begin with.
  { "values reach the driver verbatim", case_params_verbatim, SUCCESS,
    "SELECT $1, $2|o'brien|; DROP TABLE dossier --" },
  { "a NULL element stays NULL", case_params_null_element, SUCCESS,
    "SELECT $1, $2|a|(null)" },
  { "too many parameters never reach the driver", case_params_too_many,
    FAIL, "" },
  { "a count with no array never reaches the driver",
    case_params_absent_array, FAIL, "" },
  // A pooled connection outlives the server it was dialled against, so
  // the facade redials it under the statement rather than handing the
  // failure on.
  { "a dead connection is redialled and the statement re-runs",
    case_dead_redialled, SUCCESS, "one|RESET|one" },
  { "an ordinary failure costs one attempt",
    case_live_failure_not_redialled, FAIL, "two" },
  { "a redial that fails does not re-run the statement",
    case_dead_redial_fails, FAIL, "one" },
  { "bound parameters are redialled too", case_dead_params_redialled,
    SUCCESS, "SELECT $1|v|RESET|SELECT $1|v" },
  { "a transaction is never redialled", case_txn_never_redialled, FAIL,
    "BEGIN|one|ROLLBACK" },
};

int
main(void)
{
  // The facade allocates results and handles through the tracked
  // allocator, which aborts rather than returning NULL.
  mem_init();

  if(db_init(&fake_driver) != SUCCESS)
    return(test_skip("db_txn", "db_init refused the fake driver"));

  for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
  {
    bool got;

    fake_log[0]      = '\0';
    fake_fail_sql[0] = '\0';
    fake_reset_fails = false;

    got = cases[i].run();

    test_check_bool("facade", cases[i].name, cases[i].want_ret, got);
    test_check_str("driver saw", cases[i].name, cases[i].want_log, fake_log);
  }

  db_exit();
  return(test_report("db_txn"));
}
