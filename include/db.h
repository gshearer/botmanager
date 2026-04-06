#ifndef BM_DB_H
#define BM_DB_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define DB_ERROR_SZ  256

// Connection states.
typedef enum
{
  DB_CONN_IDLE,     // connected, not in use
  DB_CONN_ACTIVE,   // claimed by a query
  DB_CONN_FAIL      // connection has failed
} db_conn_state_t;

// Query result: row/column data returned by a query.
// Callers must call db_result_free() when done.
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

// Callback for async queries. Receives result and user data.
// The callback is responsible for calling db_result_free().
typedef void (*db_cb_t)(db_result_t *result, void *data);

// Driver interface: functions a DB plugin must implement.
typedef struct
{
  const char *name;

  // returns: opaque handle or NULL on failure
  // host: database host
  // port: database port
  // dbname: database name
  // user: database user
  // pass: database password
  void *(*connect)(const char *host, uint16_t port, const char *dbname,
                   const char *user, const char *pass);

  // Close a connection and free driver-specific resources.
  // handle: opaque connection handle
  void (*disconnect)(void *handle);

  // returns: SUCCESS or FAIL
  // handle: opaque connection handle
  bool (*ping)(void *handle);

  // returns: SUCCESS or FAIL
  // handle: opaque connection handle
  bool (*reset)(void *handle);

  // returns: SUCCESS or FAIL
  // handle: opaque connection handle
  // sql: SQL query string
  // result: destination for row/column data
  bool (*query)(void *handle, const char *sql, db_result_t *result);

  // returns: mem_alloc'd escaped string (caller frees)
  // handle: opaque connection handle
  // input: string to escape
  char *(*escape)(void *handle, const char *input);

  // returns: last error message from the driver
  // handle: opaque connection handle
  const char *(*error)(void *handle);
} db_driver_t;

// Pool statistics (thread-safe snapshot).
typedef struct
{
  uint16_t total;     // slots with live connections
  uint16_t idle;      // idle connections
  uint16_t active;    // connections currently in use
  uint16_t failed;    // connections in failed state
  uint64_t queries;   // lifetime query count
  uint64_t errors;    // lifetime query errors
} db_pool_stats_t;

// Override pool configuration. Must be called before db_init().
// Defaults: max_conns=10, idle_timeout=300s, reap_interval=60s.
// max_conns: maximum connections in pool
// idle_timeout_secs: seconds before idle connection is reaped
// reap_interval_secs: seconds between reaper runs
void db_set_pool_config(uint16_t max_conns, uint32_t idle_timeout_secs,
    uint32_t reap_interval_secs);

// returns: SUCCESS or FAIL
// drv: database driver interface (must not be NULL)
bool db_init(const db_driver_t *drv);

// Shut down the DB subsystem. Closes all connections, frees pool.
void db_exit(void);

// returns: zeroed result struct
db_result_t *db_result_alloc(void);

// Free a result struct and all its dynamic data.
// r: result to free (NULL is a no-op)
void db_result_free(db_result_t *r);

// returns: cell value string, or NULL if out of bounds
// r: query result
// row: row index
// col: column index
const char *db_result_get(const db_result_t *r, uint32_t row, uint32_t col);

// returns: column name string, or NULL if out of bounds
// r: query result
// col: column index
const char *db_result_col_name(const db_result_t *r, uint32_t col);

// Allocate internal storage for rows and columns in a result.
// r: result to populate
// rows: number of rows
// cols: number of columns
void db_result_set_size(db_result_t *r, uint32_t rows, uint32_t cols);

// Set a column name (mem_strdup'd internally).
// r: result to populate
// col: column index
// name: column name string
void db_result_set_col_name(db_result_t *r, uint32_t col, const char *name);

// Set a cell value (mem_strdup'd internally). NULL for SQL NULL.
// r: result to populate
// row: row index
// col: column index
// val: cell value string (or NULL)
void db_result_set_value(db_result_t *r, uint32_t row, uint32_t col,
    const char *val);

// returns: SUCCESS or FAIL
// sql: SQL query string
// result: pre-allocated result struct
bool db_query(const char *sql, db_result_t *result);

// returns: SUCCESS (task submitted) or FAIL
// sql: SQL query string
// cb: callback to receive the result
// data: opaque user data passed to callback
bool db_query_async(const char *sql, db_cb_t cb, void *data);

// returns: mem_alloc'd escaped string (caller frees), or NULL on failure
// input: string to escape
char *db_escape(const char *input);

// Get pool statistics (thread-safe snapshot).
// out: destination for the snapshot
void db_get_pool_stats(db_pool_stats_t *out);

// Pool slot iteration callback type. Invoked once per pool slot
// while the slot's mutex is held — must be fast.
typedef void (*db_pool_iter_cb_t)(uint16_t slot, db_conn_state_t state,
    uint64_t queries, time_t created, time_t last_used, void *data);

// Iterate database connection pool slots. Thread-safe.
// cb: callback invoked for each pool slot that has been used
// data: opaque user data passed to callback
void db_iterate_pool(db_pool_iter_cb_t cb, void *data);

#ifdef DB_INTERNAL

#include "common.h"
#include "bconf.h"
#include "clam.h"
#include "mem.h"
#include "task.h"

// Internal connection struct.
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

// Pool configuration.
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

// DB credentials (copied from bconf at init time).
typedef struct
{
  char     host[256];
  char     dbname[128];
  char     user[128];
  char     pass[256];
  uint16_t port;
} db_creds_t;

static db_creds_t creds;

// Module state.
static const db_driver_t *driver   = NULL;
static db_conn_t         *pool     = NULL;
static bool               db_ready = false;

// Lifetime counters (atomic, no lock needed).
static uint64_t           db_stat_queries = 0;
static uint64_t           db_stat_errors  = 0;

// Async query context (passed through task system).
typedef struct
{
  char    *sql;
  db_cb_t  cb;
  void    *data;
} db_async_ctx_t;

#endif // DB_INTERNAL

#endif // BM_DB_H
