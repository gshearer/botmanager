// botmanager — MIT
// melee feature plugin (PLUGIN_FEATURE, kind: melee).
//
// A channel brawl themed on the Drow: Lolth, Menzoberranzan, Narbondel.
// `!melee <nick>` opens a round in the current room and rolls damage at
// another registered user; the round, its combatants, and the lifetime
// standings all live in Postgres, scoped to the caller's namespace.
//
// This file owns the descriptor, the KV schema, and the lifecycle. The
// tables live in melee_db.c.

#define MELEE_INTERNAL
#include "melee.h"

#include "kv.h"

// ------------------------------------------------------------------ //
// KV schema                                                           //
// ------------------------------------------------------------------ //

static const plugin_kv_entry_t melee_kv_schema[] = {
  { MELEE_KV_PREFIX,     KV_STR,    "melee",
    "Table-name prefix for the pit's three tables (bare SQL identifier)" },
  { MELEE_KV_START_HP,   KV_UINT32, "100",
    "Hit points each combatant enters a round with" },
  { MELEE_KV_HIT_MAX,    KV_UINT32, "10",
    "Maximum damage of an ordinary blow (rolls 1..N)" },
  { MELEE_KV_CRIT_PCT,   KV_UINT32, "12",
    "Percent chance a blow lands as a critical hit" },
  { MELEE_KV_CRIT_MIN,   KV_UINT32, "10",
    "Minimum critical-hit damage" },
  { MELEE_KV_CRIT_MAX,   KV_UINT32, "20",
    "Maximum critical-hit damage" },
  { MELEE_KV_TIMEOUT,    KV_UINT32, "1800",
    "Seconds of inactivity before a round is abandoned" },
  { MELEE_KV_EJECT,      KV_UINT8,  "1",
    "Remove the fallen from the room (KILL/KICK) where the method allows" },
  { MELEE_KV_SCORE_ROWS, KV_UINT32, "15",
    "Rows shown by `show melee scores`" },
};

// ------------------------------------------------------------------ //
// Tunables                                                            //
// ------------------------------------------------------------------ //

static uint32_t
melee_clamp(uint64_t val, uint32_t lo, uint32_t hi)
{
  if(val < (uint64_t)lo)
    return(lo);

  if(val > (uint64_t)hi)
    return(hi);

  return((uint32_t)val);
}

// Nothing downstream re-reads the raw KV: a zero hit_max would make
// util_rand(0) meaningless and an inverted crit band would roll a
// negative width, so both are corrected here, once, at the boundary.
void
melee_tunables_load(melee_tunables_t *out)
{
  if(out == NULL)
    return;

  out->start_hp        = melee_clamp(kv_get_uint(MELEE_KV_START_HP),   1, 100000);
  out->hit_max         = melee_clamp(kv_get_uint(MELEE_KV_HIT_MAX),    1, 1000);
  out->crit_pct        = melee_clamp(kv_get_uint(MELEE_KV_CRIT_PCT),   0, 100);
  out->crit_min        = melee_clamp(kv_get_uint(MELEE_KV_CRIT_MIN),   1, 100000);
  out->crit_max        = melee_clamp(kv_get_uint(MELEE_KV_CRIT_MAX),   1, 100000);
  out->round_timeout   = melee_clamp(kv_get_uint(MELEE_KV_TIMEOUT),   30, 604800);
  out->scoreboard_rows = melee_clamp(kv_get_uint(MELEE_KV_SCORE_ROWS), 1, 50);
  out->eject_on_death  = (kv_get_uint(MELEE_KV_EJECT) != 0);

  if(out->crit_max < out->crit_min)
    out->crit_max = out->crit_min;
}

// ------------------------------------------------------------------ //
// Lifecycle                                                           //
// ------------------------------------------------------------------ //

static bool
melee_init(void)
{
  clam(CLAM_INFO, MELEE_CTX, "melee plugin initialized");
  return(SUCCESS);
}

// Schema bootstrap runs in start(), after kv_load(), so the table prefix
// reflects the persisted KV rather than the register-time default.
static bool
melee_start(void)
{
  if(melee_schema_ensure() != SUCCESS)
    clam(CLAM_WARN, MELEE_CTX,
        "melee schema init failed (the pit will error until fixed)");

  return(SUCCESS);
}

static void
melee_deinit(void)
{
  clam(CLAM_INFO, MELEE_CTX, "melee plugin deinitialized");
}

// ------------------------------------------------------------------ //
// Plugin descriptor                                                   //
// ------------------------------------------------------------------ //

const plugin_desc_t bm_plugin_desc = {
  .api_version          = PLUGIN_API_VERSION,
  .name                 = "melee",
  .version              = "1.0",
  .type                 = PLUGIN_FEATURE,
  .kind                 = "melee",
  .provides             = { { .name = "feature_melee" } },
  .provides_count       = 1,
  .requires             = { { .name = "method_text" } },
  .requires_count       = 1,
  .kv_schema            = melee_kv_schema,
  .kv_schema_count      = sizeof(melee_kv_schema) / sizeof(melee_kv_schema[0]),
  .kv_inst_schema       = NULL,
  .kv_inst_schema_count = 0,
  .init                 = melee_init,
  .start                = melee_start,
  .stop                 = NULL,
  .deinit               = melee_deinit,
  .ext                  = NULL,
};
