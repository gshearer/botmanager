// botmanager — MIT
// sextant: sidequest1 round-3 census probe. Engine-true confirmation
// instrument for the round's trigger census (temp/sidequest1/
// strategies/sextant/NOTES.md): two indicator-free trigger families
// behind a selector axis, fixed time-horizon exits, no stops, no
// regime state. The point is that a family's census numbers (capture
// per trade, calendar-year cadence) must reproduce through the real
// fill path before they are believed. The selector axis is a probe
// affordance (RULES §2: probe and strategy are the same plugin,
// evolved); it is frozen to a single family before any official run.
//
// family=0  time-of-day: enter when the 1h bar closing at UTC hour
//           `p1` closes; the next-open fill lands on hour-p1's first
//           minute. Unconditional — cadence by construction.
// family=1  donchian break: enter when the 1h close exceeds the max
//           high of the prior `p1` bars (self-maintained ring, pushed
//           after evaluation — the cp1 lookahead-safe pattern).
//
// Exit for both: `hold` 1h bars after the entry signal, counted so
// the exit fill lands exactly `hold` bars after the entry fill —
// matching the census sim's occupancy arithmetic bar for bar.

#include "whenmoon_strategy.h"

#include "alloc.h"

#include <stdio.h>
#include <string.h>

#define SXT_NAME     "sextant"
#define SXT_VERSION  "0.1"
#define SXT_LOG_CTX  "strategy.sextant"

#define SXT_RING_CAP 512u

typedef enum
{
  SXT_FAMILY_TIME_OF_DAY = 0,
  SXT_FAMILY_DONCHIAN    = 1
} sxt_family_t;

// Per-attachment state. Params snapshot at init; the high ring only
// ever holds bars strictly before the one being evaluated.
typedef struct
{
  uint32_t family;
  uint32_t p1;             // family 0: UTC hour; family 1: lookback bars
  uint32_t hold;           // time exit, 1h bars after the entry signal

  bool     in_pos;
  uint32_t held;

  double   ring[SXT_RING_CAP];
  uint32_t ring_n;
  uint32_t ring_head;
} sxt_state_t;

static const wm_strategy_param_t sxt_params[] = {
  {
    .name        = "family",
    .type        = WM_PARAM_UINT,
    .default_int = 0,
    .min_int     = 0,
    .max_int     = 1,
    .step_dbl    = 1.0,
    .help        = "Trigger family: 0 = time-of-day, 1 = donchian"
                   " breakout. Census probe selector; frozen before"
                   " official runs.",
  },
  {
    .name        = "p1",
    .type        = WM_PARAM_UINT,
    .default_int = 21,
    .min_int     = 0,
    .max_int     = 500,
    .step_dbl    = 1.0,
    .help        = "family 0: UTC hour of the entry fill (0..23)."
                   " family 1: donchian lookback in 1h bars (1..500).",
  },
  {
    .name        = "hold",
    .type        = WM_PARAM_UINT,
    .default_int = 48,
    .min_int     = 1,
    .max_int     = 1024,
    .step_dbl    = 1.0,
    .help        = "Time exit: close the position this many 1h bars"
                   " after the entry signal.",
  },
};

// -----------------------------------------------------------------------
// Trigger evaluation (values strictly up to and including this bar's
// close; the ring is pushed after evaluation, never before)
// -----------------------------------------------------------------------

static bool
sxt_trigger(const sxt_state_t *s, const wm_candle_full_t *bar)
{
  if(s->family == SXT_FAMILY_TIME_OF_DAY)
  {
    uint32_t hour = (uint32_t)((bar->ts_close_ms / 3600000LL) % 24LL);

    return(hour == s->p1);
  }

  if(s->ring_n < s->p1)
    return(false);

  {
    double   hi = 0.0;
    uint32_t i;

    for(i = 0; i < s->p1; i++)
    {
      uint32_t at = (s->ring_head + SXT_RING_CAP - 1 - i) % SXT_RING_CAP;

      if(s->ring[at] > hi)
        hi = s->ring[at];
    }

    return(bar->close > hi);
  }
}

static void
sxt_ring_push(sxt_state_t *s, double high)
{
  s->ring[s->ring_head] = high;
  s->ring_head          = (s->ring_head + 1) % SXT_RING_CAP;

  if(s->ring_n < SXT_RING_CAP)
    s->ring_n++;
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

  snprintf(out->name, sizeof(out->name), "%s", SXT_NAME);
  snprintf(out->version, sizeof(out->version), "%s", SXT_VERSION);

  out->grains_mask             = (uint16_t)(1u << WM_GRAN_1H);
  out->min_history[WM_GRAN_1H] = 200;

  out->params               = sxt_params;
  out->n_params             = (uint32_t)(sizeof(sxt_params)
                                  / sizeof(sxt_params[0]));
  out->wants_trade_callback = false;
}

int
wm_strategy_init(wm_strategy_ctx_t *ctx)
{
  sxt_state_t *s;
  const char  *mid;
  const char  *strat;

  if(ctx == NULL)
    return(-1);

  s = mem_alloc("strategy." SXT_NAME, "state", sizeof(*s));
  memset(s, 0, sizeof(*s));

  mid   = wm_strategy_ctx_market_id(ctx);
  strat = wm_strategy_ctx_strategy_name(ctx);

  s->family = (uint32_t)wm_strategy_kv_get_uint(mid, strat, "family", 0);
  s->p1     = (uint32_t)wm_strategy_kv_get_uint(mid, strat, "p1", 21);
  s->hold   = (uint32_t)wm_strategy_kv_get_uint(mid, strat, "hold", 48);

  if(s->family > SXT_FAMILY_DONCHIAN)
    s->family = SXT_FAMILY_TIME_OF_DAY;

  if(s->family == SXT_FAMILY_TIME_OF_DAY)
  {
    if(s->p1 > 23)
      s->p1 %= 24;
  }

  else
  {
    if(s->p1 < 1)
      s->p1 = 1;

    if(s->p1 > SXT_RING_CAP - 1)
      s->p1 = SXT_RING_CAP - 1;
  }

  if(s->hold < 1)
    s->hold = 1;

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, SXT_LOG_CTX,
      "init: %s -> %s family=%u p1=%u hold=%u",
      strat, mid, s->family, s->p1, s->hold);

  return(0);
}

void
wm_strategy_finalize(wm_strategy_ctx_t *ctx)
{
  sxt_state_t *s;

  if(ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s != NULL)
  {
    mem_free(s);
    wm_strategy_ctx_set_user(ctx, NULL);
  }

  clam(CLAM_INFO, SXT_LOG_CTX,
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
  sxt_state_t          *s;
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
    s->held++;

    if(s->held >= s->hold)
    {
      sig.score = -1.0;
      snprintf(sig.reason, sizeof(sig.reason),
          "time exit f%u %u bars", s->family, s->held);
      wm_strategy_emit_signal(ctx, &sig);

      s->in_pos = false;
      s->held   = 0;
    }
  }

  else if(sxt_trigger(s, bar))
  {
    sig.score = 1.0;

    if(s->family == SXT_FAMILY_TIME_OF_DAY)
      snprintf(sig.reason, sizeof(sig.reason), "tod h%02u", s->p1);
    else
      snprintf(sig.reason, sizeof(sig.reason), "break n%u", s->p1);

    wm_strategy_emit_signal(ctx, &sig);

    s->in_pos = true;
    s->held   = 0;
  }

  if(s->family == SXT_FAMILY_DONCHIAN)
    sxt_ring_push(s, bar->high);
}

// -----------------------------------------------------------------------
// Plugin lifecycle
// -----------------------------------------------------------------------

static bool
sxt_plugin_init(void)
{
  clam(CLAM_INFO, SXT_LOG_CTX, "%s v%s loaded", SXT_NAME, SXT_VERSION);
  return(false);
}

static void
sxt_plugin_deinit(void)
{
  wm_strategy_detach_self(SXT_NAME);

  clam(CLAM_INFO, SXT_LOG_CTX, "%s deinit", SXT_NAME);
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "strategy_" SXT_NAME,
  .version         = SXT_VERSION,
  .type            = PLUGIN_STRATEGY,
  .kind            = SXT_NAME,
  .provides        = { { .name = "strategy_" SXT_NAME } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_whenmoon" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = sxt_plugin_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = sxt_plugin_deinit,
  .ext             = NULL,
};
