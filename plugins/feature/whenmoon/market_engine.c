// botmanager — MIT
// market_engine.c — WM-MK-2 market position engine.
//
// State machine: ONE position per market (long-only this chunk),
// TWO stat ledgers (paper + real) tracked independently, per-mode
// fills rings, pending-order ring, lazy-loaded risk/economic params
// per market. Replaces the per-(market, strategy) trade-book model
// from WM-LT-4 — but only when WM-MK-3 swaps consumers onto these
// entry points; in WM-MK-2 the only caller is the selftest verb.

#define WHENMOON_INTERNAL
#include "market_engine.h"
#include "live.h"
#include "market.h"
#include "market_persist.h"
#include "whenmoon.h"
#include "whenmoon_strategy.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "kv.h"

#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// ------------------------------------------------------------------ //
// KV path helpers                                                    //
// ------------------------------------------------------------------ //
//
// Per-market KV slots live under `plugin.whenmoon.market.<id>.*`.
// Lazy-registered on first read so the operator can `/set kv …`
// without an explicit registration table; mirrors the WM-LT-8-B2
// pattern in live.c (`caaefea`).

#define WM_MK_KV_BUF_SZ   (KV_KEY_SZ + 64)

static double
wm_mk_kv_get_double(const char *market_id_str, const char *suffix,
    const char *def_str, double def_val, const char *help)
{
  char path[WM_MK_KV_BUF_SZ];

  snprintf(path, sizeof(path),
      "plugin.whenmoon.market.%s.%s", market_id_str, suffix);

  if(!kv_exists(path))
  {
    if(kv_register(path, KV_DOUBLE, def_str, NULL, NULL, help) != SUCCESS)
      return(def_val);
  }

  return(kv_get_double(path));
}

static uint64_t
wm_mk_kv_get_uint(const char *market_id_str, const char *suffix,
    const char *def_str, uint64_t def_val, const char *help)
{
  char path[WM_MK_KV_BUF_SZ];

  snprintf(path, sizeof(path),
      "plugin.whenmoon.market.%s.%s", market_id_str, suffix);

  if(!kv_exists(path))
  {
    if(kv_register(path, KV_UINT32, def_str, NULL, NULL, help) != SUCCESS)
      return(def_val);
  }

  return(kv_get_uint(path));
}

// Caller MUST NOT hold mk->lock — KV reads can lazy-register, and
// the function takes mk->lock internally to apply cached values
// atomically. Writers serialise on mk->lock so concurrent
// `apply_fill_locked` callers see a consistent param set.
void
wm_market_session_refresh_kv(whenmoon_market_t *mk)
{
  uint64_t pending_cap;
  uint32_t i;
  double   starting_cash;
  double   fee_bps;
  double   slip_bps;
  double   size_frac;
  double   max_notional;
  double   daily_loss_bps;

  if(mk == NULL)
    return;

  // Read every KV slot off-lock — kv_register/kv_exists are
  // serialised by the KV subsystem internally.
  starting_cash = wm_mk_kv_get_double(mk->market_id_str, "starting_cash",
      "10000.0", WM_MARKET_DEFAULT_STARTING_CASH,
      "Per-market starting cash (quote currency). Each mode's stats"
      " ledger uses this as the cash anchor; reset restores cash to"
      " this value.");

  fee_bps = wm_mk_kv_get_double(mk->market_id_str, "fee_bps",
      "5.0", WM_MARKET_DEFAULT_FEE_BPS,
      "Per-side fee in basis points of executed notional. Applied to"
      " every paper fill; real fills carry the actual exchange fee.");

  slip_bps = wm_mk_kv_get_double(mk->market_id_str, "slip_bps",
      "5.0", WM_MARKET_DEFAULT_SLIP_BPS,
      "Per-side synthetic slippage in basis points of mark, applied"
      " to paper fills only. Real fills use the actual execution px.");

  size_frac = wm_mk_kv_get_double(mk->market_id_str, "size_frac",
      "0.25", WM_MARKET_DEFAULT_SIZE_FRAC,
      "Default sizer fraction. Strategy advisors may override; force-"
      "trades take an explicit qty and ignore this knob.");

  max_notional = wm_mk_kv_get_double(mk->market_id_str, "max_notional",
      "0.0", WM_MARKET_DEFAULT_MAX_NOTIONAL,
      "Real-mode per-order notional cap (quote currency). 0 = uncapped.");

  daily_loss_bps = wm_mk_kv_get_double(mk->market_id_str,
      "daily_loss_bps", "200.0", WM_MARKET_DEFAULT_DAILY_LOSS_BPS,
      "Real-mode daily realized-loss cap, basis points of starting"
      " cash. Real-mode signals FAIL closed once realized PnL since"
      " the day-anchor breaches -starting_cash * bps/10000.");

  pending_cap = wm_mk_kv_get_uint(mk->market_id_str, "pending_cap",
      "8", WM_MARKET_DEFAULT_PENDING_CAP,
      "Real-mode pending-order ceiling. New signals FAIL closed once"
      " the pending ring holds this many entries.");

  if(pending_cap > WM_MARKET_PENDING_CAP)
    pending_cap = WM_MARKET_PENDING_CAP;

  // Apply atomically under mk->lock so a concurrent fill engine call
  // sees a consistent cached-param set. cash-seeding is gated on
  // lifetime_fills_count to preserve live ledgers across a refresh.
  pthread_mutex_lock(&mk->lock);

  for(i = 0; i < WM_MARKET_MODE_COUNT; i++)
  {
    mk->session.stats[i].starting_cash = starting_cash;

    if(mk->session.stats[i].lifetime_fills_count == 0)
      mk->session.stats[i].cash = starting_cash;
  }

  mk->session.fee_bps        = fee_bps;
  mk->session.slip_bps       = slip_bps;
  mk->session.size_frac      = size_frac;
  mk->session.max_notional   = max_notional;
  mk->session.daily_loss_bps = daily_loss_bps;
  mk->session.pending_cap    = (uint32_t)pending_cap;

  pthread_mutex_unlock(&mk->lock);
}

// ------------------------------------------------------------------ //
// Daily-loss anchor                                                  //
// ------------------------------------------------------------------ //

static int64_t
wm_mk_utc_day_floor_ms(int64_t ts_ms)
{
  return((ts_ms / 86400000LL) * 86400000LL);
}

// Roll the per-mode daily PnL accumulator on UTC midnight. Caller
// holds mk->lock. Idempotent — first call after midnight resets
// realized_pnl_today; same-day calls are no-ops.
static void
wm_mk_roll_daily_anchor_locked(wm_market_stats_t *st, int64_t ts_ms)
{
  int64_t today_floor;

  if(st == NULL)
    return;

  today_floor = wm_mk_utc_day_floor_ms(ts_ms);

  if(st->daily_anchor_ms == 0 || st->daily_anchor_ms < today_floor)
  {
    st->daily_anchor_ms     = today_floor;
    st->realized_pnl_today  = 0.0;
  }
}

// ------------------------------------------------------------------ //
// Fill engine                                                        //
// ------------------------------------------------------------------ //

// Apply one fill to (position, stats[mode], fills[mode]). Long-only
// invariant: flat+sell and oversell are dropped with a WARN and the
// state is left unchanged.
//
// Caller MUST hold mk->lock.
void
wm_market_apply_fill_locked(whenmoon_market_t *mk, wm_market_mode_t mode,
    char side, double qty, double exec_px, double fee, int64_t ts_ms,
    const char *reason)
{
  wm_market_session_t *s;
  wm_market_stats_t   *st;
  wm_market_fill_t    *slot;
  uint32_t             head;
  double               notional;
  double               realized;
  double               close_qty;
  double               new_qty;
  double               new_notional;
  double               position_after;
  bool                 is_buy;

  if(mk == NULL || qty <= 0.0 || exec_px <= 0.0)
    return;

  if(mode >= WM_MARKET_MODE_COUNT)
    return;

  if(side != 'b' && side != 's')
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "market %s: drop fill — bad side '%c'",
        mk->market_id_str, side);
    return;
  }

  is_buy = (side == 'b');
  s      = &mk->session;
  st     = &s->stats[mode];

  // Long-only invariant: a sell with no open long is a no-op.
  if(!is_buy && s->position.side == WM_MARKET_POS_FLAT)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "market %s: drop fill — sell against flat (long-only)",
        mk->market_id_str);
    return;
  }

  // Long-only invariant: clip an oversell to the open long qty.
  if(!is_buy && s->position.side == WM_MARKET_POS_LONG &&
     qty > s->position.qty)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "market %s: clip oversell %.10g -> %.10g (long-only)",
        mk->market_id_str, qty, s->position.qty);
    qty = s->position.qty;
  }

  notional = qty * exec_px;

  // Position state machine — long-only.
  close_qty = 0.0;
  realized  = 0.0;

  if(is_buy)
  {
    if(s->position.side == WM_MARKET_POS_FLAT)
    {
      s->position.side          = WM_MARKET_POS_LONG;
      s->position.qty           = qty;
      s->position.avg_entry_px  = exec_px;
      s->position.opened_at_ms  = ts_ms;
    }
    else /* LONG: VWAP scale-in */
    {
      new_qty      = s->position.qty + qty;
      new_notional = s->position.qty * s->position.avg_entry_px
                   + qty * exec_px;
      s->position.qty          = new_qty;
      s->position.avg_entry_px = new_notional / new_qty;
      // opened_at_ms preserved on scale-in.
    }
  }

  else /* sell against open long */
  {
    close_qty = qty;
    realized  = (exec_px - s->position.avg_entry_px) * close_qty;

    s->position.qty -= close_qty;

    if(s->position.qty <= 1e-12)
    {
      s->position.side          = WM_MARKET_POS_FLAT;
      s->position.qty           = 0.0;
      s->position.avg_entry_px  = 0.0;
      s->position.opened_at_ms  = 0;
    }
  }

  // Apply cash + fee. Buys spend notional; sells receive notional.
  // Fees are deducted in both directions.
  if(is_buy)
    st->cash -= notional;
  else
    st->cash += notional;

  st->cash          -= fee;
  st->lifetime_fees += fee;

  // Roll the daily anchor before crediting today's realized PnL.
  wm_mk_roll_daily_anchor_locked(st, ts_ms);

  if(close_qty > 0.0)
  {
    st->realized_pnl_lifetime += realized;
    st->realized_pnl_today    += realized;
  }

  st->lifetime_fills_count++;
  st->last_fill_ms = ts_ms;

  position_after = (s->position.side == WM_MARKET_POS_LONG)
      ? s->position.qty
      : 0.0;

  // Append to the fills ring.
  head = s->fills_head[mode];
  slot = &s->fills[mode][head];
  memset(slot, 0, sizeof(*slot));

  slot->ts_ms          = ts_ms;
  slot->side           = side;
  slot->qty            = qty;
  slot->price          = exec_px;
  slot->fee            = fee;
  slot->slippage       = 0.0;        // synth slip is the caller's job
  slot->realized_pnl   = realized;
  slot->cash_after     = st->cash;
  slot->position_after = position_after;

  if(reason != NULL)
    snprintf(slot->reason, sizeof(slot->reason), "%s", reason);

  s->fills_head[mode] = (head + 1) % WM_MARKET_FILL_RING_CAP;
  s->fills_n[mode]++;

  // Update the cached mark — exec_px is the freshest price-of-record.
  s->last_mark_px = exec_px;
  s->last_mark_ms = ts_ms;

  clam(CLAM_INFO, WHENMOON_CTX,
      "market %s [%s] fill: %c qty=%.10g px=%.10g fee=%.6g"
      " realized=%.6g cash=%.4f pos=%.10g",
      mk->market_id_str, wm_market_mode_name(mode),
      side, qty, exec_px, fee, realized, st->cash, position_after);

  // Persistence is the caller's responsibility — engine_on_signal +
  // engine_record_external_fill enqueue an upsert after their fills,
  // selftest deliberately does not (it snapshot/restores the session
  // around its synthetic round-trip). Keeping persist out of this
  // function lets the selftest run without polluting the durable
  // wm_market_state row.
}

// ------------------------------------------------------------------ //
// Mode + reset                                                       //
// ------------------------------------------------------------------ //

bool
wm_market_set_mode(const char *market_id_str, wm_market_mode_t mode,
    char *errbuf, size_t errbuf_sz)
{
  whenmoon_state_t  *st;
  whenmoon_market_t *mk;
  wm_market_mode_t   prev;

  if(errbuf != NULL && errbuf_sz > 0)
    errbuf[0] = '\0';

  if(market_id_str == NULL || mode >= WM_MARKET_MODE_COUNT)
  {
    if(errbuf != NULL) snprintf(errbuf, errbuf_sz, "bad args");
    return(FAIL);
  }

  st = whenmoon_get_state();
  mk = wm_market_lookup_by_id(st, market_id_str);

  if(mk == NULL)
  {
    if(errbuf != NULL)
      snprintf(errbuf, errbuf_sz, "market %s not running", market_id_str);
    return(FAIL);
  }

  pthread_mutex_lock(&mk->lock);

  if(mk->session.position.side != WM_MARKET_POS_FLAT)
  {
    pthread_mutex_unlock(&mk->lock);

    if(errbuf != NULL)
      snprintf(errbuf, errbuf_sz,
          "market is long; flatten before switching mode");
    return(FAIL);
  }

  prev             = mk->session.mode;
  mk->session.mode = mode;

  (void)wm_market_persist_locked(mk);

  pthread_mutex_unlock(&mk->lock);

  clam(CLAM_INFO, WHENMOON_CTX,
      "market %s mode: %s -> %s",
      mk->market_id_str, wm_market_mode_name(prev),
      wm_market_mode_name(mode));

  return(SUCCESS);
}

void
wm_market_reset(const char *market_id_str, wm_market_mode_t mode_to_reset)
{
  whenmoon_state_t  *st;
  whenmoon_market_t *mk;
  wm_market_stats_t *target;

  if(market_id_str == NULL || mode_to_reset >= WM_MARKET_MODE_COUNT)
    return;

  st = whenmoon_get_state();
  mk = wm_market_lookup_by_id(st, market_id_str);

  if(mk == NULL)
    return;

  pthread_mutex_lock(&mk->lock);

  target = &mk->session.stats[mode_to_reset];

  // Flatten only when the targeted mode is the live one AND the market
  // currently holds a position. The operator is asserting "this ledger
  // is fresh" — drop the position silently.
  if(mode_to_reset == mk->session.mode &&
     mk->session.position.side != WM_MARKET_POS_FLAT)
  {
    mk->session.position.side          = WM_MARKET_POS_FLAT;
    mk->session.position.qty           = 0.0;
    mk->session.position.avg_entry_px  = 0.0;
    mk->session.position.opened_at_ms  = 0;
  }

  target->cash                  = target->starting_cash;
  target->realized_pnl_lifetime = 0.0;
  target->realized_pnl_today    = 0.0;
  target->daily_anchor_ms       = 0;
  target->lifetime_fees         = 0.0;
  target->lifetime_fills_count  = 0;
  target->last_fill_ms          = 0;

  memset(mk->session.fills[mode_to_reset], 0,
      sizeof(mk->session.fills[mode_to_reset]));
  mk->session.fills_n[mode_to_reset]    = 0;
  mk->session.fills_head[mode_to_reset] = 0;

  (void)wm_market_persist_locked(mk);

  pthread_mutex_unlock(&mk->lock);

  clam(CLAM_INFO, WHENMOON_CTX,
      "market %s [%s] reset (cash=%.4f)",
      mk->market_id_str, wm_market_mode_name(mode_to_reset),
      target->starting_cash);
}

// ------------------------------------------------------------------ //
// Engine entry points (installed but not yet wired)                  //
// ------------------------------------------------------------------ //

// Translate a strategy signal into a long-only directional advice.
// score > 0  -> buy (open / scale-in long if flat / long)
// score < 0  -> sell (close long; no-op if already flat)
// score == 0 -> hold
//
// Idempotency: a "buy" advice when already long is a no-op (no
// scale-in until WM-MK-3 introduces explicit advisor schemas).
// "Sell" advice when flat is also a no-op. This preserves the
// "first-advice-wins" priority model from whenmoon_market_model.md.
typedef enum
{
  WM_MK_ADVICE_HOLD = 0,
  WM_MK_ADVICE_BUY,
  WM_MK_ADVICE_SELL,
} wm_mk_advice_t;

static wm_mk_advice_t
wm_mk_signal_advice(const wm_strategy_signal_t *sig)
{
  if(sig == NULL)
    return(WM_MK_ADVICE_HOLD);

  if(sig->score > 0.0)
    return(WM_MK_ADVICE_BUY);

  if(sig->score < 0.0)
    return(WM_MK_ADVICE_SELL);

  return(WM_MK_ADVICE_HOLD);
}

void
wm_market_engine_on_signal(const char *market_id_str, double mark_px,
    int64_t mark_ms, const wm_strategy_signal_t *sig)
{
  whenmoon_state_t  *st;
  whenmoon_market_t *mk;
  wm_mk_advice_t     advice;
  wm_market_mode_t   mode;
  double             qty;
  double             slip;
  double             fill_px;
  double             fee_bps;
  double             slip_bps;
  double             notional;
  double             fee;

  if(sig == NULL || mark_px <= 0.0)
    return;

  st = whenmoon_get_state();
  mk = wm_market_lookup_by_id(st, market_id_str);

  if(mk == NULL)
    return;

  advice = wm_mk_signal_advice(sig);

  pthread_mutex_lock(&mk->lock);

  mk->session.last_mark_px = mark_px;
  mk->session.last_mark_ms = mark_ms;

  // Cache last-acted signal regardless of whether we act — the audit
  // trail per whenmoon_market_model.md says every tick's advice is
  // logged.
  mk->session.last_acted_signal     = *sig;
  mk->session.has_last_acted_signal = true;

  mode = mk->session.mode;

  // Manual mode: no auto-action. Strategies still log advice via
  // CLAM (above); the operator drives via WM-MK-4 force-trades.
  if(mode == WM_MARKET_MODE_MANUAL)
  {
    pthread_mutex_unlock(&mk->lock);
    return;
  }

  if(advice == WM_MK_ADVICE_HOLD)
  {
    pthread_mutex_unlock(&mk->lock);
    return;
  }

  // Idempotency: first-advice-wins skips no-op direction changes.
  if(advice == WM_MK_ADVICE_BUY &&
     mk->session.position.side == WM_MARKET_POS_LONG)
  {
    pthread_mutex_unlock(&mk->lock);
    clam(CLAM_DEBUG2, WHENMOON_CTX,
        "market %s: buy advice no-op (already long)",
        mk->market_id_str);
    return;
  }

  if(advice == WM_MK_ADVICE_SELL &&
     mk->session.position.side == WM_MARKET_POS_FLAT)
  {
    pthread_mutex_unlock(&mk->lock);
    clam(CLAM_DEBUG2, WHENMOON_CTX,
        "market %s: sell advice no-op (already flat)",
        mk->market_id_str);
    return;
  }

  fee_bps  = mk->session.fee_bps;
  slip_bps = mk->session.slip_bps;

  // Sizer: buys open `size_frac * cash / mark`; sells close the full
  // open long qty. Real-mode caps + gate trips are owned by
  // wm_market_engine_real_submit_locked (single source of truth).
  if(advice == WM_MK_ADVICE_BUY)
    qty = (mk->session.stats[mode].cash * mk->session.size_frac) / mark_px;

  else /* sell — close the full open long */
    qty = mk->session.position.qty;

  if(qty <= 0.0)
  {
    pthread_mutex_unlock(&mk->lock);
    return;
  }

  // WM-MK-3-B: real mode dispatches to the live submit path. Risk
  // gates + pending-row registration live in
  // wm_market_engine_real_submit_locked. On SUCCESS we do NOT call
  // apply_fill — the fill arrives asynchronously via the WS
  // user-channel + REST /fills consumers, which call back through
  // wm_market_engine_record_external_fill. On FAIL the helper has
  // already self-logged; just persist (no-op for the fail path) and
  // unlock.
  if(mode == WM_MARKET_MODE_REAL)
  {
    char errbuf[160];

    // Helper self-logs CLAM_WARN on every gate trip; errbuf is for
    // operator-issued verbs (not used here) so we drop it on the
    // floor. On SUCCESS the pending row is registered in mk->session;
    // the fill arrives asynchronously via the WS user-channel / REST
    // poll consumers in live.c.
    (void)wm_market_engine_real_submit_locked(mk,
        (advice == WM_MK_ADVICE_BUY) ? 'b' : 's',
        qty, mark_px, sig->ts_ms != 0 ? sig->ts_ms : mark_ms,
        sig, errbuf, sizeof(errbuf));

    (void)wm_market_persist_locked(mk);

    pthread_mutex_unlock(&mk->lock);
    return;
  }

  // Paper mode: synthetic slippage + immediate apply_fill.
  slip    = mark_px * (slip_bps / 10000.0);
  fill_px = (advice == WM_MK_ADVICE_BUY) ? mark_px + slip
                                         : mark_px - slip;

  if(fill_px <= 0.0)
  {
    pthread_mutex_unlock(&mk->lock);
    return;
  }

  notional = qty * fill_px;
  fee      = notional * (fee_bps / 10000.0);

  wm_market_apply_fill_locked(mk, mode,
      (advice == WM_MK_ADVICE_BUY) ? 'b' : 's',
      qty, fill_px, fee, sig->ts_ms != 0 ? sig->ts_ms : mark_ms,
      sig->reason);

  (void)wm_market_persist_locked(mk);

  pthread_mutex_unlock(&mk->lock);
}

// External fill recorder: dedup by trade_id within the per-market
// ring, then funnel into apply_fill_locked.
void
wm_market_engine_record_external_fill(const char *market_id_str,
    int64_t trade_id, char side, double qty, double exec_px, double fee,
    int64_t ts_ms, const char *reason)
{
  whenmoon_state_t    *st;
  whenmoon_market_t   *mk;
  wm_market_pending_t *p;
  uint32_t             i;
  uint8_t              j;

  if(market_id_str == NULL || qty <= 0.0 || exec_px <= 0.0)
    return;

  st = whenmoon_get_state();
  mk = wm_market_lookup_by_id(st, market_id_str);

  if(mk == NULL)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "external fill dropped: market %s not running",
        market_id_str);
    return;
  }

  pthread_mutex_lock(&mk->lock);

  // Dedup: scan every pending order's recorded trade ids. WM-MK-2
  // does not yet wire submission, so the pending ring is empty in
  // this chunk — the dedup pass is dead code until WM-MK-3 fills it
  // in. Retained for ABI completeness.
  for(i = 0; i < mk->session.pending_n; i++)
  {
    p = &mk->session.pending[i];

    for(j = 0; j < p->n_recorded_trades; j++)
    {
      if(p->recorded_trade_ids[j] == trade_id)
      {
        pthread_mutex_unlock(&mk->lock);
        clam(CLAM_DEBUG2, WHENMOON_CTX,
            "market %s: external fill trade_id=%" PRId64 " dedup hit",
            mk->market_id_str, trade_id);
        return;
      }
    }
  }

  wm_market_apply_fill_locked(mk, WM_MARKET_MODE_REAL, side, qty, exec_px,
      fee, ts_ms, reason);

  (void)wm_market_persist_locked(mk);

  pthread_mutex_unlock(&mk->lock);
}

// ------------------------------------------------------------------ //
// Force trade (WM-MK-4)                                              //
// ------------------------------------------------------------------ //

// Caller MUST hold `mk->lock`. Routes by `mk->session.mode`; see the
// header for the px-resolution rules and reply contract. Synth modes
// pre-flight sell-against-flat and detect the apply_fill no-op so the
// verb can produce a meaningful reply; real mode delegates every gate
// to `wm_market_engine_real_submit_locked`.
bool
wm_market_engine_force_trade_locked(whenmoon_market_t *mk, char side,
    double qty, double px_override, int64_t ts_ms, const char *reason,
    char *errbuf, size_t errbuf_sz)
{
  wm_market_mode_t mode;
  double           exec_px;
  double           slip;
  double           fee;
  double           notional;
  uint64_t         fills_pre;
  uint64_t         fills_post;
  bool             is_buy;
  bool             ok;

  #define ERRSET(...) do { \
      if(errbuf != NULL && errbuf_sz > 0) \
        snprintf(errbuf, errbuf_sz, __VA_ARGS__); \
    } while(0)

  if(errbuf != NULL && errbuf_sz > 0)
    errbuf[0] = '\0';

  if(mk == NULL || qty <= 0.0)
  {
    ERRSET("invalid args");
    return(FAIL);
  }

  if(side != 'b' && side != 's')
  {
    ERRSET("side must be 'b' or 's'");
    return(FAIL);
  }

  is_buy = (side == 'b');
  mode   = mk->session.mode;

  // Resolve exec / limit px. Operator override always wins; otherwise
  // fall through to last live ticker, then cached advice mark.
  exec_px = px_override;

  if(exec_px <= 0.0)
  {
    if(mk->last_px > 0.0)
      exec_px = mk->last_px;
    else if(mk->session.last_mark_px > 0.0)
      exec_px = mk->session.last_mark_px;
    else
    {
      ERRSET("market %s has no live mark (no ticker yet and no override)",
          mk->market_id_str);
      return(FAIL);
    }
  }

  // Real-mode dispatch — sig=NULL is honored by the helper (param
  // reserved for future audit hooks per live.c:975). All five gates
  // apply; helper self-logs CLAM_WARN on every gate trip and writes
  // `errbuf` for the verb to surface verbatim. No persist call here:
  // wm_market_engine_real_submit_locked already persists the pending
  // row, and the fill-arrival path persists from
  // wm_market_engine_record_external_fill.
  if(mode == WM_MARKET_MODE_REAL)
  {
    ok = wm_market_engine_real_submit_locked(mk, side, qty, exec_px,
        ts_ms, NULL, errbuf, errbuf_sz);

    if(ok == SUCCESS)
      clam(CLAM_INFO, WHENMOON_CTX,
          "whenmoon.force: market %s mode=real side=%c qty=%.10g"
          " px=%.10g (submitted)%s%s%s",
          mk->market_id_str, side, qty, exec_px,
          reason != NULL ? " reason=\"" : "",
          reason != NULL ? reason       : "",
          reason != NULL ? "\""         : "");

    return(ok);
  }

  // Manual + paper: pre-flight long-only invariant so the verb can
  // reply meaningfully. apply_fill_locked drops sell-against-flat
  // with a CLAM_WARN otherwise and the operator sees no apparent
  // state change.
  if(!is_buy && mk->session.position.side == WM_MARKET_POS_FLAT)
  {
    ERRSET("%s is flat — nothing to sell", mk->market_id_str);
    return(FAIL);
  }

  // Paper applies synth slippage only when there's no operator
  // override. Manual never applies synth slippage. Fees apply in
  // both modes (operator bookkeeping consistent with reality).
  slip = 0.0;

  if(mode == WM_MARKET_MODE_PAPER && px_override <= 0.0)
    slip = exec_px * (mk->session.slip_bps / 10000.0);

  if(is_buy)
    exec_px = exec_px + slip;
  else
    exec_px = exec_px - slip;

  if(exec_px <= 0.0)
  {
    ERRSET("computed exec px <= 0");
    return(FAIL);
  }

  notional = qty * exec_px;
  fee      = notional * (mk->session.fee_bps / 10000.0);

  fills_pre = mk->session.stats[mode].lifetime_fills_count;

  wm_market_apply_fill_locked(mk, mode, side, qty, exec_px, fee, ts_ms,
      reason != NULL ? reason : "force-operator");

  fills_post = mk->session.stats[mode].lifetime_fills_count;

  // apply_fill_locked drops oversell-against-flat (which we
  // pre-flighted above) and clips oversell silently. Detect the
  // no-op path so the verb can report it as a soft FAIL.
  if(fills_post == fills_pre)
  {
    ERRSET("apply_fill no-op (long-only invariant tripped)");
    return(FAIL);
  }

  (void)wm_market_persist_locked(mk);

  clam(CLAM_INFO, WHENMOON_CTX,
      "whenmoon.force: market %s mode=%s side=%c qty=%.10g px=%.10g"
      "%s%s%s",
      mk->market_id_str, wm_market_mode_name(mode), side, qty, exec_px,
      reason != NULL ? " reason=\"" : "",
      reason != NULL ? reason       : "",
      reason != NULL ? "\""         : "");

  return(SUCCESS);

  #undef ERRSET
}

// ------------------------------------------------------------------ //
// Selftest                                                           //
// ------------------------------------------------------------------ //

bool
wm_market_engine_selftest(const char *market_id_str, char *errbuf,
    size_t errbuf_sz)
{
  whenmoon_state_t      *st;
  whenmoon_market_t     *mk;
  wm_market_mode_t       prev_mode;
  wm_market_position_t   prev_position;
  wm_market_stats_t      prev_paper_stats;
  uint64_t               prev_fills_n;
  uint32_t               prev_fills_head;
  double                 prev_last_mark_px;
  int64_t                prev_last_mark_ms;
  wm_strategy_signal_t   prev_last_signal;
  bool                   prev_has_signal;
  /* Heap-buffer the pre-test fills ring: stack-local would be a
     few hundred KiB and risks the cmd worker thread stack. */
  wm_market_fill_t      *prev_fills;
  double                 buy_px = 50000.0;
  double                 sell_px = 51000.0;
  double                 qty = 0.1;
  int64_t                ts;
  uint64_t               fills_pre;
  uint64_t               fills_post;
  double                 cash_pre;
  double                 cash_post;
  double                 realized;
  bool                   ok = SUCCESS;

  if(errbuf != NULL && errbuf_sz > 0)
    errbuf[0] = '\0';

  if(market_id_str == NULL)
  {
    if(errbuf != NULL) snprintf(errbuf, errbuf_sz, "bad args");
    return(FAIL);
  }

  st = whenmoon_get_state();
  mk = wm_market_lookup_by_id(st, market_id_str);

  if(mk == NULL)
  {
    if(errbuf != NULL)
      snprintf(errbuf, errbuf_sz, "market %s not running", market_id_str);
    return(FAIL);
  }

  // Refresh KV before selftest so the run uses operator-edited values.
  wm_market_session_refresh_kv(mk);

  prev_fills = mem_alloc("whenmoon", "mp_selftest_prev_fills",
      sizeof(*prev_fills) * WM_MARKET_FILL_RING_CAP);

  if(prev_fills == NULL)
  {
    if(errbuf != NULL)
      snprintf(errbuf, errbuf_sz, "alloc failed");
    return(FAIL);
  }

  ts = (int64_t)time(NULL) * 1000;

  pthread_mutex_lock(&mk->lock);

  if(mk->session.position.side != WM_MARKET_POS_FLAT)
  {
    pthread_mutex_unlock(&mk->lock);
    mem_free(prev_fills);
    if(errbuf != NULL)
      snprintf(errbuf, errbuf_sz,
          "selftest requires flat position (current side=%d qty=%.10g)",
          (int)mk->session.position.side, mk->session.position.qty);
    return(FAIL);
  }

  // Snapshot every paper-mode field the test may mutate. The selftest
  // restores all of them before unlocking + re-persists, so the
  // operator's durable wm_market_state row is unaffected by the
  // synthetic round-trip.
  prev_mode         = mk->session.mode;
  prev_position     = mk->session.position;
  prev_paper_stats  = mk->session.stats[WM_MARKET_MODE_PAPER];
  prev_fills_n      = mk->session.fills_n[WM_MARKET_MODE_PAPER];
  prev_fills_head   = mk->session.fills_head[WM_MARKET_MODE_PAPER];
  prev_last_mark_px = mk->session.last_mark_px;
  prev_last_mark_ms = mk->session.last_mark_ms;
  prev_last_signal  = mk->session.last_acted_signal;
  prev_has_signal   = mk->session.has_last_acted_signal;
  memcpy(prev_fills, mk->session.fills[WM_MARKET_MODE_PAPER],
      sizeof(*prev_fills) * WM_MARKET_FILL_RING_CAP);

  // Force PAPER mode for selftest — no real-money side effects.
  mk->session.mode = WM_MARKET_MODE_PAPER;

  fills_pre = mk->session.stats[WM_MARKET_MODE_PAPER].lifetime_fills_count;
  cash_pre  = mk->session.stats[WM_MARKET_MODE_PAPER].cash;

  wm_market_apply_fill_locked(mk, WM_MARKET_MODE_PAPER, 'b', qty, buy_px,
      0.0, ts, "selftest-buy");

  if(mk->session.position.side != WM_MARKET_POS_LONG ||
     fabs(mk->session.position.qty - qty) > 1e-9)
  {
    ok = FAIL;
    if(errbuf != NULL)
      snprintf(errbuf, errbuf_sz,
          "selftest buy: expected long qty=%.10g, got side=%d qty=%.10g",
          qty, (int)mk->session.position.side, mk->session.position.qty);
  }

  if(ok == SUCCESS)
  {
    wm_market_apply_fill_locked(mk, WM_MARKET_MODE_PAPER, 's', qty,
        sell_px, 0.0, ts + 1000, "selftest-sell");

    realized = (sell_px - buy_px) * qty;

    if(mk->session.position.side != WM_MARKET_POS_FLAT)
    {
      ok = FAIL;
      if(errbuf != NULL)
        snprintf(errbuf, errbuf_sz,
            "selftest sell: expected flat, got side=%d qty=%.10g",
            (int)mk->session.position.side, mk->session.position.qty);
    }

    fills_post = mk->session.stats[WM_MARKET_MODE_PAPER]
        .lifetime_fills_count;
    cash_post  = mk->session.stats[WM_MARKET_MODE_PAPER].cash;

    if(ok == SUCCESS && fills_post != fills_pre + 2)
    {
      ok = FAIL;
      if(errbuf != NULL)
        snprintf(errbuf, errbuf_sz,
            "selftest fills: expected %" PRIu64 ", got %" PRIu64,
            fills_pre + 2, fills_post);
    }

    if(ok == SUCCESS &&
       fabs((cash_post - cash_pre) - realized) > 1e-6)
    {
      ok = FAIL;
      if(errbuf != NULL)
        snprintf(errbuf, errbuf_sz,
            "selftest cash delta: expected %.6f, got %.6f",
            realized, cash_post - cash_pre);
    }
  }

  // Restore the entire paper-mode session state regardless of outcome
  // so the operator's durable ledger is untouched.
  mk->session.mode                              = prev_mode;
  mk->session.position                          = prev_position;
  mk->session.stats[WM_MARKET_MODE_PAPER]       = prev_paper_stats;
  mk->session.fills_n[WM_MARKET_MODE_PAPER]     = prev_fills_n;
  mk->session.fills_head[WM_MARKET_MODE_PAPER]  = prev_fills_head;
  mk->session.last_mark_px                      = prev_last_mark_px;
  mk->session.last_mark_ms                      = prev_last_mark_ms;
  mk->session.last_acted_signal                 = prev_last_signal;
  mk->session.has_last_acted_signal             = prev_has_signal;
  memcpy(mk->session.fills[WM_MARKET_MODE_PAPER], prev_fills,
      sizeof(*prev_fills) * WM_MARKET_FILL_RING_CAP);

  // Re-enqueue with the restored state. The persist queue coalesces
  // by market_id, so this overwrites the spurious snapshots that
  // apply_fill_locked DID NOT enqueue (we moved the persist call out
  // of apply_fill_locked specifically so this restore is sufficient).
  // But the periodic flush task may have flushed the pre-test row
  // mid-test on a slow machine, so a final persist guarantees the
  // restored state lands.
  (void)wm_market_persist_locked(mk);

  pthread_mutex_unlock(&mk->lock);

  mem_free(prev_fills);

  if(ok == SUCCESS)
    clam(CLAM_INFO, WHENMOON_CTX,
        "market %s selftest PASS (paper buy %.10g @ %.2f, sell @ %.2f"
        " realized=%.4f; durable state unchanged)",
        mk->market_id_str, qty, buy_px, sell_px,
        (sell_px - buy_px) * qty);
  else
    clam(CLAM_WARN, WHENMOON_CTX,
        "market %s selftest FAIL: %s",
        mk->market_id_str, errbuf != NULL ? errbuf : "(no detail)");

  return(ok);
}
