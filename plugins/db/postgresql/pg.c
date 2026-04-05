#define PG_INTERNAL
#include "pg.h"

// -----------------------------------------------------------------------
// Driver callbacks
// -----------------------------------------------------------------------

// returns: connection handle, or NULL on failure
// host: database server hostname
// port: database server port
// dbname: database name
// user: authentication username
// pass: authentication password
static void *
pg_connect(const char *host, uint16_t port, const char *dbname,
    const char *user, const char *pass)
{
  char connstr[1024];

  snprintf(connstr, sizeof(connstr),
      "host=%s port=%u dbname=%s user=%s password=%s connect_timeout=5",
      host, port, dbname, user, pass);

  PGconn *conn = PQconnectdb(connstr);

  if(PQstatus(conn) != CONNECTION_OK)
  {
    snprintf(pg_last_error, DB_ERROR_SZ, "%s", PQerrorMessage(conn));

    // Trim trailing newline that libpq adds.
    size_t len = strlen(pg_last_error);

    if(len > 0 && pg_last_error[len - 1] == '\n')
      pg_last_error[len - 1] = '\0';

    PQfinish(conn);
    return(NULL);
  }

  pg_last_error[0] = '\0';
  return(conn);
}

// handle: connection handle to close
static void
pg_disconnect(void *handle)
{
  PQfinish((PGconn *)handle);
}

// returns: SUCCESS or FAIL
// handle: connection handle
static bool
pg_ping(void *handle)
{
  return(PQstatus((PGconn *)handle) == CONNECTION_OK ? SUCCESS : FAIL);
}

// returns: SUCCESS or FAIL
// handle: connection handle
static bool
pg_reset(void *handle)
{
  PQreset((PGconn *)handle);
  return(PQstatus((PGconn *)handle) == CONNECTION_OK ? SUCCESS : FAIL);
}

// returns: SUCCESS or FAIL
// handle: connection handle
// sql: SQL query string
// result: destination for query results
static bool
pg_query(void *handle, const char *sql, db_result_t *result)
{
  PGconn *conn = (PGconn *)handle;
  PGresult *res = PQexec(conn, sql);

  if(res == NULL)
  {
    result->ok = false;
    snprintf(result->error, DB_ERROR_SZ, "%s", PQerrorMessage(conn));
    return(FAIL);
  }

  ExecStatusType status = PQresultStatus(res);

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
      {
        if(!PQgetisnull(res, (int)r, (int)c))
          db_result_set_value(result, r, c,
              PQgetvalue(res, (int)r, (int)c));
      }
    }

    result->ok = true;
    result->rows_affected = 0;

    const char *tuples = PQcmdTuples(res);

    if(tuples != NULL && tuples[0] != '\0')
      result->rows_affected = (uint32_t)strtoul(tuples, NULL, 10);

    PQclear(res);
    return(SUCCESS);
  }

  if(status == PGRES_COMMAND_OK)
  {
    result->ok = true;
    result->rows_affected = 0;

    const char *tuples = PQcmdTuples(res);

    if(tuples != NULL && tuples[0] != '\0')
      result->rows_affected = (uint32_t)strtoul(tuples, NULL, 10);

    PQclear(res);
    return(SUCCESS);
  }

  // Failure.
  result->ok = false;
  snprintf(result->error, DB_ERROR_SZ, "%s", PQerrorMessage(conn));

  // Trim trailing newline.
  size_t len = strlen(result->error);

  if(len > 0 && result->error[len - 1] == '\n')
    result->error[len - 1] = '\0';

  PQclear(res);
  return(FAIL);
}

// returns: escaped string (caller must free), or NULL on error
// handle: connection handle
// input: string to escape
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

// returns: human-readable error string
// handle: connection handle (may be NULL)
static const char *
pg_error(void *handle)
{
  if(handle != NULL)
    return(PQerrorMessage((PGconn *)handle));

  return(pg_last_error[0] != '\0' ? pg_last_error : "unknown error");
}

// -----------------------------------------------------------------------
// Driver struct
// -----------------------------------------------------------------------

const db_driver_t pg_driver = {
  .name       = "postgresql",
  .connect    = pg_connect,
  .disconnect = pg_disconnect,
  .ping       = pg_ping,
  .reset      = pg_reset,
  .query      = pg_query,
  .escape     = pg_escape,
  .error      = pg_error,
};

// -----------------------------------------------------------------------
// Plugin descriptor
// -----------------------------------------------------------------------

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
