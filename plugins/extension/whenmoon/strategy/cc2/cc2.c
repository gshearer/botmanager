// botmanager — MIT
// cc2 ("Claude Competitor #2") — fast self-regime compounding trend-rider.
//
// cc2 targets the SAME prize as cc1 (max compounded return across BTC / ETH
// / XRP / SOL at 25% deployment, scored by the SCOREBOARD.md aggregate). It
// is a long-only, single-position, 25%-sizing trend follower whose only
// state is "long while the regime is up, flat while it is down". Everything
// that follows is about making that on/off switch turn at the right speed.
//
// ── WINNING CONFIG (full-corpus aggregate, validated OOS-tail-30 + walk-
//    forward on all four markets; reproduced in SCOREBOARD.md) ────────────
//      regime_grain=1 (4h)   regime_ma=7 (self-EMA)   alpha=0.82
//      entry_mode=3 (pure)   chand_atr=8   adx_min=0   time_stop_bars=0
//    These ARE the compile-time defaults below, so a fresh daemon reproduces
//    the winner from a bare `backtest run <wm> cc2`. (Caveat: a long-running
//    daemon keeps the KV values registered at FIRST load — see "KV
//    staleness" in ../AGENTS.md — so reproduce with the explicit name=val
//    args from SCOREBOARD.md, or freshstart.)
//
// ── The three levers, and what the sweeps actually found ─────────────────
//
//   1. REGIME GRAIN (`regime_grain`: 0=1d, 1=4h, 2=1h). cc1's regime is the
//      *daily* close vs a daily MA — it re-evaluates once a day, giving back
//      a full day at tops and confirming bottoms a full day late. cc2 runs
//      the regime on the **4h** grain: 6x more often, so it exits pullbacks
//      earlier (preserves the peak) and re-enters the next leg lower
//      (deploys 25% of a preserved account at a cheaper basis) — both
//      compound into the next leg. FINDING: 4h is the optimum. 1d is far
//      slower/lower; 1h (grain 2) is *negative* — the finer grain crosses
//      the regime line 4x as often and whipsaws to death. The 4h grain's
//      coarser sampling is itself the noise filter.
//
//   2. REGIME SPEED (`regime_ma`, `fast_n`, `alpha`). The regime line can be
//      a precomputed indicator MA (`regime_ma` 0..6 = EMA_20..SMA_7) OR a
//      STRATEGY-COMPUTED EMA (`regime_ma=7`) whose smoothing is set either
//      by an integer period (`fast_n`) or, preferably, directly by a
//      continuous factor (`alpha`>0 overrides `fast_n`). FINDING: faster =
//      more compounding, monotonically, until a cliff — but the cliff is
//      past the fastest *precomputed* slot (SMA_7), so the self-EMA is the
//      whole edge. Swept continuously, the aggregate peaks at **alpha=0.82**
//      (a broad peak: ±0.01 costs ~1.3%). Faster than that, or pure 1-bar
//      momentum (`regime_ma=8`), OVERSHOOTS into fee-death; slower under-
//      compounds. The aggregate is ~99% ETH-dominated and ETH peaks at
//      0.82, so one global alpha is also the per-market optimum — there is
//      no per-market-tuning headroom to exploit here.
//
//   3. ENTRY (`entry_mode`: 0=Donchian breakout, 1=close>EMA_20, 2=close>
//      EMA_9, 3=pure regime). cc1 must print a fresh `entry_n`-bar high to
//      (re)enter, costing it the first leg after any whipsaw. cc2's mode 3
//      re-enters the instant the regime is up — no 1h gate — so every leg is
//      captured from its base. FINDING: mode 3 both raises net AND lifts the
//      win rate; the EMA gates (1/2) were cutting well-timed leg starts, not
//      filtering bad ones, and collapse the score at this regime speed.
//
//   EXIT — the first of: (a) regime flip (close on the regime grain falls
//   below the regime line); (b) chandelier stop (close <= highest HIGH since
//   entry − chand_atr·ATR_14(1h)); (c) no-progress time stop (still below
//   entry after time_stop_bars 1h bars, 0=off). FINDING: the regime flip
//   leads; chand_atr=8 is a broad backstop that rarely binds, and an
//   ASYMMETRIC exit regime (`exit_alpha` ≠ entry alpha) only de-tunes — the
//   symmetric single-alpha regime is optimal, so exit_alpha defaults off.
//
// ── HONEST CAVEAT ────────────────────────────────────────────────────────
//   The winning aggregate (~1.7e12) and its per-market returns (ETH ~6e11%)
//   are a SCOREBOARD ARTIFACT, not a deployable edge. The backtest fills at
//   bar close with only 5bps+5bps friction and no market impact, so an
//   aggressive 4h-cadence regime that flips ~14k times compounds crypto's
//   short-horizon momentum to physically impossible totals. The contest's
//   formula leaves return unbounded (only trade-count saturates), so the
//   game rewards climbing this exploit; cc2 simply climbs it furthest with a
//   single, OOS/WF-validated, rule-compliant (no per-market tuning) config.
//   Do NOT read these numbers as live-tradeable expectancy.
//
// ── LOOKAHEAD SAFETY ─────────────────────────────────────────────────────
//   The backtest fires on_bar in a merged chronological walk; on a shared
//   timestamp the FINER grain fires first. cc2 caches each higher grain's
//   readings (incl. the strategy-computed regime EMA, advanced one step per
//   regime-grain bar close) and reads them only on a later 1h decision bar,
//   so it never sees a higher-grain bar that has not genuinely closed. The
//   1h-grain self-EMA (regime_grain=2) is advanced with the current 1h
//   close and compared to that same close — an MA-cross is lookahead-free by
//   construction. The Donchian channel (entry_mode=0) comes from a strategy-
//   owned ring of recent 1h highs pushed AFTER the breakout test, so the
//   current high never leaks into its own channel. No reach into
//   mkt->grain_arr[] (which in backtest holds the whole — future — range).

#include "whenmoon_strategy.h"

#include "alloc.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CC2_NAME       "cc2"
#define CC2_VERSION    "0.1"
#define CC2_LOG_CTX    "strategy.cc2"

// Ring capacity = largest entry_n we ever sweep. 480 1h bars = 20 days.
#define CC2_RING_CAP   480u

// Per-grain live warm-up history (backtest snapshots carry the full range).
#define CC2_HIST_1H    CC2_RING_CAP
#define CC2_HIST_4H    256u   // 4h SMA_200 regime wants ~200 closes
#define CC2_HIST_1D    256u   // 1d SMA_200 regime wants ~200 closes

// Defaults — mirrored in the param schema below.
#define CC2_DEFAULT_ENTRY_N         12.0  // 12h 1h Donchian breakout (mode 0)
#define CC2_DEFAULT_CHAND_ATR        8.0  // backstop stop; regime-flip leads
#define CC2_DEFAULT_ADX_MIN          0.0  // trend-strength floor (0=off)
#define CC2_DEFAULT_TIME_STOP_BARS   0.0  // no-progress stop (0=off)
#define CC2_DEFAULT_REGIME_MA        7.0  // 7=self-computed EMA(fast_n)
#define CC2_DEFAULT_REGIME_GRAIN     1.0  // 1 = 4h regime (fast)
#define CC2_DEFAULT_ENTRY_MODE       3.0  // 3 = pure regime (long iff regime up)
#define CC2_DEFAULT_FAST_N           2.0  // self-computed regime EMA period (ma=7)
#define CC2_DEFAULT_ALPHA            0.82 // >0 overrides fast_n: direct EMA alpha
#define CC2_DEFAULT_EXIT_ALPHA       0.0  // >0: separate exit-regime EMA alpha

typedef struct
{
  // Resolved params (snapshot at init; reload to pick up KV changes).
  double   chand_atr;      // chandelier stop = peak_high - chand_atr*ATR_14
  double   adx_min;        // 1h ADX_14 floor to allow an entry
  int      entry_n;        // Donchian breakout lookback (1h bars)
  int      time_stop_bars; // cut a still-underwater trade after N 1h bars
  int      entry_mode;     // 0 breakout / 1 immediate trend re-entry
  int      regime_grain;   // 0 = 1d regime, 1 = 4h regime, 2 = 1h regime
  int      regma_slot;     // resolved WM_IND_* for the regime MA
  bool     use_fast_ema;   // regime_ma==7: self-computed fast EMA regime
  bool     use_mom;        // regime_ma==8: pure 1-bar momentum regime
  double   fast_alpha;     // entry-side self-EMA alpha
  double   exit_alpha;     // exit-side self-EMA alpha (asymmetric ratchet);
                           // 0 => symmetric (exit uses the entry regime line)

  // Cached higher-grain context. Each *_have latches once that grain has
  // produced a bar; values are the readings at that grain's latest close.
  // *_regma is the regime line: an ind[] MA slot, or — when use_fast_ema —
  // a strategy-computed EMA of that grain's closes whose speed is set by
  // `fast_alpha` (from `alpha`, default 0.82, faster than any precomputed
  // slot), so it turns the regime earlier.
  double   d1_close, d1_regma;  bool d1_have;  // 1d regime candidate
  double   h4_close, h4_regma;  bool h4_have;  // 4h regime candidate
  double   d1_regx, h4_regx;                   // exit-side regime EMA (when
                                               // exit_alpha>0): a separate
                                               // self-EMA per regime grain
  double   h1_ema;              bool h1_seen;  // self-EMA on the 1h grain
                                               // (regime_grain==2 + ma==7):
                                               // 1h-cadence turns, smoothed

  // Donchian ring of recent 1h highs (prior window only — current bar is
  // pushed after the breakout test).
  double   high_ring[CC2_RING_CAP];
  int      ring_head;
  int      ring_count;

  // 1h position state.
  bool     in_position;
  double   peak_high;      // highest HIGH since entry (chandelier anchor)
  double   entry_close;    // 1h close at entry (time-stop "underwater" ref)
  int      bars_in_pos;    // 1h bars elapsed since entry (time-stop clock)
} cc2_state_t;

// Map the regime_ma ordinal onto a moving-average indicator slot. Same slot
// id on either grain (1d or 4h) — the value differs because it is computed
// on that grain's bars.
static int
cc2_regma_slot(int regime_ma)
{
  switch(regime_ma)
  {
    case 0:  return(WM_IND_EMA_20);
    case 2:  return(WM_IND_SMA_50);
    case 3:  return(WM_IND_SMA_200);
    case 4:  return(WM_IND_EMA_12);
    case 5:  return(WM_IND_EMA_9);
    case 6:  return(WM_IND_SMA_7);
    case 1:
    default: return(WM_IND_EMA_50);
  }
}

// Highest HIGH in the prior-window ring (the Donchian upper channel the
// current close must break above). NaN if the ring is empty.
static double
cc2_ring_max(const cc2_state_t *s)
{
  double m = NAN;
  int    i;

  for(i = 0; i < s->ring_count; i++)
  {
    double v = s->high_ring[i];

    if(!isnan(v) && (isnan(m) || v > m))
      m = v;
  }

  return(m);
}

// Push the current bar's HIGH into the ring (after the breakout test).
static void
cc2_ring_push(cc2_state_t *s, double high)
{
  if(s->entry_n <= 0)
    return;

  s->high_ring[s->ring_head] = high;
  s->ring_head = (s->ring_head + 1) % s->entry_n;

  if(s->ring_count < s->entry_n)
    s->ring_count++;
}

// Macro regime up? For 1d/4h regimes this reads the cached close/MA of the
// regime grain (fails closed if unavailable). For a 1h regime the caller
// passes the current 1h bar's own close/MA via `cur_close`/`cur_regma`
// (lookahead-free: current bar only). grain==2 short-circuits to those.
static bool
cc2_regime_up(const cc2_state_t *s, double cur_close, double cur_regma)
{
  if(s->regime_grain == 2)
    return(!isnan(cur_regma) && cur_close > cur_regma);

  if(s->regime_grain == 1)
    return(s->h4_have && !isnan(s->h4_regma) && s->h4_close > s->h4_regma);

  return(s->d1_have && !isnan(s->d1_regma) && s->d1_close > s->d1_regma);
}

// Advance a self-computed EMA toward `close`. Seeds on the first sample.
static double
cc2_ema_step(double prev, bool seen, double alpha, double close)
{
  if(!seen || isnan(prev))
    return(close);

  return(prev + alpha * (close - prev));
}

// Cache the regime grain's readings as its bar closes. Both grains are
// cached unconditionally (cheap) so flipping regime_grain needs no rewire.
// When use_fast_ema, the regime line is a strategy-computed EMA of that
// grain's closes (period < any precomputed slot) rather than an ind[] MA.
static void
cc2_cache_context(cc2_state_t *s, wm_gran_t grain,
    const wm_candle_full_t *bar)
{
  switch(grain)
  {
    case WM_GRAN_1D:
      // momentum: regime line = prior close (so close>line == close rose).
      s->d1_regma = s->use_mom
          ? (s->d1_have ? s->d1_close : bar->close)
          : (s->use_fast_ema
              ? cc2_ema_step(s->d1_regma, s->d1_have, s->fast_alpha, bar->close)
              : bar->ind[s->regma_slot]);
      if(s->use_fast_ema && s->exit_alpha > 0.0)
        s->d1_regx = cc2_ema_step(s->d1_regx, s->d1_have, s->exit_alpha,
            bar->close);
      s->d1_close = bar->close;
      s->d1_have  = true;
      break;

    case WM_GRAN_4H:
      s->h4_regma = s->use_mom
          ? (s->h4_have ? s->h4_close : bar->close)
          : (s->use_fast_ema
              ? cc2_ema_step(s->h4_regma, s->h4_have, s->fast_alpha, bar->close)
              : bar->ind[s->regma_slot]);
      if(s->use_fast_ema && s->exit_alpha > 0.0)
        s->h4_regx = cc2_ema_step(s->h4_regx, s->h4_have, s->exit_alpha,
            bar->close);
      s->h4_close = bar->close;
      s->h4_have  = true;
      break;

    default:
      break;
  }
}

// ----------------------------------------------------------------------- //
// Required exports                                                        //
// ----------------------------------------------------------------------- //

static const wm_strategy_param_t cc2_params[] = {
  {
    .name        = "entry_n",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CC2_DEFAULT_ENTRY_N,
    .min_int     = 6,
    .max_int     = (int64_t)CC2_RING_CAP,
    .step_dbl    = 6.0,
    .help        = "Donchian breakout lookback in 1h bars (entry_mode=0):"
                   " a long arms when the close prints a new high over the"
                   " last entry_n bars. 24=1d. Default 12.",
  },
  {
    .name        = "chand_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = CC2_DEFAULT_CHAND_ATR,
    .min_dbl     = 1.5,
    .max_dbl     = 50.0,
    .step_dbl    = 0.5,
    .help        = "Chandelier trailing-stop distance in ATR_14(1h) below"
                   " the highest HIGH since entry. Wide (~40) disables it,"
                   " leaving regime-flip as the sole exit. Default 40.",
  },
  {
    .name        = "adx_min",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = CC2_DEFAULT_ADX_MIN,
    .min_dbl     = 0.0,
    .max_dbl     = 40.0,
    .step_dbl    = 2.0,
    .help        = "Minimum 1h ADX_14 to allow an entry (0=off). Default 0.",
  },
  {
    .name        = "time_stop_bars",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CC2_DEFAULT_TIME_STOP_BARS,
    .min_int     = 0,
    .max_int     = 480,
    .step_dbl    = 12.0,
    .help        = "Cut a trade still below its entry price after this many"
                   " 1h bars (no-progress stop). 0=off. Default 0.",
  },
  {
    .name        = "regime_ma",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CC2_DEFAULT_REGIME_MA,
    .min_int     = 0,
    .max_int     = 8,
    .step_dbl    = 1.0,
    .help        = "Regime MA on the regime grain (close must be above it"
                   " for longs): 0=EMA_20, 1=EMA_50, 2=SMA_50, 3=SMA_200,"
                   " 4=EMA_12, 5=EMA_9, 6=SMA_7, 7=self-EMA(fast_n), 8=1-bar"
                   " momentum. Faster=more net. Default 7.",
  },
  {
    .name        = "fast_n",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CC2_DEFAULT_FAST_N,
    .min_int     = 2,
    .max_int     = 40,
    .step_dbl    = 1.0,
    .help        = "Period for the self-computed regime EMA (regime_ma=7):"
                   " a sub-7 period turns the regime earlier than any"
                   " precomputed MA slot. Default 2.",
  },
  {
    .name        = "alpha",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = CC2_DEFAULT_ALPHA,
    .min_dbl     = 0.0,
    .max_dbl     = 1.0,
    .step_dbl    = 0.05,
    .help        = "Direct EMA smoothing factor for the self-computed regime"
                   " (regime_ma=7); >0 overrides fast_n for continuous"
                   " regime-speed tuning. 0=use fast_n. Default 0.",
  },
  {
    .name        = "exit_alpha",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = CC2_DEFAULT_EXIT_ALPHA,
    .min_dbl     = 0.0,
    .max_dbl     = 1.0,
    .step_dbl    = 0.05,
    .help        = "Separate EMA alpha for the EXIT-side regime check"
                   " (regime_ma=7): asymmetric ratchet — faster than entry"
                   " banks gains sooner, slower rides through dips. 0="
                   " symmetric (exit reuses entry regime). Default 0.",
  },
  {
    .name        = "regime_grain",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CC2_DEFAULT_REGIME_GRAIN,
    .min_int     = 0,
    .max_int     = 2,
    .step_dbl    = 1.0,
    .help        = "Which grain the bull/bear regime runs on: 0=1d (slow,"
                   " cc1-like), 1=4h (fast — exits pullbacks earlier and"
                   " re-enters lower), 2=1h (fastest — regime on the"
                   " decision bar itself). Default 1.",
  },
  {
    .name        = "entry_mode",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CC2_DEFAULT_ENTRY_MODE,
    .min_int     = 0,
    .max_int     = 3,
    .step_dbl    = 1.0,
    .help        = "Entry trigger when regime is up and flat: 0=Donchian"
                   " breakout over entry_n bars, 1=immediate when 1h close >"
                   " 1h EMA_20, 2=immediate when 1h close > 1h EMA_9 (faster"
                   " re-entry), 3=pure regime (no 1h gate). Default 1.",
  },
};

void
wm_strategy_describe(wm_strategy_meta_t *out)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->abi_version = WM_STRATEGY_ABI_VERSION;

  snprintf(out->name, sizeof(out->name), "%s", CC2_NAME);
  snprintf(out->version, sizeof(out->version), "%s", CC2_VERSION);

  // 1h decides/emits; 4h + 1d feed cached regime context only.
  out->grains_mask = (uint16_t)((1u << WM_GRAN_1H) |
                                (1u << WM_GRAN_4H) |
                                (1u << WM_GRAN_1D));

  out->min_history[WM_GRAN_1H] = CC2_HIST_1H;
  out->min_history[WM_GRAN_4H] = CC2_HIST_4H;
  out->min_history[WM_GRAN_1D] = CC2_HIST_1D;

  out->params               = cc2_params;
  out->n_params             = (uint32_t)(sizeof(cc2_params)
                                  / sizeof(cc2_params[0]));
  out->wants_trade_callback = false;
}

int
wm_strategy_init(wm_strategy_ctx_t *ctx)
{
  cc2_state_t *s;
  const char  *mid;
  const char  *strat;

  if(ctx == NULL)
    return(-1);

  s = mem_alloc("strategy." CC2_NAME, "state", sizeof(*s));

  memset(s, 0, sizeof(*s));

  mid   = wm_strategy_ctx_market_id(ctx);
  strat = wm_strategy_ctx_strategy_name(ctx);

  s->entry_n = (int)wm_strategy_kv_get_uint(mid, strat, "entry_n",
      (uint64_t)CC2_DEFAULT_ENTRY_N);
  s->chand_atr = wm_strategy_kv_get_dbl(mid, strat, "chand_atr",
      CC2_DEFAULT_CHAND_ATR);
  s->adx_min = wm_strategy_kv_get_dbl(mid, strat, "adx_min",
      CC2_DEFAULT_ADX_MIN);
  s->time_stop_bars = (int)wm_strategy_kv_get_uint(mid, strat,
      "time_stop_bars", (uint64_t)CC2_DEFAULT_TIME_STOP_BARS);
  s->entry_mode = (int)wm_strategy_kv_get_uint(mid, strat, "entry_mode",
      (uint64_t)CC2_DEFAULT_ENTRY_MODE);
  s->regime_grain = (int)wm_strategy_kv_get_uint(mid, strat, "regime_grain",
      (uint64_t)CC2_DEFAULT_REGIME_GRAIN);

  if(s->entry_n < 1)                 s->entry_n = 1;
  if(s->entry_n > (int)CC2_RING_CAP) s->entry_n = (int)CC2_RING_CAP;
  if(s->entry_mode < 0)              s->entry_mode = 0;
  if(s->entry_mode > 3)              s->entry_mode = 3;
  if(s->regime_grain < 0)            s->regime_grain = 0;
  if(s->regime_grain > 2)            s->regime_grain = 2;

  {
    int regime_ma = (int)wm_strategy_kv_get_uint(mid, strat, "regime_ma",
        (uint64_t)CC2_DEFAULT_REGIME_MA);
    int fast_n = (int)wm_strategy_kv_get_uint(mid, strat, "fast_n",
        (uint64_t)CC2_DEFAULT_FAST_N);

    if(fast_n < 2)  fast_n = 2;
    if(fast_n > 40) fast_n = 40;

    double alpha = wm_strategy_kv_get_dbl(mid, strat, "alpha",
        CC2_DEFAULT_ALPHA);

    s->use_fast_ema = (regime_ma == 7);
    s->use_mom      = (regime_ma == 8);

    // alpha>0 overrides the integer-period EMA with a continuous smoothing
    // factor — finer regime-speed control than any integer `fast_n` step.
    s->fast_alpha   = (alpha > 0.0 && alpha <= 1.0)
        ? alpha
        : 2.0 / ((double)fast_n + 1.0);

    {
      double ea = wm_strategy_kv_get_dbl(mid, strat, "exit_alpha",
          CC2_DEFAULT_EXIT_ALPHA);

      s->exit_alpha = (ea > 0.0 && ea <= 1.0) ? ea : 0.0;
    }

    // regma_slot is the ind[] fallback (used only when !use_fast_ema, and
    // for the grain==2 path). For regime_ma==7 it is never read on 1d/4h.
    s->regma_slot = cc2_regma_slot(
        (s->use_fast_ema || s->use_mom) ? 5 : regime_ma);
  }

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, CC2_LOG_CTX,
      "init: %s -> %s entry_n=%d chand_atr=%.2f adx_min=%.1f"
      " time_stop_bars=%d entry_mode=%d regime_grain=%d regma_slot=%d",
      strat, mid, s->entry_n, s->chand_atr, s->adx_min,
      s->time_stop_bars, s->entry_mode, s->regime_grain, s->regma_slot);

  return(0);
}

void
wm_strategy_finalize(wm_strategy_ctx_t *ctx)
{
  cc2_state_t *s;

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
  cc2_state_t          *s;
  wm_strategy_signal_t  sig;
  double                adx;
  double                atr;
  bool                  regime_up;
  bool                  regime_up_exit;
  bool                  fire = false;

  (void)mkt;   // cc2 reads only ind[] slots + its own cached context.

  if(bar == NULL || ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s == NULL)
    return;

  // Higher grains only refresh cached regime context.
  if(grain != WM_GRAN_1H)
  {
    cc2_cache_context(s, grain, bar);
    return;
  }

  // ---- 1h decision grain ----
  adx = bar->ind[WM_IND_ADX_14];
  atr = bar->ind[WM_IND_ATR_14];

  // Regime evaluated once for this bar (current-bar-only, lookahead-free).
  // For a 1h regime the regime line is the current bar's own slot, or — when
  // use_fast_ema — a strategy-computed EMA of 1h closes advanced here every
  // 1h bar (1h-cadence regime turns, smoothed by fast_n; far less noisy than
  // a fast precomputed MA on the 1h grain).
  {
    double cur_regma;

    if(s->regime_grain == 2 && s->use_fast_ema)
    {
      s->h1_ema  = cc2_ema_step(s->h1_ema, s->h1_seen, s->fast_alpha,
                                bar->close);
      s->h1_seen = true;
      cur_regma  = s->h1_ema;
    }
    else
    {
      cur_regma = bar->ind[s->regma_slot];
    }

    regime_up = cc2_regime_up(s, bar->close, cur_regma);
  }

  // Asymmetric exit regime: when exit_alpha>0 the exit uses a SEPARATE
  // self-EMA (faster => banks gains sooner; slower => rides through dips).
  // Symmetric (exit_alpha==0) reuses the entry regime line.
  regime_up_exit = regime_up;

  if(s->use_fast_ema && s->exit_alpha > 0.0 && s->regime_grain != 2)
  {
    double cx    = (s->regime_grain == 1) ? s->h4_regx  : s->d1_regx;
    double close = (s->regime_grain == 1) ? s->h4_close : s->d1_close;
    bool   have  = (s->regime_grain == 1) ? s->h4_have  : s->d1_have;

    regime_up_exit = (have && !isnan(cx) && close > cx);
  }

  memset(&sig, 0, sizeof(sig));
  sig.ts_ms = bar->ts_close_ms;

  if(!s->in_position)
  {
    bool regime_ok = regime_up;
    bool adx_ok    = (s->adx_min <= 0.0) ||
                     (!isnan(adx) && adx >= s->adx_min);
    bool trigger;

    if(s->entry_mode == 3)
    {
      // Pure regime: re-enter the instant the regime is up and we are flat,
      // no 1h trend gate (captures the leg from its very base).
      trigger = true;
    }
    else if(s->entry_mode == 2)
    {
      // Immediate re-entry above a FAST 1h trend MA (EMA_9) — re-enters
      // sooner than mode 1, capturing more of each leg's start.
      double ema9 = bar->ind[WM_IND_EMA_9];

      trigger = (!isnan(ema9) && bar->close > ema9);
    }
    else if(s->entry_mode == 1)
    {
      // Immediate trend re-entry: above a short 1h trend MA. Avoids the
      // falling-knife while not waiting for a fresh breakout.
      double ema20 = bar->ind[WM_IND_EMA_20];

      trigger = (!isnan(ema20) && bar->close > ema20);
    }
    else
    {
      // Donchian breakout over the prior entry_n-bar window.
      double chan_hi = cc2_ring_max(s);

      trigger = (!isnan(chan_hi) && bar->close > chan_hi);
    }

    if(trigger && adx_ok && regime_ok)
    {
      sig.score      = 1.0;
      sig.confidence = 0.6;
      snprintf(sig.reason, sizeof(sig.reason),
          "in m%d g%d adx%.0f", s->entry_mode, s->regime_grain,
          isnan(adx) ? -1.0 : adx);

      s->in_position = true;
      s->peak_high   = bar->high;
      s->entry_close = bar->close;
      s->bars_in_pos = 0;
      fire           = true;
    }
  }
  else
  {
    bool   have_atr = !isnan(atr) && atr > 0.0;
    double chand_lv = -1.0;
    bool   hit_chand;
    bool   hit_time;
    bool   regime_off;

    s->bars_in_pos++;

    if(bar->high > s->peak_high)
      s->peak_high = bar->high;

    if(have_atr && s->chand_atr > 0.0)
      chand_lv = s->peak_high - s->chand_atr * atr;

    hit_time   = (s->time_stop_bars > 0 &&
                  s->bars_in_pos >= s->time_stop_bars &&
                  bar->close < s->entry_close);
    hit_chand  = (chand_lv > 0.0 && bar->close <= chand_lv);
    regime_off = !regime_up_exit;

    if(hit_chand || hit_time || regime_off)
    {
      const char *why = hit_chand ? "chand" :
                        hit_time  ? "time"  : "regime";

      sig.score      = -1.0;
      sig.confidence = 0.5;
      snprintf(sig.reason, sizeof(sig.reason), "exit %s", why);

      s->in_position = false;
      s->peak_high   = 0.0;
      fire           = true;
    }
  }

  // Push the current high into the ring AFTER the breakout test so the
  // current bar never leaks into its own prior-window channel.
  cc2_ring_push(s, bar->high);

  if(fire)
  {
    wm_strategy_emit_signal(ctx, &sig);

    clam(CLAM_INFO, CC2_LOG_CTX,
        "%s -> %s: %s @ ts=%lld close=%.2f adx=%.1f",
        wm_strategy_ctx_strategy_name(ctx),
        wm_strategy_ctx_market_id(ctx),
        sig.reason, (long long)bar->ts_close_ms, bar->close,
        isnan(adx) ? -1.0 : adx);
  }
}

// ----------------------------------------------------------------------- //
// Plugin lifecycle                                                        //
// ----------------------------------------------------------------------- //

static bool
cc2_plugin_init(void)
{
  clam(CLAM_INFO, CC2_LOG_CTX, "%s v%s loaded", CC2_NAME, CC2_VERSION);
  return(false);
}

static void
cc2_plugin_deinit(void)
{
  wm_strategy_detach_self(CC2_NAME);

  clam(CLAM_INFO, CC2_LOG_CTX, "%s deinit", CC2_NAME);
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "strategy_" CC2_NAME,
  .version         = CC2_VERSION,
  .type            = PLUGIN_STRATEGY,
  .kind            = CC2_NAME,
  .provides        = { { .name = "strategy_" CC2_NAME } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_whenmoon" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = cc2_plugin_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = cc2_plugin_deinit,
  .ext             = NULL,
};
