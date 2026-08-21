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
    "Table-name prefix for the pit's four tables (bare SQL identifier)" },
  { ATK_KV_CLASSES,    KV_STR,    ATK_CLASSES_DIR,
    "Directory the character sheets are read from; relative paths "
    "resolve against the daemon's working directory" },
  { ATK_KV_START_HP,   KV_UINT32, "100",
    "Hit points each combatant enters a round with" },
  { ATK_KV_DMG_MAX,    KV_UINT32, "20",
    "Maximum damage of any blow; every damage turn rolls 1..N, uniform "
    "and identical for every class" },

  // 25 / 55 / 85, not 25 / 50 / 75. The top band is now BOTH the word
  // and the crit statistic, so it has to be the rarest: over dmg_max 20
  // this pays minor 1-4 (20%), medium 5-10 (30%), major 11-16 (30%),
  // critical 17-20 (20%). At the old 75 a third of every blow struck
  // would have been recorded as a critical hit.
  { ATK_KV_SEV_MEDIUM, KV_UINT32, "25",
    "Percent of the heaviest possible blow at which a hit reads as "
    "'medium' rather than 'minor'" },
  { ATK_KV_SEV_MAJOR,  KV_UINT32, "55",
    "Percent of the heaviest possible blow at which a hit reads as "
    "'major'" },
  { ATK_KV_SEV_CRIT,   KV_UINT32, "85",
    "Percent of the heaviest possible blow at which a hit reads as "
    "'critical' — and counts as a critical hit in the standings" },

  { ATK_KV_ROUND_IDLE, KV_UINT32, "14400",
    "Seconds a brawl may go idle before the game is over; each !attack "
    "or !heal resets the clock and a fresh attack opens a new one" },
  { ATK_KV_EJECT,      KV_UINT8,  "1",
    "Remove the fallen from the room (KILL/KICK) where the method allows" },
  { ATK_KV_SCORE_ROWS, KV_UINT32, "15",
    "Rows shown by `show attack scores`" },
  { ATK_KV_AOE_PCT,    KV_UINT32, "3",
    "Percent chance a damage turn strikes everyone in the pit but the "
    "attacker; 0 disables it" },

  { ATK_KV_DOT_CHANCE,  KV_UINT32, "25",
    "Percent chance a turn deals its damage over time instead of at "
    "once, where the attacker's class has the moves for it; 0 disables "
    "the feature entirely" },
  { ATK_KV_DOT_TICKS,   KV_UINT32, "3",
    "Most messages one affliction may ever speak; its damage is spread "
    "across exactly this many ticks" },
  { ATK_KV_DOT_TICK,    KV_UINT32, "20",
    "Seconds between the ticks of an affliction" },
  { ATK_KV_DOT_STACK,   KV_UINT32, "3",
    "Afflictions one combatant may carry at once within a round" },
  { ATK_KV_DOT_LINGER,  KV_UINT32, "3600",
    "Seconds the decay task stays queued after the last affliction "
    "clears, before it removes itself" },

  { ATK_KV_HEAL_PCT,    KV_UINT32, "25",
    "Percent chance a heal is major rather than minor" },
  { ATK_KV_HEAL_MIN_LO, KV_UINT32, "1",
    "Fewest hit points a minor heal restores" },
  { ATK_KV_HEAL_MIN_HI, KV_UINT32, "10",
    "Most hit points a minor heal restores" },
  { ATK_KV_HEAL_MAJ_LO, KV_UINT32, "10",
    "Fewest hit points a major heal restores" },
  { ATK_KV_HEAL_MAJ_HI, KV_UINT32, "20",
    "Most hit points a major heal restores" },

  { ATK_KV_DEFER_MAX,   KV_UINT32, "3",
    "Turns one combatant may defer per round; 0 disables deferral" },
  { ATK_KV_DEFER_LO,    KV_UINT32, "15",
    "Smallest percentage bonus one deferral adds to the next turn" },
  { ATK_KV_DEFER_HI,    KV_UINT32, "35",
    "Largest percentage bonus one deferral adds to the next turn" },
  { ATK_KV_DEFER_CAP,   KV_UINT32, "100",
    "Hard ceiling on the bonus deferrals may accumulate; 100 means a "
    "deferred turn can at most double its damage" },
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

// Nothing downstream re-reads the raw KV: a zero dmg_max would make
// util_rand(0) meaningless and an inverted band would roll a negative
// width, so both are corrected here, once, at the boundary.
void
atk_tunables_load(atk_tunables_t *out)
{
  const char *path;

  if(out == NULL)
    return;

  path = kv_get_str(ATK_KV_CLASSES);

  snprintf(out->classes_path, sizeof(out->classes_path), "%s",
      path[0] != '\0' ? path : ATK_CLASSES_DIR);

  out->start_hp        = atk_clamp(kv_get_uint(ATK_KV_START_HP),   1, 100000);
  out->dmg_max         = atk_clamp(kv_get_uint(ATK_KV_DMG_MAX),    1, 1000);
  out->round_max_idle_secs = atk_clamp(kv_get_uint(ATK_KV_ROUND_IDLE), 60,
                                     604800);
  out->scoreboard_rows = atk_clamp(kv_get_uint(ATK_KV_SCORE_ROWS), 1,
                                     ATK_MAX_SCORE_ROWS);
  out->aoe_pct         = atk_clamp(kv_get_uint(ATK_KV_AOE_PCT),     0, 100);
  out->eject_on_death  = (kv_get_uint(ATK_KV_EJECT) != 0);

  out->sev_medium_at = atk_clamp(kv_get_uint(ATK_KV_SEV_MEDIUM), 1, 98);
  out->sev_major_at  = atk_clamp(kv_get_uint(ATK_KV_SEV_MAJOR),  1, 99);
  out->sev_crit_at   = atk_clamp(kv_get_uint(ATK_KV_SEV_CRIT),   1, 100);

  // Strictly ascending, or a tier would be unreachable — and a tier the
  // engine can roll but no class can speak is a fairness bug now, not
  // merely a dead pool of lines. The 98/99/100 ceilings above are what
  // make both corrections safe.
  if(out->sev_major_at <= out->sev_medium_at)
    out->sev_major_at = out->sev_medium_at + 1;

  if(out->sev_crit_at <= out->sev_major_at)
    out->sev_crit_at = out->sev_major_at + 1;

  // Damage over time. Zero chance is legal — it is the off switch — but
  // every number below it must be usable. There is no tick damage knob:
  // a DOT spends the ordinary roll across its ticks and nothing else.
  out->dot_chance_pct  = atk_clamp(kv_get_uint(ATK_KV_DOT_CHANCE), 0, 100);
  out->dot_max_ticks   = atk_clamp(kv_get_uint(ATK_KV_DOT_TICKS),  1, 50);
  out->dot_tick_secs   = atk_clamp(kv_get_uint(ATK_KV_DOT_TICK),   1, 3600);
  out->dot_stack_max   = atk_clamp(kv_get_uint(ATK_KV_DOT_STACK),  1,
                                     ATK_DOT_STACK_CAP);
  out->dot_linger_secs = atk_clamp(kv_get_uint(ATK_KV_DOT_LINGER), 60, 86400);

  out->heal_major_pct = atk_clamp(kv_get_uint(ATK_KV_HEAL_PCT),    0, 100);
  out->heal_minor_min = atk_clamp(kv_get_uint(ATK_KV_HEAL_MIN_LO), 1, 100000);
  out->heal_minor_max = atk_clamp(kv_get_uint(ATK_KV_HEAL_MIN_HI), 1, 100000);
  out->heal_major_min = atk_clamp(kv_get_uint(ATK_KV_HEAL_MAJ_LO), 1, 100000);
  out->heal_major_max = atk_clamp(kv_get_uint(ATK_KV_HEAL_MAJ_HI), 1, 100000);

  out->defer_max     = atk_clamp(kv_get_uint(ATK_KV_DEFER_MAX),  0, 10);
  out->defer_step_lo = atk_clamp(kv_get_uint(ATK_KV_DEFER_LO),   0, 200);
  out->defer_step_hi = atk_clamp(kv_get_uint(ATK_KV_DEFER_HI),   0, 200);
  out->defer_cap_pct = atk_clamp(kv_get_uint(ATK_KV_DEFER_CAP),  0, 1000);

  // Three bands, one correction, one reason: a random width of
  // max - min + 1 must never be computed from an inverted band.
  if(out->heal_minor_max < out->heal_minor_min)
    out->heal_minor_max = out->heal_minor_min;

  if(out->heal_major_max < out->heal_major_min)
    out->heal_major_max = out->heal_major_min;

  if(out->defer_step_hi < out->defer_step_lo)
    out->defer_step_hi = out->defer_step_lo;
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
  atk_load_report_t rep;

  if(atk_schema_ensure() != SUCCESS)
    clam(CLAM_WARN, ATK_CTX,
        "attack schema init failed (the pit will error until fixed)");

  // After the schema, and never before kv_load(): classes_path is a
  // persisted knob like every other. A directory that yields nothing
  // still leaves one class standing — see attack_class.c's D7 fallback.
  atk_class_load(&rep);
  clam(CLAM_INFO, ATK_CTX, "%u character class(es) loaded, %u rejected",
      rep.accepted, rep.rejected);

  // Afflictions outlive the mapping that was ticking them, and the task
  // does not. Never a bare atk_dot_wake() here: a periodic's first
  // iteration runs IMMEDIATELY, and on a cold boot that lands before any
  // method instance exists — which cancels the very rows it came to save.
  atk_dot_resume();

  return(SUCCESS);
}

static void
atk_deinit(void)
{
  // Before anything else: a periodic callback left pointing into an
  // unloaded .so is a jump into freed memory, and the task queue outlives
  // this plugin.
  atk_dot_stop();

  atk_class_free();
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
  .requires             = { { .name = "bot_chat" } },
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
