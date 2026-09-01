// botmanager — MIT
// reef — long-biased daily trend rider that reefs exposure only on a
// confirmed, deep breakdown and re-hoists it quickly.

// The benchmark this strategy is written against is holding the traded
// asset, not a cash return, so every bar spent flat is a deliberate
// short of the asset's right tail. That asymmetry drives the whole
// design: the default state is LONG, leaving is made expensive and slow,
// and returning is made cheap and fast.
//
//   reef (exit) : requires BOTH axes to agree, on the 1d grain —
//                 `confirm_n` consecutive daily closes below the slow
//                 mean SMA(`trend_n`), AND a drawdown of at least
//                 `dd_pct` from the highest close of the trailing
//                 REEF_PEAK_N days. A single close below a mean is
//                 noise; a confirmed streak inside a real drawdown is
//                 structure.
//   hoist (entry): close back above the fast mean SMA(`entry_n`).
//                 `re_mode` 1 additionally demands the close be back
//                 above the slow mean — slower, safer, and it gives up
//                 more of a V-shaped recovery.
//
// Both means come from a strategy-owned ring of daily closes. The
// current bar is pushed BEFORE evaluation and every window is read
// backwards from the newest entry, so a decision can only ever see
// closes at or before its own bar — lookahead-free in live and
// backtest alike. Nothing reads mkt->grain_arr, which in a backtest is
// pre-filled with the entire date range.
//
// The cost model this targets: at real exchange fees a round trip is
// expensive, so the design is deliberately low-frequency — a handful of
// round trips per year, each one paid for by a whole bear leg rather
// than by a 1% swing.

#include "whenmoon_strategy.h"

#include "alloc.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define REEF_NAME       "reef"
#define REEF_VERSION    "0.1"
#define REEF_LOG_CTX    "strategy.reef"

// Ring capacity bounds the slow mean and the trailing-peak window.
#define REEF_RING_CAP        512u
#define REEF_MIN_HISTORY_1D  400u

// Trailing-peak window, in days. Fixed rather than swept: it exists to
// answer "is this a real drawdown", and a year is the horizon the
// drawdown threshold is calibrated in. Leaving it out of the parameter
// schema keeps the search surface small.
#define REEF_PEAK_N          365u

// Defaults sit at the CENTRE of the measured plateau, not at its peak.
// On BTC-USD daily bars to 2023-12-31 at 60 bps + 5 bps a side, the
// whole region trend_n 150-300 x confirm_n 15-20 x entry_n 5-20 scores
// within a 0.14 band, so no single cell in it is distinguishable from
// its neighbours and the centre is the honest pick. dd_pct defaults to
// 0 because it was measured INERT at trend_n >= 200 — the confirmation
// streak always binds first — and mildly harmful below that.
#define REEF_DEFAULT_TREND_N     200
#define REEF_DEFAULT_CONFIRM_N   20
#define REEF_DEFAULT_DD_PCT      0.0
#define REEF_DEFAULT_ENTRY_N     10
#define REEF_DEFAULT_RE_MODE     1

typedef struct
{
  uint32_t trend_n;        // slow mean, days
  uint32_t confirm_n;      // consecutive sub-mean closes required to exit
  double   dd_pct;         // min drawdown from trailing peak to permit exit
  uint32_t entry_n;        // fast mean, days
  int      re_mode;        // 0 = fast mean only, 1 = also above slow mean

  double   closes[REEF_RING_CAP];
  uint32_t head;           // next write slot
  uint32_t count;          // saturates at REEF_RING_CAP

  bool     in_position;
  uint32_t below_streak;   // consecutive closes under the slow mean
} reef_state_t;

// ----------------------------------------------------------------------- //
// Rolling-window helpers                                                  //
// ----------------------------------------------------------------------- //

static void
reef_ring_push(reef_state_t *s, double close)
{
  s->closes[s->head] = close;
  s->head            = (s->head + 1) % REEF_RING_CAP;

  if(s->count < REEF_RING_CAP)
    s->count++;
}

// Mean of the most recent `n` stored closes. NAN when fewer than `n`
// bars have been seen — the caller treats that as "not warm yet".
static double
reef_window_mean(const reef_state_t *s, uint32_t n)
{
  double   sum = 0.0;
  uint32_t i;

  if(n == 0 || s->count < n)
    return(NAN);

  for(i = 0; i < n; i++)
  {
    uint32_t idx = (s->head + REEF_RING_CAP - 1 - i) % REEF_RING_CAP;

    sum += s->closes[idx];
  }

  return(sum / (double)n);
}

// Highest close over the most recent min(count, n) stored closes.
// Unlike the mean this degrades rather than refusing: a short history
// yields a shallow peak, which only ever makes the exit gate harder to
// pass, so it can never manufacture a trade.
static double
reef_window_peak(const reef_state_t *s, uint32_t n)
{
  double   hi = -INFINITY;
  uint32_t lim;
  uint32_t i;

  lim = (s->count < n) ? s->count : n;

  if(lim == 0)
    return(NAN);

  for(i = 0; i < lim; i++)
  {
    uint32_t idx = (s->head + REEF_RING_CAP - 1 - i) % REEF_RING_CAP;

    if(s->closes[idx] > hi)
      hi = s->closes[idx];
  }

  return(hi);
}

// ----------------------------------------------------------------------- //
// Required exports                                                        //
// ----------------------------------------------------------------------- //

static const wm_strategy_param_t reef_params[] = {
  {
    .name        = "trend_n",
    .type        = WM_PARAM_UINT,
    .default_int = REEF_DEFAULT_TREND_N,
    .min_int     = 20,
    .max_int     = 400,
    .help        = "Slow mean period in daily bars. Closing below it is"
                   " the first of the two conditions that can reef"
                   " exposure. Default 200.",
  },
  {
    .name        = "confirm_n",
    .type        = WM_PARAM_UINT,
    .default_int = REEF_DEFAULT_CONFIRM_N,
    .min_int     = 1,
    .max_int     = 30,
    .help        = "Consecutive daily closes below the slow mean before"
                   " an exit is allowed. Raising it trades reaction"
                   " speed for whipsaw immunity. Default 20.",
  },
  {
    .name        = "dd_pct",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = REEF_DEFAULT_DD_PCT,
    .min_dbl     = 0.0,
    .max_dbl     = 0.60,
    .step_dbl    = 0.05,
    .help        = "Minimum drawdown from the trailing 365-day high"
                   " before an exit is allowed, as a fraction. 0"
                   " disables the second gate. Default 0 —"
                   " measured inert at trend_n >= 200.",
  },
  {
    .name        = "entry_n",
    .type        = WM_PARAM_UINT,
    .default_int = REEF_DEFAULT_ENTRY_N,
    .min_int     = 2,
    .max_int     = 120,
    .help        = "Fast mean period in daily bars; closing above it"
                   " re-hoists full exposure. Default 10.",
  },
  {
    .name        = "re_mode",
    .type        = WM_PARAM_UINT,
    .default_int = REEF_DEFAULT_RE_MODE,
    .min_int     = 0,
    .max_int     = 1,
    .help        = "Re-entry gate: 0 = close above the fast mean alone,"
                   " 1 = also require the close above the slow mean."
                   " Default 1: mode 0 was measured worse at every one"
                   " of 12 paired points, because after an exit it"
                   " re-enters inside the same downtrend and churns.",
  },
};

void
wm_strategy_describe(wm_strategy_meta_t *out)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->abi_version = WM_STRATEGY_ABI_VERSION;

  snprintf(out->name, sizeof(out->name), "%s", REEF_NAME);
  snprintf(out->version, sizeof(out->version), "%s", REEF_VERSION);

  out->grains_mask             = (uint16_t)(1u << WM_GRAN_1D);
  out->min_history[WM_GRAN_1D] = REEF_MIN_HISTORY_1D;

  out->params               = reef_params;
  out->n_params             = (uint32_t)(sizeof(reef_params)
                                  / sizeof(reef_params[0]));
  out->wants_trade_callback = false;
}

int
wm_strategy_init(wm_strategy_ctx_t *ctx)
{
  reef_state_t *s;
  const char   *mid;
  const char   *strat;

  if(ctx == NULL)
    return(-1);

  s = mem_alloc("strategy." REEF_NAME, "state", sizeof(*s));

  mid   = wm_strategy_ctx_market_id(ctx);
  strat = wm_strategy_ctx_strategy_name(ctx);

  s->trend_n = (uint32_t)wm_strategy_kv_get_int(mid, strat, "trend_n",
      REEF_DEFAULT_TREND_N);
  s->confirm_n = (uint32_t)wm_strategy_kv_get_int(mid, strat, "confirm_n",
      REEF_DEFAULT_CONFIRM_N);
  s->dd_pct =
      wm_strategy_kv_get_dbl(mid, strat, "dd_pct", REEF_DEFAULT_DD_PCT);
  s->entry_n = (uint32_t)wm_strategy_kv_get_int(mid, strat, "entry_n",
      REEF_DEFAULT_ENTRY_N);
  s->re_mode = (int)wm_strategy_kv_get_int(mid, strat, "re_mode",
      REEF_DEFAULT_RE_MODE);

  if(s->trend_n > REEF_RING_CAP) s->trend_n = REEF_RING_CAP;
  if(s->entry_n > REEF_RING_CAP) s->entry_n = REEF_RING_CAP;
  if(s->trend_n == 0) s->trend_n = 1;
  if(s->entry_n == 0) s->entry_n = 1;
  if(s->confirm_n == 0) s->confirm_n = 1;

  s->head         = 0;
  s->count        = 0;
  s->in_position  = false;
  s->below_streak = 0;

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, REEF_LOG_CTX,
      "init: %s -> %s trend_n=%u confirm_n=%u dd_pct=%.3f entry_n=%u"
      " re_mode=%d (1d)",
      strat, mid, s->trend_n, s->confirm_n, s->dd_pct, s->entry_n,
      s->re_mode);

  return(0);
}

void
wm_strategy_finalize(wm_strategy_ctx_t *ctx)
{
  reef_state_t *s;

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
  reef_state_t         *s;
  wm_strategy_signal_t  sig;
  double                slow;
  double                fast;
  double                peak;
  double                dd;
  bool                  fire = false;

  (void)mkt;   // reef reads only this bar's close and its own ring.

  if(grain != WM_GRAN_1D || bar == NULL || ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s == NULL)
    return;

  // Push first, then read backwards: every window below ends at this
  // bar's own close and can never reach a later one.
  reef_ring_push(s, bar->close);

  slow = reef_window_mean(s, s->trend_n);

  // The streak counts closes below the slow mean since the last time
  // this strategy acted, so it only advances once that mean is warm.
  // Until then exits are impossible and the fast mean carries entry on
  // its own — idling flat through the slow mean's warmup would be a
  // deliberate short of the asset for no signal at all.
  if(!isnan(slow))
  {
    if(bar->close < slow)
      s->below_streak++;

    else
      s->below_streak = 0;
  }

  memset(&sig, 0, sizeof(sig));
  sig.ts_ms = bar->ts_close_ms;

  if(s->in_position)
  {
    peak = reef_window_peak(s, REEF_PEAK_N);
    dd   = (isnan(peak) || peak <= 0.0) ? 0.0 : 1.0 - (bar->close / peak);

    if(!isnan(slow) && s->below_streak >= s->confirm_n && dd >= s->dd_pct)
    {
      sig.score      = -1.0;
      sig.confidence = 0.6;
      snprintf(sig.reason, sizeof(sig.reason),
          "reef d%u dd%.1f%%", s->below_streak, dd * 100.0);

      s->in_position = false;
      fire           = true;
    }
  }

  else
  {
    fast = reef_window_mean(s, s->entry_n);

    if(!isnan(fast) && bar->close > fast &&
       (s->re_mode == 0 || isnan(slow) || bar->close > slow))
    {
      sig.score      = 1.0;
      sig.confidence = 0.6;
      snprintf(sig.reason, sizeof(sig.reason),
          "hoist>ma%u m%d", s->entry_n, s->re_mode);

      s->in_position  = true;
      // Acting clears the evidence: leaving again must be earned by a
      // fresh run of `confirm_n` closes, not by the run that is still
      // in progress from before this entry.
      s->below_streak = 0;
      fire            = true;
    }
  }

  if(fire)
  {
    wm_strategy_emit_signal(ctx, &sig);

    clam(CLAM_INFO, REEF_LOG_CTX,
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
reef_plugin_init(void)
{
  clam(CLAM_INFO, REEF_LOG_CTX, "%s v%s loaded", REEF_NAME, REEF_VERSION);
  return(false);
}

static void
reef_plugin_deinit(void)
{
  wm_strategy_detach_self(REEF_NAME);

  clam(CLAM_INFO, REEF_LOG_CTX, "%s deinit", REEF_NAME);
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "strategy_" REEF_NAME,
  .version         = REEF_VERSION,
  .type            = PLUGIN_STRATEGY,
  .kind            = REEF_NAME,
  .provides        = { { .name = "strategy_" REEF_NAME } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_whenmoon" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = reef_plugin_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = reef_plugin_deinit,
  .ext             = NULL,
};
