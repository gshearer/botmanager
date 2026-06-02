// backtest.h — snapshot + single-iteration replay.
//
// A backtest is one (market_id, strategy_name, range) replay through
// historical 1m candles. Bars are fed through a dedicated aggregator
// (which performs the multi-grain cascade + indicator pass) into a
// stub `whenmoon_market_t`; the strategy callback fires manually from
// the replay loop. The strategy's signal dispatch lands on a per-
// iteration synthetic `whenmoon_market_t` (heap-owned, NOT in
// st->markets->arr) created by wm_market_create_synthetic, snapshotted
// at the end of the iteration, then destroyed.
//
// Internal to the whenmoon plugin. WHENMOON_INTERNAL gated.

#ifndef BM_WHENMOON_BACKTEST_H
#define BM_WHENMOON_BACKTEST_H

#ifdef WHENMOON_INTERNAL

#include "market.h"
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
  //
  // When `is_mapped` is true the `mkt.grain_arr[g]` pointers are
  // borrowed from the mmap'd region (see WM-BT-2); the snapshot does
  // NOT own those allocations and `mkt.aggregator` stays NULL.
  whenmoon_market_t   mkt;

  int32_t             market_id_db;                       // wm_market.id
  char                source_market_id[WM_MARKET_ID_STR_SZ];

  // Range covered by the warmup, post-pre-flight. Postgres canonical
  // strings ("YYYY-MM-DD HH:MM:SS+00") preserved for downstream
  // renderers + the upcoming WM-BT-2 .wm header.
  char                range_start[40];
  char                range_end[40];

  // WM-BT-2: range epoch ms for downstream consumers (walk-forward
  // + OOS window math). Populated from header on mmap'd snapshots;
  // computed from range_start/range_end on heap-built snapshots
  // (which currently leave both as 0 — WM-BT-3 wires the warmup
  // path to fill them in when the compile verb lands).
  int64_t             range_start_ms;
  int64_t             range_end_ms;

  uint32_t            bars_loaded_1m;

  // WM-BT-2: mmap discriminator. `is_mapped` distinguishes a heap-
  // built snapshot (own the grain rings + aggregator; teardown
  // calls wm_aggregator_destroy + pthread_mutex_destroy + mem_free)
  // from a file-backed snapshot opened via wm_bt_file_open (borrow
  // grain rings from the mapped region; teardown calls
  // wm_bt_file_close which munmap's and frees the struct).
  void               *map_base;
  size_t              map_size;
  int                 map_fd;
  bool                is_mapped;
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
  // Iteration index assigned by the sweep planner (1-based). Persisted
  // run_id semantics retired in WM-BT-1; disk-based persistence lands
  // in WM-BT-6.
  int64_t                       run_id_db;
  uint32_t                      bars_replayed;
  uint64_t                      wallclock_ms;
  wm_market_session_snapshot_t  trade;            // final synth-market snapshot

  // WM-BT-8: deep fills buffer for the chart emitter. Captured from the
  // synth market's PAPER fills ring immediately before
  // wm_market_destroy_synthetic; oldest-to-newest order. Heap-owned via
  // mem_alloc; ownership transfers to the caller (the sweep result row
  // or the OOS validation block frees it). NULL/0 when the iteration
  // produced no fills.
  wm_market_fill_t             *fills;
  uint32_t                      n_fills;
} wm_backtest_result_t;

// ----------------------------------------------------------------------- //
// Snapshot lifecycle                                                      //
// ----------------------------------------------------------------------- //

// Pre-flight check over the actual 1m candle rows of `market_id_db`.
// Holes are tolerated at any size — early illiquid history can be
// missing days that the exchange simply never had, and the snapshot
// builder replays around gaps. FAIL only when [range_start, range_end]
// holds *no* rows at all (a typo'd market or a range entirely before
// the data exists), which would compile an empty snapshot. On FAIL
// `err` (when non-NULL) carries a human-readable summary including the
// exact /whenmoon download invocation for the empty window;
// `market_id_str` is substituted into that suggestion (pass the
// canonical <exch>-<base>-<quote> id).
bool wm_backtest_preflight_gap(int32_t market_id_db,
    const char *market_id_str,
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

// WM-BT-LINK-1: maximum strategies linked on one backtest market. The
// live advisor walk caps attachments at WM_MK3_DISPATCH_MAX_ATTACH (32);
// a backtest comparison never needs that many, and a small cap keeps the
// per-iteration stack arrays tiny on the sweep worker threads.
#define WM_BT_MAX_LINKED  4u

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
// backtest-private book, and snapshots the result.
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

// WM-BT-LINK-1: run one iteration with N (1..WM_BT_MAX_LINKED) strategies
// LINKED on a single backtest market — the faithful test of whenmoon's
// market-owned-position / strategies-as-advisors model (whenmoon_market_
// model.md). All strategies share the one synthetic market (the market
// owns the single position); on each subscribed bar they are polled in
// ARRAY order (strategy_names[0] first = highest priority / lowest
// priority number) and the FIRST to emit a non-zero signal wins, the rest
// are not polled for that bar — bit-identical to the live priority walk in
// wm_strategy_dispatch_bar. Each strategy reads its own params from the KV
// resolver under its own name, so a linked run pins per-strategy configs
// via the global/market KV slots (no per-strategy CLI axes yet).
//
// wm_backtest_run_iteration_with_id is a thin n==1 wrapper around this, so
// every existing single-strategy caller (sweep / OOS / walk-forward) runs
// the identical engine path and stays comparable to a linked run.
bool wm_backtest_run_iteration_multi(struct whenmoon_state *st,
    wm_backtest_snapshot_t *snap,
    const char *const *strategy_names, uint32_t n_strats,
    const char *synth_id,
    const wm_backtest_params_t *params,
    const wm_bt_window_t *windows, uint32_t n_windows,
    wm_backtest_result_t *out,
    char *err, size_t err_cap);

// ----------------------------------------------------------------------- //
// Verb registration                                                       //
// ----------------------------------------------------------------------- //
//
// Defined in backtest_cmds.c.

bool wm_backtest_register_verbs(void);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_BACKTEST_H
