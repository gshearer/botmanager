// botmanager — MIT
// melee persistence: table naming and the schema bootstrap for the pit's
// three tables. The configured prefix is the only identifier ever pasted
// into SQL verbatim, so it is validated as a bare identifier first; every
// user-originated value goes through db_escape at its own call site.

#define MELEE_INTERNAL
#include "melee.h"

#include "db.h"
#include "kv.h"
#include "validate.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------------ //
// Table naming                                                        //
// ------------------------------------------------------------------ //

// Guard against injection *and* against an identifier Postgres would
// refuse: the prefix is pasted into DDL and queries verbatim, so it must
// be letters, digits and underscores, and must not open with a digit.
static bool
melee_table_prefix(char *out, size_t cap)
{
  const char *name = kv_get_str(MELEE_KV_PREFIX);
  size_t      len;

  if(out == NULL || cap == 0)
    return(FAIL);

  if(name == NULL || name[0] == '\0')
    name = "melee";

  len = strlen(name);

  if(len >= cap || !validate_alnum(name, cap - 1) ||
     isdigit((unsigned char)name[0]))
  {
    clam(CLAM_WARN, MELEE_CTX,
        "configured table prefix '%s' is not a safe identifier", name);
    return(FAIL);
  }

  memcpy(out, name, len + 1);
  return(SUCCESS);
}

bool
melee_tables_resolve(melee_tables_t *out)
{
  char prefix[MELEE_PREFIX_SZ];

  if(out == NULL)
    return(FAIL);

  if(melee_table_prefix(prefix, sizeof(prefix)) != SUCCESS)
    return(FAIL);

  snprintf(out->rounds,  sizeof(out->rounds),  "%s_rounds",  prefix);
  snprintf(out->players, sizeof(out->players), "%s_players", prefix);
  snprintf(out->scores,  sizeof(out->scores),  "%s_scores",  prefix);

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Schema                                                              //
// ------------------------------------------------------------------ //

// CREATE TABLE IF NOT EXISTS is idempotent, but a mutex keeps concurrent
// bot starts from racing the batch and doubling the log noise.
static bool            melee_schema_done = false;
static pthread_mutex_t melee_schema_lock = PTHREAD_MUTEX_INITIALIZER;

static bool
melee_run_ddl(const char *sql)
{
  db_result_t *res = db_result_alloc();
  bool         ok  = SUCCESS;

  if(res == NULL)
    return(FAIL);

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, MELEE_CTX, "ddl failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");
    ok = FAIL;
  }

  db_result_free(res);
  return(ok);
}

bool
melee_schema_ensure(void)
{
  melee_tables_t t;
  char           sql[1280];
  bool           ok = FAIL;

  pthread_mutex_lock(&melee_schema_lock);

  if(melee_schema_done)
  {
    pthread_mutex_unlock(&melee_schema_lock);
    return(SUCCESS);
  }

  if(melee_tables_resolve(&t) != SUCCESS)
  {
    pthread_mutex_unlock(&melee_schema_lock);
    return(FAIL);
  }

  // A round is one brawl in one room of one namespace. state: 0 active,
  // 1 ended, 2 abandoned. The top_crit trio records the heaviest blow
  // struck, for the round card.
  snprintf(sql, sizeof(sql),
      "CREATE TABLE IF NOT EXISTS %s ("
      " id          BIGSERIAL    PRIMARY KEY,"
      " ns_id       INTEGER      NOT NULL,"
      " method      VARCHAR(64)  NOT NULL DEFAULT '',"
      " channel     VARCHAR(128) NOT NULL DEFAULT '',"
      " state       SMALLINT     NOT NULL DEFAULT 0,"
      " wave        INTEGER      NOT NULL DEFAULT 1,"
      " blows       INTEGER      NOT NULL DEFAULT 0,"
      " opener      VARCHAR(31)  NOT NULL DEFAULT '',"
      " slayer      VARCHAR(31)  NOT NULL DEFAULT '',"
      " fallen      VARCHAR(31)  NOT NULL DEFAULT '',"
      " top_crit    INTEGER      NOT NULL DEFAULT 0,"
      " top_crit_by VARCHAR(31)  NOT NULL DEFAULT '',"
      " top_crit_on VARCHAR(31)  NOT NULL DEFAULT '',"
      " started_at  TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
      " last_action TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
      " ended_at    TIMESTAMPTZ"
      ")", t.rounds);

  if(melee_run_ddl(sql) != SUCCESS)
    goto out;

  // One active round per (namespace, method, channel). This partial
  // unique index is the *only* thing enforcing that invariant; the turn
  // engine leans on it rather than on a read-then-write check.
  snprintf(sql, sizeof(sql),
      "CREATE UNIQUE INDEX IF NOT EXISTS idx_%s_active"
      " ON %s(ns_id, method, channel) WHERE state = 0",
      t.rounds, t.rounds);

  if(melee_run_ddl(sql) != SUCCESS)
    goto out;

  // A combatant's standing within one round. last_wave is the wave in
  // which they last swung, which is what gates a second blow.
  snprintf(sql, sizeof(sql),
      "CREATE TABLE IF NOT EXISTS %s ("
      " round_id  BIGINT      NOT NULL REFERENCES %s(id) ON DELETE CASCADE,"
      " ns_id     INTEGER     NOT NULL,"
      " username  VARCHAR(31) NOT NULL,"
      " nickname  VARCHAR(64) NOT NULL DEFAULT '',"
      " hp        INTEGER     NOT NULL,"
      " hp_max    INTEGER     NOT NULL,"
      " dmg_given INTEGER     NOT NULL DEFAULT 0,"
      " dmg_taken INTEGER     NOT NULL DEFAULT 0,"
      " blows     INTEGER     NOT NULL DEFAULT 0,"
      " crits     INTEGER     NOT NULL DEFAULT 0,"
      " best_crit INTEGER     NOT NULL DEFAULT 0,"
      " last_wave INTEGER     NOT NULL DEFAULT 0,"
      " joined_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      " died_at   TIMESTAMPTZ,"
      " PRIMARY KEY (round_id, username)"
      ")", t.players, t.rounds);

  if(melee_run_ddl(sql) != SUCCESS)
    goto out;

  // Lifetime standings, keyed on the namespace-scoped username so a nick
  // change never splits a combatant's record in two.
  snprintf(sql, sizeof(sql),
      "CREATE TABLE IF NOT EXISTS %s ("
      " ns_id        INTEGER     NOT NULL,"
      " username     VARCHAR(31) NOT NULL,"
      " nickname     VARCHAR(64) NOT NULL DEFAULT '',"
      " rounds       INTEGER     NOT NULL DEFAULT 0,"
      " kills        INTEGER     NOT NULL DEFAULT 0,"
      " deaths       INTEGER     NOT NULL DEFAULT 0,"
      " dmg_given    BIGINT      NOT NULL DEFAULT 0,"
      " dmg_taken    BIGINT      NOT NULL DEFAULT 0,"
      " blows        INTEGER     NOT NULL DEFAULT 0,"
      " crits        INTEGER     NOT NULL DEFAULT 0,"
      " best_crit    INTEGER     NOT NULL DEFAULT 0,"
      " best_crit_on VARCHAR(31) NOT NULL DEFAULT '',"
      " first_seen   TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      " last_seen    TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      " PRIMARY KEY (ns_id, username)"
      ")", t.scores);

  if(melee_run_ddl(sql) != SUCCESS)
    goto out;

  // The leaderboard's one ordering.
  snprintf(sql, sizeof(sql),
      "CREATE INDEX IF NOT EXISTS idx_%s_dmg ON %s(ns_id, dmg_given DESC)",
      t.scores, t.scores);

  if(melee_run_ddl(sql) != SUCCESS)
    goto out;

  melee_schema_done = true;
  ok = SUCCESS;
  clam(CLAM_INFO, MELEE_CTX, "melee schema ready (tables '%s', '%s', '%s')",
      t.rounds, t.players, t.scores);

out:
  pthread_mutex_unlock(&melee_schema_lock);
  return(ok);
}
