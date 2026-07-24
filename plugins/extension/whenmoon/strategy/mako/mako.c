// botmanager — MIT
// mako — long-only regime-gated dip harvester + legacy regime flipper
// (competitor #3).
//
// Thesis (round 3+, the per-fold robust_ratio score): the contest now ranks
// mean per-window return / cross-regime dispersion, so the winning shape is
// a STEADY per-window edge, not a mania beta-harvest — the flipper's fold
// distribution is all right-tail (mania windows +300..600%) and the ratio
// punishes exactly that. mako's answer is `hunt` mode: keep the proven
// self-EMA regime line, but only as a PERMISSION GATE — inside an up
// regime, buy 1h closes washed out >= dip_atr ATRs below EMA_20(1h) and
// exit on the reversion to EMA_20 (+ target_atr ATRs), an entry-anchored
// ATR stop, an optional time stop, or the regime flipping down. Both legs
// are ATR-scaled, so per-trade gain is vol-normalized — per-window returns
// come out FLAT across regimes instead of exploding in manias (buy fear,
// sell the snap-back; opposite phase to the flipper, which buys strength).
//
//   hunt       : 0 = legacy regime flipper (bit-compatible with the round-2
//                scoreboard rows). 1 = dip harvester (the round-3 shape).
//   dip_atr    : entry stretch — 1h close <= anchor - dip_atr*ATR_14(1h).
//   dip_ref    : the dip anchor. 0 = EMA_20(1h) (fixed-ATR stretch below
//                the mean). 1 = BB_LOWER(1h) (the 2-sigma Bollinger band:
//                a sigma-scaled selector that demands deeper absolute dips
//                when recent vol is expanding, shallower when quiet).
//   stop_atr   : hard stop below the entry fill, in entry-time ATRs.
//   target_atr : reversion exit offset in ATRs; anchored per tgt_mode.
//   tgt_mode   : 0 = target at EMA_20 + target_atr*ATR_14 (the mean drifts
//                while held, so wins stretch with trend strength).
//                1 = target at entry_px + target_atr*entry_atr (fixed
//                R-multiple per trade: strictly vol-normalized wins,
//                per-window return decoupled from how hard the mean ran).
//   cool_bars  : re-entry throttle — after any hunt exit, suppress new
//                entries for this many 1h bars (0 = off). Caps how densely
//                a single mania window can be harvested, trimming the
//                right-tail folds that dominate cross-regime dispersion.
//   max_hold   : time stop in 1h bars (0 = off; the EMA converging onto a
//                stagnant price is already a soft time stop).
//   bear_hunt  : 1 = ignore the regime entirely (pure reversion engine;
//                entries in bears allowed, regime exit disabled).
//
// The legacy flipper (hunt=0) — long while the regime grain's close is
// above a fast self-EMA/KAMA line, flat below — remains for cross-checks.
// Its chop-damping mechanisms:
//
//   band_bps   : a HYSTERESIS band around the regime line. Enter only when
//                close > line*(1+band), exit only when close < line*(1-band).
//                Inside the band the current state holds, so chop centered
//                on the line churns nothing. band=0 = raw flipper.
//   line_mode  : 0 = self-EMA of the regime grain's closes, smoothing set
//                directly by `alpha` (cc2's continuous-knob edge).
//                1 = KAMA (Kaufman adaptive MA): smoothing scales with the
//                efficiency ratio over `er_n` bars, so the line hugs price
//                in clean trends (fast flips = compounding) and flattens in
//                chop (fewer whipsaws). er/fast/slow knobs gate it.
//   cmp_mode   : 0 = compare the regime GRAIN's close to the line (state
//                changes only as that grain closes — cc2 classic).
//                1 = compare the CURRENT 1h close to the higher-grain line
//                (reacts intra-bar of the regime grain: exits leg breaks
//                up to 3h earlier, re-enters recoveries earlier).
//
//   entry      : pure regime (long the moment the regime is up while flat —
//                cc2's finding: impulse gates cut well-timed leg starts).
//   exit       : regime down (through the band), or a chandelier backstop
//                (close <= peak_high - chand_atr*ATR_14(1h); 0 = off).
//                Chandelier applies to hunt=0 only; hunt=1 has its own
//                stop/target/time exits.
//
// Decision/emit grain is 1h; 4h + 1d feed cached context only. With hunt=0
// band_bps=0 line_mode=0 cmp_mode=0 alpha=0.82 regime_grain=1 chand_atr=8
// mako reproduces cc2's validated winner bit-for-bit (cross-check).
//
// LOOKAHEAD SAFETY. The backtest fires on_bar in a merged chronological
// walk; on a shared timestamp the FINER grain fires first. Each grain's
// regime line (self-EMA or KAMA, advanced one step per that grain's bar
// close) and close are cached in that grain's own on_bar branch; the 1h
// decision reads only the cache, this 1h bar's own fields/ind[] slots, and
// strategy-owned state tracked forward since entry. The 1h-grain line
// (regime_grain=2) is advanced with the current 1h close and compared to
// that same close — an MA comparison against current+past closes only,
// lookahead-free by construction. The KAMA efficiency ring holds only
// closes already seen at that grain. No reach into mkt->grain_arr[] (which
// in backtest holds the whole — future — date range).

#include "whenmoon_strategy.h"

#include "alloc.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define MAKO_NAME      "mako"
#define MAKO_VERSION   "0.3"
#define MAKO_LOG_CTX   "strategy.mako"

// KAMA efficiency-ratio ring: er_n deltas need er_n+1 closes.
#define MAKO_ER_CAP    64u

// Per-grain live warm-up history (backtest snapshots carry the full range;
// this matters only at live cold start). 1h wants ATR_14 warm; every grain
// wants the KAMA ring full at its largest er_n.
#define MAKO_HIST_1H   128u
#define MAKO_HIST_4H   128u
#define MAKO_HIST_1D   128u

// Defaults — mirrored in the param schema below. These are the cc2-
// equivalent baseline (band/KAMA/cmp innovations off) until sweeps pin the
// contest config; the SCOREBOARD row is the source of truth for that.
#define MAKO_DEFAULT_REGIME_GRAIN   1.0   // 1 = 4h regime line
#define MAKO_DEFAULT_LINE_MODE      0.0   // 0 = self-EMA(alpha)
#define MAKO_DEFAULT_ALPHA          0.82  // self-EMA smoothing (line_mode 0)
#define MAKO_DEFAULT_BAND_BPS       0.0   // hysteresis half-band (0 = off)
#define MAKO_DEFAULT_CMP_MODE       0.0   // 0 = grain close vs line
#define MAKO_DEFAULT_ER_N          10.0   // KAMA efficiency lookback
#define MAKO_DEFAULT_KAMA_FAST_N    2.0   // KAMA fast smoothing period
#define MAKO_DEFAULT_KAMA_SLOW_N   30.0   // KAMA slow smoothing period
#define MAKO_DEFAULT_CHAND_ATR      8.0   // chandelier backstop (0 = off)
#define MAKO_DEFAULT_HUNT           0.0   // 0 = flipper, 1 = dip harvester
#define MAKO_DEFAULT_DIP_ATR        2.0   // entry stretch below the anchor
#define MAKO_DEFAULT_DIP_REF        0.0   // dip anchor: 0=EMA_20 1=BB_LOWER
#define MAKO_DEFAULT_STOP_ATR       3.0   // hard stop below entry (0 = off)
#define MAKO_DEFAULT_TARGET_ATR     0.0   // reversion exit offset in ATRs
#define MAKO_DEFAULT_TGT_MODE       0.0   // target anchor: 0=EMA 1=entry
#define MAKO_DEFAULT_COOL_BARS      0.0   // post-exit entry throttle (0=off)
#define MAKO_DEFAULT_MAX_HOLD       0.0   // time stop in 1h bars (0 = off)
#define MAKO_DEFAULT_BEAR_HUNT      0.0   // 1 = hunt without a regime gate

// One regime grain's cached context: the close and regime line as of that
// grain's latest closed bar, plus the KAMA close ring (line_mode 1).
typedef struct
{
  double   close;
  double   line;
  bool     have;

  double   ring[MAKO_ER_CAP];
  int      head;
  int      count;
} mako_grain_t;

typedef struct
{
  // Resolved params (snapshot at init; reload to pick up KV changes).
  int      regime_grain;   // 0 = 1d, 1 = 4h, 2 = 1h
  int      line_mode;      // 0 = self-EMA(alpha), 1 = KAMA
  int      cmp_mode;       // 0 = grain close vs line, 1 = 1h close vs line
  double   alpha;          // self-EMA smoothing (line_mode 0)
  double   band;           // hysteresis half-band as a fraction (bps/1e4)
  int      er_n;           // KAMA efficiency lookback (line_mode 1)
  double   kama_fast_sc;   // KAMA fast smoothing constant
  double   kama_slow_sc;   // KAMA slow smoothing constant
  double   chand_atr;      // chandelier give-back in ATR_14(1h) (0 = off)
  int      hunt;           // 0 = regime flipper, 1 = dip harvester
  double   dip_atr;        // hunt entry stretch in ATR_14(1h) below anchor
  int      dip_ref;        // hunt dip anchor: 0 = EMA_20, 1 = BB_LOWER
  double   stop_atr;       // hunt hard stop in entry-time ATRs (0 = off)
  double   target_atr;     // hunt reversion exit offset in ATRs
  int      tgt_mode;       // hunt target anchor: 0 = EMA_20, 1 = entry
  int      cool_bars;      // hunt post-exit re-entry throttle (0 = off)
  int      max_hold;       // hunt time stop in 1h bars (0 = off)
  int      bear_hunt;      // 1 = hunt with no regime gate at all

  mako_grain_t g1d;
  mako_grain_t g4h;
  mako_grain_t g1h;

  // 1h position state.
  bool     in_position;
  double   peak_high;      // highest HIGH since entry (chandelier anchor)
  double   entry_px;       // hunt: close at entry emit (~ the fill)
  double   entry_atr;      // hunt: ATR_14(1h) at entry (stop anchor)
  int      hold_bars;      // hunt: 1h bars held since entry
  int      cool_left;      // hunt: 1h bars of entry throttle remaining
} mako_state_t;

// Advance one grain's regime line with that grain's just-closed bar.
// Self-EMA: line = alpha*close + (1-alpha)*line, seeded on first close.
// KAMA: smoothing scales with |net move| / sum(|bar moves|) over er_n bars,
// squared, between the fast and slow constants (Kaufman's formula).
static void
mako_grain_step(const mako_state_t *s, mako_grain_t *g, double close)
{
  if(!g->have || isnan(g->line))
    g->line = close;

  else if(s->line_mode == 1)
  {
    double sc;
    double er = 0.0;

    // ER over er_n periods ending at the current close: the er_n-th most
    // recent stored close anchors the net move; noise sums the er_n bar-
    // to-bar moves between it and the current close.
    if(g->count >= s->er_n)
    {
      int    start = (g->head - s->er_n + (int)MAKO_ER_CAP * 2)
                       % (int)MAKO_ER_CAP;
      double prev  = g->ring[start];
      double noise = 0.0;
      int    i;

      for(i = 1; i < s->er_n; i++)
      {
        double cur = g->ring[(start + i) % (int)MAKO_ER_CAP];

        noise += fabs(cur - prev);
        prev   = cur;
      }

      noise += fabs(close - prev);

      if(noise > 0.0)
        er = fabs(close - g->ring[start]) / noise;
    }

    sc = er * (s->kama_fast_sc - s->kama_slow_sc) + s->kama_slow_sc;
    sc = sc * sc;

    g->line += sc * (close - g->line);
  }

  else
    g->line = s->alpha * close + (1.0 - s->alpha) * g->line;

  g->ring[g->head] = close;
  g->head          = (g->head + 1) % (int)MAKO_ER_CAP;

  if(g->count < (int)MAKO_ER_CAP)
    g->count++;

  g->close = close;
  g->have  = true;
}

// Regime state with hysteresis. `ref_close` is the price compared against
// the line (the regime grain's own close, or the current 1h close in
// cmp_mode 1). Fails closed on missing/NaN context.
static bool
mako_regime_enter_ok(const mako_state_t *s, const mako_grain_t *g,
    double ref_close)
{
  if(!g->have || isnan(g->line))
    return(false);

  return(ref_close > g->line * (1.0 + s->band));
}

static bool
mako_regime_exit_hit(const mako_state_t *s, const mako_grain_t *g,
    double ref_close)
{
  if(!g->have || isnan(g->line))
    return(true);

  return(ref_close < g->line * (1.0 - s->band));
}

// ----------------------------------------------------------------------- //
// Required exports                                                        //
// ----------------------------------------------------------------------- //

static const wm_strategy_param_t mako_params[] = {
  {
    .name        = "regime_grain",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)MAKO_DEFAULT_REGIME_GRAIN,
    .min_int     = 0,
    .max_int     = 2,
    .step_dbl    = 1.0,
    .help        = "Grain the regime line runs on: 0=1d 1=4h 2=1h."
                   " Default 1.",
  },
  {
    .name        = "line_mode",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)MAKO_DEFAULT_LINE_MODE,
    .min_int     = 0,
    .max_int     = 1,
    .step_dbl    = 1.0,
    .help        = "Regime line: 0=self-EMA of the grain's closes (smoothing"
                   " = alpha), 1=KAMA (efficiency-adaptive: fast in trends,"
                   " flat in chop; er_n/kama_fast_n/kama_slow_n). Default 0.",
  },
  {
    .name        = "alpha",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = MAKO_DEFAULT_ALPHA,
    .min_dbl     = 0.02,
    .max_dbl     = 1.0,
    .step_dbl    = 0.02,
    .help        = "Self-EMA smoothing in (0,1] (line_mode 0). Faster ="
                   " more legs banked, until fee-death. Default 0.82.",
  },
  {
    .name        = "band_bps",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = MAKO_DEFAULT_BAND_BPS,
    .min_dbl     = 0.0,
    .max_dbl     = 300.0,
    .step_dbl    = 5.0,
    .help        = "Hysteresis half-band around the regime line in basis"
                   " points: enter above line*(1+b), exit below line*(1-b),"
                   " hold inside. Cuts chop whipsaw. 0 = raw flipper."
                   " Default 0.",
  },
  {
    .name        = "cmp_mode",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)MAKO_DEFAULT_CMP_MODE,
    .min_int     = 0,
    .max_int     = 1,
    .step_dbl    = 1.0,
    .help        = "Price compared against the regime line: 0=the regime"
                   " grain's own close (state changes on that grain's"
                   " closes), 1=the current 1h close (reacts intra-bar of"
                   " the regime grain). Default 0.",
  },
  {
    .name        = "er_n",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)MAKO_DEFAULT_ER_N,
    .min_int     = 2,
    .max_int     = 60,
    .step_dbl    = 2.0,
    .help        = "KAMA efficiency-ratio lookback in regime-grain bars"
                   " (line_mode 1). Default 10.",
  },
  {
    .name        = "kama_fast_n",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)MAKO_DEFAULT_KAMA_FAST_N,
    .min_int     = 1,
    .max_int     = 30,
    .step_dbl    = 1.0,
    .help        = "KAMA fast smoothing period (line_mode 1): the line's"
                   " speed at efficiency 1. Default 2.",
  },
  {
    .name        = "kama_slow_n",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)MAKO_DEFAULT_KAMA_SLOW_N,
    .min_int     = 5,
    .max_int     = 120,
    .step_dbl    = 5.0,
    .help        = "KAMA slow smoothing period (line_mode 1): the line's"
                   " speed at efficiency 0 (dead chop). Default 30.",
  },
  {
    .name        = "chand_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = MAKO_DEFAULT_CHAND_ATR,
    .min_dbl     = 0.0,
    .max_dbl     = 60.0,
    .step_dbl    = 1.0,
    .help        = "Chandelier trailing-stop backstop in ATR_14(1h) below"
                   " the highest HIGH since entry (0 = off; the regime flip"
                   " leads). Flipper (hunt=0) only. Default 8.",
  },
  {
    .name        = "hunt",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)MAKO_DEFAULT_HUNT,
    .min_int     = 0,
    .max_int     = 1,
    .step_dbl    = 1.0,
    .help        = "0 = legacy regime flipper. 1 = dip harvester: inside an"
                   " up regime buy 1h closes >= dip_atr ATRs below"
                   " EMA_20(1h); exit on reversion to EMA_20+target_atr*ATR,"
                   " stop_atr, max_hold, or the regime flipping down."
                   " Default 0.",
  },
  {
    .name        = "dip_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = MAKO_DEFAULT_DIP_ATR,
    .min_dbl     = 0.0,
    .max_dbl     = 6.0,
    .step_dbl    = 0.25,
    .help        = "Hunt entry stretch: enter when the 1h close is at least"
                   " this many ATR_14(1h) below the dip anchor (dip_ref)."
                   " Default 2.",
  },
  {
    .name        = "dip_ref",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)MAKO_DEFAULT_DIP_REF,
    .min_int     = 0,
    .max_int     = 1,
    .step_dbl    = 1.0,
    .help        = "Hunt dip anchor: 0=EMA_20(1h) (fixed-ATR stretch below"
                   " the mean), 1=BB_LOWER(1h) (2-sigma band: sigma-scaled"
                   " selector, deeper dips demanded when vol expands)."
                   " Default 0.",
  },
  {
    .name        = "stop_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = MAKO_DEFAULT_STOP_ATR,
    .min_dbl     = 0.0,
    .max_dbl     = 12.0,
    .step_dbl    = 0.5,
    .help        = "Hunt hard stop: exit when the close falls this many"
                   " entry-time ATRs below the entry price (0 = off)."
                   " Default 3.",
  },
  {
    .name        = "target_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = MAKO_DEFAULT_TARGET_ATR,
    .min_dbl     = -2.0,
    .max_dbl     = 6.0,
    .step_dbl    = 0.25,
    .help        = "Hunt reversion exit offset in ATRs above the target"
                   " anchor (tgt_mode; negative = bank below the anchor)."
                   " Default 0.",
  },
  {
    .name        = "tgt_mode",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)MAKO_DEFAULT_TGT_MODE,
    .min_int     = 0,
    .max_int     = 1,
    .step_dbl    = 1.0,
    .help        = "Hunt target anchor: 0=EMA_20(1h)+target_atr*ATR_14 (the"
                   " mean drifts while held), 1=entry_px+target_atr*entry"
                   "-ATR (fixed R-multiple, vol-normalized win size)."
                   " Default 0.",
  },
  {
    .name        = "cool_bars",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)MAKO_DEFAULT_COOL_BARS,
    .min_int     = 0,
    .max_int     = 500,
    .step_dbl    = 12.0,
    .help        = "Hunt re-entry throttle: after any exit, suppress new"
                   " entries for this many 1h bars (0 = off). Caps how"
                   " densely one window can be harvested. Default 0.",
  },
  {
    .name        = "max_hold",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)MAKO_DEFAULT_MAX_HOLD,
    .min_int     = 0,
    .max_int     = 2000,
    .step_dbl    = 24.0,
    .help        = "Hunt time stop in 1h bars since entry (0 = off)."
                   " Default 0.",
  },
  {
    .name        = "bear_hunt",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)MAKO_DEFAULT_BEAR_HUNT,
    .min_int     = 0,
    .max_int     = 1,
    .step_dbl    = 1.0,
    .help        = "1 = hunt with no regime gate: entries allowed in any"
                   " regime and the regime-flip exit is disabled (stop/"
                   "target/time only). Default 0.",
  },
};

void
wm_strategy_describe(wm_strategy_meta_t *out)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->abi_version = WM_STRATEGY_ABI_VERSION;

  snprintf(out->name, sizeof(out->name), "%s", MAKO_NAME);
  snprintf(out->version, sizeof(out->version), "%s", MAKO_VERSION);

  // 1h decides/emits; 4h + 1d feed cached regime context only.
  out->grains_mask = (uint16_t)((1u << WM_GRAN_1H) |
                                (1u << WM_GRAN_4H) |
                                (1u << WM_GRAN_1D));

  out->min_history[WM_GRAN_1H] = MAKO_HIST_1H;
  out->min_history[WM_GRAN_4H] = MAKO_HIST_4H;
  out->min_history[WM_GRAN_1D] = MAKO_HIST_1D;

  out->params               = mako_params;
  out->n_params             = (uint32_t)(sizeof(mako_params)
                                  / sizeof(mako_params[0]));
  out->wants_trade_callback = false;
}

int
wm_strategy_init(wm_strategy_ctx_t *ctx)
{
  mako_state_t *s;
  const char   *mid;
  const char   *strat;
  int           kama_fast_n;
  int           kama_slow_n;

  if(ctx == NULL)
    return(-1);

  s = mem_alloc("strategy." MAKO_NAME, "state", sizeof(*s));

  memset(s, 0, sizeof(*s));

  mid   = wm_strategy_ctx_market_id(ctx);
  strat = wm_strategy_ctx_strategy_name(ctx);

  s->regime_grain = (int)wm_strategy_kv_get_uint(mid, strat, "regime_grain",
      (uint64_t)MAKO_DEFAULT_REGIME_GRAIN);
  s->line_mode = (int)wm_strategy_kv_get_uint(mid, strat, "line_mode",
      (uint64_t)MAKO_DEFAULT_LINE_MODE);
  s->cmp_mode = (int)wm_strategy_kv_get_uint(mid, strat, "cmp_mode",
      (uint64_t)MAKO_DEFAULT_CMP_MODE);
  s->alpha = wm_strategy_kv_get_dbl(mid, strat, "alpha", MAKO_DEFAULT_ALPHA);
  s->band = wm_strategy_kv_get_dbl(mid, strat, "band_bps",
      MAKO_DEFAULT_BAND_BPS) / 10000.0;
  s->er_n = (int)wm_strategy_kv_get_uint(mid, strat, "er_n",
      (uint64_t)MAKO_DEFAULT_ER_N);
  kama_fast_n = (int)wm_strategy_kv_get_uint(mid, strat, "kama_fast_n",
      (uint64_t)MAKO_DEFAULT_KAMA_FAST_N);
  kama_slow_n = (int)wm_strategy_kv_get_uint(mid, strat, "kama_slow_n",
      (uint64_t)MAKO_DEFAULT_KAMA_SLOW_N);
  s->chand_atr = wm_strategy_kv_get_dbl(mid, strat, "chand_atr",
      MAKO_DEFAULT_CHAND_ATR);
  s->hunt = (int)wm_strategy_kv_get_uint(mid, strat, "hunt",
      (uint64_t)MAKO_DEFAULT_HUNT);
  s->dip_atr = wm_strategy_kv_get_dbl(mid, strat, "dip_atr",
      MAKO_DEFAULT_DIP_ATR);
  s->dip_ref = (int)wm_strategy_kv_get_uint(mid, strat, "dip_ref",
      (uint64_t)MAKO_DEFAULT_DIP_REF);
  s->stop_atr = wm_strategy_kv_get_dbl(mid, strat, "stop_atr",
      MAKO_DEFAULT_STOP_ATR);
  s->target_atr = wm_strategy_kv_get_dbl(mid, strat, "target_atr",
      MAKO_DEFAULT_TARGET_ATR);
  s->tgt_mode = (int)wm_strategy_kv_get_uint(mid, strat, "tgt_mode",
      (uint64_t)MAKO_DEFAULT_TGT_MODE);
  s->cool_bars = (int)wm_strategy_kv_get_uint(mid, strat, "cool_bars",
      (uint64_t)MAKO_DEFAULT_COOL_BARS);
  s->max_hold = (int)wm_strategy_kv_get_uint(mid, strat, "max_hold",
      (uint64_t)MAKO_DEFAULT_MAX_HOLD);
  s->bear_hunt = (int)wm_strategy_kv_get_uint(mid, strat, "bear_hunt",
      (uint64_t)MAKO_DEFAULT_BEAR_HUNT);

  if(s->regime_grain < 0)             s->regime_grain = 0;
  if(s->regime_grain > 2)             s->regime_grain = 2;
  if(s->line_mode < 0)                s->line_mode = 0;
  if(s->line_mode > 1)                s->line_mode = 1;
  if(s->cmp_mode < 0)                 s->cmp_mode = 0;
  if(s->cmp_mode > 1)                 s->cmp_mode = 1;
  if(s->alpha <= 0.0)                 s->alpha = MAKO_DEFAULT_ALPHA;
  if(s->alpha > 1.0)                  s->alpha = 1.0;
  if(s->band < 0.0)                   s->band = 0.0;
  if(s->er_n < 2)                     s->er_n = 2;
  if(s->er_n > (int)MAKO_ER_CAP - 2)  s->er_n = (int)MAKO_ER_CAP - 2;
  if(kama_fast_n < 1)                 kama_fast_n = 1;
  if(kama_slow_n < kama_fast_n)       kama_slow_n = kama_fast_n;
  if(s->chand_atr < 0.0)              s->chand_atr = 0.0;
  if(s->hunt < 0)                     s->hunt = 0;
  if(s->hunt > 1)                     s->hunt = 1;
  if(s->dip_atr < 0.0)                s->dip_atr = 0.0;
  if(s->dip_ref < 0)                  s->dip_ref = 0;
  if(s->dip_ref > 1)                  s->dip_ref = 1;
  if(s->tgt_mode < 0)                 s->tgt_mode = 0;
  if(s->tgt_mode > 1)                 s->tgt_mode = 1;
  if(s->cool_bars < 0)                s->cool_bars = 0;
  if(s->stop_atr < 0.0)               s->stop_atr = 0.0;
  if(s->max_hold < 0)                 s->max_hold = 0;
  if(s->bear_hunt < 0)                s->bear_hunt = 0;
  if(s->bear_hunt > 1)                s->bear_hunt = 1;

  s->kama_fast_sc = 2.0 / ((double)kama_fast_n + 1.0);
  s->kama_slow_sc = 2.0 / ((double)kama_slow_n + 1.0);

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, MAKO_LOG_CTX,
      "init: %s -> %s regime_grain=%d line_mode=%d cmp_mode=%d alpha=%.4f"
      " band=%.4f%% er_n=%d kama_sc=%.3f/%.3f chand_atr=%.2f hunt=%d"
      " dip_atr=%.2f dip_ref=%d stop_atr=%.2f target_atr=%.2f tgt_mode=%d"
      " cool_bars=%d max_hold=%d bear_hunt=%d",
      strat, mid, s->regime_grain, s->line_mode, s->cmp_mode, s->alpha,
      s->band * 100.0, s->er_n, s->kama_fast_sc, s->kama_slow_sc,
      s->chand_atr, s->hunt, s->dip_atr, s->dip_ref, s->stop_atr,
      s->target_atr, s->tgt_mode, s->cool_bars, s->max_hold, s->bear_hunt);

  return(0);
}

void
wm_strategy_finalize(wm_strategy_ctx_t *ctx)
{
  mako_state_t *s;

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
  mako_state_t         *s;
  mako_grain_t         *g;
  wm_strategy_signal_t  sig;
  double                ref_close;
  bool                  fire = false;

  (void)mkt;   // mako reads only this bar + its own cached context.

  if(bar == NULL || ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s == NULL)
    return;

  // Higher grains only advance their cached regime line + close.
  if(grain == WM_GRAN_1D)
  {
    mako_grain_step(s, &s->g1d, bar->close);
    return;
  }

  if(grain == WM_GRAN_4H)
  {
    mako_grain_step(s, &s->g4h, bar->close);
    return;
  }

  if(grain != WM_GRAN_1H)
    return;

  // ---- 1h decision grain ----
  mako_grain_step(s, &s->g1h, bar->close);

  g = (s->regime_grain == 0) ? &s->g1d :
      (s->regime_grain == 2) ? &s->g1h : &s->g4h;

  // cmp_mode 1 measures the current 1h close against the higher-grain
  // line; mode 0 (and the 1h regime, where they coincide) measures the
  // regime grain's own latest close.
  ref_close = (s->cmp_mode == 1 || s->regime_grain == 2)
                ? bar->close : g->close;

  memset(&sig, 0, sizeof(sig));
  sig.ts_ms = bar->ts_close_ms;

  if(s->hunt == 1)
  {
    // Dip harvester: ATR-stretched pullback entry below the chosen anchor,
    // reversion/stop/time/regime exits, optional post-exit entry throttle.
    // Reads only this bar's ind[] and cached regime context.
    float  ema      = bar->ind[WM_IND_EMA_20];
    float  atr      = bar->ind[WM_IND_ATR_14];
    float  bbl      = bar->ind[WM_IND_BB_LOWER];
    bool   have_ind = !isnanf(ema) && !isnanf(atr) && atr > 0.0f;
    double anchor   = (s->dip_ref == 1) ? (double)bbl : (double)ema;
    bool   have_anchor = have_ind && (s->dip_ref == 0 || !isnanf(bbl));

    if(!s->in_position)
    {
      bool regime_ok = s->bear_hunt == 1
                         || mako_regime_enter_ok(s, g, ref_close);

      if(s->cool_left > 0)
        s->cool_left--;

      else if(have_anchor && regime_ok
          && bar->close <= anchor - s->dip_atr * (double)atr)
      {
        sig.score      = 1.0;
        sig.confidence = 0.6;
        snprintf(sig.reason, sizeof(sig.reason),
            "dip %.1fatr r%d g%d", s->dip_atr, s->dip_ref, s->regime_grain);

        s->in_position = true;
        s->entry_px    = bar->close;
        s->entry_atr   = (double)atr;
        s->hold_bars   = 0;
        fire           = true;
      }
    }

    else
    {
      const char *why = NULL;

      s->hold_bars++;

      if(s->stop_atr > 0.0
          && bar->close <= s->entry_px - s->stop_atr * s->entry_atr)
        why = "stop";

      else if(s->bear_hunt == 0 && mako_regime_exit_hit(s, g, ref_close))
        why = "regime";

      else if(s->tgt_mode == 1
          ? bar->close >= s->entry_px + s->target_atr * s->entry_atr
          : (have_ind
              && bar->close >= (double)ema + s->target_atr * (double)atr))
        why = "target";

      else if(s->max_hold > 0 && s->hold_bars >= s->max_hold)
        why = "time";

      if(why != NULL)
      {
        sig.score      = -1.0;
        sig.confidence = 0.5;
        snprintf(sig.reason, sizeof(sig.reason), "exit %s", why);

        s->in_position = false;
        s->entry_px    = 0.0;
        s->entry_atr   = 0.0;
        s->hold_bars   = 0;
        s->cool_left   = s->cool_bars;
        fire           = true;
      }
    }

    if(fire)
      wm_strategy_emit_signal(ctx, &sig);

    return;
  }

  if(!s->in_position)
  {
    if(mako_regime_enter_ok(s, g, ref_close))
    {
      sig.score      = 1.0;
      sig.confidence = 0.6;
      snprintf(sig.reason, sizeof(sig.reason),
          "in g%d m%d", s->regime_grain, s->line_mode);

      s->in_position = true;
      s->peak_high   = bar->high;
      fire           = true;
    }
  }

  else
  {
    float  atr       = bar->ind[WM_IND_ATR_14];
    bool   have_atr  = !isnanf(atr) && atr > 0.0f;
    double chand_lv  = -1.0;
    bool   hit_chand;
    bool   hit_regime;

    if(bar->high > s->peak_high)
      s->peak_high = bar->high;

    if(have_atr && s->chand_atr > 0.0)
      chand_lv = s->peak_high - s->chand_atr * (double)atr;

    hit_chand  = (chand_lv > 0.0 && bar->close <= chand_lv);
    hit_regime = mako_regime_exit_hit(s, g, ref_close);

    if(hit_regime || hit_chand)
    {
      sig.score      = -1.0;
      sig.confidence = 0.5;
      snprintf(sig.reason, sizeof(sig.reason),
          "exit %s", hit_regime ? "regime" : "chand");

      s->in_position = false;
      s->peak_high   = 0.0;
      fire           = true;
    }
  }

  if(fire)
    wm_strategy_emit_signal(ctx, &sig);
}

// ----------------------------------------------------------------------- //
// Plugin lifecycle                                                        //
// ----------------------------------------------------------------------- //

static bool
mako_plugin_init(void)
{
  clam(CLAM_INFO, MAKO_LOG_CTX, "%s v%s loaded", MAKO_NAME, MAKO_VERSION);
  return(false);
}

static void
mako_plugin_deinit(void)
{
  clam(CLAM_INFO, MAKO_LOG_CTX, "%s deinit", MAKO_NAME);
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "strategy_" MAKO_NAME,
  .version         = MAKO_VERSION,
  .type            = PLUGIN_STRATEGY,
  .kind            = MAKO_NAME,
  .provides        = { { .name = "strategy_" MAKO_NAME } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_whenmoon" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = mako_plugin_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = mako_plugin_deinit,
  .ext             = NULL,
};
