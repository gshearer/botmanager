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
#include <sys/types.h>

// Mode passed to mkdir() for every backtest output directory (report
// root, sweep dir, charts/, charts/iter-K/). 0777 so the process umask
// alone decides the final permissions — the same policy as the 0666
// the artifact files are written with via fopen. A 022 umask yields
// 0755 (web-servable); a restrictive umask keeps them private.
#define WM_BT_REPORT_DIR_MODE   ((mode_t)0777)

// Resolve the report-root directory. Reads
// `plugin.whenmoon.backtest.report_path`; empty (the registered
// default) → `$HOME/.local/share/botmanager/backtests/`. Ensures the
// directory tree exists, creating each path segment with
// WM_BT_REPORT_DIR_MODE (so the umask decides). Writes a
// NUL-terminated absolute path into `out` with no trailing slash.
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

// Create `<report_path>/<sweep_id>/` and its `charts/` subdir, both
// with WM_BT_REPORT_DIR_MODE (umask applies). Writes the sweep-dir
// path back into `out_path`.
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

// Render `<sweep_dir>/top-N.txt` — ANSI-stripped twin of the
// cmd_reply top-N output. Writes via tmp + rename so a partial
// emit is never visible. Uses `wm_bt_topk_compute` to share sorting
// with the cmd_reply path; columns + row format are byte-identical
// minus the CLR_BOLD/CLR_RESET wrappers around the header line.
bool wm_bt_render_topn_txt(const char *sweep_dir,
    const wm_bt_sweep_plan_t *plan,
    const wm_bt_sweep_mode_t *mode,
    const wm_bt_sweep_result_t *results, uint32_t n_results,
    uint32_t n_ok, char *err, size_t err_cap);

// Render `<sweep_dir>/report.md` — a self-contained sweep summary.
// Sections: header bullets (strategy/market/range/mode/threads/
// iterations/wallclock/rank-by), sweep-axes table, top-N markdown
// table (mode-aware), per-axis marginal-best table (one table per
// axis), and a failures list (or "(none)"). Atomic write via
// tmp + rename. `n_ok` + `n_fail` are precomputed by the caller;
// `wallclock_ms` is the total sweep duration.
bool wm_bt_render_report_md(const char *sweep_dir,
    const char *sweep_id, const char *wm_path,
    const wm_backtest_snapshot_t *snap, const char *strategy,
    const wm_bt_sweep_plan_t *plan, const wm_bt_sweep_mode_t *mode,
    const wm_bt_sweep_result_t *results, uint32_t n_results,
    uint32_t n_ok, uint32_t n_fail, uint64_t wallclock_ms,
    char *err, size_t err_cap);

// Render `<sweep_dir>/index.html` — a self-contained, browser-friendly
// landing page for the sweep. Summarises the run (market / range / mode
// / rank-by / fixed economics), lists every charted (top-K) config with
// its swept arguments + headline metrics, and for each gives a per-trade
// table (entry/exit time + price, qty, P/L $ and %, exit reason) where
// each row links to the matching Lightweight-Charts visualization under
// `charts/iter-K/`. The per-trade chart links are emitted for every
// grain the snapshot carries (`snap->mkt.grain_n[g] > 0`) — identical
// to the chart pass — so the generated hrefs point at files that exist.
// The top-K cap honours `plugin.whenmoon.backtest.charts_top_n` exactly
// as the chart pass does. Atomic write via tmp + rename. Intended to be
// called right after chart emission so all linked files are present.
bool wm_bt_render_index_html(const char *sweep_dir,
    const char *sweep_id, const char *wm_path,
    const wm_backtest_snapshot_t *snap, const char *strategy,
    const wm_bt_sweep_plan_t *plan, const wm_bt_sweep_mode_t *mode,
    const wm_backtest_params_t *fixed_params,
    const wm_bt_sweep_result_t *results, uint32_t n_results,
    uint32_t n_ok, uint32_t n_fail, uint64_t wallclock_ms,
    char *err, size_t err_cap);

// Heap-owned listing of sweep dir entries. names[i] is a NUL-terminated
// string heap-strdup'd from the on-disk dirent name. Free via
// wm_bt_dir_listing_free.
typedef struct
{
  char    **names;
  uint32_t  n;
} wm_bt_dir_listing_t;

// Read `path` (the report-root) and list every entry whose first 8
// chars match `[0-9]{8}` (the YYYYMMDD sweep-id prefix). The
// YYYYMMDD-HHMMSS- prefix is chrono-aligned with lexical order, so a
// descending lexical sort returns newest-first. Skips dot-prefixed
// entries and non-directory entries. Returns SUCCESS on a clean
// listing (possibly empty); FAIL with err populated otherwise.
bool wm_bt_dir_listdir(const char *path,
    wm_bt_dir_listing_t *out, char *err, size_t err_cap);

void wm_bt_dir_listing_free(wm_bt_dir_listing_t *l);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_WM_BT_REPORT_H
