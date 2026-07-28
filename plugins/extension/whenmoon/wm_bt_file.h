// wm_bt_file.h — `.wm` binary snapshot file: format + serialize + mmap.
//
// A `.wm` file is a serialized wm_backtest_snapshot_t. Compiled once
// via `/whenmoon backtest compile` (WM-BT-3), opened read-only and
// mmap'd into every sweep worker. The format is host-endian + page-
// aligned + indicator-schema-versioned; any mismatch on load is a hard
// reject. There is no backwards-compatibility shim — operators
// recompile on schema bumps (per `feedback_no_deprecated_code`).
//
// On-disk layout:
//
//   [ wm_bt_file_header_t                            ] @ offset 0
//   [ ... zero padding to WM_BT_FILE_PAGE_ALIGN      ]
//   [ wm_candle_full_t[blocks[0].n_bars]             ] @ blocks[0].off_bytes
//   [ ... zero padding to next page boundary         ]
//   [ wm_candle_full_t[blocks[1].n_bars]             ] @ blocks[1].off_bytes
//   ...
//
// Empty grains (n_bars == 0) consume no body bytes; their off_bytes
// is 0 and the loader leaves grain_arr[g] = NULL / grain_n[g] = 0.
//
// Internal to the whenmoon plugin. WHENMOON_INTERNAL gated.

#ifndef BM_WHENMOON_WM_BT_FILE_H
#define BM_WHENMOON_WM_BT_FILE_H

#ifdef WHENMOON_INTERNAL

#include "backtest.h"
#include "whenmoon_strategy.h"
#include "market.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Magic = "WBNM" in little-endian byte order ('W'=0x57, 'B'=0x42,
// 'N'=0x4E, 'M'=0x4D → 0x4D4E4257). Files are host-endian; the magic
// doubles as an endianness sniffer for diagnostic purposes only.
//
// v2: cascade boundary-roll fix (WM-AGG-1) — pre-fix corpora carry
// future-skewed higher grains.
#define WM_BT_FILE_MAGIC        0x4D4E4257u
#define WM_BT_FILE_VERSION      2u
#define WM_BT_FILE_PAGE_ALIGN   4096u

// One per-grain block descriptor. `bar_size` is sizeof(wm_candle_full_t)
// at compile time — duplicated in the header so a layout drift is
// caught at load time without trusting the indicator schema version
// alone.
typedef struct wm_bt_file_grain_block
{
  uint64_t  off_bytes;
  uint32_t  n_bars;
  uint32_t  bar_size;
} wm_bt_file_grain_block_t;

// Fixed-size header. The trailing _pad[] member rounds the on-disk
// header out to WM_BT_FILE_PAGE_ALIGN so the first grain block lands
// on a page boundary (mmap-friendly). The struct itself is < 4096 B;
// the writer pads the file on disk regardless of struct size.
typedef struct wm_bt_file_header
{
  uint32_t  magic;                                  // = WM_BT_FILE_MAGIC
  uint32_t  file_version;                           // = WM_BT_FILE_VERSION
  uint32_t  indicator_schema_version;               // = WM_INDICATOR_SCHEMA_VERSION at compile
  uint32_t  bar_size;                               // = sizeof(wm_candle_full_t)
  uint32_t  n_grains;                               // = WM_GRAN_MAX
  uint32_t  _pad0;
  char      source_market_id[WM_MARKET_ID_STR_SZ];  // canonical "<exch>-<base>-<quote>"
  char      range_start[40];                        // postgres canonical
  char      range_end[40];
  int64_t   range_start_ms;
  int64_t   range_end_ms;
  uint32_t  bars_loaded_1m;
  uint32_t  _pad1;
  wm_bt_file_grain_block_t  blocks[WM_GRAN_MAX];
} wm_bt_file_header_t;

// Serialize an in-memory snapshot to disk. Writes to <path>.tmp,
// fsyncs, then atomically rename(2)s to <path>. The caller's
// snapshot must be heap-built (is_mapped == false) — re-serializing
// an mmap'd snapshot is not supported.
//
// Returns SUCCESS on a complete write; FAIL with err populated on any
// open/write/fsync/rename failure (the partial .tmp file is removed).
bool wm_bt_file_write(const char *path,
    const wm_backtest_snapshot_t *snap,
    char *err, size_t err_cap);

// Open + validate + mmap a `.wm` file. Returns a heap-allocated
// wm_backtest_snapshot_t whose `mkt.grain_arr[g]` pointers index into
// the mapped region (borrowed, read-only). `is_mapped = true`,
// `map_base/_size/_fd` set; teardown via wm_bt_file_close (or just
// wm_backtest_snapshot_free which dispatches).
//
// `mkt.aggregator` stays NULL — mmap'd snapshots do not re-build the
// aggregator; the grain rings ARE the aggregator's output. Iteration
// reads them directly.
//
// Returns NULL on any failure (file, validation, alloc); err (when
// non-NULL) describes which check failed.
wm_backtest_snapshot_t *wm_bt_file_open(const char *path,
    char *err, size_t err_cap);

// Symmetric to wm_bt_file_open. munmap's, closes the fd, and frees
// the snapshot struct. Safe to call on a NULL snap or a snap whose
// is_mapped == false (no-op — caller used the wrong teardown
// function, but we don't compound the error).
void wm_bt_file_close(wm_backtest_snapshot_t *snap);

// Header-only inspector. Reads just the header, renders a multi-line
// human summary into `summary` (NUL-terminated). Used by
// `/whenmoon backtest inspect` (WM-BT-3) to inspect a compiled file
// without running it.
//
// Returns SUCCESS on a valid (or stale-schema) header; FAIL only on
// I/O / magic / file_version / bar_size mismatches. A stale indicator
// schema version is NOT a FAIL — inspect emits a NOTE line and
// returns SUCCESS so the user can still see what the file holds.
bool wm_bt_file_inspect(const char *path,
    char *summary, size_t cap,
    char *err, size_t err_cap);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_WM_BT_FILE_H
