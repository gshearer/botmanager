// botmanager — MIT
// featreq persistence: one global table, bootstrapped on start(). Every
// value that came from a person is bound out of band through
// db_query_params(); the only thing interpolated into a statement is
// the table name, which fr_table_name() has already proved to be a bare
// SQL identifier.

#define FEATREQ_INTERNAL
#include "featreq.h"

#include "kv.h"
#include "validate.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------------ //
// Table naming                                                        //
// ------------------------------------------------------------------ //

bool
fr_table_name(char *out, size_t cap)
{
  const char *name = kv_get_str(FR_KV_TABLE);
  size_t      len;

  // A cleared knob lands here as the empty string; validate_alnum
  // refuses it, so the clear is reported rather than papered over with
  // a fallback that restates the schema's own default.
  len = (name != NULL) ? strlen(name) : 0;

  if(name == NULL || len >= cap || !validate_alnum(name, cap - 1))
  {
    clam(CLAM_WARN, FR_CTX,
        "configured table name '%s' is not a safe identifier",
        (name != NULL) ? name : "");
    return(FAIL);
  }

  memcpy(out, name, len + 1);
  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Schema                                                              //
// ------------------------------------------------------------------ //

// CREATE TABLE IF NOT EXISTS is idempotent on its own; the mutex only
// keeps two concurrent starts from doubling the log noise.
static bool            fr_schema_done = false;
static pthread_mutex_t fr_schema_lock = PTHREAD_MUTEX_INITIALIZER;

bool
fr_schema_ensure(void)
{
  char table[FR_TABLE_SZ];
  char sql[1024];
  bool ok = SUCCESS;

  pthread_mutex_lock(&fr_schema_lock);

  if(fr_schema_done)
  {
    pthread_mutex_unlock(&fr_schema_lock);
    return(SUCCESS);
  }

  if(fr_table_name(table, sizeof(table)) != SUCCESS)
  {
    pthread_mutex_unlock(&fr_schema_lock);
    return(FAIL);
  }

  // No ns_id: the board is global on purpose. A request is about the
  // software every namespace shares, so scoping it to the room it was
  // typed in would hide it from whoever has to act on it.
  //
  // nickname + method is who asked, as their method knows them;
  // username is who they are to us; botname is which bot heard it.
  // status_at starts equal to created_at — a request that has not moved
  // has still had its status set exactly once, when it was filed.
  snprintf(sql, sizeof(sql),
      "CREATE TABLE IF NOT EXISTS %s ("
      " id          BIGSERIAL   PRIMARY KEY,"
      " req_type    VARCHAR(16) NOT NULL DEFAULT '%s',"
      " status      VARCHAR(16) NOT NULL DEFAULT '%s',"
      " nickname    VARCHAR(64) NOT NULL DEFAULT '',"
      " method      VARCHAR(64) NOT NULL DEFAULT '',"
      " username    VARCHAR(64) NOT NULL DEFAULT '',"
      " botname     VARCHAR(64) NOT NULL DEFAULT '',"
      " description TEXT        NOT NULL,"
      " created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      " status_at   TIMESTAMPTZ NOT NULL DEFAULT NOW()"
      ")", table, fr_type_word(FR_TYPE_FEAT), fr_status_word(FR_ST_NEW));

  if(db_exec(sql, FR_CTX) != SUCCESS)
    ok = FAIL;

  // Both reads the board offers — the whole list newest-first, and one
  // status's slice of it — are served by this one index.
  if(ok == SUCCESS)
  {
    snprintf(sql, sizeof(sql),
        "CREATE INDEX IF NOT EXISTS idx_%s_status ON %s"
        " (status, created_at DESC)", table, table);

    if(db_exec(sql, FR_CTX) != SUCCESS)
      ok = FAIL;
  }

  if(ok == SUCCESS)
  {
    fr_schema_done = true;
    clam(CLAM_INFO, FR_CTX, "featreq schema ready (table '%s')", table);
  }

  pthread_mutex_unlock(&fr_schema_lock);
  return(ok);
}

// ------------------------------------------------------------------ //
// Filtering                                                           //
// ------------------------------------------------------------------ //

// Render the filter as a WHERE clause with its values bound, appending
// each to `params`. `out` is left empty when nothing is filtered.
// Returns the number of parameters written.
//
// The closed statuses are excluded unless the caller either named a
// status outright or asked for the whole board: the default view is a
// worklist, and a request the operator has finished with has stopped
// being work. Naming `--status completed` is therefore not a filter ON
// the default set but a different question, which is why it replaces
// the clause rather than narrowing it.
static uint16_t
fr_where_build(const fr_filter_t *f, char *out, size_t cap,
    const char **params)
{
  uint16_t n    = 0;
  size_t   used = 0;

  out[0] = '\0';

  if(f->by_type)
  {
    params[n++] = fr_type_word(f->type);
    used += (size_t)snprintf(out + used, cap - used,
        " WHERE req_type = $%" PRIu16, n);
  }

  if(f->by_status)
  {
    params[n++] = fr_status_word(f->status);
    used += (size_t)snprintf(out + used, cap - used,
        " %s status = $%" PRIu16, (used > 0) ? "AND" : "WHERE", n);
  }

  else if(!f->all)
  {
    char open[128];

    snprintf(out + used, cap - used, " %s %s", (used > 0) ? "AND" : "WHERE",
        fr_status_open_sql(open, sizeof(open)));
  }

  return(n);
}

// ------------------------------------------------------------------ //
// Reads                                                               //
// ------------------------------------------------------------------ //

bool
fr_db_list(const fr_filter_t *f, uint32_t limit, db_result_t *res)
{
  const char *params[2];
  char        table[FR_TABLE_SZ];
  char        where[128];
  char        order[256];
  char        sql[1024];
  uint16_t    n;

  if(fr_table_name(table, sizeof(table)) != SUCCESS)
    return(FAIL);

  n = fr_where_build(f, where, sizeof(where), params);

  // Newest-first within a status too: sorting by status alone would
  // leave the order inside each band down to whatever the planner did.
  if(f->sort == FR_SORT_STATUS)
  {
    char rank[256];

    snprintf(order, sizeof(order), "%s, created_at DESC, id DESC",
        fr_status_case_sql(rank, sizeof(rank)));
  }

  // Oldest first is the queue order: it is what "the next one to work
  // on" means, and it has to be the DATABASE's ordering rather than the
  // caller reading the newest-first list backwards — the row cap trims
  // the far end of the sort, so read backwards it hands back the
  // oldest of the newest N and not the oldest at all.
  else if(f->sort == FR_SORT_OLD)
    strlcpy(order, "created_at ASC, id ASC", sizeof(order));

  else
    strlcpy(order, "created_at DESC, id DESC", sizeof(order));

  snprintf(sql, sizeof(sql),
      "SELECT " FR_SELECT_COLS " FROM %s%s ORDER BY %s LIMIT %" PRIu32,
      table, where, order, limit);

  if(db_query_params(sql, params, n, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, FR_CTX, "list failed: %s",
        (res->error[0] != '\0') ? res->error : "(no driver error)");
    return(FAIL);
  }

  return(SUCCESS);
}

int
fr_db_count(const fr_filter_t *f)
{
  const char  *params[2];
  db_result_t *res = NULL;
  char         table[FR_TABLE_SZ];
  char         where[128];
  char         sql[512];
  uint16_t     n;
  int          count = -1;

  if(fr_table_name(table, sizeof(table)) != SUCCESS)
    return(-1);

  n = fr_where_build(f, where, sizeof(where), params);

  snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s%s", table, where);

  res = db_result_alloc();

  if(db_query_params(sql, params, n, res) == SUCCESS && res->ok
      && res->rows == 1)
    count = (int)db_result_get_i64(res, 0, 0, 0);

  db_result_free(res);
  return(count);
}

bool
fr_db_fetch(int64_t id, db_result_t *res)
{
  const char *params[1];
  char        table[FR_TABLE_SZ];
  char        sql[512];
  char        id_s[24];

  if(fr_table_name(table, sizeof(table)) != SUCCESS)
    return(FAIL);

  snprintf(id_s, sizeof(id_s), "%" PRId64, id);
  params[0] = id_s;

  snprintf(sql, sizeof(sql),
      "SELECT " FR_SELECT_COLS " FROM %s WHERE id = $1", table);

  if(db_query_params(sql, params, 1, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, FR_CTX, "fetch failed: %s",
        (res->error[0] != '\0') ? res->error : "(no driver error)");
    return(FAIL);
  }

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Writes                                                              //
// ------------------------------------------------------------------ //

int64_t
fr_db_add(const fr_new_t *req)
{
  const char  *params[6];
  db_result_t *res = NULL;
  char         table[FR_TABLE_SZ];
  char         sql[512];
  int64_t      id  = -1;

  if(fr_table_name(table, sizeof(table)) != SUCCESS)
    return(-1);

  params[0] = fr_type_word(req->type);
  params[1] = req->nickname;
  params[2] = req->method;
  params[3] = req->username;
  params[4] = req->botname;
  params[5] = req->desc;

  snprintf(sql, sizeof(sql),
      "INSERT INTO %s (req_type, nickname, method, username, botname,"
      " description) VALUES ($1, $2, $3, $4, $5, $6) RETURNING id", table);

  res = db_result_alloc();

  if(db_query_params(sql, params, 6, res) == SUCCESS && res->ok
      && res->rows == 1)
    id = db_result_get_i64(res, 0, 0, -1);

  else
    clam(CLAM_WARN, FR_CTX, "insert failed: %s",
        (res->error[0] != '\0') ? res->error : "(no driver error)");

  db_result_free(res);
  return(id);
}

fr_upd_t
fr_db_set_status(int64_t id, fr_status_t status)
{
  const char  *params[2];
  db_result_t *res = NULL;
  char         table[FR_TABLE_SZ];
  char         sql[512];
  char         id_s[24];
  fr_upd_t     rc  = FR_UPD_ERROR;

  if(fr_table_name(table, sizeof(table)) != SUCCESS)
    return(FR_UPD_ERROR);

  snprintf(id_s, sizeof(id_s), "%" PRId64, id);

  params[0] = fr_status_word(status);
  params[1] = id_s;

  // RETURNING is what separates "no such request" from "the write
  // failed": an UPDATE that matched nothing is a perfectly successful
  // statement, and the caller has to say something different about it.
  snprintf(sql, sizeof(sql),
      "UPDATE %s SET status = $1, status_at = NOW() WHERE id = $2"
      " RETURNING id", table);

  res = db_result_alloc();

  if(db_query_params(sql, params, 2, res) == SUCCESS && res->ok)
    rc = (res->rows == 1) ? FR_UPD_OK : FR_UPD_NO_ROW;

  else
    clam(CLAM_WARN, FR_CTX, "status update failed: %s",
        (res->error[0] != '\0') ? res->error : "(no driver error)");

  db_result_free(res);
  return(rc);
}
