// kraken_ws.h — Kraken WebSocket v2 transport.
//
// One long-lived libcurl WebSocket session against `wss://ws.kraken.com/v2`.
// Public channels (ticker, trade, ohlc) and private channels (executions,
// balances) all multiplex over the same session — Kraken's v2 docs accept
// both flavours on the public URL when the subscribe carries a token.
// `plugin.kraken.ws_url_private` is exposed as a fallback knob; it is not
// the default.
//
// A dedicated reader task drives curl_ws_recv; reconnect uses exponential
// backoff with the per-tick KV `plugin.kraken.ws_reconnect_ms` as the base
// (capped at KR_WS_MAX_BACKOFF_MS). Reassembly buffers grow on demand
// inside KR_WS_ASSEMBLY_CAP.
//
// The token cache is shared with kraken_ws_channels.c — every private
// subscribe inlines `token` from the cached value. Acquisition runs
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

// ------------------------------------------------------------------ //
// Lifecycle. Paired with kr_init / kr_start / kr_stop / kr_deinit in   //
// kraken.c.                                                           //
// ------------------------------------------------------------------ //

void    kr_ws_init  (void);
void    kr_ws_start (void);
void    kr_ws_stop  (void);
void    kr_ws_deinit(void);

// Human-readable name for a session state (logging).
const char *kr_ws_state_name(kr_ws_state_t s);

// Send a UTF-8 text frame on the active session. Thread-safe. Returns
// FAIL when the session is not OPEN or curl_ws_send errors. The channel
// multiplexer retries on the next reconnect via kr_ws_channels_on_open.
bool    kr_ws_send_text(const char *buf, size_t len);

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
