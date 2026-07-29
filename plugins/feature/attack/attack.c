// botmanager — MIT
// attack feature plugin (PLUGIN_FEATURE, kind: attack).
//
// A channel brawl. The engine owns the numbers and speaks no setting of
// its own; the words belong to the combatants' character sheets.
// `!attack <nick>` opens a round in the current room and rolls damage at
// another registered user; the round, its combatants, and the lifetime
// standings all live in Postgres, scoped to the caller's namespace.
//
// This file owns the descriptor, the KV schema, and the lifecycle. The
// tables live in attack_db.c.

#define ATTACK_INTERNAL
#include "attack.h"

#include "kv.h"

#include <stdio.h>

// ------------------------------------------------------------------ //
// KV schema                                                           //
// ------------------------------------------------------------------ //

static const plugin_kv_entry_t atk_kv_schema[] = {
  { ATK_KV_PREFIX,     KV_STR,    "attack",
    "Table-name prefix for the pit's three tables (bare SQL identifier)" },
  { ATK_KV_START_HP,   KV_UINT32, "100",
    "Hit points each combatant enters a round with" },
  { ATK_KV_HIT_MAX,    KV_UINT32, "10",
    "Maximum damage of an ordinary blow (rolls 1..N)" },
  { ATK_KV_CRIT_PCT,   KV_UINT32, "12",
    "Percent chance a blow lands as a critical hit" },
  { ATK_KV_CRIT_MIN,   KV_UINT32, "10",
    "Minimum critical-hit damage" },
  { ATK_KV_CRIT_MAX,   KV_UINT32, "20",
    "Maximum critical-hit damage" },
  { ATK_KV_SEV_MEDIUM, KV_UINT32, "25",
    "Percent of the heaviest possible blow at which a hit reads as "
    "'medium' rather than 'minor'" },
  { ATK_KV_SEV_MAJOR,  KV_UINT32, "50",
    "Percent of the heaviest possible blow at which a hit reads as "
    "'major'" },
  { ATK_KV_SEV_CRIT,   KV_UINT32, "75",
    "Percent of the heaviest possible blow at which a hit reads as "
    "'critical'" },
  { ATK_KV_TIMEOUT,    KV_UINT32, "1800",
    "Seconds of inactivity before a round is abandoned" },
  { ATK_KV_EJECT,      KV_UINT8,  "1",
    "Remove the fallen from the room (KILL/KICK) where the method allows" },
  { ATK_KV_SCORE_ROWS, KV_UINT32, "15",
    "Rows shown by `show attack scores`" },

  { ATK_KV_DOT_CHANCE,  KV_UINT32, "25",
    "Percent chance a landing, non-fatal blow leaves an affliction that "
    "keeps dealing damage; 0 disables the feature entirely" },
  { ATK_KV_DOT_MIN,     KV_UINT32, "5",
    "Shortest an affliction lasts, in seconds" },
  { ATK_KV_DOT_MAX,     KV_UINT32, "60",
    "Longest an affliction lasts, in seconds" },
  { ATK_KV_DOT_TICK,    KV_UINT32, "5",
    "Seconds between the ticks of an affliction" },
  { ATK_KV_DOT_DMG,     KV_UINT32, "3",
    "Maximum damage one tick of an affliction deals (rolls 1..N)" },
  { ATK_KV_DOT_STACK,   KV_UINT32, "1",
    "Afflictions one combatant may carry at once within a round" },
  { ATK_KV_DOT_LINGER,  KV_UINT32, "3600",
    "Seconds the decay task stays queued after the last affliction "
    "clears, before it removes itself" },
};

// ------------------------------------------------------------------ //
// Tunables                                                            //
// ------------------------------------------------------------------ //

static uint32_t
atk_clamp(uint64_t val, uint32_t lo, uint32_t hi)
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
atk_tunables_load(atk_tunables_t *out)
{
  if(out == NULL)
    return;

  out->start_hp        = atk_clamp(kv_get_uint(ATK_KV_START_HP),   1, 100000);
  out->hit_max         = atk_clamp(kv_get_uint(ATK_KV_HIT_MAX),    1, 1000);
  out->crit_pct        = atk_clamp(kv_get_uint(ATK_KV_CRIT_PCT),   0, 100);
  out->crit_min        = atk_clamp(kv_get_uint(ATK_KV_CRIT_MIN),   1, 100000);
  out->crit_max        = atk_clamp(kv_get_uint(ATK_KV_CRIT_MAX),   1, 100000);
  out->round_timeout   = atk_clamp(kv_get_uint(ATK_KV_TIMEOUT),   30, 604800);
  out->scoreboard_rows = atk_clamp(kv_get_uint(ATK_KV_SCORE_ROWS), 1,
                                     ATK_MAX_SCORE_ROWS);
  out->eject_on_death  = (kv_get_uint(ATK_KV_EJECT) != 0);

  if(out->crit_max < out->crit_min)
    out->crit_max = out->crit_min;

  out->sev_medium_at = atk_clamp(kv_get_uint(ATK_KV_SEV_MEDIUM), 1, 98);
  out->sev_major_at  = atk_clamp(kv_get_uint(ATK_KV_SEV_MAJOR),  1, 99);
  out->sev_crit_at   = atk_clamp(kv_get_uint(ATK_KV_SEV_CRIT),   1, 100);

  // Strictly ascending, or a tier would be unreachable and the pool
  // behind it would fill with lines nothing ever speaks. The 98/99/100
  // ceilings above are what make both corrections safe.
  if(out->sev_major_at <= out->sev_medium_at)
    out->sev_major_at = out->sev_medium_at + 1;

  if(out->sev_crit_at <= out->sev_major_at)
    out->sev_crit_at = out->sev_major_at + 1;

  // Damage over time. Zero chance is legal — it is the off switch — but
  // every duration below it must be a usable number.
  out->dot_chance_pct   = atk_clamp(kv_get_uint(ATK_KV_DOT_CHANCE), 0, 100);
  out->dot_min_secs     = atk_clamp(kv_get_uint(ATK_KV_DOT_MIN),    1, 3600);
  out->dot_max_secs     = atk_clamp(kv_get_uint(ATK_KV_DOT_MAX),    1, 86400);
  out->dot_tick_secs    = atk_clamp(kv_get_uint(ATK_KV_DOT_TICK),   1, 300);
  out->dot_tick_dmg_max = atk_clamp(kv_get_uint(ATK_KV_DOT_DMG),    1, 1000);
  out->dot_stack_max    = atk_clamp(kv_get_uint(ATK_KV_DOT_STACK),  1, 4);
  out->dot_linger_secs  = atk_clamp(kv_get_uint(ATK_KV_DOT_LINGER),
                                      60, 86400);

  // The same correction crit_max gets, for the same reason: a random
  // width of max - min + 1 must never be computed from an inverted band.
  if(out->dot_max_secs < out->dot_min_secs)
    out->dot_max_secs = out->dot_min_secs;

  // A cadence slower than the shortest affliction would mint DOTs that
  // expire before they ever speak. Silently lower it, as everywhere else.
  if(out->dot_tick_secs > out->dot_min_secs)
    out->dot_tick_secs = out->dot_min_secs;
}

// ------------------------------------------------------------------ //
// Lifecycle                                                           //
// ------------------------------------------------------------------ //

static bool
atk_init(void)
{
  if(atk_commands_register() != SUCCESS)
  {
    clam(CLAM_WARN, ATK_CTX, "command registration failed");
    return(FAIL);
  }

  clam(CLAM_INFO, ATK_CTX, "attack plugin initialized");
  return(SUCCESS);
}

// Schema bootstrap runs in start(), after kv_load(), so the table prefix
// reflects the persisted KV rather than the register-time default.
static bool
atk_start(void)
{
  if(atk_schema_ensure() != SUCCESS)
    clam(CLAM_WARN, ATK_CTX,
        "attack schema init failed (the pit will error until fixed)");

  return(SUCCESS);
}

static void
atk_deinit(void)
{
  // Before anything else: a periodic callback left pointing into an
  // unloaded .so is a jump into freed memory, and the task queue outlives
  // this plugin.
  atk_dot_stop();

  atk_commands_unregister();
  clam(CLAM_INFO, ATK_CTX, "attack plugin deinitialized");
}

// ------------------------------------------------------------------ //
// Plugin descriptor                                                   //
// ------------------------------------------------------------------ //

const plugin_desc_t bm_plugin_desc = {
  .api_version          = PLUGIN_API_VERSION,
  .name                 = "attack",
  .version              = "1.0",
  .type                 = PLUGIN_FEATURE,
  .kind                 = "attack",
  .provides             = { { .name = "feature_attack" } },
  .provides_count       = 1,
  .requires             = { { .name = "method_text" } },
  .requires_count       = 1,
  .kv_schema            = atk_kv_schema,
  .kv_schema_count      = sizeof(atk_kv_schema) / sizeof(atk_kv_schema[0]),
  .kv_inst_schema       = NULL,
  .kv_inst_schema_count = 0,
  .init                 = atk_init,
  .start                = atk_start,
  .stop                 = NULL,
  .deinit               = atk_deinit,
  .ext                  = NULL,
};
