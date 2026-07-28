// botmanager — MIT
// kedge: sidequest2 round-1 contestant — the donchian robust corner
// with adaptive exits.
//
// The base is the one cell sidequest1 left standing after ~210 census
// cells and three contestant rounds (temp/sidequest1/CLOSEOUT.md): a
// long-only donchian breakout on the 1h grain, entered when the close
// exceeds the highest high of the prior `entry_n` bars, exited by a
// fixed `hold`-bar clock. Re-measured engine-true on sidequest2's
// extended training set it clears five of RULES §4's six gates and
// fails exactly one — minimum-year cadence, at 22-25 round trips
// against a floor of 24.
//
// The refinement is the lever sidequest1 recorded and never pulled:
// adaptive exits. A protective stop and a profit target, both scaled
// by ATR(14) at the entry bar, race the clock; whichever comes first
// closes the position. Stops ADD round trips by construction — a
// stop-out is an exit, and the next breakout is then free to be taken
// — so the lever is aimed precisely at the gate the base misses,
// while the stop's other job is truncating the losing tail that sets
// the worst calendar year.
//
// Null hypothesis inside the instrument: at `stop_atr=0 targ_atr=0`
// both adaptive exits are disabled and kedge is bit-identical to the
// unrefined base (the committed sextant probe, `family=1`, at the
// same entry_n/hold). That equivalence is regression-proven before
// any refinement is swept — a refinement whose null case does not
// reproduce the base is a broken instrument, not a result.
//
// Levels are anchored to the entry bar's CLOSE, which is what the
// strategy knows when it decides; the engine fills the advice at the
// next 1m open (`--fill next-open`). Breaches are evaluated against
// the closed bar's own high/low — both known at that close, so the
// rule is lookahead-free — and the resulting exit fills at the next
// open rather than at the level itself. This is an hourly stop
// DISCIPLINE, not a resting stop order, and it is modelled as such.

#include "whenmoon_strategy.h"

#include "alloc.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define KDG_NAME     "kedge"
#define KDG_VERSION  "0.1"
#define KDG_LOG_CTX  "strategy.kedge"

#define KDG_RING_CAP 512u

// Per-attachment state. Params snapshot at init; the high ring only
// ever holds bars strictly before the one being evaluated. A level of
// 0.0 means "not armed" — either the axis is off or ATR was unusable
// at entry.
typedef struct
{
  uint32_t entry_n;
  uint32_t hold;
  double   stop_atr;
  double   targ_atr;

  bool     in_pos;
  uint32_t held;
  double   stop_px;
  double   targ_px;

  double   ring[KDG_RING_CAP];
  uint32_t ring_n;
  uint32_t ring_head;
} kdg_state_t;

static const wm_strategy_param_t kdg_params[] = {
  {
    .name        = "entry_n",
    .type        = WM_PARAM_UINT,
    .default_int = 168,
    .min_int     = 1,
    .max_int     = 500,
    .step_dbl    = 1.0,
    .help        = "Donchian entry lookback in 1h bars: enter when the"
                   " close exceeds the highest high of the prior N"
                   " bars.",
  },
  {
    .name        = "hold",
    .type        = WM_PARAM_UINT,
    .default_int = 72,
    .min_int     = 1,
    .max_int     = 1024,
    .step_dbl    = 1.0,
    .help        = "Time exit: close the position this many 1h bars"
                   " after the entry signal. The backstop the adaptive"
                   " exits race.",
  },
  {
    .name        = "stop_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = 0.0,
    .min_dbl     = 0.0,
    .max_dbl     = 20.0,
    .step_dbl    = 0.5,
    .help        = "Protective stop, in ATR(14) multiples below the"
                   " entry bar's close. 0 disables it (the unrefined"
                   " base).",
  },
  {
    .name        = "targ_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = 0.0,
    .min_dbl     = 0.0,
    .max_dbl     = 40.0,
    .step_dbl    = 0.5,
    .help        = "Profit target, in ATR(14) multiples above the"
                   " entry bar's close. 0 disables it (the unrefined"
                   " base).",
  },
};

// -----------------------------------------------------------------------
// Entry trigger — values strictly up to and including this bar's close;
// the ring is pushed after evaluation, never before (the cp1 pattern)
// -----------------------------------------------------------------------

static bool
kdg_break(const kdg_state_t *s, const wm_candle_full_t *bar)
{
  double   hi = 0.0;
  uint32_t i;

  if(s->ring_n < s->entry_n)
    return(false);

  for(i = 0; i < s->entry_n; i++)
  {
    uint32_t at = (s->ring_head + KDG_RING_CAP - 1 - i) % KDG_RING_CAP;

    if(s->ring[at] > hi)
      hi = s->ring[at];
  }

  return(bar->close > hi);
}

static void
kdg_ring_push(kdg_state_t *s, double high)
{
  s->ring[s->ring_head] = high;
  s->ring_head          = (s->ring_head + 1) % KDG_RING_CAP;

  if(s->ring_n < KDG_RING_CAP)
    s->ring_n++;
}

// -----------------------------------------------------------------------
// Adaptive exits
// -----------------------------------------------------------------------

// Arm the stop/target off the entry bar's close and its ATR(14). An
// axis set to 0, a NaN ATR (still warming) or a non-positive ATR all
// leave that level unarmed for the trade — the clock still governs.
static void
kdg_arm(kdg_state_t *s, const wm_candle_full_t *bar)
{
  double atr = (double)bar->ind[WM_IND_ATR_14];

  s->stop_px = 0.0;
  s->targ_px = 0.0;

  if(isnan(atr) || atr <= 0.0)
    return;

  if(s->stop_atr > 0.0)
    s->stop_px = bar->close - (s->stop_atr * atr);

  if(s->targ_atr > 0.0)
    s->targ_px = bar->close + (s->targ_atr * atr);

  if(s->stop_px < 0.0)
    s->stop_px = 0.0;
}

// Which exit fires on this closed bar, or NULL to stay in. A bar that
// breaches both levels is scored as a stop: the adverse excursion is
// assumed to have come first, which is the conservative reading and
// the one that cannot flatter the refinement.
static const char *
kdg_exit_reason(const kdg_state_t *s, const wm_candle_full_t *bar)
{
  if(s->stop_px > 0.0 && bar->low <= s->stop_px)
    return("stop");

  if(s->targ_px > 0.0 && bar->high >= s->targ_px)
    return("target");

  if(s->held >= s->hold)
    return("time");

  return(NULL);
}

// -----------------------------------------------------------------------
// Required exports
// -----------------------------------------------------------------------

void
wm_strategy_describe(wm_strategy_meta_t *out)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->abi_version = WM_STRATEGY_ABI_VERSION;

  snprintf(out->name, sizeof(out->name), "%s", KDG_NAME);
  snprintf(out->version, sizeof(out->version), "%s", KDG_VERSION);

  out->grains_mask             = (uint16_t)(1u << WM_GRAN_1H);
  out->min_history[WM_GRAN_1H] = 200;

  out->params               = kdg_params;
  out->n_params             = (uint32_t)(sizeof(kdg_params)
                                  / sizeof(kdg_params[0]));
  out->wants_trade_callback = false;
}

int
wm_strategy_init(wm_strategy_ctx_t *ctx)
{
  kdg_state_t *s;
  const char  *mid;
  const char  *strat;

  if(ctx == NULL)
    return(-1);

  s = mem_alloc("strategy." KDG_NAME, "state", sizeof(*s));
  memset(s, 0, sizeof(*s));

  mid   = wm_strategy_ctx_market_id(ctx);
  strat = wm_strategy_ctx_strategy_name(ctx);

  s->entry_n  = (uint32_t)wm_strategy_kv_get_uint(mid, strat,
                    "entry_n", 168);
  s->hold     = (uint32_t)wm_strategy_kv_get_uint(mid, strat, "hold", 72);
  s->stop_atr = wm_strategy_kv_get_dbl(mid, strat, "stop_atr", 0.0);
  s->targ_atr = wm_strategy_kv_get_dbl(mid, strat, "targ_atr", 0.0);

  if(s->entry_n < 1)
    s->entry_n = 1;

  if(s->entry_n > KDG_RING_CAP - 1)
    s->entry_n = KDG_RING_CAP - 1;

  if(s->hold < 1)
    s->hold = 1;

  if(s->stop_atr < 0.0)
    s->stop_atr = 0.0;

  if(s->targ_atr < 0.0)
    s->targ_atr = 0.0;

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, KDG_LOG_CTX,
      "init: %s -> %s entry_n=%u hold=%u stop_atr=%.2f targ_atr=%.2f",
      strat, mid, s->entry_n, s->hold, s->stop_atr, s->targ_atr);

  return(0);
}

void
wm_strategy_finalize(wm_strategy_ctx_t *ctx)
{
  kdg_state_t *s;

  if(ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s != NULL)
  {
    mem_free(s);
    wm_strategy_ctx_set_user(ctx, NULL);
  }

  clam(CLAM_INFO, KDG_LOG_CTX,
      "finalize: %s -> %s",
      wm_strategy_ctx_strategy_name(ctx),
      wm_strategy_ctx_market_id(ctx));
}

void
wm_strategy_on_bar(wm_strategy_ctx_t *ctx,
    const struct whenmoon_market *mkt,
    wm_gran_t grain,
    const wm_candle_full_t *bar)
{
  kdg_state_t          *s;
  wm_strategy_signal_t  sig;

  (void)mkt;

  if(grain != WM_GRAN_1H || bar == NULL || ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s == NULL)
    return;

  memset(&sig, 0, sizeof(sig));
  sig.ts_ms      = bar->ts_close_ms;
  sig.confidence = 0.5;

  if(s->in_pos)
  {
    const char *why;

    s->held++;
    why = kdg_exit_reason(s, bar);

    if(why != NULL)
    {
      sig.score = -1.0;
      snprintf(sig.reason, sizeof(sig.reason), "%s exit %u bars",
          why, s->held);
      wm_strategy_emit_signal(ctx, &sig);

      s->in_pos  = false;
      s->held    = 0;
      s->stop_px = 0.0;
      s->targ_px = 0.0;
    }
  }

  else if(kdg_break(s, bar))
  {
    sig.score = 1.0;
    snprintf(sig.reason, sizeof(sig.reason), "break n%u", s->entry_n);
    wm_strategy_emit_signal(ctx, &sig);

    s->in_pos = true;
    s->held   = 0;
    kdg_arm(s, bar);
  }

  kdg_ring_push(s, bar->high);
}

// -----------------------------------------------------------------------
// Plugin lifecycle
// -----------------------------------------------------------------------

static bool
kdg_plugin_init(void)
{
  clam(CLAM_INFO, KDG_LOG_CTX, "%s v%s loaded", KDG_NAME, KDG_VERSION);
  return(false);
}

static void
kdg_plugin_deinit(void)
{
  clam(CLAM_INFO, KDG_LOG_CTX, "%s deinit", KDG_NAME);
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "strategy_" KDG_NAME,
  .version         = KDG_VERSION,
  .type            = PLUGIN_STRATEGY,
  .kind            = KDG_NAME,
  .provides        = { { .name = "strategy_" KDG_NAME } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_whenmoon" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = kdg_plugin_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = kdg_plugin_deinit,
  .ext             = NULL,
};
