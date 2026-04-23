// account.h — per-bot coinbase account (balance) snapshot.
//
// Internal. WHENMOON_INTERNAL gated.

#ifndef BM_WHENMOON_ACCOUNT_H
#define BM_WHENMOON_ACCOUNT_H

#ifdef WHENMOON_INTERNAL

#include "coinbase_api.h"
#include "task.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// Balance-row cap. coinbase_accounts_result_t already caps at 64 rows;
// we mirror that capacity so a snapshot copy is in-struct and the show
// verb does not need to chase a heap buffer.
#define WM_ACCOUNT_ROW_CAP   64

struct whenmoon_account
{
  coinbase_account_t    rows[WM_ACCOUNT_ROW_CAP];
  uint32_t              n_rows;
  time_t                last_refresh_ts;
  char                  last_err[128];   // empty on success

  // Periodic task handle. NULL when credentials are not configured and
  // the account feed is disabled; set when the periodic task has been
  // submitted.
  task_t               *refresh_task;

  pthread_mutex_t       lock;
};

struct whenmoon_state;

// Init: allocates the struct, reads refresh cadence from KV. If the
// coinbase plugin has no apikey configured, logs a one-shot CLAM_INFO
// and returns SUCCESS without scheduling the periodic task. SUCCESS
// otherwise; FAIL only on allocation failure.
bool wm_account_init(struct whenmoon_state *st);

// Destroy: clears task pointer (task cancels itself on next tick when
// it observes the state), destroys the mutex, frees the struct. Safe on
// a state whose account pointer is NULL.
void wm_account_destroy(struct whenmoon_state *st);

// Async callback invoked by coinbase on accounts fetch completion.
// `user` is the whenmoon_state_t*.
void wm_account_on_accounts(const coinbase_accounts_result_t *res,
    void *user);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_ACCOUNT_H
