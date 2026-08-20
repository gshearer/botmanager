// botmanager — MIT
// shorturl: PostgreSQL access — connection lifecycle, prepared hot path, admin CRUD.

#define DB_INTERNAL

#include "db.h"
#include "sql.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static PGconn *conn;

// Prepared statements belong to the session that created them, so this is
// cleared on every connect and reset, and honoured lazily by db_resolve.
static bool resolve_prepared;

static const char *const env_names[] =
{
  SHORTURL_ENV_DB_HOST,
  SHORTURL_ENV_DB_PORT,
  SHORTURL_ENV_DB_NAME,
  SHORTURL_ENV_DB_USER,
  SHORTURL_ENV_DB_PASS,
};

#define ENV_COUNT (sizeof env_names / sizeof env_names[0])

static const char *
require_env(const char *name)
{
  const char *value = getenv(name);

  if(!value || !*value)
  {
    fprintf(stderr, "shorturl: required environment variable %s is not set\n", name);
    return(NULL);
  }

  return(value);
}

static bool
db_connect(void)
{
  // PQconnectdbParams rather than a formatted conninfo string: keyword/value
  // arrays need no escaping, so a password containing a space or a quote
  // cannot corrupt the connection request.
  static const char *const keywords[] =
  {
    "host", "port", "dbname", "user", "password", "connect_timeout", NULL
  };

  const char *values[ENV_COUNT + 2];
  size_t i;

  if(conn)
  {
    PQfinish(conn);
    conn = NULL;
  }

  resolve_prepared = false;

  for(i = 0; i < ENV_COUNT; i++)
  {
    values[i] = require_env(env_names[i]);

    if(!values[i])
      return(false);
  }

  values[ENV_COUNT]     = SHORTURL_CONNECT_TIMEOUT;
  values[ENV_COUNT + 1] = NULL;

  conn = PQconnectdbParams(keywords, values, 0);

  if(!conn)
  {
    fprintf(stderr, "shorturl: db: out of memory opening connection\n");
    return(false);
  }

  if(PQstatus(conn) != CONNECTION_OK)
  {
    fprintf(stderr, "shorturl: db: %s", PQerrorMessage(conn));
    PQfinish(conn);
    conn = NULL;
    return(false);
  }

  return(true);
}

static bool
db_ready(void)
{
  if(!conn)
    return(db_connect());

  if(PQstatus(conn) == CONNECTION_OK)
    return(true);

  PQreset(conn);

  if(PQstatus(conn) != CONNECTION_OK)
  {
    fprintf(stderr, "shorturl: db: connection lost: %s", PQerrorMessage(conn));
    return(false);
  }

  resolve_prepared = false;

  return(true);
}

static bool
db_prepare_resolve(void)
{
  PGresult *res;

  if(resolve_prepared)
    return(true);

  res = PQprepare(conn, SQL_RESOLVE_NAME, SQL_RESOLVE, 1, NULL);

  if(!res || PQresultStatus(res) != PGRES_COMMAND_OK)
  {
    fprintf(stderr, "shorturl: db: prepare: %s", PQerrorMessage(conn));
    PQclear(res);
    return(false);
  }

  PQclear(res);
  resolve_prepared = true;

  return(true);
}

static db_result_t
db_result_of(PGresult *res, const char *what)
{
  ExecStatusType status;

  if(!res)
  {
    fprintf(stderr, "shorturl: db: %s: %s", what, PQerrorMessage(conn));
    return(DB_ERROR);
  }

  status = PQresultStatus(res);

  if(status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK)
  {
    fprintf(stderr, "shorturl: db: %s: %s", what, PQerrorMessage(conn));
    return(DB_ERROR);
  }

  return(DB_OK);
}

static db_result_t
copy_target(const char *src, char *dst, size_t dst_size)
{
  if(!url_valid(src))
  {
    fprintf(stderr, "shorturl: db: refusing malformed target\n");
    return(DB_ERROR);
  }

  // A truncated Location would redirect somewhere other than intended, so
  // truncation is an error rather than a shortened answer.
  if(strlcpy(dst, src, dst_size) >= dst_size)
  {
    fprintf(stderr, "shorturl: db: target does not fit in %zu bytes\n", dst_size);
    return(DB_ERROR);
  }

  return(DB_OK);
}

bool
db_open(void)
{
  return(db_connect());
}

void
db_close(void)
{
  if(conn)
  {
    PQfinish(conn);
    conn = NULL;
  }

  resolve_prepared = false;
}

db_result_t
db_resolve(const token_t *token, char *target, size_t target_size)
{
  int attempt;

  // Two passes, because libpq reports a connection as healthy until a command
  // is actually attempted on it: PQstatus still says CONNECTION_OK after the
  // server has restarted, failed over, or dropped an idle session. Checking
  // before the query therefore cannot see the loss, and a single-pass resolve
  // spends one real visitor's request discovering it.
  //
  // The retry can in principle double-count a hit, if the server committed the
  // UPDATE and then died before the result reached us. For a hit counter that
  // is a far better trade than answering 500.
  for(attempt = 0; attempt < 2; attempt++)
  {
    const char *values[1];
    PGresult *res;
    db_result_t result;

    // On the second pass this repairs the connection: db_ready() sees
    // CONNECTION_BAD, resets, and clears resolve_prepared so that
    // db_prepare_resolve() re-issues the statement into the new session.
    if(!db_ready() || !db_prepare_resolve())
      return(DB_ERROR);

    values[0] = token->s;

    res = PQexecPrepared(conn, SQL_RESOLVE_NAME, 1, values, NULL, NULL, 0);

    if(!res || PQresultStatus(res) != PGRES_TUPLES_OK)
    {
      bool lost = PQstatus(conn) != CONNECTION_OK;

      // A live connection returning an error means the query itself is at
      // fault, and retrying it would fail identically.
      if(!lost || attempt > 0)
      {
        fprintf(stderr, "shorturl: db: resolve: %s", PQerrorMessage(conn));
        PQclear(res);
        return(DB_ERROR);
      }

      PQclear(res);
      continue;
    }

    result = PQntuples(res) > 0
           ? copy_target(PQgetvalue(res, 0, 0), target, target_size)
           : DB_NOT_FOUND;

    PQclear(res);

    return(result);
  }

  return(DB_ERROR);
}

db_result_t
db_insert(const token_t *token, const char *target)
{
  const char *values[2];
  PGresult *res;
  db_result_t result;

  if(!url_valid(target) || !db_ready())
    return(DB_ERROR);

  values[0] = token->s;
  values[1] = target;

  res = PQexecParams(conn, SQL_INSERT, 2, NULL, values, NULL, NULL, 0);
  result = db_result_of(res, "insert");

  if(result == DB_OK && PQntuples(res) == 0)
    result = DB_CONFLICT;

  PQclear(res);

  return(result);
}

db_result_t
db_delete(const token_t *token)
{
  const char *values[1];
  PGresult *res;
  db_result_t result;

  if(!db_ready())
    return(DB_ERROR);

  values[0] = token->s;

  res = PQexecParams(conn, SQL_DELETE, 1, NULL, values, NULL, NULL, 0);
  result = db_result_of(res, "delete");

  if(result == DB_OK && PQntuples(res) == 0)
    result = DB_NOT_FOUND;

  PQclear(res);

  return(result);
}

db_result_t
db_list(db_row_fn emit, void *context)
{
  PGresult *res;
  db_result_t result;
  int rows;
  int i;

  if(!db_ready())
    return(DB_ERROR);

  res = PQexecParams(conn, SQL_LIST, 0, NULL, NULL, NULL, NULL, 0);
  result = db_result_of(res, "list");

  if(result != DB_OK)
  {
    PQclear(res);
    return(result);
  }

  rows = PQntuples(res);

  for(i = 0; i < rows; i++)
  {
    db_row_t row;

    row.token       = PQgetvalue(res, i, 0);
    row.target      = PQgetvalue(res, i, 1);
    row.hits        = PQgetvalue(res, i, 2);
    row.created_at  = PQgetvalue(res, i, 3);
    row.last_hit_at = PQgetvalue(res, i, 4);

    emit(&row, context);
  }

  PQclear(res);

  return(rows > 0 ? DB_OK : DB_NOT_FOUND);
}
