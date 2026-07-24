// indicators.h — Tulip-backed indicator computation for closed bars.
//
// Internal to the whenmoon plugin. Consumers outside this plugin must
// NOT include this header. Gate: WHENMOON_INTERNAL.
//
// WM-LT-1 ships a stub; the full implementation that populates every
// `WM_IND_*` slot via Tulip lands in WM-LT-2.

#ifndef BM_WHENMOON_INDICATORS_H
#define BM_WHENMOON_INDICATORS_H

#ifdef WHENMOON_INTERNAL

#include "aggregator.h"

// Compute every indicator slot for a single closed bar at `bar_idx`
// in the multi-grain ring `ring`. `n` is the count of populated bars,
// newest at `n - 1`. Slots whose indicator window exceeds the
// available history are left at NaN; strategies must check via
// `isnan()` before consuming a slot.
//
// Caller holds `whenmoon_market_t.lock`. The Tulip implementation is
// thread-safe per call; the lock protects the ring, not the math.
void wm_indicators_compute_bar(const wm_candle_full_t *ring,
    uint32_t n, uint32_t bar_idx);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_INDICATORS_H
