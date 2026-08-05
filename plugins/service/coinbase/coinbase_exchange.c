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
#include "json.h"

#include <math.h>
#include <stdint.h>
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

// ------------------------------------------------------------------ //
// Per-call adapter context, and the in-flight registry over it         //
// ------------------------------------------------------------------ //

// One context per dispatch: it carries the consumer's typed callback
// across the inner coinbase_*_async call and is freed by the adapter
// that delivers it. Six shapes of callback, one struct — they differ
// only in which arm of the union is live, and a single type is what
// lets one registry walk them all.
typedef enum
{
  CB_FWD_ORDER,
  CB_FWD_ORDERS,
  CB_FWD_ACCOUNTS,
  CB_FWD_FILLS,
  CB_FWD_CANDLES,
  CB_FWD_TICKERS
} cb_fwd_type_t;

typedef struct cb_exch_fwd
{
  cb_fwd_type_t type;

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

  struct cb_exch_fwd *next_active;
} cb_exch_fwd_t;

// Every callback in that union belongs to another mapping — the vtable
// is reached only from `whenmoon`, through feature_exchange. We hand
// core's curl layer one of our own completions and keep the consumer's
// one indirection deeper, where neither plugin_quiesce nor plugin_audit
// can see it: both range-test curl_iter_req_t.cb, which for our
// transfers names THIS mapping, never the consumer's. Reload whenmoon
// with an order, a fills page or a candle range airborne and the stored
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
// ⭑ WS subscriptions are deliberately NOT filed. cb_exch_ws_sub_t holds
// a consumer callback for the life of the subscription — a far wider
// window — but whenmoon drains every binding through
// exchange_ws_unsubscribe on both of its teardown paths
// (market.c:wm_market_destroy, live.c:wm_live_shutdown), so the handle
// is gone before the unload. That is a consumer-side guarantee, not a
// property of this file; if a second consumer ever subscribes, it owes
// the same discipline or this registry owes it an arm.
static pthread_mutex_t cb_fwd_mutex = PTHREAD_MUTEX_INITIALIZER;
static cb_exch_fwd_t  *cb_fwd_head  = NULL;
static uint32_t        cb_fwd_count = 0;

// Allocate a dispatch context with its consumer half installed and file
// it before anything can be submitted, never after: a completion can run
// on a curl worker before the submitting call has returned.
static cb_exch_fwd_t *
cb_fwd_new(cb_fwd_type_t type, void *user)
{
  cb_exch_fwd_t *fwd = mem_alloc(CB_CTX, "exch.fwd", sizeof(*fwd));

  memset(fwd, 0, sizeof(*fwd));
  fwd->type = type;
  fwd->user = user;

  pthread_mutex_lock(&cb_fwd_mutex);

  fwd->next_active = cb_fwd_head;
  cb_fwd_head      = fwd;
  cb_fwd_count++;

  pthread_mutex_unlock(&cb_fwd_mutex);

  return(fwd);
}

// Unlink `fwd`, copy it to `out` and free it. The copy is taken under
// the lock so an adapter reads the consumer's callback in the same
// critical section the unmap sweep would null it in — read it afterwards
// and the two interleave, which is the whole bug.
static void
cb_fwd_retire(cb_exch_fwd_t *fwd, cb_exch_fwd_t *out)
{
  cb_exch_fwd_t **pp;

  pthread_mutex_lock(&cb_fwd_mutex);

  for(pp = &cb_fwd_head; *pp != NULL; pp = &(*pp)->next_active)
  {
    if(*pp != fwd)
      continue;

    *pp = fwd->next_active;
    cb_fwd_count--;
    break;
  }

  *out = *fwd;

  pthread_mutex_unlock(&cb_fwd_mutex);

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
// operator-timescale unload. Closing it would need core to wait on a
// lock a curl worker holds.
static void
cb_exch_unmap_cb(uintptr_t lo, uintptr_t hi, void *data)
{
  uint32_t orphaned = 0;

  (void)data;

  pthread_mutex_lock(&cb_fwd_mutex);

  for(cb_exch_fwd_t *f = cb_fwd_head; f != NULL; f = f->next_active)
  {
    uintptr_t cb = 0;

    switch(f->type)
    {
      case CB_FWD_ORDER:    cb = (uintptr_t)fn_addr(&f->cb.order);    break;
      case CB_FWD_ORDERS:   cb = (uintptr_t)fn_addr(&f->cb.orders);   break;
      case CB_FWD_ACCOUNTS: cb = (uintptr_t)fn_addr(&f->cb.accounts); break;
      case CB_FWD_FILLS:    cb = (uintptr_t)fn_addr(&f->cb.fills);    break;
      case CB_FWD_CANDLES:  cb = (uintptr_t)fn_addr(&f->cb.candles);  break;
      case CB_FWD_TICKERS:  cb = (uintptr_t)fn_addr(&f->cb.tickers);  break;
    }

    if(cb == 0 || cb < lo || cb >= hi)
      continue;

    // memset rather than one arm's NULL: the arms are a union, and
    // all-bits-zero is the null test every adapter makes.
    memset(&f->cb, 0, sizeof(f->cb));
    f->user = NULL;
    orphaned++;
  }

  pthread_mutex_unlock(&cb_fwd_mutex);

  if(orphaned > 0)
    clam(CLAM_WARN, CB_CTX, "%u exchange request(s) lost their consumer "
        "to an unload; they will complete and deliver nothing", orphaned);
}

void
cb_exch_init(void)
{
  plugin_unmap_notify_register(cb_exch_unmap_cb, NULL);
}

void
cb_exch_deinit(void)
{
  uint32_t stranded;

  plugin_unmap_notify_unregister(cb_exch_unmap_cb);

  pthread_mutex_lock(&cb_fwd_mutex);
  stranded = cb_fwd_count;
  pthread_mutex_unlock(&cb_fwd_mutex);

  // Nothing to free: those contexts belong to requests curl still owns,
  // and their adapters live in the mapping now going away. Core's
  // residual audit sees them — cb_curl_done is curl_iter_req_t.cb for
  // every one — so it is the audit that refuses the dlclose, not us.
  // Naming the count here is what makes that refusal legible.
  if(stranded > 0)
    clam(CLAM_WARN, CB_CTX, "%u exchange request(s) still in flight at "
        "deinit", stranded);
}

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
  exchange_order_result_t   out;
  cb_exch_fwd_t             fwd;

  if(user == NULL)
    return;

  cb_fwd_retire(user, &fwd);

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);
  cb_to_exch_order(&res->order, &out.order);

  if(fwd.cb.order != NULL)
    fwd.cb.order(&out, fwd.user);
}

static void
cb_exch_orders_done_adapter(const coinbase_orders_result_t *res, void *user)
{
  exchange_orders_result_t  out;
  cb_exch_fwd_t             fwd;
  uint32_t                  i;
  uint32_t                  n;

  if(user == NULL)
    return;

  cb_fwd_retire(user, &fwd);

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);

  n = res->count;

  if(n > EXCHANGE_MAX_ORDERS_LIST)
    n = EXCHANGE_MAX_ORDERS_LIST;

  for(i = 0; i < n; i++)
    cb_to_exch_order(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd.cb.orders != NULL)
    fwd.cb.orders(&out, fwd.user);
}

static void
cb_exch_accounts_done_adapter(const coinbase_accounts_result_t *res,
    void *user)
{
  exchange_accounts_result_t   out;
  cb_exch_fwd_t                fwd;
  uint32_t                     i;
  uint32_t                     n;

  if(user == NULL)
    return;

  cb_fwd_retire(user, &fwd);

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);

  n = res->count;

  if(n > EXCHANGE_MAX_ACCOUNTS)
    n = EXCHANGE_MAX_ACCOUNTS;

  for(i = 0; i < n; i++)
    cb_to_exch_account(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd.cb.accounts != NULL)
    fwd.cb.accounts(&out, fwd.user);
}

static void
cb_exch_fills_done_adapter(const coinbase_fills_result_t *res, void *user)
{
  exchange_fills_result_t    out;
  cb_exch_fwd_t              fwd;
  uint32_t                   i;
  uint32_t                   n;

  if(user == NULL)
    return;

  cb_fwd_retire(user, &fwd);

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);

  n = res->count;

  if(n > EXCHANGE_MAX_FILLS_LIST)
    n = EXCHANGE_MAX_FILLS_LIST;

  for(i = 0; i < n; i++)
    cb_to_exch_fill(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd.cb.fills != NULL)
    fwd.cb.fills(&out, fwd.user);
}

static void
cb_exch_candles_done_adapter(const coinbase_candles_result_t *res,
    void *user)
{
  exchange_candles_result_t   out;
  cb_exch_fwd_t               fwd;
  uint32_t                    i;
  uint32_t                    n;

  if(user == NULL)
    return;

  cb_fwd_retire(user, &fwd);

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);

  n = res->count;

  if(n > EXCHANGE_MAX_CANDLES)
    n = EXCHANGE_MAX_CANDLES;

  for(i = 0; i < n; i++)
    cb_to_exch_candle(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd.cb.candles != NULL)
    fwd.cb.candles(&out, fwd.user);
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
  cb_exch_fwd_t              *fwd;

  if(req == NULL || cb == NULL)
  {
    cb_exch_fail_order(cb, user, "invalid place_order arguments");
    return(FAIL);
  }

  fwd           = cb_fwd_new(CB_FWD_ORDER, user);
  fwd->cb.order = cb;

  // The neutral `req->*` buffers are sized at-or-above coinbase's
  // per-field caps; copy via memcpy after a bounded strnlen so we
  // truncate rather than tripping -Wformat-truncation on the snprintf
  // bound mismatch. The coinbase request path enforces its own length
  // limits at submit time, so a truncated id surfaces as a clean
  // server-side rejection.
  memset(&inner, 0, sizeof(inner));
  {
    size_t n;

    n = strnlen(req->product_id, sizeof(inner.product_id) - 1);
    memcpy(inner.product_id, req->product_id, n);
    inner.product_id[n] = '\0';

    n = strnlen(req->side, sizeof(inner.side) - 1);
    memcpy(inner.side, req->side, n);
    inner.side[n] = '\0';

    n = strnlen(req->type, sizeof(inner.type) - 1);
    memcpy(inner.type, req->type, n);
    inner.type[n] = '\0';

    n = strnlen(req->tif, sizeof(inner.tif) - 1);
    memcpy(inner.tif, req->tif, n);
    inner.tif[n] = '\0';

    n = strnlen(req->client_oid, sizeof(inner.client_oid) - 1);
    memcpy(inner.client_oid, req->client_oid, n);
    inner.client_oid[n] = '\0';
  }
  inner.price     = req->price;
  inner.size      = req->size;
  inner.funds     = req->funds;
  inner.post_only = req->post_only;

  return(coinbase_place_order_async(&inner,
        cb_exch_order_done_adapter, fwd));
}

static bool
cb_exch_cancel_order_async(const char *order_id,
    exchange_done_order_cb_t cb, void *user)
{
  cb_exch_fwd_t *fwd;

  if(cb == NULL)
    return(FAIL);

  if(order_id == NULL || order_id[0] == '\0')
  {
    cb_exch_fail_order(cb, user, "order_id required");
    return(FAIL);
  }

  fwd           = cb_fwd_new(CB_FWD_ORDER, user);
  fwd->cb.order = cb;

  return(coinbase_cancel_order_async(order_id,
        cb_exch_order_done_adapter, fwd));
}

static bool
cb_exch_get_order_async(const char *order_id,
    exchange_done_order_cb_t cb, void *user)
{
  cb_exch_fwd_t *fwd;

  if(cb == NULL)
    return(FAIL);

  if(order_id == NULL || order_id[0] == '\0')
  {
    cb_exch_fail_order(cb, user, "order_id required");
    return(FAIL);
  }

  fwd           = cb_fwd_new(CB_FWD_ORDER, user);
  fwd->cb.order = cb;

  return(coinbase_get_order_async(order_id,
        cb_exch_order_done_adapter, fwd));
}

static bool
cb_exch_list_orders_async(const char *status, const char *product_id,
    exchange_done_orders_cb_t cb, void *user)
{
  cb_exch_fwd_t *fwd;

  if(cb == NULL)
    return(FAIL);

  fwd            = cb_fwd_new(CB_FWD_ORDERS, user);
  fwd->cb.orders = cb;

  return(coinbase_list_orders_async(status, product_id,
        cb_exch_orders_done_adapter, fwd));
}

static bool
cb_exch_list_fills_async(const char *order_id, const char *product_id,
    int64_t start_ms, exchange_done_fills_cb_t cb, void *user)
{
  cb_exch_fwd_t *fwd;

  if(cb == NULL)
    return(FAIL);

  fwd           = cb_fwd_new(CB_FWD_FILLS, user);
  fwd->cb.fills = cb;

  return(coinbase_list_fills_async(order_id, product_id, start_ms,
        cb_exch_fills_done_adapter, fwd));
}

static bool
cb_exch_get_accounts_async(exchange_done_accounts_cb_t cb, void *user)
{
  cb_exch_fwd_t *fwd;

  if(cb == NULL)
    return(FAIL);

  fwd              = cb_fwd_new(CB_FWD_ACCOUNTS, user);
  fwd->cb.accounts = cb;

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
  cb_exch_fwd_t *fwd;
  int32_t        cb_gran = 0;
  int64_t        start_s;
  int64_t        end_s;

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

  fwd             = cb_fwd_new(CB_FWD_CANDLES, user);
  fwd->cb.candles = cb;

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
// MW-1: bulk-ticker fetch                                              //
//                                                                      //
// Calls Coinbase Advanced Trade `GET /api/v3/brokerage/market/products` //
// (public, unauthenticated) and translates the products array into the //
// neutral exchange_ticker_snapshot_t shape. Coinbase product_id is     //
// already canonical (BTC-USD), so no cache lookup is needed. Numeric  //
// fields arrive as JSON strings; parse with strtod. Fields the         //
// endpoint does not publish (hi/lo/vwap/trades) are set to the absent //
// sentinels (NAN / UINT64_MAX) so consumers can distinguish them from //
// honest zeros. FCM (futures) products are filtered out via            //
// `product_type == "SPOT"` when the field is present.                  //
// ------------------------------------------------------------------ //

static double
cb_json_str_double_local(struct json_object *obj, const char *key)
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

static exchange_ticker_status_t
cb_map_product_status(const char *s)
{
  if(s == NULL || s[0] == '\0')
    return(EXCH_TICK_UNKNOWN);
  if(strcmp(s, "online") == 0)
    return(EXCH_TICK_ONLINE);
  if(strcmp(s, "trading_disabled") == 0 || strcmp(s, "delisted") == 0)
    return(EXCH_TICK_OFFLINE);
  if(strcmp(s, "post_only") == 0)
    return(EXCH_TICK_POST_ONLY);
  if(strcmp(s, "limit_only") == 0)
    return(EXCH_TICK_LIMIT_ONLY);
  return(EXCH_TICK_UNKNOWN);
}

// Deliver from a RETIRED context, whose callback is either the
// consumer's or NULL because the consumer was unloaded mid-flight.
static void
cb_exch_deliver_tickers(const cb_exch_fwd_t *f, bool ok, const char *err,
    const exchange_ticker_snapshot_t *rows, size_t n)
{
  if(f->cb.tickers != NULL)
    f->cb.tickers(ok, err, rows, n, f->user);
}

static void
cb_exch_tickers_resp(int http_status, const char *body, size_t body_len,
    const char *err, void *user)
{
  struct json_object          *root;
  struct json_object          *products;
  struct json_object          *v;
  exchange_ticker_snapshot_t  *rows = NULL;
  cb_exch_fwd_t                fwd;
  size_t                       row_cap;
  size_t                       kept = 0;
  int                          len;
  int                          i;

  (void)http_status;

  if(user == NULL)
    return;

  // Retired up front: every exit below delivers through the stack copy,
  // so no path can miss the unlink.
  cb_fwd_retire(user, &fwd);

  if(err != NULL)
  {
    cb_exch_deliver_tickers(&fwd, false, err, NULL, 0);
    return;
  }

  root = json_parse_buf(body, body_len, CB_CTX);

  if(root == NULL)
  {
    cb_exch_deliver_tickers(&fwd, false,
        "malformed JSON from Coinbase products", NULL, 0);
    return;
  }

  if(!json_object_is_type(root, json_type_object) ||
     !json_object_object_get_ex(root, "products", &products) ||
     !json_object_is_type(products, json_type_array))
  {
    json_object_put(root);
    cb_exch_deliver_tickers(&fwd, false,
        "unexpected Coinbase products response shape", NULL, 0);
    return;
  }

  len = (int)json_object_array_length(products);

  if(len <= 0)
  {
    json_object_put(root);
    cb_exch_deliver_tickers(&fwd, true, NULL, NULL, 0);
    return;
  }

  row_cap = (size_t)len;

  if(row_cap > EXCHANGE_TICKERS_MAX)
  {
    clam(CLAM_WARN, CB_CTX,
        "tickers: products array %d exceeds cap %d; truncating",
        len, (int)EXCHANGE_TICKERS_MAX);
    row_cap = EXCHANGE_TICKERS_MAX;
  }

  rows = mem_alloc(CB_CTX, "exch.tickers",
      row_cap * sizeof(*rows));

  if(rows == NULL)
  {
    json_object_put(root);
    cb_exch_deliver_tickers(&fwd, false, "out of memory", NULL, 0);
    return;
  }

  for(i = 0; i < len && kept < row_cap; i++)
  {
    struct json_object         *row = json_object_array_get_idx(products, i);
    exchange_ticker_snapshot_t *out;
    const char                 *pid;
    const char                 *ptype;
    const char                 *pstatus;
    double                      price;
    double                      vol_base;

    if(row == NULL || !json_object_is_type(row, json_type_object))
      continue;

    // SPOT-only when product_type is present; tolerate older responses
    // that omit the field.
    if(json_object_object_get_ex(row, "product_type", &v) &&
       json_object_is_type(v, json_type_string))
    {
      ptype = json_object_get_string(v);
      if(ptype != NULL && strcmp(ptype, "SPOT") != 0)
        continue;
    }

    if(!json_object_object_get_ex(row, "product_id", &v) ||
       !json_object_is_type(v, json_type_string))
      continue;

    pid = json_object_get_string(v);
    if(pid == NULL || pid[0] == '\0')
      continue;

    out = &rows[kept];
    memset(out, 0, sizeof(*out));
    snprintf(out->product_id, sizeof(out->product_id), "%s", pid);

    price    = cb_json_str_double_local(row, "price");
    vol_base = cb_json_str_double_local(row, "volume_24h");

    out->price          = price;
    out->pct_24h        = cb_json_str_double_local(row,
                              "price_percentage_change_24h");
    out->vol_24h_base   = vol_base;
    // Approximate quote-volume from base × last; the products endpoint
    // does not carry a separate quote-volume field. NaN when either
    // input is absent so consumers don't multiply garbage.
    out->vol_24h_quote  = (!isnan(vol_base) && !isnan(price))
                              ? vol_base * price : NAN;
    out->hi_24h         = NAN;
    out->lo_24h         = NAN;
    out->vwap_24h       = NAN;
    out->num_trades_24h = UINT64_MAX;

    pstatus = NULL;
    if(json_object_object_get_ex(row, "status", &v) &&
       json_object_is_type(v, json_type_string))
      pstatus = json_object_get_string(v);

    out->status = cb_map_product_status(pstatus);
    kept++;
  }

  clam(CLAM_DEBUG2, CB_CTX,
      "tickers: products=%d kept=%zu", len, kept);

  cb_exch_deliver_tickers(&fwd, true, NULL, rows, kept);

  mem_free(rows);
  json_object_put(root);
}

static bool
cb_exch_fetch_all_tickers_async(exchange_done_tickers_cb_t cb, void *user)
{
  cb_exch_fwd_t *fwd;
  cb_exch_fwd_t  dead;

  if(cb == NULL)
    return(FAIL);

  fwd             = cb_fwd_new(CB_FWD_TICKERS, user);
  fwd->cb.tickers = cb;

  if(exchange_request("coinbase", EXCHANGE_PRIO_MARKET_BACKFILL,
        EXCHANGE_OP_REST_GET,
        "/api/v3/brokerage/market/products", NULL,
        cb_exch_tickers_resp, fwd) != SUCCESS)
  {
    cb_fwd_retire(fwd, &dead);
    cb(false, "failed to submit Coinbase products request",
        NULL, 0, user);
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

  // WM-OR-1 capability hook.
  .is_authenticated    = cb_exch_is_authenticated,
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

  // MW-1 capability hook.
  .fetch_all_tickers   = cb_exch_fetch_all_tickers_async,
};

bool
cb_exchange_register_vtable(void)
{
  return(exchange_register("coinbase", &cb_vtable));
}
