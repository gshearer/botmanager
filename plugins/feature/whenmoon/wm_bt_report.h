// wm_bt_report.h — WM-BT-6 on-disk artifact emitter.
//
// Each `/whenmoon backtest run` invocation produces a sweep directory
// under the report-path root with three artifacts:
//   manifest.json     — high-level metadata for the entire sweep
//   iterations.jsonl  — one JSONL line per iteration (results + metrics)
//   top-N.txt         — human-friendly top-N renderer (WM-BT-7 fills it)
//
// Internal to the whenmoon plugin. WHENMOON_INTERNAL gated.

#ifndef BM_WHENMOON_WM_BT_REPORT_H
#define BM_WHENMOON_WM_BT_REPORT_H

#ifdef WHENMOON_INTERNAL

#include "backtest.h"
#include "sweep.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Resolve the report-root directory. Reads
// `plugin.whenmoon.backtest.report_path`; empty (the registered
// default) → `$HOME/.local/share/botmanager/backtests/`. Ensures the
// directory tree exists, creating parents with mode 0755 and the
// leaf with mode 0700. Writes a NUL-terminated absolute path into
// `out` with no trailing slash.
//
// Returns SUCCESS on a usable directory; FAIL with `err` populated
// on `$HOME` missing, alloc failure, or mkdir/stat failure.
bool wm_bt_report_path_resolve(char *out, size_t cap,
    char *err, size_t err_cap);

// Render a sweep id of the form
// `YYYYMMDD-HHMMSS-<strategy>-<short_market>` from the current UTC
// wall-clock + the strategy name + the snapshot's
// `source_market_id`. `short_market` strips the leading `<exch>-`
// segment from the canonical id (e.g. `coinbase-btc-usd` → `btc-usd`);
// when the id has no hyphen the full id is used. Truncates safely
// to `cap`.
void wm_bt_sweep_id_generate(const char *strategy,
    const char *source_market_id, char *out, size_t cap);

// Create `<report_path>/<sweep_id>/` (mode 0700) and its `charts/`
// subdir (also 0700). Writes the sweep-dir path back into `out_path`.
// Idempotent on EEXIST — the timestamped id makes collisions
// effectively impossible, but defensive against a clock-skewed
// re-run.
bool wm_bt_sweep_dir_create(const char *report_path,
    const char *sweep_id, char *out_path, size_t cap,
    char *err, size_t err_cap);

// JSONL writer. Each call to wm_bt_iter_append serializes one row
// under the writer's mutex; WM-BT-6 drives this from the main-thread
// post-pass loop (workers must not call it concurrently — see the
// "results in memory, flush in a single pass" landmine).
typedef struct
{
  FILE             *fp;
  pthread_mutex_t   lock;
  uint32_t          n_written;
} wm_bt_iterations_writer_t;

// Open `<sweep_dir>/iterations.jsonl` for writing ("w", truncating
// any pre-existing file). Initialises the writer's mutex.
bool wm_bt_iter_open(wm_bt_iterations_writer_t *w,
    const char *sweep_dir, char *err, size_t err_cap);

// Render one iteration's JSONL row and emit it. On worker FAIL
// (`result->ok == false`), emits `{"iter":N,"params":{...},"ok":false,
// "err":"..."}` with no metrics block. Otherwise emits the full
// metrics object + an `"oos":{"have":...}` subobject populated from
// the result's OOS fields when set by the post-pass validator.
bool wm_bt_iter_append(wm_bt_iterations_writer_t *w,
    const wm_bt_sweep_plan_t *plan, uint32_t iter,
    const wm_bt_sweep_result_t *result);

void wm_bt_iter_close(wm_bt_iterations_writer_t *w);

// Atomic write of `<sweep_dir>/manifest.json` via tmp + rename.
// Captures sweep_id, source wm_file, snapshot range, strategy, sweep
// axes, fixed params, run mode, ranking metric, top-N cap, thread
// count, iteration totals, ok/fail counts, total wallclock, and the
// indicator schema version baked into the .wm file.
bool wm_bt_manifest_write(const char *sweep_dir,
    const char *sweep_id, const char *wm_path,
    const wm_backtest_snapshot_t *snap,
    const char *strategy,
    const wm_bt_sweep_plan_t *plan,
    const wm_bt_sweep_mode_t *mode,
    const wm_backtest_params_t *fixed_params,
    uint64_t wallclock_ms, uint32_t ok_count, uint32_t fail_count,
    char *err, size_t err_cap);

// Write the BT-7 placeholder `<sweep_dir>/top-N.txt`. WM-BT-7
// replaces this with the colored top-N renderer.
bool wm_bt_top_n_placeholder_write(const char *sweep_dir,
    const wm_bt_sweep_plan_t *plan);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_WM_BT_REPORT_H
