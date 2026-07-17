// sweep.h — backtest sweep planner + worker pool + reload gate (WM-LT-6).
//
// One sweep = N iterations of wm_backtest_run_iteration over the same
// snapshot, each with a different parameter vector drawn from the
// cartesian product of the declared sweep axes. Iterations dispatch
// through a worker pool; each worker creates a per-iteration synthetic
// `whenmoon_market_t` so iterations never contend on the live market
// registry (see backtest.c).
//
// The reload gate prevents a strategy dlclose+dlopen from invalidating
// cached function pointers held by an in-flight iteration. Sweeps
// increment an active-counter on entry and decrement on exit; reload
// blocks until the counter drops to zero.
//
// Internal to the whenmoon plugin. WHENMOON_INTERNAL gated.

#ifndef BM_WHENMOON_SWEEP_H
#define BM_WHENMOON_SWEEP_H

#ifdef WHENMOON_INTERNAL

#include "backtest.h"
#include "strategy.h"
#include "whenmoon_strategy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct cmd_ctx;
struct whenmoon_state;

// ----------------------------------------------------------------------- //
// Caps                                                                    //
// ----------------------------------------------------------------------- //

#define WM_BT_SWEEP_MAX_PARAMS    8u
#define WM_BT_SWEEP_MAX_VALUES   64u
#define WM_BT_SWEEP_MAX_ITERS    (1u << 20)
#define WM_BT_DEFAULT_TOP_K      20u
#define WM_BT_WORKERS_MIN         1u
#define WM_BT_WORKERS_MAX        64u
#define WM_BT_SWEEP_NAME_SZ      48u

// ----------------------------------------------------------------------- //
// Score selector                                                          //
// ----------------------------------------------------------------------- //

typedef enum
{
  WM_BT_SCORE_REALIZED      = 0,
  WM_BT_SCORE_SHARPE        = 1,
  WM_BT_SCORE_SORTINO       = 2,
  WM_BT_SCORE_EQUITY        = 3,
  WM_BT_SCORE_PROFIT_FACTOR = 4,
} wm_bt_sweep_score_t;

bool        wm_bt_sweep_score_parse(const char *tok,
    wm_bt_sweep_score_t *out);
const char *wm_bt_sweep_score_name(wm_bt_sweep_score_t s);

// ----------------------------------------------------------------------- //
// Plan                                                                    //
// ----------------------------------------------------------------------- //
//
// Each axis declares the sweep range for one strategy parameter. Values
// are stored as doubles; the per-iteration KV writer formats them back
// to the schema-declared type (INT/UINT/DOUBLE) at apply time.

typedef struct
{
  char             name[WM_BT_SWEEP_NAME_SZ];
  wm_param_type_t  type;
  uint32_t         n_values;
  double           values[WM_BT_SWEEP_MAX_VALUES];
} wm_bt_sweep_axis_t;

typedef struct
{
  wm_bt_sweep_axis_t   axes[WM_BT_SWEEP_MAX_PARAMS];
  uint32_t             n_axes;
  uint32_t             total_iters;          // populated by finalize
  wm_bt_sweep_score_t  score;
  uint32_t             top_k;
  uint32_t             perfold_top_k;        // 0 = follow top_k
  uint32_t             workers;
} wm_bt_sweep_plan_t;

// Initialise plan to a single-iteration default. score = REALIZED,
// top_k = 1, perfold_top_k = 0 (follow top_k), workers = 1,
// n_axes = 0, total_iters = 1.
void wm_bt_sweep_plan_init(wm_bt_sweep_plan_t *plan);

// Parse + add one axis to the plan, validating against `ls`'s param
// schema. expr forms:
//   "name=v1,v2,v3"           — discrete value list
//   "name=[v1,v2,v3]"         — same, with surrounding brackets
//   "name=lo:step:hi"         — range, inclusive of lo, advancing by
//                                step until > hi (FP tolerance)
// Returns SUCCESS on a successful add; FAIL with err populated on parse
// error, unknown param name, type mismatch, value count overflow, or
// caps overflow. Lists the available param names on unknown. A second
// add for the same axis name silently replaces the earlier entry; this
// is what lets inline --sweep override --config-loaded axes.
bool wm_bt_sweep_axis_add(wm_bt_sweep_plan_t *plan,
    const loaded_strategy_t *ls, const char *expr,
    char *err, size_t err_cap);

// Compute total_iters as the cartesian product of axis n_values; FAIL
// when total > WM_BT_SWEEP_MAX_ITERS. Always succeeds for n_axes == 0
// (total_iters = 1; the single-iteration baseline).
bool wm_bt_sweep_plan_finalize(wm_bt_sweep_plan_t *plan,
    char *err, size_t err_cap);

// Load a JSON sweep config file. Schema:
//   {"params": {
//      "fast_period": {"start": 5, "step": 1, "end": 15},
//      "slow_period": [20, 30, 50],
//      "fee_bps":     10
//   }}
// Each "params" entry is converted to the textual expr form and routed
// through wm_bt_sweep_axis_add. Scalars become "name=value", arrays
// become "name=[v1,v2,...]", and {start,step,end} objects become
// "name=start:step:end". Subsequent inline --sweep overrides win via
// axis_add's replace-on-collision semantic.
bool wm_bt_load_config_file(wm_bt_sweep_plan_t *plan,
    const loaded_strategy_t *ls, const char *path,
    char *err, size_t err_cap);

// ----------------------------------------------------------------------- //
// Iteration index helpers                                                 //
// ----------------------------------------------------------------------- //

// Decompose iteration index `iter` into per-axis indices. Caller passes
// out_indices[plan->n_axes]. iter must be < plan->total_iters.
void wm_bt_sweep_iter_indices(const wm_bt_sweep_plan_t *plan,
    uint32_t iter, uint32_t *out_indices);

// ----------------------------------------------------------------------- //
// Per-fold (walk-forward test-window) metric                              //
// ----------------------------------------------------------------------- //
//
// WM-BT-WF-PERFOLD-1: one out-of-sample test window measured
// INDEPENDENTLY of the others. The walk-forward aggregate runs every
// test slice back-to-back through one compounding book; this instead
// re-runs each slice on its own book seeded from `starting_cash` (with
// the strategy warmed by the full pre-window history), so `realized_pnl`
// / `return_frac` are non-compounding per-fold readings for the
// consistency gate. Populated by wm_bt_sweep_run_walk_perfold for the
// top-K rows only.
typedef struct
{
  int64_t   start_ts_ms;    // test-window bounds (from the walk set)
  int64_t   end_ts_ms;
  double    realized_pnl;   // stats[PAPER].realized_pnl_lifetime
  double    return_frac;    // realized_pnl / starting_cash (0 if cash<=0)
  double    final_equity;   // wm_bt_compute_equity of the fold snapshot
  uint32_t  n_trades;       // round-trips = n_wins + n_losses
  bool      ok;             // false if this fold's iteration failed

  // WM-RIGOR-2: buy-and-hold return of the same asset over the same
  // window (close/close - 1 from the snapshot's 1m bars), so the scorer
  // can compute active (benchmark-relative) fold returns. Valid only
  // when bench_ok; bench_ok false means the window held < 2 1m bars.
  double    bench_return;
  bool      bench_ok;
} wm_bt_fold_metric_t;

// ----------------------------------------------------------------------- //
// Result row                                                              //
// ----------------------------------------------------------------------- //

typedef struct
{
  uint32_t              indices[WM_BT_SWEEP_MAX_PARAMS];
  uint64_t              wallclock_ms;
  uint32_t              bars_replayed;
  uint32_t              n_windows;          // 1 for full/oos head
  double                       score;
  // 1-based iteration index. WM-BT-1 retired the wm_backtest_run
  // BIGSERIAL semantic; disk-based persistence lands in WM-BT-6.
  int64_t                      run_id_db;
  bool                         ok;                 // false on iteration failure
  char                         err[160];           // populated when !ok
  wm_market_session_snapshot_t trade;

  // WM-LT-7 OOS post-pass output. have_oos = true when this row was
  // selected as top-K and its OOS validation iteration completed.
  bool                  have_oos;
  double                oos_score;
  double                oos_realized;
  uint32_t              oos_n_trades;
  char                  oos_err[160];       // populated when have_oos
                                            // is intended but the
                                            // validation iter failed

  // WM-BT-8: deep fills buffer transferred from wm_backtest_result_t
  // by wm_bt_sweep_run_one. Owner is the sweep_results table; the
  // caller (wm_bt_cmd_run) frees each row's buffer before freeing
  // the table itself.
  wm_market_fill_t     *fills;
  uint32_t              n_fills;

  // WM-BT-WF-PERFOLD-1: per-test-window (fold) OOS metrics for
  // walk-forward mode; length n_folds (== mode->walk.n). Heap-owned;
  // owner is the sweep_results table (wm_bt_results_free_fills frees it
  // alongside `fills`). NULL / 0 for FULL / OOS mode and for
  // walk-forward rows outside the top-K.
  wm_bt_fold_metric_t  *folds;
  uint32_t              n_folds;
} wm_bt_sweep_result_t;

// Score extraction from a synth-market snapshot. NaN/inf collapse to
// 0.0. Reads from `.stats[WM_MARKET_MODE_PAPER]`. The legacy book
// engine published a richer metric package (sharpe, sortino, etc.);
// the per-market session only carries lifetime realized PnL + cash —
// the SHARPE / SORTINO / PROFIT_FACTOR selectors collapse to
// NOSCORE so the renderer sorts them to the bottom rather than
// misleading the operator with a phantom zero.
double wm_bt_sweep_score_value(const wm_market_session_snapshot_t *snap,
    wm_bt_sweep_score_t score);

// ----------------------------------------------------------------------- //
// Per-iteration KV cleanup                                                //
// ----------------------------------------------------------------------- //

// Drop every per-iteration KV slot under
// "plugin.whenmoon.market.bt:". Called at sweep run start to clear any
// stale rows left over from a prior crashed worker.
void wm_bt_sweep_cleanup_stale_kv(void);

// ----------------------------------------------------------------------- //
// Sweep run mode (WM-LT-7)                                                //
// ----------------------------------------------------------------------- //
//
// Three modes share the iteration loop with different window scopes:
//   * FULL: every iteration walks the full snapshot range.
//   * WALK_FORWARD: every iteration walks `walk.windows` test slices
//     back-to-back through one trade book per param vector.
//   * OOS: head sweep iterates over `oos_head` only; after the sweep
//     finishes, the caller runs `wm_bt_sweep_run_oos_validation` to
//     re-iterate the top-K rows against `oos_tail` and stamp the
//     OOS columns on each top-K result row in memory.

typedef struct
{
  wm_bt_run_mode_t    mode;
  wm_bt_window_set_t  walk;
  wm_bt_window_t      oos_head;
  wm_bt_window_t      oos_tail;
} wm_bt_sweep_mode_t;

// ----------------------------------------------------------------------- //
// Sweep run                                                               //
// ----------------------------------------------------------------------- //

// Run an entire sweep. Spawns plan->workers worker threads; each
// worker pulls jobs off a FIFO queue, runs one iteration with a fresh
// private trade registry, and records its result into
// out_results[iter]. Blocks until every worker finishes. Returns
// SUCCESS when the orchestration completes; per-iteration failures
// are recorded in result.ok / .err.
//
// `mode` carries the run-mode + window scope. NULL = legacy WM-LT-6
// behaviour (FULL mode, no windows).
//
// out_results storage is caller-owned; cap = plan->total_iters.
bool wm_bt_sweep_run(struct whenmoon_state *st,
    wm_backtest_snapshot_t *snap,
    const char *strategy_name,
    int32_t market_id_db,
    const wm_bt_sweep_plan_t *plan,
    const wm_bt_sweep_mode_t *mode,
    const wm_backtest_params_t *base_params,
    wm_bt_sweep_result_t *out_results,
    char *err, size_t err_cap);

// OOS post-pass. Called after wm_bt_sweep_run completes in OOS mode.
// Takes the same plan + results table, picks the top-K by score,
// re-runs each one against `oos_tail`, and stamps the OOS columns
// on each top-K result row in memory so the renderer can show them
// inline.
//
// Returns SUCCESS if every top-K validation iteration completed; FAIL
// (with err) if no top-K was eligible (e.g. all head iterations
// failed). Per-row OOS failures are recorded in `results[i].oos_err`
// and do not cause the overall pass to FAIL.
bool wm_bt_sweep_run_oos_validation(struct whenmoon_state *st,
    wm_backtest_snapshot_t *snap,
    const char *strategy_name,
    int32_t market_id_db,
    const wm_bt_sweep_plan_t *plan,
    const wm_bt_window_t *oos_tail,
    const wm_backtest_params_t *base_params,
    wm_bt_sweep_result_t *results,
    char *err, size_t err_cap);

// WM-BT-WF-PERFOLD-1 post-pass. Called after wm_bt_sweep_run completes in
// WALK_FORWARD mode. Picks the top-K rows by score and, for each, measures
// every test window INDEPENDENTLY (a fresh single-window iteration per
// slice, book seeded from starting_cash, strategy warmed by the full
// pre-window history) and stamps `results[i].folds` / `.n_folds` in
// memory. Leaves the back-to-back aggregate (`results[i].trade`)
// untouched. The heap `folds` array is owned by the results table and
// freed by wm_bt_results_free_fills.
//
// Returns SUCCESS when at least one top-K row got a per-fold breakdown;
// FAIL (with err) when no eligible top-K row could complete. Per-fold
// iteration failures are recorded in `folds[w].ok = false` and do not
// FAIL the pass.
bool wm_bt_sweep_run_walk_perfold(struct whenmoon_state *st,
    wm_backtest_snapshot_t *snap,
    const char *strategy_name,
    const wm_bt_sweep_plan_t *plan,
    const wm_bt_window_set_t *walk,
    const wm_backtest_params_t *base_params,
    wm_bt_sweep_result_t *results,
    char *err, size_t err_cap);

// ----------------------------------------------------------------------- //
// Render                                                                  //
// ----------------------------------------------------------------------- //
//
// The renderer comes in three pieces (WM-BT-7):
//
//   * wm_bt_topk_compute — score-descending sort, returns top-K indices
//     into `results[]`. Caller owns `out_indices` (cap >= top_n).
//   * wm_bt_topk_to_ctx — header + per-row colorized cmd_reply.
//   * wm_bt_topk_to_file — same content, no ANSI, FILE * stream.
//
// `wm_bt_sweep_render_topk` is the legacy single-call wrapper retained
// for `wm_bt_cmd_run`'s immediate-feedback cmd_reply path; it composes
// compute + to_ctx internally.

uint32_t wm_bt_topk_compute(const wm_bt_sweep_result_t *results,
    uint32_t n_results, uint32_t top_n, uint32_t *out_indices);

void wm_bt_topk_to_ctx(const struct cmd_ctx *ctx,
    const wm_bt_sweep_plan_t *plan,
    const wm_bt_sweep_mode_t *mode,
    const wm_bt_sweep_result_t *results,
    const uint32_t *indices, uint32_t top_k,
    uint32_t n_total, uint32_t n_ok);

bool wm_bt_topk_to_file(FILE *fp,
    const wm_bt_sweep_plan_t *plan,
    const wm_bt_sweep_mode_t *mode,
    const wm_bt_sweep_result_t *results,
    const uint32_t *indices, uint32_t top_k,
    uint32_t n_total, uint32_t n_ok);

void wm_bt_sweep_render_topk(const struct cmd_ctx *ctx,
    const wm_bt_sweep_plan_t *plan,
    const wm_bt_sweep_mode_t *mode,
    const wm_bt_sweep_result_t *results, uint32_t n);

// ----------------------------------------------------------------------- //
// Reload gating                                                           //
// ----------------------------------------------------------------------- //

void     wm_bt_sweep_active_inc(void);
void     wm_bt_sweep_active_dec(void);
uint32_t wm_bt_sweep_active_count(void);

// Reload a strategy under the global reload lock. Waits for active
// sweeps to drain (CLAM_INFO every 5s while waiting), then performs
// the dlclose + dlopen + resolve + init + start sequence + registry
// rescan. Returns SUCCESS on a clean reload; FAIL with err populated
// otherwise.
bool wm_bt_sweep_reload_strategy(struct whenmoon_state *st,
    const char *strategy_name, uint32_t *out_n_detached,
    char *err, size_t err_cap);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_SWEEP_H
