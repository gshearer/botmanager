// botmanager — MIT
// Kraken WebSocket channel multiplexer. KR-3 ships only the lifecycle
// hooks as no-ops. KR-5 fills in the per-channel fanout, the local
// subscription registry, and the resubscribe-on-reconnect contract.
#define KR_INTERNAL
#include "kraken.h"

void
kr_ws_channels_init(void)
{
}

void
kr_ws_channels_deinit(void)
{
}
