// botmanager — MIT
// Gemini exchange-vtable: bridges feature_exchange to the gemini REST
// surface (gem_submit_public / gem_submit_private).
//
// GEM-1 ships the skeleton: the three mandatory vtable slots
// (build_request / submit / free_request), the is_authenticated probe,
// and the advertised rps/burst budget. Every capability hook (place
// order, cancel order, fetch candles, ws subscribe, …) is NULL, which
// makes the public `exchange_*_async` shims FAIL synchronously with
// the abstraction's standard "verb unsupported" string.
//
// GEM-2 fills in the REST capability hooks (candles, balances,
// orders, fills). GEM-3 wires the WS subscribe / unsubscribe slots
// into the channel multiplexer.
#define GEM_INTERNAL
#include "gemini.h"

#include "exchange_api.h"

#include <stdio.h>
#include <string.h>

#define GEM_EXCHANGE_PATH_SZ  GEM_URL_SZ
#define GEM_EXCHANGE_BODY_SZ  GEM_BODY_SZ

typedef struct
{
  exchange_op_kind_t      kind;

  char                    path[GEM_EXCHANGE_PATH_SZ];
  char                   *body;
  size_t                  body_len;

  exchange_response_cb_t  abstr_cb;
  void                   *abstr_user;
} gem_exchange_handle_t;

// ------------------------------------------------------------------ //
// curl completion adapter                                             //
// ------------------------------------------------------------------ //

static void
gem_exchange_curl_done(const curl_response_t *resp)
{
  gem_exchange_handle_t  *h = (gem_exchange_handle_t *)resp->user_data;
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
gem_exchange_build_request(exchange_op_kind_t kind, const char *path,
    const char *body_json, void **out_handle)
{
  gem_exchange_handle_t *h;
  size_t                 plen;

  if(out_handle == NULL)
    return(FAIL);

  *out_handle = NULL;

  if(path == NULL || path[0] == '\0')
    return(FAIL);

  plen = strnlen(path, GEM_EXCHANGE_PATH_SZ);

  if(plen >= GEM_EXCHANGE_PATH_SZ)
  {
    clam(CLAM_WARN, GEM_CTX,
        "exchange build: path too long (>%zu)",
        (size_t)(GEM_EXCHANGE_PATH_SZ - 1));
    return(FAIL);
  }

  h = mem_alloc(GEM_CTX ".exch", "handle", sizeof(*h));

  if(h == NULL)
    return(FAIL);

  memset(h, 0, sizeof(*h));
  h->kind = kind;
  memcpy(h->path, path, plen);
  h->path[plen] = '\0';

  if(body_json != NULL && body_json[0] != '\0')
  {
    h->body_len = strnlen(body_json, GEM_EXCHANGE_BODY_SZ);

    if(h->body_len >= GEM_EXCHANGE_BODY_SZ)
    {
      clam(CLAM_WARN, GEM_CTX,
          "exchange build: body too long (>%zu)",
          (size_t)(GEM_EXCHANGE_BODY_SZ - 1));
      mem_free(h);
      return(FAIL);
    }

    h->body = mem_alloc(GEM_CTX ".exch", "body", h->body_len + 1);

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
gem_exchange_submit(void *handle, uint8_t prio,
    exchange_response_cb_t cb, void *user)
{
  gem_exchange_handle_t *h = handle;
  bool                   is_private;

  if(h == NULL || cb == NULL)
    return(FAIL);

  h->abstr_cb   = cb;
  h->abstr_user = user;

  is_private = (h->kind == EXCHANGE_OP_PRIVATE_REST_GET
             || h->kind == EXCHANGE_OP_PRIVATE_REST_POST
             || h->kind == EXCHANGE_OP_PRIVATE_REST_DELETE);

  if(is_private)
  {
    // Gemini's private surface is POST-only (every parameter rides
    // in the base64 payload header). Surface a clean refusal for
    // any GET/DELETE callers since both EXCHANGE_OP_PRIVATE_REST_GET
    // and EXCHANGE_OP_PRIVATE_REST_DELETE are unsupported at the
    // wire level.
    if(h->kind != EXCHANGE_OP_PRIVATE_REST_POST)
    {
      clam(CLAM_WARN, GEM_CTX,
          "exchange submit: Gemini private endpoints are POST-only");
      return(FAIL);
    }

    if(gem_submit_private(h, prio, h->path, h->body, h->body_len,
          gem_exchange_curl_done) != SUCCESS)
      return(FAIL);
  }
  else
  {
    if(gem_submit_public(h, prio, h->path, gem_exchange_curl_done)
        != SUCCESS)
      return(FAIL);
  }

  return(SUCCESS);
}

static void
gem_exchange_free_request(void *handle)
{
  gem_exchange_handle_t *h = handle;

  if(h == NULL)
    return;

  if(h->body != NULL)
    mem_free(h->body);

  mem_free(h);
}

// ------------------------------------------------------------------ //
// Capability hooks (GEM-2).                                           //
//                                                                     //
// Each hook is a thin adapter over the typed gemini_*_async wrapper   //
// in gemini_orders.c. The trampoline:                                 //
//   1. Validates inputs at the seam (NULL gates).                     //
//   2. Allocates a small forward context carrying (cb, user) so the   //
//      typed callback can find the operator's pointers when the typed //
//      result lands.                                                  //
//   3. Translates the generic exchange_*_t input into the typed       //
//      gemini_*_t form (memcpy field-for-field — every exchange_*_t   //
//      buffer is sized at-or-above the gemini_*_t equivalent).        //
//   4. Returns SUCCESS/FAIL from the typed wrapper. On the FAIL path  //
//      the typed wrapper has already fired the synchronous fail-cb +  //
//      released its forward context.                                  //
//                                                                     //
// Auth gate: every hook except fetch_candles_async refuses early when //
// gem_apikey_configured() returns false. The abstraction's pre-flight //
// applies the same gate via is_authenticated; defending in depth at   //
// the hook layer costs one branch and prevents a stale registration   //
// from leaking an unsigned request.                                   //
// ------------------------------------------------------------------ //

static bool
gem_exch_is_authenticated(void)
{
  return(gem_apikey_configured());
}

typedef struct
{
  exchange_done_order_cb_t cb;
  void                    *user;
} gem_exch_order_fwd_t;

typedef struct
{
  exchange_done_orders_cb_t cb;
  void                     *user;
} gem_exch_orders_fwd_t;

typedef struct
{
  exchange_done_accounts_cb_t cb;
  void                       *user;
} gem_exch_accounts_fwd_t;

typedef struct
{
  exchange_done_fills_cb_t cb;
  void                    *user;
} gem_exch_fills_fwd_t;

typedef struct
{
  exchange_done_candles_cb_t cb;
  void                      *user;
} gem_exch_candles_fwd_t;

// ---- typed-to-generic translation helpers ----
//
// gem_type_to_generic collapses Gemini's verbose order-type strings
// ("exchange limit" / "exchange market" / "exchange stop") to the
// abstraction's compact form ("limit" / "market" / "stop").
// EXCHANGE_TYPE_SZ is 16 bytes; GEMINI_TYPE_SZ is 24, so a naked
// memcpy could overrun the destination buffer. Always NUL-terminates.

static void
gem_type_to_generic(const char *src, char *dst, size_t cap)
{
  if(dst == NULL || cap == 0)
    return;

  dst[0] = '\0';

  if(src == NULL || src[0] == '\0')
    return;

  if(strcmp(src, "exchange limit") == 0)
    snprintf(dst, cap, "limit");
  else if(strcmp(src, "exchange market") == 0)
    snprintf(dst, cap, "market");
  else if(strcmp(src, "exchange stop") == 0
       || strcmp(src, "exchange stop_limit") == 0)
    snprintf(dst, cap, "stop");
  else
  {
    // Unknown / passthrough — silent truncation if the source happens
    // to exceed the destination. memcpy + NUL terminator silences gcc's
    // -Wformat-truncation on the snprintf-of-wider-into-narrower case.
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
  }
}

static void
gem_to_exch_order(const gemini_order_t *src, exchange_order_t *dst)
{
  memset(dst, 0, sizeof(*dst));
  snprintf(dst->order_id,   sizeof(dst->order_id),   "%s", src->order_id);
  snprintf(dst->client_oid, sizeof(dst->client_oid), "%s", src->client_oid);
  snprintf(dst->product_id, sizeof(dst->product_id), "%s", src->product_id);
  snprintf(dst->side,       sizeof(dst->side),       "%s", src->side);
  gem_type_to_generic(src->type, dst->type, sizeof(dst->type));
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
gem_to_exch_account(const gemini_account_t *src, exchange_account_t *dst)
{
  memset(dst, 0, sizeof(*dst));
  snprintf(dst->currency, sizeof(dst->currency), "%s", src->currency);
  dst->balance   = src->balance;
  dst->hold      = src->hold;
  dst->available = src->available;
}

static void
gem_to_exch_fill(const gemini_fill_t *src, exchange_fill_t *dst)
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
gem_to_exch_candle(const gemini_candle_t *src, exchange_candle_t *dst)
{
  memset(dst, 0, sizeof(*dst));
  dst->ts_open_ms = src->ts_open_ms;
  dst->open       = src->open;
  dst->high       = src->high;
  dst->low        = src->low;
  dst->close      = src->close;
  dst->volume     = src->volume;
}

// ---- adapter callbacks ----

static void
gem_exch_order_done_adapter(const gemini_order_result_t *res, void *user)
{
  gem_exch_order_fwd_t     *fwd = user;
  exchange_order_result_t   out;

  if(fwd == NULL)
    return;

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);
  gem_to_exch_order(&res->order, &out.order);

  if(fwd->cb != NULL)
    fwd->cb(&out, fwd->user);

  mem_free(fwd);
}

static void
gem_exch_orders_done_adapter(const gemini_orders_result_t *res, void *user)
{
  gem_exch_orders_fwd_t     *fwd = user;
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
    gem_to_exch_order(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd->cb != NULL)
    fwd->cb(&out, fwd->user);

  mem_free(fwd);
}

static void
gem_exch_accounts_done_adapter(const gemini_balances_result_t *res,
    void *user)
{
  gem_exch_accounts_fwd_t      *fwd = user;
  exchange_accounts_result_t    out;
  uint32_t                      i;
  uint32_t                      n;

  if(fwd == NULL)
    return;

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);

  n = res->count;

  if(n > EXCHANGE_MAX_ACCOUNTS)
    n = EXCHANGE_MAX_ACCOUNTS;

  for(i = 0; i < n; i++)
    gem_to_exch_account(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd->cb != NULL)
    fwd->cb(&out, fwd->user);

  mem_free(fwd);
}

static void
gem_exch_fills_done_adapter(const gemini_fills_result_t *res, void *user)
{
  gem_exch_fills_fwd_t       *fwd = user;
  exchange_fills_result_t     out;
  uint32_t                    i;
  uint32_t                    n;

  if(fwd == NULL)
    return;

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);

  n = res->count;

  if(n > EXCHANGE_MAX_FILLS_LIST)
    n = EXCHANGE_MAX_FILLS_LIST;

  for(i = 0; i < n; i++)
    gem_to_exch_fill(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd->cb != NULL)
    fwd->cb(&out, fwd->user);

  mem_free(fwd);
}

static void
gem_exch_candles_done_adapter(const gemini_candles_result_t *res, void *user)
{
  gem_exch_candles_fwd_t     *fwd = user;
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
    gem_to_exch_candle(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd->cb != NULL)
    fwd->cb(&out, fwd->user);

  mem_free(fwd);
}

// ---- fail helpers ----

static void
gem_exch_fail_order(exchange_done_order_cb_t cb, void *user, const char *err)
{
  exchange_order_result_t res;

  if(cb == NULL)
    return;

  memset(&res, 0, sizeof(res));
  snprintf(res.err, sizeof(res.err), "%s", err);
  cb(&res, user);
}

static void
gem_exch_fail_orders(exchange_done_orders_cb_t cb, void *user,
    const char *err)
{
  exchange_orders_result_t res;

  if(cb == NULL)
    return;

  memset(&res, 0, sizeof(res));
  snprintf(res.err, sizeof(res.err), "%s", err);
  cb(&res, user);
}

static void
gem_exch_fail_accounts(exchange_done_accounts_cb_t cb, void *user,
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
gem_exch_fail_fills(exchange_done_fills_cb_t cb, void *user, const char *err)
{
  exchange_fills_result_t res;

  if(cb == NULL)
    return;

  memset(&res, 0, sizeof(res));
  snprintf(res.err, sizeof(res.err), "%s", err);
  cb(&res, user);
}

static void
gem_exch_fail_candles(exchange_done_candles_cb_t cb, void *user,
    const char *err)
{
  exchange_candles_result_t res;

  if(cb == NULL)
    return;

  memset(&res, 0, sizeof(res));
  snprintf(res.err, sizeof(res.err), "%s", err);
  cb(&res, user);
}

// ---- trampolines ----

static bool
gem_exch_place_order_async(const exchange_place_order_req_t *req,
    exchange_done_order_cb_t cb, void *user)
{
  gemini_place_order_req_t  inner;
  gem_exch_order_fwd_t     *fwd;
  const char               *type_str;

  if(cb == NULL)
    return(FAIL);

  if(req == NULL
      || req->product_id[0] == '\0'
      || req->side[0]       == '\0'
      || req->type[0]       == '\0')
  {
    gem_exch_fail_order(cb, user, "invalid place_order arguments");
    return(FAIL);
  }

  if(!gem_apikey_configured())
  {
    gem_exch_fail_order(cb, user, "gemini: api keys not configured");
    return(FAIL);
  }

  // Map the generic "limit" / "market" tokens to Gemini's "exchange
  // limit" / "exchange market" form. Reject anything else cleanly.
  if(strcmp(req->type, "limit") == 0)
    type_str = "exchange limit";
  else if(strcmp(req->type, "market") == 0)
    type_str = "exchange market";
  else
  {
    gem_exch_fail_order(cb, user,
        "gemini: supported order types are 'limit' and 'market'");
    return(FAIL);
  }

  fwd = mem_alloc(GEM_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    gem_exch_fail_order(cb, user, "out of memory");
    return(FAIL);
  }

  fwd->cb   = cb;
  fwd->user = user;

  memset(&inner, 0, sizeof(inner));
  snprintf(inner.product_id, sizeof(inner.product_id), "%s",
      req->product_id);
  snprintf(inner.side,       sizeof(inner.side),       "%s", req->side);
  snprintf(inner.type,       sizeof(inner.type),       "%s", type_str);
  snprintf(inner.tif,        sizeof(inner.tif),        "%s", req->tif);
  snprintf(inner.client_oid, sizeof(inner.client_oid), "%s",
      req->client_oid);
  inner.price     = req->price;
  inner.size      = req->size;
  inner.post_only = req->post_only;

  return(gemini_add_order_async(&inner, gem_exch_order_done_adapter, fwd));
}

static bool
gem_exch_cancel_order_async(const char *order_id,
    exchange_done_order_cb_t cb, void *user)
{
  gem_exch_order_fwd_t *fwd;

  if(cb == NULL)
    return(FAIL);

  if(order_id == NULL || order_id[0] == '\0')
  {
    gem_exch_fail_order(cb, user, "order_id required");
    return(FAIL);
  }

  if(!gem_apikey_configured())
  {
    gem_exch_fail_order(cb, user, "gemini: api keys not configured");
    return(FAIL);
  }

  fwd = mem_alloc(GEM_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    gem_exch_fail_order(cb, user, "out of memory");
    return(FAIL);
  }

  fwd->cb   = cb;
  fwd->user = user;

  return(gemini_cancel_order_async(order_id, gem_exch_order_done_adapter, fwd));
}

static bool
gem_exch_get_order_async(const char *order_id,
    exchange_done_order_cb_t cb, void *user)
{
  gem_exch_order_fwd_t *fwd;

  if(cb == NULL)
    return(FAIL);

  if(order_id == NULL || order_id[0] == '\0')
  {
    gem_exch_fail_order(cb, user, "order_id required");
    return(FAIL);
  }

  if(!gem_apikey_configured())
  {
    gem_exch_fail_order(cb, user, "gemini: api keys not configured");
    return(FAIL);
  }

  fwd = mem_alloc(GEM_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    gem_exch_fail_order(cb, user, "out of memory");
    return(FAIL);
  }

  fwd->cb   = cb;
  fwd->user = user;

  return(gemini_query_order_async(order_id, gem_exch_order_done_adapter, fwd));
}

// list_orders_async: Gemini's /v1/orders surfaces OPEN orders only. The
// abstraction's `status` argument is therefore advisory: empty / "open"
// goes through to gemini_active_orders_async; anything else FAILs
// cleanly so the consumer knows to look elsewhere for closed orders
// (Gemini exposes those only via /v1/mytrades).
static bool
gem_exch_list_orders_async(const char *status, const char *product_id,
    exchange_done_orders_cb_t cb, void *user)
{
  gem_exch_orders_fwd_t *fwd;

  (void)product_id;

  if(cb == NULL)
    return(FAIL);

  if(status != NULL && status[0] != '\0' && strcmp(status, "open") != 0)
  {
    gem_exch_fail_orders(cb, user,
        "gemini: closed orders not supported (server returns open only)");
    return(FAIL);
  }

  if(!gem_apikey_configured())
  {
    gem_exch_fail_orders(cb, user, "gemini: api keys not configured");
    return(FAIL);
  }

  fwd = mem_alloc(GEM_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    gem_exch_fail_orders(cb, user, "out of memory");
    return(FAIL);
  }

  fwd->cb   = cb;
  fwd->user = user;

  return(gemini_active_orders_async(gem_exch_orders_done_adapter, fwd));
}

static bool
gem_exch_list_fills_async(const char *order_id, const char *product_id,
    int64_t start_ms, exchange_done_fills_cb_t cb, void *user)
{
  gem_exch_fills_fwd_t *fwd;

  (void)order_id;     // Gemini's mytrades scopes by symbol only.

  if(cb == NULL)
    return(FAIL);

  if(product_id == NULL || product_id[0] == '\0')
  {
    gem_exch_fail_fills(cb, user,
        "gemini: list_fills requires product_id");
    return(FAIL);
  }

  if(!gem_apikey_configured())
  {
    gem_exch_fail_fills(cb, user, "gemini: api keys not configured");
    return(FAIL);
  }

  fwd = mem_alloc(GEM_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    gem_exch_fail_fills(cb, user, "out of memory");
    return(FAIL);
  }

  fwd->cb   = cb;
  fwd->user = user;

  return(gemini_mytrades_async(product_id, start_ms,
        gem_exch_fills_done_adapter, fwd));
}

static bool
gem_exch_get_accounts_async(exchange_done_accounts_cb_t cb, void *user)
{
  gem_exch_accounts_fwd_t *fwd;

  if(cb == NULL)
    return(FAIL);

  if(!gem_apikey_configured())
  {
    gem_exch_fail_accounts(cb, user, "gemini: api keys not configured");
    return(FAIL);
  }

  fwd = mem_alloc(GEM_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    gem_exch_fail_accounts(cb, user, "out of memory");
    return(FAIL);
  }

  fwd->cb   = cb;
  fwd->user = user;

  return(gemini_get_balance_async(gem_exch_accounts_done_adapter, fwd));
}

// Public market data — no auth gate. The typed wrapper owns the
// granularity translation + client-side window filtering.
static bool
gem_exch_fetch_candles_async(const char *product_id,
    exchange_granularity_t gran, int64_t since_ms, int64_t until_ms,
    exchange_done_candles_cb_t cb, void *user)
{
  gem_exch_candles_fwd_t *fwd;

  if(cb == NULL)
    return(FAIL);

  if(product_id == NULL || product_id[0] == '\0')
  {
    gem_exch_fail_candles(cb, user, "product_id required");
    return(FAIL);
  }

  fwd = mem_alloc(GEM_CTX, "exch.fwd", sizeof(*fwd));

  if(fwd == NULL)
  {
    gem_exch_fail_candles(cb, user, "out of memory");
    return(FAIL);
  }

  fwd->cb   = cb;
  fwd->user = user;

  if(gemini_fetch_candles_async(product_id, gran, since_ms, until_ms,
        EXCHANGE_PRIO_MARKET_BACKFILL,
        gem_exch_candles_done_adapter, fwd) != SUCCESS)
  {
    // The typed wrapper has already delivered the fail callback +
    // freed fwd. Nothing left to do here.
    return(FAIL);
  }

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// File-scope vtable.                                                  //
//                                                                     //
// advertised_rps = 8 matches Coinbase (Gemini's documented private    //
// budget is 600/min = 10/s; advertised 8 leaves headroom under the    //
// public 120/min budget that the queue-shared limiter also services). //
// advertised_burst = 15 matches the other backends.                   //
// ------------------------------------------------------------------ //

static const exchange_protocol_vtable_t gem_vtable =
{
  .build_request       = gem_exchange_build_request,
  .submit              = gem_exchange_submit,
  .free_request        = gem_exchange_free_request,
  .advertised_rps      = 8,
  .advertised_burst    = 15,

  .is_authenticated    = gem_exch_is_authenticated,

  .place_order_async   = gem_exch_place_order_async,
  .cancel_order_async  = gem_exch_cancel_order_async,
  .get_order_async     = gem_exch_get_order_async,
  .list_orders_async   = gem_exch_list_orders_async,
  .list_fills_async    = gem_exch_list_fills_async,
  .get_accounts_async  = gem_exch_get_accounts_async,
  .fetch_candles_async = gem_exch_fetch_candles_async,

  // WS slots wired in GEM-3. NULL here means the public exchange WS
  // shim FAILs synchronously with `ws_subscribe unsupported` — by
  // design at this chunk.
  .ws_subscribe        = NULL,
  .ws_unsubscribe      = NULL,
};

bool
gem_exchange_register_vtable(void)
{
  return(exchange_register("gemini", &gem_vtable));
}
