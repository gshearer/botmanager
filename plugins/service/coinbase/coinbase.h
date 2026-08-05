// coinbase.h — Coinbase Exchange service plugin (kind: coinbase)
//
// Internal-only declarations. Public mechanism lives in
// coinbase_api.h, consumed by external callers via the dlsym-shim
// block gated with !CB_INTERNAL. Subsystems land per TODO.md
// chunks CB1–CB6.

#ifndef BM_COINBASE_H
#define BM_COINBASE_H

#ifdef CB_INTERNAL

#include "clam.h"
#include "common.h"
#include "curl.h"
#include "kv.h"
#include "alloc.h"
#include "plugin.h"

#include "coinbase_api.h"

#include <stddef.h>
#include <stdint.h>

#define CB_CTX "coinbase"

// Size limits.
// REST / WebSocket base URL buffer. Matches the convention used by
// the coinmarketcap and openweather plugins.
#define CB_URL_SZ        512
// Upper bound on a signed REST request body (the JSON payload of a
// place-order or batch_cancel call). Coinbase order bodies sit well
// under 1 KiB; 4 KiB gives headroom for bulk operations.
#define CB_BODY_SZ       4096
// Error message buffer for transient classifier output. Matches the
// CMC convention.
#define CB_ERR_SZ        128

// CB4 — WebSocket transport sizing knobs.
//
// Coinbase Exchange frames rarely exceed a few KiB; a 64 KiB single-read
// buffer absorbs any normal ticker / level2 / heartbeat in one recv call
// and leaves slack for the server to batch smaller messages. The 1 MiB
// reassembly cap is a protocol guard — a single logical text frame
// that exceeds it is almost certainly malformed and we drop the session
// rather than keep growing.
#define CB_WS_RECV_BUF_SZ       65536
#define CB_WS_ASSEMBLY_CAP      (1u * 1024u * 1024u)
#define CB_WS_MAX_BACKOFF_MS    60000
#define CB_WS_IDLE_TIMEOUT_MS   45000
#define CB_WS_PING_INTERVAL_MS  20000
#define CB_WS_POLL_MS           250
#define CB_WS_SUB_ACK_TIMEOUT_MS 15000
#define CB_WS_STOP_WAIT_MS      5000
#define CB_WS_MAX_CONSEC_FAIL   10

typedef enum
{
  CB_WS_DISCONNECTED,
  CB_WS_CONNECTING,
  CB_WS_OPEN,
  CB_WS_RECONNECTING
} cb_ws_state_t;

// REST request type. Enum values for private endpoints are declared
// here so the union shape is stable across the typed callers.
typedef enum
{
  CB_REQ_CANDLES,
  CB_REQ_PLACE_ORDER,
  CB_REQ_CANCEL_ORDER,
  CB_REQ_GET_ORDER,
  CB_REQ_GET_ACCOUNTS,
  CB_REQ_LIST_FILLS,
} cb_req_type_t;

// REST request context. Freelist-managed; exactly one callback member
// is valid per `type`. Signed POST paths populate `body`/`body_len`;
// GETs leave them NULL/0.
typedef struct cb_request
{
  cb_req_type_t  type;

  // Candle selectors.
  char           product_id[COINBASE_PRODUCT_ID_SZ];
  int32_t        granularity;
  int64_t        start_ts;
  int64_t        end_ts;

  // Signed POST/DELETE body. JSON already rendered by the caller.
  char          *body;
  size_t         body_len;

  // Order selectors.
  char           order_id[COINBASE_ORDER_ID_SZ];
  char           status[COINBASE_STATUS_SZ];

  // Typed completion callback. Exactly one member is valid per `type`.
  union
  {
    coinbase_done_candles_cb_t   candles;
    coinbase_done_order_cb_t     order;
    coinbase_done_orders_cb_t    orders;
    coinbase_done_accounts_cb_t  accounts;
    coinbase_done_fills_cb_t     fills;
  } cb;
  void          *user;

  struct cb_request *next;   // freelist linkage
} cb_request_t;

// coinbase_sign.c — URL + credential helpers.

bool    cb_rest_base_url(char *out, size_t cap);
bool    cb_ws_base_url(char *out, size_t cap);
bool    cb_apikey_configured(void);

// coinbase_sign_cdp.c — Advanced Trade JWT/ES256 signer.
//
// Buffer size for a rendered JWT including the two dots and the
// trailing NUL. ~600-700 B in practice for our claim set; 1024 leaves
// slack for a long key_name string.
#define CB_JWT_SZ        1024

// Build a fresh CDP-style JWT for `method path` against the configured
// REST host. `method` is uppercase ("GET"/"POST"/"DELETE"); `path` is
// the absolute path including any query. `out` receives a
// NUL-terminated JWT on SUCCESS; contents are unspecified on FAIL.
bool    cb_sign_jwt(const char *method, const char *path,
            char *out, size_t cap);

// Mint a CDP JWT suitable for the Advanced Trade WS subscribe payload.
// The "uri" claim is omitted — Advanced Trade WS accepts a single JWT
// per subscribe regardless of channel. Caller mints per subscribe and
// embeds the result in the JSON {... "jwt": "<jwt>"}.
bool    cb_sign_jwt_ws(char *out, size_t cap);

// True iff both plugin.coinbase.creds.key_name and
// plugin.coinbase.creds.private_key_pem are set. Safe to call at any
// time (does not parse the PEM).
bool    cb_cdp_configured(void);

// Release the cached EVP_PKEY + PEM snapshot. Idempotent.
void    cb_cdp_deinit(void);

// coinbase_rest.c

// Lifecycle. Paired with cb_init / cb_deinit in coinbase.c.
void            cb_rest_init(void);
void            cb_rest_deinit(void);

// Freelist-managed request context. Zero-initialized on hand-out;
// caller populates `type`, `cb.<member>`, `user`, and any selectors
// before submitting. cb_req_release() frees any `body` the caller
// attached.
cb_request_t *  cb_req_alloc(void);
void            cb_req_release(cb_request_t *r);

// Translate a curl transport + HTTP status into a human-readable error
// string suitable for forwarding into res->err. Returns NULL on 200.
// `buf` is scratch storage for the cases that compose a message at
// runtime; the returned pointer is either `buf` or a string literal.
const char *    cb_classify_http(const curl_response_t *resp,
                    char *buf, size_t sz);

// Build and submit a signed REST request. `path` is absolute (e.g.
// "/orders" or "/accounts"), may carry a query string, and will be
// appended to the selected base URL. `body` may be NULL for GET /
// DELETE paths; if non-NULL, `body_len` bytes are used verbatim for
// both the signing prehash and the POSTFIELDS payload. `done_cb` fires
// on the curl worker thread with `user_data` echoed back via
// `curl_response_t::user_data`. Returns FAIL if credentials are
// missing, the base URL is unset, or the request could not be queued;
// caller is responsible for emitting the typed failure callback on
// FAIL.
//
// WM-LT-8-A: signature mirrors cb_submit_public — `void *user_data` +
// `uint8_t prio` so the exchange-vtable submit hook can route
// EXCHANGE_OP_PRIVATE_REST_* traffic through here without a typed
// adapter. `prio` byte values match CURL_PRIO_* / EXCHANGE_PRIO_* on
// purpose. Legacy typed callers (orders / accounts) pass their
// `cb_request_t *` as `user_data` and CURL_PRIO_NORMAL as `prio`; the
// exchange-vtable path passes its own handle and the abstraction's
// per-request priority byte.
bool    cb_submit_private(void *user_data, uint8_t prio,
            curl_method_t method, const char *path,
            const char *body, size_t body_len,
            curl_done_cb_t done_cb);

// Build and submit a public (unauthenticated) REST GET. Mirror of
// cb_submit_private for traffic that flows through the exchange
// abstraction. `user_data` of the resulting curl request is the
// caller-supplied pointer, exactly as for cb_submit_private. EX-1
// promoted this from a file-scope static — no thin wrappers per
// AGENTS.md `feedback_no_thin_wrappers.md`.
//
// `prio` is the curl request priority (one of CURL_PRIO_*; byte values
// match EXCHANGE_PRIO_* on purpose so the exchange-vtable submit can
// pass its 8-bit priority through unchanged). Legacy typed callers
// (products / ticker) pass CURL_PRIO_NORMAL.
bool    cb_submit_public(void *user_data, uint8_t prio, const char *path,
            curl_done_cb_t done_cb);

// coinbase_exchange.c — vtable registration with the feature_exchange
// abstraction. Called from cb_init.
bool    cb_exchange_register_vtable(void);

// In-flight registry for the consumer callbacks the vtable borrows.
// cb_exch_init registers the unmap listener that keeps it honest;
// cb_exch_deinit unregisters it and reports anything still airborne.
// Both are idempotent. See the registry section in coinbase_exchange.c
// for why a service with an async API needs this at all.
void    cb_exch_init(void);
void    cb_exch_deinit(void);

// coinbase_ws.c

// Lifecycle. cb_ws_init prepares the session struct and subscribes to
// the config knobs that force a reconnect; it does NOT spawn the reader
// thread. cb_ws_start spawns the reader (latching the current value of
// plugin.coinbase.ws_enabled), and cb_ws_deinit frees reassembly
// buffers + destroys the lock.
//
// cb_ws_stop signals the reader and *joins* it, up to
// CB_WS_STOP_WAIT_MS. FAIL means the thread is still inside this
// plugin's mapping, which makes the unload unsafe — it is propagated
// out of the plugin's stop() rather than logged and swallowed.
void    cb_ws_init(void);
void    cb_ws_start(void);
bool    cb_ws_stop(void);
void    cb_ws_deinit(void);

// Send a UTF-8 text frame on the WebSocket. Thread-safe. Returns FAIL
// when the session is not in the OPEN state or curl_ws_send errors; the
// caller is responsible for retrying once the session is back up (CB5
// re-sends live subscriptions on reconnect).
bool    cb_ws_send_json(const char *buf, size_t len);

// Human-readable name for a session state (logging / admin commands).
const char *cb_ws_state_name(cb_ws_state_t s);

// Reader-thread hook for a fully-reassembled text frame. CB4 provides a
// logging stub; CB5 replaces the body with the channel multiplexer. The
// pointer is only valid for the duration of the call.
void    cb_ws_dispatch_frame(const char *buf, size_t len);

// coinbase_ws_channels.c

// Lifecycle. cb_ws_channels_init sets up the subscription registry +
// slot table; cb_ws_channels_deinit tears them down, releasing every
// live subscription. Both are idempotent and safe to call before the
// WS reader has spawned.
void    cb_ws_channels_init(void);
void    cb_ws_channels_deinit(void);

// Called from coinbase_ws.c the moment the WS session transitions to
// OPEN. Rebuilds the upstream subscription set from the live slot
// table and emits a single batched subscribe frame so all local subs
// are live again after a reconnect. Resets every slot's sent_upstream
// flag before rendering, so a session that had previously received
// subscribe acks gets a fresh resubscribe after a flap. The session
// lock must NOT be held by the caller — this function calls
// cb_ws_send_json internally.
void    cb_ws_channels_on_open(void);

// Reader-thread hook for the reassembled text frame. Parses the JSON,
// applies sequence-gap detection, and fans out typed events to every
// matching local subscriber. Takes ownership of nothing; the buffer
// pointer is only valid for the duration of the call.
void    cb_ws_channels_dispatch(const char *buf, size_t len);

// Subscribe-ack watchdog probe for the transport's reader loop. True
// when the most recent subscribe batch has waited longer than
// CB_WS_SUB_ACK_TIMEOUT_MS with no "subscriptions" ack from the
// gateway — a state where the socket stays ping-pong-alive while every
// subscribe is silently ignored (INCIDENTS.md 2026-07-23), which the
// idle watchdog can never see. Thread-safe; the caller owns the
// response (schedule a reconnect) — this only reports.
bool    cb_ws_channels_sub_ack_overdue(void);

#endif // CB_INTERNAL

#endif // BM_COINBASE_H
