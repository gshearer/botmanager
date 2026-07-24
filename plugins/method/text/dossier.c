// botmanager — MIT
// Chat bot dossier store: per-user fact records and mention indexing.
#define DOSSIER_INTERNAL
#include "dossier.h"

#include <ctype.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "identity.h"

// Module state

static bool                   dossier_ready   = false;
static pthread_mutex_t        dossier_stat_mutex;
static dossier_stats_t        dossier_stats;

// Stat helpers

static void
dossier_stat_bump_resolves(void)
{
  pthread_mutex_lock(&dossier_stat_mutex);
  dossier_stats.resolves++;
  pthread_mutex_unlock(&dossier_stat_mutex);
}

static void
dossier_stat_bump_creates(void)
{
  pthread_mutex_lock(&dossier_stat_mutex);
  dossier_stats.creates++;
  pthread_mutex_unlock(&dossier_stat_mutex);
}

static void
dossier_stat_bump_sightings(void)
{
  pthread_mutex_lock(&dossier_stat_mutex);
  dossier_stats.sightings++;
  pthread_mutex_unlock(&dossier_stat_mutex);
}

static void
dossier_stat_bump_merges(uint64_t n)
{
  pthread_mutex_lock(&dossier_stat_mutex);
  dossier_stats.merges += n;
  pthread_mutex_unlock(&dossier_stat_mutex);
}

static void
dossier_stat_bump_scorer(void)
{
  pthread_mutex_lock(&dossier_stat_mutex);
  dossier_stats.scorer_calls++;
  pthread_mutex_unlock(&dossier_stat_mutex);
}

// Schema

// Run a single DDL statement, warning on failure but not aborting.
static void
dossier_run_ddl(const char *sql)
{
  db_result_t *res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    if(res->error[0] != '\0')
      clam(CLAM_WARN, "dossier", "ensure_tables: %s", res->error);
  }

  db_result_free(res);
}

static void
dossier_ensure_tables(void)
{
  dossier_run_ddl(
      "CREATE TABLE IF NOT EXISTS dossier ("
      " id             BIGSERIAL    PRIMARY KEY,"
      " ns_id          INTEGER      NOT NULL REFERENCES userns(id) ON DELETE CASCADE,"
      " user_id        INTEGER               REFERENCES userns_user(id) ON DELETE SET NULL,"
      " display_label  VARCHAR(128) NOT NULL DEFAULT '',"
      " first_seen     TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
      " last_seen      TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
      " message_count  INTEGER      NOT NULL DEFAULT 0"
      ")");

  dossier_run_ddl(
      "CREATE INDEX IF NOT EXISTS idx_dossier_ns_user"
      " ON dossier(ns_id, user_id)");

  dossier_run_ddl(
      "CREATE TABLE IF NOT EXISTS dossier_signature ("
      " id            BIGSERIAL    PRIMARY KEY,"
      " dossier_id    BIGINT       NOT NULL REFERENCES dossier(id) ON DELETE CASCADE,"
      " method_kind   VARCHAR(64)  NOT NULL,"
      " nickname      VARCHAR(64)  NOT NULL DEFAULT '',"
      " username      VARCHAR(64)  NOT NULL DEFAULT '',"
      " hostname      VARCHAR(128) NOT NULL DEFAULT '',"
      " verified_id   VARCHAR(128) NOT NULL DEFAULT '',"
      " seen_count    INTEGER      NOT NULL DEFAULT 1,"
      " last_seen     TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
      " UNIQUE(dossier_id, method_kind, nickname, username, hostname, verified_id)"
      ")");

  dossier_run_ddl(
      "CREATE INDEX IF NOT EXISTS idx_dossier_sig_pmk"
      " ON dossier_signature(dossier_id, method_kind)");
  dossier_run_ddl(
      "CREATE INDEX IF NOT EXISTS idx_dossier_sig_mk"
      " ON dossier_signature(method_kind)");
  dossier_run_ddl(
      "CREATE INDEX IF NOT EXISTS idx_dossier_sig_verified"
      " ON dossier_signature(method_kind, verified_id)"
      " WHERE verified_id <> ''");

  dossier_run_ddl(
      "CREATE TABLE IF NOT EXISTS dossier_facts ("
      " id           BIGSERIAL    PRIMARY KEY,"
      " dossier_id   BIGINT       NOT NULL REFERENCES dossier(id) ON DELETE CASCADE,"
      " kind         SMALLINT     NOT NULL,"
      " fact_key     VARCHAR(128) NOT NULL,"
      " fact_value   TEXT         NOT NULL,"
      " source       VARCHAR(32)  NOT NULL DEFAULT 'llm_extract',"
      " channel      VARCHAR(128) NOT NULL DEFAULT '',"
      " confidence   REAL         NOT NULL DEFAULT 0.6,"
      " observed_at  TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
      " last_seen    TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
      " UNIQUE (dossier_id, kind, fact_key)"
      ")");

  dossier_run_ddl(
      "CREATE INDEX IF NOT EXISTS idx_dossier_facts_pid"
      " ON dossier_facts(dossier_id)");
  dossier_run_ddl(
      "CREATE INDEX IF NOT EXISTS idx_dossier_facts_lastseen"
      " ON dossier_facts(last_seen)");

  // conversation_log.dossier_id was created by memory_register_config()
  // without a FK because the dossier table did not yet exist. Now that
  // dossier is in place, attach the FK (idempotent).
  dossier_run_ddl(
      "DO $$ BEGIN"
      "  IF NOT EXISTS ("
      "    SELECT 1 FROM pg_constraint"
      "    WHERE conname = 'conversation_log_dossier_fk') THEN"
      "    ALTER TABLE conversation_log"
      "      ADD CONSTRAINT conversation_log_dossier_fk"
      "      FOREIGN KEY (dossier_id) REFERENCES dossier(id)"
      "      ON DELETE SET NULL;"
      "  END IF;"
      " END $$");
}

// Small helpers: escape every quad-tuple field plus method_kind for
// safe SQL embedding, free any partial allocation on failure.

typedef struct
{
  char *method_kind;
  char *nickname;
  char *username;
  char *hostname;
  char *verified_id;
} dossier_sig_escaped_t;

static void
dossier_sig_escaped_free(dossier_sig_escaped_t *e)
{
  if(e->method_kind) mem_free(e->method_kind);
  if(e->nickname)    mem_free(e->nickname);
  if(e->username)    mem_free(e->username);
  if(e->hostname)    mem_free(e->hostname);
  if(e->verified_id) mem_free(e->verified_id);
  memset(e, 0, sizeof(*e));
}

// Escape every borrowed string on `sig` for safe SQL embedding. On any
// db_escape() OOM, every already-allocated buffer is freed and the
// caller sees zeroed pointers — same shape as a fresh, unused struct.
static bool
dossier_sig_escape(const dossier_sig_t *sig, dossier_sig_escaped_t *out)
{
  memset(out, 0, sizeof(*out));

  out->method_kind = db_escape(sig->method_kind);
  out->nickname    = db_escape(sig->nickname    != NULL ? sig->nickname    : "");
  out->username    = db_escape(sig->username    != NULL ? sig->username    : "");
  out->hostname    = db_escape(sig->hostname    != NULL ? sig->hostname    : "");
  out->verified_id = db_escape(sig->verified_id != NULL ? sig->verified_id : "");

  if(out->method_kind == NULL || out->nickname    == NULL
      || out->username == NULL || out->hostname    == NULL
      || out->verified_id == NULL)
  {
    dossier_sig_escaped_free(out);
    return(FAIL);
  }

  return(SUCCESS);
}

// Dossier row creation

// Insert a new dossier + its first signature row in two statements.
// Returns the new dossier_id, or 0 on failure.
static dossier_id_t
dossier_create(uint32_t ns_id, const dossier_sig_t *sig,
    const char *display_label)
{
  char *e_label = db_escape(display_label != NULL ? display_label : "");
  db_result_t *res;
  dossier_id_t new_id;
  char sql[DOSSIER_SQL_SZ];

  if(e_label == NULL)
    return(0);

  snprintf(sql, sizeof(sql),
      "INSERT INTO dossier (ns_id, display_label, message_count)"
      " VALUES (%u, '%s', 1) RETURNING id",
      ns_id, e_label);

  mem_free(e_label);

  res = db_result_alloc();
  new_id = 0;

  if(db_query(sql, res) == SUCCESS && res->ok && res->rows > 0)
  {
    const char *id_s = db_result_get(res, 0, 0);

    if(id_s != NULL)
      new_id = (dossier_id_t)strtoll(id_s, NULL, 10);
  }

  else if(res->error[0] != '\0')
    clam(CLAM_WARN, "dossier", "create: %s", res->error);

  db_result_free(res);

  if(new_id == 0)
    return(0);

  // Insert initial signature row.
  {
    dossier_sig_escaped_t e;

    if(dossier_sig_escape(sig, &e) != SUCCESS)
    {
      clam(CLAM_WARN, "dossier", "create: escape failed for sig");
      return(new_id);   // dossier row is there; signature will be added on next sighting
    }

    snprintf(sql, sizeof(sql),
        "INSERT INTO dossier_signature"
        " (dossier_id, method_kind, nickname, username, hostname, verified_id)"
        " VALUES (%" PRId64 ", '%s', '%s', '%s', '%s', '%s')"
        " ON CONFLICT (dossier_id, method_kind, nickname, username, hostname,"
        "              verified_id) DO NOTHING",
        (int64_t)new_id, e.method_kind, e.nickname, e.username,
        e.hostname, e.verified_id);

    dossier_sig_escaped_free(&e);
  }

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
    clam(CLAM_WARN, "dossier", "create sig: %s", res->error);

  db_result_free(res);

  dossier_stat_bump_creates();
  dossier_stat_bump_sightings();
  return(new_id);
}

// Sighting bookkeeping

// Upsert the signature row (bumps seen_count) and bump the dossier's
// last_seen/message_count. Two statements; caller bears the cost.
bool
dossier_record_sighting(dossier_id_t dossier_id, const dossier_sig_t *sig)
{
  db_result_t *res;
  bool ok;
  char sql[DOSSIER_SQL_SZ];
  dossier_sig_escaped_t e;

  if(!dossier_ready || dossier_id <= 0 || sig == NULL
      || sig->method_kind == NULL)
    return(FAIL);

  if(dossier_sig_escape(sig, &e) != SUCCESS)
    return(FAIL);

  snprintf(sql, sizeof(sql),
      "INSERT INTO dossier_signature"
      " (dossier_id, method_kind, nickname, username, hostname, verified_id)"
      " VALUES (%" PRId64 ", '%s', '%s', '%s', '%s', '%s')"
      " ON CONFLICT (dossier_id, method_kind, nickname, username, hostname,"
      "              verified_id) DO UPDATE"
      " SET seen_count = dossier_signature.seen_count + 1,"
      "     last_seen  = NOW()",
      (int64_t)dossier_id, e.method_kind, e.nickname, e.username,
      e.hostname, e.verified_id);

  dossier_sig_escaped_free(&e);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok;

  if(!ok && res->error[0] != '\0')
    clam(CLAM_WARN, "dossier", "record_sighting (sig): %s", res->error);

  db_result_free(res);

  if(!ok)
    return(FAIL);

  snprintf(sql, sizeof(sql),
      "UPDATE dossier SET last_seen = NOW(),"
      " message_count = message_count + 1"
      " WHERE id = %" PRId64,
      (int64_t)dossier_id);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok;

  if(!ok && res->error[0] != '\0')
    clam(CLAM_WARN, "dossier", "record_sighting (p): %s", res->error);

  db_result_free(res);

  if(ok)
    dossier_stat_bump_sightings();

  return(ok ? SUCCESS : FAIL);
}

// Resolution

dossier_id_t
dossier_resolve(uint32_t ns_id, const dossier_sig_t *sig,
    const char *display_label, bool create_if_missing)
{
  db_result_t *res;
  dossier_id_t best_id;
  float        best_score;
  char sql[DOSSIER_SQL_SZ];
  char *e_mk;
  bool any_field;

  if(!dossier_ready || sig == NULL
      || sig->method_kind == NULL || sig->method_kind[0] == '\0')
    return(0);

  // Refuse a fully-empty quad-tuple. Otherwise every protocol that
  // didn't successfully parse its identity would collide on the same
  // empty-tuple dossier.
  any_field =
      (sig->nickname    != NULL && sig->nickname   [0] != '\0')
      || (sig->username    != NULL && sig->username   [0] != '\0')
      || (sig->hostname    != NULL && sig->hostname   [0] != '\0')
      || (sig->verified_id != NULL && sig->verified_id[0] != '\0');

  if(!any_field)
    return(0);

  dossier_stat_bump_resolves();

  e_mk = db_escape(sig->method_kind);
  if(e_mk == NULL)
    return(0);

  // Two-stage candidate fetch. When the inbound sig has a verified_id,
  // we can short-circuit to "any signature in this namespace with the
  // same method_kind and the same verified_id" — that's the strongest
  // possible signal and chat_identity_score returns 1.0 for it.
  // Otherwise walk every signature in the (ns_id, method_kind) bucket.
  if(sig->verified_id != NULL && sig->verified_id[0] != '\0')
  {
    char *e_vid = db_escape(sig->verified_id);

    if(e_vid == NULL)
    {
      mem_free(e_mk);
      return(0);
    }

    snprintf(sql, sizeof(sql),
        "SELECT ps.dossier_id, ps.nickname, ps.username, ps.hostname,"
        "       ps.verified_id"
        " FROM dossier_signature ps"
        " JOIN dossier p ON p.id = ps.dossier_id"
        " WHERE p.ns_id = %u AND ps.method_kind = '%s'"
        "   AND ps.verified_id = '%s'",
        ns_id, e_mk, e_vid);

    mem_free(e_vid);
  }
  else
  {
    snprintf(sql, sizeof(sql),
        "SELECT ps.dossier_id, ps.nickname, ps.username, ps.hostname,"
        "       ps.verified_id"
        " FROM dossier_signature ps"
        " JOIN dossier p ON p.id = ps.dossier_id"
        " WHERE p.ns_id = %u AND ps.method_kind = '%s'",
        ns_id, e_mk);
  }

  mem_free(e_mk);

  res = db_result_alloc();
  best_id = 0;
  best_score = 0.0f;

  if(db_query(sql, res) == SUCCESS && res->ok)
  {
    for(uint32_t i = 0; i < res->rows; i++)
    {
      const char *pid_s = db_result_get(res, i, 0);
      const char *nick  = db_result_get(res, i, 1);
      const char *uname = db_result_get(res, i, 2);
      const char *hname = db_result_get(res, i, 3);
      const char *vid   = db_result_get(res, i, 4);
      dossier_sig_t existing;
      float         score;

      if(pid_s == NULL)
        continue;

      existing.method_kind = sig->method_kind;
      existing.nickname    = nick  != NULL ? nick  : "";
      existing.username    = uname != NULL ? uname : "";
      existing.hostname    = hname != NULL ? hname : "";
      existing.verified_id = vid   != NULL ? vid   : "";

      score = chat_identity_score(sig, &existing);
      dossier_stat_bump_scorer();

      if(score > best_score)
      {
        best_score = score;
        best_id    = (dossier_id_t)strtoll(pid_s, NULL, 10);
      }
    }
  }

  else if(res->error[0] != '\0')
    clam(CLAM_WARN, "dossier", "resolve: %s", res->error);

  db_result_free(res);

  if(best_score >= DOSSIER_MATCH_THRESHOLD && best_id > 0)
  {
    dossier_record_sighting(best_id, sig);
    return(best_id);
  }

  if(create_if_missing)
    return(dossier_create(ns_id, sig, display_label));

  return(0);
}

// Attach / detach userns_user link

bool
dossier_set_user(dossier_id_t dossier_id, int user_id)
{
  db_result_t *res;
  bool ok;
  char sql[256];

  if(!dossier_ready || dossier_id <= 0)
    return(FAIL);

  if(user_id > 0)
    snprintf(sql, sizeof(sql),
        "UPDATE dossier SET user_id = %d WHERE id = %" PRId64,
        user_id, (int64_t)dossier_id);
  else
    snprintf(sql, sizeof(sql),
        "UPDATE dossier SET user_id = NULL WHERE id = %" PRId64,
        (int64_t)dossier_id);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok;

  if(!ok && res->error[0] != '\0')
    clam(CLAM_WARN, "dossier", "set_user: %s", res->error);

  db_result_free(res);
  return(ok ? SUCCESS : FAIL);
}

// Merge

// Absorb a single source dossier into the survivor: reassign signatures
// and facts that don't collide, let colliding rows fall through ON
// DELETE CASCADE when the source row is removed, and unify the counters
// on the survivor.
static bool
dossier_merge_one(dossier_id_t survivor, dossier_id_t source)
{
  db_result_t *res;
  bool ok;
  char sql[DOSSIER_SQL_SZ];

  if(survivor == source)
    return(FAIL);

  // Reassign signatures that don't collide on the survivor. Colliding
  // signatures stay on the source and will be cascaded away by the
  // final DELETE.
  snprintf(sql, sizeof(sql),
      "UPDATE dossier_signature ps SET dossier_id = %" PRId64
      " WHERE ps.dossier_id = %" PRId64
      " AND NOT EXISTS (SELECT 1 FROM dossier_signature ps2"
      "                  WHERE ps2.dossier_id  = %" PRId64
      "                    AND ps2.method_kind = ps.method_kind"
      "                    AND ps2.nickname    = ps.nickname"
      "                    AND ps2.username    = ps.username"
      "                    AND ps2.hostname    = ps.hostname"
      "                    AND ps2.verified_id = ps.verified_id)",
      (int64_t)survivor, (int64_t)source, (int64_t)survivor);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok;

  if(!ok && res->error[0] != '\0')
    clam(CLAM_WARN, "dossier", "merge sig: %s", res->error);

  db_result_free(res);

  if(!ok)
    return(FAIL);

  // Reassign facts that don't collide.
  snprintf(sql, sizeof(sql),
      "UPDATE dossier_facts pf SET dossier_id = %" PRId64
      " WHERE pf.dossier_id = %" PRId64
      " AND NOT EXISTS (SELECT 1 FROM dossier_facts pf2"
      "                  WHERE pf2.dossier_id = %" PRId64
      "                    AND pf2.kind       = pf.kind"
      "                    AND pf2.fact_key   = pf.fact_key)",
      (int64_t)survivor, (int64_t)source, (int64_t)survivor);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok;

  if(!ok && res->error[0] != '\0')
    clam(CLAM_WARN, "dossier", "merge facts: %s", res->error);

  db_result_free(res);

  if(!ok)
    return(FAIL);

  // Unify counters on the survivor using LEAST/GREATEST.
  snprintf(sql, sizeof(sql),
      "UPDATE dossier s SET"
      " message_count = s.message_count + src.message_count,"
      " first_seen    = LEAST(s.first_seen, src.first_seen),"
      " last_seen     = GREATEST(s.last_seen, src.last_seen)"
      " FROM dossier src"
      " WHERE s.id = %" PRId64 " AND src.id = %" PRId64,
      (int64_t)survivor, (int64_t)source);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok;

  if(!ok && res->error[0] != '\0')
    clam(CLAM_WARN, "dossier", "merge unify: %s", res->error);

  db_result_free(res);

  if(!ok)
    return(FAIL);

  // Delete the source dossier; any remaining colliding signatures and
  // facts still attached to it cascade via FK.
  snprintf(sql, sizeof(sql),
      "DELETE FROM dossier WHERE id = %" PRId64,
      (int64_t)source);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok;

  if(!ok && res->error[0] != '\0')
    clam(CLAM_WARN, "dossier", "merge delete: %s", res->error);

  db_result_free(res);
  return(ok ? SUCCESS : FAIL);
}

bool
dossier_merge(dossier_id_t survivor_id,
    const dossier_id_t *absorbed_ids, size_t n_absorbed)
{
  uint64_t merged;
  dossier_info_t s_info;

  if(!dossier_ready || survivor_id <= 0
      || absorbed_ids == NULL || n_absorbed == 0)
    return(FAIL);

  // Sanity: every absorbed id must be in the same namespace as the
  // survivor. We verify up front to avoid partial merges.

  if(dossier_get(survivor_id, &s_info) != SUCCESS)
    return(FAIL);

  for(size_t i = 0; i < n_absorbed; i++)
  {
    dossier_info_t a_info;

    if(absorbed_ids[i] == survivor_id || absorbed_ids[i] <= 0)
      return(FAIL);

    if(dossier_get(absorbed_ids[i], &a_info) != SUCCESS)
      return(FAIL);

    if(a_info.ns_id != s_info.ns_id)
    {
      clam(CLAM_WARN, "dossier",
          "merge: absorbed %" PRId64 " namespace %u != survivor %u",
          (int64_t)absorbed_ids[i], a_info.ns_id, s_info.ns_id);
      return(FAIL);
    }
  }

  merged = 0;

  for(size_t i = 0; i < n_absorbed; i++)
  {
    if(dossier_merge_one(survivor_id, absorbed_ids[i]) == SUCCESS)
      merged++;

    else
      return(FAIL);
  }

  dossier_stat_bump_merges(merged);
  return(SUCCESS);
}

// Lookup

bool
dossier_get(dossier_id_t dossier_id, dossier_info_t *out)
{
  db_result_t *res;
  bool ok;
  char sql[256];

  if(!dossier_ready || dossier_id <= 0 || out == NULL)
    return(FAIL);

  memset(out, 0, sizeof(*out));

  snprintf(sql, sizeof(sql),
      "SELECT id, ns_id, COALESCE(user_id, 0), display_label,"
      " EXTRACT(EPOCH FROM first_seen)::bigint,"
      " EXTRACT(EPOCH FROM last_seen)::bigint,"
      " message_count"
      " FROM dossier WHERE id = %" PRId64,
      (int64_t)dossier_id);

  res = db_result_alloc();
  ok = false;

  if(db_query(sql, res) == SUCCESS && res->ok && res->rows > 0)
  {
    const char *v;

    v = db_result_get(res, 0, 0);
    if(v != NULL) out->id = (dossier_id_t)strtoll(v, NULL, 10);

    v = db_result_get(res, 0, 1);
    if(v != NULL) out->ns_id = (uint32_t)strtoul(v, NULL, 10);

    v = db_result_get(res, 0, 2);
    if(v != NULL) out->user_id_or_0 = (int)strtol(v, NULL, 10);

    v = db_result_get(res, 0, 3);
    if(v != NULL) snprintf(out->display_label, sizeof(out->display_label), "%s", v);

    v = db_result_get(res, 0, 4);
    if(v != NULL) out->first_seen = (time_t)strtoll(v, NULL, 10);

    v = db_result_get(res, 0, 5);
    if(v != NULL) out->last_seen = (time_t)strtoll(v, NULL, 10);

    v = db_result_get(res, 0, 6);
    if(v != NULL) out->message_count = (uint32_t)strtoul(v, NULL, 10);

    ok = (out->id == dossier_id);
  }

  else if(res->error[0] != '\0')
    clam(CLAM_WARN, "dossier", "get: %s", res->error);

  db_result_free(res);
  return(ok ? SUCCESS : FAIL);
}

// Split

bool
dossier_split_signature(int64_t signature_id, dossier_id_t *out_new_id)
{
  bool ok;
  dossier_id_t new_id;
  char *e_label;
  uint32_t sig_count;
  db_result_t *res;
  dossier_id_t src_pid;
  uint32_t     ns_id;
  char         label[DOSSIER_LABEL_SZ] = {0};
  char sql[DOSSIER_SQL_SZ];

  if(!dossier_ready || signature_id <= 0 || out_new_id == NULL)
    return(FAIL);

  *out_new_id = 0;

  // Look up the signature and its source dossier.
  snprintf(sql, sizeof(sql),
      "SELECT ps.dossier_id, p.ns_id, p.display_label"
      " FROM dossier_signature ps"
      " JOIN dossier p ON p.id = ps.dossier_id"
      " WHERE ps.id = %" PRId64,
      signature_id);

  res = db_result_alloc();
  src_pid = 0;
  ns_id = 0;

  if(db_query(sql, res) == SUCCESS && res->ok && res->rows > 0)
  {
    const char *v;

    v = db_result_get(res, 0, 0);
    if(v != NULL) src_pid = (dossier_id_t)strtoll(v, NULL, 10);

    v = db_result_get(res, 0, 1);
    if(v != NULL) ns_id = (uint32_t)strtoul(v, NULL, 10);

    v = db_result_get(res, 0, 2);
    if(v != NULL) snprintf(label, sizeof(label), "%s", v);
  }

  db_result_free(res);

  if(src_pid <= 0)
    return(FAIL);

  // Refuse to strip the source dossier of its last signature.
  snprintf(sql, sizeof(sql),
      "SELECT COUNT(*) FROM dossier_signature"
      " WHERE dossier_id = %" PRId64,
      (int64_t)src_pid);

  res = db_result_alloc();
  sig_count = 0;

  if(db_query(sql, res) == SUCCESS && res->ok && res->rows > 0)
  {
    const char *v = db_result_get(res, 0, 0);
    if(v != NULL) sig_count = (uint32_t)strtoul(v, NULL, 10);
  }

  db_result_free(res);

  if(sig_count <= 1)
    return(FAIL);

  // Create a bare new dossier in the same namespace.
  e_label = db_escape(label);

  if(e_label == NULL)
    return(FAIL);

  snprintf(sql, sizeof(sql),
      "INSERT INTO dossier (ns_id, display_label)"
      " VALUES (%u, '%s') RETURNING id",
      ns_id, e_label);

  mem_free(e_label);

  res = db_result_alloc();
  new_id = 0;

  if(db_query(sql, res) == SUCCESS && res->ok && res->rows > 0)
  {
    const char *v = db_result_get(res, 0, 0);
    if(v != NULL) new_id = (dossier_id_t)strtoll(v, NULL, 10);
  }

  else if(res->error[0] != '\0')
    clam(CLAM_WARN, "dossier", "split create: %s", res->error);

  db_result_free(res);

  if(new_id == 0)
    return(FAIL);

  // Reassign the signature to the new dossier.
  snprintf(sql, sizeof(sql),
      "UPDATE dossier_signature SET dossier_id = %" PRId64
      " WHERE id = %" PRId64,
      (int64_t)new_id, signature_id);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok;

  if(!ok && res->error[0] != '\0')
    clam(CLAM_WARN, "dossier", "split reassign: %s", res->error);

  db_result_free(res);

  if(!ok)
  {
    // Roll back the new dossier row to avoid orphaning it.
    snprintf(sql, sizeof(sql),
        "DELETE FROM dossier WHERE id = %" PRId64,
        (int64_t)new_id);
    res = db_result_alloc();
    db_query(sql, res);
    db_result_free(res);
    return(FAIL);
  }

  *out_new_id = new_id;
  return(SUCCESS);
}

// Candidate search

// True when pid is already present in out_ids (linear scan; caller
// caps n_out). Keeps the result set deduplicated without an extra
// hash-set allocation.
static bool
dossier_ids_contains(const dossier_id_t *ids, size_t n, dossier_id_t pid)
{
  for(size_t i = 0; i < n; i++)
    if(ids[i] == pid)
      return(true);

  return(false);
}

size_t
dossier_find_candidates(uint32_t ns_id,
    dossier_sig_filter_t filter, void *user,
    dossier_id_t *out_ids, size_t max)
{
  db_result_t *res;
  size_t n_out;
  char sql[DOSSIER_SQL_SZ];

  if(!dossier_ready || filter == NULL || out_ids == NULL || max == 0)
    return(0);

  snprintf(sql, sizeof(sql),
      "SELECT ps.dossier_id, ps.method_kind, ps.nickname, ps.username,"
      "       ps.hostname, ps.verified_id"
      " FROM dossier_signature ps"
      " JOIN dossier p ON p.id = ps.dossier_id"
      " WHERE p.ns_id = %u"
      " ORDER BY ps.dossier_id ASC, ps.last_seen DESC",
      ns_id);

  res = db_result_alloc();
  n_out = 0;

  if(db_query(sql, res) == SUCCESS && res->ok)
  {
    for(uint32_t i = 0; i < res->rows && n_out < max; i++)
    {
      const char *pid_s = db_result_get(res, i, 0);
      const char *mk_s  = db_result_get(res, i, 1);
      const char *nick  = db_result_get(res, i, 2);
      const char *uname = db_result_get(res, i, 3);
      const char *hname = db_result_get(res, i, 4);
      const char *vid   = db_result_get(res, i, 5);
      dossier_id_t pid;
      dossier_sig_t sig;

      if(pid_s == NULL || mk_s == NULL)
        continue;

      pid = (dossier_id_t)strtoll(pid_s, NULL, 10);

      if(dossier_ids_contains(out_ids, n_out, pid))
        continue;

      sig.method_kind = mk_s;
      sig.nickname    = nick  != NULL ? nick  : "";
      sig.username    = uname != NULL ? uname : "";
      sig.hostname    = hname != NULL ? hname : "";
      sig.verified_id = vid   != NULL ? vid   : "";

      if(filter(&sig, user))
        out_ids[n_out++] = pid;
    }
  }

  else if(res->error[0] != '\0')
    clam(CLAM_WARN, "dossier", "find_candidates: %s", res->error);

  db_result_free(res);
  return(n_out);
}

// Mention resolution

// Tiny built-in English stoplist. Intentionally small -- the fact
// extractor will further disambiguate downstream, so we prefer a
// handful of high-traffic words over an exhaustive list that risks
// dropping legitimate nicks (e.g., someone named "Will").
static const char *dossier_stoplist[] = {
  "the","and","for","you","are","was","but","not","with","this",
  "that","have","has","had","she","her","him","his","its","out",
  "they","them","there","then","when","what","why","how","who",
  "where","some","any","all","one","two","from","here","just",
  "about","into","your","our","their","can","will","would","should",
  "could","been","being","does","did","yes","no",
  NULL
};

static bool
dossier_is_stopword(const char *tok, size_t len)
{
  for(uint32_t i = 0; dossier_stoplist[i] != NULL; i++)
  {
    const char *w = dossier_stoplist[i];
    if(strlen(w) != len) continue;
    if(strncasecmp(w, tok, len) == 0) return(true);
  }
  return(false);
}

// Extract the next alphanumeric token of length >= 3 into `out` (NUL
// terminated). Returns a pointer to the position *after* the token in
// `src`, or NULL when no more tokens are available.
static const char *
dossier_next_token(const char *src, char *out, size_t out_sz)
{
  size_t len;
  const char *start;

  // Skip to first alnum byte.
  while(*src != '\0' && !isalnum((unsigned char)*src)) src++;
  if(*src == '\0') return(NULL);

  start = src;
  while(*src != '\0' && isalnum((unsigned char)*src)) src++;

  len = (size_t)(src - start);
  if(len < 3 || len >= out_sz) {
    // Too short or too long: skip and caller will call us again.
    if(len >= out_sz) return(src);   // still advance past oversized token
    return(src);
  }

  memcpy(out, start, len);
  out[len] = '\0';
  return(src);
}

static void
dossier_mention_score_dossier(dossier_id_t pid, const char *method_kind,
    const char *tokens[], size_t n_tokens,
    dossier_id_t *out_ids, size_t *n_out, size_t max)
{
  db_result_t *res;
  char sql[DOSSIER_SQL_SZ];
  char *e_mk;

  if(*n_out >= max) return;
  if(dossier_ids_contains(out_ids, *n_out, pid)) return;

  e_mk = db_escape(method_kind);
  if(e_mk == NULL) return;

  snprintf(sql, sizeof(sql),
      "SELECT nickname, username, hostname, verified_id"
      " FROM dossier_signature"
      " WHERE dossier_id = %" PRId64 " AND method_kind = '%s'",
      (int64_t)pid, e_mk);
  mem_free(e_mk);

  res = db_result_alloc();
  if(db_query(sql, res) == SUCCESS && res->ok)
  {
    for(uint32_t i = 0; i < res->rows; i++)
    {
      const char *nick  = db_result_get(res, i, 0);
      const char *uname = db_result_get(res, i, 1);
      const char *hname = db_result_get(res, i, 2);
      const char *vid   = db_result_get(res, i, 3);
      dossier_sig_t sig = {
        .method_kind = method_kind,
        .nickname    = nick  != NULL ? nick  : "",
        .username    = uname != NULL ? uname : "",
        .hostname    = hname != NULL ? hname : "",
        .verified_id = vid   != NULL ? vid   : "",
      };

      for(size_t t = 0; t < n_tokens; t++)
      {
        if(chat_identity_token_score(tokens[t], &sig)
            >= DOSSIER_MENTION_THRESHOLD)
        {
          out_ids[(*n_out)++] = pid;
          db_result_free(res);
          return;
        }
      }
    }
  }
  db_result_free(res);
}

size_t
dossier_find_mentions(uint32_t ns_id, const char *method_kind,
    uint32_t window_secs, const char *text,
    dossier_id_t *out_ids, size_t max)
{
  db_result_t *res;
  size_t n_out;
  char sql[DOSSIER_SQL_SZ];
  char ts_clause[96] = "";
  char  tokbuf[32][64];
  const char *tokens[32];
  size_t n_tokens;
  const char *p;

  if(!dossier_ready || method_kind == NULL || text == NULL
      || out_ids == NULL || max == 0)
    return(0);

  // Tokenize once.
  n_tokens = 0;

  p = text;
  while(p != NULL && n_tokens < (sizeof(tokbuf) / sizeof(tokbuf[0])))
  {
    char t[64];
    p = dossier_next_token(p, t, sizeof(t));
    if(p == NULL) break;
    if(t[0] == '\0') continue;   // too-short, try next
    if(dossier_is_stopword(t, strlen(t))) continue;

    snprintf(tokbuf[n_tokens], sizeof(tokbuf[n_tokens]), "%s", t);
    tokens[n_tokens] = tokbuf[n_tokens];
    n_tokens++;
  }

  if(n_tokens == 0) return(0);

  // Candidate dossiers: seen anywhere in ns_id within window. The
  // namespace is the identity boundary (users are scoped to userns),
  // so dossiers are too -- channel scoping would be wrong on methods
  // where the same display name is network-unique (e.g. IRC nicks).
  if(window_secs > 0)
    snprintf(ts_clause, sizeof(ts_clause),
        " AND ts > NOW() - INTERVAL '%u seconds'", window_secs);

  snprintf(sql, sizeof(sql),
      "SELECT DISTINCT dossier_id FROM conversation_log"
      " WHERE ns_id = %u AND dossier_id IS NOT NULL%s"
      " LIMIT 512",
      ns_id, ts_clause);

  res = db_result_alloc();
  n_out = 0;

  if(db_query(sql, res) == SUCCESS && res->ok)
  {
    for(uint32_t i = 0; i < res->rows && n_out < max; i++)
    {
      const char *pid_s = db_result_get(res, i, 0);
      dossier_id_t pid;

      if(pid_s == NULL) continue;
      pid = (dossier_id_t)strtoll(pid_s, NULL, 10);
      if(pid <= 0) continue;

      dossier_mention_score_dossier(pid, method_kind,
          tokens, n_tokens, out_ids, &n_out, max);
    }
  }
  db_result_free(res);

  return(n_out);
}

// Stats

void
dossier_get_stats(dossier_stats_t *out)
{
  if(out == NULL)
    return;

  pthread_mutex_lock(&dossier_stat_mutex);
  *out = dossier_stats;
  pthread_mutex_unlock(&dossier_stat_mutex);
}

// Lifecycle

void
dossier_init(void)
{
  if(dossier_ready)
    return;

  pthread_mutex_init(&dossier_stat_mutex, NULL);
  memset(&dossier_stats, 0, sizeof(dossier_stats));

  dossier_ready = true;
  clam(CLAM_INFO, "dossier", "dossier subsystem initialized");
}

void
dossier_register_config(void)
{
  dossier_ensure_tables();
}

void
dossier_exit(void)
{
  if(!dossier_ready)
    return;

  dossier_ready = false;

  pthread_mutex_destroy(&dossier_stat_mutex);

  clam(CLAM_INFO, "dossier", "dossier subsystem shut down");
}

// Test hooks

#ifdef DOSSIER_TEST_HOOKS

bool
dossier_test_delete(dossier_id_t dossier_id)
{
  if(!dossier_ready || dossier_id <= 0)
    return(FAIL);

  char sql[128];
  snprintf(sql, sizeof(sql),
      "DELETE FROM dossier WHERE id = %" PRId64,
      (int64_t)dossier_id);

  db_result_t *res = db_result_alloc();
  bool ok = (db_query(sql, res) == SUCCESS) && res->ok;
  db_result_free(res);
  return(ok ? SUCCESS : FAIL);
}

void
dossier_test_reset_stats(void)
{
  pthread_mutex_lock(&dossier_stat_mutex);
  memset(&dossier_stats, 0, sizeof(dossier_stats));
  pthread_mutex_unlock(&dossier_stat_mutex);
}

bool
dossier_test_db_ok(void)
{
  db_result_t *r = db_result_alloc();
  bool ok = (db_query("SELECT 1", r) == SUCCESS) && r->ok;
  db_result_free(r);
  return(ok);
}

#endif // DOSSIER_TEST_HOOKS
