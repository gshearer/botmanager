// botmanager — MIT
// Database abstraction facade over registered DB plugins.
#define DB_INTERNAL
#include "db.h"

// Close a connection and clear its slot.
// Connection must be locked by caller.
static void
conn_close(db_conn_t *c)
{
  if(c->handle != NULL)
  {
    driver->disconnect(c->handle);
    c->handle = NULL;
  }

  c->state = DB_CONN_IDLE;
}

// Release a connection back to the pool.
// Connection must be locked by caller.
static void
conn_release(db_conn_t *c)
{
  c->state = DB_CONN_IDLE;
  c->last_used = time(NULL);
  pthread_mutex_unlock(&c->mutex);
}

// Acquire a connection from the pool.
// Two passes: first reuse an idle connection, then create a new one.
// returns: locked connection in ACTIVE state, or NULL if none available
static db_conn_t *
conn_acquire(void)
{
  // Pass 1: find an idle connection with a live handle.
  for(uint16_t i = 0; i < db_pcfg.max_conns; i++)
  {
    db_conn_t *c = &pool[i];

    if(pthread_mutex_trylock(&c->mutex) != 0)
      continue;

    if(c->handle != NULL && c->state == DB_CONN_IDLE)
    {
      c->state = DB_CONN_ACTIVE;
      return(c);  // locked
    }

    pthread_mutex_unlock(&c->mutex);
  }

  // Pass 2: find an empty slot and establish a new connection.
  for(uint16_t i = 0; i < db_pcfg.max_conns; i++)
  {
    db_conn_t *c = &pool[i];

    if(pthread_mutex_trylock(&c->mutex) != 0)
      continue;

    if(c->handle != NULL)
    {
      pthread_mutex_unlock(&c->mutex);
      continue;
    }

    // Empty slot — try to connect.
    c->handle = driver->connect(creds.host, creds.port,
        creds.dbname, creds.user, creds.pass);

    if(c->handle == NULL)
    {
      pthread_mutex_unlock(&c->mutex);
      clam(CLAM_WARN, "db", "connect #%u failed: %s",
          c->id, driver->error(NULL));
      return(NULL);
    }

    c->state = DB_CONN_ACTIVE;
    c->created = time(NULL);
    c->last_used = c->created;
    c->queries = 0;

    clam(CLAM_INFO, "db", "connection #%u established", c->id);
    return(c);  // locked
  }

  clam(CLAM_WARN, "db", "pool exhausted (%u max)", db_pcfg.max_conns);
  return(NULL);
}

// Report a refusal the driver never saw. Every early return on the
// query paths goes through here so the caller reads one field for the
// reason whether the statement ran or not.
static bool
result_fail(db_result_t *r, const char *why)
{
  if(r != NULL)
  {
    r->ok = false;
    strlcpy(r->error, why, sizeof(r->error));
  }

  return(FAIL);
}

// A bound-parameter call is well-formed when the count fits the array
// the drivers are built around and the array is there for every value
// the count claims.
static bool
params_ok(const char *const *params, uint16_t n_params)
{
  return(n_params <= DB_PARAMS_MAX && (params != NULL || n_params == 0));
}

static void
result_clear(db_result_t *r)
{
  if(r->col_names != NULL)
  {
    for(uint32_t i = 0; i < r->cols; i++)
      if(r->col_names[i] != NULL)
        mem_free(r->col_names[i]);

    mem_free(r->col_names);
    r->col_names = NULL;
  }

  if(r->data != NULL)
  {
    uint32_t total = r->rows * r->cols;

    for(uint32_t i = 0; i < total; i++)
      if(r->data[i] != NULL)
        mem_free(r->data[i]);

    mem_free(r->data);
    r->data = NULL;
  }
}

static void
reaper_cb(task_t *t)
{
  time_t   now;
  uint16_t reaped = 0;

  if(!db_ready)
  {
    t->state = TASK_ENDED;
    return;
  }

  now = time(NULL);

  for(uint16_t i = 0; i < db_pcfg.max_conns; i++)
  {
    db_conn_t *c = &pool[i];

    if(pthread_mutex_trylock(&c->mutex) != 0)
      continue;

    if(c->handle != NULL && c->state == DB_CONN_IDLE
        && (now - c->last_used) > (time_t)db_pcfg.idle_timeout)
    {
      clam(CLAM_DEBUG, "db", "reaping idle connection #%u "
          "(idle: %lus)", c->id, (unsigned long)(now - c->last_used));
      conn_close(c);
      reaped++;
    }

    pthread_mutex_unlock(&c->mutex);
  }

  if(reaped > 0)
    clam(CLAM_DEBUG, "db", "reaper: closed %u idle connection(s)", reaped);

  t->state = TASK_SLEEPING;
  t->sleep_until = now + db_pcfg.reap_interval;
}

static void
async_ctx_free(db_async_ctx_t *ctx)
{
  if(ctx->params != NULL)
  {
    for(uint16_t i = 0; i < ctx->n_params; i++)
      if(ctx->params[i] != NULL)
        mem_free(ctx->params[i]);

    mem_free(ctx->params);
  }

  mem_free(ctx->sql);
  mem_free(ctx);
}

// Async query task callback.
static void
async_cb(task_t *t)
{
  db_async_ctx_t *ctx = t->data;
  db_conn_t      *c;
  db_result_t    *result;
  bool            ok;

  c = conn_acquire();

  if(c == NULL)
  {
    // No connection available — retry after 1 second.
    t->state = TASK_SLEEPING;
    t->sleep_until = time(NULL) + 1;
    return;
  }

  result = db_result_alloc();

  if(ctx->params != NULL)
    ok = driver->query_params(c->handle, ctx->sql,
        (const char *const *)ctx->params, ctx->n_params, result);

  else
    ok = driver->query(c->handle, ctx->sql, result);

  __atomic_add_fetch(&db_stat_queries, 1, __ATOMIC_RELAXED);

  if(ok != SUCCESS)
    __atomic_add_fetch(&db_stat_errors, 1, __ATOMIC_RELAXED);

  c->queries++;
  conn_release(c);

  // Invoke user callback (takes ownership of result).
  ctx->cb(result, ctx->data);

  async_ctx_free(ctx);

  t->state = TASK_ENDED;
}

// Public API

// Override pool configuration. Must be called before db_init().
// idle_timeout_secs: seconds before idle connection is reaped
void
db_set_pool_config(uint16_t max_conns, uint32_t idle_timeout_secs,
    uint32_t reap_interval_secs)
{
  if(max_conns < 1)
    max_conns = 1;

  db_pcfg.max_conns     = max_conns;
  db_pcfg.idle_timeout  = idle_timeout_secs;
  db_pcfg.reap_interval = reap_interval_secs;
}

// drv: database driver interface (must not be NULL)
bool
db_init(const db_driver_t *drv)
{
  const char *v;
  int         pmax;
  int         pidle;

  if(drv == NULL)
  {
    clam(CLAM_FATAL, "db_init", "no driver provided");
    return(FAIL);
  }

  driver = drv;

  // Read credentials from bootstrap config.
  v = bconf_get("DBHOST");
  strlcpy(creds.host, v ? v : "localhost", sizeof(creds.host));

  creds.port = (uint16_t)bconf_get_int("DBPORT", 5432);

  v = bconf_get("DBNAME");

  if(v != NULL)
    strlcpy(creds.dbname, v, sizeof(creds.dbname));

  v = bconf_get("DBUSER");

  if(v != NULL)
    strlcpy(creds.user, v, sizeof(creds.user));

  v = bconf_get("DBPASS");

  if(v != NULL)
    strlcpy(creds.pass, v, sizeof(creds.pass));

  // Read optional pool configuration from bootstrap config.
  pmax = bconf_get_int("DBPOOL", 0);

  if(pmax > 0)
    db_pcfg.max_conns = (uint16_t)pmax;

  pidle = bconf_get_int("DBIDLE", 0);

  if(pidle > 0)
    db_pcfg.idle_timeout = (uint32_t)pidle;

  // Allocate connection pool.
  pool = mem_alloc("db", "pool", sizeof(db_conn_t) * db_pcfg.max_conns);

  for(uint16_t i = 0; i < db_pcfg.max_conns; i++)
  {
    pool[i].id = i;
    pool[i].state = DB_CONN_IDLE;
    pool[i].handle = NULL;
    pool[i].created = 0;
    pool[i].last_used = 0;
    pool[i].queries = 0;
    pthread_mutex_init(&pool[i].mutex, NULL);
  }

  db_ready = true;

  // Start idle connection reaper task.
  task_add("db_reaper", TASK_ANY, 200, reaper_cb, NULL);

  clam(CLAM_INFO, "db_init", "%s driver, pool: %u, idle: %us, reap: %us",
      driver->name, db_pcfg.max_conns, db_pcfg.idle_timeout,
      db_pcfg.reap_interval);

  return(SUCCESS);
}

// Shut down the database subsystem.
// Closes all pooled connections, frees the pool, and clears the driver.
void
db_exit(void)
{
  uint16_t closed = 0;

  if(!db_ready)
    return;

  db_ready = false;

  // Close all connections.

  for(uint16_t i = 0; i < db_pcfg.max_conns; i++)
  {
    db_conn_t *c = &pool[i];

    pthread_mutex_lock(&c->mutex);

    if(c->handle != NULL)
    {
      clam(CLAM_DEBUG, "db_exit", "closing connection #%u "
          "(queries: %lu)", c->id, (unsigned long)c->queries);
      conn_close(c);
      closed++;
    }

    pthread_mutex_unlock(&c->mutex);
    pthread_mutex_destroy(&c->mutex);
  }

  mem_free(pool);
  pool = NULL;
  driver = NULL;

  clam(CLAM_INFO, "db_exit", "shut down (%u connection(s) closed)", closed);
}

db_result_t *
db_result_alloc(void)
{
  return(mem_alloc("db", "result", sizeof(db_result_t)));
}

bool
db_exec(const char *sql, const char *clam_ctx)
{
  db_result_t *res = db_result_alloc();
  bool         ok;

  ok = (db_query(sql, res) == SUCCESS && res->ok) ? SUCCESS : FAIL;

  if(ok != SUCCESS)
    clam(CLAM_WARN, clam_ctx, "sql failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");

  db_result_free(res);
  return(ok);
}

void
db_result_copy(char *dst, size_t cap, const db_result_t *r, uint32_t row,
    uint32_t col)
{
  const char *s = db_result_get(r, row, col);

  strlcpy(dst, s != NULL ? s : "", cap);
}

int64_t
db_result_get_i64(const db_result_t *r, uint32_t row, uint32_t col,
    int64_t dflt)
{
  const char *s = db_result_get(r, row, col);

  return(s != NULL && s[0] != '\0' ? (int64_t)strtoll(s, NULL, 10) : dflt);
}

// Free a result struct and all its dynamic data.
// r: result to free (NULL is a no-op)
void
db_result_free(db_result_t *r)
{
  if(r == NULL)
    return;

  result_clear(r);
  mem_free(r);
}

const char *
db_result_get(const db_result_t *r, uint32_t row, uint32_t col)
{
  if(r == NULL || r->data == NULL || row >= r->rows || col >= r->cols)
    return(NULL);

  return(r->data[row * r->cols + col]);
}

const char *
db_result_col_name(const db_result_t *r, uint32_t col)
{
  if(r == NULL || r->col_names == NULL || col >= r->cols)
    return(NULL);

  return(r->col_names[col]);
}

void
db_result_set_size(db_result_t *r, uint32_t rows, uint32_t cols)
{
  r->rows = rows;
  r->cols = cols;

  if(cols > 0)
    r->col_names = mem_alloc("db", "col_names", sizeof(char *) * cols);

  if(rows > 0 && cols > 0)
    r->data = mem_alloc("db", "data", sizeof(char *) * rows * cols);
}

void
db_result_set_col_name(db_result_t *r, uint32_t col, const char *name)
{
  if(col < r->cols && name != NULL)
    r->col_names[col] = mem_strdup("db", "col_name", name);
}

void
db_result_set_value(db_result_t *r, uint32_t row, uint32_t col,
    const char *val)
{
  if(row < r->rows && col < r->cols && val != NULL)
    r->data[row * r->cols + col] = mem_strdup("db", "cell", val);
}

bool
db_query(const char *sql, db_result_t *result)
{
  db_conn_t *c;
  bool       ret;

  if(!db_ready || driver == NULL)
    return(result_fail(result, "db not initialized"));

  c = conn_acquire();

  if(c == NULL)
    return(result_fail(result, "no connection available"));

  clam(CLAM_DEBUG, "db_query", "sql: %s", sql);

  ret = driver->query(c->handle, sql, result);

  __atomic_add_fetch(&db_stat_queries, 1, __ATOMIC_RELAXED);

  if(ret != SUCCESS)
    __atomic_add_fetch(&db_stat_errors, 1, __ATOMIC_RELAXED);

  c->queries++;
  conn_release(c);

  return(ret);
}

bool
db_query_params(const char *sql, const char *const *params,
    uint16_t n_params, db_result_t *result)
{
  db_conn_t *c;
  bool       ret;

  if(!db_ready || driver == NULL || sql == NULL)
    return(result_fail(result, "db not initialized"));

  if(driver->query_params == NULL)
    return(result_fail(result, "driver lacks bound parameters"));

  if(!params_ok(params, n_params))
    return(result_fail(result, "malformed parameter list"));

  c = conn_acquire();

  if(c == NULL)
    return(result_fail(result, "no connection available"));

  clam(CLAM_DEBUG, "db_query", "sql: %s (%u param(s))", sql, n_params);

  ret = driver->query_params(c->handle, sql, params, n_params, result);

  __atomic_add_fetch(&db_stat_queries, 1, __ATOMIC_RELAXED);

  if(ret != SUCCESS)
    __atomic_add_fetch(&db_stat_errors, 1, __ATOMIC_RELAXED);

  c->queries++;
  conn_release(c);

  return(ret);
}

async_rc_t
db_query_async(const char *sql, db_cb_t cb, void *data)
{
  db_async_ctx_t *ctx;

  if(!db_ready || driver == NULL || sql == NULL || cb == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  ctx = mem_alloc("db", "async_ctx", sizeof(db_async_ctx_t));

  ctx->sql      = mem_strdup("db", "async_sql", sql);
  ctx->params   = NULL;
  ctx->n_params = 0;
  ctx->cb       = cb;
  ctx->data     = data;

  task_add("db_query", TASK_THREAD, 128, async_cb, ctx);

  clam(CLAM_DEBUG, "db_query_async", "submitted: %s", sql);
  return(ASYNC_AIRBORNE);
}

async_rc_t
db_query_params_async(const char *sql, const char *const *params,
    uint16_t n_params, db_cb_t cb, void *data)
{
  db_async_ctx_t *ctx;

  if(!db_ready || driver == NULL || sql == NULL || cb == NULL
      || driver->query_params == NULL || !params_ok(params, n_params))
    return(ASYNC_FAILED_UNDELIVERED);

  ctx = mem_alloc("db", "async_ctx", sizeof(db_async_ctx_t));

  ctx->sql      = mem_strdup("db", "async_sql", sql);
  ctx->params   = NULL;
  ctx->n_params = n_params;
  ctx->cb       = cb;
  ctx->data     = data;

  // The values must outlive the caller's frame, so they are copied
  // alongside the statement. A NULL element stays NULL — that is SQL
  // NULL, not a missing string.
  if(n_params > 0)
  {
    ctx->params = mem_alloc("db", "async_params", sizeof(char *) * n_params);

    for(uint16_t i = 0; i < n_params; i++)
      ctx->params[i] = params[i] != NULL
          ? mem_strdup("db", "async_param", params[i]) : NULL;
  }

  task_add("db_query", TASK_THREAD, 128, async_cb, ctx);

  clam(CLAM_DEBUG, "db_query_async", "submitted: %s (%u param(s))",
      sql, n_params);
  return(ASYNC_AIRBORNE);
}

bool
db_query_stream(const char *sql, db_row_cb_t row_cb, void *data,
    char *err, size_t err_cap)
{
  db_conn_t *c;
  bool       ret;

  if(!db_ready || driver == NULL || sql == NULL || row_cb == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "db not initialized");
    return(FAIL);
  }

  if(driver->query_stream == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "driver lacks streaming reads");
    return(FAIL);
  }

  c = conn_acquire();

  if(c == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "no connection available");
    return(FAIL);
  }

  clam(CLAM_DEBUG, "db_query_stream", "sql: %s", sql);

  ret = driver->query_stream(c->handle, sql, row_cb, data, err, err_cap);

  __atomic_add_fetch(&db_stat_queries, 1, __ATOMIC_RELAXED);

  if(ret != SUCCESS)
    __atomic_add_fetch(&db_stat_errors, 1, __ATOMIC_RELAXED);

  c->queries++;
  conn_release(c);

  return(ret);
}

// Transactions
//
// The pool hands out a connection per statement, so a transaction is
// exactly one thing: a connection held across several. The handle owns
// that pin — the slot stays ACTIVE and locked until commit or rollback
// hands it back — which is why the two enders also free the handle and
// why nothing else may block underneath one.

db_txn_t *
db_txn_begin(char *err, size_t err_cap)
{
  db_conn_t *c;
  db_txn_t  *txn;

  if(!db_ready || driver == NULL || driver->txn == NULL)
  {
    if(err != NULL)
      strlcpy(err, "db has no transaction support", err_cap);

    return(NULL);
  }

  c = conn_acquire();

  if(c == NULL)
  {
    if(err != NULL)
      strlcpy(err, "no connection available", err_cap);

    return(NULL);
  }

  if(driver->txn(c->handle, DB_TXN_BEGIN, err, err_cap) != SUCCESS)
  {
    conn_release(c);
    return(NULL);
  }

  txn = mem_alloc("db", "txn", sizeof(db_txn_t));

  txn->conn   = c;
  txn->failed = false;

  return(txn);
}

// Both statement paths, sharing the one thing that matters here: a
// failure latches, because everything after it in this transaction is
// going to be discarded anyway and the engine will refuse it regardless.
static bool
txn_run(db_txn_t *txn, const char *sql, const char *const *params,
    uint16_t n_params, db_result_t *result)
{
  bool ret;

  if(txn == NULL || sql == NULL)
    return(result_fail(result, "no transaction"));

  if(txn->failed)
    return(result_fail(result, "transaction already failed"));

  if(params != NULL && (driver->query_params == NULL
      || !params_ok(params, n_params)))
    return(result_fail(result, "driver lacks bound parameters"));

  clam(CLAM_DEBUG, "db_txn", "sql: %s (%u param(s))", sql, n_params);

  if(params != NULL)
    ret = driver->query_params(txn->conn->handle, sql, params,
        n_params, result);

  else
    ret = driver->query(txn->conn->handle, sql, result);

  __atomic_add_fetch(&db_stat_queries, 1, __ATOMIC_RELAXED);

  if(ret != SUCCESS)
  {
    __atomic_add_fetch(&db_stat_errors, 1, __ATOMIC_RELAXED);
    txn->failed = true;
  }

  txn->conn->queries++;
  return(ret);
}

bool
db_txn_query(db_txn_t *txn, const char *sql, db_result_t *result)
{
  return(txn_run(txn, sql, NULL, 0, result));
}

bool
db_txn_query_params(db_txn_t *txn, const char *sql,
    const char *const *params, uint16_t n_params, db_result_t *result)
{
  return(txn_run(txn, sql, params, n_params, result));
}

// End the transaction and give the connection back. `op` is what the
// caller asked for; a COMMIT that fails still has to end the
// transaction, so it falls through to a rollback of its own.
static bool
txn_end(db_txn_t *txn, db_txn_op_t op)
{
  char err[DB_ERROR_SZ] = {0};
  bool ret;

  ret = driver->txn(txn->conn->handle, op, err, sizeof(err));

  if(op == DB_TXN_COMMIT && ret != SUCCESS)
  {
    __atomic_add_fetch(&db_stat_errors, 1, __ATOMIC_RELAXED);
    driver->txn(txn->conn->handle, DB_TXN_ROLLBACK, NULL, 0);
  }

  conn_release(txn->conn);
  mem_free(txn);

  // Logged with the pin already given back: a clam() subscriber is free
  // to reach the database itself.
  if(ret != SUCCESS)
    clam(CLAM_WARN, "db_txn", "%s failed: %s",
        op == DB_TXN_COMMIT ? "commit" : "rollback",
        err[0] != '\0' ? err : "(no driver error)");

  return(ret);
}

bool
db_txn_commit(db_txn_t *txn)
{
  if(txn == NULL)
    return(FAIL);

  if(txn->failed)
  {
    txn_end(txn, DB_TXN_ROLLBACK);
    return(FAIL);
  }

  return(txn_end(txn, DB_TXN_COMMIT));
}

void
db_txn_rollback(db_txn_t *txn)
{
  if(txn != NULL)
    txn_end(txn, DB_TXN_ROLLBACK);
}

// returns: mem_alloc'd escaped string (caller frees), or NULL on failure
char *
db_escape(const char *input)
{
  db_conn_t *c;
  char      *escaped;

  if(!db_ready || driver == NULL || input == NULL)
    return(NULL);

  c = conn_acquire();

  if(c == NULL)
    return(NULL);

  escaped = driver->escape(c->handle, input);

  conn_release(c);

  return(escaped);
}

// Get pool statistics (thread-safe snapshot).
void
db_get_pool_stats(db_pool_stats_t *out)
{
  memset(out, 0, sizeof(*out));

  if(pool == NULL)
    return;

  for(uint16_t i = 0; i < db_pcfg.max_conns; i++)
  {
    db_conn_t *c = &pool[i];

    if(pthread_mutex_trylock(&c->mutex) != 0)
    {
      // Cannot lock — likely active.
      out->active++;
      out->total++;
      continue;
    }

    if(c->handle != NULL)
    {
      out->total++;

      switch(c->state)
      {
        case DB_CONN_IDLE:   out->idle++;   break;
        case DB_CONN_ACTIVE: out->active++; break;
        case DB_CONN_FAIL:   out->failed++; break;
      }
    }

    pthread_mutex_unlock(&c->mutex);
  }

  out->queries = __atomic_load_n(&db_stat_queries, __ATOMIC_RELAXED);
  out->errors  = __atomic_load_n(&db_stat_errors, __ATOMIC_RELAXED);
}

const db_driver_t *
db_audit_driver(void)
{
  return(driver);
}

// Iterate database connection pool slots. For each slot that has an
// active connection (handle != NULL), the callback receives the slot
// index, state, per-slot query count, creation time, and last use time.
// cb: iteration callback (must be fast — slot mutex is held briefly)
void
db_iterate_pool(db_pool_iter_cb_t cb, void *data)
{
  if(cb == NULL || pool == NULL)
    return;

  for(uint16_t i = 0; i < db_pcfg.max_conns; i++)
  {
    db_conn_t *c = &pool[i];

    if(pthread_mutex_trylock(&c->mutex) != 0)
    {
      // Cannot lock — likely active. Report what we can.
      cb(i, DB_CONN_ACTIVE, 0, 0, 0, data);
      continue;
    }

    if(c->handle != NULL)
      cb(i, c->state, c->queries, c->created, c->last_used, data);

    pthread_mutex_unlock(&c->mutex);
  }
}
