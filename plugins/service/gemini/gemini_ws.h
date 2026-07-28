// gemini_ws.h — Gemini WebSocket transport.
//
// Two long-lived libcurl WebSocket sessions multiplex Gemini's WS
// surface:
//
//   * Market Data v2 (public) — wss://api.gemini.com/v2/marketdata
//   * Order Events  (private) — wss://api.gemini.com/v1/order/events
//
// Each session owns its own persist-task reader, mutex, reconnect state,
// reassembly buffer, and idle/ping watchdog. They do NOT share state
// with each other — only the channel multiplexer's slot/subscriber
// tables are shared, and those are protected by its own mutex.
//
// Order Events authenticates via the same X-GEMINI-APIKEY /
// X-GEMINI-PAYLOAD / X-GEMINI-SIGNATURE headers on the HTTP handshake —
// there is NO token-fetch endpoint like Kraken's GetWebSocketsToken.
// Re-signs every reconnect against `{"request":"/v1/order/events",
// "nonce":<n>}` with a fresh nonce minted via gem_next_nonce.
//
// Heartbeat flag is appended to the Order Events URL via
// `?heartbeat=true` at connect time only if the operator-set KV does
// not already carry a query string (preserves customisation).

#ifndef BM_GEMINI_WS_H
#define BM_GEMINI_WS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ------------------------------------------------------------------ //
// Sizing knobs.                                                       //
// (GEM_WS_IDLE_TIMEOUT_MS / GEM_WS_PING_INTERVAL_MS /                 //
//  GEM_WS_MAX_BACKOFF_MS / GEM_WS_STOP_WAIT_MS /                      //
//  GEM_WS_MAX_CONSEC_FAIL / GEM_SUBS_MAX / GEM_PRODS_PER_SUB_MAX /    //
//  GEM_SLOTS_MAX / GEM_REQ_ID_RING live in gemini.h.)                 //
// ------------------------------------------------------------------ //

// Single-read recv buffer. Gemini frames are typically 1-4 KiB; a
// 64 KiB buffer absorbs the largest snapshot in one call.
#define GEM_WS_RECV_BUF_SZ        65536
// Reassembly cap. A logical text frame larger than this is treated as
// a protocol violation; the session is dropped + reconnected.
#define GEM_WS_ASSEMBLY_CAP       (1u * 1024u * 1024u)
// Reader-loop poll cadence (matches Kraken).
#define GEM_WS_POLL_MS            250
// Outbound frame buffer for subscribe / unsubscribe payloads. Gemini's
// l2/candles/trades subscribe frames sit under 1 KiB even for the
// largest symbol arrays.
#define GEM_WS_TX_BUF_SZ          8192

typedef enum
{
  GEM_WS_DISCONNECTED,
  GEM_WS_CONNECTING,
  GEM_WS_OPEN,
  GEM_WS_RECONNECTING
} gem_ws_state_t;

// Session identifier. Two transports live behind the same lifecycle:
//   GEM_WS_MD — Market Data v2 (public, no auth).
//   GEM_WS_OE — Order Events   (private, HMAC-headered handshake).
typedef enum
{
  GEM_WS_MD = 0,
  GEM_WS_OE = 1
} gem_ws_session_id_t;

// ------------------------------------------------------------------ //
// Lifecycle. Paired with gem_init / gem_start / gem_stop /            //
// gem_deinit in gemini.c.                                             //
// ------------------------------------------------------------------ //

void    gem_ws_init  (void);
void    gem_ws_start (void);

// gem_ws_stop signals both readers and *joins* them, up to
// GEM_WS_STOP_WAIT_MS each. FAIL means a thread is still inside this
// plugin's mapping, which makes the unload unsafe — it is propagated
// out of the plugin's stop() rather than logged and swallowed.
bool    gem_ws_stop  (void);
void    gem_ws_deinit(void);

// Human-readable name for a session state (logging).
const char *gem_ws_state_name(gem_ws_state_t s);

// Send a UTF-8 text frame on the named session. Thread-safe. Returns
// FAIL when the session is not OPEN or curl_ws_send errors. The
// channel multiplexer retries on the next reconnect via
// gem_ws_channels_on_open.
//
// The multiplexer only sends on GEM_WS_MD (OE is account-implicit with
// no subscribe frame); the symmetric API is kept for future use.
bool    gem_ws_send_text(gem_ws_session_id_t sid,
            const char *buf, size_t len);

// Reader-thread hook for a fully-reassembled text frame. Forwards to
// gem_ws_channels_dispatch_md or _oe based on `sid`. Pointer is only
// valid for the call duration.
void    gem_ws_dispatch_frame(gem_ws_session_id_t sid,
            const char *buf, size_t len);

#endif // BM_GEMINI_WS_H
