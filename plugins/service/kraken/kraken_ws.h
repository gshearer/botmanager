// kraken_ws.h — Kraken WebSocket v2 transport.
//
// TWO long-lived libcurl WebSocket sessions, because Kraken's v2 gateway
// splits its channels across two endpoints and each refuses the other's
// (measured 2026-08-17, OBS-18):
//
//   KR_WS_PUBLIC  `wss://ws.kraken.com/v2`       ticker, trade, ohlc
//   KR_WS_PRIVATE `wss://ws-auth.kraken.com/v2`  executions, balances
//
// A private subscribe on the public URL comes back `Private data and
// trading are unavailable on this endpoint. Try ws-auth.kraken.com`, and
// a public subscribe on the auth URL comes back with the mirror image of
// that sentence. There is no single endpoint that serves both, whatever
// a token is attached to.
//
// The private session is opened LAZILY — only while credentials are
// configured and the slot table holds a private subscription — since an
// authenticated session with nothing on it earns nothing. It is not a
// liveness hazard either way: an unsubscribed session receives no
// heartbeat, but Kraken pongs the keepalive ping, which is what arrests
// the idle watchdog.
//
// A dedicated reader task per session drives curl_ws_recv; reconnect uses exponential
// backoff with the per-tick KV `plugin.kraken.ws_reconnect_ms` as the base
// (capped at KR_WS_MAX_BACKOFF_MS). Reassembly buffers grow on demand
// inside KR_WS_ASSEMBLY_CAP.
//
// The token cache is shared with kraken_ws_channels.c — every private
// subscribe on the private session inlines `token` from the cached value
// (`Token(s) not found` is the gateway's answer without one). Acquisition runs
// asynchronously through `POST /0/private/GetWebSocketsToken`; the cache
// is monotonic and refreshed pre-emptively at KR_WS_TOKEN_REFRESH_MS.

#ifndef BM_KRAKEN_WS_H
#define BM_KRAKEN_WS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ------------------------------------------------------------------ //
// Sizing knobs.                                                       //
// ------------------------------------------------------------------ //

// Single-read recv buffer. Kraken v2 frames are typically 1-4 KiB; a 64 KiB
// buffer absorbs the largest snapshot in one call.
#define KR_WS_RECV_BUF_SZ        65536
// Reassembly cap. A logical text frame larger than this is treated as a
// protocol violation; the session is dropped + reconnected.
#define KR_WS_ASSEMBLY_CAP       (1u * 1024u * 1024u)
#define KR_WS_MAX_BACKOFF_MS     60000
// Idle window. Past this without any frame the watchdog drops + reconnects.
// Kraken sends a heartbeat every ~1 s on a subscribed session, so a 45 s
// silence is unmistakably a dead connection.
#define KR_WS_IDLE_TIMEOUT_MS    45000
#define KR_WS_PING_INTERVAL_MS   20000
#define KR_WS_POLL_MS            250
#define KR_WS_STOP_WAIT_MS       5000
#define KR_WS_MAX_CONSEC_FAIL    10

// Token. Kraken's docs document a 15-min lifetime for the WS token; we
// refresh proactively at 14 min to absorb clock skew + occasional refresh
// failures.
#define KR_WS_TOKEN_SZ           128
#define KR_WS_TOKEN_REFRESH_MS   (14 * 60 * 1000)

// Outbound frame buffer for subscribe / unsubscribe payloads. Kraken v2
// frames sit well under 1 KiB even for the largest symbol arrays.
#define KR_WS_TX_BUF_SZ          8192

typedef enum
{
  KR_WS_DISCONNECTED,
  KR_WS_CONNECTING,
  KR_WS_OPEN,
  KR_WS_RECONNECTING
} kr_ws_state_t;

// Which of the two gateways a frame belongs on. Derived from the channel
// alone — kraken_ws_channels.c owns that mapping — so no caller ever
// picks an endpoint by hand.
typedef enum
{
  KR_WS_PUBLIC  = 0,
  KR_WS_PRIVATE = 1
} kr_ws_session_id_t;

#define KR_WS_N_SESSIONS 2

// ------------------------------------------------------------------ //
// Lifecycle. Paired with kr_init / kr_start / kr_stop / kr_deinit in   //
// kraken.c.                                                           //
// ------------------------------------------------------------------ //

void    kr_ws_init  (void);
void    kr_ws_start (void);

// kr_ws_stop signals BOTH readers and *joins* them, up to
// KR_WS_STOP_WAIT_MS each. FAIL means a thread is still inside this
// plugin's mapping, which makes the unload unsafe — it is propagated out
// of the plugin's stop() rather than logged and swallowed. Every session
// is signalled and joined even after one has failed: a session left
// running because a sibling timed out is a second live thread in a
// mapping already being torn down.
bool    kr_ws_stop  (void);
void    kr_ws_deinit(void);

// Human-readable name for a session state (logging).
const char *kr_ws_state_name(kr_ws_state_t s);

// Send a UTF-8 text frame on one session. Thread-safe. Returns FAIL when
// that session is not OPEN or curl_ws_send errors — which is the normal
// answer for a private frame rendered before the lazy session has opened;
// the channel multiplexer retries it on the next kr_ws_channels_on_open.
bool    kr_ws_send_text(kr_ws_session_id_t sid, const char *buf, size_t len);

// Reader-thread hook for a fully-reassembled text frame. Forwards to
// kr_ws_channels_dispatch. Pointer is only valid for the call duration.
void    kr_ws_dispatch_frame(const char *buf, size_t len);

// ------------------------------------------------------------------ //
// Token cache. The fetcher is async (curl roundtrip); subscribe paths //
// that need a token take a snapshot of the cached value.              //
// ------------------------------------------------------------------ //

// Async completion shape for kr_ws_token_acquire. Fires once with `ok=true`
// when a fresh token is cached, or `ok=false` when the fetch failed (no
// creds, transport error, Kraken returned an error envelope). The user
// pointer is echoed back verbatim.
typedef void (*kr_ws_token_done_cb_t)(bool ok, void *user);

// Issue a `POST /0/private/GetWebSocketsToken` request. Coalesces with
// any in-flight fetch — every concurrent caller's callback fires once
// the single roundtrip lands. Safe to call from any thread; the callback
// fires on the curl-multi worker.
bool    kr_ws_token_acquire(kr_ws_token_done_cb_t cb, void *user);

// Snapshot the cached token into `out`. Returns SUCCESS when a non-stale
// token is available (ie. fetched within KR_WS_TOKEN_REFRESH_MS) and
// `out` is large enough; FAIL otherwise. Callers consume the snapshot
// inline in a subscribe payload — the cache is monotonic so the value
// remains valid past the callback boundary.
bool    kr_ws_token_snapshot(char *out, size_t cap);

// True when the cached token is older than KR_WS_TOKEN_REFRESH_MS — the
// channel multiplexer probes this on its tick to decide whether to kick
// off a refresh.
bool    kr_ws_token_needs_refresh(void);

#endif // BM_KRAKEN_WS_H
