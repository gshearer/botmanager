// botmanager — MIT
// cp1 ("Claude's Play #1") — long-only Fisher Transform reversal.
//
// First real whenmoon strategy. Trades the 5m grain. The signal is a
// mean-reversion bounce off an oversold extreme:
//
//   arm   : Fisher Transform dips to <= arm_level (default -3, deeply
//           oversold). The arm latches until an entry fires.
//   enter : while armed, Fisher crosses UP through entry_level
//           (default 0 — "flips to positive"). Emit score=+1 (open
//           long) and disarm.
//   exit  : while long, Fisher crosses DOWN through exit_level
//           (default 0). Emit score=-1 (close the long).
//
// Long-only by construction: the engine maps score>0 -> open long,
// score<0 -> close the open long, and a close-when-flat is a guarded
// no-op, so a -1 exit never opens a short. arm_level / entry_level /
// exit_level are sweepable params so the profit/loss tradeoff can be
// tuned in the backtest. Fisher itself is precomputed by the
// aggregator (WM_IND_FISHER, period 9); cp1 only reads the slot and
// tracks the crossing state.

#include "whenmoon_strategy.h"

#include "alloc.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CP1_NAME       "cp1"
#define CP1_VERSION    "0.1"
#define CP1_LOG_CTX    "strategy.cp1"

// 5m bars to fetch on attach so Fisher is warm at live cold start.
// Fisher(9) converges well inside this; the backtest snapshot carries
// far more history regardless.
#define CP1_MIN_HISTORY_5M   100u

// Defaults — mirrored in the param schema below.
#define CP1_DEFAULT_ARM_LEVEL     (-3.0)
#define CP1_DEFAULT_ENTRY_LEVEL    (0.0)
#define CP1_DEFAULT_EXIT_LEVEL     (0.0)

typedef struct
{
  // Resolved params (snapshot at init; reload to pick up KV changes).
  double  arm_level;       // <= this arms a long (oversold)
  double  entry_level;     // armed up-cross through this opens long
  double  exit_level;      // down-cross through this closes long

  // Crossing-detection state.
  double  prev_fisher;     // previous bar's fisher
  bool    have_prev;       // prev_fisher seeded yet?
  bool    armed;           // seen fisher <= arm_level since last entry
  bool    in_position;     // strategy's view of long/flat (edge-only emit)
} cp1_state_t;

// ----------------------------------------------------------------------- //
// Required exports                                                        //
// ----------------------------------------------------------------------- //

static const wm_strategy_param_t cp1_params[] = {
  {
    .name        = "arm_level",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = CP1_DEFAULT_ARM_LEVEL,
    .min_dbl     = -6.0,
    .max_dbl     = -1.0,
    .step_dbl    = 0.5,
    .help        = "Fisher must dip to <= this to arm a long"
                   " (oversold extreme). Default -3.",
  },
  {
    .name        = "entry_level",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = CP1_DEFAULT_ENTRY_LEVEL,
    .min_dbl     = -1.0,
    .max_dbl     = 1.0,
    .step_dbl    = 0.25,
    .help        = "Once armed, go long when Fisher crosses UP through"
                   " this ('flips to positive'). Default 0.",
  },
  {
    .name        = "exit_level",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = CP1_DEFAULT_EXIT_LEVEL,
    .min_dbl     = -2.0,
    .max_dbl     = 2.0,
    .step_dbl    = 0.25,
    .help        = "Close the long when Fisher crosses DOWN through"
                   " this. Default 0.",
  },
};

void
wm_strategy_describe(wm_strategy_meta_t *out)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->abi_version = WM_STRATEGY_ABI_VERSION;

  snprintf(out->name, sizeof(out->name), "%s", CP1_NAME);
  snprintf(out->version, sizeof(out->version), "%s", CP1_VERSION);

  out->grains_mask             = (uint16_t)(1u << WM_GRAN_5M);
  out->min_history[WM_GRAN_5M] = CP1_MIN_HISTORY_5M;

  out->params               = cp1_params;
  out->n_params             = (uint32_t)(sizeof(cp1_params)
                                  / sizeof(cp1_params[0]));
  out->wants_trade_callback = false;
}

int
wm_strategy_init(wm_strategy_ctx_t *ctx)
{
  cp1_state_t *s;
  const char  *mid;
  const char  *strat;

  if(ctx == NULL)
    return(-1);

  s = mem_alloc("strategy." CP1_NAME, "state", sizeof(*s));

  mid   = wm_strategy_ctx_market_id(ctx);
  strat = wm_strategy_ctx_strategy_name(ctx);

  s->arm_level =
      wm_strategy_kv_get_dbl(mid, strat, "arm_level", CP1_DEFAULT_ARM_LEVEL);
  s->entry_level =
      wm_strategy_kv_get_dbl(mid, strat, "entry_level", CP1_DEFAULT_ENTRY_LEVEL);
  s->exit_level =
      wm_strategy_kv_get_dbl(mid, strat, "exit_level", CP1_DEFAULT_EXIT_LEVEL);

  s->prev_fisher = 0.0;
  s->have_prev   = false;
  s->armed       = false;
  s->in_position = false;

  // Sanity: the arm extreme must sit below the entry trigger or the
  // "dip then flip up" geometry is degenerate (we would arm at or above
  // the level we wait to cross). Accept the KV values but warn.
  if(s->arm_level >= s->entry_level)
    clam(CLAM_WARN, CP1_LOG_CTX,
        "%s -> %s: arm_level (%.2f) >= entry_level (%.2f);"
        " entries will not fire as intended",
        strat, mid, s->arm_level, s->entry_level);

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, CP1_LOG_CTX,
      "init: %s -> %s arm<=%.2f entry-up>%.2f exit-dn<%.2f (5m)",
      strat, mid, s->arm_level, s->entry_level, s->exit_level);

  return(0);
}

void
wm_strategy_finalize(wm_strategy_ctx_t *ctx)
{
  cp1_state_t *s;

  if(ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s != NULL)
  {
    mem_free(s);
    wm_strategy_ctx_set_user(ctx, NULL);
  }

  clam(CLAM_INFO, CP1_LOG_CTX,
      "finalize: %s -> %s",
      wm_strategy_ctx_strategy_name(ctx),
      wm_strategy_ctx_market_id(ctx));
}

void
wm_strategy_on_bar(wm_strategy_ctx_t *ctx,
    const struct whenmoon_market *mkt,
    wm_gran_t grain,
    const wm_candle_full_t *bar)
{
  cp1_state_t          *s;
  wm_strategy_signal_t  sig;
  float                 fisher;
  bool                  fire = false;

  (void)mkt;   // cp1 reads only the just-closed bar's fisher slot.

  // Subscribed to 5m only via grains_mask; defensive double-check.
  if(grain != WM_GRAN_5M || bar == NULL || ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s == NULL)
    return;

  fisher = bar->ind[WM_IND_FISHER];

  // NaN until the aggregator has enough history to compute Fisher.
  if(isnanf(fisher))
    return;

  // Seed prev on the first valid bar so the first crossing test has a
  // baseline; never fires on the seeding bar.
  if(!s->have_prev)
  {
    s->prev_fisher = fisher;
    s->have_prev   = true;
    return;
  }

  // Arm whenever Fisher reaches the oversold extreme. The arm latches
  // until an entry consumes it.
  if(fisher <= s->arm_level)
    s->armed = true;

  memset(&sig, 0, sizeof(sig));
  sig.ts_ms = bar->ts_close_ms;

  if(!s->in_position)
  {
    // Entry: armed AND Fisher crossed UP through entry_level this bar.
    if(s->armed &&
       s->prev_fisher <= s->entry_level && fisher > s->entry_level)
    {
      sig.score      = 1.0;
      sig.confidence = 0.6;
      snprintf(sig.reason, sizeof(sig.reason),
          "armed<=%.1f up-cross>%.1f", s->arm_level, s->entry_level);

      s->in_position = true;
      s->armed       = false;
      fire           = true;
    }
  }

  else
  {
    // Exit: Fisher crossed DOWN through exit_level this bar.
    if(s->prev_fisher >= s->exit_level && fisher < s->exit_level)
    {
      sig.score      = -1.0;
      sig.confidence = 0.5;
      snprintf(sig.reason, sizeof(sig.reason),
          "fisher down-cross<%.1f (close)", s->exit_level);

      s->in_position = false;
      fire           = true;
    }
  }

  if(fire)
  {
    wm_strategy_emit_signal(ctx, &sig);

    clam(CLAM_INFO, CP1_LOG_CTX,
        "%s -> %s: %s @ ts=%lld fisher=%.4f",
        wm_strategy_ctx_strategy_name(ctx),
        wm_strategy_ctx_market_id(ctx),
        sig.reason, (long long)bar->ts_close_ms, (double)fisher);
  }

  s->prev_fisher = fisher;
}

// ----------------------------------------------------------------------- //
// Plugin lifecycle                                                        //
// ----------------------------------------------------------------------- //

static bool
cp1_plugin_init(void)
{
  clam(CLAM_INFO, CP1_LOG_CTX, "%s v%s loaded", CP1_NAME, CP1_VERSION);
  return(false);
}

static void
cp1_plugin_deinit(void)
{
  clam(CLAM_INFO, CP1_LOG_CTX, "%s deinit", CP1_NAME);
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "strategy_" CP1_NAME,
  .version         = CP1_VERSION,
  .type            = PLUGIN_STRATEGY,
  .kind            = CP1_NAME,
  .provides        = { { .name = "strategy_" CP1_NAME } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_whenmoon" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = cp1_plugin_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = cp1_plugin_deinit,
  .ext             = NULL,
};
