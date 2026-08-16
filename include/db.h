#ifndef BM_DB_H
#define BM_DB_H

#include "async.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define DB_ERROR_SZ  256

// Bind parameters per statement. A parameter carries a value, never
// syntax: the driver sends it out of band, so nothing in it can be read
// as SQL and no escape is owed. Placeholders are `$1`..`$n`, `params[i]`
// binds `$(i + 1)`, a NULL element is SQL NULL, and one call carries
// exactly one statement.
#define DB_PARAMS_MAX  32

typedef enum
{
  DB_CONN_IDLE,     // connected, not in use
  DB_CONN_ACTIVE,   // claimed by a query
  DB_CONN_FAIL      // connection has failed
} db_conn_state_t;

// Row/column data returned by a query. Callers must call
// db_result_free() when done.
typedef struct db_result
{
  bool       ok;                  // true if query succeeded
  char       error[DB_ERROR_SZ];  // error message on failure
  uint32_t   rows;                // number of result rows
  uint32_t   cols;                // number of result columns
  uint32_t   rows_affected;       // rows affected (INSERT/UPDATE/DELETE)
  char     **col_names;           // col_names[col]
  char     **data;                // data[row * cols + col]
} db_result_t;

// The callback is responsible for calling db_result_free().
typedef void (*db_cb_t)(db_result_t *result, void *data);

// Row callback for the streaming read path (db_query_stream). values[c]
// is the NUL-terminated text of column c, or NULL for SQL NULL; the
// pointers are valid ONLY for the duration of this call (they point into
// the driver's per-row buffer, freed right after). Return true to keep
// streaming, false to stop further callback invocations.
typedef bool (*db_row_cb_t)(uint32_t row, uint32_t cols,
    const char *const *values, void *data);

// Transaction control verb, passed to the driver's txn() entry point.
typedef enum
{
  DB_TXN_BEGIN,
  DB_TXN_COMMIT,
  DB_TXN_ROLLBACK
} db_txn_op_t;

// Functions a DB plugin must implement.
typedef struct
{
  const char *name;

  void *(*connect)(const char *host, uint16_t port, const char *dbname,
                   const char *user, const char *pass);

  void (*disconnect)(void *handle);
  bool (*ping)(void *handle);
  bool (*reset)(void *handle);
  bool (*query)(void *handle, const char *sql, db_result_t *result);

  // Optional streaming read - NULL if the driver doesn't implement it.
  // Invokes row_cb once per row without materializing a db_result_t.
  bool (*query_stream)(void *handle, const char *sql,
      db_row_cb_t row_cb, void *data, char *err, size_t err_cap);

  // Optional bound-parameter execution - NULL if the driver has none.
  // n_params is already bounded by DB_PARAMS_MAX when this is called.
  bool (*query_params)(void *handle, const char *sql,
      const char *const *params, uint16_t n_params, db_result_t *result);

  // Optional transaction control - NULL if the driver has none. The
  // caller pins this handle for the whole BEGIN..COMMIT/ROLLBACK span,
  // so the driver must suspend any per-query transaction cleanup of its
  // own between them: an open transaction is the caller's intent here,
  // not the leak it looks like on the one-statement path.
  bool (*txn)(void *handle, db_txn_op_t op, char *err, size_t err_cap);

  // Returns a mem_alloc'd escaped string (caller frees).
  char *(*escape)(void *handle, const char *input);

  // Last error message from the driver.
  const char *(*error)(void *handle);
} db_driver_t;

typedef struct
{
  uint16_t total;     // slots with live connections
  uint16_t idle;      // idle connections
  uint16_t active;    // connections currently in use
  uint16_t failed;    // connections in failed state
  uint64_t queries;   // lifetime query count
  uint64_t errors;    // lifetime query errors
} db_pool_stats_t;

// Must be called before db_init(). Defaults: max_conns=10,
// idle_timeout=300s, reap_interval=60s.
void db_set_pool_config(uint16_t max_conns, uint32_t idle_timeout_secs,
    uint32_t reap_interval_secs);

bool db_init(const db_driver_t *drv);

// Closes all connections, frees pool.
void db_exit(void);

db_result_t *db_result_alloc(void);

// NULL is a no-op.
void db_result_free(db_result_t *r);

// Returns NULL if out of bounds.
const char *db_result_get(const db_result_t *r, uint32_t row, uint32_t col);

// Returns NULL if out of bounds.
const char *db_result_col_name(const db_result_t *r, uint32_t col);

void db_result_set_size(db_result_t *r, uint32_t rows, uint32_t cols);

// name is mem_strdup'd internally.
void db_result_set_col_name(db_result_t *r, uint32_t col, const char *name);

// val is mem_strdup'd internally. NULL for SQL NULL.
void db_result_set_value(db_result_t *r, uint32_t row, uint32_t col,
    const char *val);

bool db_query(const char *sql, db_result_t *result);

// Run `sql` on a task worker and hand the result to `cb`. `sql` is
// copied; the caller may free it on return.
//
//   ASYNC_AIRBORNE           — queued; `cb` runs exactly once, later,
//                              on a task worker. Never synchronously,
//                              so `data` may be owned by the callback
//                              from this point.
//   ASYNC_FAILED_UNDELIVERED — refused before anything was queued: the
//                              DB is not ready, no driver is bound, or
//                              `sql`/`cb` is NULL. The only failure
//                              this can return.
async_rc_t db_query_async(const char *sql, db_cb_t cb, void *data);

// Stream a SELECT row-by-row without materializing a db_result_t. Runs
// synchronously on the calling thread; row_cb is invoked once per row
// before this returns. Returns SUCCESS if the query ran and all rows
// were delivered (or row_cb asked to stop), FAIL on no-driver /
// unsupported / query error (err filled when non-NULL).
bool db_query_stream(const char *sql, db_row_cb_t row_cb, void *data,
    char *err, size_t err_cap);

// Run one statement with its values bound out of band. Prefer this to
// db_escape() + "%s" wherever a value comes from anywhere but this
// source file: there is no escape to forget and no quoting to get
// wrong. Returns FAIL when the driver has no parameter support, when
// n_params exceeds DB_PARAMS_MAX, or on the ordinary query failures —
// `result->error` carries which.
bool db_query_params(const char *sql, const char *const *params,
    uint16_t n_params, db_result_t *result);

// db_query_async() with values bound out of band. `sql` and every
// non-NULL parameter are copied; the caller may free them on return.
// Return values are db_query_async()'s, plus ASYNC_FAILED_UNDELIVERED
// when n_params exceeds DB_PARAMS_MAX or a NULL params array is paired
// with a non-zero count.
async_rc_t db_query_params_async(const char *sql, const char *const *params,
    uint16_t n_params, db_cb_t cb, void *data);

// A transaction pins one pool connection from db_txn_begin() until
// db_txn_commit() or db_txn_rollback(), either of which frees the
// handle. The pin is what makes the statements one unit, so it is also
// the cost: hold it for the mutation and nothing else — never across an
// async wait, a network call, or a second db_txn_begin() on the same
// thread. Statements issued through db_query() meanwhile take a
// different connection and are NOT part of the transaction.
typedef struct db_txn db_txn_t;

// Returns NULL when the driver has no transaction support, when no
// connection is free, or when BEGIN itself fails (err filled when
// non-NULL).
db_txn_t *db_txn_begin(char *err, size_t err_cap);

bool db_txn_query(db_txn_t *txn, const char *sql, db_result_t *result);

bool db_txn_query_params(db_txn_t *txn, const char *sql,
    const char *const *params, uint16_t n_params, db_result_t *result);

// Commit and free. Returns FAIL — having rolled back instead — if any
// statement on this handle failed: an engine that aborts a transaction
// on its first error reports the later COMMIT as a success, and that
// success means the opposite of what it says.
bool db_txn_commit(db_txn_t *txn);

// Roll back and free. NULL is a no-op.
void db_txn_rollback(db_txn_t *txn);

// Returns a mem_alloc'd escaped string (caller frees), or NULL on failure.
char *db_escape(const char *input);

void db_get_pool_stats(db_pool_stats_t *out);

// Invoked once per pool slot while the slot's mutex is held — must be fast.
typedef void (*db_pool_iter_cb_t)(uint16_t slot, db_conn_state_t state,
    uint64_t queries, time_t created, time_t last_used, void *data);

void db_iterate_pool(db_pool_iter_cb_t cb, void *data);

// The driver vtable currently installed by db_init(), or NULL. It lives
// in the DB plugin's mapping, so plugin teardown audits must be able to
// see it.
const db_driver_t *db_audit_driver(void);

#ifdef DB_INTERNAL

#include "common.h"
#include "bconf.h"
#include "clam.h"
#include "alloc.h"
#include "task.h"

typedef struct
{
  uint16_t          id;
  db_conn_state_t   state;
  time_t            created;
  time_t            last_used;
  uint64_t          queries;
  void             *handle;      // driver-specific handle
  pthread_mutex_t   mutex;
} db_conn_t;

typedef struct
{
  uint16_t max_conns;
  uint32_t idle_timeout;
  uint32_t reap_interval;
} db_pcfg_t;

static db_pcfg_t db_pcfg = {
  .max_conns     = 10,
  .idle_timeout  = 300,
  .reap_interval = 60,
};

// Copied from bconf at init time.
typedef struct
{
  char     host[256];
  char     dbname[128];
  char     user[128];
  char     pass[256];
  uint16_t port;
} db_creds_t;

static db_creds_t creds;

static const db_driver_t *driver   = NULL;
static db_conn_t         *pool     = NULL;
static bool               db_ready = false;

// Atomic, no lock needed.
static uint64_t           db_stat_queries = 0;
static uint64_t           db_stat_errors  = 0;

// Passed through task system. `params` is NULL on the plain path; on
// the bound path it holds n_params deep copies, any of which may be
// NULL for SQL NULL.
typedef struct
{
  char     *sql;
  char    **params;
  uint16_t  n_params;
  db_cb_t   cb;
  void     *data;
} db_async_ctx_t;

// A pinned connection plus the one thing COMMIT cannot be trusted to
// report: whether a statement on it already failed.
struct db_txn
{
  db_conn_t *conn;      // locked and ACTIVE for the life of the handle
  bool       failed;
};

#endif // DB_INTERNAL

#endif // BM_DB_H
