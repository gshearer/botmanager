// botmanager — MIT
// juggernaut — ADX trend-strength-gated momentum rider (4h decision grain).
//
// cp1/cp2/surf all gate entries on trend DIRECTION (a regime MA, a MACD/EMA/
// RSI cross). None of them ask whether the trend has any STRENGTH behind it,
// so all three happily enter grinding, directionless chop as long as price
// sits marginally above a moving average. juggernaut adds that missing axis:
// ADX_14 measures trend strength (direction-agnostic), so entry requires
// BOTH a confirmed directional tide (the shared "stay out of the bear"
// regime gate every winner uses) AND ADX_14 sitting above a strength floor
// -- the trend is not just up, it is actually MOVING.
//
// This is a LEVEL gate, not a cross: it is only ever tested while flat, so
// flat -> conditions-true is already the edge. An ADX-cross-up requirement
// would let a stop-out mid-trend (where ADX stays elevated the whole time)
// strand juggernaut flat for the rest of that trend, since ADX would need
// to dip below the floor and cross back up before a new entry could arm --
// which may never happen inside one strong trend. The level gate re-arms
// the instant the strategy is flat again with the strength condition still
// true, so it keeps re-entering a trend after each stop-out instead of
// sitting out the remainder of it.
//
// The same ADX axis powers an early-bank exit: ADX fading back below a
// (lower, hysteresis) floor means the move has stalled even before the
// regime itself flips, so juggernaut can bank before the macro trend
// formally ends.
//
//   regime (1d / 4h) : longs allowed only while the close on the regime
//                      grain is above its regime MA (regime_ma: EMA_20 /
//                      EMA_50 / SMA_50 / SMA_200). Macro tide + hard exit
//                      backstop, identical shape to cp1/cp2/surf.
//   entry (4h)       : regime up, flat, AND ADX_14(4h) >= adx_entry (trend
//                      strength confirmed) AND close > EMA_20(4h) (confirms
//                      the strength is to the UPSIDE -- ADX alone is
//                      directionless).
//   exit (4h)        : bank or ride, per exit_mode --
//                        0 = chandelier (close <= peak_high - chand_atr *
//                            ATR_14) -- ride the leg,
//                        1 = ADX fade (ADX_14 drops below adx_exit -- the
//                            move has stalled) -- bank early, most active,
//                        2 = either, whichever fires first.
//                      The regime flip is an unconditional backstop in all
//                      three modes, same convention as surf.
//
// entry is a single fixed shape (the ADX-level + direction-confirm IS the
// thesis -- splitting it into toggles would just be surf's entry_mode
// again); exit_mode is the one ordinal dial, mirroring cp2/surf's proven
// "one knob bundles behaviour" pattern for overfit resistance.
//
// ── VALIDATED CONFIG (v0.3 defaults, round 2 / robust_ratio recipe) ──────
//      regime_grain=0 (1d)   regime_ma=0 (EMA_20)   adx_entry=8
//      chand_atr=3.5   exit_mode=0 (chandelier ride). Official walk-forward
//      robust_ratio=1.091 (pooled BTC+ETH), worst_fold=0.00%, pos_frac
//      96.9%, maxDD 3.9%/3.2% -- see SCOREBOARD.md for the full row.
//      chand_atr=3.5 REVERSES the v0.2/round-1 conclusion that tighter
//      chandeliers were "strictly worse" -- that was true for compounded
//      terminal equity (a wide chandelier lets a few mega-trend windows
//      run further, inflating final_equity) but FALSE for robust_ratio
//      (mean per-fold return / its cross-regime stddev): a tight
//      chandelier trims exactly those mega-trend windows, cutting stddev
//      more than it cuts the mean, and nearly doubles trade count as a
//      free bonus (3.0 -> 4.68 tr/mo). Confirmed via full chand_atr sweep
//      1.5-14 (peak ridge 3.0-3.75, all within 5% of each other -- broad,
//      not a knife-edge) then a matching adx_entry re-sweep at the new
//      chand_atr (flat plateau 6-9, chosen mid-plateau). LESSON: a metric
//      change can invert a "REJECTED" conclusion from a prior round --
//      always re-sweep the axes that metric cares about, don't just carry
//      the old winner forward.
//
// ── ROUND 3 additions (all default OFF -- v0.3 behaviour unchanged unless
//    swept in) ───────────────────────────────────────────────────────────
//      d1_adx_min : a second, orthogonal ADX_14 floor measured on the
//                   cached 1d regime bar (vs. adx_entry's 4h floor) --
//                   tests whether requiring strength on BOTH the macro and
//                   decision grain tightens the fold distribution.
//      min_hold   : suppresses chandelier/ADX-fade/cflip exits for the
//                   first N 4h bars after entry (regime flip still fires
//                   unconditionally) -- guards against noise-driven
//                   premature stop-outs, worth checking now that
//                   chand_atr=3.5 is tight enough for early whipsaw.
//      max_hold   : force-exits a long after N 4h bars regardless of
//                   chandelier/ADX state. Targets the diagnosed cause of
//                   robust_ratio's remaining dispersion directly: a
//                   handful of mega-trend windows (BTC 2020-21, ETH
//                   2017/2020-21) still ride uncapped and dominate
//                   pooled_std. Capping hold time chunks one giant fold
//                   into several bounded ones and, if the level-gate is
//                   still true, re-arms next bar -- same mechanism, more
//                   (bounded) folds instead of one huge one.
//
// LOOKAHEAD SAFETY. Same pattern as surf: the backtest fires on_bar in a
// merged chronological walk across grains, and on a shared timestamp the
// FINER grain fires first (4h before 1d). juggernaut caches the 1d regime
// grain's close + regime MA as that grain's bar closes and reads the cache
// only on a later 4h decision bar, so a 4h decision never sees a 1d bar
// that has not genuinely closed yet. The 4h decision itself reads only
// this bar's own ind[] slots (each is the value at this bar's close --
// lookahead-free in both live and backtest) plus a strategy-owned
// peak_high tracked forward since entry. No reach into mkt->grain_arr[]
// (which in backtest holds the whole -- future -- date range).

#include "whenmoon_strategy.h"

#include "alloc.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define JUG_NAME       "juggernaut"
#define JUG_VERSION    "0.4"
#define JUG_LOG_CTX    "strategy.juggernaut"

// Per-grain live warm-up history (bars). Backtest snapshots carry the full
// date range regardless; this only matters at live cold start. ADX_14
// wants ~2x its period to stabilize; the regime grains want up to SMA_200.
#define JUG_HIST_4H   256u
#define JUG_HIST_1D   256u

// Defaults -- mirrored in the param schema below.
#define JUG_DEFAULT_REGIME_GRAIN   0.0    // 0 = 1d regime (slow, robust tide)
#define JUG_DEFAULT_REGIME_MA      0.0    // 0 = EMA_20 on the regime grain
#define JUG_DEFAULT_ADX_ENTRY      8.0    // ADX_14(4h) floor to arm entry
#define JUG_DEFAULT_ADX_EXIT      16.0    // ADX_14(4h) fade level to bank
#define JUG_DEFAULT_CHAND_ATR      3.5    // chandelier give-back in ATR_14(4h)
#define JUG_DEFAULT_EXIT_MODE      0.0    // 0 = chandelier ride
#define JUG_DEFAULT_D1_ADX_MIN     0.0    // 0 = disabled (round-3 axis)
#define JUG_DEFAULT_MIN_HOLD       0.0    // 0 = disabled (round-3 axis)
#define JUG_DEFAULT_MAX_HOLD       0.0    // 0 = disabled (round-3 axis)

typedef struct
{
  // Resolved params (snapshot at init; reload to pick up KV changes).
  int      regime_grain;   // 0 = 1d regime, 1 = 4h regime
  int      regma_slot;     // resolved WM_IND_* for the regime MA
  double   adx_entry;      // ADX_14(4h) floor to arm a long
  double   adx_exit;       // ADX_14(4h) fade level (exit_mode 1/2)
  double   chand_atr;      // chandelier = peak_high - chand_atr*ATR_14 (0=off)
  int      exit_mode;      // 0 chandelier / 1 ADX-fade / 2 either / 3 cflip
  double   d1_adx_min;     // orthogonal ADX_14(1d) floor (0=disabled)
  int      min_hold;       // bars (4h) exits suppressed after entry (0=off)
  int      max_hold;       // bars (4h) forced exit ceiling (0=unbounded)

  // Cached 1d regime context. *_have latches once the daily grain produces
  // a bar; values are the readings at that grain's latest close (NaN-
  // guarded on read). Cached unconditionally so flipping regime_grain
  // between 1d/4h needs no rewire (4h regime reads the decision bar
  // itself, see jug_regime_up below).
  double   d1_close, d1_regma, d1_adx;   bool d1_have;

  // 4h position state.
  bool     in_position;
  double   peak_high;      // highest HIGH since entry (chandelier anchor)
  int      bars_held;      // 4h bars elapsed since entry (min/max_hold)
} jug_state_t;

// Map the regime_ma ordinal onto a moving-average indicator slot. Same slot
// id whichever grain regime_grain selects -- the value differs because it
// is computed on that grain's own bars.
static int
jug_regma_slot(int regime_ma)
{
  switch(regime_ma)
  {
    case 0:  return(WM_IND_EMA_20);
    case 2:  return(WM_IND_SMA_50);
    case 3:  return(WM_IND_SMA_200);
    case 1:
    default: return(WM_IND_EMA_50);
  }
}

// Macro regime up? regime_grain=1 (4h) reads the current 4h decision bar
// directly (it is "now", not cached future data); regime_grain=0 (1d)
// reads the cached daily close/MA from the last CLOSED daily bar. Fails
// closed if the daily grain has not produced a bar yet or its MA is NaN.
static bool
jug_regime_up(const jug_state_t *s, const wm_candle_full_t *bar4h)
{
  if(s->regime_grain == 1)
  {
    float regma = bar4h->ind[s->regma_slot];

    return(!isnan(regma) && bar4h->close > (double)regma);
  }

  return(s->d1_have && !isnan(s->d1_regma) && s->d1_close > s->d1_regma);
}

// Cache the 1d regime grain's close + MA as its bar closes (lookahead-free:
// a later 4h bar only ever reads a daily bar that closed in its past).
static void
jug_cache_context(jug_state_t *s, const wm_candle_full_t *bar)
{
  s->d1_close = bar->close;
  s->d1_regma = bar->ind[s->regma_slot];
  s->d1_adx   = bar->ind[WM_IND_ADX_14];
  s->d1_have  = true;
}

// ----------------------------------------------------------------------- //
// Required exports                                                        //
// ----------------------------------------------------------------------- //

static const wm_strategy_param_t jug_params[] = {
  {
    .name        = "regime_grain",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)JUG_DEFAULT_REGIME_GRAIN,
    .min_int     = 0,
    .max_int     = 1,
    .step_dbl    = 1.0,
    .help        = "Grain the long-bias regime runs on: 0=1d (slow, robust"
                   " tide), 1=4h (faster -- re-enables longs sooner in a"
                   " recovery). Default 0.",
  },
  {
    .name        = "regime_ma",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)JUG_DEFAULT_REGIME_MA,
    .min_int     = 0,
    .max_int     = 3,
    .step_dbl    = 1.0,
    .help        = "Regime MA on the regime grain (close must be above it"
                   " for longs; below it forces exit): 0=EMA_20, 1=EMA_50,"
                   " 2=SMA_50, 3=SMA_200. Default 0.",
  },
  {
    .name        = "adx_entry",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = JUG_DEFAULT_ADX_ENTRY,
    .min_dbl     = 6.0,
    .max_dbl     = 40.0,
    .step_dbl    = 1.0,
    .help        = "ADX_14(4h) floor the trend strength must sit at or"
                   " above to arm a long (direction confirmed by close >"
                   " EMA_20(4h) the same bar). Default 8.",
  },
  {
    .name        = "adx_exit",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = JUG_DEFAULT_ADX_EXIT,
    .min_dbl     = 6.0,
    .max_dbl     = 30.0,
    .step_dbl    = 1.0,
    .help        = "ADX_14(4h) level a fading trend must drop below to bank"
                   " early (exit_mode 1/2). Should sit below adx_entry"
                   " (hysteresis). Default 16.",
  },
  {
    .name        = "chand_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = JUG_DEFAULT_CHAND_ATR,
    .min_dbl     = 1.5,
    .max_dbl     = 20.0,
    .step_dbl    = 0.5,
    .help        = "Chandelier trailing-stop distance in ATR_14(4h) below"
                   " the highest HIGH since entry (exit_mode 0/2). Default"
                   " 3.5.",
  },
  {
    .name        = "exit_mode",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)JUG_DEFAULT_EXIT_MODE,
    .min_int     = 0,
    .max_int     = 3,
    .step_dbl    = 1.0,
    .help        = "How a long closes (the regime flip is always a"
                   " backstop): 0=chandelier only (ride the leg), 1=ADX"
                   " fade only (bank each stall), 2=either first (most"
                   " active), 3=condition-flip (exit the instant ADX drops"
                   " below adx_entry OR close drops below EMA_20 -- no"
                   " ride, no hysteresis; symmetric with the entry test)."
                   " Default 0.",
  },
  {
    .name        = "d1_adx_min",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = JUG_DEFAULT_D1_ADX_MIN,
    .min_dbl     = 0.0,
    .max_dbl     = 40.0,
    .step_dbl    = 1.0,
    .help        = "Orthogonal ADX_14(1d) floor entry must also clear (0 ="
                   " disabled). A second, slower strength confirm alongside"
                   " adx_entry's 4h floor. Default 0 (off).",
  },
  {
    .name        = "min_hold",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)JUG_DEFAULT_MIN_HOLD,
    .min_int     = 0,
    .max_int     = 60,
    .step_dbl    = 1.0,
    .help        = "Bars (4h) after entry during which chandelier/ADX-fade/"
                   "cflip exits are suppressed -- the regime-flip backstop"
                   " still fires unconditionally. Guards against"
                   " noise-driven premature stop-outs. Default 0 (off).",
  },
  {
    .name        = "max_hold",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)JUG_DEFAULT_MAX_HOLD,
    .min_int     = 0,
    .max_int     = 720,
    .step_dbl    = 8.0,
    .help        = "Force-exit a long after this many 4h bars regardless of"
                   " chandelier/ADX state (0 = unbounded). Caps how much of"
                   " one mega-trend a single fold can capture; re-arms next"
                   " bar if the entry gate is still true. Default 0 (off).",
  },
};

void
wm_strategy_describe(wm_strategy_meta_t *out)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->abi_version = WM_STRATEGY_ABI_VERSION;

  snprintf(out->name, sizeof(out->name), "%s", JUG_NAME);
  snprintf(out->version, sizeof(out->version), "%s", JUG_VERSION);

  // 4h decides/emits; 1d feeds cached regime context only.
  out->grains_mask = (uint16_t)((1u << WM_GRAN_4H) | (1u << WM_GRAN_1D));

  out->min_history[WM_GRAN_4H] = JUG_HIST_4H;
  out->min_history[WM_GRAN_1D] = JUG_HIST_1D;

  out->params               = jug_params;
  out->n_params             = (uint32_t)(sizeof(jug_params)
                                  / sizeof(jug_params[0]));
  out->wants_trade_callback = false;
}

int
wm_strategy_init(wm_strategy_ctx_t *ctx)
{
  jug_state_t *s;
  const char  *mid;
  const char  *strat;
  int          regime_ma;

  if(ctx == NULL)
    return(-1);

  s = mem_alloc("strategy." JUG_NAME, "state", sizeof(*s));

  memset(s, 0, sizeof(*s));

  mid   = wm_strategy_ctx_market_id(ctx);
  strat = wm_strategy_ctx_strategy_name(ctx);

  s->regime_grain = (int)wm_strategy_kv_get_uint(mid, strat, "regime_grain",
      (uint64_t)JUG_DEFAULT_REGIME_GRAIN);
  regime_ma = (int)wm_strategy_kv_get_uint(mid, strat, "regime_ma",
      (uint64_t)JUG_DEFAULT_REGIME_MA);
  s->adx_entry = wm_strategy_kv_get_dbl(mid, strat, "adx_entry",
      JUG_DEFAULT_ADX_ENTRY);
  s->adx_exit = wm_strategy_kv_get_dbl(mid, strat, "adx_exit",
      JUG_DEFAULT_ADX_EXIT);
  s->chand_atr = wm_strategy_kv_get_dbl(mid, strat, "chand_atr",
      JUG_DEFAULT_CHAND_ATR);
  s->exit_mode = (int)wm_strategy_kv_get_uint(mid, strat, "exit_mode",
      (uint64_t)JUG_DEFAULT_EXIT_MODE);
  s->d1_adx_min = wm_strategy_kv_get_dbl(mid, strat, "d1_adx_min",
      JUG_DEFAULT_D1_ADX_MIN);
  s->min_hold = (int)wm_strategy_kv_get_uint(mid, strat, "min_hold",
      (uint64_t)JUG_DEFAULT_MIN_HOLD);
  s->max_hold = (int)wm_strategy_kv_get_uint(mid, strat, "max_hold",
      (uint64_t)JUG_DEFAULT_MAX_HOLD);

  if(s->regime_grain < 0) s->regime_grain = 0;
  if(s->regime_grain > 1) s->regime_grain = 1;
  if(s->exit_mode    < 0) s->exit_mode    = 0;
  if(s->exit_mode    > 3) s->exit_mode    = 3;
  if(s->min_hold      < 0) s->min_hold      = 0;
  if(s->max_hold      < 0) s->max_hold      = 0;

  s->regma_slot = jug_regma_slot(regime_ma);

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, JUG_LOG_CTX,
      "init: %s -> %s regime_grain=%d regma_slot=%d adx_entry=%.1f"
      " adx_exit=%.1f chand_atr=%.2f exit_mode=%d d1_adx_min=%.1f"
      " min_hold=%d max_hold=%d (4h)",
      strat, mid, s->regime_grain, s->regma_slot, s->adx_entry,
      s->adx_exit, s->chand_atr, s->exit_mode, s->d1_adx_min, s->min_hold,
      s->max_hold);

  return(0);
}

void
wm_strategy_finalize(wm_strategy_ctx_t *ctx)
{
  jug_state_t *s;

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
  jug_state_t          *s;
  wm_strategy_signal_t  sig;
  float                 adx;
  float                 atr;
  float                 ema20;
  bool                  regime_up;
  bool                  fire = false;

  (void)mkt;   // juggernaut reads only ind[] slots + its own cached context.

  if(bar == NULL || ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s == NULL)
    return;

  // The daily grain only refreshes cached regime context.
  if(grain == WM_GRAN_1D)
  {
    jug_cache_context(s, bar);
    return;
  }

  if(grain != WM_GRAN_4H)
    return;

  // ---- 4h decision grain ----
  adx       = bar->ind[WM_IND_ADX_14];
  atr       = bar->ind[WM_IND_ATR_14];
  ema20     = bar->ind[WM_IND_EMA_20];
  regime_up = jug_regime_up(s, bar);

  memset(&sig, 0, sizeof(sig));
  sig.ts_ms = bar->ts_close_ms;

  {
    bool have_adx  = !isnan(adx);
    bool strong    = have_adx && (double)adx >= s->adx_entry;
    bool dir_up    = !isnan(ema20) && bar->close > (double)ema20;
    bool d1_strong = (s->d1_adx_min <= 0.0) ||
                     (s->d1_have && !isnan(s->d1_adx) &&
                      s->d1_adx >= s->d1_adx_min);

    if(!s->in_position)
    {
      // Entry: regime up, flat, ADX at/above the strength floor, AND the
      // confirmed strength is to the upside (ADX alone is direction-
      // agnostic). A level gate, not a cross -- see file header for why.
      // d1_strong is a no-op (always true) unless d1_adx_min > 0.
      if(regime_up && strong && dir_up && d1_strong)
      {
        sig.score      = 1.0;
        sig.confidence = 0.6;
        snprintf(sig.reason, sizeof(sig.reason),
            "in adx%.0f>=%.0f g%d", (double)adx, s->adx_entry,
            s->regime_grain);

        s->in_position = true;
        s->peak_high   = bar->high;
        s->bars_held   = 0;
        fire           = true;
      }
    }
    else
    {
      bool   have_atr    = !isnan(atr) && atr > 0.0f;
      double chand_lv     = -1.0;
      bool   want_fade    = (s->exit_mode == 1 || s->exit_mode == 2);
      bool   want_chand   = (s->exit_mode == 0 || s->exit_mode == 2);
      bool   want_cflip   = (s->exit_mode == 3);
      bool   past_minhold = (s->min_hold == 0 || s->bars_held >= s->min_hold);
      bool   hit_fade;
      bool   hit_chand;
      bool   hit_cflip;
      bool   hit_maxhold;

      s->bars_held++;

      if(bar->high > s->peak_high)
        s->peak_high = bar->high;

      if(want_chand && have_atr && s->chand_atr > 0.0)
        chand_lv = s->peak_high - s->chand_atr * (double)atr;

      // min_hold suppresses the price/indicator exits only -- the regime
      // backstop below is never suppressed. max_hold is a ceiling, not
      // gated by min_hold (the two bound opposite ends of hold time).
      hit_fade    = past_minhold &&
                    (want_fade && have_adx && (double)adx < s->adx_exit);
      hit_chand   = past_minhold && (chand_lv > 0.0 && bar->close <= chand_lv);
      hit_cflip   = past_minhold && (want_cflip && !(strong && dir_up));
      hit_maxhold = (s->max_hold > 0 && s->bars_held >= s->max_hold);

      // The regime flip is an unconditional backstop in every exit mode.
      if(hit_fade || hit_chand || hit_cflip || hit_maxhold || !regime_up)
      {
        const char *why = !regime_up   ? "regime"  :
                          hit_maxhold  ? "maxhold" :
                          hit_cflip    ? "cflip"   :
                          hit_fade     ? "adxfade" : "chand";

        sig.score      = -1.0;
        sig.confidence = 0.5;
        snprintf(sig.reason, sizeof(sig.reason), "exit %s", why);

        s->in_position = false;
        s->peak_high   = 0.0;
        s->bars_held   = 0;
        fire           = true;
      }
    }
  }

  if(fire)
  {
    wm_strategy_emit_signal(ctx, &sig);

    clam(CLAM_INFO, JUG_LOG_CTX,
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
jug_plugin_init(void)
{
  clam(CLAM_INFO, JUG_LOG_CTX, "%s v%s loaded", JUG_NAME, JUG_VERSION);
  return(false);
}

static void
jug_plugin_deinit(void)
{
  wm_strategy_detach_self(JUG_NAME);

  clam(CLAM_INFO, JUG_LOG_CTX, "%s deinit", JUG_NAME);
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "strategy_" JUG_NAME,
  .version         = JUG_VERSION,
  .type            = PLUGIN_STRATEGY,
  .kind            = JUG_NAME,
  .provides        = { { .name = "strategy_" JUG_NAME } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_whenmoon" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = jug_plugin_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = jug_plugin_deinit,
  .ext             = NULL,
};
