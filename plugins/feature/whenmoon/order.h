// order.h — per-(market, strategy) trade book + paper-mode fill engine.
//
// WM-LT-4 substrate. The trade book is the bookkeeping hub for one
// strategy attached to one market: cash, position, recent fills ring,
// PnL accumulators, and the cached sizer/fee/slippage parameters
// resolved from the two-tier strategy KV.
//
// Modes (per book):
//   - WM_TRADE_MODE_OFF       — record signal, no order side effects
//   - WM_TRADE_MODE_PAPER     — synthetic fill against the cached mark
//                               px + slippage + fee; cash + position
//                               updated in-memory, fill appended to
//                               the ring
//   - WM_TRADE_MODE_BACKTEST  — placeholder; WM-LT-5 wires the snapshot
//                               replay loop through this branch
//   - WM_TRADE_MODE_LIVE      — placeholder; WM-LT-8 routes here through
//                               exchange_request(EXCHANGE_PRIO_TRANSACTIONAL)
//
// Locking discipline:
//   - One mutex on the trade-book registry. All book reads + writes
//     happen under it. Per-book locks were considered but rejected:
//     /show paths need the registry lock to find a book anyway, and
//     the signal-handling work (sizer + fill arithmetic) is tiny.
//   - Lock order across the plugin:
//       market_lock -> strategy_registry_lock -> trade_registry_lock
//   - wm_trade_engine_on_signal is invoked from
//     wm_strategy_emit_signal_impl, which runs inside the strategy
//     registry lock (held by dispatch_bar / dispatch_trade).
//   - Books are removed by wm_trade_book_remove during detach, which
//     never re-enters the strategy registry.
//
// Internal to the whenmoon plugin. WHENMOON_INTERNAL gated.

#ifndef BM_WHENMOON_ORDER_H
#define BM_WHENMOON_ORDER_H

#ifdef WHENMOON_INTERNAL

#include "market.h"
#include "pnl.h"
#include "whenmoon_strategy.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

// ----------------------------------------------------------------------- //
// Trade mode                                                              //
// ----------------------------------------------------------------------- //

typedef enum
{
  WM_TRADE_MODE_OFF      = 0,
  WM_TRADE_MODE_PAPER    = 1,
  WM_TRADE_MODE_BACKTEST = 2,
  WM_TRADE_MODE_LIVE     = 3,
} wm_trade_mode_t;

// Token <-> enum helpers. Parser is case-insensitive; printer returns a
// pointer to a static string ("off" / "paper" / "backtest" / "live").
bool        wm_trade_mode_parse(const char *tok, wm_trade_mode_t *out);
const char *wm_trade_mode_name(wm_trade_mode_t m);

// ----------------------------------------------------------------------- //
// Position                                                                //
// ----------------------------------------------------------------------- //

typedef enum
{
  WM_POS_FLAT  = 0,
  WM_POS_LONG  = 1,
  WM_POS_SHORT = 2,
} wm_position_side_t;

typedef struct wm_position
{
  wm_position_side_t side;
  double             qty;             // base units, always >= 0
  double             avg_entry_px;    // VWAP across opening fills
  int64_t            opened_at_ms;    // ts of the most recent open
} wm_position_t;

// ----------------------------------------------------------------------- //
// Fill                                                                    //
// ----------------------------------------------------------------------- //

typedef struct wm_fill
{
  int64_t  ts_ms;
  char     side;                              // 'b' (buy) / 's' (sell)
  double   qty;                               // base units, > 0
  double   price;                             // executed px (post-slip)
  double   fee;                               // quote-ccy fee
  double   slippage;                          // signed quote-ccy cost
  double   realized_pnl;                      // closing-portion PnL
  double   cash_after;                        // book cash after this fill
  double   position_after;                    // signed position after fill
  char     reason[WM_STRATEGY_REASON_SZ];     // copied from sizer intent
} wm_fill_t;

#define WM_FILL_RING_CAP   256

// ----------------------------------------------------------------------- //
// Trade book                                                              //
// ----------------------------------------------------------------------- //

typedef struct wm_trade_book
{
  // Identity (immutable post-create).
  char                  market_id_str[WM_MARKET_ID_STR_SZ];
  char                  strategy_name[WM_STRATEGY_NAME_SZ];

  // Mode + economics.
  wm_trade_mode_t       mode;
  double                starting_cash;
  double                cash;
  wm_position_t         position;

  // Cached params resolved from the strategy KV at refresh time.
  double                fee_bps;
  double                slip_bps;
  double                size_frac;
  double                max_position;

  // Mark cache. Updated either by the dispatch ctx mark (bar close /
  // trade tick) or by an explicit /whenmoon trade refresh.
  double                last_mark_px;
  int64_t               last_mark_ms;

  // Last signal we acted on. Recorded for /show whenmoon trade.
  wm_strategy_signal_t  last_signal;
  bool                  has_last_signal;

  // Recent fills ring. Indexed [0..WM_FILL_RING_CAP); fill_n counts
  // the lifetime total, fill_head is the next write slot.
  wm_fill_t             fills[WM_FILL_RING_CAP];
  uint64_t              fill_n;
  uint32_t              fill_head;

  // PnL accumulators. Owned; allocated at create.
  wm_pnl_acc_t         *pnl;

  // Linkage in the registry list.
  struct wm_trade_book *next;
} wm_trade_book_t;

// ----------------------------------------------------------------------- //
// Trade-book registry                                                     //
// ----------------------------------------------------------------------- //
//
// The trade engine owns a process-global registry — every book created by
// the live + paper paths lives here, serialised on a single mutex. Any
// book op (create, signal, snapshot) takes that mutex.
//
// WM-LT-6 promotes the registry struct to a public type so the backtest
// + sweep planner can allocate private registries and pin them to a
// worker thread via a pthread_key. With one private registry per worker
// thread, parallel sweep iterations see no cross-thread contention on
// the global mutex; live + paper traffic still routes to the global
// registry as before.
//
// Routing is implicit. Every public book API reads the calling thread's
// TLS slot (see wm_trade_engine_use_registry below); when the slot is
// NULL it falls back to the global registry. Strategy-callback emission
// rides the same TLS read inside wm_trade_engine_on_signal, so a sweep
// iteration that fires emit_signal lands on the worker's private book.

typedef struct wm_trade_registry
{
  pthread_mutex_t   lock;
  wm_trade_book_t  *head;
  uint32_t          n_books;
  bool              initialized;
} wm_trade_registry_t;

// Allocate + initialise a private registry. The returned pointer is
// owned by the caller; it is NOT linked into any global list. Pass it
// to wm_trade_engine_use_registry on the worker thread that will
// dispatch through it. Returns NULL on OOM or if the engine has not
// been initialised.
wm_trade_registry_t *wm_trade_registry_create(void);

// Drop every book held in the registry, destroy its mutex, and free
// the registry struct. Safe to call NULL (no-op). Caller must ensure
// no thread still has this registry bound via use_registry.
void wm_trade_registry_destroy(wm_trade_registry_t *r);

// Bind `r` for the calling thread. Subsequent book + signal calls on
// this thread route to `r` instead of the global registry. Pass NULL
// to restore default (global) routing. Idempotent within a thread.
//
// The TLS slot is per-thread; one worker can pin its own private
// registry without affecting any other thread.
void wm_trade_engine_use_registry(wm_trade_registry_t *r);

// ----------------------------------------------------------------------- //
// Engine lifecycle                                                        //
// ----------------------------------------------------------------------- //

bool wm_trade_engine_init(void);
void wm_trade_engine_destroy(void);

// ----------------------------------------------------------------------- //
// Book lookup / lifecycle                                                 //
// ----------------------------------------------------------------------- //

// Find an existing book in `reg`. Returns NULL if absent. Caller MUST
// hold reg->lock and MUST NOT free the returned pointer; the registry
// owns it. Pointer is stable until wm_trade_book_remove fires for the
// same (market, strategy).
//
// Most callers should use the higher-level on-signal / snapshot APIs
// below, which resolve the active registry + take the lock internally.
wm_trade_book_t *wm_trade_book_find(wm_trade_registry_t *reg,
    const char *market_id_str, const char *strategy_name);

// Find or create. On miss, allocates a fresh book in mode OFF with
// cash = starting_cash (resolved from KV) and a flat position. Returns
// NULL on OOM.
wm_trade_book_t *wm_trade_book_get_or_create(const char *market_id_str,
    const char *strategy_name);

// Detach: drops the book from the registry, fires its finalize
// (frees pnl + the book itself). No-op when absent.
void wm_trade_book_remove(const char *market_id_str,
    const char *strategy_name);

// Detach every book whose market_id_str matches. Mirrors the
// wm_strategy_detach_market hook so a market stop tears down the
// books too. Returns the count detached.
uint32_t wm_trade_books_remove_market(const char *market_id_str);

// ----------------------------------------------------------------------- //
// Book operations                                                         //
// ----------------------------------------------------------------------- //

// Refresh cached parameters (mode, sizer, fee/slip, max_position,
// starting_cash) from the strategy KV. Called at attach time and again
// after /whenmoon trade reset to pick up edited values.
//
// Mode is NOT mutated by this call; mode lives on the book and is
// changed only via wm_trade_book_set_mode. The KV slot for mode is
// the *initial* value applied at create time.
void wm_trade_book_refresh_kv(wm_trade_book_t *book);

// Set the mode. Returns SUCCESS on a recognized value; FAIL otherwise.
// Logs a CLAM_INFO transition message.
bool wm_trade_book_set_mode(const char *market_id_str,
    const char *strategy_name, wm_trade_mode_t mode);

// Reset: flatten position, restore cash to starting_cash, clear PnL
// accumulators, clear fills ring, drop last_signal. Mode preserved.
void wm_trade_book_reset(const char *market_id_str,
    const char *strategy_name);

// WM-LT-5: override the cached economics/sizing params on a book
// outside the normal refresh-from-KV path. Used by the backtest CLI
// to apply --fee-bps / --slip-bps / etc. overrides without polluting
// the global KV. Each `have_*` flag selects whether the matching
// double is applied. starting_cash overrides also reset cash to the
// new starting value (so the fresh book begins from the override).
// No-op when the book does not exist.
void wm_trade_book_override_params(const char *market_id_str,
    const char *strategy_name,
    bool have_fee_bps,       double fee_bps,
    bool have_slip_bps,      double slip_bps,
    bool have_size_frac,     double size_frac,
    bool have_starting_cash, double starting_cash);

// ----------------------------------------------------------------------- //
// Signal entry point                                                      //
// ----------------------------------------------------------------------- //

// Called from wm_strategy_emit_signal_impl. Looks up the book; if mode
// != OFF and a sizer intent fires, generates a fill. mark_px is the
// dispatch-ctx-cached price (bar->close on a bar-close path,
// trade->price on a trade-tick path). mark_ms is the matching ts.
//
// No-op when the book does not exist (the strategy is attached but no
// trade verb has ever been issued for it), when mode == OFF, or when
// the sizer says hold.
void wm_trade_engine_on_signal(const char *market_id_str,
    const char *strategy_name, double mark_px, int64_t mark_ms,
    const wm_strategy_signal_t *sig);

// Mark refresh path used by /show whenmoon trade so the rendered
// unrealized PnL line reflects the live ticker rather than the last
// signal-time mark. Caller passes the current mark; book updates only
// the cached fields, no fill engine entry.
void wm_trade_book_update_mark(const char *market_id_str,
    const char *strategy_name, double mark_px, int64_t mark_ms);

// ----------------------------------------------------------------------- //
// Snapshot (for /show)                                                    //
// ----------------------------------------------------------------------- //

#define WM_TRADE_SNAPSHOT_FILLS  16

typedef struct wm_trade_snapshot
{
  char                 market_id_str[WM_MARKET_ID_STR_SZ];
  char                 strategy_name[WM_STRATEGY_NAME_SZ];
  wm_trade_mode_t      mode;

  double               starting_cash;
  double               cash;
  wm_position_t        position;

  double               fee_bps;
  double               slip_bps;
  double               size_frac;
  double               max_position;

  double               last_mark_px;
  int64_t              last_mark_ms;

  wm_strategy_signal_t last_signal;
  bool                 has_last_signal;

  wm_pnl_metrics_t     metrics;
  double               unrealized_pnl;
  double               equity;            // cash + position*mark

  // Tail of the fills ring (newest last). n_fills is how many slots
  // are populated, capped at WM_TRADE_SNAPSHOT_FILLS.
  wm_fill_t            fills[WM_TRADE_SNAPSHOT_FILLS];
  uint32_t             n_fills;
  uint64_t             fill_total;        // lifetime fills count
} wm_trade_snapshot_t;

// Snapshot one book. Returns SUCCESS when the book exists; FAIL
// otherwise (out is left untouched on FAIL).
bool wm_trade_book_snapshot(const char *market_id_str,
    const char *strategy_name, wm_trade_snapshot_t *out);

// Iterate every registered book. The callback receives one snapshot
// per call; storage is on the iteration stack. Iteration runs under
// the registry lock; the callback MUST NOT call back into the engine.
typedef void (*wm_trade_book_iter_cb_t)(const wm_trade_snapshot_t *snap,
    void *user);

void wm_trade_books_iterate(wm_trade_book_iter_cb_t cb, void *user);

// Count of registered books.
uint32_t wm_trade_books_count(void);

// ----------------------------------------------------------------------- //
// Reconciliation (WM-PT-2)                                                //
// ----------------------------------------------------------------------- //
//
// Walk the fills ring + book state and verify the book's cash + position
// + fees match the math derived from first principles over the fills.
// Read-only; runs entirely under the registry lock with no allocations
// beyond the supplied result struct.
//
// When fill_n <= WM_FILL_RING_CAP, every lifetime fill is in the ring,
// so the walk starts from starting_cash + flat position and produces a
// full reconciliation. When fill_n > WM_FILL_RING_CAP, the oldest fills
// have been overwritten; the walk anchors at the ring's oldest live
// fill's (cash_after, position_after) and replays the cap-1 fills that
// follow. expected_fees is not reconstructable from the ring alone in
// this mode and is reported as not-reconciled.

#define WM_TRADE_RECONCILE_EPS  1e-6

typedef struct wm_trade_reconcile
{
  char     market_id_str[WM_MARKET_ID_STR_SZ];
  char     strategy_name[WM_STRATEGY_NAME_SZ];

  // Math derived from walking the fills ring oldest-first.
  double   expected_cash;
  double   expected_position;     // signed (long > 0, short < 0)
  double   expected_fees;

  // Live state read from the book at lock-held time.
  double   actual_cash;
  double   actual_position;       // signed
  double   actual_fees;

  // Deltas (actual - expected). status is OK iff every reconciled
  // delta has |x| < WM_TRADE_RECONCILE_EPS. In ring-truncated mode
  // fee_delta is not reconciled (set to 0 with fees_reconciled=false).
  double   cash_delta;
  double   position_delta;
  double   fee_delta;

  uint32_t fills_walked;          // entries walked in the ring
  uint64_t fills_total;           // book->fill_n at lock time
  bool     ring_truncated;        // true when fill_n > WM_FILL_RING_CAP
  bool     fees_reconciled;       // false in ring-truncated mode
  bool     ok;                    // every reconciled delta within EPS
} wm_trade_reconcile_t;

// Reconcile one book. Returns SUCCESS when the book exists; FAIL
// otherwise (out is left untouched on FAIL).
bool wm_trade_book_reconcile(const char *market_id_str,
    const char *strategy_name, wm_trade_reconcile_t *out);

// Verb registration (called from whenmoon_init). Registers the
// /whenmoon trade {mode, reset} and /show whenmoon trade verbs. The
// implementations live in trade_cmds.c.
bool wm_trade_register_verbs(void);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_ORDER_H
