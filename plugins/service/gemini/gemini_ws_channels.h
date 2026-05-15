// gemini_ws_channels.h — Gemini WebSocket channel multiplexer.
//
// GEM-3 lands the real multiplexer (per-(channel, symbol) slot table,
// subscribe-id correlator, per-channel parser, resubscribe-on-flap).
// Until then the lifecycle hooks are no-op stubs.

#ifndef BM_GEMINI_WS_CHANNELS_H
#define BM_GEMINI_WS_CHANNELS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Lifecycle. Idempotent and safe to call before the WS reader has
// spawned.
void    gem_ws_channels_init  (void);
void    gem_ws_channels_deinit(void);

#endif // BM_GEMINI_WS_CHANNELS_H
