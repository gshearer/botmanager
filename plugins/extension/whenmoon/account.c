// botmanager — MIT
// whenmoon per-exchange account (balance) snapshot cache. See account.h
// for the hybrid model (scheduled poll + fast-forward + on-demand
// fallback) and the WM-PAPER-GATE-1 gate rationale.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "account.h"
#include "market.h"
#include "market_engine.h"
#include "dl_jobtable.h"

#include "exchange_api.h"
#include "alloc.h"
#include "kv.h"
#include "task.h"

#include <stdio.h>
#include <string.h>

// 5-minute default cadence (the original whenmoon model). A trade event
// fast-forwards this, so the schedule is the idle-refresh floor, not the
// freshness ceiling.
#define WM_ACCOUNT_DEFAULT_REFRESH_SECS   300
#define WM_ACCOUNT_MIN_REFRESH_SECS         5

// Per-async-call heap context. Lets wm_account_on_accounts find the
// matching slot under st->account even if the slot list grew between
// dispatch and completion. Freed by the callback on every exit path.
typedef struct
{
  struct whenmoon_state *st;
  char                   exchange_name[EXCHANGE_NAME_SZ];
} wm_account_refresh_ctx_t;

// ------------------------------------------------------------------ //
// Slot helpers                                                       //
// ------------------------------------------------------------------ //

static wm_account_slot_t *
wm_account_slot_find(whenmoon_account_t *acc, const char *name)
{
  uint32_t i;

  if(acc == NULL || name == NULL)
    return(NULL);

  for(i = 0; i < acc->n_slots; i++)
  {
    if(strncmp(acc->slots[i].exchange_name, name,
           EXCHANGE_NAME_SZ) == 0)
      return(&acc->slots[i]);
  }

  return(NULL);
}

// ------------------------------------------------------------------ //
// Accounts fetch completion                                          //
// ------------------------------------------------------------------ //

void
wm_account_on_accounts(const exchange_accounts_result_t *res, void *user)
{
  wm_account_refresh_ctx_t *ctx = user;
  whenmoon_state_t         *st;
  whenmoon_account_t       *acc;
  wm_account_slot_t        *slot;
  uint32_t                  n;

  if(ctx == NULL)
    return;

  st = ctx->st;

  if(st == NULL || st->account == NULL)
  {
    mem_free(ctx);
    return;
  }

  acc  = st->account;
  slot = wm_account_slot_find(acc, ctx->exchange_name);

  if(slot == NULL || res == NULL)
  {
    mem_free(ctx);
    return;
  }

  if(res->err[0] != '\0')
  {
    pthread_mutex_lock(&slot->lock);
    snprintf(slot->last_err, sizeof(slot->last_err), "%s", res->err);
    pthread_mutex_unlock(&slot->lock);

    clam(CLAM_INFO, WHENMOON_CTX,
        "account refresh failed (exchange=%s): %s",
        ctx->exchange_name, res->err);
    mem_free(ctx);
    return;
  }

  n = res->count;

  if(n > WM_ACCOUNT_ROW_CAP)
    n = WM_ACCOUNT_ROW_CAP;

  pthread_mutex_lock(&slot->lock);
  if(n > 0)
    memcpy(slot->rows, res->rows, sizeof(slot->rows[0]) * n);
  slot->n_rows               = n;
  slot->last_refresh_mono_ms = wm_dl_now_ms();
  slot->last_err[0]          = '\0';
  pthread_mutex_unlock(&slot->lock);

  clam(CLAM_DEBUG2, WHENMOON_CTX,
      "account refresh ok (exchange=%s rows=%u)",
      ctx->exchange_name, n);

  // Auto-reconcile real cash for flat markets on this exchange from the
  // fresh snapshot — no extra exchange call. Removes the manual
  // /whenmoon market sync for the common (flat) case.
  wm_live_reconcile_from_accounts(ctx->exchange_name, res->rows,
      res->count);

  mem_free(ctx);
}

// Build a fresh refresh ctx; caller is responsible for cleanup on the
// no-dispatch path (FAIL before submit). On dispatch the callback owns
// the free.
static wm_account_refresh_ctx_t *
wm_account_ctx_new(whenmoon_state_t *st, const char *exchange_name)
{
  wm_account_refresh_ctx_t *ctx;

  ctx = mem_alloc("whenmoon", "acct.refresh_ctx", sizeof(*ctx));

  if(ctx == NULL)
    return(NULL);

  ctx->st = st;
  snprintf(ctx->exchange_name, sizeof(ctx->exchange_name), "%s",
      exchange_name);
  return(ctx);
}

// True iff `exchange_name` should be polled: at least one real-mode
// market routes there AND credentials are configured. Both the periodic
// tick and the fast-forward gate on this so paper/manual deploys emit no
// background authenticated traffic (WM-PAPER-GATE-1).
static bool
wm_account_should_poll(whenmoon_state_t *st, const char *exchange_name)
{
  exchange_capabilities_t caps;

  if(!wm_market_exchange_has_real_mode(st, exchange_name))
    return(false);

  if(exchange_get_capabilities(exchange_name, &caps) != SUCCESS
      || !caps.has_credentials)
    return(false);

  return(true);
}

// ------------------------------------------------------------------ //
// Periodic tick (per-slot)                                           //
// ------------------------------------------------------------------ //

static void
wm_account_tick(task_t *t)
{
  wm_account_slot_t        *slot;
  whenmoon_state_t         *st;
  wm_account_refresh_ctx_t *ctx;

  // task_cancel runs synchronously, so destroy never frees slot while
  // this tick is in flight. The null check is belt-and-braces.
  slot = t->data;

  if(slot == NULL)
  {
    t->state = TASK_ENDED;
    return;
  }

  st = whenmoon_get_state();

  if(st == NULL || st->account == NULL)
  {
    t->state = TASK_ENDED;
    return;
  }

  // Only refresh balances for exchanges actually trading real money.
  // Account balances feed real-mode observability only; polling the
  // authenticated /accounts endpoint for a paper / manual exchange is
  // needless private-endpoint traffic that reads as anomalous to the
  // exchange's fraud tooling. This gate is stronger than the creds
  // check: stale credential KVs keep is_authenticated() true long after
  // the operator stops trading.
  if(!wm_account_should_poll(st, slot->exchange_name))
  {
    t->state = TASK_ENDED;
    return;
  }

  ctx = wm_account_ctx_new(st, slot->exchange_name);

  if(ctx == NULL)
  {
    t->state = TASK_ENDED;
    return;
  }

  if(exchange_get_accounts_async(slot->exchange_name,
        wm_account_on_accounts, ctx) != SUCCESS)
  {
    // exchange_get_accounts_async fires the typed cb synchronously with
    // err set on FAIL; the cb has already freed ctx. Do not touch.
    clam(CLAM_INFO, WHENMOON_CTX,
        "account refresh submit failed (exchange=%s)",
        slot->exchange_name);
  }

  t->state = TASK_ENDED;
}

// Refresh cadence (seconds). Reads the per-exchange KV key with a floor
// of WM_ACCOUNT_MIN_REFRESH_SECS — unset / out-of-range falls to the
// default.
static uint32_t
wm_account_refresh_secs(const char *exchange_name)
{
  char     key[160];
  uint32_t val;
  int      n;

  n = snprintf(key, sizeof(key),
      "plugin.whenmoon.exchange.%s.account.refresh_sec", exchange_name);

  if(n < 0 || (size_t)n >= sizeof(key))
    return(WM_ACCOUNT_DEFAULT_REFRESH_SECS);

  val = (uint32_t)kv_get_uint(key);

  if(val < WM_ACCOUNT_MIN_REFRESH_SECS)
    val = WM_ACCOUNT_DEFAULT_REFRESH_SECS;

  return(val);
}

// ------------------------------------------------------------------ //
// Fast-forward + snapshot read/write                                  //
// ------------------------------------------------------------------ //

void
wm_account_fast_forward(const char *exchange_name)
{
  whenmoon_state_t         *st;
  wm_account_refresh_ctx_t *ctx;

  if(exchange_name == NULL || exchange_name[0] == '\0')
    return;

  st = whenmoon_get_state();

  if(st == NULL || st->account == NULL)
    return;

  // Same gate as the periodic tick. A real fill on a real-mode market is
  // what calls this, so the gate is normally satisfied; the re-check
  // keeps a stray caller from generating paper-mode authenticated
  // traffic.
  if(!wm_account_should_poll(st, exchange_name))
    return;

  ctx = wm_account_ctx_new(st, exchange_name);

  if(ctx == NULL)
    return;

  if(exchange_get_accounts_async(exchange_name,
        wm_account_on_accounts, ctx) != SUCCESS)
    clam(CLAM_INFO, WHENMOON_CTX,
        "account fast-forward submit failed (exchange=%s)",
        exchange_name);
}

bool
wm_account_get_snapshot(const char *exchange_name,
    exchange_account_t *rows_out, uint32_t cap, uint32_t *n_out,
    int64_t *age_ms_out)
{
  whenmoon_state_t  *st;
  wm_account_slot_t *slot;
  uint32_t           n;

  st = whenmoon_get_state();

  if(st == NULL || st->account == NULL || exchange_name == NULL)
    return(false);

  slot = wm_account_slot_find(st->account, exchange_name);

  if(slot == NULL)
    return(false);

  pthread_mutex_lock(&slot->lock);

  if(slot->last_refresh_mono_ms == 0)
  {
    pthread_mutex_unlock(&slot->lock);
    return(false);
  }

  n = slot->n_rows;

  if(n > cap)
    n = cap;

  if(rows_out != NULL && n > 0)
    memcpy(rows_out, slot->rows, sizeof(rows_out[0]) * n);

  if(n_out != NULL)
    *n_out = n;

  if(age_ms_out != NULL)
    *age_ms_out = wm_dl_now_ms() - slot->last_refresh_mono_ms;

  pthread_mutex_unlock(&slot->lock);
  return(true);
}

void
wm_account_store_snapshot(const char *exchange_name,
    const exchange_account_t *rows, uint32_t n)
{
  whenmoon_state_t  *st;
  wm_account_slot_t *slot;

  st = whenmoon_get_state();

  if(st == NULL || st->account == NULL || exchange_name == NULL
      || rows == NULL)
    return;

  slot = wm_account_slot_find(st->account, exchange_name);

  if(slot == NULL)
    return;

  if(n > WM_ACCOUNT_ROW_CAP)
    n = WM_ACCOUNT_ROW_CAP;

  pthread_mutex_lock(&slot->lock);
  if(n > 0)
    memcpy(slot->rows, rows, sizeof(slot->rows[0]) * n);
  slot->n_rows               = n;
  slot->last_refresh_mono_ms = wm_dl_now_ms();
  slot->last_err[0]          = '\0';
  pthread_mutex_unlock(&slot->lock);
}

// ------------------------------------------------------------------ //
// Init / start / destroy                                              //
// ------------------------------------------------------------------ //

bool
wm_account_init(whenmoon_state_t *st)
{
  whenmoon_account_t *acc;

  if(st == NULL)
    return(FAIL);

  acc = mem_alloc("whenmoon", "account", sizeof(*acc));

  if(acc == NULL)
    return(FAIL);

  memset(acc, 0, sizeof(*acc));
  st->account = acc;
  return(SUCCESS);
}

bool
wm_account_start(whenmoon_state_t *st)
{
  whenmoon_account_t *acc;
  char                names[WM_ACCOUNT_MAX_EXCHANGES][EXCHANGE_NAME_SZ];
  uint32_t            n_names = 0;
  uint32_t            i;

  if(st == NULL || st->account == NULL)
    return(FAIL);

  acc = st->account;

  // Idempotent: skip when slots already exist (start can be re-entered
  // after a freshstart-style reload).
  if(acc->n_slots > 0)
    return(SUCCESS);

  if(exchange_name_list(names, WM_ACCOUNT_MAX_EXCHANGES, &n_names) != SUCCESS)
    n_names = 0;

  if(n_names > WM_ACCOUNT_MAX_EXCHANGES)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "exchange list overflow (%u > %u); capping",
        n_names, WM_ACCOUNT_MAX_EXCHANGES);
    n_names = WM_ACCOUNT_MAX_EXCHANGES;
  }

  for(i = 0; i < n_names; i++)
  {
    wm_account_slot_t *slot = &acc->slots[acc->n_slots];
    uint32_t           refresh_secs;

    memset(slot, 0, sizeof(*slot));
    snprintf(slot->exchange_name, sizeof(slot->exchange_name), "%.*s",
        (int)(sizeof(slot->exchange_name) - 1), names[i]);
    pthread_mutex_init(&slot->lock, NULL);

    refresh_secs = wm_account_refresh_secs(slot->exchange_name);

    slot->refresh_task = task_add_periodic("wm.acct", TASK_ANY, 200,
        refresh_secs * 1000, wm_account_tick, slot);

    if(slot->refresh_task == TASK_HANDLE_NONE)
      clam(CLAM_INFO, WHENMOON_CTX,
          "account periodic task submit failed (exchange=%s)",
          slot->exchange_name);

    else
      clam(CLAM_INFO, WHENMOON_CTX,
          "account refresh scheduled every %us (exchange=%s)",
          refresh_secs, slot->exchange_name);

    acc->n_slots++;

    // Mirror the tick gate: no initial fetch unless a real-mode market
    // on this exchange exists (and creds are present).
    if(wm_account_should_poll(st, slot->exchange_name))
    {
      wm_account_refresh_ctx_t *ctx;

      ctx = wm_account_ctx_new(st, slot->exchange_name);

      if(ctx != NULL
          && exchange_get_accounts_async(slot->exchange_name,
                wm_account_on_accounts, ctx) != SUCCESS)
        clam(CLAM_INFO, WHENMOON_CTX,
            "initial account fetch submit failed (exchange=%s)",
            slot->exchange_name);
    }
  }

  return(SUCCESS);
}

void
wm_account_destroy(whenmoon_state_t *st)
{
  whenmoon_account_t *acc;
  uint32_t            i;

  if(st == NULL || st->account == NULL)
    return;

  acc = st->account;

  // Cancel every periodic synchronously so no stale tick fires after
  // the per-slot lock is destroyed below.
  for(i = 0; i < acc->n_slots; i++)
  {
    task_cancel(acc->slots[i].refresh_task);
    acc->slots[i].refresh_task = TASK_HANDLE_NONE;
  }

  // Detach first so a racing wm_account_on_accounts callback sees
  // st->account == NULL and bails before touching freed memory.
  st->account = NULL;

  for(i = 0; i < acc->n_slots; i++)
    pthread_mutex_destroy(&acc->slots[i].lock);

  mem_free(acc);
}
