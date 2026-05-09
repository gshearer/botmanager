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

static bool
cb_exch_is_sandbox(void)
{
  return(cb_sandbox_enabled());
}

static bool
cb_exch_get_products_count(uint32_t *out_count, uint32_t *out_active)
{
  return(cb_products_count_locked(out_count, out_active));
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

// File-scope vtable. Static storage so the abstraction can keep the
// pointer; advertised_rps reflects Coinbase Exchange's public-API cap.
static const exchange_protocol_vtable_t cb_vtable = {
  .build_request      = cb_exchange_build_request,
  .submit             = cb_exchange_submit,
  .free_request       = cb_exchange_free_request,
  .advertised_rps     = 10,
  .advertised_burst   = 15,

  // WM-OR-1 capability hooks.
  .is_authenticated   = cb_exch_is_authenticated,
  .is_sandbox         = cb_exch_is_sandbox,
  .get_products_count = cb_exch_get_products_count,
  .place_order_async  = cb_exch_place_order_async,
  .cancel_order_async = cb_exch_cancel_order_async,
  .get_order_async    = cb_exch_get_order_async,
  .list_orders_async  = cb_exch_list_orders_async,
  .list_fills_async   = cb_exch_list_fills_async,
  .get_accounts_async = cb_exch_get_accounts_async,
};

bool
cb_exchange_register_vtable(void)
{
  // WM-DC-1: register under the latched name so sandbox and prod
  // occupy distinct registry slots. cb_active_exchange_name() resolves
  // to "coinbase-sb" when plugin.coinbase.sandbox is true at plugin
  // init, "coinbase" otherwise.
  return(exchange_register(cb_active_exchange_name(), &cb_vtable));
}
