// botmanager — MIT
// riptide — regime-flip compounder with a 1h entry-quality gate.
//
// The compounding engine every full-history winner converges on is the same:
// a FAST self-EMA regime on the 4h grain (long-bias while the 4h close is
// above a fast-decaying EMA of its own closes), immediate re-entry, and an
// exit the instant that regime flips down. Because the framework re-deploys
// size_frac of the GROWN cash on each entry and samples equity per fill, a
// fast regime that banks each clean up-leg and re-buys the pullback ("sell
// high, re-buy low") compounds hardest. A structural probe of the whole
// knob space (regime grain 1d/4h/1h, chandelier tightness, symmetric vs
// asymmetric self-EMA exit, dual-EMA cross) confirmed this exact point is
// the ceiling: 1d is too slow (few legs), a 1h regime is pure noise-death
// (fee-bleeds equity to ~$1), a slower OR faster exit both bleed net, and
// the chandelier is a dead knob (the regime flips first). So riptide keeps
// that proven core verbatim and adds ONE orthogonal lever the ceiling
// strategy cannot express:
//
//   entry_gate — the instant the 4h regime flips up, do we ALWAYS enter
//   (the ceiling's "immediate"), or wait for the 1h micro-context to
//   confirm the leg is real? A regime flip is a 4h event; the first 1h bars
//   after it are where whipsaw fakeouts live. Gating entry on a 1h momentum
//   confirmation DELAYS (never skips) each entry until the 1h agrees, aiming
//   to drop the specifically-bad entries — the ones that enter straight into
//   1h weakness and immediately stop out — lifting the average edge per leg.
//   The exit stays ungated and immediate (strict entry, fast exit): banking
//   is never delayed. entry_gate=0 reduces riptide EXACTLY to the ceiling
//   config, so the gate is a pure, measurable overlay on a known floor.
//
//   entry_gate values (all read only THIS 1h bar's own lookahead-free ind[]):
//     0 = none                (immediate — the ceiling floor)
//     1 = close > EMA_20       (price above the fast 1h trend)
//     2 = RSI_14 >= gate_thresh(1h momentum not weak)
//     3 = MACD_HIST > 0        (1h impulse positive)
//     4 = close > EMA_20 AND MACD_HIST > 0 (both — strictest)
//
// Params (small + orthogonal, to sit on a broad ridge, not a knife-edge):
//   alpha        fast self-EMA smoothing of the regime grain's closes (0,1]
//   regime_grain 0 = 1d tide, 1 = 4h tide (default 4h — the sweet spot)
//   entry_gate   0..4 as above (the differentiator)
//   gate_thresh  RSI level for entry_gate=2
//   chand_atr    chandelier backstop distance in ATR_14(1h); loose by
//                default (regime flip is the real exit) but available as a
//                genuine catastrophe stop if a regime is slow to flip.
//
// LOOKAHEAD SAFETY. The backtest fires on_bar in a merged chronological walk
// across grains; on a shared timestamp the FINER grain fires first (1h
// before 4h before 1d). riptide caches the regime grain's close + self-EMA
// as that grain's bar closes and reads the cache only on a later 1h decision
// bar, so it never sees a higher-grain bar that has not genuinely closed.
// The 1h decision reads only this bar's own ind[] slots (each computed at
// this bar's close) and a strategy-owned peak_high tracked forward since
// entry. No reach into mkt->grain_arr[] (in backtest that ring holds the
// whole, future, date range). Identical in live and backtest.

#include "whenmoon_strategy.h"

#include "alloc.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define RIPTIDE_NAME     "riptide"
#define RIPTIDE_VERSION  "0.1"
#define RIPTIDE_LOG_CTX  "strategy.riptide"

// Per-grain live warm-up history (bars). Backtest snapshots carry the full
// range regardless; this only matters at live cold start. 1h wants
// MACD(26+9) + EMA_20/RSI/ATR warm; the regime grains only need a few bars
// to seed the self-EMA.
#define RIPTIDE_HIST_1H  210u
#define RIPTIDE_HIST_4H  64u
#define RIPTIDE_HIST_1D  64u

// Defaults — mirrored in the param schema. entry_gate=0 reproduces the
// ceiling config exactly; sweeps decide whether a gate beats it.
#define RIPTIDE_DEFAULT_ALPHA         0.82  // fast self-EMA regime smoothing
#define RIPTIDE_DEFAULT_REGIME_GRAIN  1.0   // 1 = 4h regime (the sweet spot)
#define RIPTIDE_DEFAULT_ENTRY_GATE    0.0   // 0 = immediate (ceiling floor)
#define RIPTIDE_DEFAULT_GATE_THRESH  50.0   // RSI level for entry_gate=2
#define RIPTIDE_DEFAULT_CHAND_ATR    40.0   // loose backstop (regime exits first)

typedef struct
{
  // Resolved params (snapshot at init; reload to pick up KV changes).
  double  alpha;          // self-EMA smoothing in (0,1]
  int     regime_grain;   // 0 = 1d, 1 = 4h
  int     entry_gate;     // 0..4
  double  gate_thresh;    // RSI level (entry_gate=2)
  double  chand_atr;      // chandelier backstop in ATR_14(1h); 0 = off

  // Cached regime-grain context. *_have latches once that grain produces a
  // bar. reg_ema is the strategy-computed self-EMA of that grain's closes.
  // Both grains cached unconditionally (cheap) so flipping regime_grain
  // needs no rewire.
  double  d1_close, d1_ema;   bool d1_have;
  double  h4_close, h4_ema;   bool h4_have;

  // 1h position state.
  bool    in_position;
  double  peak_high;      // highest HIGH since entry (chandelier anchor)
} riptide_state_t;

// One EMA step (seed on first sample): ema = alpha*close + (1-alpha)*prev.
static double
riptide_ema_step(double prev, bool seen, double close, double alpha)
{
  if(!seen || isnan(prev))
    return(close);

  return(alpha * close + (1.0 - alpha) * prev);
}

// Advance one regime grain's cached self-EMA + close as its bar closes
// (lookahead-free: a later 1h bar only ever reads a regime bar that closed
// in its past).
static void
riptide_cache_context(riptide_state_t *s, wm_gran_t grain,
    const wm_candle_full_t *bar)
{
  switch(grain)
  {
    case WM_GRAN_1D:
      s->d1_ema   = riptide_ema_step(s->d1_ema, s->d1_have, bar->close,
                        s->alpha);
      s->d1_close = bar->close;
      s->d1_have  = true;
      break;

    case WM_GRAN_4H:
      s->h4_ema   = riptide_ema_step(s->h4_ema, s->h4_have, bar->close,
                        s->alpha);
      s->h4_close = bar->close;
      s->h4_have  = true;
      break;

    default:
      break;
  }
}

// Macro regime up? close of the selected regime grain above its self-EMA.
// Fails closed until that grain has produced a bar.
static bool
riptide_regime_up(const riptide_state_t *s)
{
  if(s->regime_grain == 0)
    return(s->d1_have && !isnan(s->d1_ema) && s->d1_close > s->d1_ema);

  return(s->h4_have && !isnan(s->h4_ema) && s->h4_close > s->h4_ema);
}

// The 1h entry-quality gate — reads only this 1h bar's own ind[] slots.
// Returns true (pass) for entry_gate=0. NaN reads fail closed (no entry
// until the indicator is warm), which the min_history warm-up covers.
static bool
riptide_entry_gate_ok(const riptide_state_t *s, const wm_candle_full_t *bar)
{
  float ema20 = bar->ind[WM_IND_EMA_20];
  float rsi   = bar->ind[WM_IND_RSI_14];
  float hist  = bar->ind[WM_IND_MACD_HIST];

  switch(s->entry_gate)
  {
    case 1:
      return(!isnanf(ema20) && bar->close > (double)ema20);

    case 2:
      return(!isnanf(rsi) && (double)rsi >= s->gate_thresh);

    case 3:
      return(!isnanf(hist) && (double)hist > 0.0);

    case 4:
      return(!isnanf(ema20) && !isnanf(hist) &&
             bar->close > (double)ema20 && (double)hist > 0.0);

    case 0:
    default:
      return(true);
  }
}

// ----------------------------------------------------------------------- //
// Param schema                                                            //
// ----------------------------------------------------------------------- //

static const wm_strategy_param_t riptide_params[] = {
  {
    .name        = "alpha",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = RIPTIDE_DEFAULT_ALPHA,
    .min_dbl     = 0.30,
    .max_dbl     = 0.98,
    .step_dbl    = 0.02,
    .help        = "Fast self-EMA smoothing of the regime grain's closes in"
                   " (0,1]. Higher = faster regime = more legs. The net-$"
                   " ridge is broad ~0.74-0.90. Default 0.82.",
  },
  {
    .name        = "regime_grain",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)RIPTIDE_DEFAULT_REGIME_GRAIN,
    .min_int     = 0,
    .max_int     = 1,
    .step_dbl    = 1.0,
    .help        = "Grain the long-bias regime runs on: 0=1d (slow tide),"
                   " 1=4h (the sweet spot). Default 1.",
  },
  {
    .name        = "entry_gate",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)RIPTIDE_DEFAULT_ENTRY_GATE,
    .min_int     = 0,
    .max_int     = 4,
    .step_dbl    = 1.0,
    .help        = "1h entry-quality gate (delays, never skips, an entry"
                   " until the 1h agrees): 0=none/immediate, 1=close>EMA_20,"
                   " 2=RSI_14>=gate_thresh, 3=MACD_HIST>0, 4=close>EMA_20 AND"
                   " MACD_HIST>0. Default 0.",
  },
  {
    .name        = "gate_thresh",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = RIPTIDE_DEFAULT_GATE_THRESH,
    .min_dbl     = 40.0,
    .max_dbl     = 60.0,
    .step_dbl    = 2.0,
    .help        = "RSI_14 level the 1h close must be at/above to enter"
                   " (entry_gate=2). Default 50.",
  },
  {
    .name        = "chand_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = RIPTIDE_DEFAULT_CHAND_ATR,
    .min_dbl     = 0.0,
    .max_dbl     = 60.0,
    .step_dbl    = 2.0,
    .help        = "Chandelier backstop distance in ATR_14(1h) below the"
                   " highest HIGH since entry (0 = off). Loose by default —"
                   " the regime flip is the real exit. Default 40.",
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

  // 1h decides/emits; 4h + 1d feed cached regime context only.
  out->grains_mask = (uint16_t)((1u << WM_GRAN_1H) |
                                (1u << WM_GRAN_4H) |
                                (1u << WM_GRAN_1D));

  out->min_history[WM_GRAN_1H] = RIPTIDE_HIST_1H;
  out->min_history[WM_GRAN_4H] = RIPTIDE_HIST_4H;
  out->min_history[WM_GRAN_1D] = RIPTIDE_HIST_1D;

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

  s->alpha = wm_strategy_kv_get_dbl(mid, strat, "alpha",
      RIPTIDE_DEFAULT_ALPHA);
  s->regime_grain = (int)wm_strategy_kv_get_uint(mid, strat, "regime_grain",
      (uint64_t)RIPTIDE_DEFAULT_REGIME_GRAIN);
  s->entry_gate = (int)wm_strategy_kv_get_uint(mid, strat, "entry_gate",
      (uint64_t)RIPTIDE_DEFAULT_ENTRY_GATE);
  s->gate_thresh = wm_strategy_kv_get_dbl(mid, strat, "gate_thresh",
      RIPTIDE_DEFAULT_GATE_THRESH);
  s->chand_atr = wm_strategy_kv_get_dbl(mid, strat, "chand_atr",
      RIPTIDE_DEFAULT_CHAND_ATR);

  if(s->alpha <= 0.0)         s->alpha = RIPTIDE_DEFAULT_ALPHA;
  if(s->alpha > 1.0)          s->alpha = 1.0;
  if(s->regime_grain < 0)     s->regime_grain = 0;
  if(s->regime_grain > 1)     s->regime_grain = 1;
  if(s->entry_gate < 0)       s->entry_gate = 0;
  if(s->entry_gate > 4)       s->entry_gate = 4;

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, RIPTIDE_LOG_CTX,
      "init: %s -> %s alpha=%.4f regime_grain=%d entry_gate=%d"
      " gate_thresh=%.1f chand_atr=%.2f (1h)",
      strat, mid, s->alpha, s->regime_grain, s->entry_gate,
      s->gate_thresh, s->chand_atr);

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
  float                 atr;
  bool                  regime_up;
  bool                  fire = false;

  (void)mkt;   // riptide reads only ind[] slots + its own cached context.

  if(bar == NULL || ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s == NULL)
    return;

  // Higher grains only refresh cached regime context.
  if(grain != WM_GRAN_1H)
  {
    riptide_cache_context(s, grain, bar);
    return;
  }

  // ---- 1h decision grain ----
  atr       = bar->ind[WM_IND_ATR_14];
  regime_up = riptide_regime_up(s);

  memset(&sig, 0, sizeof(sig));
  sig.ts_ms = bar->ts_close_ms;

  if(!s->in_position)
  {
    // Entry: regime up, flat, and the 1h quality gate passes. The gate
    // delays (never skips) an entry until the 1h micro-context confirms.
    if(regime_up && riptide_entry_gate_ok(s, bar))
    {
      sig.score      = 1.0;
      sig.confidence = 0.6;
      snprintf(sig.reason, sizeof(sig.reason),
          "in g%d gate%d", s->regime_grain, s->entry_gate);

      s->in_position = true;
      s->peak_high   = bar->high;
      fire           = true;
    }
  }
  else
  {
    bool   have_atr  = !isnanf(atr) && atr > 0.0f;
    double chand_lv  = -1.0;
    bool   hit_chand;

    if(bar->high > s->peak_high)
      s->peak_high = bar->high;

    if(have_atr && s->chand_atr > 0.0)
      chand_lv = s->peak_high - s->chand_atr * (double)atr;

    hit_chand = (chand_lv > 0.0 && bar->close <= chand_lv);

    // Exit is ungated + immediate: the regime flip is the primary banker,
    // the chandelier a catastrophe backstop.
    if(!regime_up || hit_chand)
    {
      const char *why = !regime_up ? "regime" : "chand";

      sig.score      = -1.0;
      sig.confidence = 0.5;
      snprintf(sig.reason, sizeof(sig.reason), "exit %s", why);

      s->in_position = false;
      s->peak_high   = 0.0;
      fire           = true;
    }
  }

  if(fire)
  {
    wm_strategy_emit_signal(ctx, &sig);

    clam(CLAM_INFO, RIPTIDE_LOG_CTX,
        "%s -> %s: %s @ ts=%lld close=%.2f",
        wm_strategy_ctx_strategy_name(ctx),
        wm_strategy_ctx_market_id(ctx),
        sig.reason, (long long)bar->ts_close_ms, bar->close);
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
