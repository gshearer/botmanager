#ifndef BM_MELEE_H
#define BM_MELEE_H

// botmanager — MIT
// melee: the Drow duelling pit. `!melee <nick>` rolls damage against
// another registered user; hit points, waves, and per-round + lifetime
// scores persist in the caller's user namespace. The first death ends
// the round, and where the protocol allows it the fallen are removed
// from the room through the generic method eject primitive.
//
// ---- Public-facing: nothing. The plugin talks to the core exclusively
// through cmd_register / cmd_reply / db_* / kv_* / method_*. All
// declarations below are internal, gated on MELEE_INTERNAL.

#ifdef MELEE_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "method.h"
#include "plugin.h"
#include "userns.h"

#include <stdint.h>

// CLAM context (registered in CLAM.md).
#define MELEE_CTX            "melee"

// KV keys (registered under the plugin schema).
#define MELEE_KV_PREFIX      "plugin.melee.table_prefix"
#define MELEE_KV_START_HP    "plugin.melee.start_hp"
#define MELEE_KV_HIT_MAX     "plugin.melee.hit_max"
#define MELEE_KV_CRIT_PCT    "plugin.melee.crit_chance_pct"
#define MELEE_KV_CRIT_MIN    "plugin.melee.crit_min"
#define MELEE_KV_CRIT_MAX    "plugin.melee.crit_max"
#define MELEE_KV_TIMEOUT     "plugin.melee.round_timeout"
#define MELEE_KV_EJECT       "plugin.melee.eject_on_death"
#define MELEE_KV_SCORE_ROWS  "plugin.melee.scoreboard_rows"

// Storage bounds. The table prefix is a SQL identifier, so it is
// validated as strict alnum/underscore before it can reach a query.
#define MELEE_PREFIX_SZ      32                 // table-name prefix
#define MELEE_TABLE_SZ       64                 // prefix + "_players"
#define MELEE_USER_SZ        USERNS_USER_SZ     // 31
#define MELEE_NICK_SZ        METHOD_NICKNAME_SZ // 64
#define MELEE_MAX_PLAYERS    32                 // per round, for the show card
#define MELEE_LINE_SZ        512                // one rendered, colorized line
// A comma-joined display list. Deliberately well under MELEE_LINE_SZ so
// it always fits inside the sentence that carries it; a roster longer
// than this would not survive an IRC line anyway.
#define MELEE_ROSTER_SZ      320

// Round lifecycle, as stored in <prefix>_rounds.state.
#define MELEE_ROUND_ACTIVE    0
#define MELEE_ROUND_ENDED     1
#define MELEE_ROUND_ABANDONED 2

// Every knob an operator can turn, already sanitised. Read once per
// command with melee_tunables_load(); never read the raw KV at a use
// site — an operator can set hit_max to 0 or invert the crit band.
typedef struct
{
  uint32_t start_hp;         // hit points each combatant enters with
  uint32_t hit_max;          // ordinary blow rolls 1..hit_max
  uint32_t crit_pct;         // percent chance a blow lands critical
  uint32_t crit_min;         // critical damage floor
  uint32_t crit_max;         // critical damage ceiling (>= crit_min)
  uint32_t round_timeout;    // seconds of silence before a round is cold
  uint32_t scoreboard_rows;  // rows shown by `show melee scores`
  bool     eject_on_death;   // remove the fallen where the method allows
} melee_tunables_t;

// The three table names for the configured prefix, resolved together so
// the prefix is validated exactly once per operation.
typedef struct
{
  char rounds [MELEE_TABLE_SZ];
  char players[MELEE_TABLE_SZ];
  char scores [MELEE_TABLE_SZ];
} melee_tables_t;

// A brawl in progress, as the turn engine needs to see it.
typedef struct
{
  int64_t id;
  int32_t wave;
  int32_t blows;
  int32_t top_crit;
  int64_t idle;        // seconds since last_action
} melee_round_t;

// One combatant's standing within a round.
typedef struct
{
  char    nickname[MELEE_NICK_SZ];
  int32_t hp;
  int32_t hp_max;
  int32_t last_wave;
} melee_player_t;

// One resolved turn, ready to be written. The `*_user` names are
// namespace-scoped usernames and are the identity of record; the
// `*_nick` names are display-only and are refreshed on every blow.
typedef struct
{
  int64_t     round_id;
  uint32_t    ns_id;
  const char *atk_user;
  const char *atk_nick;
  const char *tgt_user;
  const char *tgt_nick;
  int32_t     dmg;
  int32_t     wave;      // the wave the attacker is spending
  bool        crit;
  bool        new_top;   // this crit is the round's heaviest so far
  bool        fatal;     // the target reaches 0 hp
} melee_blow_t;

// ---- Plugin core (melee.c) ----------------------------------------- //

void melee_tunables_load(melee_tunables_t *out);

// ---- DB layer (melee_db.c) ----------------------------------------- //

// Resolve + validate the configured table names into `out`. FAIL when
// the KV prefix is not a safe SQL identifier.
bool melee_tables_resolve(melee_tables_t *out);

// Ensure the three tables + their indexes exist. Idempotent; runs once.
bool melee_schema_ensure(void);

// The active round in this room, if there is one.
bool melee_db_round_find(uint32_t ns_id, const char *method,
    const char *channel, melee_round_t *out);

// Retire a round nobody came back to.
bool melee_db_round_abandon(int64_t round_id);

// Open a round. Returns the new id (>0) or -1.
int64_t melee_db_round_open(uint32_t ns_id, const char *method,
    const char *channel, const char *opener);

// Enrol a combatant at full health, bumping <p>_scores.rounds iff they
// were not already in this round. Returns true when newly enrolled.
bool melee_db_player_enrol(int64_t round_id, uint32_t ns_id,
    const char *username, const char *nickname, int32_t hp);

bool melee_db_player_get(int64_t round_id, const char *username,
    melee_player_t *out);

// Comma-joined display names of the living who have not swung in
// `wave`. Writes an empty string when nobody is pending.
bool melee_db_pending(int64_t round_id, int32_t wave, char *out,
    size_t cap);

// Write a whole turn as one transaction: both combatants, the round
// counters, both lifetime score rows, the wave advance, and — when the
// blow is fatal — the round close and the kill/death tally. All or
// nothing; a daemon death mid-turn can never leave the attacker charged
// for a blow the target never took.
bool melee_db_blow_apply(const melee_blow_t *blow);

// ---- Combat (melee_combat.c) --------------------------------------- //

// Roll one blow. *crit_out reports whether it landed critical.
int32_t melee_roll(const melee_tunables_t *t, bool *crit_out);

void melee_render_blow(char *out, size_t cap, const char *atk_nick,
    const char *tgt_nick, int32_t dmg, bool crit, int32_t hp,
    int32_t hp_max);

void melee_render_death(char *out, size_t cap, const char *slayer_nick,
    const char *fallen_nick);

// ---- Command surface (melee_cmds.c) -------------------------------- //

bool melee_commands_register(void);
void melee_commands_unregister(void);

#endif // MELEE_INTERNAL

#endif // BM_MELEE_H
