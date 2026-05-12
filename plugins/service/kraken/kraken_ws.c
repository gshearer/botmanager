// botmanager — MIT
// Kraken WebSocket v2 transport. KR-3 ships only the lifecycle
// hooks as no-ops. KR-5 fills in the reader thread, the JSON-RPC
// framing, the public/private session split, and the token-rotation
// machinery against `POST /0/private/GetWebSocketsToken`.
#define KR_INTERNAL
#include "kraken.h"

void
kr_ws_init(void)
{
}

void
kr_ws_start(void)
{
}

void
kr_ws_stop(void)
{
}

void
kr_ws_deinit(void)
{
}
