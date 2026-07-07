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
#include "account.h"
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

// Public (declared in market_engine.h): also used by the real-cash
// reconcile path in live.c to read the quote-allocation knobs fresh.
double
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
// WM-MK-6: risk-adjusted metric helpers                              //
// ------------------------------------------------------------------ //
//
// Sharpe + Sortino walk the shared session equity-samples ring,
// oldest→newest, and compute simple per-step returns:
//   r[i] = (eq[i] - eq[i-1]) / eq[i-1]
// Sharpe   = mean(r) / stddev(r)        (population variance)
// Sortino  = mean(r) / downside_dev(r)  (sqrt of mean of negative-r²)
// Both are per-trade / per-fill values — unannualized, matching the
// legacy wm_pnl_acc_t. Fewer than two samples (zero returns) yield
// 0.0 so sweep scoring sorts no-trade iterations to the bottom.

static uint32_t
wm_mk_equity_walk_count(uint64_t total_n)
{
  if(total_n > (uint64_t)WM_MARKET_EQUITY_RING_CAP)
    return(WM_MARKET_EQUITY_RING_CAP);

  return((uint32_t)total_n);
}

static uint32_t
wm_mk_equity_first_index(uint64_t total_n, uint32_t head)
{
  // Ring not yet wrapped — oldest is at slot 0.
  if(total_n <= (uint64_t)WM_MARKET_EQUITY_RING_CAP)
    return(0u);

  // Wrapped — `head` indexes the next write slot, which is also the
  // oldest live sample after the wrap point.
  return(head);
}

double
wm_market_stats_sharpe(const wm_market_stats_t *st,
    const wm_market_equity_sample_t *ring, uint64_t total_n,
    uint32_t head)
{
  uint32_t n;
  uint32_t i;
  uint32_t first;
  uint32_t prev_idx;
  uint32_t curr_idx;
  uint32_t n_returns;
  double   prev_eq;
  double   curr_eq;
  double   r;
  double   sum;
  double   sum_sq;
  double   mean;
  double   variance;
  double   stddev;

  (void)st;

  if(ring == NULL || total_n < 2)
    return(0.0);

  n = wm_mk_equity_walk_count(total_n);

  if(n < 2)
    return(0.0);

  first      = wm_mk_equity_first_index(total_n, head);
  prev_idx   = first;
  prev_eq    = ring[prev_idx].equity;
  sum        = 0.0;
  sum_sq     = 0.0;
  n_returns  = 0;

  for(i = 1; i < n; i++)
  {
    curr_idx = (first + i) % WM_MARKET_EQUITY_RING_CAP;
    curr_eq  = ring[curr_idx].equity;

    if(prev_eq <= 0.0)
    {
      prev_idx = curr_idx;
      prev_eq  = curr_eq;
      continue;
    }

    r = (curr_eq - prev_eq) / prev_eq;

    sum    += r;
    sum_sq += r * r;
    n_returns++;

    prev_idx = curr_idx;
    prev_eq  = curr_eq;
  }

  if(n_returns < 2)
    return(0.0);

  mean     = sum / (double)n_returns;
  variance = (sum_sq / (double)n_returns) - mean * mean;

  if(variance <= 0.0)
    return(0.0);

  stddev = sqrt(variance);

  if(stddev <= 0.0 || !isfinite(stddev))
    return(0.0);

  return(mean / stddev);
}

double
wm_market_stats_sortino(const wm_market_stats_t *st,
    const wm_market_equity_sample_t *ring, uint64_t total_n,
    uint32_t head)
{
  uint32_t n;
  uint32_t i;
  uint32_t first;
  uint32_t prev_idx;
  uint32_t curr_idx;
  uint32_t n_returns;
  double   prev_eq;
  double   curr_eq;
  double   r;
  double   sum;
  double   sum_down_sq;
  double   mean;
  double   downside_variance;
  double   downside_dev;

  (void)st;

  if(ring == NULL || total_n < 2)
    return(0.0);

  n = wm_mk_equity_walk_count(total_n);

  if(n < 2)
    return(0.0);

  first       = wm_mk_equity_first_index(total_n, head);
  prev_idx    = first;
  prev_eq     = ring[prev_idx].equity;
  sum         = 0.0;
  sum_down_sq = 0.0;
  n_returns   = 0;

  for(i = 1; i < n; i++)
  {
    curr_idx = (first + i) % WM_MARKET_EQUITY_RING_CAP;
    curr_eq  = ring[curr_idx].equity;

    if(prev_eq <= 0.0)
    {
      prev_idx = curr_idx;
      prev_eq  = curr_eq;
      continue;
    }

    r = (curr_eq - prev_eq) / prev_eq;

    sum += r;

    if(r < 0.0)
      sum_down_sq += r * r;

    n_returns++;

    prev_idx = curr_idx;
    prev_eq  = curr_eq;
  }

  if(n_returns < 2)
    return(0.0);

  mean              = sum / (double)n_returns;
  downside_variance = sum_down_sq / (double)n_returns;

  if(downside_variance <= 0.0)
    return(0.0);

  downside_dev = sqrt(downside_variance);

  if(downside_dev <= 0.0 || !isfinite(downside_dev))
    return(0.0);

  return(mean / downside_dev);
}

double
wm_market_stats_profit_factor(const wm_market_stats_t *st)
{
  double pf;

  if(st == NULL || st->gross_loss <= 0.0)
    return(0.0);

  pf = st->gross_profit / st->gross_loss;

  if(!isfinite(pf) || pf < 0.0)
    return(0.0);

  return(pf);
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

    // WM-MK-6: per-mode trade outcome accounting. Only closing fills
    // count toward n_trades / wins / losses + gross profit/loss — the
    // legacy wm_pnl_acc_t did the same. Zero-realized closes
    // (rounding edge) land in neither bucket but still bump n_trades.
    st->n_trades++;

    if(realized > 0.0)
    {
      st->n_wins++;
      st->gross_profit += realized;
    }
    else if(realized < 0.0)
    {
      st->n_losses++;
      st->gross_loss += -realized;
    }
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

  // WM-MK-6: post-fill equity = cash[mode] + position-at-mark. Drives
  // the per-mode drawdown tracker AND feeds the shared session
  // equity-samples ring used by Sharpe/Sortino. Ring is shared across
  // modes (single mode in backtest; live markets that switch modes
  // accept the mixed-stream simplification per WM-MK-6 scope).
  {
    double equity;
    double drawdown;

    equity = st->cash + position_after * exec_px;

    if(equity > st->equity_peak)
      st->equity_peak = equity;

    if(st->equity_peak > 0.0)
    {
      drawdown = (st->equity_peak - equity) / st->equity_peak;

      if(drawdown > st->max_drawdown)
        st->max_drawdown = drawdown;
    }

    s->equity_samples[s->equity_head].ts_ms  = ts_ms;
    s->equity_samples[s->equity_head].equity = equity;
    s->equity_head = (s->equity_head + 1u) % WM_MARKET_EQUITY_RING_CAP;
    s->equity_n++;
  }

  // WM-BT-FILLLOG-1: synthetic backtest markets (market_id == -1, set in
  // wm_market_create_synthetic) fire dozens of fills per simulated second
  // across N sweep workers. Emitting a per-fill CLAM_INFO here funnels
  // every worker through clam()'s global non-recursive clam_mutex (I/O
  // held inside the critical section — printf + a socket write per
  // attached `botmanctl -S` subscriber), collapsing an N-thread sweep back
  // to ~1 core. Suppress the per-fill line for synthetics; keep it for
  // live paper/real markets (low volume, genuinely useful there).
  if(mk->market_id != -1)
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

  if(st == NULL || st->markets == NULL)
  {
    if(errbuf != NULL) snprintf(errbuf, errbuf_sz, "no market state");
    return(FAIL);
  }

  // WM-MKT-ARR-UAF-1: hold rdlock across lookup + the mk->lock edit and the
  // trailing clam (which still dereferences mk->market_id_str).
  pthread_rwlock_rdlock(&st->markets->arr_lock);
  mk = wm_market_lookup_by_id(st, market_id_str);

  if(mk == NULL)
  {
    pthread_rwlock_unlock(&st->markets->arr_lock);
    if(errbuf != NULL)
      snprintf(errbuf, errbuf_sz, "market %s not running", market_id_str);
    return(FAIL);
  }

  pthread_mutex_lock(&mk->lock);

  if(mk->session.position.side != WM_MARKET_POS_FLAT)
  {
    pthread_mutex_unlock(&mk->lock);
    pthread_rwlock_unlock(&st->markets->arr_lock);

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

  pthread_rwlock_unlock(&st->markets->arr_lock);
  return(SUCCESS);
}

// Operator halt. Bypasses wm_market_set_mode's flat-position rule by
// design — see the header comment.
void
wm_market_halt_all(uint32_t *out_visited, uint32_t *out_with_position)
{
  whenmoon_state_t   *st;
  whenmoon_markets_t *m;
  uint32_t            visited        = 0;
  uint32_t            with_position  = 0;
  uint32_t            i;

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
    goto out;

  m = st->markets;

  // WM-MKT-ARR-UAF-1: rdlock across the walk — arr stays stable and no
  // session is freed by a concurrent remove while we visit it.
  pthread_rwlock_rdlock(&m->arr_lock);

  for(i = 0; i < m->n_markets; i++)
  {
    whenmoon_market_t *mk = m->arr[i];
    wm_market_mode_t   prev;
    bool               was_long;

    pthread_mutex_lock(&mk->lock);

    prev     = mk->session.mode;
    was_long = (mk->session.position.side != WM_MARKET_POS_FLAT);

    if(prev != WM_MARKET_MODE_MANUAL)
      mk->session.mode = WM_MARKET_MODE_MANUAL;

    (void)wm_market_persist_locked(mk);

    pthread_mutex_unlock(&mk->lock);

    visited++;

    if(was_long)
      with_position++;

    if(prev != WM_MARKET_MODE_MANUAL)
      clam(CLAM_INFO, WHENMOON_CTX,
          "market %s mode: %s -> manual (halt%s)",
          mk->market_id_str, wm_market_mode_name(prev),
          was_long ? "; position open" : "");
  }

  pthread_rwlock_unlock(&m->arr_lock);

  clam(CLAM_WARN, WHENMOON_CTX,
      "operator halt: %u market%s -> manual (%u with open positions)",
      visited, visited == 1 ? "" : "s", with_position);

out:
  if(out_visited != NULL)
    *out_visited = visited;

  if(out_with_position != NULL)
    *out_with_position = with_position;
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

  if(st == NULL || st->markets == NULL)
    return;

  // WM-MKT-ARR-UAF-1: hold rdlock across lookup + the mk->lock edit and the
  // trailing clam (which dereferences mk->market_id_str).
  pthread_rwlock_rdlock(&st->markets->arr_lock);
  mk = wm_market_lookup_by_id(st, market_id_str);

  if(mk == NULL)
  {
    pthread_rwlock_unlock(&st->markets->arr_lock);
    return;
  }

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

  pthread_rwlock_unlock(&st->markets->arr_lock);
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

  if(market_id_str == NULL)
    return;

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
    return;

  // WM-MKT-ARR-UAF-1: hold rdlock across lookup + the engine pass (which
  // works on mk under mk->lock). Recursive rdlock inside is deadlock-safe.
  pthread_rwlock_rdlock(&st->markets->arr_lock);
  mk = wm_market_lookup_by_id(st, market_id_str);

  if(mk == NULL)
  {
    pthread_rwlock_unlock(&st->markets->arr_lock);
    return;
  }

  wm_market_engine_on_signal_with_mk(mk, mark_px, mark_ms, sig);
  pthread_rwlock_unlock(&st->markets->arr_lock);
}

void
wm_market_engine_on_signal_with_mk(whenmoon_market_t *mk, double mark_px,
    int64_t mark_ms, const wm_strategy_signal_t *sig)
{
  wm_mk_advice_t     advice;
  wm_market_mode_t   mode;
  double             qty;
  double             slip;
  double             fill_px;
  double             fee_bps;
  double             slip_bps;
  double             notional;
  double             fee;

  if(mk == NULL || sig == NULL || mark_px <= 0.0)
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

  // WM-WARMUP-2: act on advice only once the market's indicators are
  // warm. Advice is already cached above for the audit trail, so a
  // warming market still records (and logs) what it would have done.
  // Synthetic backtest markets are forced READY at creation, so this
  // never gates them.
  if(mk->warmup_state != WM_WARM_READY)
  {
    pthread_mutex_unlock(&mk->lock);
    return;
  }

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
  char                 exch[EXCHANGE_NAME_SZ];

  if(market_id_str == NULL || qty <= 0.0 || exec_px <= 0.0)
    return;

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
    return;

  // WM-MKT-ARR-UAF-1: hold rdlock across lookup + the mk->lock apply.
  pthread_rwlock_rdlock(&st->markets->arr_lock);
  mk = wm_market_lookup_by_id(st, market_id_str);

  if(mk == NULL)
  {
    pthread_rwlock_unlock(&st->markets->arr_lock);
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
        pthread_rwlock_unlock(&st->markets->arr_lock);
        return;
      }
    }
  }

  wm_market_apply_fill_locked(mk, WM_MARKET_MODE_REAL, side, qty, exec_px,
      fee, ts_ms, reason);

  (void)wm_market_persist_locked(mk);

  // Capture the bound exchange while locked; used post-unlock to
  // fast-forward the balance cache (a real fill moves cash, so refresh
  // it immediately rather than waiting for the next scheduled poll).
  snprintf(exch, sizeof(exch), "%s", mk->exchange_name);

  pthread_mutex_unlock(&mk->lock);
  pthread_rwlock_unlock(&st->markets->arr_lock);

  // Outside the lock: never hold mk->lock across the async submit (it
  // can re-enter clam/registry paths — see the clam-reentry deadlock
  // class). wm_account_fast_forward re-checks the real-mode + creds gate.
  wm_account_fast_forward(exch);
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
