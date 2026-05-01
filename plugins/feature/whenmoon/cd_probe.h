// cd_probe.h — runtime probe of Coinbase candle history depth.
// WM-CD-1 Phase 1.
//
// Per granularity in {60, 300, 900, 3600, 21600, 86400}, fires a
// fire-and-forget bisection probe against the candles endpoint to
// discover the maximum reachable lookback (in days). Each probe's
// completion writes its result to KV under
// `plugin.whenmoon.candles.<gran>.max_lookback_days` and emits a
// CLAM_INFO line. Internal; WHENMOON_INTERNAL-gated.

#ifndef BM_WHENMOON_CD_PROBE_H
#define BM_WHENMOON_CD_PROBE_H

#ifdef WHENMOON_INTERNAL

#include <stdbool.h>

// Fire one probe per granularity against `symbol` (Coinbase wire form,
// e.g. "BTC-USD"). Returns SUCCESS if all six probes were submitted;
// FAIL on bad symbol or if every submit attempt failed. Probes complete
// asynchronously on the curl-multi worker thread; this call returns
// immediately.
bool wm_cd_probe_run(const char *symbol);

#endif // WHENMOON_INTERNAL
#endif // BM_WHENMOON_CD_PROBE_H
