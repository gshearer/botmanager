// botmanager — MIT
// gale — long-only channel breakout with a volatility-scaled risk bracket.

// Written against a CASH benchmark: an idle book earns nothing, so being
// flat costs nothing and there is no penalty for sitting out. That makes
// selectivity free, and the whole design spends it on one question —
// where is the DOWNSIDE bounded — rather than on finding more entries.
//
// The shape:
//
//   entry  : a 1h close above the highest high of the previous `entry_n`
//            1h bars — a genuine new high for the channel, not a poke at
//            it.
//   risk   : the stop sits `stop_atr` multiples of the entry bar's
//            ATR(14) below the entry reference. Expressing it in units
//            of realised volatility rather than in percent is what keeps
//            the geometry constant across eras: a fixed 3% stop is a
//            half-day move in a wild market and a two-week move in a
//            quiet one, so a percentage stop silently re-tunes itself
//            every year while an ATR one does not.
//   target : an optional ceiling `targ_rr` risk-multiples above entry,
//            and it is NET — the climb must clear `cost_bps` of round
//            trip friction before it counts. A gross target inverts as
//            fees rise; a net one degrades gracefully and stays portable
//            across venues and fee tiers.
//   regime : breakouts are taken only while the last CLOSED daily bar is
//            above its own `regime_n`-day mean. Without it the same
//            entry bleeds through every range: a breakout inside a
//            downtrend is the one that pays the stop.
//
// There is deliberately no trailing stop and no time stop. Both were
// measured and neither carried its keep: a chandelier trail at the stop
// distance cut winners without improving the drawdown, and a time stop
// anywhere from five days to never changed the result by less than the
// spread between neighbouring parameter cells. Every knob costs evidence,
// so a knob that does not change the answer is a knob that should not
// ship.
//
// Everything the strategy reads is either this bar's own indicator block
// or its own ring of past highs, which is pushed AFTER the decision is
// taken, so a decision can only ever see bars at or before its own.
// Nothing touches mkt->grain_arr — in a backtest that ring is pre-filled
// with the entire date range, and reading it is a look at the future.
//
// Cost model: a round trip is expensive relative to the moves on offer,
// so the design is deliberately low-frequency — a couple of dozen round
// trips a year, each one sized to be paid for by a multi-day leg rather
// than by an intraday wiggle.

#include "whenmoon_strategy.h"

#include "alloc.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define GALE_NAME     "gale"
#define GALE_VERSION  "0.1"
#define GALE_LOG_CTX  "strategy.gale"

// Hourly-high ring for the breakout channel; 768 bars is 32 days, past
// anything the search found useful, and `entry_n` is bounded by it.
#define GALE_RING_CAP        768u
#define GALE_MAX_ENTRY_N     (GALE_RING_CAP)
#define GALE_MIN_HISTORY_1H  (GALE_RING_CAP + 32u)

// Daily-close ring for the regime mean, and the bound on `regime_n`.
#define GALE_DAILY_CAP       400u
#define GALE_MAX_REGIME_N    (GALE_DAILY_CAP)
#define GALE_MIN_HISTORY_1D  (GALE_DAILY_CAP + 8u)

// Defaults are the CENTRE of the measured plateau, not its peak: on the
// grid entry_n 192..720 x stop_atr 1.75..3.0 x targ_rr 3..8, this cell has
// the highest fraction of admissible neighbours of any cell on the grid.
#define GALE_DEFAULT_ENTRY_N    480
#define GALE_DEFAULT_STOP_ATR   2.25
#define GALE_DEFAULT_TARG_RR    4.0
#define GALE_DEFAULT_REGIME_N   50
#define GALE_DEFAULT_COST_BPS   60.0

typedef struct
{
  uint32_t entry_n;
  double   stop_atr;
  double   targ_rr;
  uint32_t regime_n;
  double   cost_bps;

  double   highs[GALE_RING_CAP];      // 1h highs, newest at head-1
  uint32_t head;
  uint32_t count;

  double   closes[GALE_DAILY_CAP];    // 1d closes, newest at dhead-1
  uint32_t dhead;
  uint32_t dcount;
  bool     regime_ok;                 // recomputed as each daily bar closes

  bool     in_position;
  double   entry_ref;                 // the 1h close the bracket was built on
  double   stop_px;                   // hard floor, fixed at entry
  double   targ_px;                   // 0 when no ceiling is configured
  int64_t  entry_ms;
} gale_state_t;

// ----------------------------------------------------------------------- //
// Rolling window over the strategy's own ring of 1h highs                 //
// ----------------------------------------------------------------------- //

static void
gale_ring_push(gale_state_t *s, double high)
{
  s->highs[s->head] = high;
  s->head           = (s->head + 1u) % GALE_RING_CAP;

  if(s->count < GALE_RING_CAP)
    s->count++;
}

// Highest of the `n` most recent stored highs. NAN while the ring holds
// fewer than `n` bars, which the caller reads as "not warm" — never as a
// permissive answer.
static double
gale_window_high(const gale_state_t *s, uint32_t n)
{
  double   hi = -INFINITY;
  uint32_t i;

  if(n == 0u || s->count < n)
    return(NAN);

  for(i = 0u; i < n; i++)
  {
    uint32_t idx = (s->head + GALE_RING_CAP - 1u - i) % GALE_RING_CAP;

    if(s->highs[idx] > hi)
      hi = s->highs[idx];
  }

  return(hi);
}

// ----------------------------------------------------------------------- //
// Signal helpers                                                          //
// ----------------------------------------------------------------------- //

static void
gale_daily_push(gale_state_t *s, double close)
{
  s->closes[s->dhead] = close;
  s->dhead            = (s->dhead + 1u) % GALE_DAILY_CAP;

  if(s->dcount < GALE_DAILY_CAP)
    s->dcount++;
}

// Mean of the most recent `n` stored daily closes; NAN until warm.
static double
gale_daily_mean(const gale_state_t *s, uint32_t n)
{
  double   sum = 0.0;
  uint32_t i;

  if(n == 0u || s->dcount < n)
    return(NAN);

  for(i = 0u; i < n; i++)
  {
    uint32_t idx = (s->dhead + GALE_DAILY_CAP - 1u - i) % GALE_DAILY_CAP;

    sum += s->closes[idx];
  }

  return(sum / (double)n);
}

static void
gale_arm_position(gale_state_t *s, const wm_candle_full_t *bar, double atr)
{
  double risk = s->stop_atr * atr;

  s->in_position = true;
  s->entry_ref   = bar->close;
  s->entry_ms    = bar->ts_close_ms;
  s->stop_px     = bar->close - risk;

  // The ceiling is NET: the climb has to cover a whole round trip of
  // friction before the configured reward multiple starts counting. A
  // gross ceiling silently inverts as fees rise; this one does not.
  s->targ_px = (s->targ_rr > 0.0)
      ? bar->close * (1.0 + s->cost_bps / 10000.0) + s->targ_rr * risk
      : 0.0;
}

// ----------------------------------------------------------------------- //
// Required exports                                                        //
// ----------------------------------------------------------------------- //

static const wm_strategy_param_t gale_params[] = {
  {
    .name        = "entry_n",
    .type        = WM_PARAM_UINT,
    .default_int = GALE_DEFAULT_ENTRY_N,
    .min_int     = 12,
    .max_int     = GALE_MAX_ENTRY_N,
    .help        = "Breakout channel length in 1h bars. A close above the"
                   " highest high of the previous entry_n bars arms a"
                   " long. Default 480 (20 days).",
  },
  {
    .name        = "stop_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = GALE_DEFAULT_STOP_ATR,
    .min_dbl     = 0.25,
    .max_dbl     = 20.0,
    .step_dbl    = 0.25,
    .help        = "Hard stop distance below the entry reference, in"
                   " multiples of the entry bar's 1h ATR(14). This is the"
                   " one unit of risk every other distance is quoted in."
                   " Default 2.25.",
  },
  {
    .name        = "targ_rr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = GALE_DEFAULT_TARG_RR,
    .min_dbl     = 0.0,
    .max_dbl     = 40.0,
    .step_dbl    = 0.5,
    .help        = "Net profit ceiling in risk multiples: the trade closes"
                   " once the climb clears cost_bps of round-trip friction"
                   " AND targ_rr stop distances. 0 disables the ceiling."
                   " Default 4.0.",
  },
  {
    .name        = "regime_n",
    .type        = WM_PARAM_UINT,
    .default_int = GALE_DEFAULT_REGIME_N,
    .min_int     = 0,
    .max_int     = GALE_MAX_REGIME_N,
    .help        = "Stand-down filter: breakouts are taken only while the"
                   " last CLOSED daily bar is above the mean of its own"
                   " last regime_n daily closes. 0 takes every breakout."
                   " Default 50.",
  },
  {
    .name        = "cost_bps",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = GALE_DEFAULT_COST_BPS,
    .min_dbl     = 0.0,
    .max_dbl     = 500.0,
    .step_dbl    = 5.0,
    .help        = "Round-trip friction the net ceiling must clear, in"
                   " basis points — a venue fact, not a tuning knob. Set"
                   " it to your own fee tier plus slippage, both sides."
                   " Default 60.",
  },
};

void
wm_strategy_describe(wm_strategy_meta_t *out)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->abi_version = WM_STRATEGY_ABI_VERSION;

  snprintf(out->name, sizeof(out->name), "%s", GALE_NAME);
  snprintf(out->version, sizeof(out->version), "%s", GALE_VERSION);

  // 1h decides, 1m manages the open bracket, 1d carries the regime.
  out->grains_mask = (uint16_t)((1u << WM_GRAN_1M) | (1u << WM_GRAN_1H)
      | (1u << WM_GRAN_1D));

  out->min_history[WM_GRAN_1H] = GALE_MIN_HISTORY_1H;
  out->min_history[WM_GRAN_1D] = GALE_MIN_HISTORY_1D;

  out->params               = gale_params;
  out->n_params             = (uint32_t)(sizeof(gale_params)
                                  / sizeof(gale_params[0]));
  out->wants_trade_callback = false;
}

int
wm_strategy_init(wm_strategy_ctx_t *ctx)
{
  gale_state_t *s;
  const char   *mid;
  const char   *strat;

  if(ctx == NULL)
    return(-1);

  s = mem_alloc("strategy." GALE_NAME, "state", sizeof(*s));
  memset(s, 0, sizeof(*s));

  mid   = wm_strategy_ctx_market_id(ctx);
  strat = wm_strategy_ctx_strategy_name(ctx);

  s->entry_n = (uint32_t)wm_strategy_kv_get_int(mid, strat, "entry_n",
      GALE_DEFAULT_ENTRY_N);
  s->stop_atr = wm_strategy_kv_get_dbl(mid, strat, "stop_atr",
      GALE_DEFAULT_STOP_ATR);
  s->targ_rr = wm_strategy_kv_get_dbl(mid, strat, "targ_rr",
      GALE_DEFAULT_TARG_RR);
  s->regime_n = (uint32_t)wm_strategy_kv_get_int(mid, strat, "regime_n",
      GALE_DEFAULT_REGIME_N);
  s->cost_bps = wm_strategy_kv_get_dbl(mid, strat, "cost_bps",
      GALE_DEFAULT_COST_BPS);

  if(s->entry_n < 2u)                s->entry_n = 2u;
  if(s->entry_n > GALE_MAX_ENTRY_N)  s->entry_n = GALE_MAX_ENTRY_N;
  if(s->stop_atr <= 0.0)             s->stop_atr = GALE_DEFAULT_STOP_ATR;
  if(s->targ_rr < 0.0)               s->targ_rr = 0.0;
  if(s->cost_bps < 0.0)              s->cost_bps = 0.0;
  if(s->regime_n > GALE_MAX_REGIME_N) s->regime_n = GALE_MAX_REGIME_N;

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, GALE_LOG_CTX,
      "init: %s -> %s entry_n=%u stop_atr=%.2f targ_rr=%.2f regime_n=%u"
      " cost_bps=%.1f",
      strat, mid, s->entry_n, s->stop_atr, s->targ_rr, s->regime_n,
      s->cost_bps);

  return(0);
}

void
wm_strategy_finalize(wm_strategy_ctx_t *ctx)
{
  gale_state_t *s;

  if(ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s != NULL)
  {
    mem_free(s);
    wm_strategy_ctx_set_user(ctx, NULL);
  }
}

// ----------------------------------------------------------------------- //
// Per-grain handlers                                                      //
// ----------------------------------------------------------------------- //

// The regime is recomputed as each daily bar CLOSES and read by the hourly
// decision that follows it, so an hourly bar can only ever see a daily bar
// that closed strictly in its past.
static void
gale_on_daily(gale_state_t *s, const wm_candle_full_t *bar)
{
  double mean;

  gale_daily_push(s, bar->close);

  if(s->regime_n == 0u)
  {
    s->regime_ok = true;
    return;
  }

  mean = gale_daily_mean(s, s->regime_n);

  s->regime_ok = (!isnan(mean) && bar->close > mean);
}

// Returns true when a signal was written to `sig`.
static bool
gale_on_hourly(gale_state_t *s, const wm_candle_full_t *bar,
    wm_strategy_signal_t *sig)
{
  double chan_hi;
  double atr;
  bool   fire = false;

  chan_hi = gale_window_high(s, s->entry_n);
  atr     = (double)bar->ind[WM_IND_ATR_14];

  if(!s->in_position && !isnan(chan_hi) && !isnan(atr) && atr > 0.0
     && bar->close > chan_hi && (s->regime_n == 0u || s->regime_ok))
  {
    gale_arm_position(s, bar, atr);

    sig->score      = 1.0;
    sig->confidence = 0.6;
    snprintf(sig->reason, sizeof(sig->reason), "brk%u atr%.2f",
        s->entry_n, atr / bar->close * 100.0);

    fire = true;
  }

  // Pushed after the decision: this bar can never be part of the channel
  // it had to clear.
  gale_ring_push(s, bar->high);

  return(fire);
}

static bool
gale_on_minute(gale_state_t *s, const wm_candle_full_t *bar,
    wm_strategy_signal_t *sig)
{
  const char *why = NULL;

  if(!s->in_position)
    return(false);

  if(bar->close <= s->stop_px)
    why = "stop";

  else if(s->targ_px > 0.0 && bar->close >= s->targ_px)
    why = "target";

  if(why == NULL)
    return(false);

  s->in_position = false;

  sig->score      = -1.0;
  sig->confidence = 0.6;
  snprintf(sig->reason, sizeof(sig->reason), "%s %+.2f%%", why,
      (bar->close / s->entry_ref - 1.0) * 100.0);

  return(true);
}

void
wm_strategy_on_bar(wm_strategy_ctx_t *ctx,
    const struct whenmoon_market *mkt,
    wm_gran_t grain,
    const wm_candle_full_t *bar)
{
  gale_state_t         *s;
  wm_strategy_signal_t  sig;
  bool                  fire = false;

  (void)mkt;   // gale reads only this bar and its own ring.

  if(bar == NULL || ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s == NULL)
    return;

  memset(&sig, 0, sizeof(sig));
  sig.ts_ms = bar->ts_close_ms;

  switch(grain)
  {
    case WM_GRAN_1D:
      gale_on_daily(s, bar);
      break;

    case WM_GRAN_1H:
      fire = gale_on_hourly(s, bar, &sig);
      break;

    case WM_GRAN_1M:
      fire = gale_on_minute(s, bar, &sig);
      break;

    default:
      break;
  }

  if(fire)
  {
    wm_strategy_emit_signal(ctx, &sig);

    clam(CLAM_DEBUG, GALE_LOG_CTX,
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
gale_plugin_init(void)
{
  clam(CLAM_INFO, GALE_LOG_CTX, "%s v%s loaded", GALE_NAME, GALE_VERSION);
  return(false);
}

static void
gale_plugin_deinit(void)
{
  wm_strategy_detach_self(GALE_NAME);

  clam(CLAM_INFO, GALE_LOG_CTX, "%s deinit", GALE_NAME);
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "strategy_" GALE_NAME,
  .version         = GALE_VERSION,
  .type            = PLUGIN_STRATEGY,
  .kind            = GALE_NAME,
  .provides        = { { .name = "strategy_" GALE_NAME } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_whenmoon" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = gale_plugin_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = gale_plugin_deinit,
  .ext             = NULL,
};
