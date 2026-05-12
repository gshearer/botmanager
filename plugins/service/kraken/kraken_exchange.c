// botmanager — MIT
// Kraken exchange-vtable: bridges feature_exchange to the kraken REST
// surface (kr_submit_public / kr_submit_private) and to the typed
// kraken_*_async wrappers in kraken_orders.c.
//
// build_request / submit / free_request own the curl-level handle for
// candle + assetpairs traffic that flows through exchange_request.
// The capability hooks (is_authenticated, place_order_async, …)
// adapt the typed kraken_* shapes to the generic exchange_*_t shapes
// — field-by-field translation since every exchange_*_t buffer is
// sized at-or-above its kraken_*_t equivalent.
//
// WS subscribe / unsubscribe stay NULL until KR-5 lands the
// WebSocket v2 reader.
#define KR_INTERNAL
#include "kraken.h"

#include "exchange_api.h"

#include <stdio.h>
#include <string.h>

#define KR_EXCHANGE_PATH_SZ  KR_URL_SZ
#define KR_EXCHANGE_BODY_SZ  KR_BODY_SZ

typedef struct
{
  exchange_op_kind_t      kind;

  char                    path[KR_EXCHANGE_PATH_SZ];
  char                   *body;
  size_t                  body_len;

  exchange_response_cb_t  abstr_cb;
  void                   *abstr_user;
} kr_exchange_handle_t;

// ------------------------------------------------------------------ //
// curl completion adapter                                             //
// ------------------------------------------------------------------ //

static void
kr_exchange_curl_done(const curl_response_t *resp)
{
  kr_exchange_handle_t   *h = (kr_exchange_handle_t *)resp->user_data;
  exchange_response_cb_t  cb;
  void                   *user;
  bool                    transport_err;

  if(h == NULL)
    return;

  cb            = h->abstr_cb;
  user          = h->abstr_user;
  transport_err = (resp->curl_code != 0);

  if(cb != NULL)
  {
    if(resp->cancelled)
    {
      cb(-1, NULL, 0,
          resp->error != NULL ? resp->error : "request cancelled",
          user);
    }
    else if(transport_err)
    {
      cb(0, NULL, 0,
          resp->error != NULL ? resp->error : "transport error",
          user);
    }
    else
    {
      cb((int)resp->status, resp->body, resp->body_len, NULL, user);
    }
  }
}

// ------------------------------------------------------------------ //
// vtable: build / submit / free                                       //
// ------------------------------------------------------------------ //

static bool
kr_exchange_build_request(exchange_op_kind_t kind, const char *path,
    const char *body_json, void **out_handle)
{
  kr_exchange_handle_t *h;
  size_t                plen;

  if(out_handle == NULL)
    return(FAIL);

  *out_handle = NULL;

  if(path == NULL || path[0] == '\0')
    return(FAIL);

  plen = strnlen(path, KR_EXCHANGE_PATH_SZ);

  if(plen >= KR_EXCHANGE_PATH_SZ)
  {
    clam(CLAM_WARN, KR_CTX,
        "exchange build: path too long (>%zu)",
        (size_t)(KR_EXCHANGE_PATH_SZ - 1));
    return(FAIL);
  }

  h = mem_alloc(KR_CTX ".exch", "handle", sizeof(*h));

  if(h == NULL)
    return(FAIL);

  memset(h, 0, sizeof(*h));
  h->kind = kind;
  memcpy(h->path, path, plen);
  h->path[plen] = '\0';

  if(body_json != NULL && body_json[0] != '\0')
  {
    h->body_len = strnlen(body_json, KR_EXCHANGE_BODY_SZ);

    if(h->body_len >= KR_EXCHANGE_BODY_SZ)
    {
      clam(CLAM_WARN, KR_CTX,
          "exchange build: body too long (>%zu)",
          (size_t)(KR_EXCHANGE_BODY_SZ - 1));
      mem_free(h);
      return(FAIL);
    }

    h->body = mem_alloc(KR_CTX ".exch", "body", h->body_len + 1);

    if(h->body == NULL)
    {
      mem_free(h);
      return(FAIL);
    }

    memcpy(h->body, body_json, h->body_len);
    h->body[h->body_len] = '\0';
  }

  *out_handle = h;
  return(SUCCESS);
}

static bool
kr_exchange_submit(void *handle, uint8_t prio,
    exchange_response_cb_t cb, void *user)
{
  kr_exchange_handle_t *h = handle;
  bool                  is_private;

  if(h == NULL || cb == NULL)
    return(FAIL);

  h->abstr_cb   = cb;
  h->abstr_user = user;

  is_private = (h->kind == EXCHANGE_OP_PRIVATE_REST_GET
             || h->kind == EXCHANGE_OP_PRIVATE_REST_POST
             || h->kind == EXCHANGE_OP_PRIVATE_REST_DELETE);

  if(is_private)
  {
    // Kraken's private surface is POST-only. Surface a clean refusal
    // for any GET/DELETE callers since EXCHANGE_OP_PRIVATE_REST_{GET,
    // DELETE} is unsupported at the wire level.
    if(h->kind != EXCHANGE_OP_PRIVATE_REST_POST)
    {
      clam(CLAM_WARN, KR_CTX,
          "exchange submit: Kraken private endpoints are POST-only");
      return(FAIL);
    }

    if(kr_submit_private(h, prio, h->path, h->body, h->body_len,
          kr_exchange_curl_done) != SUCCESS)
      return(FAIL);
  }
  else
  {
    if(kr_submit_public(h, prio, h->path, kr_exchange_curl_done)
        != SUCCESS)
      return(FAIL);
  }

  return(SUCCESS);
}

static void
kr_exchange_free_request(void *handle)
{
  kr_exchange_handle_t *h = handle;

  if(h == NULL)
    return;

  if(h->body != NULL)
    mem_free(h->body);

  mem_free(h);
}

// ------------------------------------------------------------------ //
// Capability hooks (KR-4) — adapt typed kraken_*_async to the         //
// generic exchange_*_t surface. Each trampoline allocates a small     //
// fwd ctx, calls the typed wrapper, and the adapter cb translates    //
// the result on the way back.                                         //
// ------------------------------------------------------------------ //

static bool
kr_exch_is_authenticated(void)
{
  return(kr_apikey_configured());
}

typedef struct
{
  exchange_done_order_cb_t cb;
  void                    *user;
} kr_exch_order_fwd_t;

typedef struct
{
  exchange_done_orders_cb_t cb;
  void                     *user;
} kr_exch_orders_fwd_t;

typedef struct
{
  exchange_done_accounts_cb_t cb;
  void                       *user;
} kr_exch_accounts_fwd_t;

typedef struct
{
  exchange_done_fills_cb_t cb;
  void                    *user;
} kr_exch_fills_fwd_t;

typedef struct
{
  exchange_done_candles_cb_t cb;
  void                      *user;
  int64_t                    until_ms;     // optional client-side cutoff
} kr_exch_candles_fwd_t;

// ---- typed translation helpers ----

static void
kr_to_exch_order(const kraken_order_t *src, exchange_order_t *dst)
{
  memset(dst, 0, sizeof(*dst));
  snprintf(dst->order_id,   sizeof(dst->order_id),   "%s", src->order_id);
  snprintf(dst->client_oid, sizeof(dst->client_oid), "%s", src->client_oid);
  snprintf(dst->product_id, sizeof(dst->product_id), "%s", src->product_id);
  snprintf(dst->side,       sizeof(dst->side),       "%s", src->side);
  snprintf(dst->type,       sizeof(dst->type),       "%s", src->type);
  snprintf(dst->status,     sizeof(dst->status),     "%s", src->status);
  snprintf(dst->tif,        sizeof(dst->tif),        "%s", src->tif);
  dst->price          = src->price;
  dst->size           = src->size;
  dst->filled_size    = src->filled_size;
  dst->executed_value = src->executed_value;
  dst->fill_fees      = src->fill_fees;
  dst->post_only      = src->post_only;
  dst->settled        = src->settled;
  dst->created_at_ms  = src->created_at_ms;
}

static void
kr_to_exch_account(const kraken_account_t *src, exchange_account_t *dst)
{
  memset(dst, 0, sizeof(*dst));
  snprintf(dst->currency, sizeof(dst->currency), "%s", src->currency);
  dst->balance   = src->balance;
  dst->hold      = src->hold;
  dst->available = src->available;
}

static void
kr_to_exch_fill(const kraken_fill_t *src, exchange_fill_t *dst)
{
  memset(dst, 0, sizeof(*dst));
  snprintf(dst->order_id,   sizeof(dst->order_id),   "%s", src->order_id);
  snprintf(dst->client_oid, sizeof(dst->client_oid), "%s", src->client_oid);
  snprintf(dst->product_id, sizeof(dst->product_id), "%s", src->product_id);
  snprintf(dst->side,       sizeof(dst->side),       "%s", src->side);
  dst->trade_id = src->trade_id;
  dst->price    = src->price;
  dst->size     = src->size;
  dst->fee      = src->fee;
  dst->time_ms  = src->time_ms;
}

static void
kr_to_exch_candle(const kraken_candle_t *src, exchange_candle_t *dst)
{
  memset(dst, 0, sizeof(*dst));
  dst->ts_open_ms = src->ts_open_sec * 1000;
  dst->open       = src->open;
  dst->high       = src->high;
  dst->low        = src->low;
  dst->close      = src->close;
  dst->volume     = src->volume;
}

// ---- adapter callbacks ----

static void
kr_exch_order_done_adapter(const kraken_order_result_t *res, void *user)
{
  kr_exch_order_fwd_t      *fwd = user;
  exchange_order_result_t   out;

  if(fwd == NULL)
    return;

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);
  kr_to_exch_order(&res->order, &out.order);

  if(fwd->cb != NULL)
    fwd->cb(&out, fwd->user);

  mem_free(fwd);
}

static void
kr_exch_orders_done_adapter(const kraken_orders_result_t *res, void *user)
{
  kr_exch_orders_fwd_t      *fwd = user;
  exchange_orders_result_t   out;
  uint32_t                   i;
  uint32_t                   n;

  if(fwd == NULL)
    return;

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);

  n = res->count;

  if(n > EXCHANGE_MAX_ORDERS_LIST)
    n = EXCHANGE_MAX_ORDERS_LIST;

  for(i = 0; i < n; i++)
    kr_to_exch_order(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd->cb != NULL)
    fwd->cb(&out, fwd->user);

  mem_free(fwd);
}

static void
kr_exch_accounts_done_adapter(const kraken_balances_result_t *res,
    void *user)
{
  kr_exch_accounts_fwd_t      *fwd = user;
  exchange_accounts_result_t   out;
  uint32_t                     i;
  uint32_t                     n;

  if(fwd == NULL)
    return;

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);

  n = res->count;

  if(n > EXCHANGE_MAX_ACCOUNTS)
    n = EXCHANGE_MAX_ACCOUNTS;

  for(i = 0; i < n; i++)
    kr_to_exch_account(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd->cb != NULL)
    fwd->cb(&out, fwd->user);

  mem_free(fwd);
}

static void
kr_exch_fills_done_adapter(const kraken_fills_result_t *res, void *user)
{
  kr_exch_fills_fwd_t       *fwd = user;
  exchange_fills_result_t    out;
  uint32_t                   i;
  uint32_t                   n;

  if(fwd == NULL)
    return;

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);

  n = res->count;

  if(n > EXCHANGE_MAX_FILLS_LIST)
    n = EXCHANGE_MAX_FILLS_LIST;

  for(i = 0; i < n; i++)
    kr_to_exch_fill(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd->cb != NULL)
    fwd->cb(&out, fwd->user);

  mem_free(fwd);
}

static void
kr_exch_candles_done_adapter(const kraken_candles_result_t *res, void *user)
{
  kr_exch_candles_fwd_t      *fwd = user;
  exchange_candles_result_t   out;
  uint32_t                    i;
  uint32_t                    n;
  uint32_t                    kept = 0;

  if(fwd == NULL)
    return;

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);

  n = res->count;

  if(n > EXCHANGE_MAX_CANDLES)
    n = EXCHANGE_MAX_CANDLES;

  for(i = 0; i < n; i++)
  {
    const kraken_candle_t *c = &res->rows[i];
    int64_t                ts_ms = c->ts_open_sec * 1000;

    // Client-side until_ms cap (Kraken's OHLC doesn't take an `until`).
    if(fwd->until_ms > 0 && ts_ms >= fwd->until_ms)
      continue;

    kr_to_exch_candle(c, &out.rows[kept]);
    kept++;
  }

  out.count = kept;

  if(fwd->cb != NULL)
    fwd->cb(&out, fwd->user);

  mem_free(fwd);
}

// ---- fail helpers ----

static void
kr_exch_fail_order(exchange_done_order_cb_t cb, void *user, const char *err)
{
  exchange_order_result_t res;

  if(cb == NULL)
    return;

  memset(&res, 0, sizeof(res));
  snprintf(res.err, sizeof(res.err), "%s", err);
  cb(&res, user);
}

static void
kr_exch_fail_orders(exchange_done_orders_cb_t cb, void *user, const char *err)
{
  exchange_orders_result_t res;

  if(cb == NULL)
    return;

  memset(&res, 0, sizeof(res));
  snprintf(res.err, sizeof(res.err), "%s", err);
  cb(&res, user);
}

static void
kr_exch_fail_accounts(exchange_done_accounts_cb_t cb, void *user,
    const char *err)
{
  exchange_accounts_result_t res;

  if(cb == NULL)
    return;

  memset(&res, 0, sizeof(res));
  snprintf(res.err, sizeof(res.err), "%s", err);
  cb(&res, user);
}

static void
kr_exch_fail_fills(exchange_done_fills_cb_t cb, void *user, const char *err)
{
  exchange_fills_result_t res;

  if(cb == NULL)
    return;

  memset(&res, 0, sizeof(res));
  snprintf(res.err, sizeof(res.err), "%s", err);
  cb(&res, user);
}

static void
kr_exch_fail_candles(exchange_done_candles_cb_t cb, void *user,
    const char *err)
{
  exchange_candles_result_t res;

  if(cb == NULL)
    return;

  memset(&res, 0, sizeof(res));
  snprintf(res.err, sizeof(res.err), "%s", err);
  cb(&res, user);
}

// ---- granularity mapping ----

static bool
kr_exch_map_granularity(exchange_granularity_t gran, uint32_t *out_minutes)
{
  switch(gran)
  {
    case EXCH_GRAN_1M:  *out_minutes = KRAKEN_GRAN_1M;  return(SUCCESS);
    case EXCH_GRAN_5M:  *out_minutes = KRAKEN_GRAN_5M;  return(SUCCESS);
    case EXCH_GRAN_15M: *out_minutes = KRAKEN_GRAN_15M; return(SUCCESS);
    case EXCH_GRAN_30M: *out_minutes = KRAKEN_GRAN_30M; return(SUCCESS);
    case EXCH_GRAN_1H:  *out_minutes = KRAKEN_GRAN_1H;  return(SUCCESS);
    case EXCH_GRAN_4H:  *out_minutes = KRAKEN_GRAN_4H;  return(SUCCESS);
    case EXCH_GRAN_1D:  *out_minutes = KRAKEN_GRAN_1D;  return(SUCCESS);
    case EXCH_GRAN_1W:  *out_minutes = KRAKEN_GRAN_1W;  return(SUCCESS);
    default:                                            return(FAIL);
  }
}

// ---- trampolines ----

static bool
kr_exch_place_order_async(const exchange_place_order_req_t *req,
    exchange_done_order_cb_t cb, void *user)
{
  kraken_place_order_req_t  inner;
  kr_exch_order_fwd_t      *fwd;

  if(req == NULL || cb == NULL)
  {
    kr_exch_fail_order(cb, user, "invalid place_order arguments");
    return(FAIL);
  }

  fwd = mem_alloc(KR_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    kr_exch_fail_order(cb, user, "out of memory");
    return(FAIL);
  }

  fwd->cb   = cb;
  fwd->user = user;

  memset(&inner, 0, sizeof(inner));
  snprintf(inner.product_id, sizeof(inner.product_id), "%s", req->product_id);
  snprintf(inner.side,       sizeof(inner.side),       "%s", req->side);
  snprintf(inner.type,       sizeof(inner.type),       "%s", req->type);
  snprintf(inner.tif,        sizeof(inner.tif),        "%s", req->tif);
  inner.price     = req->price;
  inner.size      = req->size;
  inner.funds     = req->funds;
  inner.post_only = req->post_only;
  inner.validate  = false;
  snprintf(inner.client_oid, sizeof(inner.client_oid), "%s", req->client_oid);

  return(kraken_add_order_async(&inner, kr_exch_order_done_adapter, fwd));
}

static bool
kr_exch_cancel_order_async(const char *order_id,
    exchange_done_order_cb_t cb, void *user)
{
  kr_exch_order_fwd_t *fwd;

  if(cb == NULL)
    return(FAIL);

  if(order_id == NULL || order_id[0] == '\0')
  {
    kr_exch_fail_order(cb, user, "order_id required");
    return(FAIL);
  }

  fwd = mem_alloc(KR_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    kr_exch_fail_order(cb, user, "out of memory");
    return(FAIL);
  }

  fwd->cb   = cb;
  fwd->user = user;

  return(kraken_cancel_order_async(order_id, kr_exch_order_done_adapter, fwd));
}

static bool
kr_exch_get_order_async(const char *order_id,
    exchange_done_order_cb_t cb, void *user)
{
  kr_exch_order_fwd_t *fwd;

  if(cb == NULL)
    return(FAIL);

  if(order_id == NULL || order_id[0] == '\0')
  {
    kr_exch_fail_order(cb, user, "order_id required");
    return(FAIL);
  }

  fwd = mem_alloc(KR_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    kr_exch_fail_order(cb, user, "out of memory");
    return(FAIL);
  }

  fwd->cb   = cb;
  fwd->user = user;

  return(kraken_query_order_async(order_id, kr_exch_order_done_adapter, fwd));
}

// list_orders_async branches on `status`:
//   ""        / "open"   → OpenOrders
//   "closed"  / others    → ClosedOrders
// `product_id` is unused at the Kraken layer (server-side filtering
// not supported by Open/ClosedOrders); the trampoline could do client-
// side filtering but the generic surface doesn't expect a partial set.
// For v1 we just forward without filtering — callers that need a
// product-specific cut should walk the returned list.
static bool
kr_exch_list_orders_async(const char *status, const char *product_id,
    exchange_done_orders_cb_t cb, void *user)
{
  kr_exch_orders_fwd_t *fwd;
  bool                  want_closed;

  (void)product_id;

  if(cb == NULL)
    return(FAIL);

  fwd = mem_alloc(KR_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    kr_exch_fail_orders(cb, user, "out of memory");
    return(FAIL);
  }

  fwd->cb   = cb;
  fwd->user = user;

  want_closed = (status != NULL
              && (strcmp(status, "closed") == 0
                  || strcmp(status, "done")   == 0
                  || strcmp(status, "settled") == 0));

  if(want_closed)
    return(kraken_closed_orders_async(0, kr_exch_orders_done_adapter, fwd));

  return(kraken_open_orders_async(kr_exch_orders_done_adapter, fwd));
}

static bool
kr_exch_list_fills_async(const char *order_id, const char *product_id,
    int64_t start_ms, exchange_done_fills_cb_t cb, void *user)
{
  kr_exch_fills_fwd_t *fwd;
  int64_t              start_sec;

  if(cb == NULL)
    return(FAIL);

  fwd = mem_alloc(KR_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    kr_exch_fail_fills(cb, user, "out of memory");
    return(FAIL);
  }

  fwd->cb   = cb;
  fwd->user = user;

  start_sec = (start_ms > 0) ? (start_ms / 1000) : 0;

  return(kraken_trades_history_async(order_id, product_id, start_sec,
        kr_exch_fills_done_adapter, fwd));
}

static bool
kr_exch_get_accounts_async(exchange_done_accounts_cb_t cb, void *user)
{
  kr_exch_accounts_fwd_t *fwd;

  if(cb == NULL)
    return(FAIL);

  fwd = mem_alloc(KR_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    kr_exch_fail_accounts(cb, user, "out of memory");
    return(FAIL);
  }

  fwd->cb   = cb;
  fwd->user = user;

  return(kraken_get_balance_async(kr_exch_accounts_done_adapter, fwd));
}

static bool
kr_exch_fetch_candles_async(const char *product_id,
    exchange_granularity_t gran, int64_t since_ms, int64_t until_ms,
    exchange_done_candles_cb_t cb, void *user)
{
  kr_exch_candles_fwd_t *fwd;
  uint32_t               interval_min = 0;
  int64_t                since_sec;

  if(cb == NULL)
    return(FAIL);

  if(product_id == NULL || product_id[0] == '\0')
  {
    kr_exch_fail_candles(cb, user, "product_id required");
    return(FAIL);
  }

  if(kr_exch_map_granularity(gran, &interval_min) != SUCCESS)
  {
    kr_exch_fail_candles(cb, user, "unsupported granularity for Kraken");
    return(FAIL);
  }

  fwd = mem_alloc(KR_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    kr_exch_fail_candles(cb, user, "out of memory");
    return(FAIL);
  }

  fwd->cb       = cb;
  fwd->user     = user;
  fwd->until_ms = until_ms;

  since_sec = (since_ms > 0) ? (since_ms / 1000) : 0;

  if(kraken_fetch_candles_async(product_id, interval_min, since_sec,
        EXCHANGE_PRIO_MARKET_BACKFILL,
        kr_exch_candles_done_adapter, fwd) != SUCCESS)
  {
    // kraken_fetch_candles_async fires the typed cb synchronously on
    // FAIL; the adapter has already freed `fwd`.
    return(FAIL);
  }

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// File-scope vtable. Static so the abstraction can keep the pointer;  //
// advertised_rps reflects Kraken Spot's tier-2 public API budget      //
// (~1 req/s sustained, 15-call burst).                                 //
// ------------------------------------------------------------------ //

static const exchange_protocol_vtable_t kr_vtable =
{
  .build_request       = kr_exchange_build_request,
  .submit              = kr_exchange_submit,
  .free_request        = kr_exchange_free_request,
  .advertised_rps      = 1,
  .advertised_burst    = 15,

  .is_authenticated    = kr_exch_is_authenticated,
  .is_sandbox          = NULL,           // Kraken has no sandbox
  .place_order_async   = kr_exch_place_order_async,
  .cancel_order_async  = kr_exch_cancel_order_async,
  .get_order_async     = kr_exch_get_order_async,
  .list_orders_async   = kr_exch_list_orders_async,
  .list_fills_async    = kr_exch_list_fills_async,
  .get_accounts_async  = kr_exch_get_accounts_async,
  .fetch_candles_async = kr_exch_fetch_candles_async,

  // KR-5: WS slots forward directly into the channel multiplexer; the
  // multiplexer owns its own opaque handle (`exchange_ws_sub_t`) so no
  // extra trampoline is needed at this layer.
  .ws_subscribe        = kr_ws_subscribe,
  .ws_unsubscribe      = kr_ws_unsubscribe,
};

bool
kr_exchange_register_vtable(void)
{
  return(exchange_register("kraken", &kr_vtable));
}
