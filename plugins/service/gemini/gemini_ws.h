// gemini_ws.h — Gemini WebSocket transport.
//
// GEM-3 lands the real transport. Until then the lifecycle hooks are
// empty stubs so the linker can see them and gem_init / gem_start /
// gem_stop / gem_deinit have a stable surface to call.
//
// GEM-3 design notes (placed here so the GEM-3 implementer doesn't
// have to dig the chunk plan back out):
//
//   * Two long-lived libcurl WebSocket sessions:
//       Market Data v2 → wss://api.gemini.com/v2/marketdata  (public)
//       Order Events   → wss://api.gemini.com/v1/order/events (private)
//   * Order Events authenticates via the same X-GEMINI-APIKEY /
//     X-GEMINI-PAYLOAD / X-GEMINI-SIGNATURE headers on the HTTP
//     handshake — there is NO token-fetch endpoint like Kraken's
//     GetWebSocketsToken.
//   * Heartbeat flag is appended to the Order Events URL via
//     `?heartbeat=true` at connect time; preserve any pre-existing
//     query string from the operator-set KV.

#ifndef BM_GEMINI_WS_H
#define BM_GEMINI_WS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Lifecycle. Paired with gem_init / gem_start / gem_stop / gem_deinit
// in gemini.c.
void    gem_ws_init  (void);
void    gem_ws_start (void);
void    gem_ws_stop  (void);
void    gem_ws_deinit(void);

#endif // BM_GEMINI_WS_H
