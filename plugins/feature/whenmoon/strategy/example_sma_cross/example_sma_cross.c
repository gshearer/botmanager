// botmanager — MIT
// Reference strategy plugin: trivial EMA crossover. Verifies the
// WM-LT-3 substrate end-to-end (load, attach, bar-close fan-out,
// signal emission, /show whenmoon strategy detail). Not a real edge
// — the cross of two bog-standard EMAs is one of the most overfit-
// prone signals in TA. WM-LT-7 will give the harness a deliberately
// overfit strategy to walk-forward against; this is that strategy.
//
// Subscribes to WM_GRAN_5M only. Reads the fast/slow EMA values out
// of the indicator block (WM_IND_EMA_12 / WM_IND_EMA_26 by default;
// the params remap if the user overrides). Tracks last signal so
// successive same-side calls don't churn the score.

#include "whenmoon_strategy.h"

#include "alloc.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define ESC_NAME       "example_sma_cross"
#define ESC_VERSION    "0.1"
#define ESC_LOG_CTX    "strategy.example_sma_cross"

// Map a user-chosen period (1..50) onto an EMA slot id. The indicator
// block exposes EMA-9/12/20/26/50; anything else falls back to the
// nearest slot. Strategies that need an arbitrary period should
// compute it themselves; we keep this trivial here to verify the
// substrate, not to provide tuning surface.
static int
esc_slot_for_period(uint32_t period)
{
  if(period <= 9)  return(WM_IND_EMA_9);
  if(period <= 12) return(WM_IND_EMA_12);
  if(period <= 20) return(WM_IND_EMA_20);
  if(period <= 26) return(WM_IND_EMA_26);

  return(WM_IND_EMA_50);
}

// Per-attachment state. Allocated in init, freed in finalize. Stashed
// on the ctx via wm_strategy_ctx_set_user.
typedef struct
{
  // Resolved params (snapshot at init; reload to pick up KV changes).
  uint32_t fast_period;
  uint32_t slow_period;
  int      fast_slot;
  int      slow_slot;

  // Cross-detection. Tracks the sign of (fast - slow) on the previous
  // bar so we can emit +1/-1 only on the actual crossing event.
  int      prev_sign;       // -1 / 0 / +1
  bool     have_prev;
} esc_state_t;

// -----------------------------------------------------------------------
// Required exports
// -----------------------------------------------------------------------

static const wm_strategy_param_t esc_params[] = {
  {
    .name        = "fast_period",
    .type        = WM_PARAM_UINT,
    .default_int = 12,
    .min_int     = 5,
    .max_int     = 50,
    .step_dbl    = 1.0,
    .help        = "Fast EMA period (5..50). Mapped to nearest"
                   " indicator slot: 9, 12, 20, 26, or 50.",
  },
  {
    .name        = "slow_period",
    .type        = WM_PARAM_UINT,
    .default_int = 26,
    .min_int     = 10,
    .max_int     = 200,
    .step_dbl    = 1.0,
    .help        = "Slow EMA period (10..200). Mapped to nearest"
                   " indicator slot: 9, 12, 20, 26, or 50.",
  },
};

void
wm_strategy_describe(wm_strategy_meta_t *out)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->abi_version = WM_STRATEGY_ABI_VERSION;

  snprintf(out->name, sizeof(out->name), "%s", ESC_NAME);
  snprintf(out->version, sizeof(out->version), "%s", ESC_VERSION);

  out->grains_mask          = (uint16_t)(1u << WM_GRAN_5M);
  out->min_history[WM_GRAN_5M] = 200;

  out->params               = esc_params;
  out->n_params             = (uint32_t)(sizeof(esc_params)
                                  / sizeof(esc_params[0]));
  out->wants_trade_callback = false;
}

int
wm_strategy_init(wm_strategy_ctx_t *ctx)
{
  esc_state_t *s;
  const char  *mid;
  const char  *strat;

  if(ctx == NULL)
    return(-1);

  s = mem_alloc("strategy." ESC_NAME, "state", sizeof(*s));

  mid   = wm_strategy_ctx_market_id(ctx);
  strat = wm_strategy_ctx_strategy_name(ctx);

  s->fast_period =
      (uint32_t)wm_strategy_kv_get_uint(mid, strat, "fast_period", 12);
  s->slow_period =
      (uint32_t)wm_strategy_kv_get_uint(mid, strat, "slow_period", 26);

  // Defensive clamp — a hand-edited KV value could fall outside the
  // declared range. The runtime accepts what was set; we bound it
  // here so the slot-mapper stays well-defined.
  if(s->fast_period < 1)  s->fast_period = 1;
  if(s->fast_period > 200) s->fast_period = 200;
  if(s->slow_period < 1)  s->slow_period = 1;
  if(s->slow_period > 200) s->slow_period = 200;

  if(s->fast_period >= s->slow_period)
  {
    clam(CLAM_WARN, ESC_LOG_CTX,
        "%s -> %s: fast_period (%u) >= slow_period (%u);"
        " behaviour will be degenerate (no crossings)",
        strat, mid, s->fast_period, s->slow_period);
  }

  s->fast_slot = esc_slot_for_period(s->fast_period);
  s->slow_slot = esc_slot_for_period(s->slow_period);
  s->prev_sign = 0;
  s->have_prev = false;

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, ESC_LOG_CTX,
      "init: %s -> %s fast=%u(slot=%d) slow=%u(slot=%d)",
      strat, mid, s->fast_period, s->fast_slot,
      s->slow_period, s->slow_slot);

  return(0);
}

void
wm_strategy_finalize(wm_strategy_ctx_t *ctx)
{
  esc_state_t *s;

  if(ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s != NULL)
  {
    mem_free(s);
    wm_strategy_ctx_set_user(ctx, NULL);
  }

  clam(CLAM_INFO, ESC_LOG_CTX,
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
  esc_state_t          *s;
  wm_strategy_signal_t  sig;
  float                 fast;
  float                 slow;
  int                   sign;

  (void)mkt;

  // Only 5m bars are subscribed via grains_mask, but keep the gate
  // here as a defensive double-check.
  if(grain != WM_GRAN_5M || bar == NULL || ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s == NULL)
    return;

  fast = bar->ind[s->fast_slot];
  slow = bar->ind[s->slow_slot];

  // Indicator slots return NaN until enough history has accumulated.
  // Skip until both fast and slow are populated.
  if(isnanf(fast) || isnanf(slow))
    return;

  if(fast > slow)       sign =  1;
  else if(fast < slow)  sign = -1;
  else                  sign =  0;

  // Emit only on a sign change. WM-LT-3 records this on the
  // attachment's last_signal slot; WM-LT-4 will turn it into a trade
  // intent.
  if(!s->have_prev)
  {
    s->prev_sign = sign;
    s->have_prev = true;
    return;
  }

  if(sign == s->prev_sign)
    return;

  memset(&sig, 0, sizeof(sig));
  sig.ts_ms      = bar->ts_close_ms;
  sig.score      = (double)sign;
  sig.confidence = 0.5;

  if(sign > 0)
    snprintf(sig.reason, sizeof(sig.reason),
        "ema%u>ema%u (cross up)",
        s->fast_period, s->slow_period);
  else if(sign < 0)
    snprintf(sig.reason, sizeof(sig.reason),
        "ema%u<ema%u (cross down)",
        s->fast_period, s->slow_period);
  else
    snprintf(sig.reason, sizeof(sig.reason), "neutral");

  wm_strategy_emit_signal(ctx, &sig);

  clam(CLAM_INFO, ESC_LOG_CTX,
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
//
// The descriptor's init/start/stop/deinit run as part of the core
// plugin loader's lifecycle. Whenmoon's strategy registry scans these
// after init_all and registers the strategy via wm_strategy_describe.

static bool
esc_plugin_init(void)
{
  clam(CLAM_INFO, ESC_LOG_CTX, "%s v%s loaded", ESC_NAME, ESC_VERSION);
  return(false);
}

static void
esc_plugin_deinit(void)
{
  clam(CLAM_INFO, ESC_LOG_CTX, "%s deinit", ESC_NAME);
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "strategy_" ESC_NAME,
  .version         = ESC_VERSION,
  .type            = PLUGIN_STRATEGY,
  .kind            = ESC_NAME,
  .provides        = { { .name = "strategy_" ESC_NAME } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_whenmoon" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = esc_plugin_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = esc_plugin_deinit,
  .ext             = NULL,
};
