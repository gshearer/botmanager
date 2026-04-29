// botmanager — MIT
// Whenmoon trade-book registry + paper-mode fill engine (WM-LT-4).
//
// One registry per process. Each registered book is keyed by
// (market_id_str, strategy_name); books are created lazily by the
// /whenmoon trade … verbs (and by direct refresh-from-KV calls during
// attach when the strategy declares a non-OFF default mode).
//
// Live + paper share this same book. Backtest (WM-LT-5) will run
// snapshot iterations against private books that never enter the
// registry, so the live registry stays small (one per attached
// strategy/market pair).
//
// Locking: a single registry mutex serialises all reads + writes
// (rationale in order.h). The signal entry point holds the registry
// lock end-to-end; the work inside is bounded (sizer + arithmetic).

#define WHENMOON_INTERNAL
#include "order.h"

#include "pnl.h"
#include "sizer.h"
#include "strategy.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"

#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define WM_TRADE_CTX        "whenmoon.trade"

// KV defaults — used when no global / per-attachment override exists.
// Picked to be safe-ish for paper testing: 0.40% fee matches Coinbase
// retail spot, 5 bps slippage is conservative for liquid pairs at
// retail size, $10k starting cash is a tractable demo balance.
#define WM_TRADE_DEF_FEE_BPS         40.0
#define WM_TRADE_DEF_SLIP_BPS        5.0
#define WM_TRADE_DEF_SIZE_FRAC       0.10
#define WM_TRADE_DEF_MAX_POSITION    0.0     // 0 = no cap
#define WM_TRADE_DEF_STARTING_CASH   10000.0

// ----------------------------------------------------------------------- //
// Registry — global singleton + TLS active-registry plumbing              //
// ----------------------------------------------------------------------- //
//
// Every public book API resolves the calling thread's "active" registry
// via wm_trade_get_active_registry. Threads that have never bound a
// private registry (live + paper paths, cmd workers, etc.) see the
// global. Backtest iterations (WM-LT-6) bind a freshly-created private
// registry via wm_trade_engine_use_registry; subsequent ops on that
// thread, including signal emission from inside a strategy callback,
// route to the private book without touching the global mutex.

static wm_trade_registry_t g_registry;
static pthread_key_t       g_bt_registry_key;
static bool                g_bt_registry_key_inited = false;

static wm_trade_registry_t *
wm_trade_get_active_registry(void)
{
  wm_trade_registry_t *r;

  if(!g_registry.initialized)
    return(NULL);

  if(!g_bt_registry_key_inited)
    return(&g_registry);

  r = (wm_trade_registry_t *)pthread_getspecific(g_bt_registry_key);
  return(r != NULL ? r : &g_registry);
}

// ----------------------------------------------------------------------- //
// Mode helpers                                                            //
// ----------------------------------------------------------------------- //

bool
wm_trade_mode_parse(const char *tok, wm_trade_mode_t *out)
{
  if(tok == NULL || out == NULL)
    return(FAIL);

  if(strcasecmp(tok, "off")      == 0) { *out = WM_TRADE_MODE_OFF;      return(SUCCESS); }
  if(strcasecmp(tok, "paper")    == 0) { *out = WM_TRADE_MODE_PAPER;    return(SUCCESS); }
  if(strcasecmp(tok, "backtest") == 0) { *out = WM_TRADE_MODE_BACKTEST; return(SUCCESS); }
  if(strcasecmp(tok, "live")     == 0) { *out = WM_TRADE_MODE_LIVE;     return(SUCCESS); }

  return(FAIL);
}

const char *
wm_trade_mode_name(wm_trade_mode_t m)
{
  switch(m)
  {
    case WM_TRADE_MODE_OFF:       return("off");
    case WM_TRADE_MODE_PAPER:     return("paper");
    case WM_TRADE_MODE_BACKTEST:  return("backtest");
    case WM_TRADE_MODE_LIVE:      return("live");
  }

  return("?");
}

// ----------------------------------------------------------------------- //
// Lifecycle                                                               //
// ----------------------------------------------------------------------- //

bool
wm_trade_engine_init(void)
{
  if(g_registry.initialized)
    return(SUCCESS);

  if(pthread_mutex_init(&g_registry.lock, NULL) != 0)
    return(FAIL);

  // Per-thread active-registry slot. The destructor is NULL — every
  // caller that pins a private registry via use_registry is responsible
  // for unbinding before its thread exits (and for destroying the
  // registry).
  if(pthread_key_create(&g_bt_registry_key, NULL) != 0)
  {
    pthread_mutex_destroy(&g_registry.lock);
    return(FAIL);
  }

  g_bt_registry_key_inited = true;
  g_registry.head          = NULL;
  g_registry.n_books       = 0;
  g_registry.initialized   = true;

  clam(CLAM_INFO, WM_TRADE_CTX, "trade engine initialized");
  return(SUCCESS);
}

static void
wm_trade_book_free_locked(wm_trade_book_t *book)
{
  if(book == NULL)
    return;

  if(book->pnl != NULL)
    wm_pnl_acc_free(book->pnl);

  mem_free(book);
}

void
wm_trade_engine_destroy(void)
{
  wm_trade_book_t *b;
  wm_trade_book_t *next;

  if(!g_registry.initialized)
    return;

  pthread_mutex_lock(&g_registry.lock);

  b = g_registry.head;

  while(b != NULL)
  {
    next = b->next;
    wm_trade_book_free_locked(b);
    b    = next;
  }

  g_registry.head    = NULL;
  g_registry.n_books = 0;

  pthread_mutex_unlock(&g_registry.lock);
  pthread_mutex_destroy(&g_registry.lock);

  if(g_bt_registry_key_inited)
  {
    pthread_key_delete(g_bt_registry_key);
    g_bt_registry_key_inited = false;
  }

  g_registry.initialized = false;

  clam(CLAM_INFO, WM_TRADE_CTX, "trade engine destroyed");
}

// ----------------------------------------------------------------------- //
// Private registry lifecycle (WM-LT-6)                                    //
// ----------------------------------------------------------------------- //

wm_trade_registry_t *
wm_trade_registry_create(void)
{
  wm_trade_registry_t *r;

  if(!g_registry.initialized)
    return(NULL);

  r = mem_alloc("whenmoon", "trade_registry", sizeof(*r));

  if(r == NULL)
    return(NULL);

  memset(r, 0, sizeof(*r));

  if(pthread_mutex_init(&r->lock, NULL) != 0)
  {
    mem_free(r);
    return(NULL);
  }

  r->initialized = true;
  return(r);
}

void
wm_trade_registry_destroy(wm_trade_registry_t *r)
{
  wm_trade_book_t *b;
  wm_trade_book_t *next;

  if(r == NULL)
    return;

  pthread_mutex_lock(&r->lock);

  b = r->head;

  while(b != NULL)
  {
    next = b->next;
    wm_trade_book_free_locked(b);
    b    = next;
  }

  r->head        = NULL;
  r->n_books     = 0;
  r->initialized = false;

  pthread_mutex_unlock(&r->lock);
  pthread_mutex_destroy(&r->lock);

  mem_free(r);
}

void
wm_trade_engine_use_registry(wm_trade_registry_t *r)
{
  if(!g_bt_registry_key_inited)
    return;

  pthread_setspecific(g_bt_registry_key, r);
}

// ----------------------------------------------------------------------- //
// Book lookup                                                             //
// ----------------------------------------------------------------------- //

wm_trade_book_t *
wm_trade_book_find(wm_trade_registry_t *reg,
    const char *market_id_str, const char *strategy_name)
{
  wm_trade_book_t *b;

  if(reg == NULL || market_id_str == NULL || strategy_name == NULL)
    return(NULL);

  for(b = reg->head; b != NULL; b = b->next)
  {
    if(strncmp(b->market_id_str, market_id_str,
           sizeof(b->market_id_str)) != 0)
      continue;

    if(strncmp(b->strategy_name, strategy_name,
           sizeof(b->strategy_name)) != 0)
      continue;

    return(b);
  }

  return(NULL);
}

// Resolve every cached parameter from the strategy KV. Caller holds
// the registry lock. Mode is reset only when book->mode is OFF (the
// default initial state); subsequent /whenmoon trade mode calls own
// the mode field outright.
static void
wm_trade_book_refresh_kv_locked(wm_trade_book_t *book)
{
  const char     *mid;
  const char     *strat;
  const char     *mode_str;
  wm_trade_mode_t parsed_mode;

  if(book == NULL)
    return;

  mid   = book->market_id_str;
  strat = book->strategy_name;

  book->fee_bps =
      wm_strategy_kv_get_dbl(mid, strat, "fee_bps",
          WM_TRADE_DEF_FEE_BPS);
  book->slip_bps =
      wm_strategy_kv_get_dbl(mid, strat, "slip_bps",
          WM_TRADE_DEF_SLIP_BPS);
  book->size_frac =
      wm_strategy_kv_get_dbl(mid, strat, "size_frac",
          WM_TRADE_DEF_SIZE_FRAC);
  book->max_position =
      wm_strategy_kv_get_dbl(mid, strat, "max_position",
          WM_TRADE_DEF_MAX_POSITION);

  // starting_cash + mode resolve only on first seed (mode == OFF +
  // starting_cash == 0 means "fresh book"); after that they are owned
  // by the runtime.
  if(book->starting_cash == 0.0)
  {
    book->starting_cash =
        wm_strategy_kv_get_dbl(mid, strat, "starting_cash",
            WM_TRADE_DEF_STARTING_CASH);

    if(book->cash == 0.0)
      book->cash = book->starting_cash;
  }

  if(book->mode == WM_TRADE_MODE_OFF)
  {
    mode_str = wm_strategy_kv_get_str(mid, strat, "mode", "off");

    if(wm_trade_mode_parse(mode_str, &parsed_mode) == SUCCESS)
      book->mode = parsed_mode;
  }
}

void
wm_trade_book_refresh_kv(wm_trade_book_t *book)
{
  wm_trade_registry_t *reg;

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return;

  pthread_mutex_lock(&reg->lock);
  wm_trade_book_refresh_kv_locked(book);
  pthread_mutex_unlock(&reg->lock);
}

static wm_trade_book_t *
wm_trade_book_create_locked(wm_trade_registry_t *reg,
    const char *market_id_str, const char *strategy_name)
{
  wm_trade_book_t *book;

  book = mem_alloc("whenmoon", "trade_book", sizeof(*book));

  if(book == NULL)
    return(NULL);

  memset(book, 0, sizeof(*book));

  snprintf(book->market_id_str, sizeof(book->market_id_str),
      "%s", market_id_str);
  snprintf(book->strategy_name, sizeof(book->strategy_name),
      "%s", strategy_name);

  book->mode             = WM_TRADE_MODE_OFF;
  book->position.side    = WM_POS_FLAT;

  book->pnl = wm_pnl_acc_create(0);

  if(book->pnl == NULL)
  {
    mem_free(book);
    return(NULL);
  }

  // Pull initial param values + starting_cash + KV-default mode.
  wm_trade_book_refresh_kv_locked(book);

  // Link.
  book->next  = reg->head;
  reg->head   = book;
  reg->n_books++;

  clam(CLAM_INFO, WM_TRADE_CTX,
      "book created: %s/%s mode=%s cash=%.2f size_frac=%.4f"
      " fee_bps=%.2f slip_bps=%.2f",
      market_id_str, strategy_name,
      wm_trade_mode_name(book->mode),
      book->cash, book->size_frac, book->fee_bps, book->slip_bps);

  return(book);
}

wm_trade_book_t *
wm_trade_book_get_or_create(const char *market_id_str,
    const char *strategy_name)
{
  wm_trade_registry_t *reg;
  wm_trade_book_t     *b;

  if(market_id_str == NULL || strategy_name == NULL)
    return(NULL);

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return(NULL);

  pthread_mutex_lock(&reg->lock);

  b = wm_trade_book_find(reg, market_id_str, strategy_name);

  if(b == NULL)
    b = wm_trade_book_create_locked(reg, market_id_str, strategy_name);

  pthread_mutex_unlock(&reg->lock);

  return(b);
}

// ----------------------------------------------------------------------- //
// Book removal                                                            //
// ----------------------------------------------------------------------- //

void
wm_trade_book_remove(const char *market_id_str, const char *strategy_name)
{
  wm_trade_registry_t  *reg;
  wm_trade_book_t     **pp;
  wm_trade_book_t      *target = NULL;

  if(market_id_str == NULL || strategy_name == NULL)
    return;

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return;

  pthread_mutex_lock(&reg->lock);

  pp = &reg->head;

  while(*pp != NULL)
  {
    if(strncmp((*pp)->market_id_str, market_id_str,
           sizeof((*pp)->market_id_str)) == 0 &&
       strncmp((*pp)->strategy_name, strategy_name,
           sizeof((*pp)->strategy_name)) == 0)
    {
      target = *pp;
      *pp    = target->next;
      reg->n_books--;
      break;
    }

    pp = &(*pp)->next;
  }

  if(target != NULL)
    wm_trade_book_free_locked(target);

  pthread_mutex_unlock(&reg->lock);

  if(target != NULL)
    clam(CLAM_INFO, WM_TRADE_CTX,
        "book removed: %s/%s", market_id_str, strategy_name);
}

uint32_t
wm_trade_books_remove_market(const char *market_id_str)
{
  wm_trade_registry_t  *reg;
  wm_trade_book_t     **pp;
  uint32_t              n_removed = 0;

  if(market_id_str == NULL)
    return(0);

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return(0);

  pthread_mutex_lock(&reg->lock);

  pp = &reg->head;

  while(*pp != NULL)
  {
    wm_trade_book_t *cur = *pp;

    if(strncmp(cur->market_id_str, market_id_str,
           sizeof(cur->market_id_str)) == 0)
    {
      *pp = cur->next;
      reg->n_books--;
      wm_trade_book_free_locked(cur);
      n_removed++;
      continue;
    }

    pp = &cur->next;
  }

  pthread_mutex_unlock(&reg->lock);

  if(n_removed > 0)
    clam(CLAM_INFO, WM_TRADE_CTX,
        "removed %u trade book(s) on market: %s",
        n_removed, market_id_str);

  return(n_removed);
}

// ----------------------------------------------------------------------- //
// Mode + reset                                                            //
// ----------------------------------------------------------------------- //

bool
wm_trade_book_set_mode(const char *market_id_str,
    const char *strategy_name, wm_trade_mode_t mode)
{
  wm_trade_registry_t *reg;
  wm_trade_book_t     *b;
  wm_trade_mode_t      prev;

  if(mode != WM_TRADE_MODE_OFF && mode != WM_TRADE_MODE_PAPER &&
     mode != WM_TRADE_MODE_BACKTEST && mode != WM_TRADE_MODE_LIVE)
    return(FAIL);

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return(FAIL);

  pthread_mutex_lock(&reg->lock);

  b = wm_trade_book_find(reg, market_id_str, strategy_name);

  if(b == NULL)
    b = wm_trade_book_create_locked(reg, market_id_str, strategy_name);

  if(b == NULL)
  {
    pthread_mutex_unlock(&reg->lock);
    return(FAIL);
  }

  prev    = b->mode;
  b->mode = mode;

  pthread_mutex_unlock(&reg->lock);

  clam(CLAM_INFO, WM_TRADE_CTX,
      "%s/%s mode %s -> %s",
      market_id_str, strategy_name,
      wm_trade_mode_name(prev), wm_trade_mode_name(mode));

  return(SUCCESS);
}

void
wm_trade_book_override_params(const char *market_id_str,
    const char *strategy_name,
    bool have_fee_bps,       double fee_bps,
    bool have_slip_bps,      double slip_bps,
    bool have_size_frac,     double size_frac,
    bool have_starting_cash, double starting_cash)
{
  wm_trade_registry_t *reg;
  wm_trade_book_t     *b;

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return;

  pthread_mutex_lock(&reg->lock);

  b = wm_trade_book_find(reg, market_id_str, strategy_name);

  if(b != NULL)
  {
    if(have_fee_bps)        b->fee_bps   = fee_bps;
    if(have_slip_bps)       b->slip_bps  = slip_bps;
    if(have_size_frac)      b->size_frac = size_frac;

    if(have_starting_cash)
    {
      b->starting_cash = starting_cash;
      b->cash          = starting_cash;
    }

    clam(CLAM_INFO, WM_TRADE_CTX,
        "%s/%s override: fee=%.2f slip=%.2f size_frac=%.4f cash=%.2f",
        market_id_str, strategy_name,
        b->fee_bps, b->slip_bps, b->size_frac, b->cash);
  }

  pthread_mutex_unlock(&reg->lock);
}

void
wm_trade_book_reset(const char *market_id_str, const char *strategy_name)
{
  wm_trade_registry_t *reg;
  wm_trade_book_t     *b;

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return;

  pthread_mutex_lock(&reg->lock);

  b = wm_trade_book_find(reg, market_id_str, strategy_name);

  if(b != NULL)
  {
    b->cash             = b->starting_cash;
    b->position.side    = WM_POS_FLAT;
    b->position.qty     = 0.0;
    b->position.avg_entry_px = 0.0;
    b->position.opened_at_ms = 0;
    b->fill_n           = 0;
    b->fill_head        = 0;
    b->has_last_signal  = false;
    memset(&b->last_signal, 0, sizeof(b->last_signal));
    memset(b->fills, 0, sizeof(b->fills));

    if(b->pnl != NULL)
      wm_pnl_acc_reset(b->pnl);

    // Re-pull cached params in case the user adjusted KV between
    // attach and reset.
    wm_trade_book_refresh_kv_locked(b);

    clam(CLAM_INFO, WM_TRADE_CTX,
        "%s/%s reset (cash=%.2f)",
        market_id_str, strategy_name, b->cash);
  }

  pthread_mutex_unlock(&reg->lock);
}

// ----------------------------------------------------------------------- //
// Fill engine (paper)                                                     //
// ----------------------------------------------------------------------- //

// Apply a synthetic fill to the book under the registry lock. Updates
// position (open / scale / partial close / full close / flip),
// cash, fees, ring, and PnL accumulators.
//
// `intent` is BUY or SELL with qty > 0; `mark_px` is the pre-slippage
// mark; ts_ms is the originating signal's timestamp.
//
// Slippage convention: a buy pays `mark + slip`, a sell receives
// `mark - slip`. Fee is taken on notional = qty * fill_px and paid
// out of cash.
static void
wm_trade_apply_paper_fill_locked(wm_trade_book_t *b,
    const wm_sizer_intent_t *intent, double mark_px, int64_t ts_ms,
    const wm_strategy_signal_t *sig)
{
  wm_fill_t *slot;
  double     slip;
  double     fill_px;
  double     notional;
  double     fee;
  double     signed_pos;
  double     signed_delta;
  double     close_qty;
  double     open_qty;
  double     realized;
  double     close_cost;
  double     equity;

  if(b == NULL || intent == NULL || sig == NULL)
    return;

  if(intent->action == WM_SIZER_HOLD || intent->qty <= 0.0)
    return;

  // Slippage is one-sided against the trader: positive slip on a buy
  // (pay above mark), negative slip on a sell (receive below mark).
  slip    = mark_px * (b->slip_bps / 10000.0);
  fill_px = intent->action == WM_SIZER_BUY ? mark_px + slip
                                           : mark_px - slip;
  if(fill_px <= 0.0)
    return;

  notional = intent->qty * fill_px;
  fee      = notional * (b->fee_bps / 10000.0);

  signed_pos   = (b->position.side == WM_POS_LONG)  ?  b->position.qty
               : (b->position.side == WM_POS_SHORT) ? -b->position.qty
                                                    :  0.0;
  signed_delta = intent->action == WM_SIZER_BUY ?  intent->qty
                                                : -intent->qty;

  // Decompose the fill into closing-portion + opening-portion. A flip
  // (e.g. long -> short) closes the entire existing position then
  // opens a fresh one in the opposite direction.
  close_qty = 0.0;
  open_qty  = intent->qty;
  realized  = 0.0;

  if(signed_pos != 0.0 && (signed_pos * signed_delta) < 0.0)
  {
    double existing_abs = fabs(signed_pos);

    close_qty = intent->qty <= existing_abs ? intent->qty : existing_abs;
    open_qty  = intent->qty - close_qty;

    // Realized PnL on the closed portion: (entry - exit) * qty for a
    // short close, (exit - entry) * qty for a long close. Equivalent
    // to signed_pos_sign * (fill_px - avg_entry_px) * close_qty.
    {
      double pos_sign = signed_pos > 0.0 ? 1.0 : -1.0;

      realized = pos_sign *
          (fill_px - b->position.avg_entry_px) * close_qty;
    }
  }

  // Apply cash: a buy spends notional + fee, a sell receives notional
  // - fee. Position economics are accounted in the realized PnL on
  // closes; the cash side just tracks the actual transfers.
  if(intent->action == WM_SIZER_BUY)
    b->cash -= notional;
  else
    b->cash += notional;

  b->cash -= fee;

  // Update position. Three cases:
  //   1) close_qty == intent->qty -> partial-or-full close, no open
  //   2) close_qty > 0 && open_qty > 0 -> flip, close then re-open
  //   3) close_qty == 0 -> open / scale in same direction
  if(close_qty > 0.0 && open_qty == 0.0)
  {
    // Pure close.
    if(b->position.side == WM_POS_LONG)
      b->position.qty -= close_qty;
    else
      b->position.qty -= close_qty;

    if(b->position.qty <= 1e-12)
    {
      b->position.side = WM_POS_FLAT;
      b->position.qty  = 0.0;
      b->position.avg_entry_px = 0.0;
      b->position.opened_at_ms = 0;
    }
  }
  else if(close_qty > 0.0 && open_qty > 0.0)
  {
    // Flip. Close existing, open opposite at fill_px.
    b->position.side    = intent->action == WM_SIZER_BUY ? WM_POS_LONG
                                                         : WM_POS_SHORT;
    b->position.qty     = open_qty;
    b->position.avg_entry_px = fill_px;
    b->position.opened_at_ms = ts_ms;
  }
  else
  {
    // Open or scale-in same direction. VWAP entry across the now-larger
    // position (existing notional + new notional) / new total qty.
    double new_qty;
    double new_notional;

    if(b->position.side == WM_POS_FLAT)
    {
      b->position.side    = intent->action == WM_SIZER_BUY ? WM_POS_LONG
                                                           : WM_POS_SHORT;
      b->position.qty     = open_qty;
      b->position.avg_entry_px = fill_px;
      b->position.opened_at_ms = ts_ms;
    }
    else
    {
      new_qty      = b->position.qty + open_qty;
      new_notional = b->position.qty * b->position.avg_entry_px
                   + open_qty * fill_px;
      b->position.qty          = new_qty;
      b->position.avg_entry_px = new_notional / new_qty;
      // opened_at_ms preserved on scale-in.
    }
  }

  signed_pos = (b->position.side == WM_POS_LONG)  ?  b->position.qty
             : (b->position.side == WM_POS_SHORT) ? -b->position.qty
                                                  :  0.0;

  // Append to the fills ring.
  slot = &b->fills[b->fill_head];
  memset(slot, 0, sizeof(*slot));
  slot->ts_ms          = ts_ms;
  slot->side           = intent->action == WM_SIZER_BUY ? 'b' : 's';
  slot->qty            = intent->qty;
  slot->price          = fill_px;
  slot->fee            = fee;
  slot->slippage       = (intent->action == WM_SIZER_BUY ? slip : -slip)
                       * intent->qty;
  slot->realized_pnl   = realized;
  slot->cash_after     = b->cash;
  slot->position_after = signed_pos;
  snprintf(slot->reason, sizeof(slot->reason), "%s", sig->reason);

  b->fill_head = (b->fill_head + 1) % WM_FILL_RING_CAP;
  b->fill_n++;

  // Record into PnL accumulators.
  wm_pnl_acc_record_fee(b->pnl, fee);

  if(close_qty > 0.0)
  {
    close_cost = b->position.avg_entry_px * close_qty;

    // For a flip, avg_entry_px now reflects the new opening side.
    // Fall back to the fill_px so the cost-basis ratio is sensible.
    if(close_qty > 0.0 && open_qty > 0.0)
      close_cost = mark_px * close_qty;

    wm_pnl_acc_record_close(b->pnl, realized, close_cost);
  }

  // Equity sample for drawdown.
  equity = b->cash + signed_pos * mark_px;
  wm_pnl_acc_record_equity(b->pnl, equity);

  clam(CLAM_INFO, WM_TRADE_CTX,
      "%s/%s fill: %c qty=%.6g px=%.6g fee=%.4f realized=%.4f"
      " cash=%.2f pos=%.6g (%s)",
      b->market_id_str, b->strategy_name,
      slot->side, slot->qty, slot->price, slot->fee,
      slot->realized_pnl, slot->cash_after, slot->position_after,
      sig->reason);
}

// ----------------------------------------------------------------------- //
// Signal entry point                                                      //
// ----------------------------------------------------------------------- //

void
wm_trade_engine_on_signal(const char *market_id_str,
    const char *strategy_name, double mark_px, int64_t mark_ms,
    const wm_strategy_signal_t *sig)
{
  wm_trade_registry_t *reg;
  wm_trade_book_t     *b;
  wm_sizer_intent_t    intent;
  double               equity_pre;
  double               signed_pos;

  if(sig == NULL)
    return;

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return;

  pthread_mutex_lock(&reg->lock);

  b = wm_trade_book_find(reg, market_id_str, strategy_name);

  if(b == NULL || b->mode == WM_TRADE_MODE_OFF)
  {
    pthread_mutex_unlock(&reg->lock);
    return;
  }

  // Update the mark cache + record an equity sample for drawdown
  // before any fill so the peak-tracking sees the pre-trade state.
  if(mark_px > 0.0)
  {
    b->last_mark_px = mark_px;
    b->last_mark_ms = mark_ms;

    signed_pos = (b->position.side == WM_POS_LONG)  ?  b->position.qty
               : (b->position.side == WM_POS_SHORT) ? -b->position.qty
                                                    :  0.0;
    equity_pre = b->cash + signed_pos * mark_px;
    wm_pnl_acc_record_equity(b->pnl, equity_pre);
  }

  b->last_signal     = *sig;
  b->has_last_signal = true;

  // Backtest + live are placeholders for WM-LT-5 + WM-LT-8. Fall
  // through to the no-op tail rather than firing a paper fill, so an
  // operator who set a mode that isn't wired yet sees signals
  // recorded but no fills (the safe behaviour).
  if(b->mode != WM_TRADE_MODE_PAPER)
  {
    pthread_mutex_unlock(&reg->lock);
    return;
  }

  if(mark_px <= 0.0)
  {
    pthread_mutex_unlock(&reg->lock);
    return;
  }

  wm_sizer_compute(b, mark_px, sig, &intent);

  if(intent.action != WM_SIZER_HOLD)
    wm_trade_apply_paper_fill_locked(b, &intent, mark_px, sig->ts_ms,
        sig);

  pthread_mutex_unlock(&reg->lock);
}

void
wm_trade_book_update_mark(const char *market_id_str,
    const char *strategy_name, double mark_px, int64_t mark_ms)
{
  wm_trade_registry_t *reg;
  wm_trade_book_t     *b;
  double               signed_pos;
  double               equity;

  if(mark_px <= 0.0)
    return;

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return;

  pthread_mutex_lock(&reg->lock);

  b = wm_trade_book_find(reg, market_id_str, strategy_name);

  if(b != NULL)
  {
    b->last_mark_px = mark_px;
    b->last_mark_ms = mark_ms;

    signed_pos = (b->position.side == WM_POS_LONG)  ?  b->position.qty
               : (b->position.side == WM_POS_SHORT) ? -b->position.qty
                                                    :  0.0;
    equity = b->cash + signed_pos * mark_px;

    wm_pnl_acc_record_equity(b->pnl, equity);
  }

  pthread_mutex_unlock(&reg->lock);
}

// ----------------------------------------------------------------------- //
// Snapshot                                                                //
// ----------------------------------------------------------------------- //

// Fill the snapshot from a book. Caller holds the registry lock.
static void
wm_trade_book_snapshot_locked(const wm_trade_book_t *b,
    wm_trade_snapshot_t *out)
{
  uint32_t to_copy;
  uint32_t i;
  uint32_t src_idx;
  uint32_t start;
  double   signed_pos;

  memset(out, 0, sizeof(*out));

  snprintf(out->market_id_str, sizeof(out->market_id_str),
      "%s", b->market_id_str);
  snprintf(out->strategy_name, sizeof(out->strategy_name),
      "%s", b->strategy_name);

  out->mode             = b->mode;
  out->starting_cash    = b->starting_cash;
  out->cash             = b->cash;
  out->position         = b->position;
  out->fee_bps          = b->fee_bps;
  out->slip_bps         = b->slip_bps;
  out->size_frac        = b->size_frac;
  out->max_position     = b->max_position;
  out->last_mark_px     = b->last_mark_px;
  out->last_mark_ms     = b->last_mark_ms;
  out->last_signal      = b->last_signal;
  out->has_last_signal  = b->has_last_signal;
  out->fill_total       = b->fill_n;

  wm_pnl_compute(b->pnl, &out->metrics);

  signed_pos = (b->position.side == WM_POS_LONG)  ?  b->position.qty
             : (b->position.side == WM_POS_SHORT) ? -b->position.qty
                                                  :  0.0;

  out->equity = b->cash + signed_pos * b->last_mark_px;
  out->unrealized_pnl = signed_pos *
      (b->last_mark_px - b->position.avg_entry_px);

  // Tail of the fills ring (newest last). We always emit the most
  // recent WM_TRADE_SNAPSHOT_FILLS entries, oldest of those first.
  to_copy = b->fill_n < WM_TRADE_SNAPSHOT_FILLS ? (uint32_t)b->fill_n
                                                : WM_TRADE_SNAPSHOT_FILLS;

  if(to_copy > 0)
  {
    // Reconstruct the absolute newest index. The ring's next-write
    // slot is fill_head; the newest entry is at (fill_head - 1) mod cap
    // when the ring has wrapped, or at (fill_n - 1) before wrap.
    if(b->fill_n <= WM_FILL_RING_CAP)
      start = (uint32_t)(b->fill_n) - to_copy;
    else
      start = (b->fill_head + WM_FILL_RING_CAP - to_copy)
            % WM_FILL_RING_CAP;

    for(i = 0; i < to_copy; i++)
    {
      if(b->fill_n <= WM_FILL_RING_CAP)
        src_idx = start + i;
      else
        src_idx = (start + i) % WM_FILL_RING_CAP;

      out->fills[i] = b->fills[src_idx];
    }

    out->n_fills = to_copy;
  }
}

bool
wm_trade_book_snapshot(const char *market_id_str,
    const char *strategy_name, wm_trade_snapshot_t *out)
{
  wm_trade_registry_t *reg;
  wm_trade_book_t     *b;
  bool                 ok = FAIL;

  if(out == NULL)
    return(FAIL);

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return(FAIL);

  pthread_mutex_lock(&reg->lock);

  b = wm_trade_book_find(reg, market_id_str, strategy_name);

  if(b != NULL)
  {
    wm_trade_book_snapshot_locked(b, out);
    ok = SUCCESS;
  }

  pthread_mutex_unlock(&reg->lock);

  return(ok);
}

void
wm_trade_books_iterate(wm_trade_book_iter_cb_t cb, void *user)
{
  wm_trade_registry_t *reg;
  wm_trade_book_t     *b;
  wm_trade_snapshot_t  snap;

  if(cb == NULL)
    return;

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return;

  pthread_mutex_lock(&reg->lock);

  for(b = reg->head; b != NULL; b = b->next)
  {
    wm_trade_book_snapshot_locked(b, &snap);
    cb(&snap, user);
  }

  pthread_mutex_unlock(&reg->lock);
}

uint32_t
wm_trade_books_count(void)
{
  wm_trade_registry_t *reg;
  uint32_t             n;

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return(0);

  pthread_mutex_lock(&reg->lock);
  n = reg->n_books;
  pthread_mutex_unlock(&reg->lock);

  return(n);
}

// ----------------------------------------------------------------------- //
// Reconciliation (WM-PT-2)                                                //
// ----------------------------------------------------------------------- //

// Fold one fill into the running expected_* accumulators. Used by the
// reconcile walker for both full and ring-truncated modes.
static void
wm_trade_reconcile_apply_fill(const wm_fill_t *f,
    double *expected_cash, double *expected_position,
    double *expected_fees)
{
  double signed_qty;

  signed_qty = (f->side == 'b') ?  f->qty
             : (f->side == 's') ? -f->qty
                                :  0.0;

  *expected_cash     -= signed_qty * f->price;
  *expected_cash     -= f->fee;
  *expected_position += signed_qty;
  *expected_fees     += f->fee;
}

static bool
wm_trade_reconcile_within_eps(double delta)
{
  return((delta < 0.0 ? -delta : delta) < WM_TRADE_RECONCILE_EPS);
}

// Reconcile a book against its fills ring. Caller holds the registry
// lock. Pure compute; no allocations beyond the supplied result struct.
static void
wm_trade_book_reconcile_locked(const wm_trade_book_t *b,
    wm_trade_reconcile_t *out)
{
  uint32_t i;
  uint32_t walk;
  uint32_t src_idx;
  double   signed_pos;

  memset(out, 0, sizeof(*out));

  snprintf(out->market_id_str, sizeof(out->market_id_str),
      "%s", b->market_id_str);
  snprintf(out->strategy_name, sizeof(out->strategy_name),
      "%s", b->strategy_name);

  out->fills_total     = b->fill_n;
  out->ring_truncated  = b->fill_n > WM_FILL_RING_CAP;
  out->fees_reconciled = !out->ring_truncated;

  // Live state captured under the lock.
  signed_pos = (b->position.side == WM_POS_LONG)  ?  b->position.qty
             : (b->position.side == WM_POS_SHORT) ? -b->position.qty
                                                  :  0.0;

  out->actual_cash     = b->cash;
  out->actual_position = signed_pos;
  out->actual_fees     = b->pnl != NULL ? b->pnl->fees_paid : 0.0;

  if(!out->ring_truncated)
  {
    // Full walk: every lifetime fill is in the ring, so we start from
    // starting_cash + flat position and apply every fill in order.
    out->expected_cash     = b->starting_cash;
    out->expected_position = 0.0;
    out->expected_fees     = 0.0;

    walk = (uint32_t)b->fill_n;

    for(i = 0; i < walk; i++)
      wm_trade_reconcile_apply_fill(&b->fills[i],
          &out->expected_cash, &out->expected_position,
          &out->expected_fees);

    out->fills_walked = walk;
  }
  else
  {
    // Ring-truncated walk. The ring's oldest live fill sits at the
    // next-write slot (fill_head); everything older was overwritten.
    // Anchor at that fill's (cash_after, position_after) — the state
    // *just after* the oldest fill executed — and replay the cap-1
    // fills that follow it. expected_fees can't be reconstructed from
    // a partial walk against the lifetime fees_paid accumulator, so
    // the fee delta is left unreconciled.
    const wm_fill_t *anchor = &b->fills[b->fill_head];

    out->expected_cash     = anchor->cash_after;
    out->expected_position = anchor->position_after;
    out->expected_fees     = 0.0;

    for(i = 1; i < WM_FILL_RING_CAP; i++)
    {
      src_idx = (b->fill_head + i) % WM_FILL_RING_CAP;
      wm_trade_reconcile_apply_fill(&b->fills[src_idx],
          &out->expected_cash, &out->expected_position,
          &out->expected_fees);
    }

    out->fills_walked = WM_FILL_RING_CAP;
  }

  out->cash_delta     = out->actual_cash     - out->expected_cash;
  out->position_delta = out->actual_position - out->expected_position;
  out->fee_delta      = out->fees_reconciled
      ? out->actual_fees - out->expected_fees : 0.0;

  out->ok = wm_trade_reconcile_within_eps(out->cash_delta) &&
            wm_trade_reconcile_within_eps(out->position_delta) &&
            (!out->fees_reconciled ||
                wm_trade_reconcile_within_eps(out->fee_delta));
}

bool
wm_trade_book_reconcile(const char *market_id_str,
    const char *strategy_name, wm_trade_reconcile_t *out)
{
  wm_trade_registry_t *reg;
  wm_trade_book_t     *b;
  bool                 ok = FAIL;

  if(out == NULL)
    return(FAIL);

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return(FAIL);

  pthread_mutex_lock(&reg->lock);

  b = wm_trade_book_find(reg, market_id_str, strategy_name);

  if(b != NULL)
  {
    wm_trade_book_reconcile_locked(b, out);
    ok = SUCCESS;
  }

  pthread_mutex_unlock(&reg->lock);

  return(ok);
}
