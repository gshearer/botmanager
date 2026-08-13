// botmanager — MIT
// note persistence: schema bootstrap, insert, and the atomic claim that
// makes delivery exactly-once. Every value that originates from a user is
// routed through db_escape before it touches a query; the only
// interpolated identifier is the table name, which is validated as a
// strict SQL identifier by note_table_name().

#define NOTE_INTERNAL
#include "note.h"

#include "alloc.h"
#include "db.h"
#include "kv.h"
#include "validate.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------ //
// Table naming                                                        //
// ------------------------------------------------------------------ //

bool
note_table_name(char *out, size_t cap)
{
  const char *name = kv_get_str(NOTE_KV_TABLE);
  size_t      len;

  if(out == NULL || cap == 0)
    return(FAIL);

  if(name == NULL || name[0] == '\0')
    name = "notes";

  len = strlen(name);

  // Guard against injection: the table name is pasted into DDL/queries
  // verbatim, so it must be a bare identifier (letters, digits, '_').
  if(len >= cap || !validate_alnum(name, cap - 1))
  {
    clam(CLAM_WARN, NOTE_CTX,
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
static bool            note_schema_done = false;
static pthread_mutex_t note_schema_lock = PTHREAD_MUTEX_INITIALIZER;

static bool
note_run_ddl(const char *sql)
{
  db_result_t *res = db_result_alloc();
  bool         ok  = SUCCESS;

  if(res == NULL)
    return(FAIL);

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, NOTE_CTX, "ddl failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");
    ok = FAIL;
  }

  db_result_free(res);
  return(ok);
}

bool
note_schema_ensure(void)
{
  char table[NOTE_TABLE_SZ];
  char sql[1024];
  bool ok = SUCCESS;

  pthread_mutex_lock(&note_schema_lock);

  if(note_schema_done)
  {
    pthread_mutex_unlock(&note_schema_lock);
    return(SUCCESS);
  }

  if(note_table_name(table, sizeof(table)) != SUCCESS)
  {
    pthread_mutex_unlock(&note_schema_lock);
    return(FAIL);
  }

  // Notes are userns-scoped: both parties are users of the same
  // namespace. method/channel record where the note was *left*, purely
  // as provenance — delivery happens wherever the recipient turns up.
  // delivered_at NULL is the whole pending state; there is no flag to
  // fall out of step with it.
  snprintf(sql, sizeof(sql),
      "CREATE TABLE IF NOT EXISTS %s ("
      " id           BIGSERIAL    PRIMARY KEY,"
      " ns_id        INTEGER      NOT NULL,"
      " sender       VARCHAR(64)  NOT NULL DEFAULT '',"
      " recipient    VARCHAR(64)  NOT NULL,"
      " body         TEXT         NOT NULL,"
      " method       VARCHAR(64)  NOT NULL DEFAULT '',"
      " channel      VARCHAR(128) NOT NULL DEFAULT '',"
      " created_at   TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
      " delivered_at TIMESTAMPTZ"
      ")", table);

  if(note_run_ddl(sql) != SUCCESS)
    ok = FAIL;

  // Partial index: the only query on the hot path asks for one
  // recipient's *undelivered* notes, and delivered rows are dead weight
  // in that lookup forever after.
  if(ok == SUCCESS)
  {
    snprintf(sql, sizeof(sql),
        "CREATE INDEX IF NOT EXISTS idx_%s_pending ON %s(ns_id, recipient)"
        " WHERE delivered_at IS NULL", table, table);

    if(note_run_ddl(sql) != SUCCESS)
      ok = FAIL;
  }

  if(ok == SUCCESS)
  {
    note_schema_done = true;
    clam(CLAM_INFO, NOTE_CTX, "note schema ready (table '%s')", table);
  }

  pthread_mutex_unlock(&note_schema_lock);
  return(ok);
}

// ------------------------------------------------------------------ //
// Insert                                                              //
// ------------------------------------------------------------------ //

int64_t
note_db_add(uint32_t ns_id, const char *sender, const char *recipient,
    const char *body, const char *method, const char *channel)
{
  db_result_t *res     = NULL;
  char        *e_from  = NULL;
  char        *e_to    = NULL;
  char        *e_body  = NULL;
  char        *e_meth  = NULL;
  char        *e_chan  = NULL;
  char        *sql     = NULL;
  char         table[NOTE_TABLE_SZ];
  int64_t      id      = -1;

  if(recipient == NULL || recipient[0] == '\0' || body == NULL
      || body[0] == '\0')
    return(-1);

  if(note_table_name(table, sizeof(table)) != SUCCESS)
    return(-1);

  e_from = db_escape(sender  != NULL ? sender  : "");
  e_to   = db_escape(recipient);
  e_body = db_escape(body);
  e_meth = db_escape(method  != NULL ? method  : "");
  e_chan = db_escape(channel != NULL ? channel : "");

  if(e_from == NULL || e_to == NULL || e_body == NULL || e_meth == NULL
      || e_chan == NULL)
    goto out;

  // Query length is dominated by the escaped body, so size the scratch
  // buffer from the actual inputs rather than a fixed stack slab.
  {
    size_t need = 256 + strlen(table) + strlen(e_from) + strlen(e_to)
        + strlen(e_body) + strlen(e_meth) + strlen(e_chan);

    sql = mem_alloc(NOTE_CTX, "add_sql", need);

    snprintf(sql, need,
        "INSERT INTO %s (ns_id, sender, recipient, body, method, channel)"
        " VALUES (%" PRIu32 ", '%s', '%s', '%s', '%s', '%s')"
        " RETURNING id",
        table, ns_id, e_from, e_to, e_body, e_meth, e_chan);
  }

  res = db_result_alloc();

  if(res != NULL && db_query(sql, res) == SUCCESS && res->ok
      && res->rows == 1)
  {
    const char *s = db_result_get(res, 0, 0);

    if(s != NULL)
      id = (int64_t)strtoll(s, NULL, 10);
  }

  else
    clam(CLAM_WARN, NOTE_CTX, "note insert failed: %s",
        (res != NULL && res->error[0] != '\0')
            ? res->error : "(no driver error)");

out:
  db_result_free(res);
  if(e_from != NULL) mem_free(e_from);
  if(e_to   != NULL) mem_free(e_to);
  if(e_body != NULL) mem_free(e_body);
  if(e_meth != NULL) mem_free(e_meth);
  if(e_chan != NULL) mem_free(e_chan);
  if(sql    != NULL) mem_free(sql);

  return(id);
}

// ------------------------------------------------------------------ //
// Pending count                                                       //
// ------------------------------------------------------------------ //

int
note_db_pending_count(uint32_t ns_id, const char *recipient)
{
  db_result_t *res   = NULL;
  char        *e_to  = NULL;
  char         table[NOTE_TABLE_SZ];
  char         sql[512];
  int          count = -1;

  if(recipient == NULL || recipient[0] == '\0')
    return(-1);

  if(note_table_name(table, sizeof(table)) != SUCCESS)
    return(-1);

  e_to = db_escape(recipient);

  if(e_to == NULL)
    return(-1);

  snprintf(sql, sizeof(sql),
      "SELECT COUNT(*) FROM %s WHERE ns_id = %" PRIu32
      " AND recipient = '%s' AND delivered_at IS NULL",
      table, ns_id, e_to);

  res = db_result_alloc();

  if(res != NULL && db_query(sql, res) == SUCCESS && res->ok
      && res->rows == 1)
  {
    const char *s = db_result_get(res, 0, 0);

    if(s != NULL)
      count = (int)strtol(s, NULL, 10);
  }

  db_result_free(res);
  mem_free(e_to);

  return(count);
}

// ------------------------------------------------------------------ //
// Claim                                                               //
// ------------------------------------------------------------------ //

static void
note_copy_col(char *dst, size_t cap, const db_result_t *res, uint32_t row,
    uint32_t col)
{
  const char *s = db_result_get(res, row, col);

  if(s == NULL)
    s = "";

  snprintf(dst, cap, "%s", s);
}

int
note_db_claim(uint32_t ns_id, const char *recipient, note_row_t *out,
    uint32_t cap)
{
  db_result_t *res    = NULL;
  char        *e_to   = NULL;
  char         table[NOTE_TABLE_SZ];
  char         sql[768];
  int          got    = -1;

  if(recipient == NULL || recipient[0] == '\0' || out == NULL || cap == 0)
    return(-1);

  if(note_table_name(table, sizeof(table)) != SUCCESS)
    return(-1);

  e_to = db_escape(recipient);

  if(e_to == NULL)
    return(-1);

  // Claim and read in one statement. The innermost SELECT picks the
  // oldest pending notes; the UPDATE stamps them; RETURNING hands back
  // exactly the rows this caller won. A second bot witnessing the same
  // line reaches an empty set and stays quiet.
  //
  // The outer SELECT is not redundant: RETURNING yields rows in whatever
  // order the UPDATE touched them, which is not the inner ORDER BY. Sort
  // the claimed set again so mail is read oldest-first.
  snprintf(sql, sizeof(sql),
      "WITH claimed AS ("
      "UPDATE %s SET delivered_at = NOW() WHERE id IN ("
      "SELECT id FROM %s WHERE ns_id = %" PRIu32 " AND recipient = '%s'"
      " AND delivered_at IS NULL ORDER BY id ASC LIMIT %" PRIu32 ")"
      " RETURNING id, sender, body,"
      " EXTRACT(EPOCH FROM created_at)::BIGINT AS created_epoch)"
      " SELECT id, sender, body, created_epoch FROM claimed"
      " ORDER BY id ASC",
      table, table, ns_id, e_to, cap);

  res = db_result_alloc();

  if(res != NULL && db_query(sql, res) == SUCCESS && res->ok)
  {
    uint32_t rows = (res->rows < cap) ? res->rows : cap;

    for(uint32_t i = 0; i < rows; i++)
    {
      const char *s_id = db_result_get(res, i, 0);
      const char *s_ts = db_result_get(res, i, 3);

      out[i].id      = (s_id != NULL) ? (int64_t)strtoll(s_id, NULL, 10) : 0;
      out[i].created = (s_ts != NULL) ? (time_t)strtoll(s_ts, NULL, 10) : 0;
      note_copy_col(out[i].sender, sizeof(out[i].sender), res, i, 1);
      note_copy_col(out[i].body,   sizeof(out[i].body),   res, i, 2);
    }

    got = (int)rows;
  }

  else
    clam(CLAM_WARN, NOTE_CTX, "note claim failed: %s",
        (res != NULL && res->error[0] != '\0')
            ? res->error : "(no driver error)");

  db_result_free(res);
  mem_free(e_to);

  return(got);
}

// ------------------------------------------------------------------ //
// Pending-set bootstrap                                               //
// ------------------------------------------------------------------ //

int
note_db_pending_reload(void)
{
  db_result_t *res = NULL;
  char         table[NOTE_TABLE_SZ];
  char         sql[256];
  int          loaded = -1;

  if(note_table_name(table, sizeof(table)) != SUCCESS)
    return(-1);

  snprintf(sql, sizeof(sql),
      "SELECT DISTINCT ns_id, recipient FROM %s WHERE delivered_at IS NULL",
      table);

  res = db_result_alloc();

  if(res != NULL && db_query(sql, res) == SUCCESS && res->ok)
  {
    for(uint32_t i = 0; i < res->rows; i++)
    {
      const char *s_ns = db_result_get(res, i, 0);
      const char *s_to = db_result_get(res, i, 1);

      if(s_ns == NULL || s_to == NULL)
        continue;

      note_pending_add((uint32_t)strtoul(s_ns, NULL, 10), s_to);
    }

    loaded = (int)res->rows;
  }

  db_result_free(res);
  return(loaded);
}
