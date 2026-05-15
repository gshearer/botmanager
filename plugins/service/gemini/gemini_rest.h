// gemini_rest.h — Gemini REST mechanism + freelist.
//
// GEM-1 lands the signed-request submitter, the response classifier,
// and the per-request freelist that every typed wrapper in
// gemini_orders.c shares. The submitter is split public/private:
//
//   gem_submit_public  — GET <rest_url><path>, no headers
//   gem_submit_private — POST <rest_url><path>, mints + signs nonce
//                        inside the payload JSON; HTTP body is empty
//
// Both deliver completion through curl_done_cb_t and pass `user_data`
// verbatim back through curl_response_t::user_data so the typed
// wrappers (gemini_orders.c) and the exchange-vtable submit hook
// (gemini_exchange.c) can both consume the same primitive.

#ifndef BM_GEMINI_REST_H
#define BM_GEMINI_REST_H

#include "curl.h"
#include "gemini_api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ------------------------------------------------------------------
// Lifecycle (paired with gem_init / gem_deinit in gemini.c).
// ------------------------------------------------------------------

void    gem_rest_init(void);
void    gem_rest_deinit(void);

// ------------------------------------------------------------------
// Request context shared across typed wrappers + the curl completion
// adapters. Freelist-managed; exactly one callback member is valid
// per `type`. Signed POST paths populate `payload`/`payload_len`;
// GETs leave them NULL/0.
// ------------------------------------------------------------------

typedef enum
{
  GEM_REQ_CANDLES,
  GEM_REQ_BALANCE,
  GEM_REQ_ADD_ORDER,
  GEM_REQ_CANCEL_ORDER,
  GEM_REQ_QUERY_ORDER,
  GEM_REQ_ACTIVE_ORDERS,
  GEM_REQ_MYTRADES,
  GEM_REQ_SYMBOLS,
  GEM_REQ_SYMBOL_DETAILS
} gem_req_type_t;

typedef struct gem_request
{
  gem_req_type_t  type;

  // Candle selectors.
  char           product_id[GEMINI_PRODUCT_ID_SZ];
  char           granularity[8];     // "1m" / "5m" / "1hr" / "1day"
  int64_t        since_ms;
  int64_t        until_ms;

  // Order / fills selectors.
  char           order_id[GEMINI_ORDER_ID_SZ];
  char           filter_product_id[GEMINI_PRODUCT_ID_SZ];

  // Symbols-details: which symbol this request is fetching.
  char           detail_symbol[16];

  // Signed POST payload (JSON envelope). Owned by the request; freed
  // by gem_req_release.
  char          *payload;
  size_t         payload_len;

  // Typed completion callback. Exactly one member is valid per `type`.
  union
  {
    gemini_done_candles_cb_t   candles;
    gemini_done_order_cb_t     order;
    gemini_done_orders_cb_t    orders;
    gemini_done_balances_cb_t  balances;
    gemini_done_fills_cb_t     fills;
    gemini_done_symbols_cb_t   symbols;
  } cb;
  void          *user;

  // Symbols-refresh aggregation state. The populator allocates a
  // single batch-control block tracked here so per-symbol detail
  // responses can converge on a single user callback. Owned by the
  // batch; per-request pointer is borrowed.
  void          *batch;

  struct gem_request *next;   // freelist linkage
} gem_request_t;

// Freelist helpers. Zero-initialized on hand-out; caller populates
// `type`, `cb.<member>`, `user`, and any selectors before submitting.
gem_request_t * gem_req_alloc(void);
void            gem_req_release(gem_request_t *r);

// ------------------------------------------------------------------
// Response classification.
//
// Gemini surfaces errors two ways:
//   * HTTP status non-2xx with a JSON body
//        { "result":"error", "reason":"...", "message":"..." }
//   * Rarely (older endpoints) HTTP 200 with the same envelope.
//
// The classifier inspects both layers. 429 + 5xx are retry-eligible;
// hard errors are terminal.
// ------------------------------------------------------------------

typedef enum
{
  GEM_RESP_OK,
  GEM_RESP_RATE_LIMIT,
  GEM_RESP_HARD_ERROR,
  GEM_RESP_TRANSPORT
} gem_resp_kind_t;

// Classify a response delivered through curl_done_cb_t. On any non-OK
// kind a human-readable message is written into `err_out`.
gem_resp_kind_t  gem_classify_curl(const curl_response_t *resp,
                     char *err_out, size_t err_cap);

// Same classifier but for the exchange-abstraction response shape
// (http_status, body, body_len, err). Useful for the candles path
// that flows through exchange_request.
gem_resp_kind_t  gem_classify_exchange(int http_status, const char *body,
                     size_t body_len, const char *err_hint,
                     char *err_out, size_t err_cap);

// ------------------------------------------------------------------
// URL helpers.
// ------------------------------------------------------------------

// Reads plugin.gemini.rest_url into `out`. Returns FAIL when the KV is
// unset / overflows the buffer.
bool    gem_rest_base_url(char *out, size_t cap);

// ------------------------------------------------------------------
// Submitters.
// ------------------------------------------------------------------

// Submit a public REST GET. `path` is the full path portion of the URL
// (e.g. "/v1/symbols", "/v2/candles/btcusd/1m"). `user_data` is echoed
// verbatim back through curl_response_t::user_data; `prio` follows the
// CURL_PRIO_* byte values (which match EXCHANGE_PRIO_*).
//
// Returns FAIL when the base URL is unset, the path overflows, or
// curl rejects the request. On FAIL the caller must emit any user-
// facing failure callback (the request context is not released here).
bool    gem_submit_public(void *user_data, uint8_t prio,
            const char *path, curl_done_cb_t done_cb);

// Submit a private REST POST. `path` is the path portion of the URL
// (e.g. "/v1/balances", "/v1/order/new"). `payload_json` is the caller's
// pre-composed JSON payload. The helper:
//
//   1. base64-encodes `payload_json` (via gem_b64_encode),
//   2. HMAC-SHA384 signs the encoded payload (via gem_sign_request),
//   3. attaches X-GEMINI-APIKEY, X-GEMINI-PAYLOAD, X-GEMINI-SIGNATURE,
//      Content-Type: text/plain, Content-Length: 0, Cache-Control:
//      no-cache,
//   4. POSTs with an empty body.
//
// FAILs early on absent credentials. The caller-provided
// `payload_json` is left untouched.
bool    gem_submit_private(void *user_data, uint8_t prio,
            const char *path, const char *payload_json,
            size_t payload_len, curl_done_cb_t done_cb);

#endif // BM_GEMINI_REST_H
