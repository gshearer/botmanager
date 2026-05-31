// botmanager — MIT
// PostgreSQL DB plugin: connection pool and query execution.
#define PG_INTERNAL
#include "pg.h"

// Driver callbacks

static void *
pg_connect(const char *host, uint16_t port, const char *dbname,
    const char *user, const char *pass)
{
  PGconn *conn;
  char connstr[1024];

  snprintf(connstr, sizeof(connstr),
      "host=%s port=%u dbname=%s user=%s password=%s connect_timeout=5",
      host, port, dbname, user, pass);

  conn = PQconnectdb(connstr);

  if(PQstatus(conn) != CONNECTION_OK)
  {
    size_t len;
    snprintf(pg_last_error, DB_ERROR_SZ, "%s", PQerrorMessage(conn));

    // Trim trailing newline that libpq adds.
    len = strlen(pg_last_error);

    if(len > 0 && pg_last_error[len - 1] == '\n')
      pg_last_error[len - 1] = '\0';

    PQfinish(conn);
    return(NULL);
  }

  pg_last_error[0] = '\0';
  return(conn);
}

static void
pg_disconnect(void *handle)
{
  PQfinish((PGconn *)handle);
}

static bool
pg_ping(void *handle)
{
  return(PQstatus((PGconn *)handle) == CONNECTION_OK ? SUCCESS : FAIL);
}

static bool
pg_reset(void *handle)
{
  PQreset((PGconn *)handle);
  return(PQstatus((PGconn *)handle) == CONNECTION_OK ? SUCCESS : FAIL);
}

// Roll back any open transaction on `conn`. PQexec runs a multi-statement
// script under one implicit driver session; if any statement errors it
// stops the script there and leaves the connection in PQTRANS_INERROR
// (open, aborted transaction). A BEGIN..COMMIT batch whose COMMIT never
// ran lands in PQTRANS_INTRANS instead. The botmanager pool releases
// connections without inspecting transaction state, so either leak
// poisons the next caller — every subsequent query reports
// "current transaction is aborted, commands ignored until end of
// transaction block". Drain the connection here before pg_query returns.
static void
pg_drain_txn(PGconn *conn)
{
  PGTransactionStatusType txs = PQtransactionStatus(conn);
  PGresult               *rb;

  if(txs != PQTRANS_INERROR && txs != PQTRANS_INTRANS)
    return;

  rb = PQexec(conn, "ROLLBACK");

  if(rb != NULL)
    PQclear(rb);
}

static bool
pg_query(void *handle, const char *sql, db_result_t *result)
{
  ExecStatusType status;
  size_t         len;
  PGconn        *conn = (PGconn *)handle;
  PGresult      *res  = PQexec(conn, sql);
  bool           ret  = FAIL;

  if(res == NULL)
  {
    result->ok = false;
    snprintf(result->error, DB_ERROR_SZ, "%s", PQerrorMessage(conn));
    goto out;
  }

  status = PQresultStatus(res);

  if(status == PGRES_TUPLES_OK)
  {
    const char *tuples;
    uint32_t    rows = (uint32_t)PQntuples(res);
    uint32_t    cols = (uint32_t)PQnfields(res);

    db_result_set_size(result, rows, cols);

    for(uint32_t c = 0; c < cols; c++)
      db_result_set_col_name(result, c, PQfname(res, (int)c));

    for(uint32_t r = 0; r < rows; r++)
    {
      for(uint32_t c = 0; c < cols; c++)
        if(!PQgetisnull(res, (int)r, (int)c))
          db_result_set_value(result, r, c,
              PQgetvalue(res, (int)r, (int)c));
    }

    result->ok            = true;
    result->rows_affected = 0;

    tuples = PQcmdTuples(res);

    if(tuples != NULL && tuples[0] != '\0')
      result->rows_affected = (uint32_t)strtoul(tuples, NULL, 10);

    PQclear(res);
    ret = SUCCESS;
    goto out;
  }

  if(status == PGRES_COMMAND_OK)
  {
    const char *tuples;
    result->ok            = true;
    result->rows_affected = 0;

    tuples = PQcmdTuples(res);

    if(tuples != NULL && tuples[0] != '\0')
      result->rows_affected = (uint32_t)strtoul(tuples, NULL, 10);

    PQclear(res);
    ret = SUCCESS;
    goto out;
  }

  // Failure.
  result->ok = false;
  snprintf(result->error, DB_ERROR_SZ, "%s", PQerrorMessage(conn));

  // Trim trailing newline.
  len = strlen(result->error);

  if(len > 0 && result->error[len - 1] == '\n')
    result->error[len - 1] = '\0';

  PQclear(res);

out:
  pg_drain_txn(conn);
  return(ret);
}

// returns: escaped string (caller must free), or NULL on error
static char *
pg_escape(void *handle, const char *input)
{
  PGconn *conn = (PGconn *)handle;
  size_t len = strlen(input);
  char *out = mem_alloc("postgresql", "escaped", len * 2 + 1);
  int err = 0;

  PQescapeStringConn(conn, out, input, len, &err);

  if(err)
  {
    mem_free(out);
    return(NULL);
  }

  return(out);
}

static const char *
pg_error(void *handle)
{
  if(handle != NULL)
    return(PQerrorMessage((PGconn *)handle));

  return(pg_last_error[0] != '\0' ? pg_last_error : "unknown error");
}

#define PG_STREAM_MAX_COLS 64

// Stream a SELECT row-by-row via libpq single-row mode. Each row's cells
// are handed to row_cb straight from the per-row PGresult (never copied
// into the tracked allocator), then PQclear'd immediately - so at most
// one row is resident and mem_mutex is untouched on the read path. We
// must loop PQgetResult to NULL even on error / early-stop, else the
// connection is left busy and poisons the next pool user.
static bool
pg_query_stream(void *handle, const char *sql, db_row_cb_t row_cb,
    void *data, char *err, size_t err_cap)
{
  PGconn   *conn = (PGconn *)handle;
  PGresult *res;
  bool      ret  = SUCCESS;
  bool      go   = true;     // row_cb has not asked to stop
  uint32_t  row  = 0;

  if(!PQsendQuery(conn, sql) || !PQsetSingleRowMode(conn))
  {
    if(err != NULL)
      snprintf(err, err_cap, "%s", PQerrorMessage(conn));
    ret = FAIL;
    // fall through: still drain to NULL to clear the connection
  }

  while((res = PQgetResult(conn)) != NULL)
  {
    ExecStatusType st = PQresultStatus(res);

    if(st == PGRES_SINGLE_TUPLE)
    {
      if(ret == SUCCESS && go)
      {
        const char *vals[PG_STREAM_MAX_COLS];
        int         cols = PQnfields(res);
        int         ci;

        if(cols > PG_STREAM_MAX_COLS)
          cols = PG_STREAM_MAX_COLS;

        for(ci = 0; ci < cols; ci++)
          vals[ci] = PQgetisnull(res, 0, ci)
              ? NULL : PQgetvalue(res, 0, ci);

        go = row_cb(row, (uint32_t)cols, (const char *const *)vals, data);
        row++;
      }
    }
    else if(st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK)
    {
      // PGRES_FATAL_ERROR etc. - capture once, keep draining.
      if(ret == SUCCESS)
      {
        size_t len;

        if(err != NULL)
        {
          snprintf(err, err_cap, "%s", PQerrorMessage(conn));
          len = strlen(err);

          if(len > 0 && err[len - 1] == '\n')
            err[len - 1] = '\0';
        }

        ret = FAIL;
      }
    }

    PQclear(res);
  }

  pg_drain_txn(conn);
  return(ret);
}

// Driver struct

const db_driver_t pg_driver = {
  .name         = "postgresql",
  .connect      = pg_connect,
  .disconnect   = pg_disconnect,
  .ping         = pg_ping,
  .reset        = pg_reset,
  .query        = pg_query,
  .query_stream = pg_query_stream,
  .escape       = pg_escape,
  .error        = pg_error,
};

// Plugin descriptor

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "postgresql",
  .version         = "1.0",
  .type            = PLUGIN_DB,
  .kind            = "postgresql",
  .provides        = { { .name = "db_postgresql" } },
  .provides_count  = 1,
  .requires_count  = 0,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = NULL,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = NULL,
  .ext             = &pg_driver,
};
