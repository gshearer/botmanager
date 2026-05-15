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
// Capability hooks — GEM-1 only ships is_authenticated. The rest land //
// in GEM-2 (REST) and GEM-3 (WS). Leaving the slots NULL is exactly   //
// what feature_exchange's public shims expect: every capability call  //
// (place_order, fetch_candles, etc.) FAILs synchronously with the     //
// abstraction's standard `verb unsupported` error string.             //
// ------------------------------------------------------------------ //

static bool
gem_exch_is_authenticated(void)
{
  return(gem_apikey_configured());
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

  // Capability hooks land in later chunks. NULL here means the
  // public exchange_*_async shim FAILs synchronously with a stable
  // `<verb> unsupported` error string — by design.
  .place_order_async   = NULL,
  .cancel_order_async  = NULL,
  .get_order_async     = NULL,
  .list_orders_async   = NULL,
  .list_fills_async    = NULL,
  .get_accounts_async  = NULL,
  .fetch_candles_async = NULL,
  .ws_subscribe        = NULL,
  .ws_unsubscribe      = NULL,
};

bool
gem_exchange_register_vtable(void)
{
  return(exchange_register("gemini", &gem_vtable));
}
