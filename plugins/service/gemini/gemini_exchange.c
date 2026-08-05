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
#include "json.h"

#include "gemini_pairs.h"

#include <math.h>
#include <stdint.h>
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

// ------------------------------------------------------------------ //
// Per-call adapter context, and the in-flight registry over it         //
// ------------------------------------------------------------------ //

// One context per dispatch: it carries the consumer's typed callback
// across the inner gemini_*_async call and is freed by the adapter that
// delivers it. Six shapes of callback, one struct — they differ only in
// which arm of the union is live, and a single type is what lets one
// registry walk them all.
typedef enum
{
  GEM_FWD_ORDER,
  GEM_FWD_ORDERS,
  GEM_FWD_ACCOUNTS,
  GEM_FWD_FILLS,
  GEM_FWD_CANDLES,
  GEM_FWD_TICKERS
} gem_fwd_type_t;

typedef struct gem_exch_fwd
{
  gem_fwd_type_t type;

  union
  {
    exchange_done_order_cb_t     order;
    exchange_done_orders_cb_t    orders;
    exchange_done_accounts_cb_t  accounts;
    exchange_done_fills_cb_t     fills;
    exchange_done_candles_cb_t   candles;
    exchange_done_tickers_cb_t   tickers;
  } cb;
  void          *user;

  struct gem_exch_fwd *next_active;
} gem_exch_fwd_t;

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
// WS subscriptions are deliberately not filed, for the reason coinbase's
// identical section states in full: whenmoon drains every binding
// through exchange_ws_unsubscribe on both of its teardown paths.
static pthread_mutex_t gem_fwd_mutex = PTHREAD_MUTEX_INITIALIZER;
static gem_exch_fwd_t *gem_fwd_head  = NULL;
static uint32_t        gem_fwd_count = 0;

// Allocate a dispatch context with its consumer half installed and file
// it before anything can be submitted, never after: a completion can run
// on a curl worker before the submitting call has returned.
static gem_exch_fwd_t *
gem_fwd_new(gem_fwd_type_t type, void *user)
{
  gem_exch_fwd_t *fwd = mem_alloc(GEM_CTX, "exch.fwd", sizeof(*fwd));

  memset(fwd, 0, sizeof(*fwd));
  fwd->type = type;
  fwd->user = user;

  pthread_mutex_lock(&gem_fwd_mutex);

  fwd->next_active = gem_fwd_head;
  gem_fwd_head     = fwd;
  gem_fwd_count++;

  pthread_mutex_unlock(&gem_fwd_mutex);

  return(fwd);
}

// Unlink `fwd`, copy it to `out` and free it. The copy is taken under
// the lock so an adapter reads the consumer's callback in the same
// critical section the unmap sweep would null it in — read it afterwards
// and the two interleave, which is the whole bug.
static void
gem_fwd_retire(gem_exch_fwd_t *fwd, gem_exch_fwd_t *out)
{
  gem_exch_fwd_t **pp;

  pthread_mutex_lock(&gem_fwd_mutex);

  for(pp = &gem_fwd_head; *pp != NULL; pp = &(*pp)->next_active)
  {
    if(*pp != fwd)
      continue;

    *pp = fwd->next_active;
    gem_fwd_count--;
    break;
  }

  *out = *fwd;

  pthread_mutex_unlock(&gem_fwd_mutex);

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
gem_exch_unmap_cb(uintptr_t lo, uintptr_t hi, void *data)
{
  uint32_t orphaned = 0;

  (void)data;

  pthread_mutex_lock(&gem_fwd_mutex);

  for(gem_exch_fwd_t *f = gem_fwd_head; f != NULL; f = f->next_active)
  {
    uintptr_t cb = 0;

    switch(f->type)
    {
      case GEM_FWD_ORDER:    cb = (uintptr_t)fn_addr(&f->cb.order);    break;
      case GEM_FWD_ORDERS:   cb = (uintptr_t)fn_addr(&f->cb.orders);   break;
      case GEM_FWD_ACCOUNTS: cb = (uintptr_t)fn_addr(&f->cb.accounts); break;
      case GEM_FWD_FILLS:    cb = (uintptr_t)fn_addr(&f->cb.fills);    break;
      case GEM_FWD_CANDLES:  cb = (uintptr_t)fn_addr(&f->cb.candles);  break;
      case GEM_FWD_TICKERS:  cb = (uintptr_t)fn_addr(&f->cb.tickers);  break;
    }

    if(cb == 0 || cb < lo || cb >= hi)
      continue;

    // memset rather than one arm's NULL: the arms are a union, and
    // all-bits-zero is the null test every adapter makes.
    memset(&f->cb, 0, sizeof(f->cb));
    f->user = NULL;
    orphaned++;
  }

  pthread_mutex_unlock(&gem_fwd_mutex);

  if(orphaned > 0)
    clam(CLAM_WARN, GEM_CTX, "%u exchange request(s) lost their consumer "
        "to an unload; they will complete and deliver nothing", orphaned);
}

void
gem_exch_init(void)
{
  plugin_unmap_notify_register(gem_exch_unmap_cb, NULL);
}

void
gem_exch_deinit(void)
{
  uint32_t stranded;

  plugin_unmap_notify_unregister(gem_exch_unmap_cb);

  pthread_mutex_lock(&gem_fwd_mutex);
  stranded = gem_fwd_count;
  pthread_mutex_unlock(&gem_fwd_mutex);

  // Nothing to free: those contexts belong to requests curl still owns,
  // and their adapters live in the mapping now going away. Core's
  // residual audit sees them, so it is the audit that refuses the
  // dlclose, not us. Naming the count here makes that refusal legible.
  if(stranded > 0)
    clam(CLAM_WARN, GEM_CTX, "%u exchange request(s) still in flight at "
        "deinit", stranded);
}

// ---- typed-to-generic translation helpers ----
//
// gem_type_to_generic is hoisted to gemini.h as static inline so both
// gemini_exchange.c (this TU) and gemini_ws_channels.c (Order Events
// parser surfacing exchange_ws_user_order_t.status fields) share the
// same collapse without a second TU.

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
  gem_exch_fwd_t            fwd;
  exchange_order_result_t   out;

  if(user == NULL)
    return;

  gem_fwd_retire(user, &fwd);

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);
  gem_to_exch_order(&res->order, &out.order);

  if(fwd.cb.order != NULL)
    fwd.cb.order(&out, fwd.user);
}

static void
gem_exch_orders_done_adapter(const gemini_orders_result_t *res, void *user)
{
  gem_exch_fwd_t             fwd;
  exchange_orders_result_t   out;
  uint32_t                   i;
  uint32_t                   n;

  if(user == NULL)
    return;

  gem_fwd_retire(user, &fwd);

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);

  n = res->count;

  if(n > EXCHANGE_MAX_ORDERS_LIST)
    n = EXCHANGE_MAX_ORDERS_LIST;

  for(i = 0; i < n; i++)
    gem_to_exch_order(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd.cb.orders != NULL)
    fwd.cb.orders(&out, fwd.user);
}

static void
gem_exch_accounts_done_adapter(const gemini_balances_result_t *res,
    void *user)
{
  gem_exch_fwd_t                fwd;
  exchange_accounts_result_t    out;
  uint32_t                      i;
  uint32_t                      n;

  if(user == NULL)
    return;

  gem_fwd_retire(user, &fwd);

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);

  n = res->count;

  if(n > EXCHANGE_MAX_ACCOUNTS)
    n = EXCHANGE_MAX_ACCOUNTS;

  for(i = 0; i < n; i++)
    gem_to_exch_account(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd.cb.accounts != NULL)
    fwd.cb.accounts(&out, fwd.user);
}

static void
gem_exch_fills_done_adapter(const gemini_fills_result_t *res, void *user)
{
  gem_exch_fwd_t              fwd;
  exchange_fills_result_t     out;
  uint32_t                    i;
  uint32_t                    n;

  if(user == NULL)
    return;

  gem_fwd_retire(user, &fwd);

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);

  n = res->count;

  if(n > EXCHANGE_MAX_FILLS_LIST)
    n = EXCHANGE_MAX_FILLS_LIST;

  for(i = 0; i < n; i++)
    gem_to_exch_fill(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd.cb.fills != NULL)
    fwd.cb.fills(&out, fwd.user);
}

static void
gem_exch_candles_done_adapter(const gemini_candles_result_t *res, void *user)
{
  gem_exch_fwd_t              fwd;
  exchange_candles_result_t   out;
  uint32_t                    i;
  uint32_t                    n;

  if(user == NULL)
    return;

  gem_fwd_retire(user, &fwd);

  memset(&out, 0, sizeof(out));
  snprintf(out.err, sizeof(out.err), "%s", res->err);

  n = res->count;

  if(n > EXCHANGE_MAX_CANDLES)
    n = EXCHANGE_MAX_CANDLES;

  for(i = 0; i < n; i++)
    gem_to_exch_candle(&res->rows[i], &out.rows[i]);

  out.count = n;

  if(fwd.cb.candles != NULL)
    fwd.cb.candles(&out, fwd.user);
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
  gem_exch_fwd_t           *fwd;
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

  fwd           = gem_fwd_new(GEM_FWD_ORDER, user);
  fwd->cb.order = cb;

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
  gem_exch_fwd_t       *fwd;

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

  fwd           = gem_fwd_new(GEM_FWD_ORDER, user);
  fwd->cb.order = cb;

  return(gemini_cancel_order_async(order_id, gem_exch_order_done_adapter, fwd));
}

static bool
gem_exch_get_order_async(const char *order_id,
    exchange_done_order_cb_t cb, void *user)
{
  gem_exch_fwd_t       *fwd;

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

  fwd           = gem_fwd_new(GEM_FWD_ORDER, user);
  fwd->cb.order = cb;

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
  gem_exch_fwd_t        *fwd;

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

  fwd            = gem_fwd_new(GEM_FWD_ORDERS, user);
  fwd->cb.orders = cb;

  return(gemini_active_orders_async(gem_exch_orders_done_adapter, fwd));
}

static bool
gem_exch_list_fills_async(const char *order_id, const char *product_id,
    int64_t start_ms, exchange_done_fills_cb_t cb, void *user)
{
  gem_exch_fwd_t       *fwd;

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

  fwd           = gem_fwd_new(GEM_FWD_FILLS, user);
  fwd->cb.fills = cb;

  return(gemini_mytrades_async(product_id, start_ms,
        gem_exch_fills_done_adapter, fwd));
}

static bool
gem_exch_get_accounts_async(exchange_done_accounts_cb_t cb, void *user)
{
  gem_exch_fwd_t          *fwd;

  if(cb == NULL)
    return(FAIL);

  if(!gem_apikey_configured())
  {
    gem_exch_fail_accounts(cb, user, "gemini: api keys not configured");
    return(FAIL);
  }

  fwd              = gem_fwd_new(GEM_FWD_ACCOUNTS, user);
  fwd->cb.accounts = cb;

  return(gemini_get_balance_async(gem_exch_accounts_done_adapter, fwd));
}

// Public market data — no auth gate. The typed wrapper owns the
// granularity translation + client-side window filtering.
static bool
gem_exch_fetch_candles_async(const char *product_id,
    exchange_granularity_t gran, int64_t since_ms, int64_t until_ms,
    exchange_done_candles_cb_t cb, void *user)
{
  gem_exch_fwd_t         *fwd;

  if(cb == NULL)
    return(FAIL);

  if(product_id == NULL || product_id[0] == '\0')
  {
    gem_exch_fail_candles(cb, user, "product_id required");
    return(FAIL);
  }

  fwd             = gem_fwd_new(GEM_FWD_CANDLES, user);
  fwd->cb.candles = cb;

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
// MW-1: bulk-ticker fetch                                              //
//                                                                      //
// `GET /v1/pricefeed` (public). Response is a flat JSON array of      //
// {pair, price, percentChange24h}. Numeric fields are JSON strings.   //
// Pair is the bare lowercase or uppercase concat ("BTCUSD"); the      //
// abstraction-canonical form is the hyphenated uppercase ISO          //
// ("BTC-USD"), produced via gem_pair_to_abstr. Pricefeed publishes    //
// neither volume, hi/lo, vwap, nor trade count — every other field   //
// is set to the absent sentinel.                                       //
// ------------------------------------------------------------------ //

static double
gem_json_str_double(struct json_object *v)
{
  const char *s;

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

// Deliver from a RETIRED context, whose callback is either the
// consumer's or NULL because the consumer was unloaded mid-flight.
static void
gem_exch_deliver_tickers(const gem_exch_fwd_t *f, bool ok, const char *err,
    const exchange_ticker_snapshot_t *rows, size_t n)
{
  if(f->cb.tickers != NULL)
    f->cb.tickers(ok, err, rows, n, f->user);
}

static void
gem_exch_tickers_resp(int http_status, const char *body, size_t body_len,
    const char *err, void *user)
{
  gem_exch_fwd_t               fwd;
  struct json_object          *root;
  exchange_ticker_snapshot_t  *rows = NULL;
  size_t                       row_cap;
  size_t                       kept = 0;
  int                          len;
  int                          i;

  (void)http_status;

  if(user == NULL)
    return;

  gem_fwd_retire(user, &fwd);

  if(err != NULL)
  {
    gem_exch_deliver_tickers(&fwd, false, err, NULL, 0);
    return;
  }

  root = json_parse_buf(body, body_len, GEM_CTX);

  if(root == NULL)
  {
    gem_exch_deliver_tickers(&fwd, false,
        "malformed JSON from Gemini pricefeed", NULL, 0);
    return;
  }

  if(!json_object_is_type(root, json_type_array))
  {
    json_object_put(root);
    gem_exch_deliver_tickers(&fwd, false,
        "unexpected Gemini pricefeed response shape", NULL, 0);
    return;
  }

  len = (int)json_object_array_length(root);

  if(len <= 0)
  {
    json_object_put(root);
    gem_exch_deliver_tickers(&fwd, true, NULL, NULL, 0);
    return;
  }

  row_cap = (size_t)len;
  if(row_cap > EXCHANGE_TICKERS_MAX)
  {
    clam(CLAM_WARN, GEM_CTX,
        "tickers: pricefeed array %d exceeds cap %d; truncating",
        len, (int)EXCHANGE_TICKERS_MAX);
    row_cap = EXCHANGE_TICKERS_MAX;
  }

  rows = mem_alloc(GEM_CTX, "exch.tickers",
      row_cap * sizeof(*rows));

  if(rows == NULL)
  {
    json_object_put(root);
    gem_exch_deliver_tickers(&fwd, false, "out of memory", NULL, 0);
    return;
  }

  for(i = 0; i < len && kept < row_cap; i++)
  {
    struct json_object         *row = json_object_array_get_idx(root, i);
    struct json_object         *v;
    exchange_ticker_snapshot_t *out;
    const char                 *pair;
    char                        canon[EXCHANGE_PRODUCT_ID_SZ];

    if(row == NULL || !json_object_is_type(row, json_type_object))
      continue;

    if(!json_object_object_get_ex(row, "pair", &v) ||
       !json_object_is_type(v, json_type_string))
      continue;

    pair = json_object_get_string(v);
    if(pair == NULL || pair[0] == '\0')
      continue;

    gem_pair_to_abstr(pair, canon, sizeof(canon));

    if(canon[0] == '\0')
    {
      clam(CLAM_DEBUG3, GEM_CTX,
          "tickers: drop uncanonical %s", pair);
      continue;
    }

    out = &rows[kept];
    memset(out, 0, sizeof(*out));
    snprintf(out->product_id, sizeof(out->product_id), "%s", canon);

    out->price = json_object_object_get_ex(row, "price", &v)
                     ? gem_json_str_double(v) : NAN;
    out->pct_24h = json_object_object_get_ex(row, "percentChange24h", &v)
                     ? gem_json_str_double(v) : NAN;
    out->vol_24h_base   = NAN;
    out->vol_24h_quote  = NAN;
    out->hi_24h         = NAN;
    out->lo_24h         = NAN;
    out->vwap_24h       = NAN;
    out->num_trades_24h = UINT64_MAX;
    out->status         = EXCH_TICK_ONLINE;

    kept++;
  }

  clam(CLAM_DEBUG2, GEM_CTX,
      "tickers: pricefeed=%d kept=%zu", len, kept);

  gem_exch_deliver_tickers(&fwd, true, NULL, rows, kept);

  mem_free(rows);
  json_object_put(root);
}

static bool
gem_exch_fetch_all_tickers_async(exchange_done_tickers_cb_t cb, void *user)
{
  gem_exch_fwd_t *fwd;
  gem_exch_fwd_t  dead;

  if(cb == NULL)
    return(FAIL);

  fwd             = gem_fwd_new(GEM_FWD_TICKERS, user);
  fwd->cb.tickers = cb;

  if(exchange_request("gemini", EXCHANGE_PRIO_MARKET_BACKFILL,
        EXCHANGE_OP_REST_GET, "/v1/pricefeed", NULL,
        gem_exch_tickers_resp, fwd) != SUCCESS)
  {
    gem_fwd_retire(fwd, &dead);
    cb(false, "failed to submit Gemini pricefeed request",
        NULL, 0, user);
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

  .ws_subscribe        = gem_ws_subscribe,
  .ws_unsubscribe      = gem_ws_unsubscribe,

  // MW-1 capability hook.
  .fetch_all_tickers   = gem_exch_fetch_all_tickers_async,
};

bool
gem_exchange_register_vtable(void)
{
  return(exchange_register("gemini", &gem_vtable));
}
