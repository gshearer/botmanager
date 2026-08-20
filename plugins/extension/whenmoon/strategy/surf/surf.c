// botmanager — MIT
// surf — regime-gated momentum-impulse swing harvester (1h decision grain).
//
// The existing winners cluster around two shapes: ride ONE leg with a wide
// structural exit (cp1 Donchian, cp2 pullback-then-wide-trail) or flip a
// BINARY regime switch and hold the whole leg (cc1/cc2). surf occupies the
// gap between them — it stays long-biased only inside a higher-grain trend
// (the "stay out of the bear" filter every winner shares), but inside that
// trend it BUYS each fresh 1h momentum impulse and BANKS it when the impulse
// fades, harvesting several round-trips per leg instead of one. That faster
// turnover is the point: attached live in a healthy market it takes a trade
// within hours-to-a-day and keeps cycling, so paper trading shows action
// rather than a single multi-week hold.
//
//   regime (4h / 1d) : longs allowed only while the close on the regime
//                      grain is above its regime MA (regime_ma: EMA_20 /
//                      EMA_50 / SMA_50 / SMA_200). This is the macro tide;
//                      below it surf is flat. It is also the hard exit
//                      backstop — a regime flip closes any open long.
//   entry (1h)       : a momentum impulse turning UP, gated by the regime.
//                      entry_mode selects which impulse:
//                        0 = MACD histogram crosses up through 0 (the MACD
//                            line crosses its signal — a fresh up-impulse),
//                        1 = close crosses up through EMA_20 (price reclaims
//                            the fast trend after a pullback),
//                        2 = RSI_14 crosses up through mom_thresh (momentum
//                            resuming). A CROSS (not a level) so we enter at
//                            the turn, not mid-extension.
//   exit (1h)        : bank or ride, per exit_mode —
//                        0 = chandelier (close <= peak_high - chand_atr *
//                            ATR_14) — ride the leg, give back chand_atr ATRs,
//                        1 = momentum fade (the entry impulse goes negative:
//                            hist<0 / close<EMA_20 / RSI<mom_thresh) — bank
//                            each impulse, the most active mode,
//                        2 = either, whichever fires first.
//                      The regime flip is an unconditional backstop in all
//                      three modes.
//
// entry_mode / exit_mode are single ordinals that bundle behaviour (cp2's
// htf_mode trick): one knob trades frequency against selectivity, which is
// far less overfit-prone than a pile of independent toggles. The whole
// param surface is six knobs, all orthogonal.
//
// ── VALIDATED CONFIG (these are the compile-time defaults) ───────────────
//      regime_grain=1 (4h)   regime_ma=0 (EMA_20)   entry_mode=1 (EMA-reclaim)
//      exit_mode=0 (chandelier ride)   chand_atr=6   (mom_thresh=50, unused
//      at entry_mode 1). A long-bias 4h-EMA_20 regime, enter when the 1h
//      close reclaims its EMA_20, ride each leg on a 6-ATR chandelier and
//      exit on the regime flip. Net-positive on ALL FOUR corpora and through
//      the full validation triad (full-sample / OOS-tail-30 / walk-forward
//      365:120:120 — see the SCOREBOARD row):
//        BTC  pf 7.39  +36,110% net  67% win  2.2% maxDD   WF pf 7.39 (32 win)
//        ETH  pf 10.4  +136,170% net 68% win  3.2% maxDD   WF pf 10.4 (28 win)
//        SOL  pf 6.67  +4,896% net   67% win  4.4% maxDD   WF pf 6.54 (13 win)
//        XRP  pf 5.30  +267% net     58% win  5.0% maxDD   WF pf 5.85 (6 win)
//      OOS-tail-30 realized is positive on all four. The robustness is broad,
//      not a knife-edge: of a 72-config structural sweep (every entry_mode x
//      exit_mode x regime_grain x regime_ma) only 2 configs were net-negative,
//      both the over-traded extreme (slow SMA_200 regime + fade exit).
//
// ── THE LEVERS THE SWEEPS FOUND ──────────────────────────────────────────
//   exit_mode is the action/edge dial. exit_mode=0 (chandelier RIDE) wins on
//   every quality metric — ride the leg, let the regime flip end it — at
//   ~0.11-0.26 round-trips/day/market (≈1 trade every 4-9 days; the first
//   trade still lands within a day or two of attach when the regime is up).
//   exit_mode=2 (fade OR chandelier) is the high-ACTION variant: it banks
//   each 1h impulse for ~0.3-0.86 round-trips/day (3-4x the turnover, ~1/day
//   on the busy markets), still net-positive on all four and OOS+ on three —
//   marginally negative OOS on XRP, the one caveat. Reach for exit_mode=2
//   when you want to watch it work; the default rides for the cleaner edge.
//   regime_ma=0 (the FAST 4h EMA_20) beats the slower MAs by re-enabling
//   longs earlier in each recovery; regime_ma=1 (EMA_50) is the close runner-
//   up (still pf ~5-6), so the regime speed sits on a ridge, not a spike.
//   entry_mode=1 (EMA-reclaim) edges entry_mode=0 (MACD) and 2 (RSI) — but
//   per cp2's finding, losers carry little entry signature, so the exit
//   structure + regime gate carry the edge and the entry choice is secondary.
//
// ── HONEST CAVEAT ────────────────────────────────────────────────────────
//   The eye-popping ride returns (ETH +136,170%) are partly a backtest
//   artifact: fills land at bar close with only 5bps+5bps friction and no
//   market impact, equity is sampled PER FILL (so a held leg's intra-trade
//   give-back never registers — which is why the ride's maxDD reads a
//   flattering ~2-5%), and BTC/ETH genuinely ran ~1000x over the corpus, so a
//   long-biased rider compounds hard. surf is far tamer than the cc1/cc2
//   regime-flippers (pf ~7 vs their ~1e12 totals) BECAUSE it does not flip a
//   fast binary regime thousands of times; treat the percentages as relative
//   robustness evidence, not live-tradeable expectancy. The more-honest read
//   is pf, win-rate and the all-positive walk-forward — all strong.
//
// LOOKAHEAD SAFETY. The backtest fires on_bar in a merged chronological walk
// across grains; on a shared timestamp the FINER grain fires first (1h
// before 4h before 1d). surf caches the regime grain's close + regime MA as
// that grain's bar closes and reads the cache only on a later 1h decision
// bar, so it never sees a higher-grain bar that has not genuinely closed.
// The 1h decision reads only this bar's own ind[] slots (each is the value
// at this bar's close — lookahead-free in live and backtest) and a strategy-
// owned peak_high tracked forward since entry. No reach into mkt->grain_arr[]
// (which in backtest holds the whole — future — date range). Identical in
// live and backtest.

#include "whenmoon_strategy.h"

#include "alloc.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define SURF_NAME      "surf"
#define SURF_VERSION   "0.1"
#define SURF_LOG_CTX   "strategy.surf"

// Per-grain live warm-up history (bars). Backtest snapshots carry the full
// date range regardless; this only matters at live cold start. 1h wants
// MACD(26+9) + RSI/EMA warm; the regime grains want up to SMA_200.
#define SURF_HIST_1H   210u
#define SURF_HIST_4H   256u
#define SURF_HIST_1D   256u

// Defaults — mirrored in the param schema below. These ARE the validated
// full-corpus winner (header "VALIDATED CONFIG"); a fresh daemon
// reproduces it from a bare `backtest run <wm> surf`. (A long-running
// daemon keeps the KV values registered at FIRST load — see "KV staleness"
// in ../AGENTS.md — so reproduce on a stale daemon with the explicit
// name=val args from the SCOREBOARD row, or `set kv --delete` each key
// back to the declaration.)
#define SURF_DEFAULT_REGIME_GRAIN   1.0   // 1 = 4h regime (fast tide)
#define SURF_DEFAULT_REGIME_MA      0.0   // 0 = EMA_20 on the 4h regime grain
#define SURF_DEFAULT_ENTRY_MODE     1.0   // 1 = close reclaims the 1h EMA_20
#define SURF_DEFAULT_EXIT_MODE      0.0   // 0 = chandelier ride (+regime exit)
#define SURF_DEFAULT_MOM_THRESH    50.0   // RSI up-cross level (entry_mode 2)
#define SURF_DEFAULT_CHAND_ATR      6.0   // chandelier give-back in ATR_14(1h)

typedef struct
{
  // Resolved params (snapshot at init; reload to pick up KV changes).
  int      regime_grain;   // 0 = 1d regime, 1 = 4h regime
  int      regma_slot;     // resolved WM_IND_* for the regime MA
  int      entry_mode;     // 0 MACD-hist / 1 EMA_20-reclaim / 2 RSI-cross
  int      exit_mode;      // 0 chandelier / 1 fade / 2 both
  double   mom_thresh;     // RSI up-cross level (entry_mode 2)
  double   chand_atr;      // chandelier = peak_high - chand_atr*ATR_14 (0=off)

  // Cached regime-grain context. *_have latches once that grain produces a
  // bar; values are the readings at that grain's latest close (NaN-guarded
  // on read). Both grains are cached unconditionally (cheap) so flipping
  // regime_grain needs no rewire.
  double   d1_close, d1_regma;   bool d1_have;   // 1d regime candidate
  double   h4_close, h4_regma;   bool h4_have;   // 4h regime candidate

  // 1h decision-grain cross-detection state. Updated every 1h bar
  // regardless of position so cross detection stays continuous.
  double   prev_hist;            bool have_prev_hist;   // MACD_HIST (mode 0)
  bool     prev_above_ema;       bool have_prev_above;   // close vs EMA_20 (1)
  double   prev_rsi;             bool have_prev_rsi;     // RSI_14 (mode 2)

  // 1h position state.
  bool     in_position;
  double   peak_high;      // highest HIGH since entry (chandelier anchor)
} surf_state_t;

// Map the regime_ma ordinal onto a moving-average indicator slot. Same slot
// id on either regime grain (1d or 4h) — the value differs because it is
// computed on that grain's bars. A faster MA re-enables longs sooner in a
// recovery; a slower one stays out longer.
static int
surf_regma_slot(int regime_ma)
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

// Macro regime up? Reads the cached close/MA of the selected regime grain.
// Fails closed if that grain has not produced a bar yet or its MA is NaN —
// surf does not trade on unknown context.
static bool
surf_regime_up(const surf_state_t *s)
{
  if(s->regime_grain == 0)
    return(s->d1_have && !isnan(s->d1_regma) && s->d1_close > s->d1_regma);

  return(s->h4_have && !isnan(s->h4_regma) && s->h4_close > s->h4_regma);
}

// Cache the regime grain's close + MA as its bar closes (lookahead-free: a
// later 1h bar only ever reads a regime bar that closed in its past).
static void
surf_cache_context(surf_state_t *s, wm_gran_t grain,
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
      s->h4_close = bar->close;
      s->h4_regma = bar->ind[s->regma_slot];
      s->h4_have  = true;
      break;

    default:
      break;
  }
}

// The entry impulse turning UP on this 1h bar (a cross, against the cached
// prior reading). Returns false until the prior reading exists.
static bool
surf_entry_cross_up(const surf_state_t *s, const wm_candle_full_t *bar)
{
  switch(s->entry_mode)
  {
    case 1:
    {
      float ema20 = bar->ind[WM_IND_EMA_20];

      if(isnan(ema20) || !s->have_prev_above)
        return(false);

      return(!s->prev_above_ema && bar->close > (double)ema20);
    }

    case 2:
    {
      float rsi = bar->ind[WM_IND_RSI_14];

      if(isnan(rsi) || !s->have_prev_rsi)
        return(false);

      return(s->prev_rsi <= s->mom_thresh && (double)rsi > s->mom_thresh);
    }

    case 0:
    default:
    {
      float hist = bar->ind[WM_IND_MACD_HIST];

      if(isnan(hist) || !s->have_prev_hist)
        return(false);

      return(s->prev_hist <= 0.0 && (double)hist > 0.0);
    }
  }
}

// The entry impulse has faded (gone negative) on this 1h bar — a level test,
// not a cross, so a gap-down through the line still banks the trade. Mirrors
// the entry_mode dimension. NaN reads fail closed (no fade).
static bool
surf_momentum_faded(const surf_state_t *s, const wm_candle_full_t *bar)
{
  switch(s->entry_mode)
  {
    case 1:
    {
      float ema20 = bar->ind[WM_IND_EMA_20];

      return(!isnan(ema20) && bar->close < (double)ema20);
    }

    case 2:
    {
      float rsi = bar->ind[WM_IND_RSI_14];

      return(!isnan(rsi) && (double)rsi < s->mom_thresh);
    }

    case 0:
    default:
    {
      float hist = bar->ind[WM_IND_MACD_HIST];

      return(!isnan(hist) && (double)hist < 0.0);
    }
  }
}

// Refresh the 1h cross-detection state at the end of every 1h bar.
static void
surf_track_prev(surf_state_t *s, const wm_candle_full_t *bar)
{
  float hist  = bar->ind[WM_IND_MACD_HIST];
  float ema20 = bar->ind[WM_IND_EMA_20];
  float rsi   = bar->ind[WM_IND_RSI_14];

  if(!isnan(hist))
  {
    s->prev_hist      = (double)hist;
    s->have_prev_hist = true;
  }

  if(!isnan(ema20))
  {
    s->prev_above_ema  = bar->close > (double)ema20;
    s->have_prev_above = true;
  }

  if(!isnan(rsi))
  {
    s->prev_rsi      = (double)rsi;
    s->have_prev_rsi = true;
  }
}

// ----------------------------------------------------------------------- //
// Required exports                                                        //
// ----------------------------------------------------------------------- //

static const wm_strategy_param_t surf_params[] = {
  {
    .name        = "regime_grain",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)SURF_DEFAULT_REGIME_GRAIN,
    .min_int     = 0,
    .max_int     = 1,
    .step_dbl    = 1.0,
    .help        = "Grain the long-bias regime runs on: 0=1d (slow tide),"
                   " 1=4h (faster — re-enables longs earlier in a recovery)."
                   " Default 1.",
  },
  {
    .name        = "regime_ma",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)SURF_DEFAULT_REGIME_MA,
    .min_int     = 0,
    .max_int     = 3,
    .step_dbl    = 1.0,
    .help        = "Regime MA on the regime grain (close must be above it for"
                   " longs; below it forces exit): 0=EMA_20, 1=EMA_50,"
                   " 2=SMA_50, 3=SMA_200. Faster=more time long. Default 0.",
  },
  {
    .name        = "entry_mode",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)SURF_DEFAULT_ENTRY_MODE,
    .min_int     = 0,
    .max_int     = 2,
    .step_dbl    = 1.0,
    .help        = "Which 1h momentum impulse arms a long (a cross UP):"
                   " 0=MACD histogram through 0, 1=close through EMA_20,"
                   " 2=RSI_14 through mom_thresh. Default 1.",
  },
  {
    .name        = "exit_mode",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)SURF_DEFAULT_EXIT_MODE,
    .min_int     = 0,
    .max_int     = 2,
    .step_dbl    = 1.0,
    .help        = "How a long closes (the regime flip is always a backstop):"
                   " 0=chandelier only (ride the leg — best edge), 1=momentum"
                   " fade only (bank each impulse), 2=either first (most"
                   " active — ~3-4x turnover). Default 0.",
  },
  {
    .name        = "mom_thresh",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = SURF_DEFAULT_MOM_THRESH,
    .min_dbl     = 40.0,
    .max_dbl     = 65.0,
    .step_dbl    = 2.0,
    .help        = "RSI_14 level the 1h close must cross UP through to arm a"
                   " long (entry_mode=2); also the fade-exit level. Default"
                   " 50.",
  },
  {
    .name        = "chand_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = SURF_DEFAULT_CHAND_ATR,
    .min_dbl     = 1.5,
    .max_dbl     = 50.0,
    .step_dbl    = 0.5,
    .help        = "Chandelier trailing-stop distance in ATR_14(1h) below the"
                   " highest HIGH since entry (exit_mode 0/2). Wide (~40)"
                   " effectively disables it (regime-flip becomes the sole"
                   " exit); the pf plateau starts near 6. Default 6.",
  },
};

void
wm_strategy_describe(wm_strategy_meta_t *out)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->abi_version = WM_STRATEGY_ABI_VERSION;

  snprintf(out->name, sizeof(out->name), "%s", SURF_NAME);
  snprintf(out->version, sizeof(out->version), "%s", SURF_VERSION);

  // 1h decides/emits; 4h + 1d feed cached regime context only.
  out->grains_mask = (uint16_t)((1u << WM_GRAN_1H) |
                                (1u << WM_GRAN_4H) |
                                (1u << WM_GRAN_1D));

  out->min_history[WM_GRAN_1H] = SURF_HIST_1H;
  out->min_history[WM_GRAN_4H] = SURF_HIST_4H;
  out->min_history[WM_GRAN_1D] = SURF_HIST_1D;

  out->params               = surf_params;
  out->n_params             = (uint32_t)(sizeof(surf_params)
                                  / sizeof(surf_params[0]));
  out->wants_trade_callback = false;
}

int
wm_strategy_init(wm_strategy_ctx_t *ctx)
{
  surf_state_t *s;
  const char   *mid;
  const char   *strat;
  int           regime_ma;

  if(ctx == NULL)
    return(-1);

  s = mem_alloc("strategy." SURF_NAME, "state", sizeof(*s));

  memset(s, 0, sizeof(*s));

  mid   = wm_strategy_ctx_market_id(ctx);
  strat = wm_strategy_ctx_strategy_name(ctx);

  s->regime_grain = (int)wm_strategy_kv_get_uint(mid, strat, "regime_grain",
      (uint64_t)SURF_DEFAULT_REGIME_GRAIN);
  regime_ma = (int)wm_strategy_kv_get_uint(mid, strat, "regime_ma",
      (uint64_t)SURF_DEFAULT_REGIME_MA);
  s->entry_mode = (int)wm_strategy_kv_get_uint(mid, strat, "entry_mode",
      (uint64_t)SURF_DEFAULT_ENTRY_MODE);
  s->exit_mode = (int)wm_strategy_kv_get_uint(mid, strat, "exit_mode",
      (uint64_t)SURF_DEFAULT_EXIT_MODE);
  s->mom_thresh = wm_strategy_kv_get_dbl(mid, strat, "mom_thresh",
      SURF_DEFAULT_MOM_THRESH);
  s->chand_atr = wm_strategy_kv_get_dbl(mid, strat, "chand_atr",
      SURF_DEFAULT_CHAND_ATR);

  if(s->regime_grain < 0) s->regime_grain = 0;
  if(s->regime_grain > 1) s->regime_grain = 1;
  if(s->entry_mode   < 0) s->entry_mode   = 0;
  if(s->entry_mode   > 2) s->entry_mode   = 2;
  if(s->exit_mode    < 0) s->exit_mode    = 0;
  if(s->exit_mode    > 2) s->exit_mode    = 2;

  s->regma_slot = surf_regma_slot(regime_ma);

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, SURF_LOG_CTX,
      "init: %s -> %s regime_grain=%d regma_slot=%d entry_mode=%d"
      " exit_mode=%d mom_thresh=%.1f chand_atr=%.2f (1h)",
      strat, mid, s->regime_grain, s->regma_slot, s->entry_mode,
      s->exit_mode, s->mom_thresh, s->chand_atr);

  return(0);
}

void
wm_strategy_finalize(wm_strategy_ctx_t *ctx)
{
  surf_state_t *s;

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
  surf_state_t         *s;
  wm_strategy_signal_t  sig;
  float                 atr;
  bool                  regime_up;
  bool                  fire = false;

  (void)mkt;   // surf reads only ind[] slots + its own cached context.

  if(bar == NULL || ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s == NULL)
    return;

  // Higher grains only refresh cached regime context.
  if(grain != WM_GRAN_1H)
  {
    surf_cache_context(s, grain, bar);
    return;
  }

  // ---- 1h decision grain ----
  atr       = bar->ind[WM_IND_ATR_14];
  regime_up = surf_regime_up(s);

  memset(&sig, 0, sizeof(sig));
  sig.ts_ms = bar->ts_close_ms;

  if(!s->in_position)
  {
    // Entry: regime up, flat, and the chosen momentum impulse crosses up.
    if(regime_up && surf_entry_cross_up(s, bar))
    {
      sig.score      = 1.0;
      sig.confidence = 0.6;
      snprintf(sig.reason, sizeof(sig.reason),
          "in e%d g%d", s->entry_mode, s->regime_grain);

      s->in_position = true;
      s->peak_high   = bar->high;
      fire           = true;
    }
  }

  else
  {
    bool   have_atr  = !isnan(atr) && atr > 0.0f;
    double chand_lv  = -1.0;
    bool   want_fade = (s->exit_mode == 1 || s->exit_mode == 2);
    bool   want_chand = (s->exit_mode == 0 || s->exit_mode == 2);
    bool   hit_fade;
    bool   hit_chand;

    if(bar->high > s->peak_high)
      s->peak_high = bar->high;

    if(want_chand && have_atr && s->chand_atr > 0.0)
      chand_lv = s->peak_high - s->chand_atr * (double)atr;

    hit_fade  = (want_fade && surf_momentum_faded(s, bar));
    hit_chand = (chand_lv > 0.0 && bar->close <= chand_lv);

    // The regime flip is an unconditional backstop in every exit mode.
    if(hit_fade || hit_chand || !regime_up)
    {
      const char *why = !regime_up ? "regime" :
                        hit_fade   ? "fade"   : "chand";

      sig.score      = -1.0;
      sig.confidence = 0.5;
      snprintf(sig.reason, sizeof(sig.reason), "exit %s", why);

      s->in_position = false;
      s->peak_high   = 0.0;
      fire           = true;
    }
  }

  // Refresh cross-detection state AFTER the decision so this bar's reading
  // becomes "prior" for the next bar's cross test (no self-reference).
  surf_track_prev(s, bar);

  if(fire)
  {
    wm_strategy_emit_signal(ctx, &sig);

    clam(CLAM_INFO, SURF_LOG_CTX,
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
surf_plugin_init(void)
{
  clam(CLAM_INFO, SURF_LOG_CTX, "%s v%s loaded", SURF_NAME, SURF_VERSION);
  return(false);
}

static void
surf_plugin_deinit(void)
{
  wm_strategy_detach_self(SURF_NAME);

  clam(CLAM_INFO, SURF_LOG_CTX, "%s deinit", SURF_NAME);
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "strategy_" SURF_NAME,
  .version         = SURF_VERSION,
  .type            = PLUGIN_STRATEGY,
  .kind            = SURF_NAME,
  .provides        = { { .name = "strategy_" SURF_NAME } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_whenmoon" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = surf_plugin_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = surf_plugin_deinit,
  .ext             = NULL,
};
