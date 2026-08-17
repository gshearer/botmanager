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
#include "json.h"

#include "kraken_pairs.h"

#include <math.h>
#include <stdint.h>
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

  // This dispatch's slot in the plugin's flight (kraken_rest.c). The
  // submitter opens it; the completion adapter closes it, which is the
  // end of this mapping's part in the work — the abstraction frees the
  // handle itself, later and without touching kraken state.
  uint64_t                slot;
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
  uint64_t                slot;
  bool                    transport_err;

  if(h == NULL)
    return;

  slot          = h->slot;
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

  // Last: the consumer's callback above runs inside this mapping and
  // reaches back into kraken (kr_pair_lookup, the typed adapters), so
  // the work is not over until it returns. The handle itself outlives
  // this — the abstraction frees it — and touches no kraken lock.
  kr_rest_slot_close(slot);
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

    if(kr_submit_private(h, &h->slot, prio, h->path, h->body, h->body_len,
          kr_exchange_curl_done) != SUCCESS)
      return(FAIL);
  }
  else
  {
    if(kr_submit_public(h, &h->slot, prio, h->path, kr_exchange_curl_done)
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

// ------------------------------------------------------------------ //
// Per-call adapter context, and the in-flight registry over it         //
// ------------------------------------------------------------------ //

// One context per dispatch: it carries the consumer's typed callback
// across the inner kraken_*_async call and is freed by the adapter that
// delivers it. Six shapes of callback, one struct — they differ only in
// which arm of the union is live, and a single type is what lets one
// registry walk them all.
typedef enum
{
  KR_FWD_ORDER,
  KR_FWD_ORDERS,
  KR_FWD_ACCOUNTS,
  KR_FWD_FILLS,
  KR_FWD_CANDLES,
  KR_FWD_TICKERS
} kr_fwd_type_t;

typedef struct kr_exch_fwd
{
  kr_fwd_type_t type;

  union
  {
    exchange_done_order_cb_t     order;
    exchange_done_orders_cb_t    orders;
    exchange_done_accounts_cb_t  accounts;
    exchange_done_fills_cb_t     fills;
    exchange_done_candles_cb_t   candles;
    exchange_done_tickers_cb_t   tickers;
  } cb;
  void         *user;

  // KR_FWD_CANDLES only: Kraken's OHLC takes no `until`, so the cutoff
  // is applied client-side in the adapter.
  int64_t       until_ms;

  struct kr_exch_fwd *next_active;
} kr_exch_fwd_t;

// Every callback in that union belongs to another mapping — the vtable
// is reached only from `whenmoon`, through feature_exchange. We hand
// core's curl layer one of our own completions and keep the consumer's
// one indirection deeper, where neither plugin_quiesce nor plugin_audit
// can see it: both range-test curl_iter_req_t.cb, which for our
// transfers names THIS mapping, never the consumer's. Reload whenmoon
// with an order, a trades page or a candle range airborne and the stored
// pointer aims into freed .text — and these are the longest requests in
// the daemon, held open for seconds against an exchange.
//
// So every live dispatch is filed here, and plugin_unmap_notify_register
// tells us when a mapping is about to go away in time to null the
// pointers that name it. A dispatch whose consumer left still completes
// normally; it simply delivers to nobody. The mechanics are commented in
// full in reachyapi.c and written up in PLUGIN.md.
//
// The consumer's `user` is dropped with the callback and whatever it
// points at is leaked. Nothing else is possible: only the consumer knows
// how to free its own context, and the consumer is precisely what is no
// longer there. A bounded leak on an operator action beats a SIGSEGV.
//
// The WS slots need no arm here: they forward straight into the channel
// multiplexer, which owns its own opaque handle, so no consumer pointer
// is stored at this layer at all.
static pthread_mutex_t kr_fwd_mutex = PTHREAD_MUTEX_INITIALIZER;
static kr_exch_fwd_t  *kr_fwd_head  = NULL;
static uint32_t        kr_fwd_count = 0;

// Allocate a dispatch context with its consumer half installed and file
// it before anything can be submitted, never after: a completion can run
// on a curl worker before the submitting call has returned.
static kr_exch_fwd_t *
kr_fwd_new(kr_fwd_type_t type, void *user)
{
  kr_exch_fwd_t *fwd = mem_alloc(KR_CTX, "exch.fwd", sizeof(*fwd));

  memset(fwd, 0, sizeof(*fwd));
  fwd->type = type;
  fwd->user = user;

  pthread_mutex_lock(&kr_fwd_mutex);

  fwd->next_active = kr_fwd_head;
  kr_fwd_head      = fwd;
  kr_fwd_count++;

  pthread_mutex_unlock(&kr_fwd_mutex);

  return(fwd);
}

// Unlink `fwd`, copy it to `out` and free it. The copy is taken under
// the lock so an adapter reads the consumer's callback in the same
// critical section the unmap sweep would null it in — read it afterwards
// and the two interleave, which is the whole bug.
static void
kr_fwd_retire(kr_exch_fwd_t *fwd, kr_exch_fwd_t *out)
{
  kr_exch_fwd_t **pp;

  pthread_mutex_lock(&kr_fwd_mutex);

  for(pp = &kr_fwd_head; *pp != NULL; pp = &(*pp)->next_active)
  {
    if(*pp != fwd)
      continue;

    *pp = fwd->next_active;
    kr_fwd_count--;
    break;
  }

  *out = *fwd;

  pthread_mutex_unlock(&kr_fwd_mutex);

  out->next_active = NULL;
  mem_free(fwd);
}

// A mapping is going away (core is between the plugin's deinit() and its
// residual audit, so nothing of it runs any more). Drop every callback
// that lives inside it.
//
// Residual race: an adapter that has already retired its context holds
// the callback on its stack and is a few instructions from calling it.
// The window is bounded above by the quiescence poll plus the audit that
// follow this broadcast, and below by two stores — against an
// operator-timescale unload.
static void
kr_exch_unmap_cb(uintptr_t lo, uintptr_t hi, void *data)
{
  uint32_t orphaned = 0;

  (void)data;

  pthread_mutex_lock(&kr_fwd_mutex);

  for(kr_exch_fwd_t *f = kr_fwd_head; f != NULL; f = f->next_active)
  {
    uintptr_t cb = 0;

    switch(f->type)
    {
      case KR_FWD_ORDER:    cb = (uintptr_t)fn_addr(&f->cb.order);    break;
      case KR_FWD_ORDERS:   cb = (uintptr_t)fn_addr(&f->cb.orders);   break;
      case KR_FWD_ACCOUNTS: cb = (uintptr_t)fn_addr(&f->cb.accounts); break;
      case KR_FWD_FILLS:    cb = (uintptr_t)fn_addr(&f->cb.fills);    break;
      case KR_FWD_CANDLES:  cb = (uintptr_t)fn_addr(&f->cb.candles);  break;
      case KR_FWD_TICKERS:  cb = (uintptr_t)fn_addr(&f->cb.tickers);  break;
    }

    if(cb == 0 || cb < lo || cb >= hi)
      continue;

    // memset rather than one arm's NULL: the arms are a union, and
    // all-bits-zero is the null test every adapter makes.
    memset(&f->cb, 0, sizeof(f->cb));
    f->user = NULL;
    orphaned++;
  }

  pthread_mutex_unlock(&kr_fwd_mutex);

  if(orphaned > 0)
    clam(CLAM_WARN, KR_CTX, "%u exchange request(s) lost their consumer "
        "to an unload; they will complete and deliver nothing", orphaned);
}

void
kr_exch_init(void)
{
  plugin_unmap_notify_register(kr_exch_unmap_cb, NULL);
}

void
kr_exch_deinit(void)
{
  uint32_t stranded;

  plugin_unmap_notify_unregister(kr_exch_unmap_cb);

  pthread_mutex_lock(&kr_fwd_mutex);
  stranded = kr_fwd_count;
  pthread_mutex_unlock(&kr_fwd_mutex);

  // Nothing to free: those contexts belong to requests curl still owns,
  // and their adapters live in the mapping now going away. Core's
  // residual audit sees them, so it is the audit that refuses the
  // dlclose, not us. Naming the count here makes that refusal legible.
  if(stranded > 0)
    clam(CLAM_WARN, KR_CTX, "%u exchange request(s) still in flight at "
        "deinit", stranded);
}

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
  kr_exch_fwd_t             fwd;
  exchange_order_result_t   out;

  if(user == NULL)
    return;

  kr_fwd_retire(user, &fwd);

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);
  kr_to_exch_order(&res->order, &out.order);

  if(fwd.cb.order != NULL)
    fwd.cb.order(&out, fwd.user);
}

static void
kr_exch_orders_done_adapter(const kraken_orders_result_t *res, void *user)
{
  kr_exch_fwd_t              fwd;
  exchange_orders_result_t   out;
  uint32_t                   i;
  uint32_t                   n;

  if(user == NULL)
    return;

  kr_fwd_retire(user, &fwd);

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);

  n = res->count;

  if(n > EXCHANGE_MAX_ORDERS_LIST)
    n = EXCHANGE_MAX_ORDERS_LIST;

  for(i = 0; i < n; i++)
    kr_to_exch_order(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd.cb.orders != NULL)
    fwd.cb.orders(&out, fwd.user);
}

static void
kr_exch_accounts_done_adapter(const kraken_balances_result_t *res,
    void *user)
{
  kr_exch_fwd_t                fwd;
  exchange_accounts_result_t   out;
  uint32_t                     i;
  uint32_t                     n;

  if(user == NULL)
    return;

  kr_fwd_retire(user, &fwd);

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);

  n = res->count;

  if(n > EXCHANGE_MAX_ACCOUNTS)
    n = EXCHANGE_MAX_ACCOUNTS;

  for(i = 0; i < n; i++)
    kr_to_exch_account(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd.cb.accounts != NULL)
    fwd.cb.accounts(&out, fwd.user);
}

static void
kr_exch_fills_done_adapter(const kraken_fills_result_t *res, void *user)
{
  kr_exch_fwd_t              fwd;
  exchange_fills_result_t    out;
  uint32_t                   i;
  uint32_t                   n;

  if(user == NULL)
    return;

  kr_fwd_retire(user, &fwd);

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);

  n = res->count;

  if(n > EXCHANGE_MAX_FILLS_LIST)
    n = EXCHANGE_MAX_FILLS_LIST;

  for(i = 0; i < n; i++)
    kr_to_exch_fill(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd.cb.fills != NULL)
    fwd.cb.fills(&out, fwd.user);
}

static void
kr_exch_candles_done_adapter(const kraken_candles_result_t *res, void *user)
{
  kr_exch_fwd_t               fwd;
  exchange_candles_result_t   out;
  uint32_t                    i;
  uint32_t                    n;
  uint32_t                    kept = 0;

  if(user == NULL)
    return;

  kr_fwd_retire(user, &fwd);

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
    if(fwd.until_ms > 0 && ts_ms >= fwd.until_ms)
      continue;

    kr_to_exch_candle(c, &out.rows[kept]);
    kept++;
  }

  out.count = kept;

  if(fwd.cb.candles != NULL)
    fwd.cb.candles(&out, fwd.user);
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

static async_rc_t
kr_exch_place_order_async(const exchange_place_order_req_t *req,
    exchange_done_order_cb_t cb, void *user)
{
  kraken_place_order_req_t  inner;
  kr_exch_fwd_t            *fwd;

  if(req == NULL || cb == NULL)
  {
    kr_exch_fail_order(cb, user, "invalid place_order arguments");
    return(ASYNC_FAILED_DELIVERED);
  }

  fwd           = kr_fwd_new(KR_FWD_ORDER, user);
  fwd->cb.order = cb;

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

static async_rc_t
kr_exch_cancel_order_async(const char *order_id,
    exchange_done_order_cb_t cb, void *user)
{
  kr_exch_fwd_t       *fwd;

  if(cb == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  if(order_id == NULL || order_id[0] == '\0')
  {
    kr_exch_fail_order(cb, user, "order_id required");
    return(ASYNC_FAILED_DELIVERED);
  }

  fwd           = kr_fwd_new(KR_FWD_ORDER, user);
  fwd->cb.order = cb;

  return(kraken_cancel_order_async(order_id, kr_exch_order_done_adapter, fwd));
}

static async_rc_t
kr_exch_get_order_async(const char *order_id,
    exchange_done_order_cb_t cb, void *user)
{
  kr_exch_fwd_t       *fwd;

  if(cb == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  if(order_id == NULL || order_id[0] == '\0')
  {
    kr_exch_fail_order(cb, user, "order_id required");
    return(ASYNC_FAILED_DELIVERED);
  }

  fwd           = kr_fwd_new(KR_FWD_ORDER, user);
  fwd->cb.order = cb;

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
static async_rc_t
kr_exch_list_orders_async(const char *status, const char *product_id,
    exchange_done_orders_cb_t cb, void *user)
{
  kr_exch_fwd_t        *fwd;
  bool                  want_closed;

  (void)product_id;

  if(cb == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  fwd            = kr_fwd_new(KR_FWD_ORDERS, user);
  fwd->cb.orders = cb;

  want_closed = (status != NULL
              && (strcmp(status, "closed") == 0
                  || strcmp(status, "done")   == 0
                  || strcmp(status, "settled") == 0));

  if(want_closed)
    return(kraken_closed_orders_async(0, kr_exch_orders_done_adapter, fwd));

  return(kraken_open_orders_async(kr_exch_orders_done_adapter, fwd));
}

static async_rc_t
kr_exch_list_fills_async(const char *order_id, const char *product_id,
    int64_t start_ms, exchange_done_fills_cb_t cb, void *user)
{
  kr_exch_fwd_t       *fwd;
  int64_t              start_sec;

  if(cb == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  fwd           = kr_fwd_new(KR_FWD_FILLS, user);
  fwd->cb.fills = cb;

  start_sec = (start_ms > 0) ? (start_ms / 1000) : 0;

  return(kraken_trades_history_async(order_id, product_id, start_sec,
        kr_exch_fills_done_adapter, fwd));
}

static async_rc_t
kr_exch_get_accounts_async(exchange_done_accounts_cb_t cb, void *user)
{
  kr_exch_fwd_t          *fwd;

  if(cb == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  fwd              = kr_fwd_new(KR_FWD_ACCOUNTS, user);
  fwd->cb.accounts = cb;

  return(kraken_get_balance_async(kr_exch_accounts_done_adapter, fwd));
}

static async_rc_t
kr_exch_fetch_candles_async(const char *product_id,
    exchange_granularity_t gran, int64_t since_ms, int64_t until_ms,
    exchange_done_candles_cb_t cb, void *user)
{
  kr_exch_fwd_t         *fwd;
  uint32_t               interval_min = 0;
  int64_t                since_sec;

  if(cb == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  if(product_id == NULL || product_id[0] == '\0')
  {
    kr_exch_fail_candles(cb, user, "product_id required");
    return(ASYNC_FAILED_DELIVERED);
  }

  if(kr_exch_map_granularity(gran, &interval_min) != SUCCESS)
  {
    kr_exch_fail_candles(cb, user, "unsupported granularity for Kraken");
    return(ASYNC_FAILED_DELIVERED);
  }

  fwd             = kr_fwd_new(KR_FWD_CANDLES, user);
  fwd->cb.candles = cb;
  fwd->until_ms   = until_ms;

  since_sec = (since_ms > 0) ? (since_ms / 1000) : 0;

  // `fwd` belongs to the typed wrapper from here, so its verdict on
  // delivery is this adapter's verdict too — pass it through rather
  // than restating it.
  return(kraken_fetch_candles_async(product_id, interval_min, since_sec,
        EXCHANGE_PRIO_MARKET_BACKFILL,
        kr_exch_candles_done_adapter, fwd));
}

// ------------------------------------------------------------------ //
// MW-1: bulk-ticker fetch                                              //
//                                                                      //
// `GET /0/public/Ticker` (no pair= argument → every spot pair). Wire  //
// shape:                                                               //
//   { "error": [], "result": {                                         //
//       "XXBTZUSD": { "a":["..."], "b":["..."],                       //
//                     "c":[<last>,<lastvol>],                           //
//                     "v":[<today>,<24h>],                              //
//                     "p":[<today>,<24h>],                              //
//                     "t":[<today>,<24h>],                              //
//                     "l":[<today>,<24h>],                              //
//                     "h":[<today>,<24h>],                              //
//                     "o":<open> }, ... } }                            //
// Every numeric arrives as a JSON string; ts (`t`) is a JSON number.  //
// Keys are wire pair names that must be re-keyed onto the canonical   //
// abstraction form (BTC-USD) via the assetpairs cache. Rows whose key //
// has no cache entry are dropped with one DBG3 line per row.          //
// ------------------------------------------------------------------ //

static double
kr_json_arr_str_double(struct json_object *arr, int idx)
{
  struct json_object *v;
  const char         *s;

  if(arr == NULL || !json_object_is_type(arr, json_type_array))
    return(NAN);

  if(idx < 0 || idx >= (int)json_object_array_length(arr))
    return(NAN);

  v = json_object_array_get_idx(arr, idx);

  if(v == NULL)
    return(NAN);

  if(json_object_is_type(v, json_type_string))
  {
    s = json_object_get_string(v);
    if(s == NULL || s[0] == '\0')
      return(NAN);
    return(strtod(s, NULL));
  }

  if(json_object_is_type(v, json_type_double) ||
     json_object_is_type(v, json_type_int))
    return(json_object_get_double(v));

  return(NAN);
}

static uint64_t
kr_json_arr_uint64(struct json_object *arr, int idx)
{
  struct json_object *v;

  if(arr == NULL || !json_object_is_type(arr, json_type_array))
    return(UINT64_MAX);

  if(idx < 0 || idx >= (int)json_object_array_length(arr))
    return(UINT64_MAX);

  v = json_object_array_get_idx(arr, idx);

  if(v == NULL)
    return(UINT64_MAX);

  if(json_object_is_type(v, json_type_int) ||
     json_object_is_type(v, json_type_double))
  {
    int64_t s = json_object_get_int64(v);
    return(s < 0 ? 0 : (uint64_t)s);
  }

  return(UINT64_MAX);
}

static double
kr_json_str_double(struct json_object *obj, const char *key)
{
  struct json_object *v;
  const char         *s;

  if(obj == NULL || !json_object_object_get_ex(obj, key, &v))
    return(NAN);

  if(json_object_is_type(v, json_type_string))
  {
    s = json_object_get_string(v);
    if(s == NULL || s[0] == '\0')
      return(NAN);
    return(strtod(s, NULL));
  }

  if(json_object_is_type(v, json_type_double) ||
     json_object_is_type(v, json_type_int))
    return(json_object_get_double(v));

  return(NAN);
}

// Deliver from a RETIRED context, whose callback is either the
// consumer's or NULL because the consumer was unloaded mid-flight.
static void
kr_exch_deliver_tickers(const kr_exch_fwd_t *f, bool ok, const char *err,
    const exchange_ticker_snapshot_t *rows, size_t n)
{
  if(f->cb.tickers != NULL)
    f->cb.tickers(ok, err, rows, n, f->user);
}

static void
kr_exch_tickers_resp(int http_status, const char *body, size_t body_len,
    const char *err, void *user)
{
  kr_exch_fwd_t                fwd;
  struct json_object          *root;
  struct json_object          *errs;
  struct json_object          *result;
  exchange_ticker_snapshot_t  *rows = NULL;
  size_t                       row_cap;
  size_t                       kept = 0;
  size_t                       n_keys;

  (void)http_status;

  if(user == NULL)
    return;

  kr_fwd_retire(user, &fwd);

  if(err != NULL)
  {
    kr_exch_deliver_tickers(&fwd, false, err, NULL, 0);
    return;
  }

  root = json_parse_buf(body, body_len, KR_CTX);

  if(root == NULL)
  {
    kr_exch_deliver_tickers(&fwd, false,
        "malformed JSON from Kraken Ticker", NULL, 0);
    return;
  }

  if(!json_object_is_type(root, json_type_object))
  {
    json_object_put(root);
    kr_exch_deliver_tickers(&fwd, false,
        "unexpected Kraken Ticker response shape", NULL, 0);
    return;
  }

  // Kraken returns errors in the top-level "error" array. Non-empty
  // means the request failed even on HTTP 200.
  if(json_object_object_get_ex(root, "error", &errs) &&
     json_object_is_type(errs, json_type_array) &&
     json_object_array_length(errs) > 0)
  {
    char        ebuf[256];
    size_t      eoff = 0;
    int         ne   = (int)json_object_array_length(errs);
    int         i;

    ebuf[0] = '\0';
    for(i = 0; i < ne && eoff < sizeof(ebuf); i++)
    {
      struct json_object *ev = json_object_array_get_idx(errs, i);
      const char         *es;

      if(ev == NULL || !json_object_is_type(ev, json_type_string))
        continue;

      es = json_object_get_string(ev);
      if(es == NULL)
        continue;

      eoff += (size_t)snprintf(ebuf + eoff, sizeof(ebuf) - eoff,
          "%s%s", eoff > 0 ? "; " : "", es);
    }

    json_object_put(root);
    kr_exch_deliver_tickers(&fwd, false,
        ebuf[0] != '\0' ? ebuf : "Kraken Ticker error", NULL, 0);
    return;
  }

  if(!json_object_object_get_ex(root, "result", &result) ||
     !json_object_is_type(result, json_type_object))
  {
    json_object_put(root);
    kr_exch_deliver_tickers(&fwd, false,
        "unexpected Kraken Ticker result shape", NULL, 0);
    return;
  }

  // Count keys so we can size the heap array tightly.
  n_keys = 0;
  json_object_object_foreach(result, _k0, _v0)
  {
    (void)_k0;
    (void)_v0;
    n_keys++;
  }

  if(n_keys == 0)
  {
    json_object_put(root);
    kr_exch_deliver_tickers(&fwd, true, NULL, NULL, 0);
    return;
  }

  row_cap = n_keys;
  if(row_cap > EXCHANGE_TICKERS_MAX)
  {
    clam(CLAM_WARN, KR_CTX,
        "tickers: result map %zu exceeds cap %d; truncating",
        n_keys, (int)EXCHANGE_TICKERS_MAX);
    row_cap = EXCHANGE_TICKERS_MAX;
  }

  rows = mem_alloc(KR_CTX, "exch.tickers",
      row_cap * sizeof(*rows));

  json_object_object_foreach(result, wire_key, pair_obj)
  {
    exchange_ticker_snapshot_t *out;
    struct json_object         *arr_c;
    struct json_object         *arr_v;
    struct json_object         *arr_p;
    struct json_object         *arr_t;
    struct json_object         *arr_h;
    struct json_object         *arr_l;
    char                        canon[EXCHANGE_PRODUCT_ID_SZ];
    double                      last;
    double                      vwap_24h;
    double                      open;

    if(kept >= row_cap)
      break;

    if(pair_obj == NULL ||
       !json_object_is_type(pair_obj, json_type_object))
      continue;

    if(wire_key == NULL)
      continue;

    kr_pair_lookup_abstr(wire_key, canon, sizeof(canon));

    if(canon[0] == '\0')
    {
      clam(CLAM_DEBUG3, KR_CTX,
          "tickers: drop uncanonical %s", wire_key);
      continue;
    }

    arr_c = NULL; arr_v = NULL; arr_p = NULL;
    arr_t = NULL; arr_h = NULL; arr_l = NULL;
    (void)json_object_object_get_ex(pair_obj, "c", &arr_c);
    (void)json_object_object_get_ex(pair_obj, "v", &arr_v);
    (void)json_object_object_get_ex(pair_obj, "p", &arr_p);
    (void)json_object_object_get_ex(pair_obj, "t", &arr_t);
    (void)json_object_object_get_ex(pair_obj, "h", &arr_h);
    (void)json_object_object_get_ex(pair_obj, "l", &arr_l);

    last     = kr_json_arr_str_double(arr_c, 0);
    vwap_24h = kr_json_arr_str_double(arr_p, 1);
    open     = kr_json_str_double(pair_obj, "o");

    out = &rows[kept];
    memset(out, 0, sizeof(*out));
    snprintf(out->product_id, sizeof(out->product_id), "%s", canon);

    out->price          = last;
    out->pct_24h        = (!isnan(last) && !isnan(open) && open != 0.0)
                              ? (last - open) / open * 100.0 : NAN;
    out->vol_24h_base   = kr_json_arr_str_double(arr_v, 1);
    out->vol_24h_quote  = (!isnan(out->vol_24h_base) && !isnan(vwap_24h))
                              ? out->vol_24h_base * vwap_24h : NAN;
    out->hi_24h         = kr_json_arr_str_double(arr_h, 1);
    out->lo_24h         = kr_json_arr_str_double(arr_l, 1);
    out->vwap_24h       = vwap_24h;
    out->num_trades_24h = kr_json_arr_uint64(arr_t, 1);
    out->status         = EXCH_TICK_ONLINE;

    kept++;
  }

  clam(CLAM_DEBUG2, KR_CTX,
      "tickers: keys=%zu kept=%zu", n_keys, kept);

  kr_exch_deliver_tickers(&fwd, true, NULL, rows, kept);

  mem_free(rows);
  json_object_put(root);
}

static async_rc_t
kr_exch_fetch_all_tickers_async(exchange_done_tickers_cb_t cb, void *user)
{
  kr_exch_fwd_t *fwd;
  kr_exch_fwd_t  dead;

  if(cb == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  fwd             = kr_fwd_new(KR_FWD_TICKERS, user);
  fwd->cb.tickers = cb;

  // kr_submit_public prepends "/0/public/" — pass only the leaf path.
  if(exchange_request("kraken", EXCHANGE_PRIO_MARKET_BACKFILL,
        EXCHANGE_OP_REST_GET, "Ticker", NULL,
        kr_exch_tickers_resp, fwd) != SUCCESS)
  {
    kr_fwd_retire(fwd, &dead);
    cb(false, "failed to submit Kraken Ticker request",
        NULL, 0, user);
    return(ASYNC_FAILED_DELIVERED);
  }

  return(ASYNC_AIRBORNE);
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

  // MW-1 capability hook.
  .fetch_all_tickers   = kr_exch_fetch_all_tickers_async,
};

bool
kr_exchange_register_vtable(void)
{
  return(exchange_register("kraken", &kr_vtable));
}
