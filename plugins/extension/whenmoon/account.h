// account.h — per-exchange account (balance) snapshot cache.
//
// Hybrid balance model (restored + enhanced from the WM-BAL-ONDEMAND-1
// rip). A per-exchange balance cache that is:
//   - refreshed on a scheduled poll (default 300s, per-exchange KV
//     override), gated on real-mode + credentials so paper/manual
//     accounts generate zero background authenticated traffic
//     (WM-PAPER-GATE-1);
//   - fast-forwarded by real fills (wm_account_fast_forward, called from
//     the real-fill funnel) so a trade's effect on cash shows up
//     immediately instead of waiting for the next tick;
//   - read instantly by `/show whenmoon balances` with a vintage line.
//
// A cache miss (e.g. an all-paper deploy that the gate never polls)
// falls back to a one-shot blocking on-demand fetch in whenmoon.c, which
// then populates the cache via wm_account_store_snapshot — so there is
// no paper-mode "empty cache" regression.
//
// Internal. WHENMOON_INTERNAL gated.

#ifndef BM_WHENMOON_ACCOUNT_H
#define BM_WHENMOON_ACCOUNT_H

#ifdef WHENMOON_INTERNAL

#include "exchange_api.h"
#include "task.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

// Balance-row cap. exchange_accounts_result_t already caps at 64 rows;
// we mirror that capacity so a snapshot copy is in-struct and the show
// verb does not need to chase a heap buffer.
#define WM_ACCOUNT_ROW_CAP        64

// Compile-time ceiling on the per-exchange slot list. Sized comfortably
// above any plausible exchange-plugin load (coinbase + kraken + gemini).
#define WM_ACCOUNT_MAX_EXCHANGES   8

// Per-exchange account slot. One per registered exchange (regardless of
// mode — a slot always exists so the on-demand fallback can store into
// it). Each slot's snapshot updates independently; the periodic task is
// per-slot so a slow gateway on exchange A does not stall exchange B.
typedef struct wm_account_slot
{
  char                  exchange_name[EXCHANGE_NAME_SZ];

  exchange_account_t    rows[WM_ACCOUNT_ROW_CAP];
  uint32_t              n_rows;

  // Monotonic ms (wm_dl_now_ms()) of the last successful snapshot;
  // 0 = never fetched. Monotonic because the vintage is elapsed-time
  // math, not a wall-clock timestamp (memory wm_dl_now_ms_is_monotonic).
  int64_t               last_refresh_mono_ms;
  char                  last_err[128];        // empty on success

  // Per-slot periodic. TASK_HANDLE_NONE when slot init failed to
  // schedule. Cancelled in wm_account_stop — see there for why the
  // cancel cannot live in wm_account_destroy.
  task_handle_t         refresh_task;

  pthread_mutex_t       lock;
} wm_account_slot_t;

struct whenmoon_account
{
  wm_account_slot_t     slots[WM_ACCOUNT_MAX_EXCHANGES];
  uint32_t              n_slots;
};

struct whenmoon_state;

// Init: allocates the (empty) container. The per-exchange slot list is
// populated by wm_account_start once the exchange registry is non-empty
// (post coinbase/kraken/gemini plugin start). SUCCESS unless allocation
// fails.
bool wm_account_init(struct whenmoon_state *st);

// Start: enumerate registered exchanges, allocate one slot per exchange,
// schedule the periodic refresh, kick an initial fetch where a real-mode
// market + credentials are present. Idempotent — re-entry is a no-op
// once slots[] is non-empty. SUCCESS even with zero registered
// exchanges.
bool wm_account_start(struct whenmoon_state *st);

// Stop: cancel every per-slot periodic. Belongs in whenmoon_stop, not
// in wm_account_destroy: task_cancel does not join, so a cancel issued
// beside the free below leaves a tick that started in between reading a
// slot that is already gone. Core's pre-deinit quiescence barrier is
// what closes that window, and it only runs between stop() and
// deinit() (OBS-34, OBS-44). Idempotent; safe on a NULL account.
void wm_account_stop(struct whenmoon_state *st);

// Destroy: destroys mutexes, frees the container. Safe on a state whose
// account pointer is NULL.
void wm_account_destroy(struct whenmoon_state *st);

// Async callback invoked by feature_exchange on accounts fetch
// completion. `user` is the heap-owned refresh ctx the refresher
// allocates; the callback frees it.
void wm_account_on_accounts(const exchange_accounts_result_t *res,
    void *user);

// Fire an immediate async balance refresh for one exchange, bypassing
// the periodic schedule. Re-checks the real-mode + creds gate, so a
// paper-only exchange is a no-op. Best-effort, never blocks. Called from
// the real-fill funnel (wm_market_engine_record_external_fill) so a real
// trade fast-forwards the next poll.
void wm_account_fast_forward(const char *exchange_name);

// Copy the cached snapshot for `exchange_name` into the caller's buffer.
// Returns true iff a snapshot exists (slot present and fetched at least
// once); on true, *n_out holds the row count (<= cap) and *age_ms_out
// the snapshot age in ms. On false nothing is written. Used by the
// cache-first path in `/show whenmoon balances`.
bool wm_account_get_snapshot(const char *exchange_name,
    exchange_account_t *rows_out, uint32_t cap, uint32_t *n_out,
    int64_t *age_ms_out);

// Store a freshly-fetched snapshot (from the on-demand blocking fallback
// in whenmoon.c) into the matching slot and stamp last_refresh_mono_ms,
// so the next /show reads it instantly with an accurate vintage. No-op
// if no slot matches `exchange_name`.
void wm_account_store_snapshot(const char *exchange_name,
    const exchange_account_t *rows, uint32_t n);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_ACCOUNT_H
