// backtest.h — snapshot + single-iteration replay (WM-LT-5).
//
// A backtest is one (market_id, strategy_name, range) replay through
// historical 1m candles. Bars are fed through a dedicated aggregator
// (which performs the multi-grain cascade + indicator pass) into a
// stub `whenmoon_market_t`; the strategy callback fires manually from
// the replay loop. The strategy's signal dispatch lands on a backtest-
// private trade book — a regular `wm_trade_book_t` registered under
// the synthetic id "bt:<run_id>" so it does not collide with live
// books.
//
// WM-LT-5 ships single-iteration synchronous-by-task runs launched
// from /whenmoon backtest run. WM-LT-6 will add a worker pool, param
// sweeps, and the dlclose/dlopen reload path; the snapshot type
// defined here is reused by those layers.
//
// Internal to the whenmoon plugin. WHENMOON_INTERNAL gated.

#ifndef BM_WHENMOON_BACKTEST_H
#define BM_WHENMOON_BACKTEST_H

#ifdef WHENMOON_INTERNAL

#include "market.h"
#include "order.h"
#include "pnl.h"
#include "strategy.h"

#include <stdbool.h>
#include <stdint.h>

// Synthetic-market-id prefix. The renderer fits "bt:<int64>" plus NUL
// inside WM_MARKET_ID_STR_SZ (= 64). Live-id collisions are impossible
// because '<exch>-<base>-<quote>' parsing rejects leading "bt:".
#define WM_BACKTEST_ID_PREFIX  "bt:"

// History headroom over the strategy's declared minimum. The snapshot
// allocates `min_history * 1d-equivalent + headroom` bars per grain
// so even a long range replays without ring shift-loss eating the
// most-recent bars; backtest is read-only against the snapshot.
#define WM_BACKTEST_HISTORY_HEADROOM_DAYS  30

// ----------------------------------------------------------------------- //
// Run mode + window types (WM-LT-7)                                       //
// ----------------------------------------------------------------------- //

typedef enum
{
  WM_BT_MODE_FULL          = 0,
  WM_BT_MODE_WALK_FORWARD  = 1,
  WM_BT_MODE_OOS           = 2,
} wm_bt_run_mode_t;

// One time slice in milliseconds. start_ts_ms is inclusive,
// end_ts_ms is exclusive — matches the candle ring's ts_close_ms
// semantics (a bar with ts_close_ms == window.end_ts_ms falls in
// the next window, not this one).
typedef struct
{
  int64_t  start_ts_ms;
  int64_t  end_ts_ms;
} wm_bt_window_t;

// Walk-forward windows-per-iteration cap. 256 covers any realistic
// step/test ratio over a multi-year range.
#define WM_BT_WALK_MAX_WINDOWS    256u

// Reject test windows that contain fewer than this many 1m bars.
// 60 = one hour; below that any one-bar trade dominates the score.
#define WM_BT_WALK_MIN_TEST_BARS    60u

typedef struct
{
  uint32_t        n;
  wm_bt_window_t  windows[WM_BT_WALK_MAX_WINDOWS];
} wm_bt_window_set_t;

// Parsed --walk-forward train=Td:test=Md:step=Sd. Days, not ms.
typedef struct
{
  uint32_t  train_days;
  uint32_t  test_days;
  uint32_t  step_days;
} wm_bt_walk_spec_t;

// Parsed --oos-tail P. P is the percent of the supplied range
// reserved as out-of-sample (1..50).
typedef struct
{
  uint32_t  pct;
} wm_bt_oos_spec_t;

// Build the test-window slice list for a walk-forward run. Walk:
// window i trains on [start + i*step, start + i*step + train], tests
// on [train_end, train_end + test], stepping by `step` until the
// next test window would extend past `range_end`. Out-of-history /
// short-test windows are dropped per WM_BT_WALK_MIN_TEST_BARS.
//
// Returns SUCCESS when at least one window is produced; FAIL with err
// populated otherwise. err covers cap overflow (> WALK_MAX_WINDOWS),
// zero windows produced (range too short for the spec), and the
// strategy-min-history deficit case (train_days < strategy's
// min_history requirement on any subscribed grain).
bool wm_bt_walk_build_windows(const wm_bt_walk_spec_t *spec,
    int64_t range_start_ms, int64_t range_end_ms,
    const loaded_strategy_t *ls,
    wm_bt_window_set_t *out, char *err, size_t err_cap);

// Split a range at the OOS-tail boundary. head_window covers the
// first (100-pct)% of the range; tail_window covers the last pct%.
// Returns SUCCESS when both windows have non-zero duration; FAIL on
// pct out of [1, 50] or zero-duration head/tail.
bool wm_bt_oos_split_range(const wm_bt_oos_spec_t *spec,
    int64_t range_start_ms, int64_t range_end_ms,
    wm_bt_window_t *out_head, wm_bt_window_t *out_tail,
    char *err, size_t err_cap);

// ----------------------------------------------------------------------- //
// Snapshot type                                                           //
// ----------------------------------------------------------------------- //

typedef struct wm_backtest_snapshot
{
  // Stub market that owns the cloned grain rings + an aggregator
  // running with strategy fanout disabled. Built by
  // wm_backtest_snapshot_build and read from during iteration. Lock
  // is initialized but uncontended in single-iter runs.
  whenmoon_market_t   mkt;

  int32_t             market_id_db;                       // wm_market.id
  char                source_market_id[WM_MARKET_ID_STR_SZ];

  // Range covered by the warmup, post-pre-flight. Postgres canonical
  // strings ("YYYY-MM-DD HH:MM:SS+00") so the persistence path can
  // hand them straight back to the wm_backtest_run insert.
  char                range_start[40];
  char                range_end[40];

  uint32_t            bars_loaded_1m;
} wm_backtest_snapshot_t;

// ----------------------------------------------------------------------- //
// Run-time params (CLI overrides)                                         //
// ----------------------------------------------------------------------- //
//
// Each `have_*` flag selects whether the matching value is applied to
// the per-iteration backtest book. Defaults flow from the strategy
// KV resolver when no override is set.

typedef struct wm_backtest_params
{
  bool    have_fee_bps;       double  fee_bps;
  bool    have_slip_bps;      double  slip_bps;
  bool    have_size_frac;     double  size_frac;
  bool    have_starting_cash; double  starting_cash;
} wm_backtest_params_t;

// ----------------------------------------------------------------------- //
// Iteration result                                                        //
// ----------------------------------------------------------------------- //

typedef struct wm_backtest_result
{
  int64_t              run_id_db;        // wm_backtest_run.run_id (post-persist)
  uint32_t             bars_replayed;
  uint64_t             wallclock_ms;
  wm_trade_snapshot_t  trade;            // final book snapshot
} wm_backtest_result_t;

// ----------------------------------------------------------------------- //
// Snapshot lifecycle                                                      //
// ----------------------------------------------------------------------- //

// Pre-flight gap check. SUCCESS when [range_start, range_end] is fully
// covered by 1m candles for `market_id_db`; FAIL with `err` populated
// otherwise. `err` (when non-NULL) carries a human-readable summary
// including the exact /whenmoon download candles invocation that
// fixes the gap. Returns SUCCESS even when no rows exist if the
// coverage tracker has no missing intervals — the warmup path will
// catch the empty case downstream.
bool wm_backtest_preflight_gap(int32_t market_id_db,
    const char *range_start, const char *range_end,
    char *err, size_t err_cap);

// Build a snapshot from DB-resident 1m candles in [range_start,
// range_end] for the given market. Replays through a dedicated
// aggregator with strategy fanout disabled so live attachments do
// NOT see warmup bars. `min_history_1d` is the strategy-declared
// history requirement; the snapshot's per-grain ring caps follow the
// same sizing as the live market path plus a headroom slack.
//
// Returns NULL on any failure (gap, OOM, DDL, query); err (when
// non-NULL) is populated with a terse description.
wm_backtest_snapshot_t *wm_backtest_snapshot_build(int32_t market_id_db,
    const char *source_market_id,
    const char *range_start, const char *range_end,
    uint32_t min_history_1d,
    char *err, size_t err_cap);

void wm_backtest_snapshot_free(wm_backtest_snapshot_t *snap);

// ----------------------------------------------------------------------- //
// Iteration                                                               //
// ----------------------------------------------------------------------- //

// Allocate a fresh synthetic market id for one backtest iteration.
// Format: "bt:<N>" where N is a process-monotonic counter. The returned
// id is unique for the daemon's lifetime; reuse across the sweep
// planner is safe because the counter never wraps in any realistic
// runtime (uint64).
//
// Used by the sweep planner (sweep.c) to pre-allocate an id so it can
// register per-iteration KV override slots BEFORE handing the id to
// wm_backtest_run_iteration_with_id below.
void wm_backtest_alloc_synthetic_id(char *out, size_t cap);

// Run one iteration against `snap`. Looks up the loaded strategy by
// name, allocates a fresh wm_strategy_ctx_t, calls init / on_bar (per
// snapshot bar in chronological order across every grain the strategy
// subscribes to) / finalize, drives the trade engine through a
// backtest-private book, and snapshots the result. Persists a
// wm_backtest_run row on success and writes the assigned run_id into
// out->run_id_db.
//
// Returns SUCCESS on a completed iteration; FAIL on setup or run
// errors. err (when non-NULL) is populated on FAIL.
//
// Uses an auto-allocated synthetic id; thin wrapper around
// wm_backtest_run_iteration_with_id (same behaviour as WM-LT-5).
bool wm_backtest_run_iteration(struct whenmoon_state *st,
    wm_backtest_snapshot_t *snap,
    const char *strategy_name,
    const wm_backtest_params_t *params,
    wm_backtest_result_t *out,
    char *err, size_t err_cap);

// Same as wm_backtest_run_iteration but with an externally allocated
// synthetic id and an optional window set. The sweep planner uses
// this so it can write per-iteration KV overrides keyed on the same
// id BEFORE the iteration's strategy init reads them, and so walk-
// forward / OOS layers can scope the on_bar firing to specific test
// windows.
//
// `synth_id` must match the "bt:<N>" pattern produced by
// wm_backtest_alloc_synthetic_id and must be unique within the
// process lifetime (the trade book registry lookups use it directly).
//
// `windows` (optional, NULL = full-range) restricts on_bar callbacks
// to bars whose ts_close_ms falls within at least one window slice.
// The cursor still walks every bar in the snapshot (the aggregator
// state advances naturally), but the strategy callback fires only
// for bars inside a window. Use NULL or n_windows == 0 for the
// single full-range path.
bool wm_backtest_run_iteration_with_id(struct whenmoon_state *st,
    wm_backtest_snapshot_t *snap,
    const char *strategy_name,
    const char *synth_id,
    const wm_backtest_params_t *params,
    const wm_bt_window_t *windows, uint32_t n_windows,
    wm_backtest_result_t *out,
    char *err, size_t err_cap);

// ----------------------------------------------------------------------- //
// Persistence                                                             //
// ----------------------------------------------------------------------- //

// One row of wm_backtest_run, denormalised for cheap rendering.
typedef struct wm_backtest_record
{
  int64_t  run_id;
  int32_t  market_id;
  char     strategy_name[WM_STRATEGY_NAME_SZ];
  char     range_start[40];
  char     range_end[40];
  int64_t  wallclock_ms;
  uint32_t bars_replayed;
  uint32_t n_trades;
  double   realized_pnl;
  double   max_drawdown;
  double   sharpe;
  double   sortino;
  double   final_equity;
  char     window_kind[16];     // 'full' | 'walk' | 'oos'
  uint32_t n_windows;
  bool     have_oos;            // false until wm_backtest_persist_oos_update
  double   oos_score;
  double   oos_realized;
  uint32_t oos_n_trades;
  char     created_at[40];
} wm_backtest_record_t;

// Insert one wm_backtest_run row. params_json is optional (pass NULL
// to insert SQL NULL); metrics_json is required. window_kind +
// n_windows are taken from `rec`; OOS columns are inserted as NULL
// (top-K rows get them populated later via
// wm_backtest_persist_oos_update). On SUCCESS, *out_run_id is
// populated with the assigned BIGSERIAL.
bool wm_backtest_persist_run(const wm_backtest_record_t *rec,
    const char *params_json, const char *metrics_json,
    int64_t *out_run_id);

// Patch the OOS columns + flip window_kind to 'oos' on an existing
// wm_backtest_run row. Used by the OOS post-pass to record the
// out-of-sample validation score for a top-K head-iteration row.
// Returns SUCCESS on a one-row update; FAIL otherwise.
bool wm_backtest_persist_oos_update(int64_t run_id,
    double oos_score, double oos_realized, uint32_t oos_n_trades);

// Load up to `cap` recent runs (newest first). Returns the count
// written. 0 on no rows or query error.
uint32_t wm_backtest_recent_runs(wm_backtest_record_t *out, uint32_t cap);

// Lookup one run by id. SUCCESS on hit, FAIL on miss/error. The
// out_metrics_json / out_params_json pointers (when non-NULL on call)
// are populated with mem_alloc'd copies of the JSONB columns; caller
// frees via mem_free. Pass NULL to skip; pass non-NULL for the JSON
// detail view.
bool wm_backtest_lookup_run(int64_t run_id, wm_backtest_record_t *out,
    char **out_metrics_json, char **out_params_json);

// ----------------------------------------------------------------------- //
// Verb registration                                                       //
// ----------------------------------------------------------------------- //
//
// Defined in backtest_cmds.c.

bool wm_backtest_register_verbs(void);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_BACKTEST_H
