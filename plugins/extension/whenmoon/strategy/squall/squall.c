// botmanager — MIT
// squall — volatility-compression -> upward-expansion breakout rider.
//
// A squall is the sudden violent wind that breaks a calm. That is this
// strategy's entire bet: markets that have gone unusually QUIET resolve
// into directional expansion, and the first upward range break out of a
// deep calm is the moment to be aboard. squall reads a signal source no
// incumbent touches — VOLATILITY STRUCTURE. riptide reads dip depth
// (ATR as a distance unit), juggernaut reads ADX trend strength; squall
// ranks the market's own normalized volatility (NATR_14 on the 4h
// grain) against its trailing self-history and only hunts while that
// rank sits in the compressed tail. Compression is a low-ADX state, so
// juggernaut's entry gate is typically SHUT exactly when squall arms —
// the two are near-disjoint in time by construction, which is the
// differentiation claim the fold-correlation gate measures.
//
// Mechanics (long-only, single position, decisions on closed bars):
//   coil     — NATR_14(4h) at or below the squeeze_pctl-th percentile
//              of its own prior squeeze_win 4h values (ring pushed
//              AFTER the test — the window only ever holds strictly-
//              prior bars). Each coil close while flat (re)arms the
//              breakout hunt for arm_expiry decision bars.
//   entry    — armed + the decision close breaks above the highest
//              HIGH of the prior break_n decision bars (Donchian break
//              of a strictly-prior window) + optional participation
//              confirm (bar volume >= vol_confirm x the mean volume of
//              those same prior bars) + optional 1d regime gate.
//   exit     — chandelier trail: close below (highest high since entry
//              - chand_atr x ATR_14(decision)); or max_hold decision
//              bars elapsed (when set); or the 1d regime flips down
//              (only when regime_gate=1). Ride the expansion — exits
//              on vol-normalization alone would starve bull-fold
//              exposure, the active-return wall that kills low-duty-
//              cycle timing here.
//
// Params (9, orthogonal; defaults are the pre-registered starting
// point, sweep ranges live in HYPOTHESES.md):
//   squeeze_win    trailing 4h bars ranked for the coil test
//   squeeze_pctl   percentile at/below which the market is coiled
//   arm_expiry     decision bars an arm persists before it lapses
//   break_n        Donchian lookback (decision bars) for the break
//   chand_atr      chandelier trail distance in ATR_14(decision)
//   vol_confirm    breakout volume vs trailing-mean multiple (0 = off)
//   regime_gate    1 = require 1d close > EMA_50(1d) (0 = off; the
//                  family claim is that compression timing itself
//                  carries the edge — see HYPOTHESES falsification)
//   max_hold       force-exit after N decision bars (0 = off)
//   decision_grain 0 = decide on 1h closes (default), 1 = decide on 4h
//
// LOOKAHEAD SAFETY. The backtest fires on_bar in a merged chronological
// walk; on a shared timestamp the FINER grain fires first (1h before 4h
// before 1d). squall caches 4h coil state and 1d regime as those bars
// close and reads the caches only on a later decision bar. Every
// trailing window it consults (NATR percentile ring, Donchian highs,
// volume mean) is pushed AFTER the bar's decision, so a window holds
// strictly-prior bars in live and backtest alike. The decision reads
// only THIS bar's own ind[] slots plus strategy-owned scalars tracked
// forward. No reach into mkt->grain_arr[] (in backtest that ring holds
// the whole, future, range).

#include "whenmoon_strategy.h"

#include "alloc.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define SQUALL_NAME     "squall"
#define SQUALL_VERSION  "0.1"
#define SQUALL_LOG_CTX  "strategy.squall"

// Ring capacities — the widest pre-registered sweep values. Params are
// clamped to these caps at init so the rings can be fixed-size.
#define SQUALL_NATR_RING_CAP   360u
#define SQUALL_BREAK_RING_CAP  96u

// Per-grain live warm-up history (bars). Backtest snapshots carry the
// full range regardless; this only matters at live cold start. Sized
// for the widest sweep values so a live attach warms the worst case:
// 1h covers break_n(96) + ATR/EMA warmup, 4h covers squeeze_win(360) +
// NATR_14 warmup, 1d covers the EMA_50 regime MA.
#define SQUALL_HIST_1H  210u
#define SQUALL_HIST_4H  420u
#define SQUALL_HIST_1D  128u

// Defaults — mirrored in the param schema and pre-registered in
// HYPOTHESES.md (H-squall-001) before any scoring run.
#define SQUALL_DEFAULT_SQUEEZE_WIN     180.0  // ~30 days of 4h bars
#define SQUALL_DEFAULT_SQUEEZE_PCTL    20.0   // coil = bottom quintile
#define SQUALL_DEFAULT_ARM_EXPIRY      24.0   // one day of 1h decisions
#define SQUALL_DEFAULT_BREAK_N         24.0   // one-day Donchian break
#define SQUALL_DEFAULT_CHAND_ATR       3.0    // trail give-back
#define SQUALL_DEFAULT_VOL_CONFIRM     0.0    // participation confirm off
#define SQUALL_DEFAULT_REGIME_GATE     0.0    // 1d gate off (see notes)
#define SQUALL_DEFAULT_MAX_HOLD        0.0    // unbounded ride
#define SQUALL_DEFAULT_DECISION_GRAIN  0.0    // 0 = 1h decisions

typedef struct
{
  // Resolved params (snapshot at init; reload to pick up KV changes).
  uint32_t   squeeze_win;
  double     squeeze_pctl;
  uint32_t   arm_expiry;
  uint32_t   break_n;
  double     chand_atr;
  double     vol_confirm;
  int        regime_gate;
  uint32_t   max_hold;
  int        decision_grain;   // 0 = 1h decisions, 1 = 4h decisions

  // Coil detection (4h): trailing NATR_14 ring, pushed after the test.
  double     natr_ring[SQUALL_NATR_RING_CAP];
  uint32_t   natr_n;
  uint32_t   natr_head;

  // Cached 1d regime context, latched as 1d bars close.
  double     d1_close, d1_ema50;   bool d1_have;

  // Breakout hunt: armed by a coil close, lapses after arm_expiry
  // decision bars without a break.
  bool       armed;
  uint32_t   arm_left;

  // Decision-grain trailing windows, pushed after each decision.
  double     high_ring[SQUALL_BREAK_RING_CAP];
  double     vol_ring[SQUALL_BREAK_RING_CAP];
  uint32_t   ring_n;
  uint32_t   ring_head;

  // Position state.
  bool       in_position;
  double     entry_price;
  double     hh_since_entry;   // chandelier high-water mark
  uint32_t   hold_bars;
} squall_state_t;

// ----------------------------------------------------------------------- //
// Param schema                                                            //
// ----------------------------------------------------------------------- //

static const wm_strategy_param_t squall_params[] = {
  {
    .name        = "squeeze_win",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)SQUALL_DEFAULT_SQUEEZE_WIN,
    .min_int     = 30,
    .max_int     = 360,
    .step_dbl    = 30.0,
    .help        = "Trailing 4h bars the coil test ranks NATR_14 against"
                   " (180 = ~30 days). The window holds strictly-prior"
                   " bars; no arming until it is full. Default 180.",
  },
  {
    .name        = "squeeze_pctl",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = SQUALL_DEFAULT_SQUEEZE_PCTL,
    .min_dbl     = 1.0,
    .max_dbl     = 50.0,
    .step_dbl    = 5.0,
    .help        = "Percentile of the trailing NATR_14 window at or"
                   " below which the market counts as coiled (20 ="
                   " bottom quintile). Lower = rarer, deeper calms."
                   " Default 20.",
  },
  {
    .name        = "arm_expiry",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)SQUALL_DEFAULT_ARM_EXPIRY,
    .min_int     = 1,
    .max_int     = 500,
    .step_dbl    = 6.0,
    .help        = "Decision bars a coil arm persists before it lapses"
                   " unbroken. Each fresh coil close while flat"
                   " re-arms. Default 24.",
  },
  {
    .name        = "break_n",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)SQUALL_DEFAULT_BREAK_N,
    .min_int     = 4,
    .max_int     = 96,
    .step_dbl    = 8.0,
    .help        = "Donchian lookback (decision bars): entry needs the"
                   " close above the highest HIGH of the prior break_n"
                   " bars. Also the volume-confirm averaging window."
                   " Default 24.",
  },
  {
    .name        = "chand_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = SQUALL_DEFAULT_CHAND_ATR,
    .min_dbl     = 1.0,
    .max_dbl     = 8.0,
    .step_dbl    = 0.5,
    .help        = "Chandelier trailing-stop distance in ATR_14"
                   " (decision grain) below the highest high since"
                   " entry. Default 3.0.",
  },
  {
    .name        = "vol_confirm",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = SQUALL_DEFAULT_VOL_CONFIRM,
    .min_dbl     = 0.0,
    .max_dbl     = 4.0,
    .step_dbl    = 0.5,
    .help        = "Participation confirm: breakout bar volume must be"
                   " at least this multiple of the prior break_n bars'"
                   " mean volume (0 disables). Default 0 (off).",
  },
  {
    .name        = "regime_gate",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)SQUALL_DEFAULT_REGIME_GATE,
    .min_int     = 0,
    .max_int     = 1,
    .step_dbl    = 1.0,
    .help        = "1 = entries require the cached 1d close above"
                   " EMA_50(1d), and a regime flip-down force-exits."
                   " 0 = off — the pre-registered family claim is that"
                   " compression timing itself carries the edge."
                   " Default 0.",
  },
  {
    .name        = "max_hold",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)SQUALL_DEFAULT_MAX_HOLD,
    .min_int     = 0,
    .max_int     = 720,
    .step_dbl    = 24.0,
    .help        = "Force-exit after this many decision bars in"
                   " position (0 = unbounded — the chandelier is the"
                   " only ride limit). Default 0 (off).",
  },
  {
    .name        = "decision_grain",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)SQUALL_DEFAULT_DECISION_GRAIN,
    .min_int     = 0,
    .max_int     = 1,
    .step_dbl    = 1.0,
    .help        = "Grain the break/exit decisions run on: 0 = 1h"
                   " (default), 1 = 4h (the coil is always measured on"
                   " 4h regardless). Default 0.",
  },
};

// ----------------------------------------------------------------------- //
// Helpers                                                                 //
// ----------------------------------------------------------------------- //

// Coil test against the trailing NATR window: the fraction of prior
// values strictly below the current one is the current bar's rank.
// Requires a full window (fails closed while warming). O(win) scan —
// 360 doubles at 4h cadence is noise.
static bool
squall_is_coiled(const squall_state_t *s, double natr)
{
  uint32_t below = 0;
  uint32_t i;

  if(s->natr_n < s->squeeze_win)
    return(false);

  for(i = 0; i < s->squeeze_win; i++)
    if(s->natr_ring[i] < natr)
      below++;

  return((double)below / (double)s->squeeze_win
             <= s->squeeze_pctl / 100.0);
}

static void
squall_natr_push(squall_state_t *s, double natr)
{
  s->natr_ring[s->natr_head] = natr;
  s->natr_head = (s->natr_head + 1u) % s->squeeze_win;

  if(s->natr_n < s->squeeze_win)
    s->natr_n++;
}

// Highest high and mean volume over the prior break_n decision bars.
// Valid only once the ring holds break_n samples (fails closed).
static bool
squall_prior_window(const squall_state_t *s, double *hh_out,
    double *volmean_out)
{
  double   hh  = 0.0;
  double   sum = 0.0;
  uint32_t i;

  if(s->ring_n < s->break_n)
    return(false);

  // Walk the last break_n entries; the ring is pushed post-decision so
  // every entry is strictly prior to the bar under decision.
  for(i = 0; i < s->break_n; i++)
  {
    uint32_t idx = (s->ring_head + SQUALL_BREAK_RING_CAP - 1u - i)
                       % SQUALL_BREAK_RING_CAP;

    if(s->high_ring[idx] > hh)
      hh = s->high_ring[idx];

    sum += s->vol_ring[idx];
  }

  *hh_out      = hh;
  *volmean_out = sum / (double)s->break_n;

  return(true);
}

static void
squall_ring_push(squall_state_t *s, double high, double volume)
{
  s->high_ring[s->ring_head] = high;
  s->vol_ring[s->ring_head]  = volume;
  s->ring_head = (s->ring_head + 1u) % SQUALL_BREAK_RING_CAP;

  if(s->ring_n < SQUALL_BREAK_RING_CAP)
    s->ring_n++;
}

// 1d regime: cached close above cached EMA_50(1d). Fails closed until
// the 1d grain has produced a bar with a warm EMA. Only consulted when
// regime_gate=1.
static bool
squall_regime_up(const squall_state_t *s)
{
  return(s->d1_have && !isnan(s->d1_ema50) &&
         s->d1_close > s->d1_ema50);
}

// ----------------------------------------------------------------------- //
// Decision core — runs on the decision grain's closed bars              //
// ----------------------------------------------------------------------- //

static void
squall_decide(wm_strategy_ctx_t *ctx, squall_state_t *s,
    const wm_candle_full_t *bar)
{
  wm_strategy_signal_t  sig;
  float                 atr;
  double                close;
  bool                  have_core;
  bool                  fire = false;

  close     = bar->close;
  atr       = bar->ind[WM_IND_ATR_14];
  have_core = !isnanf(atr) && atr > 0.0f;

  memset(&sig, 0, sizeof(sig));
  sig.ts_ms = bar->ts_close_ms;

  if(s->in_position)
  {
    const char *why = NULL;

    s->hold_bars++;

    // Conventional chandelier: the high-water mark includes this bar's
    // high; the stop is tested against this bar's close. Both belong
    // to the same closed bar — no lookahead.
    if(bar->high > s->hh_since_entry)
      s->hh_since_entry = bar->high;

    if(s->regime_gate == 1 && !squall_regime_up(s))
      why = "regime";
    else if(have_core &&
            close < s->hh_since_entry - s->chand_atr * (double)atr)
      why = "chand";
    else if(s->max_hold > 0u && s->hold_bars >= s->max_hold)
      why = "hold";

    if(why != NULL)
    {
      sig.score      = -1.0;
      sig.confidence = 0.5;
      snprintf(sig.reason, sizeof(sig.reason), "exit %s", why);

      s->in_position    = false;
      s->entry_price    = 0.0;
      s->hh_since_entry = 0.0;
      s->hold_bars      = 0u;
      fire              = true;
    }
  }

  else if(s->armed)
  {
    // Breakout hunt: close above the prior Donchian high, optional
    // participation confirm, optional 1d regime gate.
    double hh      = 0.0;
    double volmean = 0.0;

    if(squall_prior_window(s, &hh, &volmean) && close > hh &&
       (s->vol_confirm <= 0.0 ||
        (volmean > 0.0 && bar->volume >= s->vol_confirm * volmean)) &&
       (s->regime_gate == 0 || squall_regime_up(s)))
    {
      sig.score      = 1.0;
      sig.confidence = 0.6;
      snprintf(sig.reason, sizeof(sig.reason), "squall break %un",
          s->break_n);

      s->in_position    = true;
      s->entry_price    = close;
      s->hh_since_entry = bar->high;
      s->hold_bars      = 0u;
      s->armed          = false;
      s->arm_left       = 0u;
      fire              = true;
    }
  }

  // Arm countdown AFTER the entry evaluation, so arm_expiry=N grants
  // exactly N decision bars of opportunity.
  if(s->armed)
  {
    if(s->arm_left > 0u)
      s->arm_left--;

    if(s->arm_left == 0u)
      s->armed = false;
  }

  // Push this bar into the trailing windows AFTER the decision, so the
  // windows only ever hold strictly-prior bars.
  squall_ring_push(s, bar->high, bar->volume);

  if(fire)
  {
    wm_strategy_emit_signal(ctx, &sig);

    clam(CLAM_INFO, SQUALL_LOG_CTX,
        "%s -> %s: %s @ ts=%lld close=%.2f",
        wm_strategy_ctx_strategy_name(ctx),
        wm_strategy_ctx_market_id(ctx),
        sig.reason, (long long)bar->ts_close_ms, close);
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

  snprintf(out->name, sizeof(out->name), "%s", SQUALL_NAME);
  snprintf(out->version, sizeof(out->version), "%s", SQUALL_VERSION);

  // 1h decision grain + 4h coil grain + 1d regime cache.
  out->grains_mask = (uint16_t)((1u << WM_GRAN_1H)  |
                                (1u << WM_GRAN_4H)  |
                                (1u << WM_GRAN_1D));

  out->min_history[WM_GRAN_1H]  = SQUALL_HIST_1H;
  out->min_history[WM_GRAN_4H]  = SQUALL_HIST_4H;
  out->min_history[WM_GRAN_1D]  = SQUALL_HIST_1D;

  out->params               = squall_params;
  out->n_params             = (uint32_t)(sizeof(squall_params)
                                  / sizeof(squall_params[0]));
  out->wants_trade_callback = false;
}

int
wm_strategy_init(wm_strategy_ctx_t *ctx)
{
  squall_state_t *s;
  const char     *mid;
  const char     *strat;

  if(ctx == NULL)
    return(-1);

  s = mem_alloc("strategy." SQUALL_NAME, "state", sizeof(*s));

  memset(s, 0, sizeof(*s));

  mid   = wm_strategy_ctx_market_id(ctx);
  strat = wm_strategy_ctx_strategy_name(ctx);

  s->squeeze_win = (uint32_t)wm_strategy_kv_get_uint(mid, strat,
      "squeeze_win", (uint64_t)SQUALL_DEFAULT_SQUEEZE_WIN);
  s->squeeze_pctl = wm_strategy_kv_get_dbl(mid, strat, "squeeze_pctl",
      SQUALL_DEFAULT_SQUEEZE_PCTL);
  s->arm_expiry = (uint32_t)wm_strategy_kv_get_uint(mid, strat,
      "arm_expiry", (uint64_t)SQUALL_DEFAULT_ARM_EXPIRY);
  s->break_n = (uint32_t)wm_strategy_kv_get_uint(mid, strat,
      "break_n", (uint64_t)SQUALL_DEFAULT_BREAK_N);
  s->chand_atr = wm_strategy_kv_get_dbl(mid, strat, "chand_atr",
      SQUALL_DEFAULT_CHAND_ATR);
  s->vol_confirm = wm_strategy_kv_get_dbl(mid, strat, "vol_confirm",
      SQUALL_DEFAULT_VOL_CONFIRM);
  s->regime_gate = (int)wm_strategy_kv_get_uint(mid, strat,
      "regime_gate", (uint64_t)SQUALL_DEFAULT_REGIME_GATE);
  s->max_hold = (uint32_t)wm_strategy_kv_get_uint(mid, strat,
      "max_hold", (uint64_t)SQUALL_DEFAULT_MAX_HOLD);
  s->decision_grain = (int)wm_strategy_kv_get_uint(mid, strat,
      "decision_grain", (uint64_t)SQUALL_DEFAULT_DECISION_GRAIN);

  if(s->squeeze_win < 30u)               s->squeeze_win = 30u;
  if(s->squeeze_win > SQUALL_NATR_RING_CAP)
    s->squeeze_win = SQUALL_NATR_RING_CAP;
  if(s->squeeze_pctl < 1.0)              s->squeeze_pctl = 1.0;
  if(s->squeeze_pctl > 50.0)             s->squeeze_pctl = 50.0;
  if(s->arm_expiry < 1u)                 s->arm_expiry = 1u;
  if(s->break_n < 4u)                    s->break_n = 4u;
  if(s->break_n > SQUALL_BREAK_RING_CAP) s->break_n = SQUALL_BREAK_RING_CAP;
  if(s->chand_atr <= 0.0)                s->chand_atr = SQUALL_DEFAULT_CHAND_ATR;
  if(s->vol_confirm < 0.0)               s->vol_confirm = 0.0;
  if(s->regime_gate < 0)                 s->regime_gate = 0;
  if(s->regime_gate > 1)                 s->regime_gate = 1;
  if(s->decision_grain < 0)              s->decision_grain = 0;
  if(s->decision_grain > 1)              s->decision_grain = 1;

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, SQUALL_LOG_CTX,
      "init: %s -> %s squeeze_win=%u squeeze_pctl=%.1f arm_expiry=%u"
      " break_n=%u chand_atr=%.2f vol_confirm=%.2f regime_gate=%d"
      " max_hold=%u decision_grain=%d",
      strat, mid, s->squeeze_win, s->squeeze_pctl, s->arm_expiry,
      s->break_n, s->chand_atr, s->vol_confirm, s->regime_gate,
      s->max_hold, s->decision_grain);

  return(0);
}

void
wm_strategy_finalize(wm_strategy_ctx_t *ctx)
{
  squall_state_t *s;

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
  squall_state_t *s;

  (void)mkt;   // squall reads only ind[] slots + its own cached state.

  if(bar == NULL || ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s == NULL)
    return;

  // 1d: latch the regime cache. Read even with regime_gate=0 — a
  // reload that flips the gate on inherits a warm cache.
  if(grain == WM_GRAN_1D)
  {
    s->d1_close  = bar->close;
    s->d1_ema50  = (double)bar->ind[WM_IND_EMA_50];
    s->d1_have   = true;
    return;
  }

  // 4h: the coil grain, always. Test against the strictly-prior
  // window, then push. A coil close while flat (re)arms the hunt.
  // When decision_grain=1 the same closed bar then runs the decision
  // core (its own rings are separate and also strictly-prior).
  if(grain == WM_GRAN_4H)
  {
    float natr = bar->ind[WM_IND_NATR_14];

    if(!isnanf(natr))
    {
      if(!s->in_position && squall_is_coiled(s, (double)natr))
      {
        s->armed    = true;
        s->arm_left = s->arm_expiry;
      }

      squall_natr_push(s, (double)natr);
    }

    if(s->decision_grain == 1)
      squall_decide(ctx, s, bar);

    return;
  }

  // 1h: the decision grain unless lifted to 4h.
  if(grain == WM_GRAN_1H && s->decision_grain == 0)
    squall_decide(ctx, s, bar);
}

// ----------------------------------------------------------------------- //
// Plugin lifecycle                                                        //
// ----------------------------------------------------------------------- //

static bool
squall_plugin_init(void)
{
  clam(CLAM_INFO, SQUALL_LOG_CTX, "%s v%s loaded", SQUALL_NAME,
      SQUALL_VERSION);
  return(false);
}

static void
squall_plugin_deinit(void)
{
  clam(CLAM_INFO, SQUALL_LOG_CTX, "%s deinit", SQUALL_NAME);
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "strategy_" SQUALL_NAME,
  .version         = SQUALL_VERSION,
  .type            = PLUGIN_STRATEGY,
  .kind            = SQUALL_NAME,
  .provides        = { { .name = "strategy_" SQUALL_NAME } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_whenmoon" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = squall_plugin_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = squall_plugin_deinit,
  .ext             = NULL,
};
