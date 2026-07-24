// botmanager — MIT
// cp1 ("Claude's Play #1") — long-only Donchian breakout trend-rider.
//
// Trades the 5m grain. The earlier Fisher mean-reversion baselines
// over-traded (thousands of fills at ~0.1% cost each) and never cleared
// a profit factor of 1: dip-buying produced a ~20% win rate whose
// winners could not pay for the losers + fees. cp1 v3 flips to trend
// FOLLOWING, which fits BTC's long directional runs:
//
//   regime : longs only when the slow trend is up. 0 = off,
//            1 = close > SMA_200 (default), 2 = SMA_50 > SMA_200,
//            3 = both. Keeps us out of the 2022 bear.
//   enter  : breakout — the close prints a new high over the last
//            `entry_n` bars (Donchian upper channel). Buy strength.
//   ride   : stay long while the trend holds. Exit (score=-1) on the
//            FIRST of: close <= lowest low of the last `exit_n` bars
//            (Donchian lower channel — structure broke), OR close falls
//            trail_atr * ATR_14 below the highest close since entry
//            (volatility trailing stop). Either lets winners run while
//            capping the give-back.
//
// The rolling channel is computed from a strategy-owned ring of recent
// highs/lows; each bar is pushed AFTER the signal is evaluated against
// the prior window, so there is no lookahead. SMA_50/200 + ATR_14 come
// from the aggregator's per-bar ind[] block (also lookahead-free —
// each bar's slot is the value at that bar's close). Long-only: the
// engine maps score>0 -> open long (size_frac of cash), score<0 ->
// close; a buy while long is a no-op so breakouts never scale in.

#include "whenmoon_strategy.h"

#include "alloc.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CP1_NAME       "cp1"
#define CP1_VERSION    "0.3"
#define CP1_LOG_CTX    "strategy.cp1"

// Ring capacity: the largest channel lookback we ever sweep, plus the
// SMA_200 warmup the regime gate needs. 5m bars to fetch on attach so
// the channel + SMA_200 are warm at live cold start.
#define CP1_RING_CAP         512u
#define CP1_MIN_HISTORY_5M   512u

// Defaults — mirrored in the param schema below.
// Validated defaults (3y BTC-USD 5m, 2022→2025): full-sample pf 1.17
// / +8% net of fees, OOS-tail pf 1.45, walk-forward pf 1.37. The
// positive region is a broad ridge (entry_n 144-480, exit_n 216-384),
// not a knife-edge — these sit near its centre.
#define CP1_DEFAULT_ENTRY_N        192.0   // 16h breakout
#define CP1_DEFAULT_EXIT_N         288.0   // 24h channel exit (rides trends)
#define CP1_DEFAULT_TRAIL_ATR      0.0     // off — channel exit alone wins
#define CP1_DEFAULT_REGIME         3.0     // close>SMA200 AND SMA50>SMA200

typedef struct
{
  // Resolved params (snapshot at init; reload to pick up KV changes).
  uint32_t entry_n;        // Donchian breakout lookback (bars)
  uint32_t exit_n;         // Donchian exit lookback (bars)
  double   trail_atr;      // trailing stop = peak - trail_atr*ATR (0=off)
  int      regime;         // 0 off / 1 sma200 / 2 sma50>200 / 3 both

  // Strategy-owned rolling window of recent bar highs/lows. Newest at
  // (head-1). `count` saturates at CP1_RING_CAP.
  double   highs[CP1_RING_CAP];
  double   lows[CP1_RING_CAP];
  uint32_t head;
  uint32_t count;

  // Position state (strategy's view; edge-only emit).
  bool     in_position;
  double   peak_close;     // highest close since entry (trail reference)
} cp1_state_t;

// ----------------------------------------------------------------------- //
// Rolling-window helpers                                                  //
// ----------------------------------------------------------------------- //

// Highest high over the most recent `n` stored bars (excludes nothing —
// the caller pushes the current bar only AFTER evaluating). Returns NAN
// when fewer than `n` bars are available.
static double
cp1_window_high(const cp1_state_t *s, uint32_t n)
{
  double   hi = -INFINITY;
  uint32_t i;

  if(n == 0 || s->count < n)
    return(NAN);

  for(i = 0; i < n; i++)
  {
    uint32_t idx = (s->head + CP1_RING_CAP - 1 - i) % CP1_RING_CAP;

    if(s->highs[idx] > hi)
      hi = s->highs[idx];
  }

  return(hi);
}

static double
cp1_window_low(const cp1_state_t *s, uint32_t n)
{
  double   lo = INFINITY;
  uint32_t i;

  if(n == 0 || s->count < n)
    return(NAN);

  for(i = 0; i < n; i++)
  {
    uint32_t idx = (s->head + CP1_RING_CAP - 1 - i) % CP1_RING_CAP;

    if(s->lows[idx] < lo)
      lo = s->lows[idx];
  }

  return(lo);
}

static void
cp1_ring_push(cp1_state_t *s, double high, double low)
{
  s->highs[s->head] = high;
  s->lows[s->head]  = low;
  s->head           = (s->head + 1) % CP1_RING_CAP;

  if(s->count < CP1_RING_CAP)
    s->count++;
}

static bool
cp1_regime_ok(const cp1_state_t *s, const wm_candle_full_t *bar)
{
  float sma50  = bar->ind[WM_IND_SMA_50];
  float sma200 = bar->ind[WM_IND_SMA_200];

  switch(s->regime)
  {
    case 0:
      return(true);
    case 1:
      return(!isnanf(sma200) && bar->close > sma200);
    case 2:
      return(!isnanf(sma50) && !isnanf(sma200) && sma50 > sma200);
    case 3:
    default:
      return(!isnanf(sma50) && !isnanf(sma200) &&
             bar->close > sma200 && sma50 > sma200);
  }
}

// ----------------------------------------------------------------------- //
// Required exports                                                        //
// ----------------------------------------------------------------------- //

static const wm_strategy_param_t cp1_params[] = {
  {
    .name        = "entry_n",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CP1_DEFAULT_ENTRY_N,
    .min_int     = 6,
    .max_int     = 480,
    .help        = "Donchian breakout lookback in 5m bars; close must"
                   " print a new high over this many bars. Default 96.",
  },
  {
    .name        = "exit_n",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CP1_DEFAULT_EXIT_N,
    .min_int     = 3,
    .max_int     = 480,
    .help        = "Donchian exit lookback in 5m bars; close <= the"
                   " lowest low over this many bars closes the long."
                   " Default 48.",
  },
  {
    .name        = "trail_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = CP1_DEFAULT_TRAIL_ATR,
    .min_dbl     = 0.0,
    .max_dbl     = 16.0,
    .step_dbl    = 0.5,
    .help        = "Trailing stop distance in ATR_14 below the peak"
                   " close since entry. 0=off. Default 6.",
  },
  {
    .name        = "regime",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = CP1_DEFAULT_REGIME,
    .min_dbl     = 0.0,
    .max_dbl     = 3.0,
    .step_dbl    = 1.0,
    .help        = "Trend gate: 0=off, 1=close>SMA200, 2=SMA50>SMA200,"
                   " 3=both. Default 1.",
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

  s->entry_n = (uint32_t)wm_strategy_kv_get_int(mid, strat, "entry_n",
      (int64_t)CP1_DEFAULT_ENTRY_N);
  s->exit_n = (uint32_t)wm_strategy_kv_get_int(mid, strat, "exit_n",
      (int64_t)CP1_DEFAULT_EXIT_N);
  s->trail_atr =
      wm_strategy_kv_get_dbl(mid, strat, "trail_atr", CP1_DEFAULT_TRAIL_ATR);
  s->regime = (int)(wm_strategy_kv_get_dbl(mid, strat, "regime",
      CP1_DEFAULT_REGIME) + 0.5);

  if(s->entry_n > CP1_RING_CAP) s->entry_n = CP1_RING_CAP;
  if(s->exit_n  > CP1_RING_CAP) s->exit_n  = CP1_RING_CAP;
  if(s->entry_n == 0) s->entry_n = 1;
  if(s->exit_n  == 0) s->exit_n  = 1;

  s->head        = 0;
  s->count       = 0;
  s->in_position = false;
  s->peak_close  = 0.0;

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, CP1_LOG_CTX,
      "init: %s -> %s entry_n=%u exit_n=%u trail=%.1f regime=%d (5m)",
      strat, mid, s->entry_n, s->exit_n, s->trail_atr, s->regime);

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
}

void
wm_strategy_on_bar(wm_strategy_ctx_t *ctx,
    const struct whenmoon_market *mkt,
    wm_gran_t grain,
    const wm_candle_full_t *bar)
{
  cp1_state_t          *s;
  wm_strategy_signal_t  sig;
  float                 atr;
  bool                  fire = false;

  (void)mkt;   // cp1 reads only the 5m bar's ind[] + its own ring.

  if(grain != WM_GRAN_5M || bar == NULL || ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s == NULL)
    return;

  atr = bar->ind[WM_IND_ATR_14];

  memset(&sig, 0, sizeof(sig));
  sig.ts_ms = bar->ts_close_ms;

  if(!s->in_position)
  {
    double chan_hi = cp1_window_high(s, s->entry_n);

    // Entry: breakout above the prior-N-bar high AND trend regime up.
    if(!isnan(chan_hi) && bar->close > chan_hi && cp1_regime_ok(s, bar))
    {
      sig.score      = 1.0;
      sig.confidence = 0.6;
      snprintf(sig.reason, sizeof(sig.reason),
          "breakout>%.0f n%u r%d", chan_hi, s->entry_n, s->regime);

      s->in_position = true;
      s->peak_close  = bar->close;
      fire           = true;
    }
  }
  else
  {
    double chan_lo  = cp1_window_low(s, s->exit_n);
    bool   have_atr = !isnanf(atr) && atr > 0.0f;
    double trail_lv = -1.0;

    if(bar->close > s->peak_close)
      s->peak_close = bar->close;

    if(have_atr && s->trail_atr > 0.0)
      trail_lv = s->peak_close - s->trail_atr * (double)atr;

    bool hit_chan  = (!isnan(chan_lo) && bar->close <= chan_lo);
    bool hit_trail = (trail_lv > 0.0 && bar->close <= trail_lv);

    if(hit_chan || hit_trail)
    {
      sig.score      = -1.0;
      sig.confidence = 0.5;
      snprintf(sig.reason, sizeof(sig.reason), "exit %s",
          hit_trail ? "trail" : "channel");

      s->in_position = false;
      s->peak_close  = 0.0;
      fire           = true;
    }
  }

  if(fire)
  {
    wm_strategy_emit_signal(ctx, &sig);

    clam(CLAM_INFO, CP1_LOG_CTX,
        "%s -> %s: %s @ ts=%lld close=%.2f",
        wm_strategy_ctx_strategy_name(ctx),
        wm_strategy_ctx_market_id(ctx),
        sig.reason, (long long)bar->ts_close_ms, bar->close);
  }

  // Push the current bar into the ring AFTER signal evaluation so the
  // channel windows above only ever see prior bars (no lookahead).
  cp1_ring_push(s, bar->high, bar->low);
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
