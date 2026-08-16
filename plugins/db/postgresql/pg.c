// botmanager — MIT
// PostgreSQL DB plugin: connection pool and query execution.
#define PG_INTERNAL
#include "pg.h"

// Driver callbacks

// libpq ends every message with a newline; nothing downstream wants it.
// A NULL destination is a caller who asked not to be told.
static void
pg_err_copy(char *dst, size_t cap, PGconn *conn)
{
  size_t len;

  if(dst == NULL || cap == 0)
    return;

  strlcpy(dst, PQerrorMessage(conn), cap);
  len = strlen(dst);

  if(len > 0 && dst[len - 1] == '\n')
    dst[len - 1] = '\0';
}

static void *
pg_connect(const char *host, uint16_t port, const char *dbname,
    const char *user, const char *pass)
{
  PGconn    *conn;
  pg_conn_t *c;
  char       connstr[1024];

  snprintf(connstr, sizeof(connstr),
      "host=%s port=%u dbname=%s user=%s password=%s connect_timeout=5",
      host, port, dbname, user, pass);

  conn = PQconnectdb(connstr);

  if(PQstatus(conn) != CONNECTION_OK)
  {
    pg_err_copy(pg_last_error, sizeof(pg_last_error), conn);
    PQfinish(conn);
    return(NULL);
  }

  pg_last_error[0] = '\0';

  c = mem_alloc("postgresql", "conn", sizeof(pg_conn_t));

  c->conn   = conn;
  c->in_txn = false;

  return(c);
}

static void
pg_disconnect(void *handle)
{
  pg_conn_t *c = handle;

  PQfinish(c->conn);
  mem_free(c);
}

static bool
pg_ping(void *handle)
{
  return(PQstatus(((pg_conn_t *)handle)->conn) == CONNECTION_OK
      ? SUCCESS : FAIL);
}

static bool
pg_reset(void *handle)
{
  pg_conn_t *c = handle;

  PQreset(c->conn);

  // A reset connection has no transaction, whatever it was carrying.
  c->in_txn = false;

  return(PQstatus(c->conn) == CONNECTION_OK ? SUCCESS : FAIL);
}

// Roll back any open transaction on `c`. PQexec runs a multi-statement
// script under one implicit driver session; if any statement errors it
// stops the script there and leaves the connection in PQTRANS_INERROR
// (open, aborted transaction). A BEGIN..COMMIT batch whose COMMIT never
// ran lands in PQTRANS_INTRANS instead. The botmanager pool releases
// connections without inspecting transaction state, so either leak
// poisons the next caller — every subsequent query reports
// "current transaction is aborted, commands ignored until end of
// transaction block". Drain the connection here before pg_query returns.
//
// Not while the caller owns the transaction: between db_txn_begin() and
// its ender, PQTRANS_INTRANS is the state that was asked for and
// PQTRANS_INERROR is the failure the caller is about to be told about.
static void
pg_drain_txn(pg_conn_t *c)
{
  PGTransactionStatusType txs;
  PGresult               *rb;

  if(c->in_txn)
    return;

  txs = PQtransactionStatus(c->conn);

  if(txs != PQTRANS_INERROR && txs != PQTRANS_INTRANS)
    return;

  rb = PQexec(c->conn, "ROLLBACK");

  if(rb != NULL)
    PQclear(rb);
}

// Copy one PGresult into the caller's db_result_t and consume it. Both
// execution paths — text statement and bound parameters — land here, so
// they cannot drift in what a row, a NULL cell or an affected count
// means.
static bool
pg_result_fill(PGconn *conn, PGresult *res, db_result_t *result)
{
  ExecStatusType status;
  const char    *tuples;

  if(res == NULL)
  {
    result->ok = false;
    pg_err_copy(result->error, DB_ERROR_SZ, conn);
    return(FAIL);
  }

  status = PQresultStatus(res);

  if(status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK)
  {
    result->ok = false;
    pg_err_copy(result->error, DB_ERROR_SZ, conn);
    PQclear(res);
    return(FAIL);
  }

  if(status == PGRES_TUPLES_OK)
  {
    uint32_t rows = (uint32_t)PQntuples(res);
    uint32_t cols = (uint32_t)PQnfields(res);

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
  }

  result->ok            = true;
  result->rows_affected = 0;

  tuples = PQcmdTuples(res);

  if(tuples != NULL && tuples[0] != '\0')
    result->rows_affected = (uint32_t)strtoul(tuples, NULL, 10);

  PQclear(res);
  return(SUCCESS);
}

static bool
pg_query(void *handle, const char *sql, db_result_t *result)
{
  pg_conn_t *c   = handle;
  bool       ret = pg_result_fill(c->conn, PQexec(c->conn, sql), result);

  pg_drain_txn(c);
  return(ret);
}

// PQexecParams sends the values as data on the wire, so no quoting,
// escaping or type-punning of the caller's strings happens anywhere:
// text in, text out (the trailing zero asks for text results, which is
// what db_result_t holds). It also accepts exactly one statement, which
// is the property that makes the placeholder form safe rather than
// merely tidy.
static bool
pg_query_params(void *handle, const char *sql, const char *const *params,
    uint16_t n_params, db_result_t *result)
{
  pg_conn_t *c = handle;
  bool       ret;

  ret = pg_result_fill(c->conn,
      PQexecParams(c->conn, sql, (int)n_params, NULL, params, NULL, NULL, 0),
      result);

  pg_drain_txn(c);
  return(ret);
}

static bool
pg_txn(void *handle, db_txn_op_t op, char *err, size_t err_cap)
{
  pg_conn_t  *c = handle;
  PGresult   *res;
  const char *stmt;
  bool        ret;

  // ROLLBACK is the default arm rather than a case of its own: a verb
  // this driver does not know still has to leave the connection clean,
  // and discarding the work is the only answer that always can.
  switch(op)
  {
    case DB_TXN_BEGIN:  stmt = "BEGIN";  break;
    case DB_TXN_COMMIT: stmt = "COMMIT"; break;
    default:            stmt = "ROLLBACK";
  }

  // Cleared before the statement runs, not after: if COMMIT or ROLLBACK
  // fails the transaction is over regardless, and leaving the flag set
  // would suppress the drain that has to clean up after it.
  c->in_txn = false;

  res = PQexec(c->conn, stmt);
  ret = (res != NULL && PQresultStatus(res) == PGRES_COMMAND_OK)
      ? SUCCESS : FAIL;

  if(ret != SUCCESS)
    pg_err_copy(err, err_cap, c->conn);

  if(res != NULL)
    PQclear(res);

  if(op == DB_TXN_BEGIN && ret == SUCCESS)
    c->in_txn = true;

  else
    pg_drain_txn(c);

  return(ret);
}

// returns: escaped string (caller must free), or NULL on error
static char *
pg_escape(void *handle, const char *input)
{
  pg_conn_t *c = handle;
  size_t len = strlen(input);
  char *out = mem_alloc("postgresql", "escaped", len * 2 + 1);
  int err = 0;

  PQescapeStringConn(c->conn, out, input, len, &err);

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
    return(PQerrorMessage(((pg_conn_t *)handle)->conn));

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
  pg_conn_t *c    = handle;
  PGconn    *conn = c->conn;
  PGresult  *res;
  bool       ret  = SUCCESS;
  bool       go   = true;    // row_cb has not asked to stop
  uint32_t   row  = 0;

  if(!PQsendQuery(conn, sql) || !PQsetSingleRowMode(conn))
  {
    pg_err_copy(err, err_cap, conn);
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
        pg_err_copy(err, err_cap, conn);
        ret = FAIL;
      }
    }

    PQclear(res);
  }

  pg_drain_txn(c);
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
  .query_params = pg_query_params,
  .txn          = pg_txn,
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
