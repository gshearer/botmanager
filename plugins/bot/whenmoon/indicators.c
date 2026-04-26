// botmanager — MIT
// indicators.c — TA-Lib-backed indicator computation for closed bars.
//
// WM-LT-1 stub: links so callers (added in WM-LT-2) can name the
// symbol; leaves the indicator block at its caller-initialised value
// (NaN by convention, which downstream strategies detect via isnan()).
// The TA-Lib-backed implementation lands in WM-LT-2.
//
// TA-Lib is the system-packaged BSD-3 indicator library (Mario
// Fortier, 1999-2024). Public header is `ta-lib/ta_libc.h`; the
// `TA_Initialize() / TA_Shutdown()` lifecycle will move into the
// whenmoon plugin's plugin_init / plugin_destroy in WM-LT-2.

#define WHENMOON_INTERNAL
#include "indicators.h"

#include <ta-lib/ta_libc.h>

void
wm_indicators_compute_bar(const wm_candle_full_t *ring,
    uint32_t n, uint32_t bar_idx)
{
  (void)ring;
  (void)n;
  (void)bar_idx;
}
