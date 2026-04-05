#define DB_INTERNAL
#include "db.h"

// -----------------------------------------------------------------------
// Close a connection and clear its slot.
// Connection must be locked by caller.
// c: connection to close
// -----------------------------------------------------------------------
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

// -----------------------------------------------------------------------
// Release a connection back to the pool.
// Connection must be locked by caller.
// c: connection to release
// -----------------------------------------------------------------------
static void
conn_release(db_conn_t *c)
{
  c->state = DB_CONN_IDLE;
  c->last_used = time(NULL);
  pthread_mutex_unlock(&c->mutex);
}

// -----------------------------------------------------------------------
// Acquire a connection from the pool.
// Two passes: first reuse an idle connection, then create a new one.
// returns: locked connection in ACTIVE state, or NULL if none available
// -----------------------------------------------------------------------
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

// -----------------------------------------------------------------------
// Free all dynamic data inside a result (but not the struct itself).
// r: result to clear
// -----------------------------------------------------------------------
static void
result_clear(db_result_t *r)
{
  if(r->col_names != NULL)
  {
    for(uint32_t i = 0; i < r->cols; i++)
    {
      if(r->col_names[i] != NULL)
        mem_free(r->col_names[i]);
    }

    mem_free(r->col_names);
    r->col_names = NULL;
  }

  if(r->data != NULL)
  {
    uint32_t total = r->rows * r->cols;

    for(uint32_t i = 0; i < total; i++)
    {
      if(r->data[i] != NULL)
        mem_free(r->data[i]);
    }

    mem_free(r->data);
    r->data = NULL;
  }
}

// -----------------------------------------------------------------------
// Idle connection reaper task callback.
// t: the reaper task
// -----------------------------------------------------------------------
static void
reaper_cb(task_t *t)
{
  if(!db_ready)
  {
    t->state = TASK_ENDED;
    return;
  }

  time_t now = time(NULL);
  uint16_t reaped = 0;

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

// -----------------------------------------------------------------------
// Async query task callback.
// t: the async query task
// -----------------------------------------------------------------------
static void
async_cb(task_t *t)
{
  db_async_ctx_t *ctx = t->data;

  db_conn_t *c = conn_acquire();

  if(c == NULL)
  {
    // No connection available — retry after 1 second.
    t->state = TASK_SLEEPING;
    t->sleep_until = time(NULL) + 1;
    return;
  }

  db_result_t *result = db_result_alloc();

  bool ok = driver->query(c->handle, ctx->sql, result);

  __atomic_add_fetch(&db_stat_queries, 1, __ATOMIC_RELAXED);

  if(!ok)
    __atomic_add_fetch(&db_stat_errors, 1, __ATOMIC_RELAXED);

  c->queries++;
  conn_release(c);

  // Invoke user callback (takes ownership of result).
  ctx->cb(result, ctx->data);

  mem_free(ctx->sql);
  mem_free(ctx);

  t->state = TASK_ENDED;
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

// Override pool configuration. Must be called before db_init().
// max_conns: maximum connections in pool
// idle_timeout_secs: seconds before idle connection is reaped
// reap_interval_secs: seconds between reaper runs
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

// returns: SUCCESS or FAIL
// drv: database driver interface (must not be NULL)
bool
db_init(const db_driver_t *drv)
{
  if(drv == NULL)
  {
    clam(CLAM_FATAL, "db_init", "no driver provided");
    return(FAIL);
  }

  driver = drv;

  // Read credentials from bootstrap config.
  const char *v;

  v = bconf_get("DBHOST");
  strncpy(creds.host, v ? v : "localhost", sizeof(creds.host) - 1);

  creds.port = (uint16_t)bconf_get_int("DBPORT", 5432);

  v = bconf_get("DBNAME");

  if(v != NULL)
    strncpy(creds.dbname, v, sizeof(creds.dbname) - 1);

  v = bconf_get("DBUSER");

  if(v != NULL)
    strncpy(creds.user, v, sizeof(creds.user) - 1);

  v = bconf_get("DBPASS");

  if(v != NULL)
    strncpy(creds.pass, v, sizeof(creds.pass) - 1);

  // Read optional pool configuration from bootstrap config.
  int pmax = bconf_get_int("DBPOOL", 0);

  if(pmax > 0)
    db_pcfg.max_conns = (uint16_t)pmax;

  int pidle = bconf_get_int("DBIDLE", 0);

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
  if(!db_ready)
    return;

  db_ready = false;

  // Close all connections.
  uint16_t closed = 0;

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

// returns: zeroed result struct
db_result_t *
db_result_alloc(void)
{
  return(mem_alloc("db", "result", sizeof(db_result_t)));
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

// returns: cell value string, or NULL if out of bounds
// r: query result
// row: row index
// col: column index
const char *
db_result_get(const db_result_t *r, uint32_t row, uint32_t col)
{
  if(r == NULL || r->data == NULL || row >= r->rows || col >= r->cols)
    return(NULL);

  return(r->data[row * r->cols + col]);
}

// returns: column name string, or NULL if out of bounds
// r: query result
// col: column index
const char *
db_result_col_name(const db_result_t *r, uint32_t col)
{
  if(r == NULL || r->col_names == NULL || col >= r->cols)
    return(NULL);

  return(r->col_names[col]);
}

// Allocate internal storage for rows and columns in a result.
// r: result to populate
// rows: number of rows
// cols: number of columns
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

// Set a column name (mem_strdup'd internally).
// r: result to populate
// col: column index
// name: column name string
void
db_result_set_col_name(db_result_t *r, uint32_t col, const char *name)
{
  if(col < r->cols && name != NULL)
    r->col_names[col] = mem_strdup("db", "col_name", name);
}

// Set a cell value (mem_strdup'd internally). NULL for SQL NULL.
// r: result to populate
// row: row index
// col: column index
// val: cell value string (or NULL)
void
db_result_set_value(db_result_t *r, uint32_t row, uint32_t col,
    const char *val)
{
  if(row < r->rows && col < r->cols && val != NULL)
    r->data[row * r->cols + col] = mem_strdup("db", "cell", val);
}

// returns: SUCCESS or FAIL
// sql: SQL query string
// result: pre-allocated result struct
bool
db_query(const char *sql, db_result_t *result)
{
  if(!db_ready || driver == NULL)
  {
    if(result != NULL)
    {
      result->ok = false;
      snprintf(result->error, DB_ERROR_SZ, "db not initialized");
    }
    return(FAIL);
  }

  db_conn_t *c = conn_acquire();

  if(c == NULL)
  {
    if(result != NULL)
    {
      result->ok = false;
      snprintf(result->error, DB_ERROR_SZ, "no connection available");
    }
    return(FAIL);
  }

  clam(CLAM_DEBUG, "db_query", "sql: %s", sql);

  bool ret = driver->query(c->handle, sql, result);

  __atomic_add_fetch(&db_stat_queries, 1, __ATOMIC_RELAXED);

  if(!ret)
    __atomic_add_fetch(&db_stat_errors, 1, __ATOMIC_RELAXED);

  c->queries++;
  conn_release(c);

  return(ret);
}

// returns: SUCCESS (task submitted) or FAIL
// sql: SQL query string
// cb: callback to receive the result
// data: opaque user data passed to callback
bool
db_query_async(const char *sql, db_cb_t cb, void *data)
{
  if(!db_ready || driver == NULL || sql == NULL || cb == NULL)
    return(FAIL);

  db_async_ctx_t *ctx = mem_alloc("db", "async_ctx", sizeof(db_async_ctx_t));

  ctx->sql  = mem_strdup("db", "async_sql", sql);
  ctx->cb   = cb;
  ctx->data = data;

  task_add("db_query", TASK_THREAD, 128, async_cb, ctx);

  clam(CLAM_DEBUG, "db_query_async", "submitted: %s", sql);
  return(SUCCESS);
}

// returns: mem_alloc'd escaped string (caller frees), or NULL on failure
// input: string to escape
char *
db_escape(const char *input)
{
  if(!db_ready || driver == NULL || input == NULL)
    return(NULL);

  db_conn_t *c = conn_acquire();

  if(c == NULL)
    return(NULL);

  char *escaped = driver->escape(c->handle, input);

  conn_release(c);

  return(escaped);
}

// Get pool statistics (thread-safe snapshot).
// out: destination for the snapshot
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
