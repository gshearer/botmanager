// botmanager — MIT
// cc1 ("Claude Competitor #1") — long-only regime trend-rider, designed to
// be tuned PER MARKET (BTC/ETH/XRP/SOL each via the whenmoon per-market KV
// override `plugin.whenmoon.market.<id>.strategy.cc1.*`), which a single
// global config cannot match.
//
// Thesis: over a multi-year secular crypto bull, compound hardest by staying
// long while a fast trend regime is up, banking each leg on a tight
// volatility trailing stop, and immediately re-deploying the grown cash on
// the next up-bar. The framework samples equity per fill and re-deploys
// size_frac of grown cash, so MORE clean banked up-legs => more compounding.
//
//   regime (entry) : long allowed only while the close on `regime_grain`
//                    (1d/4h/1h) is above its regime line. The line is a
//                    precomputed MA slot (`regime_ma` 0..6) OR, when
//                    `regime_ma`==7, a STRATEGY-COMPUTED EMA of that grain's
//                    closes whose smoothing is `alpha` in (0,1] directly
//                    (or 2/(fast_n+1) when alpha<=0). Faster = more legs.
//   regime (exit)  : same line, OR — when `exit_alpha`>0 — a SEPARATE,
//                    independently-smoothed self-EMA (asymmetric ratchet:
//                    fast entry / slow exit holds a leg through pullbacks).
//   entry          : `entry_mode` 0 = breakout (new entry_n-bar high),
//                    1 = immediate (enter whenever flat and the regime is
//                    up). An ADX_14 floor (`adx_min`, 0 = off) gates either.
//   exit (1h)      : the FIRST of — the exit regime flips down, a chandelier
//                    stop (close <= peak_high - chand_atr*ATR_14; 0 = off),
//                    or a no-progress time stop (still below entry after
//                    time_stop_bars 1h bars; 0 = off). A TIGHT chand_atr
//                    banks each leg early so the immediate re-entry can
//                    re-compound from a lower base.
//
// Decision/emit grain is 1h; 4h + 1d feed cached regime context only.
//
// LOOKAHEAD SAFETY. The backtest fires on_bar in a merged chronological
// walk; on a shared timestamp the FINER grain fires first. Each grain's
// regime line + close are cached in that grain's own on_bar branch; the 1h
// decision reads the cache, so it only ever sees higher-grain bars that have
// genuinely closed in its past. The 1h regime line is advanced on the 1h bar
// itself. The Donchian ring is pushed AFTER the breakout test. No reach into
// mkt->grain_arr[] (which in backtest holds the whole, future, range).

#include "whenmoon_strategy.h"

#include "alloc.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CC1_NAME       "cc1"
#define CC1_VERSION    "0.6"
#define CC1_LOG_CTX    "strategy.cc1"

#define CC1_RING_CAP        480u

#define CC1_HIST_1H    CC1_RING_CAP
#define CC1_HIST_4H    64u
#define CC1_HIST_1D    256u

#define CC1_REGMA_SELF_EMA  7
// regime_ma == 8: dual self-EMA CROSS regime. Long while a FAST self-EMA
// (alpha) of the grain's closes is above a SLOW self-EMA (slow_alpha) —
// a lower-whipsaw trend filter than close>EMA. Purely additive; the
// default (regime_ma=7) is unchanged.
#define CC1_REGMA_CROSS     8

#define CC1_DEFAULT_ENTRY_N         12.0
#define CC1_DEFAULT_CHAND_ATR        8.0
#define CC1_DEFAULT_ADX_MIN          0.0
#define CC1_DEFAULT_TIME_STOP_BARS   0.0
#define CC1_DEFAULT_REGIME_MA        7.0
#define CC1_DEFAULT_REGIME_GRAIN     1.0
#define CC1_DEFAULT_ENTRY_MODE       1.0
#define CC1_DEFAULT_FAST_N           2.0
#define CC1_DEFAULT_ALPHA            0.82
#define CC1_DEFAULT_EXIT_ALPHA       0.0
#define CC1_DEFAULT_SLOW_N          20.0  // slow self-EMA period (regime_ma=8)

typedef struct
{
  double   chand_atr;
  double   adx_min;
  int      entry_n;
  int      time_stop_bars;
  int      regma_slot;
  int      regime_grain;   // 0 = 1d / 1 = 4h / 2 = 1h
  int      entry_mode;     // 0 = breakout / 1 = immediate
  bool     use_self_ema;   // regime_ma == 7 or 8
  bool     use_cross;      // regime_ma == 8: regma=fast EMA, regx=slow EMA,
                           // regime up iff fast > slow (close ignored)
  double   fast_alpha;     // entry self-EMA smoothing
  double   exit_alpha;     // exit self-EMA smoothing (0 => symmetric)
  double   slow_alpha;     // slow self-EMA smoothing (cross mode)

  // Per-grain cached regime context: close, entry line (regma), exit line
  // (regx). *_have latches on the first bar of that grain.
  double   d1_close, d1_regma, d1_regx;  bool d1_have;
  double   h4_close, h4_regma, h4_regx;  bool h4_have;
  double   h1_close, h1_regma, h1_regx;  bool h1_have;

  // Donchian ring of recent 1h highs (breakout mode; prior window only).
  double   high_ring[CC1_RING_CAP];
  int      ring_head;
  int      ring_count;

  // 1h position state.
  bool     in_position;
  double   peak_high;
  double   entry_close;
  int      bars_in_pos;
} cc1_state_t;

static int
cc1_regma_slot(int regime_ma)
{
  switch(regime_ma)
  {
    case 0:  return(WM_IND_EMA_20);
    case 2:  return(WM_IND_SMA_50);
    case 3:  return(WM_IND_SMA_200);
    case 4:  return(WM_IND_EMA_9);
    case 5:  return(WM_IND_EMA_12);
    case 6:  return(WM_IND_SMA_7);
    case 1:
    default: return(WM_IND_EMA_50);
  }
}

// One EMA step (seed on first sample): ema = alpha*close + (1-alpha)*prev.
static double
cc1_ema_step(double prev, bool seen, double close, double alpha)
{
  if(!seen || isnan(prev))
    return(close);

  return(alpha * close + (1.0 - alpha) * prev);
}

static double
cc1_ring_max(const cc1_state_t *s)
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

static void
cc1_ring_push(cc1_state_t *s, double high)
{
  if(s->entry_n <= 0)
    return;

  s->high_ring[s->ring_head] = high;
  s->ring_head = (s->ring_head + 1) % s->entry_n;

  if(s->ring_count < s->entry_n)
    s->ring_count++;
}

// Advance one grain's cached regime line(s) + close. Self-EMA path: entry
// line = EMA(fast_alpha) of closes; exit line = EMA(exit_alpha) when
// exit_alpha>0 else equals the entry line. Slot path: both = ind[] slot.
static void
cc1_cache_grain(cc1_state_t *s, const wm_candle_full_t *bar,
    double *regma, double *regx, double *close_cell, bool *have)
{
  if(s->use_cross)
  {
    // regma = fast EMA, regx = slow EMA; regime test is fast>slow.
    *regma = cc1_ema_step(*regma, *have, bar->close, s->fast_alpha);
    *regx  = cc1_ema_step(*regx,  *have, bar->close, s->slow_alpha);
  }
  else if(s->use_self_ema)
  {
    double e = cc1_ema_step(*regma, *have, bar->close, s->fast_alpha);

    if(s->exit_alpha > 0.0)
      *regx = cc1_ema_step(*regx, *have, bar->close, s->exit_alpha);
    else
      *regx = e;

    *regma = e;
  }
  else
  {
    *regma = bar->ind[s->regma_slot];
    *regx  = *regma;
  }

  *close_cell = bar->close;
  *have       = true;
}

// ----------------------------------------------------------------------- //
// Param schema                                                            //
// ----------------------------------------------------------------------- //

static const wm_strategy_param_t cc1_params[] = {
  {
    .name        = "entry_n",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CC1_DEFAULT_ENTRY_N,
    .min_int     = 2,
    .max_int     = (int64_t)CC1_RING_CAP,
    .step_dbl    = 6.0,
    .help        = "Breakout lookback in 1h bars (entry_mode=0). Default 12.",
  },
  {
    .name        = "chand_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = CC1_DEFAULT_CHAND_ATR,
    .min_dbl     = 0.0,
    .max_dbl     = 60.0,
    .step_dbl    = 1.0,
    .help        = "Chandelier trailing-stop distance in ATR_14(1h) below"
                   " the highest HIGH since entry. 0 = off. Tight (~2) banks"
                   " legs early for re-compounding. Default 8.",
  },
  {
    .name        = "adx_min",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = CC1_DEFAULT_ADX_MIN,
    .min_dbl     = 0.0,
    .max_dbl     = 40.0,
    .step_dbl    = 2.0,
    .help        = "Minimum 1h ADX_14 to allow an entry (0 = off). Default 0.",
  },
  {
    .name        = "time_stop_bars",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CC1_DEFAULT_TIME_STOP_BARS,
    .min_int     = 0,
    .max_int     = 480,
    .step_dbl    = 12.0,
    .help        = "Cut a trade still below entry after this many 1h bars"
                   " (0 = off). Default 0.",
  },
  {
    .name        = "regime_ma",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CC1_DEFAULT_REGIME_MA,
    .min_int     = 0,
    .max_int     = 8,
    .step_dbl    = 1.0,
    .help        = "Regime line: 0=EMA20 1=EMA50 2=SMA50 3=SMA200 4=EMA9"
                   " 5=EMA12 6=SMA7 7=self-EMA(alpha/fast_n)"
                   " 8=dual self-EMA cross (fast alpha > slow slow_n)."
                   " Default 7.",
  },
  {
    .name        = "regime_grain",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CC1_DEFAULT_REGIME_GRAIN,
    .min_int     = 0,
    .max_int     = 2,
    .step_dbl    = 1.0,
    .help        = "Grain the regime is evaluated on: 0=1d 1=4h 2=1h."
                   " Default 1.",
  },
  {
    .name        = "entry_mode",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CC1_DEFAULT_ENTRY_MODE,
    .min_int     = 0,
    .max_int     = 1,
    .step_dbl    = 1.0,
    .help        = "0 = breakout (new entry_n-bar high), 1 = immediate."
                   " Default 1.",
  },
  {
    .name        = "fast_n",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CC1_DEFAULT_FAST_N,
    .min_int     = 2,
    .max_int     = 200,
    .step_dbl    = 1.0,
    .help        = "Entry self-EMA period (regime_ma=7, used when alpha<=0)."
                   " alpha = 2/(fast_n+1). Default 2.",
  },
  {
    .name        = "alpha",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = CC1_DEFAULT_ALPHA,
    .min_dbl     = 0.0,
    .max_dbl     = 1.0,
    .step_dbl    = 0.02,
    .help        = "Direct entry self-EMA smoothing in (0,1] (regime_ma=7);"
                   " >0 overrides fast_n. Default 0.82.",
  },
  {
    .name        = "exit_alpha",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = CC1_DEFAULT_EXIT_ALPHA,
    .min_dbl     = 0.0,
    .max_dbl     = 1.0,
    .step_dbl    = 0.02,
    .help        = "Separate exit self-EMA smoothing (regime_ma=7); 0 ="
                   " symmetric. < entry alpha = ratchet. Default 0.",
  },
  {
    .name        = "slow_n",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CC1_DEFAULT_SLOW_N,
    .min_int     = 2,
    .max_int     = 400,
    .step_dbl    = 2.0,
    .help        = "Slow self-EMA period for the dual-EMA cross regime"
                   " (regime_ma=8): long while fast(alpha) > slow(slow_n)."
                   " Default 20.",
  },
};

void
wm_strategy_describe(wm_strategy_meta_t *out)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->abi_version = WM_STRATEGY_ABI_VERSION;

  snprintf(out->name, sizeof(out->name), "%s", CC1_NAME);
  snprintf(out->version, sizeof(out->version), "%s", CC1_VERSION);

  out->grains_mask = (uint16_t)((1u << WM_GRAN_1H) |
                                (1u << WM_GRAN_4H) |
                                (1u << WM_GRAN_1D));

  out->min_history[WM_GRAN_1H] = CC1_HIST_1H;
  out->min_history[WM_GRAN_4H] = CC1_HIST_4H;
  out->min_history[WM_GRAN_1D] = CC1_HIST_1D;

  out->params               = cc1_params;
  out->n_params             = (uint32_t)(sizeof(cc1_params)
                                  / sizeof(cc1_params[0]));
  out->wants_trade_callback = false;
}

int
wm_strategy_init(wm_strategy_ctx_t *ctx)
{
  cc1_state_t *s;
  const char  *mid;
  const char  *strat;
  int          regime_ma;
  int          fast_n;
  double       alpha;

  if(ctx == NULL)
    return(-1);

  s = mem_alloc("strategy." CC1_NAME, "state", sizeof(*s));

  memset(s, 0, sizeof(*s));

  mid   = wm_strategy_ctx_market_id(ctx);
  strat = wm_strategy_ctx_strategy_name(ctx);

  s->entry_n = (int)wm_strategy_kv_get_uint(mid, strat, "entry_n",
      (uint64_t)CC1_DEFAULT_ENTRY_N);
  s->chand_atr = wm_strategy_kv_get_dbl(mid, strat, "chand_atr",
      CC1_DEFAULT_CHAND_ATR);
  s->adx_min = wm_strategy_kv_get_dbl(mid, strat, "adx_min",
      CC1_DEFAULT_ADX_MIN);
  s->time_stop_bars = (int)wm_strategy_kv_get_uint(mid, strat,
      "time_stop_bars", (uint64_t)CC1_DEFAULT_TIME_STOP_BARS);
  s->regime_grain = (int)wm_strategy_kv_get_uint(mid, strat, "regime_grain",
      (uint64_t)CC1_DEFAULT_REGIME_GRAIN);
  s->entry_mode = (int)wm_strategy_kv_get_uint(mid, strat, "entry_mode",
      (uint64_t)CC1_DEFAULT_ENTRY_MODE);
  regime_ma = (int)wm_strategy_kv_get_uint(mid, strat, "regime_ma",
      (uint64_t)CC1_DEFAULT_REGIME_MA);
  fast_n = (int)wm_strategy_kv_get_uint(mid, strat, "fast_n",
      (uint64_t)CC1_DEFAULT_FAST_N);
  alpha = wm_strategy_kv_get_dbl(mid, strat, "alpha", CC1_DEFAULT_ALPHA);
  s->exit_alpha = wm_strategy_kv_get_dbl(mid, strat, "exit_alpha",
      CC1_DEFAULT_EXIT_ALPHA);
  {
    int slow_n = (int)wm_strategy_kv_get_uint(mid, strat, "slow_n",
        (uint64_t)CC1_DEFAULT_SLOW_N);

    if(slow_n < 2) slow_n = 2;
    s->slow_alpha = 2.0 / ((double)slow_n + 1.0);
  }

  if(s->entry_n < 1)                  s->entry_n = 1;
  if(s->entry_n > (int)CC1_RING_CAP)  s->entry_n = (int)CC1_RING_CAP;
  if(s->regime_grain < 0)             s->regime_grain = 0;
  if(s->regime_grain > 2)             s->regime_grain = 2;
  if(s->entry_mode < 0)               s->entry_mode = 0;
  if(s->entry_mode > 1)               s->entry_mode = 1;
  if(fast_n < 2)                      fast_n = 2;
  if(alpha < 0.0)                     alpha = 0.0;
  if(alpha > 1.0)                     alpha = 1.0;
  if(s->exit_alpha < 0.0)             s->exit_alpha = 0.0;
  if(s->exit_alpha > 1.0)             s->exit_alpha = 1.0;

  s->use_cross    = (regime_ma == CC1_REGMA_CROSS);
  s->use_self_ema = (regime_ma >= CC1_REGMA_SELF_EMA);
  s->regma_slot   = cc1_regma_slot(s->use_self_ema ? 1 : regime_ma);
  s->fast_alpha   = (alpha > 0.0) ? alpha : 2.0 / ((double)fast_n + 1.0);

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, CC1_LOG_CTX,
      "init: %s -> %s entry_n=%d chand_atr=%.2f adx_min=%.1f"
      " time_stop_bars=%d regime_ma=%d self_ema=%d alpha=%.4f exit_alpha=%.4f"
      " regime_grain=%d entry_mode=%d",
      strat, mid, s->entry_n, s->chand_atr, s->adx_min,
      s->time_stop_bars, regime_ma, (int)s->use_self_ema, s->fast_alpha,
      s->exit_alpha, s->regime_grain, s->entry_mode);

  return(0);
}

void
wm_strategy_finalize(wm_strategy_ctx_t *ctx)
{
  cc1_state_t *s;

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
  cc1_state_t          *s;
  wm_strategy_signal_t  sig;
  double                adx;
  double                atr;
  double                chan_hi;
  bool                  entry_up;
  bool                  exit_up;
  bool                  fire = false;

  (void)mkt;

  if(bar == NULL || ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s == NULL)
    return;

  if(grain == WM_GRAN_1D)
  {
    cc1_cache_grain(s, bar, &s->d1_regma, &s->d1_regx, &s->d1_close,
        &s->d1_have);
    return;
  }

  if(grain == WM_GRAN_4H)
  {
    cc1_cache_grain(s, bar, &s->h4_regma, &s->h4_regx, &s->h4_close,
        &s->h4_have);
    return;
  }

  if(grain != WM_GRAN_1H)
    return;

  // Advance the 1h regime line (used only when regime_grain == 2).
  cc1_cache_grain(s, bar, &s->h1_regma, &s->h1_regx, &s->h1_close,
      &s->h1_have);

  // ---- 1h decision grain ----
  adx     = bar->ind[WM_IND_ADX_14];
  atr     = bar->ind[WM_IND_ATR_14];
  chan_hi = cc1_ring_max(s);

  {
    // Pick the configured grain's cached cells, then derive up/down. In
    // cross mode (regma=fast, regx=slow) regime up iff fast>slow, same for
    // entry+exit. Otherwise close>entry-line (entry) / close>exit-line.
    double  rc, rma, rx;
    bool    have;

    if(s->regime_grain == 2)
    { rc = s->h1_close; rma = s->h1_regma; rx = s->h1_regx; have = s->h1_have; }
    else if(s->regime_grain == 0)
    { rc = s->d1_close; rma = s->d1_regma; rx = s->d1_regx; have = s->d1_have; }
    else
    { rc = s->h4_close; rma = s->h4_regma; rx = s->h4_regx; have = s->h4_have; }

    if(s->use_cross)
    {
      bool up  = have && !isnan(rma) && !isnan(rx) && rma > rx;

      entry_up = up;
      exit_up  = up;
    }
    else
    {
      entry_up = have && !isnan(rma) && rc > rma;
      exit_up  = have && !isnan(rx)  && rc > rx;
    }
  }

  memset(&sig, 0, sizeof(sig));
  sig.ts_ms = bar->ts_close_ms;

  if(!s->in_position)
  {
    bool adx_ok  = (s->adx_min <= 0.0) ||
                   (!isnan(adx) && adx >= s->adx_min);
    bool trigger = (s->entry_mode == 1)
                     ? true
                     : (!isnan(chan_hi) && bar->close > chan_hi);

    if(entry_up && adx_ok && trigger)
    {
      sig.score      = 1.0;
      sig.confidence = 0.6;
      snprintf(sig.reason, sizeof(sig.reason),
          "%s g%d", s->entry_mode == 1 ? "imm" : "brk", s->regime_grain);

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

    s->bars_in_pos++;

    if(bar->high > s->peak_high)
      s->peak_high = bar->high;

    if(have_atr && s->chand_atr > 0.0)
      chand_lv = s->peak_high - s->chand_atr * atr;

    hit_time  = (s->time_stop_bars > 0 &&
                 s->bars_in_pos >= s->time_stop_bars &&
                 bar->close < s->entry_close);
    hit_chand = (chand_lv > 0.0 && bar->close <= chand_lv);

    if(hit_chand || hit_time || !exit_up)
    {
      const char *why = !exit_up  ? "regime" :
                        hit_chand ? "chand"  : "time";

      sig.score      = -1.0;
      sig.confidence = 0.5;
      snprintf(sig.reason, sizeof(sig.reason), "exit %s", why);

      s->in_position = false;
      s->peak_high   = 0.0;
      fire           = true;
    }
  }

  cc1_ring_push(s, bar->high);

  if(fire)
    wm_strategy_emit_signal(ctx, &sig);
}

// ----------------------------------------------------------------------- //
// Plugin lifecycle                                                        //
// ----------------------------------------------------------------------- //

static bool
cc1_plugin_init(void)
{
  clam(CLAM_INFO, CC1_LOG_CTX, "%s v%s loaded", CC1_NAME, CC1_VERSION);
  return(false);
}

static void
cc1_plugin_deinit(void)
{
  clam(CLAM_INFO, CC1_LOG_CTX, "%s deinit", CC1_NAME);
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "strategy_" CC1_NAME,
  .version         = CC1_VERSION,
  .type            = PLUGIN_STRATEGY,
  .kind            = CC1_NAME,
  .provides        = { { .name = "strategy_" CC1_NAME } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_whenmoon" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = cc1_plugin_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = cc1_plugin_deinit,
  .ext             = NULL,
};
