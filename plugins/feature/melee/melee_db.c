// botmanager — MIT
// melee persistence: table naming and the schema bootstrap for the pit's
// three tables. The configured prefix is the only identifier ever pasted
// into SQL verbatim, so it is validated as a bare identifier first; every
// user-originated value goes through db_escape at its own call site.

#define MELEE_INTERNAL
#include "melee.h"

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

// ------------------------------------------------------------------ //
// Small helpers                                                       //
// ------------------------------------------------------------------ //

static int32_t
melee_col_i32(const db_result_t *res, uint32_t row, uint32_t col)
{
  const char *s = db_result_get(res, row, col);

  return((s != NULL) ? (int32_t)strtol(s, NULL, 10) : 0);
}

static int64_t
melee_col_i64(const db_result_t *res, uint32_t row, uint32_t col)
{
  const char *s = db_result_get(res, row, col);

  return((s != NULL) ? (int64_t)strtoll(s, NULL, 10) : 0);
}

static void
melee_col_str(char *dst, size_t cap, const db_result_t *res, uint32_t row,
    uint32_t col)
{
  const char *s = db_result_get(res, row, col);

  snprintf(dst, cap, "%s", (s != NULL) ? s : "");
}

// Run a statement that returns no rows we care about. `what` names the
// operation for the failure log.
static bool
melee_exec(const char *sql, const char *what, uint32_t *affected)
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
    clam(CLAM_WARN, MELEE_CTX, "%s failed: %s", what,
        (res->error[0] != '\0') ? res->error : "(no driver error)");

  db_result_free(res);
  return(ok);
}

// ------------------------------------------------------------------ //
// Rounds                                                              //
// ------------------------------------------------------------------ //

bool
melee_db_round_find(uint32_t ns_id, const char *method, const char *channel,
    melee_round_t *out)
{
  melee_tables_t t;
  db_result_t   *res     = NULL;
  char          *e_meth  = NULL;
  char          *e_chan  = NULL;
  char           sql[768];
  bool           hit = false;

  if(out == NULL || melee_tables_resolve(&t) != SUCCESS)
    return(false);

  memset(out, 0, sizeof(*out));

  e_meth = db_escape(method  != NULL ? method  : "");
  e_chan = db_escape(channel != NULL ? channel : "");

  if(e_meth == NULL || e_chan == NULL)
    goto out;

  snprintf(sql, sizeof(sql),
      "SELECT id, wave, blows, top_crit,"
      " EXTRACT(EPOCH FROM (NOW() - last_action))::bigint"
      " FROM %s WHERE ns_id = %" PRIu32 " AND method = '%s'"
      " AND channel = '%s' AND state = %d",
      t.rounds, ns_id, e_meth, e_chan, MELEE_ROUND_ACTIVE);

  res = db_result_alloc();

  if(res != NULL && db_query(sql, res) == SUCCESS && res->ok && res->rows == 1)
  {
    out->id       = melee_col_i64(res, 0, 0);
    out->wave     = melee_col_i32(res, 0, 1);
    out->blows    = melee_col_i32(res, 0, 2);
    out->top_crit = melee_col_i32(res, 0, 3);
    out->idle     = melee_col_i64(res, 0, 4);
    hit = true;
  }

out:
  db_result_free(res);
  if(e_meth != NULL) mem_free(e_meth);
  if(e_chan != NULL) mem_free(e_chan);

  return(hit);
}

bool
melee_db_round_abandon(int64_t round_id)
{
  melee_tables_t t;
  char           sql[256];

  if(round_id <= 0 || melee_tables_resolve(&t) != SUCCESS)
    return(FAIL);

  snprintf(sql, sizeof(sql),
      "UPDATE %s SET state = %d, ended_at = NOW() WHERE id = %" PRId64,
      t.rounds, MELEE_ROUND_ABANDONED, round_id);

  return(melee_exec(sql, "round abandon", NULL));
}

int64_t
melee_db_round_open(uint32_t ns_id, const char *method, const char *channel,
    const char *opener)
{
  melee_tables_t t;
  db_result_t   *res      = NULL;
  char          *e_meth   = NULL;
  char          *e_chan   = NULL;
  char          *e_opener = NULL;
  char           sql[1024];
  int64_t        id = -1;

  if(melee_tables_resolve(&t) != SUCCESS)
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
    id = melee_col_i64(res, 0, 0);

  else
    clam(CLAM_WARN, MELEE_CTX, "round open failed: %s",
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
melee_db_player_enrol(int64_t round_id, uint32_t ns_id, const char *username,
    const char *nickname, int32_t hp)
{
  melee_tables_t t;
  char          *e_user = NULL;
  char          *e_nick = NULL;
  char           sql[1536];
  uint32_t       affected = 0;
  bool           fresh    = false;

  if(round_id <= 0 || username == NULL || melee_tables_resolve(&t) != SUCCESS)
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

  if(melee_exec(sql, "player enrol", &affected) == SUCCESS)
    fresh = (affected > 0);

out:
  if(e_user != NULL) mem_free(e_user);
  if(e_nick != NULL) mem_free(e_nick);

  return(fresh);
}

bool
melee_db_player_get(int64_t round_id, const char *username,
    melee_player_t *out)
{
  melee_tables_t t;
  db_result_t   *res    = NULL;
  char          *e_user = NULL;
  char           sql[512];
  bool           hit = false;

  if(out == NULL || username == NULL || melee_tables_resolve(&t) != SUCCESS)
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
    melee_col_str(out->nickname, sizeof(out->nickname), res, 0, 0);
    out->hp        = melee_col_i32(res, 0, 1);
    out->hp_max    = melee_col_i32(res, 0, 2);
    out->last_wave = melee_col_i32(res, 0, 3);
    hit = true;
  }

  db_result_free(res);
  mem_free(e_user);

  return(hit);
}

bool
melee_db_pending(int64_t round_id, int32_t wave, char *out, size_t cap)
{
  melee_tables_t t;
  db_result_t   *res = NULL;
  char           sql[512];
  bool           ok = FAIL;

  if(out == NULL || cap == 0)
    return(FAIL);

  out[0] = '\0';

  if(melee_tables_resolve(&t) != SUCCESS)
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
    melee_col_str(out, cap, res, 0, 0);
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
melee_db_card_find(uint32_t ns_id, const char *method, const char *channel,
    melee_card_t *out)
{
  melee_tables_t t;
  db_result_t   *res    = NULL;
  char          *e_meth = NULL;
  char          *e_chan = NULL;
  char           room[512] = "";
  char           sql[1024];
  bool           hit = false;

  if(out == NULL || melee_tables_resolve(&t) != SUCCESS)
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
      t.rounds, ns_id, room, MELEE_ROUND_ACTIVE);

  res = db_result_alloc();

  if(res != NULL && db_query(sql, res) == SUCCESS && res->ok && res->rows == 1)
  {
    out->id     = melee_col_i64(res, 0, 0);
    melee_col_str(out->channel, sizeof(out->channel), res, 0, 1);
    out->state  = melee_col_i32(res, 0, 2);
    out->wave   = melee_col_i32(res, 0, 3);
    out->blows  = melee_col_i32(res, 0, 4);
    out->length = melee_col_i64(res, 0, 5);
    out->top_crit = melee_col_i32(res, 0, 6);
    melee_col_str(out->top_by,  sizeof(out->top_by),  res, 0, 7);
    melee_col_str(out->top_on,  sizeof(out->top_on),  res, 0, 8);
    melee_col_str(out->slayer,  sizeof(out->slayer),  res, 0, 9);
    melee_col_str(out->fallen,  sizeof(out->fallen),  res, 0, 10);
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
melee_db_card_roster(int64_t round_id, melee_card_row_t *out, uint32_t cap,
    uint32_t *total)
{
  melee_tables_t t;
  db_result_t   *res = NULL;
  char           sql[768];
  uint32_t       n = 0;

  if(total != NULL)
    *total = 0;

  if(out == NULL || cap == 0 || round_id <= 0 ||
     melee_tables_resolve(&t) != SUCCESS)
    return(0);

  snprintf(sql, sizeof(sql),
      "SELECT CASE WHEN nickname <> '' THEN nickname ELSE username END,"
      " hp, hp_max, dmg_given, dmg_taken, best_crit, last_wave,"
      " COUNT(*) OVER ()"
      " FROM %s WHERE round_id = %" PRId64
      " ORDER BY hp DESC, dmg_given DESC, username LIMIT %" PRIu32,
      t.players, round_id, cap);

  res = db_result_alloc();

  if(res != NULL && db_query(sql, res) == SUCCESS && res->ok)
  {
    for(n = 0; n < res->rows && n < cap; n++)
    {
      melee_col_str(out[n].name, sizeof(out[n].name), res, n, 0);
      out[n].hp        = melee_col_i32(res, n, 1);
      out[n].hp_max    = melee_col_i32(res, n, 2);
      out[n].dmg_given = melee_col_i32(res, n, 3);
      out[n].dmg_taken = melee_col_i32(res, n, 4);
      out[n].best_crit = melee_col_i32(res, n, 5);
      out[n].last_wave = melee_col_i32(res, n, 6);

      if(total != NULL)
        *total = (uint32_t)melee_col_i64(res, n, 7);
    }
  }

  db_result_free(res);
  return(n);
}

uint32_t
melee_db_scores(uint32_t ns_id, uint32_t limit, melee_score_row_t *out,
    uint32_t cap)
{
  melee_tables_t t;
  db_result_t   *res = NULL;
  char           sql[640];
  uint32_t       n = 0;

  if(out == NULL || cap == 0 || melee_tables_resolve(&t) != SUCCESS)
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
      melee_col_str(out[n].name, sizeof(out[n].name), res, n, 0);
      out[n].rounds    = melee_col_i32(res, n, 1);
      out[n].kills     = melee_col_i32(res, n, 2);
      out[n].deaths    = melee_col_i32(res, n, 3);
      out[n].dmg_given = melee_col_i64(res, n, 4);
      out[n].dmg_taken = melee_col_i64(res, n, 5);
      out[n].crits     = melee_col_i32(res, n, 6);
      out[n].best_crit = melee_col_i32(res, n, 7);
    }
  }

  db_result_free(res);
  return(n);
}

bool
melee_db_deadliest(uint32_t ns_id, char *by, size_t by_cap, char *on,
    size_t on_cap, int32_t *dmg)
{
  melee_tables_t t;
  db_result_t   *res = NULL;
  char           sql[512];
  bool           hit = false;

  if(by == NULL || on == NULL || dmg == NULL || by_cap == 0 || on_cap == 0)
    return(false);

  by[0] = '\0';
  on[0] = '\0';
  *dmg  = 0;

  if(melee_tables_resolve(&t) != SUCCESS)
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
    melee_col_str(by, by_cap, res, 0, 0);
    melee_col_str(on, on_cap, res, 0, 1);
    *dmg = melee_col_i32(res, 0, 2);
    hit  = true;
  }

  db_result_free(res);
  return(hit);
}

// ------------------------------------------------------------------ //
// The blow                                                            //
// ------------------------------------------------------------------ //

bool
melee_db_blow_apply(const melee_blow_t *b)
{
  melee_tables_t t;
  char          *e_atk_u = NULL;
  char          *e_atk_n = NULL;
  char          *e_tgt_u = NULL;
  char          *e_tgt_n = NULL;
  char          *sql     = NULL;
  char           top  [320]  = "";
  char           death[2048] = "";
  size_t         need;
  bool           ok = FAIL;

  if(b == NULL || melee_tables_resolve(&t) != SUCCESS)
    return(FAIL);

  e_atk_u = db_escape(b->atk_user);
  e_atk_n = db_escape(b->atk_nick);
  e_tgt_u = db_escape(b->tgt_user);
  e_tgt_n = db_escape(b->tgt_nick);

  if(e_atk_u == NULL || e_atk_n == NULL || e_tgt_u == NULL || e_tgt_n == NULL)
    goto out;

  if(b->new_top)
    snprintf(top, sizeof(top),
        ", top_crit = %d, top_crit_by = '%s', top_crit_on = '%s'",
        b->dmg, e_atk_u, e_tgt_u);

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
        t.rounds, MELEE_ROUND_ENDED, e_atk_u, e_tgt_u, b->round_id,
        t.scores, b->ns_id, e_atk_u,
        t.scores, b->ns_id, e_tgt_u);

  need = 4096 + strlen(top) + strlen(death)
      + 8 * (strlen(e_atk_u) + strlen(e_atk_n) + strlen(e_tgt_u)
             + strlen(e_tgt_n) + strlen(t.rounds) + strlen(t.players)
             + strlen(t.scores));

  sql = mem_alloc(MELEE_CTX, "blow_sql", need);

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

      t.players, e_atk_n, b->dmg, b->crit ? 1 : 0,
      b->crit ? b->dmg : 0, b->wave, b->round_id, e_atk_u,

      t.players, e_tgt_n, b->dmg, b->dmg, b->dmg, b->round_id, e_tgt_u,

      t.rounds, top, b->round_id,

      t.scores, b->ns_id, e_atk_u, e_atk_n, b->dmg, b->crit ? 1 : 0,
      b->crit ? b->dmg : 0, b->crit ? e_tgt_u : "",
      t.scores, t.scores, t.scores, t.scores, t.scores, t.scores,

      t.scores, b->ns_id, e_tgt_u, e_tgt_n, b->dmg,
      t.scores,

      t.rounds, b->round_id, t.players,

      death);

  ok = melee_exec(sql, "blow apply", NULL);

out:
  if(e_atk_u != NULL) mem_free(e_atk_u);
  if(e_atk_n != NULL) mem_free(e_atk_n);
  if(e_tgt_u != NULL) mem_free(e_tgt_u);
  if(e_tgt_n != NULL) mem_free(e_tgt_n);
  if(sql     != NULL) mem_free(sql);

  return(ok);
}
