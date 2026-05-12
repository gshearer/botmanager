// kraken_ws_channels.h — Kraken WebSocket channel multiplexer.
//
// KR-3 ships only the lifecycle hooks. KR-5 fills in the per-channel
// fanout, the local subscription registry, and the resubscribe-on-
// reconnect contract.

#ifndef BM_KRAKEN_WS_CHANNELS_H
#define BM_KRAKEN_WS_CHANNELS_H

void    kr_ws_channels_init(void);
void    kr_ws_channels_deinit(void);

#endif // BM_KRAKEN_WS_CHANNELS_H
