// botmanager — MIT
// userquote persistence: schema bootstrap + quote CRUD. Every value that
// originates from a user is routed through db_escape before it touches a
// query; the only interpolated identifier is the table name, which is
// validated as a strict SQL identifier by uq_table_name().

#define USERQUOTE_INTERNAL
#include "userquote.h"

#include "alloc.h"
#include "db.h"
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
uq_table_name(char *out, size_t cap)
{
  const char *name = kv_get_str(UQ_KV_TABLE);
  size_t      len;

  if(out == NULL || cap == 0)
    return(FAIL);

  if(name == NULL || name[0] == '\0')
    name = "userquotes";

  len = strlen(name);

  // Guard against injection: the table name is pasted into DDL/queries
  // verbatim, so it must be a bare identifier (letters, digits, '_').
  if(len >= cap || !validate_alnum(name, cap - 1))
  {
    clam(CLAM_WARN, UQ_CTX,
        "configured table name '%s' is not a safe identifier", name);
    return(FAIL);
  }

  memcpy(out, name, len + 1);
  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Schema                                                              //
// ------------------------------------------------------------------ //

// CREATE TABLE IF NOT EXISTS is idempotent, but a mutex keeps concurrent
// bot starts from racing the batch and doubling the log noise.
static bool            uq_schema_done = false;
static pthread_mutex_t uq_schema_lock = PTHREAD_MUTEX_INITIALIZER;

static bool
uq_run_ddl(const char *sql)
{
  db_result_t *res = db_result_alloc();
  bool         ok  = SUCCESS;

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, UQ_CTX, "ddl failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");
    ok = FAIL;
  }

  db_result_free(res);
  return(ok);
}

bool
uq_schema_ensure(void)
{
  char table[UQ_TABLE_SZ];
  char sql[1024];
  bool ok = SUCCESS;

  pthread_mutex_lock(&uq_schema_lock);

  if(uq_schema_done)
  {
    pthread_mutex_unlock(&uq_schema_lock);
    return(SUCCESS);
  }

  if(uq_table_name(table, sizeof(table)) != SUCCESS)
  {
    pthread_mutex_unlock(&uq_schema_lock);
    return(FAIL);
  }

  // Quotes are userns-scoped: recall filters on ns_id, never on channel.
  // method/channel are kept for the verbose card and provenance only.
  snprintf(sql, sizeof(sql),
      "CREATE TABLE IF NOT EXISTS %s ("
      " id         BIGSERIAL    PRIMARY KEY,"
      " ns_id      INTEGER      NOT NULL,"
      " method     VARCHAR(64)  NOT NULL DEFAULT '',"
      " channel    VARCHAR(128) NOT NULL DEFAULT '',"
      " sayer      VARCHAR(64)  NOT NULL DEFAULT '',"
      " quoter     VARCHAR(64)  NOT NULL DEFAULT '',"
      " quote      TEXT         NOT NULL,"
      " created_at TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
      " lastview   TIMESTAMPTZ  NOT NULL DEFAULT NOW()"
      ")", table);

  if(uq_run_ddl(sql) != SUCCESS)
    ok = FAIL;

  if(ok == SUCCESS)
  {
    snprintf(sql, sizeof(sql),
        "CREATE INDEX IF NOT EXISTS idx_%s_ns_sayer ON %s(ns_id, sayer)",
        table, table);
    if(uq_run_ddl(sql) != SUCCESS)
      ok = FAIL;
  }

  if(ok == SUCCESS)
  {
    snprintf(sql, sizeof(sql),
        "CREATE INDEX IF NOT EXISTS idx_%s_ns_lastview"
        " ON %s(ns_id, lastview)", table, table);
    if(uq_run_ddl(sql) != SUCCESS)
      ok = FAIL;
  }

  if(ok == SUCCESS)
  {
    uq_schema_done = true;
    clam(CLAM_INFO, UQ_CTX, "quote schema ready (table '%s')", table);
  }

  pthread_mutex_unlock(&uq_schema_lock);
  return(ok);
}

// ------------------------------------------------------------------ //
// Insert                                                              //
// ------------------------------------------------------------------ //

int64_t
uq_db_add(uint32_t ns_id, const char *method, const char *channel,
    const char *sayer, const char *quoter, const char *quote)
{
  db_result_t *res      = NULL;
  char        *e_method = NULL;
  char        *e_chan   = NULL;
  char        *e_sayer  = NULL;
  char        *e_quoter = NULL;
  char        *e_quote  = NULL;
  char         table[UQ_TABLE_SZ];
  char        *sql      = NULL;
  int64_t      id       = -1;

  if(quote == NULL || quote[0] == '\0')
    return(-1);

  if(uq_table_name(table, sizeof(table)) != SUCCESS)
    return(-1);

  e_method = db_escape(method  != NULL ? method  : "");
  e_chan   = db_escape(channel != NULL ? channel : "");
  e_sayer  = db_escape(sayer   != NULL ? sayer   : "");
  e_quoter = db_escape(quoter  != NULL ? quoter  : "");
  e_quote  = db_escape(quote);

  if(e_method == NULL || e_chan == NULL || e_sayer == NULL ||
     e_quoter == NULL || e_quote == NULL)
    goto out;

  // Query length is dominated by the escaped quote body, so size the
  // scratch buffer from the actual inputs rather than a fixed stack slab.
  {
    size_t need = 256 + strlen(table) + strlen(e_method) + strlen(e_chan)
        + strlen(e_sayer) + strlen(e_quoter) + strlen(e_quote);

    sql = mem_alloc(UQ_CTX, "add_sql", need);

    snprintf(sql, need,
        "INSERT INTO %s (ns_id, method, channel, sayer, quoter, quote)"
        " VALUES (%" PRIu32 ", '%s', '%s', '%s', '%s', '%s')"
        " RETURNING id",
        table, ns_id, e_method, e_chan, e_sayer, e_quoter, e_quote);
  }

  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok &&
     res->rows == 1)
  {
    const char *s = db_result_get(res, 0, 0);

    if(s != NULL)
      id = (int64_t)strtoll(s, NULL, 10);
  }

  else
    clam(CLAM_WARN, UQ_CTX, "quote insert failed: %s",
        (res->error[0] != '\0')
            ? res->error : "(no driver error)");

out:
  db_result_free(res);
  if(e_method != NULL) mem_free(e_method);
  if(e_chan   != NULL) mem_free(e_chan);
  if(e_sayer  != NULL) mem_free(e_sayer);
  if(e_quoter != NULL) mem_free(e_quoter);
  if(e_quote  != NULL) mem_free(e_quote);
  if(sql      != NULL) mem_free(sql);

  return(id);
}

// ------------------------------------------------------------------ //
// Recall                                                              //
// ------------------------------------------------------------------ //

static void
uq_copy_col(char *dst, size_t cap, const db_result_t *res, uint32_t col)
{
  const char *s = db_result_get(res, 0, col);

  if(s == NULL)
    s = "";

  snprintf(dst, cap, "%s", s);
}

bool
uq_db_get(uint32_t ns_id, int64_t id, const char *sayer, uq_quote_t *out)
{
  db_result_t *res     = NULL;
  char        *e_sayer = NULL;
  char         table[UQ_TABLE_SZ];
  char         sql[768];
  bool         hit = false;

  if(out == NULL)
    return(false);

  memset(out, 0, sizeof(*out));

  if(uq_table_name(table, sizeof(table)) != SUCCESS)
    return(false);

  if(id > 0)
    snprintf(sql, sizeof(sql),
        "SELECT id, sayer, quoter, quote,"
        " to_char(created_at, 'YYYY-MM-DD HH24:MI'),"
        " to_char(lastview,   'YYYY-MM-DD HH24:MI')"
        " FROM %s WHERE ns_id = %" PRIu32 " AND id = %" PRId64,
        table, ns_id, id);

  else if(sayer != NULL && sayer[0] != '\0')
  {
    e_sayer = db_escape(sayer);

    if(e_sayer == NULL)
      return(false);

    // Case-insensitive search key, least-recently-viewed first — the
    // classic quotebot rotation so repeated recalls cycle the book.
    // Equality, not ILIKE: '_' is a legal nick byte and as a pattern it
    // matches any character, so one nick recalls another's quotes.
    snprintf(sql, sizeof(sql),
        "SELECT id, sayer, quoter, quote,"
        " to_char(created_at, 'YYYY-MM-DD HH24:MI'),"
        " to_char(lastview,   'YYYY-MM-DD HH24:MI')"
        " FROM %s WHERE ns_id = %" PRIu32 " AND LOWER(sayer) = LOWER('%s')"
        " ORDER BY lastview ASC LIMIT 1",
        table, ns_id, e_sayer);
  }

  else
    snprintf(sql, sizeof(sql),
        "SELECT id, sayer, quoter, quote,"
        " to_char(created_at, 'YYYY-MM-DD HH24:MI'),"
        " to_char(lastview,   'YYYY-MM-DD HH24:MI')"
        " FROM %s WHERE ns_id = %" PRIu32
        " ORDER BY lastview ASC LIMIT 1",
        table, ns_id);

  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok &&
     res->rows == 1)
  {
    const char *s_id = db_result_get(res, 0, 0);

    out->id = (s_id != NULL) ? (int64_t)strtoll(s_id, NULL, 10) : 0;
    uq_copy_col(out->sayer,    sizeof(out->sayer),    res, 1);
    uq_copy_col(out->quoter,   sizeof(out->quoter),   res, 2);
    uq_copy_col(out->quote,    sizeof(out->quote),    res, 3);
    uq_copy_col(out->created,  sizeof(out->created),  res, 4);
    uq_copy_col(out->lastview, sizeof(out->lastview), res, 5);
    hit = true;
  }

  db_result_free(res);
  res = NULL;

  if(e_sayer != NULL)
    mem_free(e_sayer);

  // Bump lastview so the rotation advances. Best-effort; a failure here
  // does not invalidate the quote we already read.
  if(hit && out->id > 0)
  {
    snprintf(sql, sizeof(sql),
        "UPDATE %s SET lastview = NOW()"
        " WHERE ns_id = %" PRIu32 " AND id = %" PRId64,
        table, ns_id, out->id);

    res = db_result_alloc();

    (void)db_query(sql, res);
    db_result_free(res);
  }

  return(hit);
}

// ------------------------------------------------------------------ //
// Delete                                                              //
// ------------------------------------------------------------------ //

int
uq_db_del(uint32_t ns_id, int64_t id)
{
  db_result_t *res = NULL;
  char         table[UQ_TABLE_SZ];
  char         sql[256];
  int          affected = -1;

  if(id <= 0)
    return(-1);

  if(uq_table_name(table, sizeof(table)) != SUCCESS)
    return(-1);

  snprintf(sql, sizeof(sql),
      "DELETE FROM %s WHERE ns_id = %" PRIu32 " AND id = %" PRId64,
      table, ns_id, id);

  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok)
    affected = (int)res->rows_affected;

  else
    clam(CLAM_WARN, UQ_CTX, "quote delete failed: %s",
        (res->error[0] != '\0')
            ? res->error : "(no driver error)");

  db_result_free(res);
  return(affected);
}
