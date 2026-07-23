// botmanager — MIT
// keel — default-long exposure with drawdown-state risk-off carve-outs.
//
// A keel does not steer the boat and it does not predict the weather —
// it just refuses to let the worst states capsize you. That is this
// strategy's entire bet, and it is the graveyard's bet inverted: three
// families (riptide-4h, hotflag, squall) died proving that long-only
// ENTRY-TIMING earns ~bench per unit exposure on these corpora. So
// keel never times an entry. Long is the RESTING state — the strategy
// holds deployment-matched bench exposure by default and spends its
// entire signal budget on when NOT to be long: a drawdown carve-out
// (close decays more than dd_off% below the trailing peak — a path /
// high-water property no moving-average level test sees) plus an
// optional volatility-blowout carve-out, with structural-recovery
// re-entry (MA reclaim + drawdown band cleared + optional debounce).
// Active return = bear-segment losses avoided − whipsaw round-trips in
// dips that recovered − re-entry lag paid after real bottoms.
//
// Mechanics (long-only, single position, decisions on closed 1d bars):
//   default  — once the trailing-peak window is warm, be long.
//   risk-off — close < trailing_peak × (1 − dd_off/100), where the
//              peak is the max close of the prior peak_win 1d bars
//              (ring pushed AFTER the test — strictly-prior window);
//              or, when vol_off > 0, NATR_14(1d) ranks at/above the
//              vol_off-th percentile of its own trailing vol_win ring.
//   risk-on  — at least min_flat 1d bars flat, close back above the
//              recovery MA (rec_ma ordinal), and the drawdown state
//              cleared. The rolling peak decays as old highs leave the
//              window, so a long bear eventually redefines its own
//              baseline — hysteresis with a built-in clock.
//
// Params (6, orthogonal; defaults are the pre-registered starting
// point, sweep ranges live in HYPOTHESES.md):
//   dd_off    drawdown %% from the trailing peak that sheds exposure
//   peak_win  1d bars in the trailing-peak ring
//   rec_ma    recovery MA ordinal (0=EMA_9 1=EMA_20 2=SMA_50 3=SMA_200)
//   min_flat  minimum 1d bars flat before re-entry (debounce)
//   vol_off   optional NATR(1d) blowout percentile carve-out (0 = off)
//   vol_win   trailing ring the vol percentile ranks against
//
// LOOKAHEAD SAFETY. Decisions run on closed 1d bars only and read only
// THIS bar's ind[] slots plus strategy-owned rings pushed AFTER each
// decision, so every window holds strictly-prior bars in live and
// backtest alike. No reach into mkt->grain_arr[] (in backtest that
// ring holds the whole, future, range).

#include "whenmoon_strategy.h"

#include "alloc.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define KEEL_NAME     "keel"
#define KEEL_VERSION  "0.1"
#define KEEL_LOG_CTX  "strategy.keel"

// Ring capacities — the widest pre-registered sweep values. Params are
// clamped to these caps at init so the rings can be fixed-size.
#define KEEL_PEAK_RING_CAP  180u
#define KEEL_VOL_RING_CAP   180u

// Live warm-up history (1d bars): sized for the FROZEN config
// (peak_win 60 + EMA_20 reclaim, with margin), NOT the widest sweep —
// the official walk-forward recipe (train=365d) preflights against
// this figure and a 400-bar demand would refuse the pre-registered
// recipe. Wide-sweep configs (peak_win > 100, rec_ma = SMA_200) warm
// inside the window instead: every read fails closed until its ring /
// ind[] slot is ready, costing early exposure rather than correctness.
#define KEEL_HIST_1D  160u

// Defaults — mirrored in the param schema and pre-registered in
// HYPOTHESES.md (H-keel-001) before any scoring run.
#define KEEL_DEFAULT_DD_OFF    15.0
#define KEEL_DEFAULT_PEAK_WIN  90.0
#define KEEL_DEFAULT_REC_MA    1.0
#define KEEL_DEFAULT_MIN_FLAT  0.0
#define KEEL_DEFAULT_VOL_OFF   0.0
#define KEEL_DEFAULT_VOL_WIN   180.0

typedef struct
{
  // Resolved params (snapshot at init; reload to pick up KV changes).
  double     dd_off;
  uint32_t   peak_win;
  int        rec_ma;      // 0=EMA_9 1=EMA_20 2=SMA_50 3=SMA_200
  uint32_t   min_flat;
  double     vol_off;
  uint32_t   vol_win;

  // Trailing close ring — the drawdown reference, pushed post-decision.
  double     close_ring[KEEL_PEAK_RING_CAP];
  uint32_t   close_n;
  uint32_t   close_head;

  // Trailing NATR_14(1d) ring for the optional blowout percentile.
  double     natr_ring[KEEL_VOL_RING_CAP];
  uint32_t   natr_n;
  uint32_t   natr_head;

  // Position state.
  bool       in_position;
  bool       ever_entered;   // first warm bar enters unconditionally
  uint32_t   flat_bars;      // decision bars since the last exit
} keel_state_t;

// ----------------------------------------------------------------------- //
// Param schema                                                            //
// ----------------------------------------------------------------------- //

static const wm_strategy_param_t keel_params[] = {
  {
    .name        = "dd_off",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = KEEL_DEFAULT_DD_OFF,
    .min_dbl     = 5.0,
    .max_dbl     = 50.0,
    .step_dbl    = 2.5,
    .help        = "Drawdown percent below the trailing peak close that"
                   " sheds exposure (15 = exit when the close sits more"
                   " than 15% under the prior peak_win-bar peak)."
                   " Default 15.",
  },
  {
    .name        = "peak_win",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)KEEL_DEFAULT_PEAK_WIN,
    .min_int     = 30,
    .max_int     = 180,
    .step_dbl    = 30.0,
    .help        = "1d bars in the trailing-peak ring the drawdown test"
                   " references (90 = one quarter). Old peaks roll out,"
                   " so a long bear eventually resets its own baseline."
                   " Default 90.",
  },
  {
    .name        = "rec_ma",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)KEEL_DEFAULT_REC_MA,
    .min_int     = 0,
    .max_int     = 3,
    .step_dbl    = 1.0,
    .help        = "Recovery MA the close must reclaim before re-entry:"
                   " 0=EMA_9(1d) 1=EMA_20(1d) 2=SMA_50(1d)"
                   " 3=SMA_200(1d). Default 1.",
  },
  {
    .name        = "min_flat",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)KEEL_DEFAULT_MIN_FLAT,
    .min_int     = 0,
    .max_int     = 30,
    .step_dbl    = 2.0,
    .help        = "Debounce: minimum 1d bars to stay flat after a"
                   " risk-off exit before re-entry is considered"
                   " (0 = re-enter as soon as conditions clear)."
                   " Default 0.",
  },
  {
    .name        = "vol_off",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = KEEL_DEFAULT_VOL_OFF,
    .min_dbl     = 0.0,
    .max_dbl     = 99.0,
    .step_dbl    = 5.0,
    .help        = "Optional volatility carve-out: exit when NATR_14(1d)"
                   " ranks at or above this percentile of its trailing"
                   " vol_win ring (0 disables). Pre-registered as a"
                   " probe axis, NOT a rescue knob. Default 0 (off).",
  },
  {
    .name        = "vol_win",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)KEEL_DEFAULT_VOL_WIN,
    .min_int     = 90,
    .max_int     = 180,
    .step_dbl    = 90.0,
    .help        = "Trailing 1d bars the volatility percentile ranks"
                   " against (only consulted when vol_off > 0)."
                   " Default 180.",
  },
};

// ----------------------------------------------------------------------- //
// Helpers                                                                 //
// ----------------------------------------------------------------------- //

// Highest close of the prior peak_win bars. Valid only once the ring
// holds peak_win samples (fails closed while warming).
static bool
keel_trailing_peak(const keel_state_t *s, double *peak_out)
{
  double   peak = 0.0;
  uint32_t i;

  if(s->close_n < s->peak_win)
    return(false);

  for(i = 0; i < s->peak_win; i++)
  {
    uint32_t idx = (s->close_head + KEEL_PEAK_RING_CAP - 1u - i)
                       % KEEL_PEAK_RING_CAP;

    if(s->close_ring[idx] > peak)
      peak = s->close_ring[idx];
  }

  *peak_out = peak;

  return(true);
}

static void
keel_close_push(keel_state_t *s, double close)
{
  s->close_ring[s->close_head] = close;
  s->close_head = (s->close_head + 1u) % KEEL_PEAK_RING_CAP;

  if(s->close_n < KEEL_PEAK_RING_CAP)
    s->close_n++;
}

// Volatility blowout test against the trailing NATR window: the
// fraction of prior values strictly below the current one is its rank.
// Requires a full window (fails closed — no blowout while warming).
static bool
keel_vol_blowout(const keel_state_t *s, double natr)
{
  uint32_t below = 0;
  uint32_t i;

  if(s->vol_off <= 0.0 || s->natr_n < s->vol_win)
    return(false);

  for(i = 0; i < s->vol_win; i++)
    if(s->natr_ring[i] < natr)
      below++;

  return((double)below / (double)s->vol_win * 100.0 >= s->vol_off);
}

static void
keel_natr_push(keel_state_t *s, double natr)
{
  s->natr_ring[s->natr_head] = natr;
  s->natr_head = (s->natr_head + 1u) % s->vol_win;

  if(s->natr_n < s->vol_win)
    s->natr_n++;
}

// Recovery MA read off the current bar's ind[] slots by ordinal.
static float
keel_rec_ma_value(const keel_state_t *s, const wm_candle_full_t *bar)
{
  switch(s->rec_ma)
  {
    case 0:  return(bar->ind[WM_IND_EMA_9]);
    case 1:  return(bar->ind[WM_IND_EMA_20]);
    case 2:  return(bar->ind[WM_IND_SMA_50]);
    default: return(bar->ind[WM_IND_SMA_200]);
  }
}

// ----------------------------------------------------------------------- //
// Required exports                                                        //
// ----------------------------------------------------------------------- //

void
wm_strategy_describe(wm_strategy_meta_t *out)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->abi_version = WM_STRATEGY_ABI_VERSION;

  snprintf(out->name, sizeof(out->name), "%s", KEEL_NAME);
  snprintf(out->version, sizeof(out->version), "%s", KEEL_VERSION);

  // 1d decisions only — the slowest strategy in the fleet by design.
  out->grains_mask = (uint16_t)(1u << WM_GRAN_1D);

  out->min_history[WM_GRAN_1D] = KEEL_HIST_1D;

  out->params               = keel_params;
  out->n_params             = (uint32_t)(sizeof(keel_params)
                                  / sizeof(keel_params[0]));
  out->wants_trade_callback = false;
}

int
wm_strategy_init(wm_strategy_ctx_t *ctx)
{
  keel_state_t *s;
  const char   *mid;
  const char   *strat;

  if(ctx == NULL)
    return(-1);

  s = mem_alloc("strategy." KEEL_NAME, "state", sizeof(*s));

  memset(s, 0, sizeof(*s));

  mid   = wm_strategy_ctx_market_id(ctx);
  strat = wm_strategy_ctx_strategy_name(ctx);

  s->dd_off = wm_strategy_kv_get_dbl(mid, strat, "dd_off",
      KEEL_DEFAULT_DD_OFF);
  s->peak_win = (uint32_t)wm_strategy_kv_get_uint(mid, strat,
      "peak_win", (uint64_t)KEEL_DEFAULT_PEAK_WIN);
  s->rec_ma = (int)wm_strategy_kv_get_uint(mid, strat, "rec_ma",
      (uint64_t)KEEL_DEFAULT_REC_MA);
  s->min_flat = (uint32_t)wm_strategy_kv_get_uint(mid, strat,
      "min_flat", (uint64_t)KEEL_DEFAULT_MIN_FLAT);
  s->vol_off = wm_strategy_kv_get_dbl(mid, strat, "vol_off",
      KEEL_DEFAULT_VOL_OFF);
  s->vol_win = (uint32_t)wm_strategy_kv_get_uint(mid, strat,
      "vol_win", (uint64_t)KEEL_DEFAULT_VOL_WIN);

  if(s->dd_off < 5.0)                  s->dd_off = 5.0;
  if(s->dd_off > 50.0)                 s->dd_off = 50.0;
  if(s->peak_win < 30u)                s->peak_win = 30u;
  if(s->peak_win > KEEL_PEAK_RING_CAP) s->peak_win = KEEL_PEAK_RING_CAP;
  if(s->rec_ma < 0)                    s->rec_ma = 0;
  if(s->rec_ma > 3)                    s->rec_ma = 3;
  if(s->vol_off < 0.0)                 s->vol_off = 0.0;
  if(s->vol_off > 99.0)                s->vol_off = 99.0;
  if(s->vol_win < 90u)                 s->vol_win = 90u;
  if(s->vol_win > KEEL_VOL_RING_CAP)   s->vol_win = KEEL_VOL_RING_CAP;

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, KEEL_LOG_CTX,
      "init: %s -> %s dd_off=%.1f peak_win=%u rec_ma=%d min_flat=%u"
      " vol_off=%.1f vol_win=%u",
      strat, mid, s->dd_off, s->peak_win, s->rec_ma, s->min_flat,
      s->vol_off, s->vol_win);

  return(0);
}

void
wm_strategy_finalize(wm_strategy_ctx_t *ctx)
{
  keel_state_t *s;

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
  keel_state_t         *s;
  wm_strategy_signal_t  sig;
  double                close;
  double                peak    = 0.0;
  float                 natr;
  bool                  have_peak;
  bool                  dd_state = false;
  bool                  fire     = false;

  (void)mkt;   // keel reads only ind[] slots + its own rings.

  if(bar == NULL || ctx == NULL || grain != WM_GRAN_1D)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s == NULL)
    return;

  close     = bar->close;
  natr      = bar->ind[WM_IND_NATR_14];
  have_peak = keel_trailing_peak(s, &peak);

  if(have_peak)
    dd_state = close < peak * (1.0 - s->dd_off / 100.0);

  memset(&sig, 0, sizeof(sig));
  sig.ts_ms = bar->ts_close_ms;

  if(s->in_position)
  {
    const char *why = NULL;

    if(have_peak && dd_state)
      why = "dd";
    else if(!isnanf(natr) && keel_vol_blowout(s, (double)natr))
      why = "vol";

    if(why != NULL)
    {
      sig.score      = -1.0;
      sig.confidence = 0.5;
      snprintf(sig.reason, sizeof(sig.reason), "risk-off %s", why);

      s->in_position = false;
      s->flat_bars   = 0u;
      fire           = true;
    }
  }

  else if(have_peak)
  {
    if(!s->ever_entered)
    {
      // The default state asserts itself: first warm bar goes long,
      // unconditionally. If the tape is mid-bear the carve-out sheds
      // it on a later close — that IS the mechanism, not a special
      // case to defend against.
      sig.score      = 1.0;
      sig.confidence = 0.6;
      snprintf(sig.reason, sizeof(sig.reason), "keel default");

      s->in_position  = true;
      s->ever_entered = true;
      fire            = true;
    }

    else
    {
      float ma = keel_rec_ma_value(s, bar);

      s->flat_bars++;

      if(s->flat_bars >= s->min_flat && !dd_state &&
         !isnanf(ma) && close > (double)ma)
      {
        sig.score      = 1.0;
        sig.confidence = 0.6;
        snprintf(sig.reason, sizeof(sig.reason), "keel reclaim");

        s->in_position = true;
        s->flat_bars   = 0u;
        fire           = true;
      }
    }
  }

  // Push this bar into the trailing windows AFTER the decision, so the
  // windows only ever hold strictly-prior bars.
  keel_close_push(s, close);

  if(!isnanf(natr))
    keel_natr_push(s, (double)natr);

  if(fire)
  {
    wm_strategy_emit_signal(ctx, &sig);

    clam(CLAM_INFO, KEEL_LOG_CTX,
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
keel_plugin_init(void)
{
  clam(CLAM_INFO, KEEL_LOG_CTX, "%s v%s loaded", KEEL_NAME,
      KEEL_VERSION);
  return(false);
}

static void
keel_plugin_deinit(void)
{
  clam(CLAM_INFO, KEEL_LOG_CTX, "%s deinit", KEEL_NAME);
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "strategy_" KEEL_NAME,
  .version         = KEEL_VERSION,
  .type            = PLUGIN_STRATEGY,
  .kind            = KEEL_NAME,
  .provides        = { { .name = "strategy_" KEEL_NAME } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_whenmoon" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = keel_plugin_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = keel_plugin_deinit,
  .ext             = NULL,
};
