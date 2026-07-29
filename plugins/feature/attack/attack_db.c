// botmanager — MIT
// attack persistence: table naming and the schema bootstrap for the pit's
// three tables. The configured prefix is the only identifier ever pasted
// into SQL verbatim, so it is validated as a bare identifier first; every
// user-originated value goes through db_escape at its own call site.

#define ATTACK_INTERNAL
#include "attack.h"

#include "alloc.h"
#include "db.h"
#include "kv.h"
#include "validate.h"

#include <ctype.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------ //
// Table naming                                                        //
// ------------------------------------------------------------------ //

// Guard against injection *and* against an identifier Postgres would
// refuse: the prefix is pasted into DDL and queries verbatim, so it must
// be letters, digits and underscores, and must not open with a digit.
static bool
atk_table_prefix(char *out, size_t cap)
{
  const char *name = kv_get_str(ATK_KV_PREFIX);
  size_t      len;

  if(out == NULL || cap == 0)
    return(FAIL);

  if(name == NULL || name[0] == '\0')
    name = "attack";

  len = strlen(name);

  if(len >= cap || !validate_alnum(name, cap - 1) ||
     isdigit((unsigned char)name[0]))
  {
    clam(CLAM_WARN, ATK_CTX,
        "configured table prefix '%s' is not a safe identifier", name);
    return(FAIL);
  }

  memcpy(out, name, len + 1);
  return(SUCCESS);
}

bool
atk_tables_resolve(atk_tables_t *out)
{
  char prefix[ATK_PREFIX_SZ];

  if(out == NULL)
    return(FAIL);

  if(atk_table_prefix(prefix, sizeof(prefix)) != SUCCESS)
    return(FAIL);

  snprintf(out->rounds,  sizeof(out->rounds),  "%s_rounds",  prefix);
  snprintf(out->players, sizeof(out->players), "%s_players", prefix);
  snprintf(out->scores,  sizeof(out->scores),  "%s_scores",  prefix);
  snprintf(out->dots,    sizeof(out->dots),    "%s_dots",    prefix);

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Schema                                                              //
// ------------------------------------------------------------------ //

// CREATE TABLE IF NOT EXISTS is idempotent, but a mutex keeps concurrent
// bot starts from racing the batch and doubling the log noise.
static bool            atk_schema_done = false;
static pthread_mutex_t atk_schema_lock = PTHREAD_MUTEX_INITIALIZER;

static bool
atk_run_ddl(const char *sql)
{
  db_result_t *res = db_result_alloc();
  bool         ok  = SUCCESS;

  if(res == NULL)
    return(FAIL);

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, ATK_CTX, "ddl failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");
    ok = FAIL;
  }

  db_result_free(res);
  return(ok);
}

bool
atk_schema_ensure(void)
{
  atk_tables_t t;
  char         sql[1280];
  bool         ok = FAIL;

  pthread_mutex_lock(&atk_schema_lock);

  if(atk_schema_done)
  {
    pthread_mutex_unlock(&atk_schema_lock);
    return(SUCCESS);
  }

  if(atk_tables_resolve(&t) != SUCCESS)
  {
    pthread_mutex_unlock(&atk_schema_lock);
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

  if(atk_run_ddl(sql) != SUCCESS)
    goto out;

  // One active round per (namespace, method, channel). This partial
  // unique index is the *only* thing enforcing that invariant; the turn
  // engine leans on it rather than on a read-then-write check.
  snprintf(sql, sizeof(sql),
      "CREATE UNIQUE INDEX IF NOT EXISTS idx_%s_active"
      " ON %s(ns_id, method, channel) WHERE state = 0",
      t.rounds, t.rounds);

  if(atk_run_ddl(sql) != SUCCESS)
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
      " class      VARCHAR(32) NOT NULL DEFAULT '',"
      " heal_given INTEGER     NOT NULL DEFAULT 0,"
      " heal_taken INTEGER     NOT NULL DEFAULT 0,"
      " heals      INTEGER     NOT NULL DEFAULT 0,"
      " defers     SMALLINT    NOT NULL DEFAULT 0,"
      " bonus_pct  INTEGER     NOT NULL DEFAULT 0,"
      " joined_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      " died_at   TIMESTAMPTZ,"
      " PRIMARY KEY (round_id, username)"
      ")", t.players, t.rounds);

  if(atk_run_ddl(sql) != SUCCESS)
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
      " heal_given   BIGINT      NOT NULL DEFAULT 0,"
      " heal_taken   BIGINT      NOT NULL DEFAULT 0,"
      " heals        INTEGER     NOT NULL DEFAULT 0,"
      " first_seen   TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      " last_seen    TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      " PRIMARY KEY (ns_id, username)"
      ")", t.scores);

  if(atk_run_ddl(sql) != SUCCESS)
    goto out;

  // The leaderboard's one ordering.
  snprintf(sql, sizeof(sql),
      "CREATE INDEX IF NOT EXISTS idx_%s_dmg ON %s(ns_id, dmg_given DESC)",
      t.scores, t.scores);

  if(atk_run_ddl(sql) != SUCCESS)
    goto out;

  // An affliction left by a blow, decaying on its own clock. `method`
  // and `channel` are denormalised onto the row on purpose: the decay
  // task holds no command context and no round, and must be able to
  // address a room from a bare row.
  snprintf(sql, sizeof(sql),
      "CREATE TABLE IF NOT EXISTS %s ("
      " id          BIGSERIAL    PRIMARY KEY,"
      " round_id    BIGINT       NOT NULL REFERENCES %s(id) ON DELETE CASCADE,"
      " ns_id       INTEGER      NOT NULL,"
      " method      VARCHAR(64)  NOT NULL DEFAULT '',"
      " channel     VARCHAR(128) NOT NULL DEFAULT '',"
      " victim      VARCHAR(31)  NOT NULL,"
      " victim_nick VARCHAR(64)  NOT NULL DEFAULT '',"
      " source      VARCHAR(31)  NOT NULL,"
      " source_nick VARCHAR(64)  NOT NULL DEFAULT '',"
      " kind        SMALLINT     NOT NULL DEFAULT 0,"
      // The sheet's own word for this affliction, carried on the row so
      // a class's wording survives every tick it ever speaks.
      " noun        VARCHAR(32)  NOT NULL DEFAULT '',"
      " state       SMALLINT     NOT NULL DEFAULT 0,"
      " ticks       INTEGER      NOT NULL DEFAULT 0,"
      " max_ticks   SMALLINT     NOT NULL DEFAULT 3,"
      // The whole rolled damage this affliction will ever deal, fixed at
      // inflict time and spread across max_ticks.
      " dmg_plan    INTEGER      NOT NULL DEFAULT 0,"
      " dmg_total   INTEGER      NOT NULL DEFAULT 0,"
      " next_tick   TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
      " expires_at  TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
      " created_at  TIMESTAMPTZ  NOT NULL DEFAULT NOW()"
      ")", t.dots, t.rounds);

  if(atk_run_ddl(sql) != SUCCESS)
    goto out;

  // The decay task's whole query plan: a range scan over live rows due
  // now. Do not drop it.
  snprintf(sql, sizeof(sql),
      "CREATE INDEX IF NOT EXISTS idx_%s_due ON %s(next_tick) WHERE state = 0",
      t.dots, t.dots);

  if(atk_run_ddl(sql) != SUCCESS)
    goto out;

  // What the stack cap counts, and what the round card asks for.
  snprintf(sql, sizeof(sql),
      "CREATE INDEX IF NOT EXISTS idx_%s_victim"
      " ON %s(round_id, victim) WHERE state = 0",
      t.dots, t.dots);

  if(atk_run_ddl(sql) != SUCCESS)
    goto out;

  atk_schema_done = true;
  ok = SUCCESS;
  clam(CLAM_INFO, ATK_CTX,
      "attack schema ready (tables '%s', '%s', '%s', '%s')",
      t.rounds, t.players, t.scores, t.dots);

out:
  pthread_mutex_unlock(&atk_schema_lock);
  return(ok);
}

// ------------------------------------------------------------------ //
// Small helpers                                                       //
// ------------------------------------------------------------------ //

static int32_t
atk_col_i32(const db_result_t *res, uint32_t row, uint32_t col)
{
  const char *s = db_result_get(res, row, col);

  return((s != NULL) ? (int32_t)strtol(s, NULL, 10) : 0);
}

static int64_t
atk_col_i64(const db_result_t *res, uint32_t row, uint32_t col)
{
  const char *s = db_result_get(res, row, col);

  return((s != NULL) ? (int64_t)strtoll(s, NULL, 10) : 0);
}

static void
atk_col_str(char *dst, size_t cap, const db_result_t *res, uint32_t row,
    uint32_t col)
{
  const char *s = db_result_get(res, row, col);

  snprintf(dst, cap, "%s", (s != NULL) ? s : "");
}

// Run a statement that returns no rows we care about. `what` names the
// operation for the failure log.
static bool
atk_exec(const char *sql, const char *what, uint32_t *affected)
{
  db_result_t *res = db_result_alloc();
  bool         ok  = FAIL;

  if(res == NULL)
    return(FAIL);

  if(db_query(sql, res) == SUCCESS && res->ok)
  {
    if(affected != NULL)
      *affected = res->rows_affected;

    ok = SUCCESS;
  }

  else
    clam(CLAM_WARN, ATK_CTX, "%s failed: %s", what,
        (res->error[0] != '\0') ? res->error : "(no driver error)");

  db_result_free(res);
  return(ok);
}

// ------------------------------------------------------------------ //
// Rounds                                                              //
// ------------------------------------------------------------------ //

bool
atk_db_round_find(uint32_t ns_id, const char *method, const char *channel,
    atk_round_t *out)
{
  atk_tables_t t;
  db_result_t *res     = NULL;
  char        *e_meth  = NULL;
  char        *e_chan  = NULL;
  char         sql[768];
  bool         hit = false;

  if(out == NULL || atk_tables_resolve(&t) != SUCCESS)
    return(false);

  memset(out, 0, sizeof(*out));

  e_meth = db_escape(method  != NULL ? method  : "");
  e_chan = db_escape(channel != NULL ? channel : "");

  if(e_meth == NULL || e_chan == NULL)
    goto out;

  snprintf(sql, sizeof(sql),
      "SELECT id, wave, blows, top_crit,"
      // The brawl's whole age, not its silence: there is one clock now,
      // and it starts when the round does.
      " EXTRACT(EPOCH FROM (NOW() - started_at))::bigint"
      " FROM %s WHERE ns_id = %" PRIu32 " AND method = '%s'"
      " AND channel = '%s' AND state = %d",
      t.rounds, ns_id, e_meth, e_chan, ATK_ROUND_ACTIVE);

  res = db_result_alloc();

  if(res != NULL && db_query(sql, res) == SUCCESS && res->ok && res->rows == 1)
  {
    out->id       = atk_col_i64(res, 0, 0);
    out->wave     = atk_col_i32(res, 0, 1);
    out->blows    = atk_col_i32(res, 0, 2);
    out->top_crit = atk_col_i32(res, 0, 3);
    out->age      = atk_col_i64(res, 0, 4);
    hit = true;
  }

out:
  db_result_free(res);
  if(e_meth != NULL) mem_free(e_meth);
  if(e_chan != NULL) mem_free(e_chan);

  return(hit);
}

bool
atk_db_round_abandon(int64_t round_id)
{
  atk_tables_t t;
  char         sql[256];

  if(round_id <= 0 || atk_tables_resolve(&t) != SUCCESS)
    return(FAIL);

  snprintf(sql, sizeof(sql),
      "UPDATE %s SET state = %d, ended_at = NOW() WHERE id = %" PRId64,
      t.rounds, ATK_ROUND_ABANDONED, round_id);

  return(atk_exec(sql, "round abandon", NULL));
}

int64_t
atk_db_round_open(uint32_t ns_id, const char *method, const char *channel,
    const char *opener)
{
  atk_tables_t t;
  db_result_t *res      = NULL;
  char        *e_meth   = NULL;
  char        *e_chan   = NULL;
  char        *e_opener = NULL;
  char         sql[1024];
  int64_t      id = -1;

  if(atk_tables_resolve(&t) != SUCCESS)
    return(-1);

  e_meth   = db_escape(method  != NULL ? method  : "");
  e_chan   = db_escape(channel != NULL ? channel : "");
  e_opener = db_escape(opener  != NULL ? opener  : "");

  if(e_meth == NULL || e_chan == NULL || e_opener == NULL)
    goto out;

  snprintf(sql, sizeof(sql),
      "INSERT INTO %s (ns_id, method, channel, opener)"
      " VALUES (%" PRIu32 ", '%s', '%s', '%s') RETURNING id",
      t.rounds, ns_id, e_meth, e_chan, e_opener);

  res = db_result_alloc();

  if(res != NULL && db_query(sql, res) == SUCCESS && res->ok && res->rows == 1)
    id = atk_col_i64(res, 0, 0);

  else
    clam(CLAM_WARN, ATK_CTX, "round open failed: %s",
        (res != NULL && res->error[0] != '\0')
            ? res->error : "(no driver error)");

out:
  db_result_free(res);
  if(e_meth   != NULL) mem_free(e_meth);
  if(e_chan   != NULL) mem_free(e_chan);
  if(e_opener != NULL) mem_free(e_opener);

  return(id);
}

// ------------------------------------------------------------------ //
// Players                                                             //
// ------------------------------------------------------------------ //

// The CTE is what keeps the lifetime `rounds` tally honest: the score
// row is bumped only for the enrolment that actually inserted, so a
// combatant struck ten times in one brawl still counts one round.
bool
atk_db_player_enrol(int64_t round_id, uint32_t ns_id, const char *username,
    const char *nickname, int32_t hp)
{
  atk_tables_t t;
  char        *e_user = NULL;
  char        *e_nick = NULL;
  char         sql[1536];
  uint32_t     affected = 0;
  bool         fresh    = false;

  if(round_id <= 0 || username == NULL || atk_tables_resolve(&t) != SUCCESS)
    return(false);

  e_user = db_escape(username);
  e_nick = db_escape(nickname != NULL ? nickname : "");

  if(e_user == NULL || e_nick == NULL)
    goto out;

  snprintf(sql, sizeof(sql),
      "WITH enrolled AS ("
      " INSERT INTO %s (round_id, ns_id, username, nickname, hp, hp_max)"
      " VALUES (%" PRId64 ", %" PRIu32 ", '%s', '%s', %d, %d)"
      " ON CONFLICT (round_id, username) DO NOTHING"
      " RETURNING 1"
      ")"
      " INSERT INTO %s (ns_id, username, nickname, rounds)"
      " SELECT %" PRIu32 ", '%s', '%s', 1 FROM enrolled"
      " ON CONFLICT (ns_id, username) DO UPDATE SET"
      " rounds = %s.rounds + 1, nickname = EXCLUDED.nickname,"
      " last_seen = NOW()",
      t.players, round_id, ns_id, e_user, e_nick, hp, hp,
      t.scores, ns_id, e_user, e_nick, t.scores);

  if(atk_exec(sql, "player enrol", &affected) == SUCCESS)
    fresh = (affected > 0);

out:
  if(e_user != NULL) mem_free(e_user);
  if(e_nick != NULL) mem_free(e_nick);

  return(fresh);
}

bool
atk_db_player_get(int64_t round_id, const char *username,
    atk_player_t *out)
{
  atk_tables_t t;
  db_result_t *res    = NULL;
  char        *e_user = NULL;
  char         sql[512];
  bool         hit = false;

  if(out == NULL || username == NULL || atk_tables_resolve(&t) != SUCCESS)
    return(false);

  memset(out, 0, sizeof(*out));

  e_user = db_escape(username);

  if(e_user == NULL)
    return(false);

  snprintf(sql, sizeof(sql),
      "SELECT nickname, hp, hp_max, last_wave FROM %s"
      " WHERE round_id = %" PRId64 " AND username = '%s'",
      t.players, round_id, e_user);

  res = db_result_alloc();

  if(res != NULL && db_query(sql, res) == SUCCESS && res->ok && res->rows == 1)
  {
    atk_col_str(out->nickname, sizeof(out->nickname), res, 0, 0);
    out->hp        = atk_col_i32(res, 0, 1);
    out->hp_max    = atk_col_i32(res, 0, 2);
    out->last_wave = atk_col_i32(res, 0, 3);
    hit = true;
  }

  db_result_free(res);
  mem_free(e_user);

  return(hit);
}

bool
atk_db_pending(int64_t round_id, int32_t wave, char *out, size_t cap)
{
  atk_tables_t t;
  db_result_t *res = NULL;
  char         sql[512];
  bool         ok = FAIL;

  if(out == NULL || cap == 0)
    return(FAIL);

  out[0] = '\0';

  if(atk_tables_resolve(&t) != SUCCESS)
    return(FAIL);

  // Display name falls back to the username for a combatant enrolled by
  // username rather than by an observed nick.
  snprintf(sql, sizeof(sql),
      "SELECT COALESCE(string_agg("
      " CASE WHEN nickname <> '' THEN nickname ELSE username END,"
      " ', ' ORDER BY username), '')"
      " FROM %s WHERE round_id = %" PRId64 " AND hp > 0 AND last_wave < %d",
      t.players, round_id, wave);

  res = db_result_alloc();

  if(res != NULL && db_query(sql, res) == SUCCESS && res->ok && res->rows == 1)
  {
    atk_col_str(out, cap, res, 0, 0);
    ok = SUCCESS;
  }

  db_result_free(res);
  return(ok);
}

// ------------------------------------------------------------------ //
// Reads for the views                                                 //
// ------------------------------------------------------------------ //

// `(state = 0) DESC` puts the live round first (Postgres orders false
// before true), and last_action breaks the tie among finished ones — so
// one query answers both "what is happening" and "what just happened".
bool
atk_db_card_find(uint32_t ns_id, const char *method, const char *channel,
    atk_card_t *out)
{
  atk_tables_t t;
  db_result_t *res    = NULL;
  char        *e_meth = NULL;
  char        *e_chan = NULL;
  char         room[512] = "";
  char         sql[1024];
  bool         hit = false;

  if(out == NULL || atk_tables_resolve(&t) != SUCCESS)
    return(false);

  memset(out, 0, sizeof(*out));

  // A direct message names no room, so the search widens to every room
  // in the namespace rather than matching the empty string.
  if(channel != NULL && channel[0] != '\0')
  {
    e_meth = db_escape(method != NULL ? method : "");
    e_chan = db_escape(channel);

    if(e_meth == NULL || e_chan == NULL)
      goto out;

    snprintf(room, sizeof(room), " AND method = '%s' AND channel = '%s'",
        e_meth, e_chan);
  }

  snprintf(sql, sizeof(sql),
      "SELECT id, channel, state, wave, blows,"
      " EXTRACT(EPOCH FROM (COALESCE(ended_at, NOW()) - started_at))::bigint,"
      " top_crit, top_crit_by, top_crit_on, slayer, fallen"
      " FROM %s WHERE ns_id = %" PRIu32 "%s"
      " ORDER BY (state = %d) DESC, last_action DESC LIMIT 1",
      t.rounds, ns_id, room, ATK_ROUND_ACTIVE);

  res = db_result_alloc();

  if(res != NULL && db_query(sql, res) == SUCCESS && res->ok && res->rows == 1)
  {
    out->id     = atk_col_i64(res, 0, 0);
    atk_col_str(out->channel, sizeof(out->channel), res, 0, 1);
    out->state  = atk_col_i32(res, 0, 2);
    out->wave   = atk_col_i32(res, 0, 3);
    out->blows  = atk_col_i32(res, 0, 4);
    out->length = atk_col_i64(res, 0, 5);
    out->top_crit = atk_col_i32(res, 0, 6);
    atk_col_str(out->top_by,  sizeof(out->top_by),  res, 0, 7);
    atk_col_str(out->top_on,  sizeof(out->top_on),  res, 0, 8);
    atk_col_str(out->slayer,  sizeof(out->slayer),  res, 0, 9);
    atk_col_str(out->fallen,  sizeof(out->fallen),  res, 0, 10);
    hit = true;
  }

out:
  db_result_free(res);
  if(e_meth != NULL) mem_free(e_meth);
  if(e_chan != NULL) mem_free(e_chan);

  return(hit);
}

// COUNT(*) OVER () rides along on every row, so the roster and its true
// size arrive together and the card can be honest about what it cut.
uint32_t
atk_db_card_roster(int64_t round_id, atk_card_row_t *out, uint32_t cap,
    uint32_t *total)
{
  atk_tables_t t;
  db_result_t *res = NULL;
  char         sql[768];
  uint32_t     n = 0;

  if(total != NULL)
    *total = 0;

  if(out == NULL || cap == 0 || round_id <= 0 ||
     atk_tables_resolve(&t) != SUCCESS)
    return(0);

  // The table is aliased and the sort keys qualified because `username`
  // is now both an output column and a table column: unqualified, that
  // ORDER BY is ambiguous and Postgres refuses the whole query.
  snprintf(sql, sizeof(sql),
      "SELECT CASE WHEN p.nickname <> '' THEN p.nickname ELSE p.username END,"
      " p.hp, p.hp_max, p.dmg_given, p.dmg_taken, p.best_crit, p.last_wave,"
      " COUNT(*) OVER (), p.username"
      " FROM %s p WHERE p.round_id = %" PRId64
      " ORDER BY p.hp DESC, p.dmg_given DESC, p.username LIMIT %" PRIu32,
      t.players, round_id, cap);

  res = db_result_alloc();

  if(res != NULL && db_query(sql, res) == SUCCESS && res->ok)
  {
    for(n = 0; n < res->rows && n < cap; n++)
    {
      atk_col_str(out[n].name, sizeof(out[n].name), res, n, 0);
      atk_col_str(out[n].user, sizeof(out[n].user), res, n, 8);
      out[n].hp        = atk_col_i32(res, n, 1);
      out[n].hp_max    = atk_col_i32(res, n, 2);
      out[n].dmg_given = atk_col_i32(res, n, 3);
      out[n].dmg_taken = atk_col_i32(res, n, 4);
      out[n].best_crit = atk_col_i32(res, n, 5);
      out[n].last_wave = atk_col_i32(res, n, 6);

      if(total != NULL)
        *total = (uint32_t)atk_col_i64(res, n, 7);
    }
  }

  // An empty card is indistinguishable from a broken one on screen, so
  // the failure has to say so somewhere.
  else
    clam(CLAM_WARN, ATK_CTX, "round card roster failed: %s",
        (res != NULL && res->error[0] != '\0') ? res->error
                                               : "(no driver error)");

  db_result_free(res);
  return(n);
}

uint32_t
atk_db_scores(uint32_t ns_id, uint32_t limit, atk_score_row_t *out,
    uint32_t cap)
{
  atk_tables_t t;
  db_result_t *res = NULL;
  char         sql[640];
  uint32_t     n = 0;

  if(out == NULL || cap == 0 || atk_tables_resolve(&t) != SUCCESS)
    return(0);

  if(limit > cap)
    limit = cap;

  if(limit == 0)
    return(0);

  snprintf(sql, sizeof(sql),
      "SELECT CASE WHEN nickname <> '' THEN nickname ELSE username END,"
      " rounds, kills, deaths, dmg_given, dmg_taken, crits, best_crit"
      " FROM %s WHERE ns_id = %" PRIu32
      " ORDER BY dmg_given DESC, username LIMIT %" PRIu32,
      t.scores, ns_id, limit);

  res = db_result_alloc();

  if(res != NULL && db_query(sql, res) == SUCCESS && res->ok)
  {
    for(n = 0; n < res->rows && n < limit; n++)
    {
      atk_col_str(out[n].name, sizeof(out[n].name), res, n, 0);
      out[n].rounds    = atk_col_i32(res, n, 1);
      out[n].kills     = atk_col_i32(res, n, 2);
      out[n].deaths    = atk_col_i32(res, n, 3);
      out[n].dmg_given = atk_col_i64(res, n, 4);
      out[n].dmg_taken = atk_col_i64(res, n, 5);
      out[n].crits     = atk_col_i32(res, n, 6);
      out[n].best_crit = atk_col_i32(res, n, 7);
    }
  }

  db_result_free(res);
  return(n);
}

bool
atk_db_deadliest(uint32_t ns_id, char *by, size_t by_cap, char *on,
    size_t on_cap, int32_t *dmg)
{
  atk_tables_t t;
  db_result_t *res = NULL;
  char         sql[512];
  bool         hit = false;

  if(by == NULL || on == NULL || dmg == NULL || by_cap == 0 || on_cap == 0)
    return(false);

  by[0] = '\0';
  on[0] = '\0';
  *dmg  = 0;

  if(atk_tables_resolve(&t) != SUCCESS)
    return(false);

  snprintf(sql, sizeof(sql),
      "SELECT CASE WHEN nickname <> '' THEN nickname ELSE username END,"
      " best_crit_on, best_crit FROM %s"
      " WHERE ns_id = %" PRIu32 " AND best_crit > 0"
      " ORDER BY best_crit DESC LIMIT 1",
      t.scores, ns_id);

  res = db_result_alloc();

  if(res != NULL && db_query(sql, res) == SUCCESS && res->ok && res->rows == 1)
  {
    atk_col_str(by, by_cap, res, 0, 0);
    atk_col_str(on, on_cap, res, 0, 1);
    *dmg = atk_col_i32(res, 0, 2);
    hit  = true;
  }

  db_result_free(res);
  return(hit);
}

// ------------------------------------------------------------------ //
// The blow                                                            //
// ------------------------------------------------------------------ //

bool
atk_db_blow_apply(const atk_blow_t *b)
{
  atk_tables_t t;
  char        *e_src_u = NULL;
  char        *e_src_n = NULL;
  char        *e_tgt_u = NULL;
  char        *e_tgt_n = NULL;
  char        *sql     = NULL;
  char         top  [320]  = "";
  char         death[2048] = "";
  size_t       need;
  bool         ok = FAIL;

  if(b == NULL || atk_tables_resolve(&t) != SUCCESS)
    return(FAIL);

  e_src_u = db_escape(b->src_user);
  e_src_n = db_escape(b->src_nick);
  e_tgt_u = db_escape(b->tgt_user);
  e_tgt_n = db_escape(b->tgt_nick);

  if(e_src_u == NULL || e_src_n == NULL || e_tgt_u == NULL || e_tgt_n == NULL)
    goto out;

  if(b->new_top)
    snprintf(top, sizeof(top),
        ", top_crit = %d, top_crit_by = '%s', top_crit_on = '%s'",
        b->dmg, e_src_u, e_tgt_u);

  // A fatal blow ends the brawl in the same transaction that lands it,
  // so the round can never be left open with a corpse still in it.
  if(b->fatal)
    snprintf(death, sizeof(death),
        "UPDATE %s SET state = %d, ended_at = NOW(), slayer = '%s',"
        " fallen = '%s' WHERE id = %" PRId64 ";"
        "UPDATE %s SET kills = kills + 1, last_seen = NOW()"
        " WHERE ns_id = %" PRIu32 " AND username = '%s';"
        "UPDATE %s SET deaths = deaths + 1, last_seen = NOW()"
        " WHERE ns_id = %" PRIu32 " AND username = '%s';",
        t.rounds, ATK_ROUND_ENDED, e_src_u, e_tgt_u, b->round_id,
        t.scores, b->ns_id, e_src_u,
        t.scores, b->ns_id, e_tgt_u);

  need = 4096 + strlen(top) + strlen(death)
      + 8 * (strlen(e_src_u) + strlen(e_src_n) + strlen(e_tgt_u)
             + strlen(e_tgt_n) + strlen(t.rounds) + strlen(t.players)
             + strlen(t.scores));

  sql = mem_alloc(ATK_CTX, "blow_sql", need);

  if(sql == NULL)
    goto out;

  snprintf(sql, need,
      "BEGIN;"

      // The attacker spends the wave, whatever the blow was worth.
      "UPDATE %s SET nickname = '%s', dmg_given = dmg_given + %d,"
      " blows = blows + 1, crits = crits + %d,"
      " best_crit = GREATEST(best_crit, %d), last_wave = %d"
      " WHERE round_id = %" PRId64 " AND username = '%s';"

      // The target takes it. hp floors at zero; died_at is stamped once.
      "UPDATE %s SET nickname = '%s', hp = GREATEST(hp - %d, 0),"
      " dmg_taken = dmg_taken + %d,"
      " died_at = CASE WHEN hp - %d <= 0 AND died_at IS NULL"
      " THEN NOW() ELSE died_at END"
      " WHERE round_id = %" PRId64 " AND username = '%s';"

      "UPDATE %s SET blows = blows + 1, last_action = NOW()%s"
      " WHERE id = %" PRId64 ";"

      // Lifetime mirror for the attacker. best_crit_on follows best_crit
      // so the two never describe different blows.
      "INSERT INTO %s (ns_id, username, nickname, dmg_given, blows,"
      " crits, best_crit, best_crit_on)"
      " VALUES (%" PRIu32 ", '%s', '%s', %d, 1, %d, %d, '%s')"
      " ON CONFLICT (ns_id, username) DO UPDATE SET"
      " nickname = EXCLUDED.nickname,"
      " dmg_given = %s.dmg_given + EXCLUDED.dmg_given,"
      " blows = %s.blows + EXCLUDED.blows,"
      " crits = %s.crits + EXCLUDED.crits,"
      " best_crit = GREATEST(%s.best_crit, EXCLUDED.best_crit),"
      " best_crit_on = CASE WHEN EXCLUDED.best_crit > %s.best_crit"
      " THEN EXCLUDED.best_crit_on ELSE %s.best_crit_on END,"
      " last_seen = NOW();"

      // Lifetime mirror for the target.
      "INSERT INTO %s (ns_id, username, nickname, dmg_taken)"
      " VALUES (%" PRIu32 ", '%s', '%s', %d)"
      " ON CONFLICT (ns_id, username) DO UPDATE SET"
      " nickname = EXCLUDED.nickname,"
      " dmg_taken = %s.dmg_taken + EXCLUDED.dmg_taken,"
      " last_seen = NOW();"

      // The wave turns only once every living combatant has swung in it.
      "UPDATE %s r SET wave = r.wave + 1 WHERE r.id = %" PRId64
      " AND NOT EXISTS (SELECT 1 FROM %s p WHERE p.round_id = r.id"
      " AND p.hp > 0 AND p.last_wave < r.wave);"

      "%s"
      "COMMIT;",

      t.players, e_src_n, b->dmg, b->crit ? 1 : 0,
      b->crit ? b->dmg : 0, b->wave, b->round_id, e_src_u,

      t.players, e_tgt_n, b->dmg, b->dmg, b->dmg, b->round_id, e_tgt_u,

      t.rounds, top, b->round_id,

      t.scores, b->ns_id, e_src_u, e_src_n, b->dmg, b->crit ? 1 : 0,
      b->crit ? b->dmg : 0, b->crit ? e_tgt_u : "",
      t.scores, t.scores, t.scores, t.scores, t.scores, t.scores,

      t.scores, b->ns_id, e_tgt_u, e_tgt_n, b->dmg,
      t.scores,

      t.rounds, b->round_id, t.players,

      death);

  ok = atk_exec(sql, "blow apply", NULL);

out:
  if(e_src_u != NULL) mem_free(e_src_u);
  if(e_src_n != NULL) mem_free(e_src_n);
  if(e_tgt_u != NULL) mem_free(e_tgt_u);
  if(e_tgt_n != NULL) mem_free(e_tgt_n);
  if(sql     != NULL) mem_free(sql);

  return(ok);
}

// ------------------------------------------------------------------ //
// Afflictions                                                         //
// ------------------------------------------------------------------ //

// One statement, one round trip, and the stack cap enforced by the WHERE
// clause rather than by a read-then-write another turn could race. At
// the cap the INSERT ... SELECT simply affects no rows, which is why the
// caller is told SUCCESS only when the row count says one landed.
bool
atk_db_dot_inflict(const atk_dot_new_t *d)
{
  atk_tables_t t;
  char        *e_meth   = NULL;
  char        *e_chan   = NULL;
  char        *e_vic_u  = NULL;
  char        *e_vic_n  = NULL;
  char        *e_src_u  = NULL;
  char        *e_src_n  = NULL;
  char        *e_noun   = NULL;
  char         sql[1792];
  uint32_t     affected = 0;
  bool         ok = FAIL;

  if(d == NULL || d->round_id <= 0 || atk_tables_resolve(&t) != SUCCESS)
    return(FAIL);

  e_meth  = db_escape(d->method      != NULL ? d->method      : "");
  e_chan  = db_escape(d->channel     != NULL ? d->channel     : "");
  e_vic_u = db_escape(d->victim      != NULL ? d->victim      : "");
  e_vic_n = db_escape(d->victim_nick != NULL ? d->victim_nick : "");
  e_src_u = db_escape(d->source      != NULL ? d->source      : "");
  e_src_n = db_escape(d->source_nick != NULL ? d->source_nick : "");
  e_noun  = db_escape(d->noun        != NULL ? d->noun        : "");

  if(e_meth == NULL || e_chan == NULL || e_vic_u == NULL ||
     e_vic_n == NULL || e_src_u == NULL || e_src_n == NULL ||
     e_noun == NULL)
    goto out;

  // expires_at is a BACKSTOP, not the thing that ends the affliction: a
  // row the decay task never serviced is swept one cadence after its
  // last scheduled tick. What actually retires it is the tick count.
  snprintf(sql, sizeof(sql),
      "INSERT INTO %s (round_id, ns_id, method, channel, victim,"
      " victim_nick, source, source_nick, kind, noun, max_ticks,"
      " dmg_plan, next_tick, expires_at)"
      " SELECT %" PRId64 ", %" PRIu32 ", '%s', '%s', '%s', '%s', '%s',"
      " '%s', %d, '%s', %" PRIu32 ", %d,"
      " NOW() + INTERVAL '%" PRIu32 " seconds',"
      " NOW() + INTERVAL '%" PRIu32 " seconds'"
      " WHERE (SELECT COUNT(*) FROM %s WHERE round_id = %" PRId64
      " AND victim = '%s' AND state = %d) < %" PRIu32,
      t.dots, d->round_id, d->ns_id, e_meth, e_chan, e_vic_u, e_vic_n,
      e_src_u, e_src_n, (int)d->kind, e_noun, d->max_ticks, d->dmg_plan,
      d->tick_secs, (d->max_ticks + 1) * d->tick_secs,
      t.dots, d->round_id, e_vic_u, ATK_DOT_LIVE, d->stack_max);

  if(atk_exec(sql, "dot inflict", &affected) == SUCCESS && affected > 0)
    ok = SUCCESS;

out:
  if(e_meth  != NULL) mem_free(e_meth);
  if(e_chan  != NULL) mem_free(e_chan);
  if(e_vic_u != NULL) mem_free(e_vic_u);
  if(e_vic_n != NULL) mem_free(e_vic_n);
  if(e_src_u != NULL) mem_free(e_src_u);
  if(e_src_n != NULL) mem_free(e_src_n);
  if(e_noun  != NULL) mem_free(e_noun);

  return(ok);
}

// Rows arrive ordered by victim, so the grouping is one pass with no
// lookup: a new username opens a new mark, and anything past the stack
// cap on one victim is dropped rather than overrunning the array.
uint32_t
atk_db_dot_marks(int64_t round_id, atk_dot_mark_t *out, uint32_t cap)
{
  atk_tables_t t;
  db_result_t *res = NULL;
  char         sql[512];
  char         victim[ATK_USER_SZ];
  uint32_t     row;
  uint32_t     n = 0;

  if(out == NULL || cap == 0 || round_id <= 0 ||
     atk_tables_resolve(&t) != SUCCESS)
    return(0);

  snprintf(sql, sizeof(sql),
      "SELECT victim, kind FROM %s WHERE round_id = %" PRId64
      " AND state = %d ORDER BY victim, id",
      t.dots, round_id, ATK_DOT_LIVE);

  res = db_result_alloc();

  if(res == NULL || db_query(sql, res) != SUCCESS || !res->ok)
    goto out;

  for(row = 0; row < res->rows; row++)
  {
    atk_dot_mark_t *mark;

    atk_col_str(victim, sizeof(victim), res, row, 0);

    if(n == 0 || strcmp(out[n - 1].victim, victim) != 0)
    {
      if(n == cap)
        break;

      mark = &out[n++];
      memset(mark, 0, sizeof(*mark));
      snprintf(mark->victim, sizeof(mark->victim), "%s", victim);
    }

    else
      mark = &out[n - 1];

    if(mark->n < ATK_DOT_STACK_CAP)
      mark->kinds[mark->n++] = (atk_dot_kind_t)atk_col_i32(res, row, 1);
  }

out:
  db_result_free(res);
  return(n);
}

// The join against the rounds table is what implements "an affliction
// dies with its round": a round that ended or was abandoned stops its
// afflictions from ever ticking again, without a second lookup here.
uint32_t
atk_db_dot_due(atk_dot_due_t *out, uint32_t cap)
{
  atk_tables_t t;
  db_result_t *res = NULL;
  char         sql[768];
  uint32_t     n = 0;

  if(out == NULL || cap == 0 || atk_tables_resolve(&t) != SUCCESS)
    return(0);

  snprintf(sql, sizeof(sql),
      "SELECT d.id, d.round_id, d.ns_id, d.method, d.channel, d.victim,"
      " d.victim_nick, d.source, d.source_nick, d.kind,"
      // An affliction ends by tick COUNT, not by the wall clock. This is
      // what makes "at most max_ticks messages" exact rather than
      // approximate, and it is why a 1-damage DOT speaks once.
      " (d.ticks + 1 >= d.max_ticks),"
      " d.noun, d.max_ticks, d.dmg_plan, d.dmg_total, d.ticks"
      " FROM %s d JOIN %s r ON r.id = d.round_id"
      " WHERE d.state = %d AND r.state = %d AND d.next_tick <= NOW()"
      " ORDER BY d.next_tick LIMIT %" PRIu32,
      t.dots, t.rounds, ATK_DOT_LIVE, ATK_ROUND_ACTIVE, cap);

  res = db_result_alloc();

  if(res != NULL && db_query(sql, res) == SUCCESS && res->ok)
  {
    for(n = 0; n < res->rows && n < cap; n++)
    {
      const char *expired;

      out[n].id       = atk_col_i64(res, n, 0);
      out[n].round_id = atk_col_i64(res, n, 1);
      out[n].ns_id    = (uint32_t)atk_col_i32(res, n, 2);

      atk_col_str(out[n].method,      sizeof(out[n].method),      res, n, 3);
      atk_col_str(out[n].channel,     sizeof(out[n].channel),     res, n, 4);
      atk_col_str(out[n].victim,      sizeof(out[n].victim),      res, n, 5);
      atk_col_str(out[n].victim_nick, sizeof(out[n].victim_nick), res, n, 6);
      atk_col_str(out[n].source,      sizeof(out[n].source),      res, n, 7);
      atk_col_str(out[n].source_nick, sizeof(out[n].source_nick), res, n, 8);

      out[n].kind = (atk_dot_kind_t)atk_col_i32(res, n, 9);

      // Postgres renders a boolean as 't' or 'f'.
      expired = db_result_get(res, n, 10);
      out[n].expired = (expired != NULL && expired[0] == 't');

      atk_col_str(out[n].noun, sizeof(out[n].noun), res, n, 11);

      out[n].max_ticks = (uint32_t)atk_col_i32(res, n, 12);
      out[n].dmg_plan  = atk_col_i32(res, n, 13);
      out[n].dmg_done  = atk_col_i32(res, n, 14);
      out[n].ticks     = (uint32_t)atk_col_i32(res, n, 15);
    }
  }

  db_result_free(res);
  return(n);
}

uint32_t
atk_db_dot_live(void)
{
  atk_tables_t t;
  db_result_t *res = NULL;
  char         sql[512];
  uint32_t     n = 0;

  if(atk_tables_resolve(&t) != SUCCESS)
    return(0);

  snprintf(sql, sizeof(sql),
      "SELECT COUNT(*) FROM %s d JOIN %s r ON r.id = d.round_id"
      " WHERE d.state = %d AND r.state = %d",
      t.dots, t.rounds, ATK_DOT_LIVE, ATK_ROUND_ACTIVE);

  res = db_result_alloc();

  if(res != NULL && db_query(sql, res) == SUCCESS && res->ok && res->rows == 1)
    n = (uint32_t)atk_col_i64(res, 0, 0);

  db_result_free(res);
  return(n);
}

bool
atk_db_dot_sweep(void)
{
  atk_tables_t t;
  char         sql[512];

  if(atk_tables_resolve(&t) != SUCCESS)
    return(FAIL);

  snprintf(sql, sizeof(sql),
      "UPDATE %s SET state = %d WHERE state = %d AND round_id IN"
      " (SELECT id FROM %s WHERE state <> %d)",
      t.dots, ATK_DOT_CANCELLED, ATK_DOT_LIVE, t.rounds,
      ATK_ROUND_ACTIVE);

  return(atk_exec(sql, "dot sweep", NULL));
}

bool
atk_db_dot_cancel(int64_t dot_id)
{
  atk_tables_t t;
  char         sql[256];

  if(dot_id <= 0 || atk_tables_resolve(&t) != SUCCESS)
    return(FAIL);

  snprintf(sql, sizeof(sql), "UPDATE %s SET state = %d WHERE id = %" PRId64,
      t.dots, ATK_DOT_CANCELLED, dot_id);

  return(atk_exec(sql, "dot cancel", NULL));
}

bool
atk_db_dot_tick(const atk_dot_hit_t *h)
{
  atk_tables_t t;
  char        *e_vic = NULL;
  char        *e_src = NULL;
  char        *sql   = NULL;
  char         death[2048] = "";
  size_t       need;
  bool         ok = FAIL;

  if(h == NULL || h->round_id <= 0 || atk_tables_resolve(&t) != SUCCESS)
    return(FAIL);

  e_vic = db_escape(h->victim != NULL ? h->victim : "");
  e_src = db_escape(h->source != NULL ? h->source : "");

  if(e_vic == NULL || e_src == NULL)
    goto out;

  // Only the tick that reaches zero closes anything — and that tick is
  // by construction the affliction's last, marked spent below.
  if(h->fatal)
    snprintf(death, sizeof(death),
        "UPDATE %s SET state = %d, ended_at = NOW(), slayer = '%s',"
        " fallen = '%s' WHERE id = %" PRId64 ";"
        "UPDATE %s SET kills = kills + 1, last_seen = NOW()"
        " WHERE ns_id = %" PRIu32 " AND username = '%s';"
        "UPDATE %s SET deaths = deaths + 1, last_seen = NOW()"
        " WHERE ns_id = %" PRIu32 " AND username = '%s';",
        t.rounds, ATK_ROUND_ENDED, e_src, e_vic, h->round_id,
        t.scores, h->ns_id, e_src,
        t.scores, h->ns_id, e_vic);

  need = 2048 + strlen(death)
      + 6 * (strlen(e_vic) + strlen(e_src) + strlen(t.rounds)
             + strlen(t.players) + strlen(t.scores) + strlen(t.dots));

  sql = mem_alloc(ATK_CTX, "dot_tick_sql", need);

  if(sql == NULL)
    goto out;

  snprintf(sql, need,
      "BEGIN;"

      // The victim bleeds. hp floors at zero; died_at is stamped once.
      // Nothing here touches last_wave: decay is not a swing.
      "UPDATE %s SET hp = GREATEST(hp - %d, 0), dmg_taken = dmg_taken + %d,"
      " died_at = CASE WHEN hp - %d <= 0 AND died_at IS NULL"
      " THEN NOW() ELSE died_at END"
      " WHERE round_id = %" PRId64 " AND username = '%s';"

      // Whoever left the wound is still credited for what it does.
      "UPDATE %s SET dmg_given = dmg_given + %d"
      " WHERE round_id = %" PRId64 " AND username = '%s';"

      // Both lifetime mirrors. Enrolment already created these rows.
      "UPDATE %s SET dmg_taken = dmg_taken + %d, last_seen = NOW()"
      " WHERE ns_id = %" PRIu32 " AND username = '%s';"
      "UPDATE %s SET dmg_given = dmg_given + %d, last_seen = NOW()"
      " WHERE ns_id = %" PRIu32 " AND username = '%s';"

      // The affliction's own ledger and its next deadline. The deadline
      // advances from the one it just met, not from now: the task wakes
      // on its own cadence and can only ever service a tick LATE, so
      // NOW() + interval would compound that lateness into a drift of
      // roughly double the configured gap. GREATEST() keeps a backlog
      // from firing a burst of catch-up ticks in consecutive seconds.
      "UPDATE %s SET ticks = ticks + 1, dmg_total = dmg_total + %d,"
      " next_tick = GREATEST(next_tick, NOW() - INTERVAL '%" PRIu32
      " seconds') + INTERVAL '%" PRIu32 " seconds', state = %d"
      " WHERE id = %" PRId64 ";"

      "%s"
      "COMMIT;",

      t.players, h->dmg, h->dmg, h->dmg, h->round_id, e_vic,
      t.players, h->dmg, h->round_id, e_src,
      t.scores,  h->dmg, h->ns_id, e_vic,
      t.scores,  h->dmg, h->ns_id, e_src,
      t.dots,    h->dmg, h->tick_secs, h->tick_secs,
      (h->last || h->fatal) ? ATK_DOT_SPENT : ATK_DOT_LIVE, h->dot_id,
      death);

  ok = atk_exec(sql, "dot tick", NULL);

out:
  if(e_vic != NULL) mem_free(e_vic);
  if(e_src != NULL) mem_free(e_src);
  if(sql   != NULL) mem_free(sql);

  return(ok);
}
