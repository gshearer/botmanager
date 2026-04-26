// botmanager — MIT
// whenmoon per-bot coinbase account (balance) snapshot refresher.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "account.h"

#include "kv.h"
#include "task.h"

#include <string.h>

#define WM_ACCOUNT_DEFAULT_REFRESH_SECS   30
#define WM_ACCOUNT_MIN_REFRESH_SECS        5

// ------------------------------------------------------------------ //
// Accounts fetch completion                                          //
// ------------------------------------------------------------------ //

void
wm_account_on_accounts(const coinbase_accounts_result_t *res, void *user)
{
  whenmoon_state_t *st = user;
  whenmoon_account_t *acc;
  uint32_t n;

  if(st == NULL || st->account == NULL)
    return;

  acc = st->account;

  if(res == NULL)
    return;

  if(res->err[0] != '\0')
  {
    pthread_mutex_lock(&acc->lock);
    snprintf(acc->last_err, sizeof(acc->last_err), "%s", res->err);
    pthread_mutex_unlock(&acc->lock);

    clam(CLAM_INFO, WHENMOON_CTX,
        "account refresh failed: %s", res->err);
    return;
  }

  n = res->count;

  if(n > WM_ACCOUNT_ROW_CAP)
    n = WM_ACCOUNT_ROW_CAP;

  pthread_mutex_lock(&acc->lock);
  memcpy(acc->rows, res->rows, sizeof(acc->rows[0]) * n);
  acc->n_rows          = n;
  acc->last_refresh_ts = time(NULL);
  acc->last_err[0]     = '\0';
  pthread_mutex_unlock(&acc->lock);

  clam(CLAM_DEBUG2, WHENMOON_CTX,
      "account refresh ok rows=%u", n);
}

// ------------------------------------------------------------------ //
// Periodic tick                                                      //
// ------------------------------------------------------------------ //

static void
wm_account_tick(task_t *t)
{
  whenmoon_state_t *st = t->data;

  // wm_account_destroy task_cancel()s the handle, so the task system
  // guarantees this callback will not fire after destroy. The null
  // check is belt-and-braces.
  if(st == NULL)
  {
    t->state = TASK_ENDED;
    return;
  }

  if(!coinbase_apikey_configured())
  {
    // Key rotated out at runtime. Leave the task scheduled — it's
    // cheap — but skip the fetch.
    t->state = TASK_ENDED;
    return;
  }

  if(coinbase_get_accounts_async(wm_account_on_accounts, st) != SUCCESS)
    clam(CLAM_INFO, WHENMOON_CTX,
        "account refresh submit failed");

  t->state = TASK_ENDED;
}

// ------------------------------------------------------------------ //
// Init / destroy                                                     //
// ------------------------------------------------------------------ //

bool
wm_account_init(whenmoon_state_t *st)
{
  whenmoon_account_t *acc;
  uint32_t refresh_secs;

  if(st == NULL)
    return(FAIL);

  acc = mem_alloc("whenmoon", "account", sizeof(*acc));

  if(acc == NULL)
    return(FAIL);

  memset(acc, 0, sizeof(*acc));
  pthread_mutex_init(&acc->lock, NULL);

  st->account = acc;

  if(!coinbase_apikey_configured())
  {
    clam(CLAM_INFO, WHENMOON_CTX,
        "account refresh disabled — no apikey");
    return(SUCCESS);
  }

  refresh_secs = (uint32_t)kv_get_uint(
      "plugin.whenmoon.exchange.coinbase.account.refresh_sec");

  if(refresh_secs < WM_ACCOUNT_MIN_REFRESH_SECS)
    refresh_secs = WM_ACCOUNT_DEFAULT_REFRESH_SECS;

  acc->refresh_task = task_add_periodic("wm.acct", TASK_ANY, 200,
      refresh_secs * 1000, wm_account_tick, st);

  if(acc->refresh_task == TASK_HANDLE_NONE)
    clam(CLAM_INFO, WHENMOON_CTX,
        "account periodic task submit failed");

  else
    clam(CLAM_INFO, WHENMOON_CTX,
        "account refresh scheduled every %us", refresh_secs);

  // Kick off an immediate first fetch so the show verb has data
  // before the first tick fires.
  if(coinbase_get_accounts_async(wm_account_on_accounts, st) != SUCCESS)
    clam(CLAM_INFO, WHENMOON_CTX,
        "initial account fetch submit failed");

  return(SUCCESS);
}

void
wm_account_destroy(whenmoon_state_t *st)
{
  whenmoon_account_t *acc;

  if(st == NULL || st->account == NULL)
    return;

  acc = st->account;

  // Cancel the periodic synchronously so no stale tick fires after
  // the struct is freed.
  task_cancel(acc->refresh_task);
  acc->refresh_task = TASK_HANDLE_NONE;

  // Detach first so a racing wm_account_on_accounts callback sees
  // st->account == NULL via the state pointer and bails before
  // touching freed memory.
  st->account = NULL;

  pthread_mutex_destroy(&acc->lock);
  mem_free(acc);
}
