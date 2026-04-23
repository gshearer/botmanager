#ifndef BM_DB_H
#define BM_DB_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define DB_ERROR_SZ  256

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

bool db_query_async(const char *sql, db_cb_t cb, void *data);

// Returns a mem_alloc'd escaped string (caller frees), or NULL on failure.
char *db_escape(const char *input);

void db_get_pool_stats(db_pool_stats_t *out);

// Invoked once per pool slot while the slot's mutex is held — must be fast.
typedef void (*db_pool_iter_cb_t)(uint16_t slot, db_conn_state_t state,
    uint64_t queries, time_t created, time_t last_used, void *data);

void db_iterate_pool(db_pool_iter_cb_t cb, void *data);

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

// Passed through task system.
typedef struct
{
  char    *sql;
  db_cb_t  cb;
  void    *data;
} db_async_ctx_t;

#endif // DB_INTERNAL

#endif // BM_DB_H
