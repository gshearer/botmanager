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
#define CB_EXCHANGE_BODY_SZ  CB_PRESIGN_SZ

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

static bool
cb_exchange_submit(void *handle, exchange_response_cb_t cb, void *user)
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

  if(is_private)
  {
    // EX-1 scope: only public REST routes through the abstraction
    // (candles + trades). Routing signed traffic through here requires
    // refactoring cb_submit_private's user_data plumbing — deferred
    // until the live-trading framework is unpaused. See the EX-1
    // outcomes section in TODO.md.
    clam(CLAM_WARN, CB_CTX,
        "exchange submit: private op routed but not supported in EX-1");
    return(FAIL);
  }

  // Public GET — the simple path. cb_submit_public gives the curl
  // request `h` as its user_data, so cb_exchange_curl_done sees the
  // exchange handle directly.
  if(cb_submit_public(h, h->path, cb_exchange_curl_done) != SUCCESS)
    return(FAIL);

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

// File-scope vtable. Static storage so the abstraction can keep the
// pointer; advertised_rps reflects Coinbase Exchange's public-API cap.
static const exchange_protocol_vtable_t cb_vtable = {
  .build_request    = cb_exchange_build_request,
  .submit           = cb_exchange_submit,
  .free_request     = cb_exchange_free_request,
  .advertised_rps   = 10,
  .advertised_burst = 15,
};

bool
cb_exchange_register_vtable(void)
{
  return(exchange_register("coinbase", &cb_vtable));
}
