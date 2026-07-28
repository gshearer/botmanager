// botmanager — MIT
// fathom — regime-adaptive dislocation/reversion harvester (1h decision grain).
//
// THE GAP THIS EXPLOITS. Every strategy in the sidequest1 field —
// cc1/cc2, cp1/cp2, surf, mako, riptide, squall, juggernaut, keel,
// capstan — treats the higher-grain trend line as PERMISSION: long
// above it, flat or absent below it. mako and riptide buy dips, but
// only "inside a confirmed up regime". Not one of them has ever opened
// a position while price sat below its own trend mean. That is where
// the largest short-horizon reversion premia live, and it is the one
// part of the tape nobody in this shop has traded.
//
// THE PREMIUM. When price is pulled a measured distance below its own
// short mean, part of that displacement is mechanical — liquidation
// cascades, stop runs, forced redemptions — not information. Whoever
// absorbs that flow carries inventory risk at the moment of maximum
// uncertainty, and is paid for it in the drift over the following
// hours. It is a risk premium, not an arbitrage, so it persists as
// long as leverage does. Priced in ATR units it self-scales: in a
// crash the displacement AND the bounce both expand.
//
// REGIME-ADAPTIVE, NOT REGIME-GATED. The premium does not switch off
// across regimes — its geometry changes. So fathom runs one mechanism
// in three parameterizations chosen by a daily z-score, and trades in
// all three:
//
//   BULL  (z >= +0.5)     : FAR target, LONG hold — trend-riding
//                            expressed as a dip entry.
//   CHOP  (|z| <  0.5)    : MEDIUM target, MEDIUM hold — the
//                            oscillation itself is the product.
//   CRASH (z <= -0.5)     : NEAR target, SHORT hold, plus a
//                            stabilization confirm (buy the first
//                            up-bar, never the knife mid-plunge).
//                            Defensive means small and fast, NOT
//                            hiding — the monthly activity gate never
//                            sleeps.
//
// Entry DEPTH is uniform across modes (v0.2 — see RETIRED below); only
// the exit geometry adapts.
//
//   regime : z = (close_1d - MA_1d) / ATR_14(1d), MA per `regime_ma`;
//            |z| >= 0.5 splits BULL / CRASH from CHOP. Vol-normalized.
//            Fails closed to CHOP while the daily context is unwarm.
//   entry  : flat AND close <= SMA_20(1h) - dip_atr * ATR_14(1h)
//            AND MOM_10(1h) <= -impulse_atr * ATR_14(1h)
//            (+ close > open when the mode is CRASH).
//
//   exits  : priority target > stop > time, all anchored at entry and
//            frozen there with the mode:
//              close >= entry + tgt[mode]*ATR(entry)   -> "tgt"
//              close <= entry - stop_atr*ATR(entry)    -> "stop" (0=off)
//              hold[mode] 1h bars elapsed              -> "time"
//            There is NO regime-flip exit, deliberately: round 1's
//            post-mortem showed trend-line churn, not target failure,
//            was the bear-year bleed. For mean reversion the TIME stop
//            is the correct stop — it exits when the thesis has not
//            worked instead of crystallizing at the worst price a hard
//            stop always picks.
//
// v0.2 — FORCED SELLING IS FAST. Depth alone cannot tell a liquidation
// cascade from a slow informed downtrend: a quiet two-day grind to
// -1 ATR trips the same wire as a two-hour plunge to -1 ATR, and only
// the second is the flow this premium is paid for. Depth identifies
// displacement; depth-per-unit-time identifies DISLOCATION. So the
// entry also demands velocity — MOM_10 (the 10-bar close-to-close
// change, an existing indicator slot, so no new state and no new
// lookahead surface) must be at least impulse_atr ATRs negative.
// impulse_atr = 0 disables the test and reproduces v0.1 exactly.
//
// RETIRED IN v0.2: `dip_scale` (regime spread on entry depth). Its
// swept optimum was its own identity value 1.0, with BOTH directions
// strictly worse (median pf 1.087 at 1.0 vs 1.016-1.059 elsewhere,
// 672-config grid) — depth adaptation is refuted, so the knob is not
// carried as dead weight; the entry depth is now uniform across modes.
//
// Mode geometry from two bases and one spread multiplier:
//   tgt [BULL]=target_atr*target_scale   hold[BULL]=max_hold*hold_scale
//   tgt [CHOP]=target_atr                hold[CHOP]=max_hold
//   tgt [CRASH]=target_atr/target_scale  hold[CRASH]=max_hold/hold_scale
//
// v0.3 DECOUPLES hold from target. v0.2 tied them ("the time a reversion
// needs is proportional to the distance waited for"), and that coupling
// is what broke the activity gate: the FAR TARGET is what earns in bull
// mode, the LONG HOLD is what starves cadence in quiet grind-up years
// (2016/2017/2020/2023 fell to 76-116 round trips). They are separable.
// RETIRED in v0.3: `regime_z`, measured inert (median pf 1.049-1.054
// flat across 0.5..1.5 over 672 configs); fixed at 0.5.
//
// BOTH SPREAD DIRECTIONS ARE REACHABLE. target_scale spans
// [0.33 .. 6.0], so a value BELOW 1 inverts the adaptation (crash gets
// the farther target and longer hold, bull the reverse). Which way each
// regime should lean is an empirical question the sweep answers, not a
// prior baked into the instrument.
//
// THE NULL HYPOTHESIS IS INSIDE THE INSTRUMENT. target_scale = 1.0 with
// hold_scale = 1.0 collapses all three modes to identical parameters — a plain,
// unadapted dislocation reverter. That degenerate config sits in the
// INTERIOR of the sweep range and is the control for the
// regime-adaptive claim; if the ridge sits there, the thesis is
// refuted by its own sweep.
//
// LOOKAHEAD SAFETY. The backtest fires on_bar in a merged chronological
// walk; on a shared timestamp the finer grain fires first (1h before
// 1d). fathom caches the daily close / MA / ATR as that 1d bar closes
// and reads the cache only on later 1h decision bars; the 1h decision
// reads this bar's own ind[] slots plus anchors frozen at entry. No
// reach into mkt->grain_arr[] (in backtest that ring holds the whole —
// future — date range). Identical in live and backtest.

#include "whenmoon_strategy.h"

#include "alloc.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define FATHOM_NAME      "fathom"
#define FATHOM_VERSION   "0.3"
#define FATHOM_LOG_CTX   "strategy.fathom"

// Per-grain live warm-up history (bars). Backtest snapshots carry the
// full range regardless; this matters at live cold start. 1h wants
// SMA_20 + ATR_14 warm with margin; 1d must cover SMA_200 (regime_ma=3).
#define FATHOM_HIST_1H   240u
#define FATHOM_HIST_1D   256u

// Defaults — mirrored in the param schema below.
#define FATHOM_DEFAULT_REGIME_MA     1u    // 1 = EMA_50 on the daily grain
#define FATHOM_DEFAULT_HOLD_SCALE    1.0   // hold spread, independent of target
#define FATHOM_DEFAULT_DIP_ATR       1.5   // CHOP displacement
#define FATHOM_DEFAULT_IMPULSE_ATR   0.0   // velocity gate off by default
#define FATHOM_DEFAULT_TARGET_ATR    1.0   // CHOP reversion target
#define FATHOM_DEFAULT_TARGET_SCALE  1.5   // regime spread on target
#define FATHOM_DEFAULT_MAX_HOLD      24u   // CHOP time stop, 1h bars
#define FATHOM_DEFAULT_STOP_ATR      0.0   // disaster stop off by default

// Regime threshold in daily ATRs. RETIRED as a param in v0.3 — measured
// inert (median pf 1.049-1.054 flat across 0.5..1.5 over 672 configs).
#define FATHOM_REGIME_Z  0.5

// Guard rails on the derived hold so a hand-edited KV cannot produce a
// zero-length or unbounded hold.
#define FATHOM_HOLD_MIN  1u
#define FATHOM_HOLD_MAX  8760u

typedef enum
{
  FATHOM_BULL = 0,
  FATHOM_CHOP,
  FATHOM_CRASH
} fathom_regime_t;

typedef struct
{
  // Resolved params (snapshot at init; reload to pick up KV changes).
  int      regime_slot;    // resolved WM_IND_* for the daily regime MA
  double   hold_scale;     // regime spread on hold (1.0 = no adaptation)
  double   dip_atr;        // CHOP entry displacement, 1h ATR multiples
  double   impulse_atr;    // required 10-bar fall, ATR multiples (0 = off)
  double   target_atr;     // CHOP target, entry-ATR multiples
  double   target_scale;   // regime spread on target (1.0 = no adaptation)
  uint32_t max_hold;       // CHOP time stop, 1h bars
  double   stop_atr;       // disaster stop, entry-ATR multiples (0 = off)

  // Cached daily regime context (cache-on-close). Latches once a 1d bar
  // closes; values are that bar's close, regime MA and ATR.
  double   d1_close;
  double   d1_ma;
  double   d1_atr;
  bool     d1_have;

  // 1h position state. Everything is frozen at entry — including the
  // mode — so the trade's geometry never drifts mid-hold.
  bool            in_position;
  fathom_regime_t entry_regime;
  double          entry_price;
  double          entry_atr;
  double          tgt_lv;
  double          stop_lv;
  uint32_t        hold_left;
} fathom_state_t;

// Map the regime_ma ordinal onto a daily indicator slot.
static int
fathom_regime_slot(uint32_t regime_ma)
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

static const char *
fathom_regime_name(fathom_regime_t r)
{
  switch(r)
  {
    case FATHOM_BULL:  return("bull");
    case FATHOM_CRASH: return("crash");
    case FATHOM_CHOP:
    default:           return("chop");
  }
}

// Classify from the cached daily context. Fails closed to CHOP — the
// middle mode — whenever the context is unwarm or non-finite, so a
// cold start never mistakes missing data for an extreme.
static fathom_regime_t
fathom_regime(const fathom_state_t *s)
{
  double z;

  if(!s->d1_have || isnan(s->d1_ma) || isnan(s->d1_atr) || s->d1_atr <= 0.0)
    return(FATHOM_CHOP);

  z = (s->d1_close - s->d1_ma) / s->d1_atr;

  if(z >= FATHOM_REGIME_Z)
    return(FATHOM_BULL);

  if(z <= -FATHOM_REGIME_Z)
    return(FATHOM_CRASH);

  return(FATHOM_CHOP);
}

static double
fathom_target(const fathom_state_t *s, fathom_regime_t r)
{
  if(r == FATHOM_BULL)
    return(s->target_atr * s->target_scale);

  if(r == FATHOM_CRASH)
    return(s->target_atr / s->target_scale);

  return(s->target_atr);
}

static uint32_t
fathom_hold(const fathom_state_t *s, fathom_regime_t r)
{
  double h = (double)s->max_hold;

  if(r == FATHOM_BULL)
    h *= s->hold_scale;

  else if(r == FATHOM_CRASH)
    h /= s->hold_scale;

  if(h < (double)FATHOM_HOLD_MIN)
    return(FATHOM_HOLD_MIN);

  if(h > (double)FATHOM_HOLD_MAX)
    return(FATHOM_HOLD_MAX);

  return((uint32_t)h);
}

// ----------------------------------------------------------------------- //
// Required exports                                                        //
// ----------------------------------------------------------------------- //

static const wm_strategy_param_t fathom_params[] = {
  {
    .name        = "regime_ma",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)FATHOM_DEFAULT_REGIME_MA,
    .min_int     = 0,
    .max_int     = 3,
    .step_dbl    = 1.0,
    .help        = "Daily moving average the regime z-score is measured"
                   " against: 0=EMA_20, 1=EMA_50, 2=SMA_50, 3=SMA_200."
                   " Faster = quicker mode flips. Default 1.",
  },
  {
    .name        = "hold_scale",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = FATHOM_DEFAULT_HOLD_SCALE,
    .min_dbl     = 0.33,
    .max_dbl     = 6.0,
    .step_dbl    = 0.1,
    .help        = "Regime spread on HOLDING TIME, independent of"
                   " target_scale: BULL holds max_hold*hold_scale bars,"
                   " CRASH max_hold/hold_scale. Decoupled in v0.3"
                   " because the far target is what earns in bull mode"
                   " while the long hold is what starves the activity"
                   " gate. 1.0 = uniform holds. Default 1.0.",
  },
  {
    .name        = "dip_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = FATHOM_DEFAULT_DIP_ATR,
    .min_dbl     = 0.25,
    .max_dbl     = 6.0,
    .step_dbl    = 0.25,
    .help        = "CHOP-mode entry displacement: buy when the 1h close"
                   " is dip_atr * ATR_14(1h) below SMA_20(1h). Larger ="
                   " rarer, deeper dislocations. Default 1.5.",
  },
  {
    .name        = "impulse_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = FATHOM_DEFAULT_IMPULSE_ATR,
    .min_dbl     = 0.0,
    .max_dbl     = 6.0,
    .step_dbl    = 0.25,
    .help        = "Velocity gate: also require MOM_10(1h) <="
                   " -impulse_atr * ATR_14(1h), i.e. the displacement"
                   " arrived within ~10 hours. Separates a liquidation"
                   " cascade (paid) from a slow informed downtrend"
                   " (not paid). 0 = off. Default 0.",
  },
  {
    .name        = "target_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = FATHOM_DEFAULT_TARGET_ATR,
    .min_dbl     = 0.25,
    .max_dbl     = 6.0,
    .step_dbl    = 0.25,
    .help        = "CHOP-mode profit target: bank the long at entry +"
                   " target_atr * ATR_14(1h at entry). Default 1.0.",
  },
  {
    .name        = "target_scale",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = FATHOM_DEFAULT_TARGET_SCALE,
    .min_dbl     = 0.33,
    .max_dbl     = 6.0,
    .step_dbl    = 0.1,
    .help        = "Regime spread on the target AND the holding time:"
                   " BULL uses target_atr*target_scale over"
                   " max_hold*target_scale bars, CRASH the reciprocal"
                   " of both. Below 1.0 inverts (crash rides farther"
                   " and longer). 1.0 = no adaptation (the null"
                   " hypothesis). Default 1.5.",
  },
  {
    .name        = "max_hold",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)FATHOM_DEFAULT_MAX_HOLD,
    .min_int     = 2,
    .max_int     = 480,
    .step_dbl    = 4.0,
    .help        = "CHOP-mode time stop in 1h bars (BULL and CRASH"
                   " scale it by target_scale). The primary risk"
                   " control: it exits when the reversion has not"
                   " happened rather than at the worst price."
                   " Default 24.",
  },
  {
    .name        = "stop_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = FATHOM_DEFAULT_STOP_ATR,
    .min_dbl     = 0.0,
    .max_dbl     = 12.0,
    .step_dbl    = 0.5,
    .help        = "Optional disaster stop: cut when close falls to"
                   " entry - stop_atr * ATR_14(1h at entry). 0 = off,"
                   " leaving the time stop in sole charge. Default 0.",
  },
};

void
wm_strategy_describe(wm_strategy_meta_t *out)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->abi_version = WM_STRATEGY_ABI_VERSION;

  snprintf(out->name, sizeof(out->name), "%s", FATHOM_NAME);
  snprintf(out->version, sizeof(out->version), "%s", FATHOM_VERSION);

  // 1h decides/emits; 1d feeds the cached regime classifier only.
  out->grains_mask = (uint16_t)((1u << WM_GRAN_1H) | (1u << WM_GRAN_1D));

  out->min_history[WM_GRAN_1H] = FATHOM_HIST_1H;
  out->min_history[WM_GRAN_1D] = FATHOM_HIST_1D;

  out->params               = fathom_params;
  out->n_params             = (uint32_t)(sizeof(fathom_params)
                                  / sizeof(fathom_params[0]));
  out->wants_trade_callback = false;
}

int
wm_strategy_init(wm_strategy_ctx_t *ctx)
{
  fathom_state_t *s;
  const char     *mid;
  const char     *strat;
  uint32_t        regime_ma;

  if(ctx == NULL)
    return(-1);

  s = mem_alloc("strategy." FATHOM_NAME, "state", sizeof(*s));

  memset(s, 0, sizeof(*s));

  mid   = wm_strategy_ctx_market_id(ctx);
  strat = wm_strategy_ctx_strategy_name(ctx);

  regime_ma = (uint32_t)wm_strategy_kv_get_uint(mid, strat, "regime_ma",
      (uint64_t)FATHOM_DEFAULT_REGIME_MA);
  s->hold_scale = wm_strategy_kv_get_dbl(mid, strat, "hold_scale",
      FATHOM_DEFAULT_HOLD_SCALE);
  s->dip_atr = wm_strategy_kv_get_dbl(mid, strat, "dip_atr",
      FATHOM_DEFAULT_DIP_ATR);
  s->impulse_atr = wm_strategy_kv_get_dbl(mid, strat, "impulse_atr",
      FATHOM_DEFAULT_IMPULSE_ATR);
  s->target_atr = wm_strategy_kv_get_dbl(mid, strat, "target_atr",
      FATHOM_DEFAULT_TARGET_ATR);
  s->target_scale = wm_strategy_kv_get_dbl(mid, strat, "target_scale",
      FATHOM_DEFAULT_TARGET_SCALE);
  s->max_hold = (uint32_t)wm_strategy_kv_get_uint(mid, strat, "max_hold",
      (uint64_t)FATHOM_DEFAULT_MAX_HOLD);
  s->stop_atr = wm_strategy_kv_get_dbl(mid, strat, "stop_atr",
      FATHOM_DEFAULT_STOP_ATR);

  // Defensive clamps — hand-edited KV values may sit outside the
  // declared ranges; bound them so the mechanics stay well-defined.
  // The scales divide, so they must never reach zero.
  if(regime_ma > 3)            regime_ma = 3;
  if(s->hold_scale < 0.05)     s->hold_scale = 0.05;
  if(s->dip_atr < 0.01)        s->dip_atr = 0.01;
  if(s->impulse_atr < 0.0)     s->impulse_atr = 0.0;
  if(s->target_atr < 0.01)     s->target_atr = 0.01;
  if(s->target_scale < 0.05)   s->target_scale = 0.05;
  if(s->max_hold < FATHOM_HOLD_MIN) s->max_hold = FATHOM_HOLD_MIN;
  if(s->max_hold > FATHOM_HOLD_MAX) s->max_hold = FATHOM_HOLD_MAX;
  if(s->stop_atr < 0.0)        s->stop_atr = 0.0;

  s->regime_slot = fathom_regime_slot(regime_ma);

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, FATHOM_LOG_CTX,
      "init: %s -> %s regime_slot=%d hold_scale=%.2f dip_atr=%.2f"
      " impulse_atr=%.2f target_atr=%.2f target_scale=%.2f max_hold=%u"
      " stop_atr=%.2f (1h decide / 1d regime)",
      strat, mid, s->regime_slot, s->hold_scale, s->dip_atr,
      s->impulse_atr, s->target_atr, s->target_scale, s->max_hold,
      s->stop_atr);

  return(0);
}

void
wm_strategy_finalize(wm_strategy_ctx_t *ctx)
{
  fathom_state_t *s;

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
  fathom_state_t       *s;
  wm_strategy_signal_t  sig;
  bool                  fire = false;

  (void)mkt;   // fathom reads only ind[] slots + its own anchors.

  if(bar == NULL || ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s == NULL)
    return;

  // The daily grain only refreshes the cached regime context.
  if(grain != WM_GRAN_1H)
  {
    if(grain == WM_GRAN_1D)
    {
      s->d1_close = bar->close;
      s->d1_ma    = (double)bar->ind[s->regime_slot];
      s->d1_atr   = (double)bar->ind[WM_IND_ATR_14];
      s->d1_have  = true;
    }

    return;
  }

  // ---- 1h decision grain ----
  memset(&sig, 0, sizeof(sig));
  sig.ts_ms = bar->ts_close_ms;

  if(!s->in_position)
  {
    fathom_regime_t r    = fathom_regime(s);
    float           atr  = bar->ind[WM_IND_ATR_14];
    float           mean = bar->ind[WM_IND_SMA_20];
    float           mom  = bar->ind[WM_IND_MOM_10];

    // Entry needs a live ATR anchor and a live short mean; both fail
    // closed so an unwarm slot can never be read as a dislocation.
    if(isnanf(atr) || atr <= 0.0f || isnanf(mean))
      return;

    if(bar->close > (double)mean - s->dip_atr * (double)atr)
      return;

    // Velocity gate (v0.2): the displacement must have ARRIVED, not
    // accumulated. Fails closed while MOM_10 is unwarm.
    if(s->impulse_atr > 0.0
        && (isnanf(mom) || (double)mom > -s->impulse_atr * (double)atr))
      return;

    // CRASH stabilization confirm: take the first up-bar, never the
    // knife mid-plunge. BULL and CHOP dips are bought on the level —
    // waiting there costs more entry than it saves.
    if(r == FATHOM_CRASH && bar->close <= bar->open)
      return;

    sig.score      = 1.0;
    sig.confidence = 0.6;
    snprintf(sig.reason, sizeof(sig.reason), "dip %s",
        fathom_regime_name(r));

    s->in_position  = true;
    s->entry_regime = r;
    s->entry_price  = bar->close;
    s->entry_atr    = (double)atr;
    s->tgt_lv       = bar->close + fathom_target(s, r) * (double)atr;
    s->stop_lv      = bar->close - s->stop_atr * (double)atr;
    s->hold_left    = fathom_hold(s, r);
    fire            = true;
  }

  else
  {
    const char *why = NULL;

    if(s->hold_left > 0u)
      s->hold_left--;

    if(bar->close >= s->tgt_lv)
      why = "tgt";

    else if(s->stop_atr > 0.0 && bar->close <= s->stop_lv)
      why = "stop";

    else if(s->hold_left == 0u)
      why = "time";

    if(why != NULL)
    {
      sig.score      = -1.0;
      sig.confidence = 0.6;
      snprintf(sig.reason, sizeof(sig.reason), "%s %s", why,
          fathom_regime_name(s->entry_regime));

      s->in_position = false;
      fire           = true;
    }
  }

  if(fire)
  {
    wm_strategy_emit_signal(ctx, &sig);

    clam(CLAM_DEBUG3, FATHOM_LOG_CTX,
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
fathom_plugin_init(void)
{
  clam(CLAM_INFO, FATHOM_LOG_CTX,
      "%s v%s loaded", FATHOM_NAME, FATHOM_VERSION);
  return(false);
}

static void
fathom_plugin_deinit(void)
{
  clam(CLAM_INFO, FATHOM_LOG_CTX, "%s deinit", FATHOM_NAME);
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "strategy_" FATHOM_NAME,
  .version         = FATHOM_VERSION,
  .type            = PLUGIN_STRATEGY,
  .kind            = FATHOM_NAME,
  .provides        = { { .name = "strategy_" FATHOM_NAME } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_whenmoon" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = fathom_plugin_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = fathom_plugin_deinit,
  .ext             = NULL,
};
