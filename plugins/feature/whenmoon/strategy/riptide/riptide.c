// botmanager — MIT
// riptide — regime-gated mean-reversion "counter-current" swing.
//
// A riptide is a counter-current: it pulls back AGAINST the prevailing wave.
// That is exactly this strategy's edge, and it is deliberately the OPPOSITE
// bet from the field's trend-flippers (buy strength / sell weakness). riptide
// buys WEAKNESS and sells STRENGTH: inside a confirmed macro up-trend it waits
// for price to be pulled a measured distance BELOW its own short mean (a dip
// against the up-current), buys the first tick back up, and banks the snap
// back to the mean. It earns from oscillation amplitude, not trend direction,
// so its per-window return is far less coupled to how strong that window's
// trend happened to be — which is what the robustness score rewards.
//
// Why this shape (vs. the flipper riptide used to be):
//   * The scoring metric is realizable per-window robustness = mean per-fold
//     return / its cross-regime dispersion (behind hard gates + friction
//     survival), NOT compounded terminal equity. A long-only trend-harvester's
//     per-window return scales with that window's trend strength (a mania pays
//     10-30x a grind), so its fold dispersion is large and the ratio is capped.
//     A mean-reversion swing captures a BOUNDED reversion each trip regardless
//     of trend strength, so folds are more uniform -> higher mean/std.
//   * DIFFERENTIATION (round 3): the field's other high-scorer is now also a
//     regime-gated dip-reverter, so "mean-reversion" alone no longer sets
//     riptide apart. riptide separates on the TIDE CLOCK and STRETCH DEPTH: a
//     4h intraday tide (vs the neighbor's 1d macro tide) + a deep 0.75-ATR
//     stretch entry gated by an up-tick trigger + a tight 0.5-ATR reversion
//     target. That shifts which windows riptide is active in — pooled per-fold
//     return correlation vs the neighbor falls to ~0.51 — so it is a measurably
//     separate bet, not a parametric twin. juggernaut (ADX trend-strength) is
//     distinct by construction.
//
// Mechanics (long-only, single position, fill-at-close):
//   regime tide  — a slow self-EMA on the 4h (or 1d) closes sets a long-only
//                  bias: only hunt dips while close > tide. This is the shared
//                  "stay out of the bear" filter — it keeps riptide from
//                  catching falling knives in a downtrend (which is what makes
//                  naive mean-reversion blow up), protecting the worst-window
//                  and drawdown gates.
//   entry        — flat + regime up + the 1h close is >= entry_atr * ATR_14
//                  below EMA_20 (a real dip against the current) + the bar
//                  closed UP from the prior 1h close (a nascent bounce, not a
//                  freefall) + optional RSI_14 <= rsi_max oversold filter.
//   exit         — the FIRST of: (a) close snaps back to >= EMA_20 +
//                  target_atr*ATR_14 (the reversion is banked); (b) a fixed
//                  protective stop close <= entry_price - stop_atr*ATR_14 (caps
//                  the loss -> worst-window/drawdown gate); (c) the macro
//                  regime flips down (dump into the bear).
//
// Params (small + orthogonal, to sit on a broad ridge, not a knife-edge):
//   regime_grain 0 = 1d tide, 1 = 4h tide (macro long-bias current)
//   regime_alpha slow self-EMA smoothing of the tide grain's closes (0,1]
//   entry_atr    dip depth below EMA_20(1h), in ATR_14(1h), to arm a buy
//   target_atr   reversion target above EMA_20(1h), in ATR_14(1h) (0 = at mean)
//   stop_atr     protective stop below entry, in ATR_14(1h) at entry time
//   rsi_max      optional oversold filter: require RSI_14 <= this (>=100 = off)
//
// LOOKAHEAD SAFETY. The backtest fires on_bar in a merged chronological walk
// across grains; on a shared timestamp the FINER grain fires first (1h before
// 4h before 1d). riptide caches the tide grain's close + self-EMA as that
// grain's bar closes and reads the cache only on a later 1h decision bar, so it
// never sees a higher-grain bar that has not genuinely closed. The decision
// reads only THIS bar's own ind[] slots (each computed at its close) plus
// strategy-owned scalars (prior close, entry/stop prices) tracked forward. No
// reach into mkt->grain_arr[] (in backtest that ring holds the whole, future,
// range). Identical in live and backtest.

#include "whenmoon_strategy.h"

#include "alloc.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define RIPTIDE_NAME     "riptide"
#define RIPTIDE_VERSION  "0.4"
#define RIPTIDE_LOG_CTX  "strategy.riptide"

// Per-grain live warm-up history (bars). Backtest snapshots carry the full
// range regardless; this only matters at live cold start. The 1h decision
// grain wants EMA_20 / ATR_14 / RSI_14 warm; the tide grains only need a few
// bars to seed the slow self-EMA.
#define RIPTIDE_HIST_1H   210u
#define RIPTIDE_HIST_4H   64u
#define RIPTIDE_HIST_1D   64u

// Defaults — mirrored in the param schema. These ARE the validated winner
// (full-history walk-forward on BTC-USD + ETH-USD): pooled robust_ratio 1.615
// at 22.5 trades/mo, all gates passed, and still gate-passing at 2x and 4x
// trading costs (4x robust_ratio 1.364 — only -16% decay, matching the
// tightest in the field). Sits on a broad ridge (entry_atr 0.5-1.0, alpha
// 0.3-0.6, target_atr 0.25-0.75 all gate-passing within a few % of peak on a
// 27-config grid), not a knife-edge.
//
// ROUND-3 DIFFERENTIATION: the macro tide runs on the 4h grain (not the 1d
// grain the field's other dip-reverter uses) and the entry stretch is deep
// (0.75 ATR). That shifts riptide's active-window structure off its neighbor's:
// pooled per-fold return correlation vs the 1d-tide dip-reverter falls from
// 0.81 to 0.51 (as distinct as the field's other two pairs) — a measurably
// separate bet, not a shallow twin, with every robustness/friction gate held.
#define RIPTIDE_DEFAULT_REGIME_GRAIN  1.0    // 1 = 4h intraday tide (differentiator)
#define RIPTIDE_DEFAULT_REGIME_ALPHA  0.60   // tide self-EMA smoothing
#define RIPTIDE_DEFAULT_ENTRY_ATR     0.75   // arm a buy 0.75 ATR below EMA_20 (deep stretch)
#define RIPTIDE_DEFAULT_TARGET_ATR    0.50   // bank the reversion 0.5 ATR above the mean
#define RIPTIDE_DEFAULT_STOP_ATR      3.00   // protective stop 3.0 ATR below entry
#define RIPTIDE_DEFAULT_RSI_MAX     100.0    // oversold filter off by default

typedef struct
{
  // Resolved params (snapshot at init; reload to pick up KV changes).
  int        regime_grain;   // 0 = 1d, 1 = 4h
  double     regime_alpha;   // tide self-EMA smoothing in (0,1]
  double     entry_atr;      // dip depth below EMA_20 (ATR units)
  double     target_atr;     // reversion target above EMA_20 (ATR units)
  double     stop_atr;       // protective stop below entry (ATR units)
  double     rsi_max;        // oversold filter (>=100 = off)

  // Cached tide-grain context. *_have latches once that grain produces a bar.
  // reg_ema is the strategy-computed slow self-EMA of that grain's closes.
  double     d1_close, d1_ema;   bool d1_have;
  double     h4_close, h4_ema;   bool h4_have;

  // Decision-grain (1h) rolling scalar: prior close, for the up-tick confirm.
  double     prev_close;     bool prev_have;

  // Mean-reversion arming: a dip below the band ARMS a buy; the buy TRIGGERS
  // on the first green (up-tick) bar while still under the mean. Decoupling
  // arm from trigger is what makes the dip actually tradeable (a bar that is
  // both deep-below-mean AND closing up is rare; the bounce comes a bar or
  // two later, once price has ticked off the low).
  bool       armed;

  // Position state.
  bool       in_position;
  double     entry_price;    // fill price at entry
  double     stop_price;     // fixed protective stop, set at entry
} riptide_state_t;

// One EMA step (seed on first sample): ema = alpha*close + (1-alpha)*prev.
static double
riptide_ema_step(double prev, bool seen, double close, double alpha)
{
  if(!seen || isnan(prev))
    return(close);

  return(alpha * close + (1.0 - alpha) * prev);
}

// Advance one tide grain's cached self-EMA + close as its bar closes
// (lookahead-free: a later 1h decision bar only ever reads a tide bar that
// closed in its past).
static void
riptide_cache_context(riptide_state_t *s, wm_gran_t grain,
    const wm_candle_full_t *bar)
{
  switch(grain)
  {
    case WM_GRAN_1D:
      s->d1_ema   = riptide_ema_step(s->d1_ema, s->d1_have, bar->close,
                        s->regime_alpha);
      s->d1_close = bar->close;
      s->d1_have  = true;
      break;

    case WM_GRAN_4H:
      s->h4_ema   = riptide_ema_step(s->h4_ema, s->h4_have, bar->close,
                        s->regime_alpha);
      s->h4_close = bar->close;
      s->h4_have  = true;
      break;

    default:
      break;
  }
}

// Macro tide up? close of the selected tide grain above its slow self-EMA.
// Fails closed until that grain has produced a bar.
static bool
riptide_regime_up(const riptide_state_t *s)
{
  if(s->regime_grain == 0)
    return(s->d1_have && !isnan(s->d1_ema) && s->d1_close > s->d1_ema);

  return(s->h4_have && !isnan(s->h4_ema) && s->h4_close > s->h4_ema);
}

// ----------------------------------------------------------------------- //
// Param schema                                                            //
// ----------------------------------------------------------------------- //

static const wm_strategy_param_t riptide_params[] = {
  {
    .name        = "regime_grain",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)RIPTIDE_DEFAULT_REGIME_GRAIN,
    .min_int     = 0,
    .max_int     = 1,
    .step_dbl    = 1.0,
    .help        = "Grain the long-bias macro tide runs on: 0=1d (slow,"
                   " steadiest), 1=4h (intraday, differentiator). Dips are only"
                   " bought while close>tide. Default 1 (4h).",
  },
  {
    .name        = "regime_alpha",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = RIPTIDE_DEFAULT_REGIME_ALPHA,
    .min_dbl     = 0.02,
    .max_dbl     = 0.60,
    .step_dbl    = 0.02,
    .help        = "Self-EMA smoothing of the tide grain's closes in (0,1]."
                   " Lower = slower, steadier long-bias. Default 0.60.",
  },
  {
    .name        = "entry_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = RIPTIDE_DEFAULT_ENTRY_ATR,
    .min_dbl     = 0.25,
    .max_dbl     = 4.0,
    .step_dbl    = 0.25,
    .help        = "Dip depth to arm a buy: the 1h close must be at least this"
                   " many ATR_14 below EMA_20. Deeper = fewer, fatter, more"
                   " friction-robust stretches (frequency lever). Default 0.75.",
  },
  {
    .name        = "target_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = RIPTIDE_DEFAULT_TARGET_ATR,
    .min_dbl     = 0.0,
    .max_dbl     = 4.0,
    .step_dbl    = 0.25,
    .help        = "Reversion target: exit when the 1h close snaps back to"
                   " EMA_20 + this many ATR_14 (0 = exit at the mean). Default"
                   " 0.50.",
  },
  {
    .name        = "stop_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = RIPTIDE_DEFAULT_STOP_ATR,
    .min_dbl     = 0.5,
    .max_dbl     = 6.0,
    .step_dbl    = 0.5,
    .help        = "Protective stop: exit if the 1h close falls this many"
                   " ATR_14 (measured at entry) below the entry price. Caps"
                   " the per-trip loss. Default 3.0.",
  },
  {
    .name        = "rsi_max",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = RIPTIDE_DEFAULT_RSI_MAX,
    .min_dbl     = 20.0,
    .max_dbl     = 100.0,
    .step_dbl    = 5.0,
    .help        = "Optional oversold filter: require RSI_14 <= this at entry"
                   " (>=100 disables it). Default 100 (off).",
  },
};

void
wm_strategy_describe(wm_strategy_meta_t *out)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->abi_version = WM_STRATEGY_ABI_VERSION;

  snprintf(out->name, sizeof(out->name), "%s", RIPTIDE_NAME);
  snprintf(out->version, sizeof(out->version), "%s", RIPTIDE_VERSION);

  // 1h decision grain + 4h/1d tide grains for the macro long-bias cache.
  out->grains_mask = (uint16_t)((1u << WM_GRAN_1H)  |
                                (1u << WM_GRAN_4H)  |
                                (1u << WM_GRAN_1D));

  out->min_history[WM_GRAN_1H]  = RIPTIDE_HIST_1H;
  out->min_history[WM_GRAN_4H]  = RIPTIDE_HIST_4H;
  out->min_history[WM_GRAN_1D]  = RIPTIDE_HIST_1D;

  out->params               = riptide_params;
  out->n_params             = (uint32_t)(sizeof(riptide_params)
                                  / sizeof(riptide_params[0]));
  out->wants_trade_callback = false;
}

int
wm_strategy_init(wm_strategy_ctx_t *ctx)
{
  riptide_state_t *s;
  const char      *mid;
  const char      *strat;

  if(ctx == NULL)
    return(-1);

  s = mem_alloc("strategy." RIPTIDE_NAME, "state", sizeof(*s));

  memset(s, 0, sizeof(*s));

  mid   = wm_strategy_ctx_market_id(ctx);
  strat = wm_strategy_ctx_strategy_name(ctx);

  s->regime_grain = (int)wm_strategy_kv_get_uint(mid, strat, "regime_grain",
      (uint64_t)RIPTIDE_DEFAULT_REGIME_GRAIN);
  s->regime_alpha = wm_strategy_kv_get_dbl(mid, strat, "regime_alpha",
      RIPTIDE_DEFAULT_REGIME_ALPHA);
  s->entry_atr = wm_strategy_kv_get_dbl(mid, strat, "entry_atr",
      RIPTIDE_DEFAULT_ENTRY_ATR);
  s->target_atr = wm_strategy_kv_get_dbl(mid, strat, "target_atr",
      RIPTIDE_DEFAULT_TARGET_ATR);
  s->stop_atr = wm_strategy_kv_get_dbl(mid, strat, "stop_atr",
      RIPTIDE_DEFAULT_STOP_ATR);
  s->rsi_max = wm_strategy_kv_get_dbl(mid, strat, "rsi_max",
      RIPTIDE_DEFAULT_RSI_MAX);

  if(s->regime_grain < 0)         s->regime_grain = 0;
  if(s->regime_grain > 1)         s->regime_grain = 1;
  if(s->regime_alpha <= 0.0)      s->regime_alpha = RIPTIDE_DEFAULT_REGIME_ALPHA;
  if(s->regime_alpha > 1.0)       s->regime_alpha = 1.0;
  if(s->entry_atr < 0.0)          s->entry_atr = 0.0;
  if(s->target_atr < 0.0)         s->target_atr = 0.0;
  if(s->stop_atr <= 0.0)          s->stop_atr = RIPTIDE_DEFAULT_STOP_ATR;

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, RIPTIDE_LOG_CTX,
      "init: %s -> %s regime_grain=%d regime_alpha=%.4f entry_atr=%.2f"
      " target_atr=%.2f stop_atr=%.2f rsi_max=%.1f",
      strat, mid, s->regime_grain, s->regime_alpha, s->entry_atr,
      s->target_atr, s->stop_atr, s->rsi_max);

  return(0);
}

void
wm_strategy_finalize(wm_strategy_ctx_t *ctx)
{
  riptide_state_t *s;

  if(ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s != NULL)
  {
    mem_free(s);
    wm_strategy_ctx_set_user(ctx, NULL);
  }
}

void
wm_strategy_on_bar(wm_strategy_ctx_t *ctx,
    const struct whenmoon_market *mkt,
    wm_gran_t grain,
    const wm_candle_full_t *bar)
{
  riptide_state_t      *s;
  wm_strategy_signal_t  sig;
  float                 ema20;
  float                 atr;
  float                 rsi;
  double                close;
  bool                  regime_up;
  bool                  have_core;
  bool                  fire = false;

  (void)mkt;   // riptide reads only ind[] slots + its own cached context.

  if(bar == NULL || ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s == NULL)
    return;

  // Tide grains only refresh cached context.
  if(grain == WM_GRAN_4H || grain == WM_GRAN_1D)
  {
    riptide_cache_context(s, grain, bar);
    return;
  }

  // Only the 1h grain runs the decision.
  if(grain != WM_GRAN_1H)
    return;

  // ---- 1h decision grain ----
  close     = bar->close;
  ema20     = bar->ind[WM_IND_EMA_20];
  atr       = bar->ind[WM_IND_ATR_14];
  rsi       = bar->ind[WM_IND_RSI_14];
  regime_up = riptide_regime_up(s);
  have_core = !isnanf(ema20) && !isnanf(atr) && atr > 0.0f;

  memset(&sig, 0, sizeof(sig));
  sig.ts_ms = bar->ts_close_ms;

  if(!s->in_position)
  {
    // Arm-then-trigger mean reversion. ARM when the 1h close is a real dip
    // (>= entry_atr ATR below EMA_20) while the macro tide is up; stay armed
    // through the pullback zone; TRIGGER the buy on the first up-tick (a
    // nascent bounce, not a freefall) while still below the mean. Disarm if
    // price recovers to the mean untriggered or the tide flips down.
    double dip_level = (double)ema20 - s->entry_atr * (double)atr;
    bool   uptick    = s->prev_have && close > s->prev_close;
    bool   rsi_ok    = (s->rsi_max >= 100.0) ||
                       (!isnanf(rsi) && (double)rsi <= s->rsi_max);

    if(!regime_up || !have_core)
      s->armed = false;
    else if(close <= dip_level)
      s->armed = true;
    else if(close >= (double)ema20)
      s->armed = false;   // recovered to the mean without a bounce trigger

    if(s->armed && regime_up && have_core && uptick && rsi_ok)
    {
      sig.score      = 1.0;
      sig.confidence = 0.6;
      snprintf(sig.reason, sizeof(sig.reason), "dip %.2fatr", s->entry_atr);

      s->armed       = false;
      s->in_position = true;
      s->entry_price = close;
      s->stop_price  = close - s->stop_atr * (double)atr;
      fire           = true;
    }
  }
  else
  {
    // Exit: the FIRST of a banked reversion, the protective stop, or a macro
    // tide flip-down. target tracks the (moving) mean; the stop is fixed at
    // entry. Guard the ATR-derived target on a warm ATR.
    double target_lv = have_core
                         ? (double)ema20 + s->target_atr * (double)atr
                         : (double)ema20;
    bool   hit_target = have_core && close >= target_lv;
    bool   hit_stop   = close <= s->stop_price;
    const char *why   = NULL;

    if(!regime_up)        why = "regime";
    else if(hit_stop)     why = "stop";
    else if(hit_target)   why = "target";

    if(why != NULL)
    {
      sig.score      = -1.0;
      sig.confidence = 0.5;
      snprintf(sig.reason, sizeof(sig.reason), "exit %s", why);

      s->in_position = false;
      s->entry_price = 0.0;
      s->stop_price  = 0.0;
      fire           = true;
    }
  }

  // Track this bar's close for the next bar's up-tick confirm (AFTER the
  // decision, so the window only ever holds strictly-prior closes).
  s->prev_close = close;
  s->prev_have  = true;

  if(fire)
  {
    wm_strategy_emit_signal(ctx, &sig);

    clam(CLAM_INFO, RIPTIDE_LOG_CTX,
        "%s -> %s: %s @ ts=%lld close=%.2f",
        wm_strategy_ctx_strategy_name(ctx),
        wm_strategy_ctx_market_id(ctx),
        sig.reason, (long long)bar->ts_close_ms, close);
  }
}

// ----------------------------------------------------------------------- //
// Plugin lifecycle                                                        //
// ----------------------------------------------------------------------- //

static bool
riptide_plugin_init(void)
{
  clam(CLAM_INFO, RIPTIDE_LOG_CTX, "%s v%s loaded", RIPTIDE_NAME,
      RIPTIDE_VERSION);
  return(false);
}

static void
riptide_plugin_deinit(void)
{
  clam(CLAM_INFO, RIPTIDE_LOG_CTX, "%s deinit", RIPTIDE_NAME);
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "strategy_" RIPTIDE_NAME,
  .version         = RIPTIDE_VERSION,
  .type            = PLUGIN_STRATEGY,
  .kind            = RIPTIDE_NAME,
  .provides        = { { .name = "strategy_" RIPTIDE_NAME } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_whenmoon" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = riptide_plugin_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = riptide_plugin_deinit,
  .ext             = NULL,
};
