// kraken_ws.h — Kraken WebSocket v2 transport.
//
// KR-3 ships only the lifecycle hooks. KR-5 fills in the reader, the
// JSON-RPC framing, the public-vs-private session split, and the
// token rotation. The descriptor in kraken.c calls kr_ws_init /
// kr_ws_start / kr_ws_stop / kr_ws_deinit symmetrically with the
// other subsystems; KR-3 makes each a no-op.

#ifndef BM_KRAKEN_WS_H
#define BM_KRAKEN_WS_H

void    kr_ws_init(void);
void    kr_ws_start(void);
void    kr_ws_stop(void);
void    kr_ws_deinit(void);

#endif // BM_KRAKEN_WS_H
