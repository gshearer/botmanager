// botmanager — MIT
// aggregator.c — placeholder for the multi-grain candle aggregator.
//
// WM-LT-1 ships only the grain-seconds table; the bar-close aggregator
// that populates `wm_candle_full_t` rings lands in WM-LT-2.

#define WHENMOON_INTERNAL
#include "aggregator.h"

const int32_t wm_gran_seconds[WM_GRAN_MAX] =
{
  60,    // 1m
  300,   // 5m
  900,   // 15m
  3600,  // 1h
  21600, // 6h
  86400, // 1d
};
