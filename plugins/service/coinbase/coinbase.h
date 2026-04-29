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
// Base64-encoded HMAC-SHA256 is always 44 chars + NUL. 64 leaves slack
// for any defensive code that rounds up.
#define CB_SIG_SZ        64
// Upper bound on the signed prehash (ts + METHOD + path + body). Order
// bodies sit well under 1 KiB in practice; 4 KiB gives headroom for
// future endpoints (e.g. bulk cancel) without risking silent truncation.
#define CB_PRESIGN_SZ    4096
// REST / WebSocket base URL buffer. Matches the convention used by
// the coinmarketcap and openweather plugins.
#define CB_URL_SZ        512
// ISO-style seconds-since-epoch string: "1745432821" ≈ 10 chars; pick
// 32 to absorb future formats (e.g. decimal fractional seconds).
#define CB_TS_SZ         32
// Error message buffer for transient classifier output. Matches the
// CMC convention.
#define CB_ERR_SZ        128
// Product cache cap. Coinbase currently exposes ~700 products; 500 keeps
// memory footprint bounded. Caller is expected to filter before ingest.
#define CB_MAX_PRODUCTS  500

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
// here so CB2's union shape is stable when CB3 wires them in.
typedef enum
{
  CB_REQ_PRODUCTS,
  CB_REQ_CANDLES,
  CB_REQ_TRADES,          // WM-S3
  CB_REQ_TICKER,
  CB_REQ_PLACE_ORDER,
  CB_REQ_CANCEL_ORDER,
  CB_REQ_GET_ORDER,
  CB_REQ_GET_ACCOUNTS,
} cb_req_type_t;

// REST request context. Freelist-managed; exactly one callback member
// is valid per `type`. CB3 will populate the `body`/`body_len` fields
// when it introduces signed POST paths; CB2 leaves them NULL/0.
typedef struct cb_request
{
  cb_req_type_t  type;

  // Candle / ticker / trade selectors.
  char           product_id[COINBASE_PRODUCT_ID_SZ];
  int32_t        granularity;
  int64_t        start_ts;
  int64_t        end_ts;
  int64_t        after;      // WM-S3: trade cursor (0 = newest page)

  // CB3: signed POST/DELETE body. JSON already rendered by the caller.
  char          *body;
  size_t         body_len;

  // Order selectors (CB3).
  char           order_id[COINBASE_ORDER_ID_SZ];
  char           status[COINBASE_STATUS_SZ];

  // Typed completion callback. Exactly one member is valid per `type`.
  union
  {
    coinbase_done_products_cb_t  products;
    coinbase_done_candles_cb_t   candles;
    coinbase_done_trades_cb_t    trades;      // WM-S3
    coinbase_done_ticker_cb_t    ticker;
    coinbase_done_order_cb_t     order;
    coinbase_done_orders_cb_t    orders;
    coinbase_done_accounts_cb_t  accounts;
  } cb;
  void          *user;

  struct cb_request *next;   // freelist linkage
} cb_request_t;

// coinbase_sign.c

bool    cb_sandbox_enabled(void);
bool    cb_rest_base_url(char *out, size_t cap);
bool    cb_ws_base_url(char *out, size_t cap);
bool    cb_apikey_configured(void);
size_t  cb_timestamp_str(char *out, size_t cap);
bool    cb_sign_request(const char *method, const char *path,
            const char *body, size_t body_len, const char *ts,
            char *sig_out, size_t sig_cap);

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
// on the curl worker thread. Returns FAIL if credentials are missing,
// the base URL is unset, or the request could not be queued; caller
// is responsible for emitting the typed failure callback on FAIL.
bool    cb_submit_private(cb_request_t *req, curl_method_t method,
            const char *path, const char *body, size_t body_len,
            curl_done_cb_t done_cb);

// Build and submit a public (unauthenticated) REST GET. Mirror of
// cb_submit_private for traffic that flows through the exchange
// abstraction. `user_data` of the resulting curl request is the
// caller-supplied pointer, exactly as for cb_submit_private. EX-1
// promoted this from a file-scope static — no thin wrappers per
// AGENTS.md `feedback_no_thin_wrappers.md`.
bool    cb_submit_public(void *user_data, const char *path,
            curl_done_cb_t done_cb);

// coinbase_exchange.c — vtable registration with the feature_exchange
// abstraction. Called from cb_init.
bool    cb_exchange_register_vtable(void);

// coinbase_ws.c

// Lifecycle. cb_ws_init prepares the session struct and subscribes to
// the config knobs that force a reconnect; it does NOT spawn the reader
// thread. cb_ws_start spawns the reader (latching the current value of
// plugin.coinbase.ws_enabled), cb_ws_stop signals it to exit and blocks
// up to CB_WS_STOP_WAIT_MS for a clean shutdown, and cb_ws_deinit frees
// reassembly buffers + destroys the lock.
void    cb_ws_init(void);
void    cb_ws_start(void);
void    cb_ws_stop(void);
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

#endif // CB_INTERNAL

#endif // BM_COINBASE_H
