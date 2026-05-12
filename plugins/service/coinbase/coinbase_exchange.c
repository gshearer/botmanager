// botmanager — MIT
// Coinbase exchange-vtable: bridges feature_exchange to the existing
// curl-backed REST submitter (cb_submit_public / cb_submit_private).
//
// EX-1: candle + trade traffic moved off the direct typed-completion
// path onto the exchange abstraction. Other typed APIs (products,
// ticker, orders, accounts) still call cb_submit_* directly — moving
// them is deferred until the live-trading framework is unpaused (see
// EX-1 outcomes in TODO.md).
//
// WM-LT-8-A: lifted the is_private → FAIL gate. With cb_submit_private
// now mirroring cb_submit_public's (void *user_data, uint8_t prio)
// shape, EXCHANGE_OP_PRIVATE_REST_{GET,POST,DELETE} routes through
// cb_submit_private exactly as public traffic routes through
// cb_submit_public. The kill-switch + risk gates that prevent live
// trading from emitting actual orders live in WM-LT-8-B at the
// whenmoon trade-engine layer, not here — this layer is intentionally
// thin (build/submit/free + curl→exchange response adapter).
//
// The vtable handle is a small heap struct that holds the (kind, path,
// body) triple plus the abstraction's response cb + user pointer.
// build_request just allocates the handle (no curl work yet); submit
// wires the handle to a curl request whose `done_cb` adapts
// curl_response_t → exchange_response_cb_t. free_request is the final
// release path.

#define CB_INTERNAL
#include "coinbase.h"
#include "exchange_api.h"

#include "curl.h"

#include <stdio.h>
#include <string.h>

#define CB_EXCHANGE_PATH_SZ  CB_URL_SZ
#define CB_EXCHANGE_BODY_SZ  CB_BODY_SZ

typedef struct
{
  exchange_op_kind_t      kind;

  // Path / body copied at build_request time so the abstraction may
  // free its caller-supplied strings before submit runs.
  char                    path[CB_EXCHANGE_PATH_SZ];
  char                   *body;     // NULL when body_len == 0
  size_t                  body_len;

  // Wired in submit. Routed back through cb_exchange_curl_done.
  exchange_response_cb_t  abstr_cb;
  void                   *abstr_user;
} cb_exchange_handle_t;

// ------------------------------------------------------------------ //
// curl completion adapter                                             //
// ------------------------------------------------------------------ //

static void
cb_exchange_curl_done(const curl_response_t *resp)
{
  cb_exchange_handle_t   *h    = (cb_exchange_handle_t *)resp->user_data;
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
      // Cancelled by the curl shutdown drain. Surface http_status=-1
      // so exchange_classify_status maps to FAIL (no retry); a
      // re-submit would be rejected by the same gate that fired the
      // cancellation.
      cb(-1, NULL, 0,
          resp->error != NULL ? resp->error : "request cancelled",
          user);
    }

    else if(transport_err)
    {
      // Surface a transport error with http_status==0 so the
      // abstraction's classifier kicks the retry path.
      cb(0, NULL, 0,
          resp->error != NULL ? resp->error : "transport error",
          user);
    }

    else
    {
      cb((int)resp->status, resp->body, resp->body_len, NULL, user);
    }
  }

  // The abstraction owns the handle's lifecycle via free_request — it
  // will fire shortly after this returns (success path) or after retry
  // exhaustion (which uses a different handle each attempt, since
  // exchange_arm_retry calls free_request between attempts).
}

// ------------------------------------------------------------------ //
// vtable                                                              //
// ------------------------------------------------------------------ //

static bool
cb_exchange_build_request(exchange_op_kind_t kind, const char *path,
    const char *body_json, void **out_handle)
{
  cb_exchange_handle_t *h;
  size_t                plen;

  if(out_handle == NULL)
    return(FAIL);

  *out_handle = NULL;

  if(path == NULL || path[0] == '\0')
    return(FAIL);

  plen = strnlen(path, CB_EXCHANGE_PATH_SZ);

  if(plen >= CB_EXCHANGE_PATH_SZ)
  {
    clam(CLAM_WARN, CB_CTX,
        "exchange build: path too long (>%zu)",
        (size_t)(CB_EXCHANGE_PATH_SZ - 1));
    return(FAIL);
  }

  h = mem_alloc("coinbase.exch", "handle", sizeof(*h));

  if(h == NULL)
    return(FAIL);

  memset(h, 0, sizeof(*h));
  h->kind = kind;
  memcpy(h->path, path, plen);
  h->path[plen] = '\0';

  if(body_json != NULL && body_json[0] != '\0')
  {
    h->body_len = strnlen(body_json, CB_EXCHANGE_BODY_SZ);

    if(h->body_len >= CB_EXCHANGE_BODY_SZ)
    {
      clam(CLAM_WARN, CB_CTX,
          "exchange build: body too long (>%zu)",
          (size_t)(CB_EXCHANGE_BODY_SZ - 1));
      mem_free(h);
      return(FAIL);
    }

    h->body = mem_alloc("coinbase.exch", "body", h->body_len + 1);

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

// WM-LT-8-A: map exchange op-kind to curl HTTP method for the private
// (signed) submit path. Public ops are GET-only and do not pass
// through here.
static curl_method_t
cb_exchange_method_for_kind(exchange_op_kind_t kind)
{
  switch(kind)
  {
    case EXCHANGE_OP_PRIVATE_REST_POST:   return(CURL_METHOD_POST);
    case EXCHANGE_OP_PRIVATE_REST_DELETE: return(CURL_METHOD_DELETE);
    case EXCHANGE_OP_PRIVATE_REST_GET:    return(CURL_METHOD_GET);
    default:                              return(CURL_METHOD_GET);
  }
}

static bool
cb_exchange_submit(void *handle, uint8_t prio,
    exchange_response_cb_t cb, void *user)
{
  cb_exchange_handle_t *h = handle;
  bool                  is_private;

  if(h == NULL || cb == NULL)
    return(FAIL);

  h->abstr_cb   = cb;
  h->abstr_user = user;

  is_private = (h->kind == EXCHANGE_OP_PRIVATE_REST_GET
             || h->kind == EXCHANGE_OP_PRIVATE_REST_POST
             || h->kind == EXCHANGE_OP_PRIVATE_REST_DELETE);

  // The handle `h` is the curl request's user_data either way, so
  // cb_exchange_curl_done sees it directly. Priority byte threads
  // through unchanged (CURL-PRIO-3).
  if(is_private)
  {
    if(cb_submit_private(h, prio,
          cb_exchange_method_for_kind(h->kind),
          h->path, h->body, h->body_len,
          cb_exchange_curl_done) != SUCCESS)
      return(FAIL);
  }
  else
  {
    if(cb_submit_public(h, prio, h->path, cb_exchange_curl_done) != SUCCESS)
      return(FAIL);
  }

  return(SUCCESS);
}

static void
cb_exchange_free_request(void *handle)
{
  cb_exchange_handle_t *h = handle;

  if(h == NULL)
    return;

  if(h->body != NULL)
    mem_free(h->body);

  mem_free(h);
}

// ------------------------------------------------------------------ //
// Capability hooks (WM-OR-1).                                         //
//                                                                      //
// Thin trampolines around the existing coinbase_*_async typed         //
// wrappers. Each verb allocates a small heap context wrapping the    //
// caller's exchange typed callback + user pointer, hands a coinbase  //
// typed callback to the inner async, and translates the typed       //
// result on the way back. Translation is field-by-field memcpy /     //
// snprintf — exchange_*_t buffers are sized at-or-above their        //
// coinbase_*_t equivalents, so no field can truncate.                //
//                                                                      //
// FAIL contract: the inner coinbase_*_async fires its typed cb with   //
// `err` populated on every FAIL path *except* `req == NULL`. The      //
// trampoline always passes non-NULL (its own translated request), so  //
// the adapter cb runs on every FAIL — and it forwards the err to the  //
// user's exchange typed cb. Consumers therefore see exactly one cb   //
// invocation per call regardless of where the failure originated.    //
// ------------------------------------------------------------------ //

static bool
cb_exch_is_authenticated(void)
{
  return(cb_apikey_configured());
}

// Per-call adapter contexts. Sized for trivial forwarding; allocated
// per dispatch and freed by the adapter cb.

typedef struct
{
  exchange_done_order_cb_t cb;
  void                    *user;
} cb_exch_order_fwd_t;

typedef struct
{
  exchange_done_orders_cb_t cb;
  void                     *user;
} cb_exch_orders_fwd_t;

typedef struct
{
  exchange_done_accounts_cb_t cb;
  void                       *user;
} cb_exch_accounts_fwd_t;

typedef struct
{
  exchange_done_fills_cb_t cb;
  void                    *user;
} cb_exch_fills_fwd_t;

typedef struct
{
  exchange_done_candles_cb_t cb;
  void                      *user;
} cb_exch_candles_fwd_t;

// KR-2: WS subscriber state. The protocol-side coinbase_ws_sub_t is
// opaque to the exchange layer; we wrap it together with the user-
// supplied exchange typed callback + user pointer. The wrapper itself
// IS the `exchange_ws_sub_t` handle exposed upward — we cast it.
typedef struct exchange_ws_sub
{
  coinbase_ws_sub_t       *inner;
  exchange_ws_event_cb_t   user_cb;
  void                    *user;
} cb_exch_ws_sub_t;

// Field-by-field translation. exchange_order_t is a strict superset
// in slot size; copy via snprintf for strings (no buffer overrun) and
// direct assignment for scalars.
static void
cb_to_exch_order(const coinbase_order_t *src, exchange_order_t *dst)
{
  memset(dst, 0, sizeof(*dst));
  snprintf(dst->order_id,   sizeof(dst->order_id),   "%s", src->order_id);
  snprintf(dst->client_oid, sizeof(dst->client_oid), "%s", src->client_oid);
  snprintf(dst->product_id, sizeof(dst->product_id), "%s", src->product_id);
  snprintf(dst->side,       sizeof(dst->side),       "%s", src->side);
  snprintf(dst->type,       sizeof(dst->type),       "%s", src->type);
  snprintf(dst->status,     sizeof(dst->status),     "%s", src->status);
  snprintf(dst->tif,         sizeof(dst->tif),        "%s", src->tif);
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
cb_to_exch_account(const coinbase_account_t *src, exchange_account_t *dst)
{
  memset(dst, 0, sizeof(*dst));
  snprintf(dst->currency, sizeof(dst->currency), "%s", src->currency);
  dst->balance   = src->balance;
  dst->hold      = src->hold;
  dst->available = src->available;
}

static void
cb_to_exch_fill(const coinbase_fill_t *src, exchange_fill_t *dst)
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

// Coinbase candles report `time` in seconds since epoch; widen to ms
// for the generic surface.
static void
cb_to_exch_candle(const coinbase_candle_t *src, exchange_candle_t *dst)
{
  memset(dst, 0, sizeof(*dst));
  dst->ts_open_ms = src->time * 1000;
  dst->open       = src->open;
  dst->high       = src->high;
  dst->low        = src->low;
  dst->close      = src->close;
  dst->volume     = src->volume;
}

// Adapter callbacks.

static void
cb_exch_order_done_adapter(const coinbase_order_result_t *res, void *user)
{
  cb_exch_order_fwd_t      *fwd = user;
  exchange_order_result_t   out;

  if(fwd == NULL)
    return;

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);
  cb_to_exch_order(&res->order, &out.order);

  if(fwd->cb != NULL)
    fwd->cb(&out, fwd->user);

  mem_free(fwd);
}

static void
cb_exch_orders_done_adapter(const coinbase_orders_result_t *res, void *user)
{
  cb_exch_orders_fwd_t     *fwd = user;
  exchange_orders_result_t  out;
  uint32_t                  i;
  uint32_t                  n;

  if(fwd == NULL)
    return;

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);

  n = res->count;

  if(n > EXCHANGE_MAX_ORDERS_LIST)
    n = EXCHANGE_MAX_ORDERS_LIST;

  for(i = 0; i < n; i++)
    cb_to_exch_order(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd->cb != NULL)
    fwd->cb(&out, fwd->user);

  mem_free(fwd);
}

static void
cb_exch_accounts_done_adapter(const coinbase_accounts_result_t *res,
    void *user)
{
  cb_exch_accounts_fwd_t      *fwd = user;
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
    cb_to_exch_account(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd->cb != NULL)
    fwd->cb(&out, fwd->user);

  mem_free(fwd);
}

static void
cb_exch_fills_done_adapter(const coinbase_fills_result_t *res, void *user)
{
  cb_exch_fills_fwd_t       *fwd = user;
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
    cb_to_exch_fill(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd->cb != NULL)
    fwd->cb(&out, fwd->user);

  mem_free(fwd);
}

static void
cb_exch_candles_done_adapter(const coinbase_candles_result_t *res,
    void *user)
{
  cb_exch_candles_fwd_t      *fwd = user;
  exchange_candles_result_t   out;
  uint32_t                    i;
  uint32_t                    n;

  if(fwd == NULL)
    return;

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);

  n = res->count;

  if(n > EXCHANGE_MAX_CANDLES)
    n = EXCHANGE_MAX_CANDLES;

  for(i = 0; i < n; i++)
    cb_to_exch_candle(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd->cb != NULL)
    fwd->cb(&out, fwd->user);

  mem_free(fwd);
}

// Trampolines. Each FAIL path that returns before the inner async
// fires must invoke the user cb itself (via the adapter helpers
// below) so the FAIL → cb-fires invariant holds.

static void
cb_exch_fail_order(exchange_done_order_cb_t cb, void *user,
    const char *err)
{
  exchange_order_result_t res;

  if(cb == NULL)
    return;

  memset(&res, 0, sizeof(res));
  snprintf(res.err, sizeof(res.err), "%s", err);
  cb(&res, user);
}

static bool
cb_exch_place_order_async(const exchange_place_order_req_t *req,
    exchange_done_order_cb_t cb, void *user)
{
  coinbase_place_order_req_t  inner;
  cb_exch_order_fwd_t        *fwd;

  if(req == NULL || cb == NULL)
  {
    cb_exch_fail_order(cb, user, "invalid place_order arguments");
    return(FAIL);
  }

  fwd = mem_alloc(CB_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    cb_exch_fail_order(cb, user, "out of memory");
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
  snprintf(inner.client_oid, sizeof(inner.client_oid), "%s", req->client_oid);

  return(coinbase_place_order_async(&inner,
        cb_exch_order_done_adapter, fwd));
}

static bool
cb_exch_cancel_order_async(const char *order_id,
    exchange_done_order_cb_t cb, void *user)
{
  cb_exch_order_fwd_t *fwd;

  if(cb == NULL)
    return(FAIL);

  if(order_id == NULL || order_id[0] == '\0')
  {
    cb_exch_fail_order(cb, user, "order_id required");
    return(FAIL);
  }

  fwd = mem_alloc(CB_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    cb_exch_fail_order(cb, user, "out of memory");
    return(FAIL);
  }

  fwd->cb   = cb;
  fwd->user = user;

  return(coinbase_cancel_order_async(order_id,
        cb_exch_order_done_adapter, fwd));
}

static bool
cb_exch_get_order_async(const char *order_id,
    exchange_done_order_cb_t cb, void *user)
{
  cb_exch_order_fwd_t *fwd;

  if(cb == NULL)
    return(FAIL);

  if(order_id == NULL || order_id[0] == '\0')
  {
    cb_exch_fail_order(cb, user, "order_id required");
    return(FAIL);
  }

  fwd = mem_alloc(CB_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    cb_exch_fail_order(cb, user, "out of memory");
    return(FAIL);
  }

  fwd->cb   = cb;
  fwd->user = user;

  return(coinbase_get_order_async(order_id,
        cb_exch_order_done_adapter, fwd));
}

static bool
cb_exch_list_orders_async(const char *status, const char *product_id,
    exchange_done_orders_cb_t cb, void *user)
{
  cb_exch_orders_fwd_t *fwd;

  if(cb == NULL)
    return(FAIL);

  fwd = mem_alloc(CB_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    exchange_orders_result_t res;

    memset(&res, 0, sizeof(res));
    snprintf(res.err, sizeof(res.err), "%s", "out of memory");
    cb(&res, user);
    return(FAIL);
  }

  fwd->cb   = cb;
  fwd->user = user;

  return(coinbase_list_orders_async(status, product_id,
        cb_exch_orders_done_adapter, fwd));
}

static bool
cb_exch_list_fills_async(const char *order_id, const char *product_id,
    int64_t start_ms, exchange_done_fills_cb_t cb, void *user)
{
  cb_exch_fills_fwd_t *fwd;

  if(cb == NULL)
    return(FAIL);

  fwd = mem_alloc(CB_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    exchange_fills_result_t res;

    memset(&res, 0, sizeof(res));
    snprintf(res.err, sizeof(res.err), "%s", "out of memory");
    cb(&res, user);
    return(FAIL);
  }

  fwd->cb   = cb;
  fwd->user = user;

  return(coinbase_list_fills_async(order_id, product_id, start_ms,
        cb_exch_fills_done_adapter, fwd));
}

static bool
cb_exch_get_accounts_async(exchange_done_accounts_cb_t cb, void *user)
{
  cb_exch_accounts_fwd_t *fwd;

  if(cb == NULL)
    return(FAIL);

  fwd = mem_alloc(CB_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    exchange_accounts_result_t res;

    memset(&res, 0, sizeof(res));
    snprintf(res.err, sizeof(res.err), "%s", "out of memory");
    cb(&res, user);
    return(FAIL);
  }

  fwd->cb   = cb;
  fwd->user = user;

  return(coinbase_get_accounts_async(cb_exch_accounts_done_adapter, fwd));
}

// ------------------------------------------------------------------ //
// KR-2: candle fetch                                                  //
// ------------------------------------------------------------------ //

static void
cb_exch_fail_candles(exchange_done_candles_cb_t cb, void *user,
    const char *err)
{
  exchange_candles_result_t res;

  if(cb == NULL)
    return;

  memset(&res, 0, sizeof(res));
  snprintf(res.err, sizeof(res.err), "%s", err != NULL ? err : "error");
  cb(&res, user);
}

// Map the neutral seconds-valued enum to coinbase's int32 granularity
// constant. Returns FAIL when the protocol doesn't support `gran`.
static bool
cb_exch_map_granularity(exchange_granularity_t gran, int32_t *out)
{
  switch(gran)
  {
    case EXCH_GRAN_1M:  *out = COINBASE_GRAN_1M;   return(SUCCESS);
    case EXCH_GRAN_5M:  *out = COINBASE_GRAN_5M;   return(SUCCESS);
    case EXCH_GRAN_15M: *out = COINBASE_GRAN_15M;  return(SUCCESS);
    case EXCH_GRAN_1H:  *out = COINBASE_GRAN_1H;   return(SUCCESS);
    case EXCH_GRAN_1D:  *out = COINBASE_GRAN_1D;   return(SUCCESS);

    // Coinbase Advanced Trade also publishes 6h candles natively; the
    // generic ladder does not reserve a slot for it, so it has no
    // EXCH_GRAN_ peer. EXCH_GRAN_30M / _4H / _1W are unsupported by
    // Coinbase REST today — surface a clean refusal so the caller can
    // fall back to a different grain or down-sample client-side.
    case EXCH_GRAN_30M:
    case EXCH_GRAN_4H:
    case EXCH_GRAN_1W:
    default:
      return(FAIL);
  }
}

static bool
cb_exch_fetch_candles_async(const char *product_id,
    exchange_granularity_t gran, int64_t since_ms, int64_t until_ms,
    exchange_done_candles_cb_t cb, void *user)
{
  cb_exch_candles_fwd_t *fwd;
  int32_t                cb_gran = 0;
  int64_t                start_s;
  int64_t                end_s;

  if(cb == NULL)
    return(FAIL);

  if(product_id == NULL || product_id[0] == '\0')
  {
    cb_exch_fail_candles(cb, user, "product_id required");
    return(FAIL);
  }

  if(cb_exch_map_granularity(gran, &cb_gran) != SUCCESS)
  {
    cb_exch_fail_candles(cb, user, "unsupported granularity");
    return(FAIL);
  }

  fwd = mem_alloc(CB_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    cb_exch_fail_candles(cb, user, "out of memory");
    return(FAIL);
  }

  fwd->cb   = cb;
  fwd->user = user;

  // Coinbase REST takes seconds (and treats `end` as inclusive); narrow
  // the millisecond window to seconds for the underlying call. The
  // upstream `coinbase_fetch_candles_async` shim already enforces the
  // 300-bucket cap.
  start_s = since_ms > 0 ? since_ms / 1000 : 0;
  end_s   = until_ms > 0 ? until_ms / 1000 : 0;

  if(coinbase_fetch_candles_async(product_id, cb_gran, start_s, end_s,
        EXCHANGE_PRIO_MARKET_BACKFILL,
        cb_exch_candles_done_adapter, fwd) != SUCCESS)
  {
    // coinbase_fetch_candles_async fires the typed cb synchronously with
    // err set on FAIL; the adapter has already freed `fwd`.
    return(FAIL);
  }

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// KR-2: WS subscribe / unsubscribe                                    //
// ------------------------------------------------------------------ //

static void
cb_to_exch_ws_ticker(const coinbase_ws_ticker_t *src,
    exchange_ws_ticker_t *dst)
{
  memset(dst, 0, sizeof(*dst));
  snprintf(dst->product_id, sizeof(dst->product_id), "%s", src->product_id);
  dst->price      = src->price;
  dst->best_bid   = src->best_bid;
  dst->best_ask   = src->best_ask;
  dst->volume_24h = src->volume_24h;
  dst->low_24h    = src->low_24h;
  dst->high_24h   = src->high_24h;
  dst->time_ms    = src->time_ms;
}

static void
cb_to_exch_ws_match(const coinbase_ws_match_t *src,
    exchange_ws_match_t *dst)
{
  memset(dst, 0, sizeof(*dst));
  snprintf(dst->product_id, sizeof(dst->product_id), "%s", src->product_id);
  snprintf(dst->side,       sizeof(dst->side),       "%s", src->side);
  dst->trade_id = src->trade_id;
  dst->price    = src->price;
  dst->size     = src->size;
  dst->time_ms  = src->time_ms;
}

static void
cb_to_exch_ws_user_order(const coinbase_ws_user_order_t *src,
    exchange_ws_user_order_t *dst)
{
  memset(dst, 0, sizeof(*dst));
  snprintf(dst->order_id,        sizeof(dst->order_id),
      "%s", src->order_id);
  snprintf(dst->client_order_id, sizeof(dst->client_order_id),
      "%s", src->client_order_id);
  snprintf(dst->product_id,      sizeof(dst->product_id),
      "%s", src->product_id);
  snprintf(dst->side,            sizeof(dst->side),
      "%s", src->side);
  snprintf(dst->status,          sizeof(dst->status),
      "%s", src->status);
  dst->limit_price         = src->limit_price;
  dst->cumulative_quantity = src->cumulative_quantity;
  dst->leaves_quantity     = src->leaves_quantity;
  dst->avg_price           = src->avg_price;
  dst->total_fees          = src->total_fees;
  dst->creation_time_ms    = src->creation_time_ms;
  dst->time_ms             = src->time_ms;
}

static void
cb_to_exch_ws_user_fill(const coinbase_ws_user_fill_t *src,
    exchange_ws_user_fill_t *dst)
{
  memset(dst, 0, sizeof(*dst));
  snprintf(dst->order_id,        sizeof(dst->order_id),
      "%s", src->order_id);
  snprintf(dst->client_order_id, sizeof(dst->client_order_id),
      "%s", src->client_order_id);
  snprintf(dst->product_id,      sizeof(dst->product_id),
      "%s", src->product_id);
  snprintf(dst->side,            sizeof(dst->side),
      "%s", src->side);
  dst->trade_id = src->trade_id;
  dst->price    = src->price;
  dst->size     = src->size;
  dst->fee      = src->fee;
  dst->time_ms  = src->time_ms;
}

// Map the neutral channel enum to the protocol-native value. Returns
// FAIL when the channel has no coinbase peer (e.g. Kraken-only book/
// ohlc channels).
static bool
cb_exch_map_ws_channel(exchange_ws_channel_t in, coinbase_ws_channel_t *out)
{
  switch(in)
  {
    case EXCH_WS_TICKER: *out = COINBASE_CH_TICKER;   return(SUCCESS);
    case EXCH_WS_TRADES: *out = COINBASE_CH_MATCHES;  return(SUCCESS);
    case EXCH_WS_USER:   *out = COINBASE_CH_USER;     return(SUCCESS);

    // Heartbeat is implicit at the coinbase WS layer (see
    // coinbase_ws_subscribe docstring). Book/OHLC are not yet wired on
    // the coinbase side — reject so callers see a deterministic refusal.
    case EXCH_WS_BOOK_L2:
    case EXCH_WS_OHLC_1M:
    default:
      return(FAIL);
  }
}

// Per-event adapter: translate a coinbase WS event into the neutral
// shape, then invoke the user callback. `user` is the wrapper handle
// (cb_exch_ws_sub_t *) — sized so we keep both the inner coinbase
// handle and the user's typed callback in one allocation.
static void
cb_exch_ws_event_adapter(const coinbase_ws_event_t *ev, void *user)
{
  cb_exch_ws_sub_t    *sub = user;
  exchange_ws_event_t  out;

  if(sub == NULL || sub->user_cb == NULL || ev == NULL)
    return;

  memset(&out, 0, sizeof(out));

  if(ev->product_id != NULL)
    snprintf(out.product_id, sizeof(out.product_id), "%s", ev->product_id);

  switch(ev->channel)
  {
    case COINBASE_CH_TICKER:
    case COINBASE_CH_TICKER_BATCH:
    {
      const coinbase_ws_ticker_t *t = ev->payload;

      if(t == NULL)
        return;

      out.channel = EXCH_WS_TICKER;
      cb_to_exch_ws_ticker(t, &out.payload.ticker);
      break;
    }

    case COINBASE_CH_MATCHES:
    {
      const coinbase_ws_match_t *m = ev->payload;

      if(m == NULL)
        return;

      out.channel = EXCH_WS_TRADES;
      cb_to_exch_ws_match(m, &out.payload.match);
      break;
    }

    case COINBASE_CH_USER:
    {
      const coinbase_ws_user_event_t *u = ev->payload;

      if(u == NULL)
        return;

      out.channel = EXCH_WS_USER;

      switch(u->kind)
      {
        case COINBASE_WS_USER_KIND_ORDER:
          out.payload.user.kind = EXCH_WS_USER_KIND_ORDER;
          cb_to_exch_ws_user_order(&u->u.order, &out.payload.user.u.order);
          break;
        case COINBASE_WS_USER_KIND_FILL:
          out.payload.user.kind = EXCH_WS_USER_KIND_FILL;
          cb_to_exch_ws_user_fill(&u->u.fill, &out.payload.user.u.fill);
          break;
      }
      break;
    }

    // Heartbeat / status / book / full are implicit or unhandled — the
    // coinbase plugin still fires them but the abstraction has no
    // consumer surface for them, so drop on the floor.
    default:
      return;
  }

  sub->user_cb(&out, sub->user);
}

static bool
cb_exch_ws_subscribe(const exchange_ws_channel_t *channels,
    uint32_t n_channels, const char *const *product_ids,
    uint32_t n_products, exchange_ws_event_cb_t cb, void *user,
    exchange_ws_sub_t **out_handle)
{
  cb_exch_ws_sub_t      *sub;
  coinbase_ws_channel_t  cb_chans[COINBASE_CH__COUNT];
  uint32_t               cb_n = 0;
  uint32_t               i;

  if(out_handle == NULL)
    return(FAIL);

  *out_handle = NULL;

  if(cb == NULL || channels == NULL || n_channels == 0)
    return(FAIL);

  for(i = 0; i < n_channels && cb_n < COINBASE_CH__COUNT; i++)
  {
    coinbase_ws_channel_t mapped;

    if(cb_exch_map_ws_channel(channels[i], &mapped) != SUCCESS)
    {
      clam(CLAM_WARN, CB_CTX,
          "ws_subscribe: channel %d not supported on coinbase",
          (int)channels[i]);
      return(FAIL);
    }

    cb_chans[cb_n++] = mapped;
  }

  sub = mem_alloc(CB_CTX, "exch.ws_sub", sizeof(*sub));

  if(sub == NULL)
    return(FAIL);

  sub->user_cb = cb;
  sub->user    = user;
  sub->inner   = coinbase_ws_subscribe(cb_chans, cb_n,
      product_ids, n_products,
      cb_exch_ws_event_adapter, sub);

  if(sub->inner == NULL)
  {
    mem_free(sub);
    return(FAIL);
  }

  // The wrapper IS the handle exposed upward — cast directly. The
  // opaque `exchange_ws_sub_t` forward-decl in exchange_api.h binds to
  // this struct at the cb_exch_ws_sub_t typedef site.
  *out_handle = sub;
  return(SUCCESS);
}

static void
cb_exch_ws_unsubscribe(exchange_ws_sub_t *handle)
{
  cb_exch_ws_sub_t *sub = handle;

  if(sub == NULL)
    return;

  if(sub->inner != NULL)
    coinbase_ws_unsubscribe(sub->inner);

  mem_free(sub);
}

// File-scope vtable. Static storage so the abstraction can keep the
// pointer; advertised_rps reflects Coinbase Exchange's public-API cap.
static const exchange_protocol_vtable_t cb_vtable = {
  .build_request       = cb_exchange_build_request,
  .submit              = cb_exchange_submit,
  .free_request        = cb_exchange_free_request,
  .advertised_rps      = 10,
  .advertised_burst    = 15,

  // WM-OR-1 capability hooks.
  .is_authenticated    = cb_exch_is_authenticated,
  .is_sandbox          = NULL,
  .place_order_async   = cb_exch_place_order_async,
  .cancel_order_async  = cb_exch_cancel_order_async,
  .get_order_async     = cb_exch_get_order_async,
  .list_orders_async   = cb_exch_list_orders_async,
  .list_fills_async    = cb_exch_list_fills_async,
  .get_accounts_async  = cb_exch_get_accounts_async,

  // KR-2 capability hooks.
  .fetch_candles_async = cb_exch_fetch_candles_async,
  .ws_subscribe        = cb_exch_ws_subscribe,
  .ws_unsubscribe      = cb_exch_ws_unsubscribe,
};

bool
cb_exchange_register_vtable(void)
{
  return(exchange_register("coinbase", &cb_vtable));
}
