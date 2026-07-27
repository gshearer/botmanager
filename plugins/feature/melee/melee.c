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

#include <stdio.h>

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
  { MELEE_KV_SEV_MEDIUM, KV_UINT32, "25",
    "Percent of the heaviest possible blow at which a hit reads as "
    "'medium' rather than 'minor'" },
  { MELEE_KV_SEV_MAJOR,  KV_UINT32, "50",
    "Percent of the heaviest possible blow at which a hit reads as "
    "'major'" },
  { MELEE_KV_SEV_CRIT,   KV_UINT32, "75",
    "Percent of the heaviest possible blow at which a hit reads as "
    "'critical'" },
  { MELEE_KV_TIMEOUT,    KV_UINT32, "1800",
    "Seconds of inactivity before a round is abandoned" },
  { MELEE_KV_EJECT,      KV_UINT8,  "1",
    "Remove the fallen from the room (KILL/KICK) where the method allows" },
  { MELEE_KV_SCORE_ROWS, KV_UINT32, "15",
    "Rows shown by `show melee scores`" },

  { MELEE_KV_LLM_MODEL,   KV_STR,    "",
    "Chat model (from the llm subsystem) that authors combat flavour; "
    "empty = use the built-in lines" },
  { MELEE_KV_LLM_PROMPT,  KV_STR,    "../prompts/melee.txt",
    "Path to the persona prompt prepended to every flavour request "
    "(relative to the daemon CWD)" },
  { MELEE_KV_LLM_POOL,    KV_UINT32, "20",
    "Lines requested per category per refill" },
  { MELEE_KV_LLM_REFILL,  KV_UINT32, "6",
    "Refill a category when its pool falls to this many lines" },
  { MELEE_KV_LLM_TEMP,    KV_UINT32, "110",
    "Sampling temperature x100 (110 = 1.10); flavour wants more variety "
    "than prose" },
  { MELEE_KV_LLM_TOKENS,  KV_UINT32, "1500",
    "Token ceiling per refill request" },
  { MELEE_KV_LLM_TIMEOUT, KV_UINT32, "90",
    "Per-refill request timeout" },
  { MELEE_KV_LLM_RETRY,   KV_UINT32, "300",
    "Seconds to wait before retrying a category whose refill failed" },
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
  const char *model  = NULL;
  const char *prompt = NULL;

  if(out == NULL)
    return;

  out->start_hp        = melee_clamp(kv_get_uint(MELEE_KV_START_HP),   1, 100000);
  out->hit_max         = melee_clamp(kv_get_uint(MELEE_KV_HIT_MAX),    1, 1000);
  out->crit_pct        = melee_clamp(kv_get_uint(MELEE_KV_CRIT_PCT),   0, 100);
  out->crit_min        = melee_clamp(kv_get_uint(MELEE_KV_CRIT_MIN),   1, 100000);
  out->crit_max        = melee_clamp(kv_get_uint(MELEE_KV_CRIT_MAX),   1, 100000);
  out->round_timeout   = melee_clamp(kv_get_uint(MELEE_KV_TIMEOUT),   30, 604800);
  out->scoreboard_rows = melee_clamp(kv_get_uint(MELEE_KV_SCORE_ROWS), 1,
                                     MELEE_MAX_SCORE_ROWS);
  out->eject_on_death  = (kv_get_uint(MELEE_KV_EJECT) != 0);

  if(out->crit_max < out->crit_min)
    out->crit_max = out->crit_min;

  out->sev_medium_at = melee_clamp(kv_get_uint(MELEE_KV_SEV_MEDIUM), 1, 98);
  out->sev_major_at  = melee_clamp(kv_get_uint(MELEE_KV_SEV_MAJOR),  1, 99);
  out->sev_crit_at   = melee_clamp(kv_get_uint(MELEE_KV_SEV_CRIT),   1, 100);

  // Strictly ascending, or a tier would be unreachable and the pool
  // behind it would fill with lines nothing ever speaks. The 98/99/100
  // ceilings above are what make both corrections safe.
  if(out->sev_major_at <= out->sev_medium_at)
    out->sev_major_at = out->sev_medium_at + 1;

  if(out->sev_crit_at <= out->sev_major_at)
    out->sev_crit_at = out->sev_major_at + 1;

  // Flavour authorship. kv_get_str returns NULL for an unset key, which
  // is the shipped state of the model key and simply means "off".
  model  = kv_get_str(MELEE_KV_LLM_MODEL);
  prompt = kv_get_str(MELEE_KV_LLM_PROMPT);

  snprintf(out->llm_model,  sizeof(out->llm_model),  "%s",
           model  != NULL ? model  : "");
  snprintf(out->llm_prompt, sizeof(out->llm_prompt), "%s",
           prompt != NULL ? prompt : "");

  out->llm_pool       = melee_clamp(kv_get_uint(MELEE_KV_LLM_POOL),    4,
                                    MELEE_LLM_POOL_MAX);
  out->llm_temp_pct   = melee_clamp(kv_get_uint(MELEE_KV_LLM_TEMP),    0, 200);
  out->llm_max_tokens = melee_clamp(kv_get_uint(MELEE_KV_LLM_TOKENS), 256, 8192);
  out->llm_timeout    = melee_clamp(kv_get_uint(MELEE_KV_LLM_TIMEOUT), 10, 300);
  out->llm_retry      = melee_clamp(kv_get_uint(MELEE_KV_LLM_RETRY),   30, 86400);

  // After llm_pool, and strictly below it: a refill_at at or above the
  // pool size would re-arm a refill the instant one completed, forever.
  out->llm_refill_at  = melee_clamp(kv_get_uint(MELEE_KV_LLM_REFILL), 1,
                                    out->llm_pool - 1);
}

// ------------------------------------------------------------------ //
// Lifecycle                                                           //
// ------------------------------------------------------------------ //

static bool
melee_init(void)
{
  if(melee_commands_register() != SUCCESS)
  {
    clam(CLAM_WARN, MELEE_CTX, "command registration failed");
    return(FAIL);
  }

  clam(CLAM_INFO, MELEE_CTX, "melee plugin initialized");
  return(SUCCESS);
}

// Schema bootstrap runs in start(), after kv_load(), so the table prefix
// reflects the persisted KV rather than the register-time default.
static bool
melee_start(void)
{
  melee_tunables_t t;

  if(melee_schema_ensure() != SUCCESS)
    clam(CLAM_WARN, MELEE_CTX,
        "melee schema init failed (the pit will error until fixed)");

  // Flavour authorship is opt-in and inert by default: melee_llm_prime()
  // returns immediately unless an operator has named a usable chat model
  // and the inference plugin is loaded.
  melee_tunables_load(&t);
  melee_llm_prime(&t);

  return(SUCCESS);
}

static void
melee_deinit(void)
{
  melee_commands_unregister();
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
