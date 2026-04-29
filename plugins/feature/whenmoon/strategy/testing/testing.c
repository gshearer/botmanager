// botmanager — MIT
// "testing" strategy — long-only SMA-7 / SMA-25 crossover on the 1m
// chart. Demonstrates the post-PT-3 strategy ABI: the strategy receives
// a const whenmoon_market * and indexes the per-grain bar ring directly,
// reading indicators precomputed by the aggregator off bar->ind[].
//
// Long-only semantics:
//   fast > slow on a fresh up-cross  -> emit score=+1 (sizer goes long)
//   fast < slow on a fresh down-cross -> emit score=0  (sizer drives
//                                       target_position to 0; closes
//                                       any open long, never goes
//                                       short — score >= 0 always)
// The first on_bar after attach silently seeds prev_sign and returns.

#include "whenmoon_strategy.h"
#include "market.h"

#include "alloc.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TST_NAME       "testing"
#define TST_VERSION    "0.2"
#define TST_LOG_CTX    "strategy.testing"

// Mirror the slot's period — bumping these requires either picking a
// different slot or adding new ones in whenmoon_strategy.h.
#define TST_FAST_PERIOD   7
#define TST_SLOW_PERIOD  25

// Per-attachment state — just the cross-detection memory.
typedef struct
{
  int   prev_sign;     // -1 / 0 / +1
  bool  have_prev;
} tst_state_t;

// -----------------------------------------------------------------------
// Required exports
// -----------------------------------------------------------------------

void
wm_strategy_describe(wm_strategy_meta_t *out)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->abi_version = WM_STRATEGY_ABI_VERSION;

  snprintf(out->name, sizeof(out->name), "%s", TST_NAME);
  snprintf(out->version, sizeof(out->version), "%s", TST_VERSION);

  out->grains_mask             = (uint16_t)(1u << WM_GRAN_1M);
  out->min_history[WM_GRAN_1M] = TST_SLOW_PERIOD;

  out->params               = NULL;
  out->n_params             = 0;
  out->wants_trade_callback = false;
}

int
wm_strategy_init(wm_strategy_ctx_t *ctx)
{
  tst_state_t *s;

  if(ctx == NULL)
    return(-1);

  s = mem_alloc("strategy." TST_NAME, "state", sizeof(*s));

  if(s == NULL)
    return(-1);

  s->prev_sign = 0;
  s->have_prev = false;

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, TST_LOG_CTX,
      "init: %s -> %s sma%d/sma%d (1m)",
      wm_strategy_ctx_strategy_name(ctx),
      wm_strategy_ctx_market_id(ctx),
      TST_FAST_PERIOD, TST_SLOW_PERIOD);

  return(0);
}

void
wm_strategy_finalize(wm_strategy_ctx_t *ctx)
{
  tst_state_t *s;

  if(ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s != NULL)
  {
    mem_free(s);
    wm_strategy_ctx_set_user(ctx, NULL);
  }

  clam(CLAM_INFO, TST_LOG_CTX,
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
  tst_state_t          *s;
  wm_strategy_signal_t  sig;
  float                 fast;
  float                 slow;
  int                   sign;

  // Subscribed only to 1m via grains_mask; defensive double-check.
  if(grain != WM_GRAN_1M || bar == NULL || ctx == NULL || mkt == NULL)
    return;

  (void)mkt;   // future strategies index mkt->grain_arr[g][i] for
               // multi-bar lookbacks; this trivial cross uses only the
               // just-closed bar's indicator block.

  s = wm_strategy_ctx_get_user(ctx);

  if(s == NULL)
    return;

  fast = bar->ind[WM_IND_SMA_7];
  slow = bar->ind[WM_IND_SMA_25];

  // Both populated only after slow_period closes have accumulated.
  if(isnanf(fast) || isnanf(slow))
    return;

  if(fast > slow)       sign =  1;
  else if(fast < slow)  sign = -1;
  else                  sign =  0;

  if(!s->have_prev)
  {
    s->prev_sign = sign;
    s->have_prev = true;
    return;
  }

  if(sign == s->prev_sign)
    return;

  // Long-only emission. Cross-up -> +1 (open / extend long), cross-down
  // -> 0 (sizer brings target_position to 0, closing any open long).
  memset(&sig, 0, sizeof(sig));
  sig.ts_ms      = bar->ts_close_ms;
  sig.confidence = 0.5;

  if(sign > s->prev_sign)
  {
    sig.score = 1.0;
    snprintf(sig.reason, sizeof(sig.reason),
        "sma%d>sma%d (cross up)",
        TST_FAST_PERIOD, TST_SLOW_PERIOD);
  }
  else
  {
    sig.score = 0.0;
    snprintf(sig.reason, sizeof(sig.reason),
        "sma%d<sma%d (cross down -> close)",
        TST_FAST_PERIOD, TST_SLOW_PERIOD);
  }

  wm_strategy_emit_signal(ctx, &sig);

  clam(CLAM_INFO, TST_LOG_CTX,
      "%s -> %s: %s @ ts=%lld fast=%.6g slow=%.6g",
      wm_strategy_ctx_strategy_name(ctx),
      wm_strategy_ctx_market_id(ctx),
      sig.reason, (long long)bar->ts_close_ms,
      (double)fast, (double)slow);

  s->prev_sign = sign;
}

// -----------------------------------------------------------------------
// Plugin lifecycle
// -----------------------------------------------------------------------

static bool
tst_plugin_init(void)
{
  clam(CLAM_INFO, TST_LOG_CTX, "%s v%s loaded", TST_NAME, TST_VERSION);
  return(false);
}

static void
tst_plugin_deinit(void)
{
  clam(CLAM_INFO, TST_LOG_CTX, "%s deinit", TST_NAME);
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "strategy_" TST_NAME,
  .version         = TST_VERSION,
  .type            = PLUGIN_STRATEGY,
  .kind            = TST_NAME,
  .provides        = { { .name = "strategy_" TST_NAME } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_whenmoon" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = tst_plugin_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = tst_plugin_deinit,
  .ext             = NULL,
};
