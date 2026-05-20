// botmanager — MIT
// feature_exchange capability surface (WM-OR-1).
//
// Public typed verbs that dispatch to the per-protocol vtable hooks
// (`place_order_async`, `cancel_order_async`, `get_order_async`,
// `list_orders_async`, `list_fills_async`, `get_accounts_async`) plus
// the synchronous probes (`get_capabilities`, `name_list`).
//
// Every async verb here is "auth-gated": it FAILs early with
// `error: <name>: api keys not configured` when the protocol's
// `is_authenticated` hook is missing or returns false. The intent is
// that the protocol plugin's auth probe is the single source of
// truth — a public-only exchange returns false and every order verb
// declines uniformly. Hook bodies fire on the protocol plugin's curl
// worker thread; this layer never touches that thread itself.
//
// Sync error path: on any pre-flight FAIL (unknown exchange, missing
// hook, no credentials), a synthetic result struct is filled with
// `err` and the caller's typed callback is invoked synchronously
// before returning FAIL. Callers therefore see the err in exactly one
// place — the typed callback — regardless of whether the failure is
// pre- or post-dispatch.

#define EXCHANGE_INTERNAL
#include "exchange.h"

#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------------ //
// Synchronous failure helpers — fire `cb` with a populated err.       //
// ------------------------------------------------------------------ //

static void
fail_order_cb(exchange_done_order_cb_t cb, void *user, const char *err)
{
  exchange_order_result_t res;

  if(cb == NULL)
    return;

  memset(&res, 0, sizeof(res));
  snprintf(res.err, sizeof(res.err), "%s", err != NULL ? err : "error");
  cb(&res, user);
}

static void
fail_orders_cb(exchange_done_orders_cb_t cb, void *user, const char *err)
{
  exchange_orders_result_t res;

  if(cb == NULL)
    return;

  memset(&res, 0, sizeof(res));
  snprintf(res.err, sizeof(res.err), "%s", err != NULL ? err : "error");
  cb(&res, user);
}

static void
fail_accounts_cb(exchange_done_accounts_cb_t cb, void *user,
    const char *err)
{
  exchange_accounts_result_t res;

  if(cb == NULL)
    return;

  memset(&res, 0, sizeof(res));
  snprintf(res.err, sizeof(res.err), "%s", err != NULL ? err : "error");
  cb(&res, user);
}

static void
fail_fills_cb(exchange_done_fills_cb_t cb, void *user, const char *err)
{
  exchange_fills_result_t res;

  if(cb == NULL)
    return;

  memset(&res, 0, sizeof(res));
  snprintf(res.err, sizeof(res.err), "%s", err != NULL ? err : "error");
  cb(&res, user);
}

static void
fail_candles_cb(exchange_done_candles_cb_t cb, void *user, const char *err)
{
  exchange_candles_result_t res;

  if(cb == NULL)
    return;

  memset(&res, 0, sizeof(res));
  snprintf(res.err, sizeof(res.err), "%s", err != NULL ? err : "error");
  cb(&res, user);
}

// MW-1: synchronous failure for the bulk-ticker callback.
static void
fail_tickers_cb(exchange_done_tickers_cb_t cb, void *user, const char *err)
{
  if(cb == NULL)
    return;

  cb(false, err != NULL ? err : "error", NULL, 0, user);
}

// Resolve `name` without any auth check. Used by public market-data
// verbs (candles, public WS channels). Writes a human-readable error
// into `errbuf` on FAIL.
static bool
resolve_exchange(const char *name, exchange_t **out_e,
    char *errbuf, size_t errbuf_sz)
{
  exchange_t *e;

  if(name == NULL || name[0] == '\0')
  {
    snprintf(errbuf, errbuf_sz, "exchange name required");
    return(FAIL);
  }

  e = exchange_find(name);

  if(e == NULL || e->vt == NULL)
  {
    snprintf(errbuf, errbuf_sz, "%s: not registered", name);
    return(FAIL);
  }

  *out_e = e;
  return(SUCCESS);
}

// Resolve `name` and check `is_authenticated`. Writes a human-readable
// error into `errbuf` on FAIL. Caller uses `errbuf` when firing the
// synthetic typed callback so the message reaches the consumer.
static bool
resolve_authed(const char *name, exchange_t **out_e,
    char *errbuf, size_t errbuf_sz)
{
  exchange_t *e;

  if(name == NULL || name[0] == '\0')
  {
    snprintf(errbuf, errbuf_sz, "exchange name required");
    return(FAIL);
  }

  e = exchange_find(name);

  if(e == NULL || e->vt == NULL)
  {
    snprintf(errbuf, errbuf_sz, "%s: not registered", name);
    return(FAIL);
  }

  if(e->vt->is_authenticated == NULL || !e->vt->is_authenticated())
  {
    snprintf(errbuf, errbuf_sz, "%s: api keys not configured", name);
    return(FAIL);
  }

  *out_e = e;
  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Synchronous probes                                                  //
// ------------------------------------------------------------------ //

bool
exchange_get_capabilities(const char *name, exchange_capabilities_t *out)
{
  exchange_t *e;
  size_t      nlen;

  if(out == NULL)
    return(FAIL);

  memset(out, 0, sizeof(*out));

  if(name == NULL || name[0] == '\0')
    return(FAIL);

  e = exchange_find(name);

  if(e == NULL || e->vt == NULL)
    return(FAIL);

  nlen = strnlen(e->name, sizeof(e->name));

  if(nlen >= sizeof(out->name))
    nlen = sizeof(out->name) - 1;

  memcpy(out->name, e->name, nlen);
  out->name[nlen]       = '\0';
  out->has_credentials  = e->vt->is_authenticated != NULL
                       && e->vt->is_authenticated();
  out->advertised_rps   = e->vt->advertised_rps;
  out->advertised_burst = e->vt->advertised_burst;
  return(SUCCESS);
}

// name_list iterator state — gathered under the registry lock by
// exchange_registry_iterate.
typedef struct
{
  char     (*out_arr)[EXCHANGE_NAME_SZ];
  uint32_t   out_cap;
  uint32_t   total;
} name_list_ctx_t;

static void
name_list_visit(exchange_t *e, void *user)
{
  name_list_ctx_t *ctx = user;
  uint32_t         idx;

  if(e == NULL || ctx == NULL)
    return;

  idx = ctx->total++;

  if(ctx->out_arr != NULL && idx < ctx->out_cap)
  {
    size_t nlen = strnlen(e->name, sizeof(e->name));

    if(nlen >= EXCHANGE_NAME_SZ)
      nlen = EXCHANGE_NAME_SZ - 1;

    memcpy(ctx->out_arr[idx], e->name, nlen);
    ctx->out_arr[idx][nlen] = '\0';
  }
}

bool
exchange_name_list(char (*out_arr)[EXCHANGE_NAME_SZ], uint32_t out_cap,
    uint32_t *out_count)
{
  name_list_ctx_t ctx;

  if(out_count == NULL)
    return(FAIL);

  *out_count = 0;

  ctx.out_arr = out_arr;
  ctx.out_cap = out_cap;
  ctx.total   = 0;

  exchange_registry_iterate(name_list_visit, &ctx);

  *out_count = ctx.total;
  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Async dispatch verbs                                                //
// ------------------------------------------------------------------ //

bool
exchange_place_order_async(const char *name,
    const exchange_place_order_req_t *req,
    exchange_done_order_cb_t cb, void *user)
{
  exchange_t *e   = NULL;
  char        err[EXCHANGE_ERR_SZ];

  if(req == NULL || cb == NULL)
    return(FAIL);

  if(resolve_authed(name, &e, err, sizeof(err)) != SUCCESS)
  {
    fail_order_cb(cb, user, err);
    return(FAIL);
  }

  if(e->vt->place_order_async == NULL)
  {
    snprintf(err, sizeof(err),
        "%s: place_order not supported", name);
    fail_order_cb(cb, user, err);
    return(FAIL);
  }

  if(e->vt->place_order_async(req, cb, user) != SUCCESS)
  {
    // Hook fired its own typed cb with err on FAIL — do not double-
    // fire here.
    return(FAIL);
  }

  return(SUCCESS);
}

bool
exchange_cancel_order_async(const char *name, const char *order_id,
    exchange_done_order_cb_t cb, void *user)
{
  exchange_t *e = NULL;
  char        err[EXCHANGE_ERR_SZ];

  if(cb == NULL)
    return(FAIL);

  if(order_id == NULL || order_id[0] == '\0')
  {
    fail_order_cb(cb, user, "order_id required");
    return(FAIL);
  }

  if(resolve_authed(name, &e, err, sizeof(err)) != SUCCESS)
  {
    fail_order_cb(cb, user, err);
    return(FAIL);
  }

  if(e->vt->cancel_order_async == NULL)
  {
    snprintf(err, sizeof(err),
        "%s: cancel_order not supported", name);
    fail_order_cb(cb, user, err);
    return(FAIL);
  }

  if(e->vt->cancel_order_async(order_id, cb, user) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

bool
exchange_get_order_async(const char *name, const char *order_id,
    exchange_done_order_cb_t cb, void *user)
{
  exchange_t *e = NULL;
  char        err[EXCHANGE_ERR_SZ];

  if(cb == NULL)
    return(FAIL);

  if(order_id == NULL || order_id[0] == '\0')
  {
    fail_order_cb(cb, user, "order_id required");
    return(FAIL);
  }

  if(resolve_authed(name, &e, err, sizeof(err)) != SUCCESS)
  {
    fail_order_cb(cb, user, err);
    return(FAIL);
  }

  if(e->vt->get_order_async == NULL)
  {
    snprintf(err, sizeof(err),
        "%s: get_order not supported", name);
    fail_order_cb(cb, user, err);
    return(FAIL);
  }

  if(e->vt->get_order_async(order_id, cb, user) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

bool
exchange_list_orders_async(const char *name, const char *status,
    const char *product_id,
    exchange_done_orders_cb_t cb, void *user)
{
  exchange_t *e = NULL;
  char        err[EXCHANGE_ERR_SZ];

  if(cb == NULL)
    return(FAIL);

  if(resolve_authed(name, &e, err, sizeof(err)) != SUCCESS)
  {
    fail_orders_cb(cb, user, err);
    return(FAIL);
  }

  if(e->vt->list_orders_async == NULL)
  {
    snprintf(err, sizeof(err),
        "%s: list_orders not supported", name);
    fail_orders_cb(cb, user, err);
    return(FAIL);
  }

  if(e->vt->list_orders_async(status, product_id, cb, user) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

bool
exchange_list_fills_async(const char *name, const char *order_id,
    const char *product_id, int64_t start_ms,
    exchange_done_fills_cb_t cb, void *user)
{
  exchange_t *e = NULL;
  char        err[EXCHANGE_ERR_SZ];

  if(cb == NULL)
    return(FAIL);

  if(resolve_authed(name, &e, err, sizeof(err)) != SUCCESS)
  {
    fail_fills_cb(cb, user, err);
    return(FAIL);
  }

  if(e->vt->list_fills_async == NULL)
  {
    snprintf(err, sizeof(err),
        "%s: list_fills not supported", name);
    fail_fills_cb(cb, user, err);
    return(FAIL);
  }

  if(e->vt->list_fills_async(order_id, product_id, start_ms,
        cb, user) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

bool
exchange_get_accounts_async(const char *name,
    exchange_done_accounts_cb_t cb, void *user)
{
  exchange_t *e = NULL;
  char        err[EXCHANGE_ERR_SZ];

  if(cb == NULL)
    return(FAIL);

  if(resolve_authed(name, &e, err, sizeof(err)) != SUCCESS)
  {
    fail_accounts_cb(cb, user, err);
    return(FAIL);
  }

  if(e->vt->get_accounts_async == NULL)
  {
    snprintf(err, sizeof(err),
        "%s: get_accounts not supported", name);
    fail_accounts_cb(cb, user, err);
    return(FAIL);
  }

  if(e->vt->get_accounts_async(cb, user) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// KR-2: candle fetch (public market data — no auth gate)              //
// ------------------------------------------------------------------ //

bool
exchange_fetch_candles_async(const char *name, const char *product_id,
    exchange_granularity_t gran, int64_t since_ms, int64_t until_ms,
    exchange_done_candles_cb_t cb, void *user)
{
  exchange_t *e = NULL;
  char        err[EXCHANGE_ERR_SZ];

  if(cb == NULL)
    return(FAIL);

  if(product_id == NULL || product_id[0] == '\0')
  {
    fail_candles_cb(cb, user, "product_id required");
    return(FAIL);
  }

  if(resolve_exchange(name, &e, err, sizeof(err)) != SUCCESS)
  {
    fail_candles_cb(cb, user, err);
    return(FAIL);
  }

  if(e->vt->fetch_candles_async == NULL)
  {
    snprintf(err, sizeof(err),
        "%s: fetch_candles not supported", name);
    fail_candles_cb(cb, user, err);
    return(FAIL);
  }

  if(e->vt->fetch_candles_async(product_id, gran, since_ms, until_ms,
        cb, user) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// MW-1: bulk-ticker fetch (public market data — no auth gate)         //
// ------------------------------------------------------------------ //

bool
exchange_fetch_all_tickers_async(const char *name,
    exchange_done_tickers_cb_t cb, void *user)
{
  exchange_t *e = NULL;
  char        err[EXCHANGE_ERR_SZ];

  if(cb == NULL)
    return(FAIL);

  if(resolve_exchange(name, &e, err, sizeof(err)) != SUCCESS)
  {
    fail_tickers_cb(cb, user, err);
    return(FAIL);
  }

  if(e->vt->fetch_all_tickers == NULL)
  {
    snprintf(err, sizeof(err),
        "%s: fetch_all_tickers not supported", name);
    fail_tickers_cb(cb, user, err);
    return(FAIL);
  }

  if(e->vt->fetch_all_tickers(cb, user) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// KR-2: WS subscribe / unsubscribe                                    //
//                                                                      //
// Per-channel auth gating lives in the protocol plugin's adapter      //
// (e.g. the user channel FAILs without credentials). The abstraction  //
// only enforces basic shape: known exchange, non-empty subscriber     //
// set, vtable hook present.                                           //
// ------------------------------------------------------------------ //

bool
exchange_ws_subscribe(const char *name,
    const exchange_ws_channel_t *channels, uint32_t n_channels,
    const char *const *product_ids, uint32_t n_products,
    exchange_ws_event_cb_t cb, void *user,
    exchange_ws_sub_t **out_handle)
{
  exchange_t *e = NULL;
  char        err[EXCHANGE_ERR_SZ];

  if(out_handle != NULL)
    *out_handle = NULL;

  if(cb == NULL || out_handle == NULL || channels == NULL ||
     n_channels == 0)
    return(FAIL);

  if(resolve_exchange(name, &e, err, sizeof(err)) != SUCCESS)
  {
    clam(CLAM_INFO, "exchange", "ws_subscribe FAIL: %s", err);
    return(FAIL);
  }

  if(e->vt->ws_subscribe == NULL)
  {
    clam(CLAM_INFO, "exchange",
        "ws_subscribe FAIL: %s: ws_subscribe not supported", name);
    return(FAIL);
  }

  if(e->vt->ws_subscribe(channels, n_channels, product_ids, n_products,
        cb, user, out_handle) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

void
exchange_ws_unsubscribe(const char *name, exchange_ws_sub_t *handle)
{
  exchange_t *e;

  if(handle == NULL || name == NULL || name[0] == '\0')
    return;

  e = exchange_find(name);

  // Tolerate "plugin already unregistered" — handle invalidated by
  // exchange_unregister; nothing further to do.
  if(e == NULL || e->vt == NULL || e->vt->ws_unsubscribe == NULL)
    return;

  e->vt->ws_unsubscribe(handle);
}
