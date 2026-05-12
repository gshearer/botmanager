// botmanager — MIT
// Kraken Spot REST mechanism + freelist. KR-3 ships only the
// lifecycle hooks so the descriptor in kraken.c can call
// kr_rest_init / kr_rest_deinit symmetrically. The signed-request
// submitter, response classifier, and typed endpoint wrappers
// (candles, balances, orders, fills) land in KR-4.
#define KR_INTERNAL
#include "kraken.h"

void
kr_rest_init(void)
{
}

void
kr_rest_deinit(void)
{
}
