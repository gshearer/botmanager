#ifndef BM_WHENMOON_WM_BT_CHART_H
#define BM_WHENMOON_WM_BT_CHART_H

#ifdef WHENMOON_INTERNAL

#include "market.h"               // wm_market_fill_t
#include "whenmoon_strategy.h"    // wm_candle_full_t, wm_gran_t

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Bars to render before the entry fill and after the exit fill. Sized
// so a single chart shows enough context to judge the strategy's setup
// + outcome without the trade dominating the frame.
#define WM_BT_CHART_PADDING_BARS  100u

// Render one trade as `<dir>/trade-<trade_idx>-<gran_name>.html`. Atomic
// via tmp + fsync + rename.
//
// `bars` / `n_bars` is the per-grain ring slice the caller has already
// computed (typically [entry.ts - PADDING, exit.ts + PADDING] clamped
// to ring bounds). `entry_fill` / `exit_fill` are the matched
// buy/sell pair; `exit_fill` may be NULL when the iteration's last
// position never closed (open at snapshot end) — in that case only
// the entry marker is drawn.
//
// SUCCESS on a complete HTML file; FAIL with `err` populated on alloc
// / fopen / write failure. Empty slices (`n_bars == 0`) and NULL
// `entry_fill` both FAIL — caller should guard.
bool wm_bt_chart_emit(const char *dir,
    uint32_t iter_idx, uint32_t trade_idx, wm_gran_t gran,
    const wm_candle_full_t *bars, uint32_t n_bars,
    const wm_market_fill_t *entry_fill,
    const wm_market_fill_t *exit_fill,
    char *err, size_t err_cap);

// Short canonical grain token ("1m", "5m", "15m", "1h", "4h", "1d").
// "?" on out-of-range. Stable for file names + chart titles.
const char *wm_bt_chart_gran_name(wm_gran_t g);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_WM_BT_CHART_H
