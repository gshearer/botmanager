// account.h — per-bot per-exchange account (balance) snapshot.
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
#include <time.h>

// Balance-row cap. exchange_accounts_result_t already caps at 64 rows;
// we mirror that capacity so a snapshot copy is in-struct and the show
// verb does not need to chase a heap buffer.
#define WM_ACCOUNT_ROW_CAP        64

// Compile-time ceiling on the per-exchange slot list. Sized comfortably
// above any plausible exchange-plugin load (coinbase + kraken today).
#define WM_ACCOUNT_MAX_EXCHANGES   8

// Per-exchange account slot. One per registered exchange that the
// account refresher iterates. Each slot's snapshot updates
// independently; the periodic task is per-slot so a slow gateway on
// exchange A does not stall exchange B.
typedef struct wm_account_slot
{
  char                  exchange_name[EXCHANGE_NAME_SZ];

  exchange_account_t    rows[WM_ACCOUNT_ROW_CAP];
  uint32_t              n_rows;
  time_t                last_refresh_ts;
  char                  last_err[128];        // empty on success

  // Per-slot periodic. TASK_HANDLE_NONE when slot init failed to
  // schedule. Cancelled synchronously in wm_account_destroy so no stale
  // tick fires after free.
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
// populated by wm_account_start once the exchange registry is non-
// empty (post coinbase/kraken plugin start). SUCCESS unless allocation
// fails.
bool wm_account_init(struct whenmoon_state *st);

// Start: enumerate registered exchanges, allocate one slot per
// exchange, schedule the periodic refresh, kick an initial fetch where
// credentials are present. Idempotent — re-entry is a no-op once
// slots[] is non-empty. SUCCESS even with zero registered exchanges.
bool wm_account_start(struct whenmoon_state *st);

// Destroy: cancels every per-slot task synchronously, destroys
// mutexes, frees the container. Safe on a state whose account pointer
// is NULL.
void wm_account_destroy(struct whenmoon_state *st);

// Async callback invoked by feature_exchange on accounts fetch
// completion. `user` is the heap-owned refresh ctx the refresher
// allocates; the callback frees it.
void wm_account_on_accounts(const exchange_accounts_result_t *res,
    void *user);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_ACCOUNT_H
