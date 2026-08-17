// kraken_rest.h — Kraken REST mechanism + freelist.
//
// KR-4 lands the signed-request submitter, the response classifier,
// the form-urlencoded body builder, and the per-request freelist that
// every typed wrapper in kraken_orders.c shares. The submitter is
// split public/private:
//
//   kr_submit_public  — GET /0/public/<path>, no headers
//   kr_submit_private — POST /0/private/<path>, mints + signs nonce
//
// Both deliver completion through curl_done_cb_t and pass `user_data`
// verbatim back through curl_response_t::user_data so the typed
// wrappers (kraken_orders.c) and the exchange-vtable submit hook
// (kraken_exchange.c) can both consume the same primitive.

#ifndef BM_KRAKEN_REST_H
#define BM_KRAKEN_REST_H

#include "curl.h"
#include "kraken_api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ------------------------------------------------------------------
// Lifecycle (paired with kr_init / kr_deinit in kraken.c).
// ------------------------------------------------------------------

void    kr_rest_init(void);
void    kr_rest_deinit(void);

// From kr_stop(): ground the plugin's curl_flight_t, cancel whatever is
// on the wire and wait up to `ms` for the callbacks to finish with the
// state kr_deinit() is about to tear down. Returns the number of pieces
// of work still open — a non-zero return is a stop() that must refuse.
// Every kraken transfer rides this flight, whichever submitter sent it.
uint32_t kr_rest_drain(uint32_t ms);

// Give a slot back. For the two request shapes that are not
// kr_request_t — the exchange-vtable handle and the WS token fetch —
// whose terminal paths live in other translation units. The LAST
// statement of that path: it is what the drain waits for, so anything
// after it runs against state deinit() may already have freed.
void    kr_rest_slot_close(uint64_t slot);

// ------------------------------------------------------------------
// Request context shared across typed wrappers + the curl completion
// adapters. Freelist-managed; exactly one callback member is valid
// per `type`. Signed POST paths populate `body`/`body_len`; GETs leave
// them NULL/0.
// ------------------------------------------------------------------

typedef enum
{
  KR_REQ_CANDLES,
  KR_REQ_BALANCE,
  KR_REQ_ADD_ORDER,
  KR_REQ_CANCEL_ORDER,
  KR_REQ_QUERY_ORDER,
  KR_REQ_OPEN_ORDERS,
  KR_REQ_CLOSED_ORDERS,
  KR_REQ_TRADES_HISTORY,
  KR_REQ_ASSETPAIRS
} kr_req_type_t;

typedef struct kr_request
{
  kr_req_type_t  type;

  // Candle selectors. Persisted for logging / response tagging.
  char           product_id[KRAKEN_PRODUCT_ID_SZ];
  uint32_t       interval_min;
  int64_t        since_sec;

  // Order / fills selectors.
  char           order_id[KRAKEN_ORDER_ID_SZ];
  char           filter_product_id[KRAKEN_PRODUCT_ID_SZ];

  // Signed POST body. Form-urlencoded; ownership transfers to the
  // request and is freed by kr_req_release.
  char          *body;
  size_t         body_len;

  // Typed completion callback. Exactly one member is valid per `type`.
  union
  {
    kraken_done_candles_cb_t      candles;
    kraken_done_order_cb_t        order;
    kraken_done_orders_cb_t       orders;
    kraken_done_balances_cb_t     balances;
    kraken_done_fills_cb_t        fills;
    kraken_done_assetpairs_cb_t   assetpairs;
  } cb;
  void          *user;

  // This request's slot in the plugin's flight: the submitter opens it,
  // kr_req_release closes it. 0 until the first submit, and again once
  // it is closed.
  uint64_t       slot;

  struct kr_request *next;   // freelist linkage
} kr_request_t;

// Freelist helpers. Zero-initialized on hand-out; caller populates
// `type`, `cb.<member>`, `user`, and any selectors before submitting.
kr_request_t *  kr_req_alloc(void);
void            kr_req_release(kr_request_t *r);

// ------------------------------------------------------------------
// Response classification.
//
// Kraken almost always returns HTTP 200 even for application errors;
// the error envelope lives inside the JSON body's `error[]` array.
// The classifier inspects both layers and decides which retry budget
// the abstraction should apply. KR_RESP_RATE_LIMIT / KR_RESP_TRANSPORT
// are retry-eligible; KR_RESP_HARD_ERROR is terminal.
// ------------------------------------------------------------------

typedef enum
{
  KR_RESP_OK,
  KR_RESP_RATE_LIMIT,
  KR_RESP_HARD_ERROR,
  KR_RESP_TRANSPORT
} kr_resp_kind_t;

// Classify a response delivered through curl_done_cb_t. `resp->error`
// is preferred when set; otherwise the JSON body's first `error[]`
// entry is inspected. On any non-OK kind a human-readable message is
// written into `err_out`.
kr_resp_kind_t  kr_classify_curl(const curl_response_t *resp,
                    char *err_out, size_t err_cap);

// Same classifier but for the exchange-abstraction response shape
// (http_status, body, body_len, err). Useful for the candles path
// that flows through exchange_request.
kr_resp_kind_t  kr_classify_exchange(int http_status, const char *body,
                    size_t body_len, const char *err_hint,
                    char *err_out, size_t err_cap);

// ------------------------------------------------------------------
// URL helpers.
// ------------------------------------------------------------------

// Reads plugin.kraken.rest_url into `out`. Returns FAIL when the KV is
// unset / overflows the buffer.
bool    kr_rest_base_url(char *out, size_t cap);

// ------------------------------------------------------------------
// Submitters.
// ------------------------------------------------------------------

// Submit a public REST GET. `path` is the part AFTER /0/public/ —
// e.g. "OHLC?pair=XBTUSD&interval=1&since=12345". `user_data` is
// echoed verbatim back through curl_response_t::user_data; `prio`
// follows the CURL_PRIO_* byte values (which match EXCHANGE_PRIO_*).
//
// `slot` is the caller's flight handle — &r->slot, &h->slot, whatever
// the completion callback will still be holding when it closes it. The
// submitter opens it just before the transfer is built and gives it
// back on every failure of its own, so a FAIL return leaves nothing to
// close.
//
// Returns FAIL when the base URL is unset, the path overflows, the
// plugin is stopping, or curl rejects the request. On FAIL the caller
// must emit any user-facing failure callback (the request context is
// not released here).
bool    kr_submit_public(void *user_data, uint64_t *slot, uint8_t prio,
            const char *path, curl_done_cb_t done_cb);

// Submit a private REST POST. `path` is the part AFTER /0/private/.
// `body` is the caller's form-urlencoded payload (without the nonce
// prefix); pass NULL/0 for endpoints that take only the nonce. The
// helper mints a fresh nonce via kr_next_nonce, prepends it to the
// body, signs the uripath + nonce + body via kr_sign_request, and
// attaches `API-Key` + `API-Sign` + `Content-Type:
// application/x-www-form-urlencoded` headers.
//
// FAILs early on absent credentials. On any non-FAIL path the body
// caller passed in is left untouched.
bool    kr_submit_private(void *user_data, uint64_t *slot, uint8_t prio,
            const char *path, const char *body, size_t body_len,
            curl_done_cb_t done_cb);

// ------------------------------------------------------------------
// Form-urlencoded body builder.
//
// Pre-allocated buffer (caller-owned). Appends `key=value` pairs with
// leading `&` after the first, URL-encoding the value per RFC 3986
// unreserved set ([A-Za-z0-9-._~]). Returns FAIL when the buffer
// would overflow.
// ------------------------------------------------------------------

typedef struct
{
  char    *buf;
  size_t   cap;
  size_t   len;
} kr_form_t;

void    kr_form_init(kr_form_t *f, char *buf, size_t cap);

bool    kr_form_add(kr_form_t *f, const char *key, const char *value);
bool    kr_form_add_int   (kr_form_t *f, const char *key, int64_t v);
bool    kr_form_add_uint  (kr_form_t *f, const char *key, uint64_t v);
bool    kr_form_add_double(kr_form_t *f, const char *key, double v);
bool    kr_form_add_bool  (kr_form_t *f, const char *key, bool v);

#endif // BM_KRAKEN_REST_H
