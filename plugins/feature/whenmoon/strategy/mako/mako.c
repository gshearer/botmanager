// botmanager — MIT
// mako — long-only adaptive regime flipper with hysteresis (competitor #3).
//
// Thesis: on the contest's compounding-neutral monthly-return score, the
// winning shape is a fast binary regime switch — long while the regime
// grain's close is above a fast regime line, flat below it — because every
// banked up-leg re-deploys grown cash (cc2's finding: faster is better,
// monotonically, until fee-death). mako keeps that core and attacks the
// shape's one weakness: in sideways chop the raw flipper crosses its line
// constantly and bleeds ~0.2%/round-trip in fees. Three orthogonal,
// individually-disableable mechanisms target exactly that:
//
//   band_bps   : a HYSTERESIS band around the regime line. Enter only when
//                close > line*(1+band), exit only when close < line*(1-band).
//                Inside the band the current state holds, so chop centered
//                on the line churns nothing. band=0 = raw flipper.
//   line_mode  : 0 = self-EMA of the regime grain's closes, smoothing set
//                directly by `alpha` (cc2's continuous-knob edge).
//                1 = KAMA (Kaufman adaptive MA): smoothing scales with the
//                efficiency ratio over `er_n` bars, so the line hugs price
//                in clean trends (fast flips = compounding) and flattens in
//                chop (fewer whipsaws). er/fast/slow knobs gate it.
//   cmp_mode   : 0 = compare the regime GRAIN's close to the line (state
//                changes only as that grain closes — cc2 classic).
//                1 = compare the CURRENT 1h close to the higher-grain line
//                (reacts intra-bar of the regime grain: exits leg breaks
//                up to 3h earlier, re-enters recoveries earlier).
//
//   entry      : pure regime (long the moment the regime is up while flat —
//                cc2's finding: impulse gates cut well-timed leg starts).
//   exit       : regime down (through the band), or a chandelier backstop
//                (close <= peak_high - chand_atr*ATR_14(1h); 0 = off).
//
// Decision/emit grain is 1h; 4h + 1d feed cached context only. With
// band_bps=0 line_mode=0 cmp_mode=0 alpha=0.82 regime_grain=1 chand_atr=8
// mako reproduces cc2's validated winner bit-for-bit (cross-check).
//
// LOOKAHEAD SAFETY. The backtest fires on_bar in a merged chronological
// walk; on a shared timestamp the FINER grain fires first. Each grain's
// regime line (self-EMA or KAMA, advanced one step per that grain's bar
// close) and close are cached in that grain's own on_bar branch; the 1h
// decision reads only the cache, this 1h bar's own fields/ind[] slots, and
// strategy-owned state tracked forward since entry. The 1h-grain line
// (regime_grain=2) is advanced with the current 1h close and compared to
// that same close — an MA comparison against current+past closes only,
// lookahead-free by construction. The KAMA efficiency ring holds only
// closes already seen at that grain. No reach into mkt->grain_arr[] (which
// in backtest holds the whole — future — date range).

#include "whenmoon_strategy.h"

#include "alloc.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define MAKO_NAME      "mako"
#define MAKO_VERSION   "0.1"
#define MAKO_LOG_CTX   "strategy.mako"

// KAMA efficiency-ratio ring: er_n deltas need er_n+1 closes.
#define MAKO_ER_CAP    64u

// Per-grain live warm-up history (backtest snapshots carry the full range;
// this matters only at live cold start). 1h wants ATR_14 warm; every grain
// wants the KAMA ring full at its largest er_n.
#define MAKO_HIST_1H   128u
#define MAKO_HIST_4H   128u
#define MAKO_HIST_1D   128u

// Defaults — mirrored in the param schema below. These are the cc2-
// equivalent baseline (band/KAMA/cmp innovations off) until sweeps pin the
// contest config; the SCOREBOARD row is the source of truth for that.
#define MAKO_DEFAULT_REGIME_GRAIN   1.0   // 1 = 4h regime line
#define MAKO_DEFAULT_LINE_MODE      0.0   // 0 = self-EMA(alpha)
#define MAKO_DEFAULT_ALPHA          0.82  // self-EMA smoothing (line_mode 0)
#define MAKO_DEFAULT_BAND_BPS       0.0   // hysteresis half-band (0 = off)
#define MAKO_DEFAULT_CMP_MODE       0.0   // 0 = grain close vs line
#define MAKO_DEFAULT_ER_N          10.0   // KAMA efficiency lookback
#define MAKO_DEFAULT_KAMA_FAST_N    2.0   // KAMA fast smoothing period
#define MAKO_DEFAULT_KAMA_SLOW_N   30.0   // KAMA slow smoothing period
#define MAKO_DEFAULT_CHAND_ATR      8.0   // chandelier backstop (0 = off)

// One regime grain's cached context: the close and regime line as of that
// grain's latest closed bar, plus the KAMA close ring (line_mode 1).
typedef struct
{
  double   close;
  double   line;
  bool     have;

  double   ring[MAKO_ER_CAP];
  int      head;
  int      count;
} mako_grain_t;

typedef struct
{
  // Resolved params (snapshot at init; reload to pick up KV changes).
  int      regime_grain;   // 0 = 1d, 1 = 4h, 2 = 1h
  int      line_mode;      // 0 = self-EMA(alpha), 1 = KAMA
  int      cmp_mode;       // 0 = grain close vs line, 1 = 1h close vs line
  double   alpha;          // self-EMA smoothing (line_mode 0)
  double   band;           // hysteresis half-band as a fraction (bps/1e4)
  int      er_n;           // KAMA efficiency lookback (line_mode 1)
  double   kama_fast_sc;   // KAMA fast smoothing constant
  double   kama_slow_sc;   // KAMA slow smoothing constant
  double   chand_atr;      // chandelier give-back in ATR_14(1h) (0 = off)

  mako_grain_t g1d;
  mako_grain_t g4h;
  mako_grain_t g1h;

  // 1h position state.
  bool     in_position;
  double   peak_high;      // highest HIGH since entry (chandelier anchor)
} mako_state_t;

// Advance one grain's regime line with that grain's just-closed bar.
// Self-EMA: line = alpha*close + (1-alpha)*line, seeded on first close.
// KAMA: smoothing scales with |net move| / sum(|bar moves|) over er_n bars,
// squared, between the fast and slow constants (Kaufman's formula).
static void
mako_grain_step(const mako_state_t *s, mako_grain_t *g, double close)
{
  if(!g->have || isnan(g->line))
    g->line = close;

  else if(s->line_mode == 1)
  {
    double sc;
    double er = 0.0;

    // ER over er_n periods ending at the current close: the er_n-th most
    // recent stored close anchors the net move; noise sums the er_n bar-
    // to-bar moves between it and the current close.
    if(g->count >= s->er_n)
    {
      int    start = (g->head - s->er_n + (int)MAKO_ER_CAP * 2)
                       % (int)MAKO_ER_CAP;
      double prev  = g->ring[start];
      double noise = 0.0;
      int    i;

      for(i = 1; i < s->er_n; i++)
      {
        double cur = g->ring[(start + i) % (int)MAKO_ER_CAP];

        noise += fabs(cur - prev);
        prev   = cur;
      }

      noise += fabs(close - prev);

      if(noise > 0.0)
        er = fabs(close - g->ring[start]) / noise;
    }

    sc = er * (s->kama_fast_sc - s->kama_slow_sc) + s->kama_slow_sc;
    sc = sc * sc;

    g->line += sc * (close - g->line);
  }

  else
    g->line = s->alpha * close + (1.0 - s->alpha) * g->line;

  g->ring[g->head] = close;
  g->head          = (g->head + 1) % (int)MAKO_ER_CAP;

  if(g->count < (int)MAKO_ER_CAP)
    g->count++;

  g->close = close;
  g->have  = true;
}

// Regime state with hysteresis. `ref_close` is the price compared against
// the line (the regime grain's own close, or the current 1h close in
// cmp_mode 1). Fails closed on missing/NaN context.
static bool
mako_regime_enter_ok(const mako_state_t *s, const mako_grain_t *g,
    double ref_close)
{
  if(!g->have || isnan(g->line))
    return(false);

  return(ref_close > g->line * (1.0 + s->band));
}

static bool
mako_regime_exit_hit(const mako_state_t *s, const mako_grain_t *g,
    double ref_close)
{
  if(!g->have || isnan(g->line))
    return(true);

  return(ref_close < g->line * (1.0 - s->band));
}

// ----------------------------------------------------------------------- //
// Required exports                                                        //
// ----------------------------------------------------------------------- //

static const wm_strategy_param_t mako_params[] = {
  {
    .name        = "regime_grain",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)MAKO_DEFAULT_REGIME_GRAIN,
    .min_int     = 0,
    .max_int     = 2,
    .step_dbl    = 1.0,
    .help        = "Grain the regime line runs on: 0=1d 1=4h 2=1h."
                   " Default 1.",
  },
  {
    .name        = "line_mode",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)MAKO_DEFAULT_LINE_MODE,
    .min_int     = 0,
    .max_int     = 1,
    .step_dbl    = 1.0,
    .help        = "Regime line: 0=self-EMA of the grain's closes (smoothing"
                   " = alpha), 1=KAMA (efficiency-adaptive: fast in trends,"
                   " flat in chop; er_n/kama_fast_n/kama_slow_n). Default 0.",
  },
  {
    .name        = "alpha",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = MAKO_DEFAULT_ALPHA,
    .min_dbl     = 0.02,
    .max_dbl     = 1.0,
    .step_dbl    = 0.02,
    .help        = "Self-EMA smoothing in (0,1] (line_mode 0). Faster ="
                   " more legs banked, until fee-death. Default 0.82.",
  },
  {
    .name        = "band_bps",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = MAKO_DEFAULT_BAND_BPS,
    .min_dbl     = 0.0,
    .max_dbl     = 300.0,
    .step_dbl    = 5.0,
    .help        = "Hysteresis half-band around the regime line in basis"
                   " points: enter above line*(1+b), exit below line*(1-b),"
                   " hold inside. Cuts chop whipsaw. 0 = raw flipper."
                   " Default 0.",
  },
  {
    .name        = "cmp_mode",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)MAKO_DEFAULT_CMP_MODE,
    .min_int     = 0,
    .max_int     = 1,
    .step_dbl    = 1.0,
    .help        = "Price compared against the regime line: 0=the regime"
                   " grain's own close (state changes on that grain's"
                   " closes), 1=the current 1h close (reacts intra-bar of"
                   " the regime grain). Default 0.",
  },
  {
    .name        = "er_n",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)MAKO_DEFAULT_ER_N,
    .min_int     = 2,
    .max_int     = 60,
    .step_dbl    = 2.0,
    .help        = "KAMA efficiency-ratio lookback in regime-grain bars"
                   " (line_mode 1). Default 10.",
  },
  {
    .name        = "kama_fast_n",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)MAKO_DEFAULT_KAMA_FAST_N,
    .min_int     = 1,
    .max_int     = 30,
    .step_dbl    = 1.0,
    .help        = "KAMA fast smoothing period (line_mode 1): the line's"
                   " speed at efficiency 1. Default 2.",
  },
  {
    .name        = "kama_slow_n",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)MAKO_DEFAULT_KAMA_SLOW_N,
    .min_int     = 5,
    .max_int     = 120,
    .step_dbl    = 5.0,
    .help        = "KAMA slow smoothing period (line_mode 1): the line's"
                   " speed at efficiency 0 (dead chop). Default 30.",
  },
  {
    .name        = "chand_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = MAKO_DEFAULT_CHAND_ATR,
    .min_dbl     = 0.0,
    .max_dbl     = 60.0,
    .step_dbl    = 1.0,
    .help        = "Chandelier trailing-stop backstop in ATR_14(1h) below"
                   " the highest HIGH since entry (0 = off; the regime flip"
                   " leads). Default 8.",
  },
};

void
wm_strategy_describe(wm_strategy_meta_t *out)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->abi_version = WM_STRATEGY_ABI_VERSION;

  snprintf(out->name, sizeof(out->name), "%s", MAKO_NAME);
  snprintf(out->version, sizeof(out->version), "%s", MAKO_VERSION);

  // 1h decides/emits; 4h + 1d feed cached regime context only.
  out->grains_mask = (uint16_t)((1u << WM_GRAN_1H) |
                                (1u << WM_GRAN_4H) |
                                (1u << WM_GRAN_1D));

  out->min_history[WM_GRAN_1H] = MAKO_HIST_1H;
  out->min_history[WM_GRAN_4H] = MAKO_HIST_4H;
  out->min_history[WM_GRAN_1D] = MAKO_HIST_1D;

  out->params               = mako_params;
  out->n_params             = (uint32_t)(sizeof(mako_params)
                                  / sizeof(mako_params[0]));
  out->wants_trade_callback = false;
}

int
wm_strategy_init(wm_strategy_ctx_t *ctx)
{
  mako_state_t *s;
  const char   *mid;
  const char   *strat;
  int           kama_fast_n;
  int           kama_slow_n;

  if(ctx == NULL)
    return(-1);

  s = mem_alloc("strategy." MAKO_NAME, "state", sizeof(*s));

  memset(s, 0, sizeof(*s));

  mid   = wm_strategy_ctx_market_id(ctx);
  strat = wm_strategy_ctx_strategy_name(ctx);

  s->regime_grain = (int)wm_strategy_kv_get_uint(mid, strat, "regime_grain",
      (uint64_t)MAKO_DEFAULT_REGIME_GRAIN);
  s->line_mode = (int)wm_strategy_kv_get_uint(mid, strat, "line_mode",
      (uint64_t)MAKO_DEFAULT_LINE_MODE);
  s->cmp_mode = (int)wm_strategy_kv_get_uint(mid, strat, "cmp_mode",
      (uint64_t)MAKO_DEFAULT_CMP_MODE);
  s->alpha = wm_strategy_kv_get_dbl(mid, strat, "alpha", MAKO_DEFAULT_ALPHA);
  s->band = wm_strategy_kv_get_dbl(mid, strat, "band_bps",
      MAKO_DEFAULT_BAND_BPS) / 10000.0;
  s->er_n = (int)wm_strategy_kv_get_uint(mid, strat, "er_n",
      (uint64_t)MAKO_DEFAULT_ER_N);
  kama_fast_n = (int)wm_strategy_kv_get_uint(mid, strat, "kama_fast_n",
      (uint64_t)MAKO_DEFAULT_KAMA_FAST_N);
  kama_slow_n = (int)wm_strategy_kv_get_uint(mid, strat, "kama_slow_n",
      (uint64_t)MAKO_DEFAULT_KAMA_SLOW_N);
  s->chand_atr = wm_strategy_kv_get_dbl(mid, strat, "chand_atr",
      MAKO_DEFAULT_CHAND_ATR);

  if(s->regime_grain < 0)             s->regime_grain = 0;
  if(s->regime_grain > 2)             s->regime_grain = 2;
  if(s->line_mode < 0)                s->line_mode = 0;
  if(s->line_mode > 1)                s->line_mode = 1;
  if(s->cmp_mode < 0)                 s->cmp_mode = 0;
  if(s->cmp_mode > 1)                 s->cmp_mode = 1;
  if(s->alpha <= 0.0)                 s->alpha = MAKO_DEFAULT_ALPHA;
  if(s->alpha > 1.0)                  s->alpha = 1.0;
  if(s->band < 0.0)                   s->band = 0.0;
  if(s->er_n < 2)                     s->er_n = 2;
  if(s->er_n > (int)MAKO_ER_CAP - 2)  s->er_n = (int)MAKO_ER_CAP - 2;
  if(kama_fast_n < 1)                 kama_fast_n = 1;
  if(kama_slow_n < kama_fast_n)       kama_slow_n = kama_fast_n;
  if(s->chand_atr < 0.0)              s->chand_atr = 0.0;

  s->kama_fast_sc = 2.0 / ((double)kama_fast_n + 1.0);
  s->kama_slow_sc = 2.0 / ((double)kama_slow_n + 1.0);

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, MAKO_LOG_CTX,
      "init: %s -> %s regime_grain=%d line_mode=%d cmp_mode=%d alpha=%.4f"
      " band=%.4f%% er_n=%d kama_sc=%.3f/%.3f chand_atr=%.2f",
      strat, mid, s->regime_grain, s->line_mode, s->cmp_mode, s->alpha,
      s->band * 100.0, s->er_n, s->kama_fast_sc, s->kama_slow_sc,
      s->chand_atr);

  return(0);
}

void
wm_strategy_finalize(wm_strategy_ctx_t *ctx)
{
  mako_state_t *s;

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
  mako_state_t         *s;
  mako_grain_t         *g;
  wm_strategy_signal_t  sig;
  double                ref_close;
  bool                  fire = false;

  (void)mkt;   // mako reads only this bar + its own cached context.

  if(bar == NULL || ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s == NULL)
    return;

  // Higher grains only advance their cached regime line + close.
  if(grain == WM_GRAN_1D)
  {
    mako_grain_step(s, &s->g1d, bar->close);
    return;
  }

  if(grain == WM_GRAN_4H)
  {
    mako_grain_step(s, &s->g4h, bar->close);
    return;
  }

  if(grain != WM_GRAN_1H)
    return;

  // ---- 1h decision grain ----
  mako_grain_step(s, &s->g1h, bar->close);

  g = (s->regime_grain == 0) ? &s->g1d :
      (s->regime_grain == 2) ? &s->g1h : &s->g4h;

  // cmp_mode 1 measures the current 1h close against the higher-grain
  // line; mode 0 (and the 1h regime, where they coincide) measures the
  // regime grain's own latest close.
  ref_close = (s->cmp_mode == 1 || s->regime_grain == 2)
                ? bar->close : g->close;

  memset(&sig, 0, sizeof(sig));
  sig.ts_ms = bar->ts_close_ms;

  if(!s->in_position)
  {
    if(mako_regime_enter_ok(s, g, ref_close))
    {
      sig.score      = 1.0;
      sig.confidence = 0.6;
      snprintf(sig.reason, sizeof(sig.reason),
          "in g%d m%d", s->regime_grain, s->line_mode);

      s->in_position = true;
      s->peak_high   = bar->high;
      fire           = true;
    }
  }

  else
  {
    float  atr       = bar->ind[WM_IND_ATR_14];
    bool   have_atr  = !isnanf(atr) && atr > 0.0f;
    double chand_lv  = -1.0;
    bool   hit_chand;
    bool   hit_regime;

    if(bar->high > s->peak_high)
      s->peak_high = bar->high;

    if(have_atr && s->chand_atr > 0.0)
      chand_lv = s->peak_high - s->chand_atr * (double)atr;

    hit_chand  = (chand_lv > 0.0 && bar->close <= chand_lv);
    hit_regime = mako_regime_exit_hit(s, g, ref_close);

    if(hit_regime || hit_chand)
    {
      sig.score      = -1.0;
      sig.confidence = 0.5;
      snprintf(sig.reason, sizeof(sig.reason),
          "exit %s", hit_regime ? "regime" : "chand");

      s->in_position = false;
      s->peak_high   = 0.0;
      fire           = true;
    }
  }

  if(fire)
    wm_strategy_emit_signal(ctx, &sig);
}

// ----------------------------------------------------------------------- //
// Plugin lifecycle                                                        //
// ----------------------------------------------------------------------- //

static bool
mako_plugin_init(void)
{
  clam(CLAM_INFO, MAKO_LOG_CTX, "%s v%s loaded", MAKO_NAME, MAKO_VERSION);
  return(false);
}

static void
mako_plugin_deinit(void)
{
  clam(CLAM_INFO, MAKO_LOG_CTX, "%s deinit", MAKO_NAME);
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "strategy_" MAKO_NAME,
  .version         = MAKO_VERSION,
  .type            = PLUGIN_STRATEGY,
  .kind            = MAKO_NAME,
  .provides        = { { .name = "strategy_" MAKO_NAME } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_whenmoon" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = mako_plugin_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = mako_plugin_deinit,
  .ext             = NULL,
};
