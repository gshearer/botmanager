// botmanager — MIT
// cp2 ("Claude's Play #2") — multi-timeframe trend-aligned pullback swing
// (long-only). Where cp1 makes every decision from the 5m grain alone,
// cp2 reads ALL six grains and gives each one a distinct job, so the
// entry is the confluence of macro trend + intermediate trend + a fresh
// pullback + a short-term momentum thrust:
//
//   1d  regime  : longs only when the daily close is above its regime
//                 MA (`regime_ma`: EMA_20 default / EMA_50 / SMA_50 /
//                 SMA_200). cp1's "stay out of the bear" edge lifted to
//                 the daily grain; also the macro exit backstop. The
//                 MA speed is the dominant lever on net return — a
//                 faster EMA_20 re-enters recoveries + post-pullback
//                 trends sooner and captures much more of BTC's runs.
//   4h  trend   : EMA_20 > EMA_50 AND MACD_HIST > 0 — the intermediate
//                 trend agrees with the daily.
//   1h  momentum: close > EMA_50 AND RSI_14 > 50 — no entering into a
//                 1h downdraft.
//   15m pullback: RSI_14 dips to <= pullback_rsi. This ARMS a long: in
//                 an uptrend a shallow RSI dip is a buyable pullback, not
//                 a reversal. The arm latches until an entry consumes it.
//   5m  trigger : the decision/emit grain. While armed AND the higher
//                 grains are aligned bullish, fire a long (score=+1) when
//                 5m RSI_14 crosses UP through trigger_rsi (momentum
//                 resuming). Disarm on entry — re-entry needs a fresh
//                 15m pullback.
//   1m  confirm : optional micro-filter (htf_mode 3): require the last
//                 closed 1m close > its EMA_9, i.e. price is ticking up
//                 right now.
//
//   exit (5m)   : the FIRST of — the wide volatility trailing stop
//                 close <= peak_close - exit_atr*ATR_14 (lets winners
//                 run), a no-progress time stop (still below entry after
//                 time_stop_bars 5m bars => the entry stalled; cut it),
//                 OR the daily regime flips down. The losing trades are
//                 indistinguishable from winners at entry, so they can
//                 only be weeded out post-entry: losers stall (their hold
//                 runs ~half a winner's) while a real winner is already
//                 above entry, so the time stop trims the dead trades
//                 without whipsawing the runners.
//
// `htf_mode` is a single ordinal that bundles how many higher-grain
// gates must align (0 = daily only … 3 = +1m confirm). One knob trades
// frequency against selectivity, which is far less overfit-prone than a
// pile of independent toggles.
//
// LOOKAHEAD SAFETY. The whenmoon backtest fires on_bar in a merged
// chronological walk across grains; on a shared timestamp the FINER
// grain fires first (5m before 15m before … before 1d). So when cp2
// caches a higher-grain bar's indicators as that grain's on_bar fires,
// and reads the cache on a later 5m bar, it only ever sees higher-grain
// bars that have genuinely closed in the past. There is no peek into
// mkt->grain_arr[] (which in backtest is fully populated with the whole
// date range — i.e. the future). Each cached value is the indicator at
// that higher-grain bar's own close, exactly as in live.

#include "whenmoon_strategy.h"

#include "alloc.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CP2_NAME       "cp2"
#define CP2_VERSION    "0.1"
#define CP2_LOG_CTX    "strategy.cp2"

// Per-grain warm-up history (bars) fetched on live attach so every
// grain's indicators are warm at cold start. Sized for the default
// EMA_20 daily regime + the 4h EMA_50 gate; pick regime_ma=SMA_200 and
// the daily warm-up wants more (the backtest snapshot carries the full
// date range regardless of these, so this only matters live).
#define CP2_HIST_1M    64u
#define CP2_HIST_5M    64u
#define CP2_HIST_15M   48u
#define CP2_HIST_1H    64u
#define CP2_HIST_4H    64u
#define CP2_HIST_1D    64u

// Defaults — mirrored in the param schema below. Validated on 3y
// BTC-USD (coinbase, 2022→2025): full-sample pf **5.13** / **+92% net**
// of fees / ~54% win, with the time_stop_bars=384 default cutting gross
// loss ~19% vs the time-stop-off baseline (pf 4.32, +91%). Walk-forward
// (11 windows, train=180d test=90d step=90d) pf **5.15** (vs 4.26 off)
// — the time stop's loss-weeding holds out of sample across the 2022
// bear + 2023 recovery + 2024 bull, where it pays off most in chop/bear.
//
// Three load-bearing findings: (1) htf_mode=0 (daily only, no 4h
// confirm) is break-even-to-losing — the 4h gate carries the entry
// edge, the whole multi-timeframe thesis. (2) the daily regime MA
// *speed* is the dominant lever on net: EMA_20 (this default) nets
// ~+91% vs EMA_50's ~+48% by getting long earlier in trends. `exit_atr`
// spans the frequency/net frontier — 6→~+48%/2.2 trips-wk (active
// swing), 15→+91%/1.2 trips-wk (this default), 100→~+137%/0.3 trips-wk
// (rides each daily-uptrend leg whole; pf there is inflated by per-fill
// equity sampling — see the on_bar note). (3) losing trades carry no
// entry signature — even the worst losers match the all-trade medians
// on every higher-grain gate — so entry filters and price stops only
// add whipsaw; the no-progress time stop is the one lever that weeds
// losses, because it keys off post-entry stalling, not entry context.
// Reported drawdowns are per-fill, so they understate the true
// mark-to-market give-back of held trends.
#define CP2_DEFAULT_PULLBACK_RSI   40.0
#define CP2_DEFAULT_TRIGGER_RSI    54.0
#define CP2_DEFAULT_EXIT_ATR       15.0
#define CP2_DEFAULT_TIME_STOP_BARS 384.0  // no-progress time stop (32h); 0=off
#define CP2_DEFAULT_HTF_MODE        1.0
#define CP2_DEFAULT_REGIME_MA       0.0   // 0 EMA20 / 1 EMA50 / 2 SMA50 / 3 SMA200

typedef struct
{
  // Resolved params (snapshot at init; reload to pick up KV changes).
  double   pullback_rsi;   // 15m RSI <= this arms a pullback long
  double   trigger_rsi;    // 5m RSI up-cross through this fires entry
  double   exit_atr;       // trailing stop = peak - exit_atr * ATR_14(5m)
  int      time_stop_bars; // cut a still-underwater trade after N 5m bars
  int      htf_mode;       // 0 daily / 1 +4h / 2 +1h / 3 +1m confirm
  int      regma_slot;     // resolved WM_IND_* for the daily regime MA

  // Cached higher-grain context. Each *_have latches true once that
  // grain has produced at least one bar; the values are the indicator
  // readings at that grain's most recent close. NaN-guarded on read.
  double   d1_close, d1_regma;                  bool d1_have;   // 1d regime
  double   h4_ema20, h4_ema50, h4_macd_hist;    bool h4_have;   // 4h trend
  double   h1_close, h1_ema50, h1_rsi;          bool h1_have;   // 1h momentum
  double   m1_close, m1_ema9;                   bool m1_have;   // 1m confirm

  // 5m decision-grain state.
  double   prev_rsi5;      // previous 5m RSI_14 (for the up-cross test)
  bool     have_prev_rsi5;
  bool     armed;          // a 15m pullback has occurred, awaiting trigger
  bool     in_position;
  double   peak_close;     // highest 5m close since entry (trail reference)
  double   entry_close;    // 5m close at entry (time-stop "underwater" ref)
  int      bars_in_pos;    // 5m bars elapsed since entry (time-stop clock)
} cp2_state_t;

// Map the regime_ma ordinal onto a daily moving-average indicator slot.
// A faster MA (EMA_20) re-enters recoveries sooner and exits sooner; a
// slower MA (SMA_200) holds major trends longer at the cost of late
// entries. The regime gates entry AND is the exit backstop.
static int
cp2_regma_slot(int regime_ma)
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

// ----------------------------------------------------------------------- //
// Higher-timeframe alignment                                             //
// ----------------------------------------------------------------------- //

// Is the multi-timeframe trend bullish enough to allow a long right now?
// Evaluated at 5m entry time against the cached higher-grain snapshots.
// `htf_mode` is inclusive: each higher level adds a gate on top of the
// ones below it. A grain that has not produced a bar yet (or whose gate
// indicator is still NaN) fails closed — we do not trade on unknown
// context.
static bool
cp2_htf_bullish(const cp2_state_t *s)
{
  // Daily regime is always required.
  if(!s->d1_have || isnan(s->d1_regma) || !(s->d1_close > s->d1_regma))
    return(false);

  if(s->htf_mode >= 1)
  {
    if(!s->h4_have || isnan(s->h4_ema20) || isnan(s->h4_ema50) ||
       isnan(s->h4_macd_hist))
      return(false);

    if(!(s->h4_ema20 > s->h4_ema50 && s->h4_macd_hist > 0.0))
      return(false);
  }

  if(s->htf_mode >= 2)
  {
    if(!s->h1_have || isnan(s->h1_ema50) || isnan(s->h1_rsi))
      return(false);

    if(!(s->h1_close > s->h1_ema50 && s->h1_rsi > 50.0))
      return(false);
  }

  if(s->htf_mode >= 3)
  {
    if(!s->m1_have || isnan(s->m1_ema9))
      return(false);

    if(!(s->m1_close > s->m1_ema9))
      return(false);
  }

  return(true);
}

// Daily macro trend still up? Used as the exit backstop. Fails closed
// if the daily bar or its EMA_50 is not available.
static bool
cp2_regime_up(const cp2_state_t *s)
{
  return(s->d1_have && !isnan(s->d1_regma) && s->d1_close > s->d1_regma);
}

// ----------------------------------------------------------------------- //
// Per-grain context caching                                              //
// ----------------------------------------------------------------------- //

static void
cp2_cache_context(cp2_state_t *s, wm_gran_t grain,
    const wm_candle_full_t *bar)
{
  switch(grain)
  {
    case WM_GRAN_1D:
      s->d1_close = bar->close;
      s->d1_regma = bar->ind[s->regma_slot];
      s->d1_have  = true;
      break;

    case WM_GRAN_4H:
      s->h4_ema20     = bar->ind[WM_IND_EMA_20];
      s->h4_ema50     = bar->ind[WM_IND_EMA_50];
      s->h4_macd_hist = bar->ind[WM_IND_MACD_HIST];
      s->h4_have      = true;
      break;

    case WM_GRAN_1H:
      s->h1_close = bar->close;
      s->h1_ema50 = bar->ind[WM_IND_EMA_50];
      s->h1_rsi   = bar->ind[WM_IND_RSI_14];
      s->h1_have  = true;
      break;

    case WM_GRAN_15M:
    {
      double rsi15 = bar->ind[WM_IND_RSI_14];

      // Arm a long on a fresh pullback: RSI dips into the buy-the-dip
      // zone. Latches until an entry consumes it.
      if(!isnan(rsi15) && rsi15 <= s->pullback_rsi)
        s->armed = true;
      break;
    }

    case WM_GRAN_1M:
      s->m1_close = bar->close;
      s->m1_ema9  = bar->ind[WM_IND_EMA_9];
      s->m1_have  = true;
      break;

    default:
      break;
  }
}

// ----------------------------------------------------------------------- //
// Required exports                                                        //
// ----------------------------------------------------------------------- //

static const wm_strategy_param_t cp2_params[] = {
  {
    .name        = "pullback_rsi",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = CP2_DEFAULT_PULLBACK_RSI,
    .min_dbl     = 25.0,
    .max_dbl     = 48.0,
    .step_dbl    = 3.0,
    .help        = "15m RSI_14 must dip to <= this to arm a long"
                   " (buyable pullback within an uptrend). Default 40.",
  },
  {
    .name        = "trigger_rsi",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = CP2_DEFAULT_TRIGGER_RSI,
    .min_dbl     = 45.0,
    .max_dbl     = 62.0,
    .step_dbl    = 3.0,
    .help        = "While armed, go long when 5m RSI_14 crosses UP"
                   " through this (momentum resuming). Default 54.",
  },
  {
    .name        = "exit_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = CP2_DEFAULT_EXIT_ATR,
    .min_dbl     = 1.5,
    .max_dbl     = 100.0,
    .step_dbl    = 1.0,
    .help        = "Trailing stop distance in ATR_14(5m) below the peak"
                   " close since entry. Wider = ride trends longer (more"
                   " net, fewer trades); a very large value (~100) makes"
                   " the exit effectively daily-regime-only. Default 15.",
  },
  {
    .name        = "time_stop_bars",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CP2_DEFAULT_TIME_STOP_BARS,
    .min_int     = 0,
    .max_int     = 576,
    .step_dbl    = 24.0,
    .help        = "Cut a trade still below its entry price after this many"
                   " 5m bars (no-progress stop; a winner is already up by"
                   " then). 0 = off. 288 bars = 1 day. Default 384 (32h).",
  },
  {
    .name        = "htf_mode",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CP2_DEFAULT_HTF_MODE,
    .min_int     = 0,
    .max_int     = 3,
    .step_dbl    = 1.0,
    .help        = "How many higher-grain gates must align: 0=daily"
                   " only, 1=+4h trend, 2=+1h momentum, 3=+1m confirm."
                   " Default 1.",
  },
  {
    .name        = "regime_ma",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CP2_DEFAULT_REGIME_MA,
    .min_int     = 0,
    .max_int     = 3,
    .step_dbl    = 1.0,
    .help        = "Daily regime MA (close must be above it to allow"
                   " longs; below it forces exit): 0=EMA_20, 1=EMA_50,"
                   " 2=SMA_50, 3=SMA_200. Faster=more net. Default 0.",
  },
};

void
wm_strategy_describe(wm_strategy_meta_t *out)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->abi_version = WM_STRATEGY_ABI_VERSION;

  snprintf(out->name, sizeof(out->name), "%s", CP2_NAME);
  snprintf(out->version, sizeof(out->version), "%s", CP2_VERSION);

  // Subscribe to every grain — cp2's whole premise is multi-timeframe
  // confluence. on_bar fires on each; only the 5m branch decides/emits.
  out->grains_mask = (uint16_t)((1u << WM_GRAN_1M)  |
                                (1u << WM_GRAN_5M)  |
                                (1u << WM_GRAN_15M) |
                                (1u << WM_GRAN_1H)  |
                                (1u << WM_GRAN_4H)  |
                                (1u << WM_GRAN_1D));

  out->min_history[WM_GRAN_1M]  = CP2_HIST_1M;
  out->min_history[WM_GRAN_5M]  = CP2_HIST_5M;
  out->min_history[WM_GRAN_15M] = CP2_HIST_15M;
  out->min_history[WM_GRAN_1H]  = CP2_HIST_1H;
  out->min_history[WM_GRAN_4H]  = CP2_HIST_4H;
  out->min_history[WM_GRAN_1D]  = CP2_HIST_1D;

  out->params               = cp2_params;
  out->n_params             = (uint32_t)(sizeof(cp2_params)
                                  / sizeof(cp2_params[0]));
  out->wants_trade_callback = false;
}

int
wm_strategy_init(wm_strategy_ctx_t *ctx)
{
  cp2_state_t *s;
  const char  *mid;
  const char  *strat;

  if(ctx == NULL)
    return(-1);

  s = mem_alloc("strategy." CP2_NAME, "state", sizeof(*s));

  memset(s, 0, sizeof(*s));

  mid   = wm_strategy_ctx_market_id(ctx);
  strat = wm_strategy_ctx_strategy_name(ctx);

  s->pullback_rsi = wm_strategy_kv_get_dbl(mid, strat, "pullback_rsi",
      CP2_DEFAULT_PULLBACK_RSI);
  s->trigger_rsi  = wm_strategy_kv_get_dbl(mid, strat, "trigger_rsi",
      CP2_DEFAULT_TRIGGER_RSI);
  s->exit_atr     = wm_strategy_kv_get_dbl(mid, strat, "exit_atr",
      CP2_DEFAULT_EXIT_ATR);
  s->time_stop_bars = (int)wm_strategy_kv_get_uint(mid, strat,
      "time_stop_bars", (uint64_t)CP2_DEFAULT_TIME_STOP_BARS);
  s->htf_mode     = (int)wm_strategy_kv_get_uint(mid, strat, "htf_mode",
      (uint64_t)CP2_DEFAULT_HTF_MODE);

  if(s->htf_mode < 0) s->htf_mode = 0;
  if(s->htf_mode > 3) s->htf_mode = 3;

  {
    int regime_ma = (int)wm_strategy_kv_get_uint(mid, strat, "regime_ma",
        (uint64_t)CP2_DEFAULT_REGIME_MA);

    s->regma_slot = cp2_regma_slot(regime_ma);
  }

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, CP2_LOG_CTX,
      "init: %s -> %s pullback_rsi=%.1f trigger_rsi=%.1f exit_atr=%.1f"
      " time_stop_bars=%d htf_mode=%d regma_slot=%d (multi-grain)",
      strat, mid, s->pullback_rsi, s->trigger_rsi, s->exit_atr,
      s->time_stop_bars, s->htf_mode, s->regma_slot);

  return(0);
}

void
wm_strategy_finalize(wm_strategy_ctx_t *ctx)
{
  cp2_state_t *s;

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
  cp2_state_t          *s;
  wm_strategy_signal_t  sig;
  double                rsi5;
  double                atr;
  bool                  fire = false;

  (void)mkt;   // cp2 reads only ind[] slots + its own cached context.

  if(bar == NULL || ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s == NULL)
    return;

  // Higher grains only update cached context (+ the 15m arm). They never
  // emit; the 5m branch below is the sole decision point.
  if(grain != WM_GRAN_5M)
  {
    cp2_cache_context(s, grain, bar);
    return;
  }

  // ---- 5m decision grain ----
  rsi5 = bar->ind[WM_IND_RSI_14];
  atr  = bar->ind[WM_IND_ATR_14];

  memset(&sig, 0, sizeof(sig));
  sig.ts_ms = bar->ts_close_ms;

  if(!s->in_position)
  {
    // Entry: armed by a recent 15m pullback, the multi-grain trend is
    // aligned bullish, and 5m RSI crosses UP through trigger_rsi.
    bool trig = s->have_prev_rsi5 && !isnan(rsi5) &&
                s->prev_rsi5 <= s->trigger_rsi && rsi5 > s->trigger_rsi;

    if(s->armed && trig && cp2_htf_bullish(s))
    {
      sig.score      = 1.0;
      sig.confidence = 0.6;
      snprintf(sig.reason, sizeof(sig.reason),
          "pull+trig>%.0f m%d", s->trigger_rsi, s->htf_mode);

      s->in_position = true;
      s->armed       = false;
      s->peak_close  = bar->close;
      s->entry_close = bar->close;
      s->bars_in_pos = 0;
      fire           = true;
    }
  }
  else
  {
    bool   have_atr = !isnan(atr) && atr > 0.0;
    double trail_lv = -1.0;
    bool   hit_trail;
    bool   hit_time;
    bool   regime_off;

    s->bars_in_pos++;

    if(bar->close > s->peak_close)
      s->peak_close = bar->close;

    // Wide volatility trailing stop (peak-anchored, current ATR) — lets
    // winners run through pullbacks.
    if(have_atr && s->exit_atr > 0.0)
      trail_lv = s->peak_close - s->exit_atr * atr;

    // No-progress time stop: a trade still below its entry price after
    // time_stop_bars 5m bars has not worked. The deep losers are
    // indistinguishable from winners at entry AND give back the full
    // trail width before exiting, so price-based gating cannot weed them
    // out cheaply. But losers stall — their median hold runs roughly half
    // a winner's — while a genuine winner is already above entry by then
    // (left untouched, to the trail). Cutting the stalled trades early
    // removes ~a fifth of the gross loss without whipsawing winners. Off
    // at 0.
    hit_time   = (s->time_stop_bars > 0 &&
                  s->bars_in_pos >= s->time_stop_bars &&
                  bar->close < s->entry_close);
    hit_trail  = (trail_lv > 0.0 && bar->close <= trail_lv);
    regime_off = !cp2_regime_up(s);

    if(hit_trail || hit_time || regime_off)
    {
      const char *why = hit_trail ? "trail" :
                        hit_time  ? "time"  : "regime";

      sig.score      = -1.0;
      sig.confidence = 0.5;
      snprintf(sig.reason, sizeof(sig.reason), "exit %s", why);

      s->in_position = false;
      s->peak_close  = 0.0;
      fire           = true;
    }
  }

  if(fire)
  {
    wm_strategy_emit_signal(ctx, &sig);

    clam(CLAM_INFO, CP2_LOG_CTX,
        "%s -> %s: %s @ ts=%lld close=%.2f rsi5=%.1f",
        wm_strategy_ctx_strategy_name(ctx),
        wm_strategy_ctx_market_id(ctx),
        sig.reason, (long long)bar->ts_close_ms, bar->close,
        isnan(rsi5) ? -1.0 : rsi5);
  }

  // Track the 5m RSI for the next bar's up-cross test. Updated every 5m
  // bar regardless of position so cross detection stays continuous.
  if(!isnan(rsi5))
  {
    s->prev_rsi5      = rsi5;
    s->have_prev_rsi5 = true;
  }
}

// ----------------------------------------------------------------------- //
// Plugin lifecycle                                                        //
// ----------------------------------------------------------------------- //

static bool
cp2_plugin_init(void)
{
  clam(CLAM_INFO, CP2_LOG_CTX, "%s v%s loaded", CP2_NAME, CP2_VERSION);
  return(false);
}

static void
cp2_plugin_deinit(void)
{
  wm_strategy_detach_self(CP2_NAME);

  clam(CLAM_INFO, CP2_LOG_CTX, "%s deinit", CP2_NAME);
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "strategy_" CP2_NAME,
  .version         = CP2_VERSION,
  .type            = PLUGIN_STRATEGY,
  .kind            = CP2_NAME,
  .provides        = { { .name = "strategy_" CP2_NAME } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_whenmoon" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = cp2_plugin_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = cp2_plugin_deinit,
  .ext             = NULL,
};
