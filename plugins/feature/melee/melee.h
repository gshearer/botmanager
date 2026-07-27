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

// ---- Plugin core (melee.c) ----------------------------------------- //

void melee_tunables_load(melee_tunables_t *out);

// ---- DB layer (melee_db.c) ----------------------------------------- //

// Resolve + validate the configured table names into `out`. FAIL when
// the KV prefix is not a safe SQL identifier.
bool melee_tables_resolve(melee_tables_t *out);

// Ensure the three tables + their indexes exist. Idempotent; runs once.
bool melee_schema_ensure(void);

#endif // MELEE_INTERNAL

#endif // BM_MELEE_H
