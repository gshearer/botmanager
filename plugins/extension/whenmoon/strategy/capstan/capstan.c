// botmanager — MIT
// capstan — tide-gated ATR-rung profit-ratchet harvester (1h decision grain).
//
// The Round-0 baseline taught two things: this window pays continuous
// trend PARTICIPATION (the regime-flippers lead the board), and the
// competition's hard constraint is ACTIVITY — ≥5 round trips in every
// calendar month on a long-only book, which condition-gated entries
// (dips, squeezes, impulse crosses) structurally fail in chop months.
// capstan is built for exactly that geometry: like a capstan winch it
// hauls the trend in and pawls each gain so it cannot slip back.
//
//   tide (permission) : longs allowed only while the tide-grain close is
//                       above its tide MA (tide_grain: 1h self-tide or a
//                       cached 4h tide; tide_ma: EMA_20 / EMA_50 / SMA_50).
//                       A tide flip-down is an unconditional exit. The
//                       default 1h EMA_50 tide is deliberately FAST — it
//                       re-arms during bear-market rallies, which is where
//                       the every-month activity gate is actually won.
//   entry (1h)        : flat + tide up ⇒ buy. A LEVEL, not a cross — after
//                       a banked rung the next bar re-enters while the
//                       tide holds. No dip / squeeze / impulse
//                       precondition; participation is the point. v0.2
//                       adds an optional TIDE-OFF re-entry discipline
//                       (the bear-year leak is rebuying a fading rally
//                       at the same or higher price): `tideoff_rearm`
//                       cools entries after a tide-off exit, and
//                       `reentry_disc` demands the re-entry close print
//                       BELOW the exit close for its window, then waives
//                       so a fresh leg is not missed forever. Both
//                       default 0 = exact v0.1 behavior; banked rungs
//                       never gate (immediate re-entry is the thesis).
//   ratchet exit (1h) : close ≥ entry + rung_atr × ATR_14(1h @ entry) ⇒
//                       bank the rung (sell), immediately eligible to
//                       re-enter. Entry-anchored and vol-normalized: each
//                       rung monetizes ~rung_atr ATRs of trend progress,
//                       so round trips scale with how far the trend runs,
//                       not with how often a regime line flips.
//   protective exit   : close ≤ entry − stop_atr × ATR_14(1h @ entry) ⇒
//                       cut, then wait rearm_bars 1h bars before the next
//                       entry (stop-outs only; banked rungs never cool
//                       down). Optional min_natr floor (NATR_14 %) skips
//                       entries in dead-vol tape where a rung cannot pay
//                       for its friction.
//   pawl (v0.3)       : once close reaches entry + pawl_atr × ATR the
//                       stop rises to entry — an unbanked run-up can no
//                       longer become a deep tide-off give-back, only a
//                       friction-priced scratch (exit reason "pawl", no
//                       cooldown: a scratch is not a loss, and the level
//                       entry re-arms next bar while the tide holds).
//                       0 = off (bit-identical to the unpawled ratchet).
//
// Distinctness in the sidequest1 field: the round-trip generator is an
// ENTRY-ANCHORED PROFIT TARGET inside a trend gate. cc1/cc2 trade only
// regime-line crosses; surf enters on impulse crosses and exits on
// fade/chandelier; mako/riptide need a dip and target EMA-anchored
// reversion; squall needs a squeeze; juggernaut gates on ADX strength;
// cp1/cp2 are breakout/pullback riders. Nobody slices trend progress
// into fixed vol-normalized rungs.
//
// LOOKAHEAD SAFETY. The backtest fires on_bar in a merged chronological
// walk; on a shared timestamp the finer grain fires first (1h before
// 4h). capstan caches the 4h close + MA as that 4h bar closes and reads
// the cache only on later 1h decision bars; the 1h decision reads only
// this bar's own ind[] slots plus strategy-owned entry anchors tracked
// forward from entry. No reach into mkt->grain_arr[] (in backtest that
// ring holds the whole — future — date range). Identical live/backtest.

#include "whenmoon_strategy.h"

#include "alloc.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CAPSTAN_NAME      "capstan"
#define CAPSTAN_VERSION   "0.3"
#define CAPSTAN_LOG_CTX   "strategy.capstan"

// Per-grain live warm-up history (bars). Backtest snapshots carry the
// full range regardless; this matters at live cold start. 1h wants
// EMA_50 + ATR_14 warm with margin; 4h wants its tide MA warm.
#define CAPSTAN_HIST_1H   210u
#define CAPSTAN_HIST_4H   256u

// Defaults — mirrored in the param schema below.
#define CAPSTAN_DEFAULT_TIDE_GRAIN   0u    // 0 = 1h self-tide (fast)
#define CAPSTAN_DEFAULT_TIDE_MA      1u    // 1 = EMA_50 on the tide grain
#define CAPSTAN_DEFAULT_RUNG_ATR     2.0   // bank at entry + 2 ATR
#define CAPSTAN_DEFAULT_STOP_ATR     2.0   // cut at entry - 2 ATR
#define CAPSTAN_DEFAULT_REARM_BARS   0u    // no post-stop cooldown
#define CAPSTAN_DEFAULT_MIN_NATR     0.0   // no dead-vol floor
#define CAPSTAN_DEFAULT_TIDEOFF_RB   0u    // no post-tide-off cooldown
#define CAPSTAN_DEFAULT_REENTRY_DISC 0u    // no below-exit re-entry gate
#define CAPSTAN_DEFAULT_PAWL_ATR     0.0   // no breakeven pawl

typedef struct
{
  // Resolved params (snapshot at init; reload to pick up KV changes).
  int      tide_grain;     // 0 = 1h self-tide, 1 = 4h cached tide
  int      tide_slot;      // resolved WM_IND_* for the tide MA
  double   rung_atr;       // profit rung in entry-ATR multiples
  double   stop_atr;       // protective stop in entry-ATR multiples
  uint32_t rearm_bars;     // 1h-bar cooldown after a stop-out
  double   min_natr;       // NATR_14(1h) %-floor for entries (0 = off)
  uint32_t tideoff_rearm;  // 1h-bar cooldown after a tide-off exit
  uint32_t reentry_disc;   // below-exit re-entry window after tide-off
  double   pawl_atr;       // breakeven-pawl arm level, entry-ATR mult

  // Cached 4h tide context (tide_grain=1). Latches once a 4h bar
  // closes; values are that bar's close + tide MA (NaN-guarded on read).
  double   h4_close;
  double   h4_ma;
  bool     h4_have;

  // 1h position state. Anchors are frozen at entry so rung and stop
  // never drift as volatility changes mid-hold.
  bool     in_position;
  bool     pawled;         // breakeven pawl armed for this hold
  double   entry_price;
  double   entry_atr;
  uint32_t cooldown;       // 1h bars left before entries re-arm

  // Tide-off re-entry discipline (v0.2). Armed only by a tide-off
  // exit when reentry_disc > 0: while gate_left counts down, a new
  // long needs a close BELOW gate_price; expiry waives the gate.
  double   gate_price;
  uint32_t gate_left;
} capstan_state_t;

// Map the tide_ma ordinal onto an indicator slot (same slot id on
// either tide grain; the value is computed on that grain's bars).
static int
capstan_tide_slot(uint32_t tide_ma)
{
  switch(tide_ma)
  {
    case 0:  return(WM_IND_EMA_20);
    case 2:  return(WM_IND_SMA_50);
    case 1:
    default: return(WM_IND_EMA_50);
  }
}

// Tide up? tide_grain=0 reads this 1h bar's own MA slot; tide_grain=1
// reads the cached 4h close/MA. Fails closed on missing context or NaN.
static bool
capstan_tide_up(const capstan_state_t *s, const wm_candle_full_t *bar)
{
  if(s->tide_grain == 1)
    return(s->h4_have && !isnan(s->h4_ma) && s->h4_close > s->h4_ma);

  {
    float ma = bar->ind[s->tide_slot];

    return(!isnanf(ma) && bar->close > (double)ma);
  }
}

// ----------------------------------------------------------------------- //
// Required exports                                                        //
// ----------------------------------------------------------------------- //

static const wm_strategy_param_t capstan_params[] = {
  {
    .name        = "tide_grain",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CAPSTAN_DEFAULT_TIDE_GRAIN,
    .min_int     = 0,
    .max_int     = 1,
    .step_dbl    = 1.0,
    .help        = "Grain the permission tide runs on: 0=1h self-tide"
                   " (fast — re-arms inside bear rallies), 1=4h cached"
                   " tide (slower, fewer chop entries). Default 0.",
  },
  {
    .name        = "tide_ma",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CAPSTAN_DEFAULT_TIDE_MA,
    .min_int     = 0,
    .max_int     = 2,
    .step_dbl    = 1.0,
    .help        = "Tide moving average on the tide grain (close above ="
                   " longs allowed, below = forced exit): 0=EMA_20,"
                   " 1=EMA_50, 2=SMA_50. Default 1.",
  },
  {
    .name        = "rung_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = CAPSTAN_DEFAULT_RUNG_ATR,
    .min_dbl     = 0.5,
    .max_dbl     = 8.0,
    .step_dbl    = 0.25,
    .help        = "Profit rung: bank the long when close reaches entry +"
                   " rung_atr * ATR_14(1h at entry), then re-enter while"
                   " the tide holds. Smaller = more round trips, more"
                   " friction. Default 2.",
  },
  {
    .name        = "stop_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = CAPSTAN_DEFAULT_STOP_ATR,
    .min_dbl     = 0.5,
    .max_dbl     = 8.0,
    .step_dbl    = 0.25,
    .help        = "Protective stop: cut the long when close falls to"
                   " entry - stop_atr * ATR_14(1h at entry). Default 2.",
  },
  {
    .name        = "rearm_bars",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CAPSTAN_DEFAULT_REARM_BARS,
    .min_int     = 0,
    .max_int     = 48,
    .step_dbl    = 4.0,
    .help        = "1h bars to wait after a STOP-OUT before entries"
                   " re-arm (banked rungs never cool down). 0 = re-enter"
                   " immediately. Default 0.",
  },
  {
    .name        = "min_natr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = CAPSTAN_DEFAULT_MIN_NATR,
    .min_dbl     = 0.0,
    .max_dbl     = 5.0,
    .step_dbl    = 0.25,
    .help        = "Minimum NATR_14(1h) percent required to open a new"
                   " long — skips dead-vol tape where a rung cannot pay"
                   " its friction. 0 = off. Default 0.",
  },
  {
    .name        = "tideoff_rearm",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CAPSTAN_DEFAULT_TIDEOFF_RB,
    .min_int     = 0,
    .max_int     = 48,
    .step_dbl    = 4.0,
    .help        = "1h bars to wait after a TIDE-OFF exit before"
                   " entries re-arm (stacks ahead of reentry_disc;"
                   " banked rungs never cool down). 0 = re-enter"
                   " immediately. Default 0.",
  },
  {
    .name        = "reentry_disc",
    .type        = WM_PARAM_UINT,
    .default_int = (int64_t)CAPSTAN_DEFAULT_REENTRY_DISC,
    .min_int     = 0,
    .max_int     = 48,
    .step_dbl    = 4.0,
    .help        = "Re-entry discipline window after a tide-off exit:"
                   " for this many 1h bars a new long needs a close"
                   " BELOW the exit close (never rebuy a fading rally"
                   " higher), then the gate waives. 0 = off. Default 0.",
  },
  {
    .name        = "pawl_atr",
    .type        = WM_PARAM_DOUBLE,
    .default_dbl = CAPSTAN_DEFAULT_PAWL_ATR,
    .min_dbl     = 0.0,
    .max_dbl     = 8.0,
    .step_dbl    = 0.25,
    .help        = "Breakeven pawl: once close reaches entry +"
                   " pawl_atr * ATR(entry), the stop rises to entry —"
                   " an unbanked run-up can only scratch, never become"
                   " a deep give-back. No cooldown after a pawl exit."
                   " 0 = off. Default 0.",
  },
};

void
wm_strategy_describe(wm_strategy_meta_t *out)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->abi_version = WM_STRATEGY_ABI_VERSION;

  snprintf(out->name, sizeof(out->name), "%s", CAPSTAN_NAME);
  snprintf(out->version, sizeof(out->version), "%s", CAPSTAN_VERSION);

  // 1h decides/emits; 4h feeds the cached tide only.
  out->grains_mask = (uint16_t)((1u << WM_GRAN_1H) | (1u << WM_GRAN_4H));

  out->min_history[WM_GRAN_1H] = CAPSTAN_HIST_1H;
  out->min_history[WM_GRAN_4H] = CAPSTAN_HIST_4H;

  out->params               = capstan_params;
  out->n_params             = (uint32_t)(sizeof(capstan_params)
                                  / sizeof(capstan_params[0]));
  out->wants_trade_callback = false;
}

int
wm_strategy_init(wm_strategy_ctx_t *ctx)
{
  capstan_state_t *s;
  const char      *mid;
  const char      *strat;
  uint32_t         tide_ma;

  if(ctx == NULL)
    return(-1);

  s = mem_alloc("strategy." CAPSTAN_NAME, "state", sizeof(*s));

  memset(s, 0, sizeof(*s));

  mid   = wm_strategy_ctx_market_id(ctx);
  strat = wm_strategy_ctx_strategy_name(ctx);

  s->tide_grain = (int)wm_strategy_kv_get_uint(mid, strat, "tide_grain",
      (uint64_t)CAPSTAN_DEFAULT_TIDE_GRAIN);
  tide_ma = (uint32_t)wm_strategy_kv_get_uint(mid, strat, "tide_ma",
      (uint64_t)CAPSTAN_DEFAULT_TIDE_MA);
  s->rung_atr = wm_strategy_kv_get_dbl(mid, strat, "rung_atr",
      CAPSTAN_DEFAULT_RUNG_ATR);
  s->stop_atr = wm_strategy_kv_get_dbl(mid, strat, "stop_atr",
      CAPSTAN_DEFAULT_STOP_ATR);
  s->rearm_bars = (uint32_t)wm_strategy_kv_get_uint(mid, strat,
      "rearm_bars", (uint64_t)CAPSTAN_DEFAULT_REARM_BARS);
  s->min_natr = wm_strategy_kv_get_dbl(mid, strat, "min_natr",
      CAPSTAN_DEFAULT_MIN_NATR);
  s->tideoff_rearm = (uint32_t)wm_strategy_kv_get_uint(mid, strat,
      "tideoff_rearm", (uint64_t)CAPSTAN_DEFAULT_TIDEOFF_RB);
  s->reentry_disc = (uint32_t)wm_strategy_kv_get_uint(mid, strat,
      "reentry_disc", (uint64_t)CAPSTAN_DEFAULT_REENTRY_DISC);
  s->pawl_atr = wm_strategy_kv_get_dbl(mid, strat, "pawl_atr",
      CAPSTAN_DEFAULT_PAWL_ATR);

  // Defensive clamps — hand-edited KV values may sit outside the
  // declared ranges; bound them so the mechanics stay well-defined.
  if(s->tide_grain < 0)     s->tide_grain = 0;
  if(s->tide_grain > 1)     s->tide_grain = 1;
  if(tide_ma > 2)           tide_ma = 2;
  if(s->rung_atr < 0.1)     s->rung_atr = 0.1;
  if(s->stop_atr < 0.1)     s->stop_atr = 0.1;
  if(s->rearm_bars > 1000)  s->rearm_bars = 1000;
  if(s->min_natr < 0.0)     s->min_natr = 0.0;
  if(s->tideoff_rearm > 1000) s->tideoff_rearm = 1000;
  if(s->reentry_disc > 1000)  s->reentry_disc = 1000;
  if(s->pawl_atr < 0.0)       s->pawl_atr = 0.0;

  s->tide_slot = capstan_tide_slot(tide_ma);

  wm_strategy_ctx_set_user(ctx, s);

  clam(CLAM_INFO, CAPSTAN_LOG_CTX,
      "init: %s -> %s tide_grain=%d tide_slot=%d rung_atr=%.2f"
      " stop_atr=%.2f rearm_bars=%u min_natr=%.2f tideoff_rearm=%u"
      " reentry_disc=%u pawl_atr=%.2f (1h)",
      strat, mid, s->tide_grain, s->tide_slot, s->rung_atr,
      s->stop_atr, s->rearm_bars, s->min_natr, s->tideoff_rearm,
      s->reentry_disc, s->pawl_atr);

  return(0);
}

void
wm_strategy_finalize(wm_strategy_ctx_t *ctx)
{
  capstan_state_t *s;

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
  capstan_state_t      *s;
  wm_strategy_signal_t  sig;
  bool                  tide;
  bool                  fire = false;

  (void)mkt;   // capstan reads only ind[] slots + its own anchors.

  if(bar == NULL || ctx == NULL)
    return;

  s = wm_strategy_ctx_get_user(ctx);

  if(s == NULL)
    return;

  // The 4h grain only refreshes the cached tide context.
  if(grain != WM_GRAN_1H)
  {
    if(grain == WM_GRAN_4H)
    {
      s->h4_close = bar->close;
      s->h4_ma    = bar->ind[s->tide_slot];
      s->h4_have  = true;
    }

    return;
  }

  // ---- 1h decision grain ----
  tide = capstan_tide_up(s, bar);

  memset(&sig, 0, sizeof(sig));
  sig.ts_ms = bar->ts_close_ms;

  if(!s->in_position)
  {
    bool gate_blocks = false;

    if(s->cooldown > 0)
    {
      s->cooldown--;
      return;
    }

    // Tide-off re-entry discipline: while armed, only a close below
    // the exit print may re-enter; the window ticks per flat 1h bar
    // (after any cooldown) and waives itself at zero.
    if(s->gate_left > 0)
    {
      gate_blocks = bar->close >= s->gate_price;
      s->gate_left--;
    }

    if(tide && !gate_blocks)
    {
      float atr  = bar->ind[WM_IND_ATR_14];
      float natr = bar->ind[WM_IND_NATR_14];

      // Entry needs a live ATR anchor; the optional NATR floor skips
      // dead-vol tape. Both fail closed.
      if(isnanf(atr) || atr <= 0.0f)
        return;

      if(s->min_natr > 0.0 && (isnanf(natr) || (double)natr < s->min_natr))
        return;

      sig.score      = 1.0;
      sig.confidence = 0.6;
      snprintf(sig.reason, sizeof(sig.reason),
          "in t%d", s->tide_grain);

      s->in_position = true;
      s->pawled      = false;
      s->entry_price = bar->close;
      s->entry_atr   = (double)atr;
      s->gate_left   = 0;
      fire           = true;
    }
  }

  else
  {
    double rung_lv = s->entry_price + s->rung_atr * s->entry_atr;
    double stop_lv = s->pawled ? s->entry_price
                   : s->entry_price - s->stop_atr * s->entry_atr;

    if(bar->close >= rung_lv)
    {
      sig.score      = -1.0;
      sig.confidence = 0.6;
      snprintf(sig.reason, sizeof(sig.reason),
          "rung +%.2fatr", s->rung_atr);

      s->in_position = false;
      fire           = true;
    }

    else if(bar->close <= stop_lv)
    {
      // A pawled hold scratches at breakeven ("pawl", no cooldown);
      // an unpawled one is a true stop-out and cools down.
      sig.score      = -1.0;
      sig.confidence = 0.5;
      snprintf(sig.reason, sizeof(sig.reason), "%s",
          s->pawled ? "pawl" : "stop");

      s->in_position = false;

      if(!s->pawled)
        s->cooldown = s->rearm_bars;

      fire = true;
    }

    else if(!tide)
    {
      sig.score      = -1.0;
      sig.confidence = 0.5;
      snprintf(sig.reason, sizeof(sig.reason), "tide off");

      s->in_position = false;
      s->cooldown    = s->tideoff_rearm;

      if(s->reentry_disc > 0)
      {
        s->gate_price = bar->close;
        s->gate_left  = s->reentry_disc;
      }

      fire = true;
    }

    // Still holding: arm the breakeven pawl once the run-up reaches
    // its level. Arming and the breakeven trigger cannot share a bar
    // (arm needs close >= entry + pawl·ATR, trigger needs <= entry).
    else if(s->pawl_atr > 0.0 && !s->pawled
        && bar->close >= s->entry_price + s->pawl_atr * s->entry_atr)
      s->pawled = true;
  }

  if(fire)
  {
    wm_strategy_emit_signal(ctx, &sig);

    clam(CLAM_DEBUG3, CAPSTAN_LOG_CTX,
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
capstan_plugin_init(void)
{
  clam(CLAM_INFO, CAPSTAN_LOG_CTX,
      "%s v%s loaded", CAPSTAN_NAME, CAPSTAN_VERSION);
  return(false);
}

static void
capstan_plugin_deinit(void)
{
  clam(CLAM_INFO, CAPSTAN_LOG_CTX, "%s deinit", CAPSTAN_NAME);
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "strategy_" CAPSTAN_NAME,
  .version         = CAPSTAN_VERSION,
  .type            = PLUGIN_STRATEGY,
  .kind            = CAPSTAN_NAME,
  .provides        = { { .name = "strategy_" CAPSTAN_NAME } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_whenmoon" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = capstan_plugin_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = capstan_plugin_deinit,
  .ext             = NULL,
};
