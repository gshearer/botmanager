// sweep.h — backtest sweep planner + worker pool + reload gate (WM-LT-6).
//
// One sweep = N iterations of wm_backtest_run_iteration over the same
// snapshot, each with a different parameter vector drawn from the
// cartesian product of the declared sweep axes. Iterations dispatch
// through a worker pool; each worker owns a private wm_trade_registry
// (see order.h) so per-iteration trade books never contend with the
// live registry.
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
  uint32_t             workers;
} wm_bt_sweep_plan_t;

// Initialise plan to a single-iteration default. score = REALIZED,
// top_k = 1, workers = 1, n_axes = 0, total_iters = 1.
void wm_bt_sweep_plan_init(wm_bt_sweep_plan_t *plan);

// Parse + add one axis to the plan, validating against `ls`'s param
// schema. expr forms:
//   "name=v1,v2,v3"           — discrete value list
//   "name=lo:step:hi"         — range, inclusive of lo, advancing by
//                                step until > hi (FP tolerance)
// Returns SUCCESS on a successful add; FAIL with err populated on parse
// error, unknown param name, type mismatch, value count overflow, or
// caps overflow. Lists the available param names on unknown.
bool wm_bt_sweep_axis_add(wm_bt_sweep_plan_t *plan,
    const loaded_strategy_t *ls, const char *expr,
    char *err, size_t err_cap);

// Compute total_iters as the cartesian product of axis n_values; FAIL
// when total > WM_BT_SWEEP_MAX_ITERS. Always succeeds for n_axes == 0
// (total_iters = 1; the single-iteration baseline).
bool wm_bt_sweep_plan_finalize(wm_bt_sweep_plan_t *plan,
    char *err, size_t err_cap);

// ----------------------------------------------------------------------- //
// Iteration index helpers                                                 //
// ----------------------------------------------------------------------- //

// Decompose iteration index `iter` into per-axis indices. Caller passes
// out_indices[plan->n_axes]. iter must be < plan->total_iters.
void wm_bt_sweep_iter_indices(const wm_bt_sweep_plan_t *plan,
    uint32_t iter, uint32_t *out_indices);

// ----------------------------------------------------------------------- //
// Result row                                                              //
// ----------------------------------------------------------------------- //

typedef struct
{
  uint32_t              indices[WM_BT_SWEEP_MAX_PARAMS];
  uint64_t              wallclock_ms;
  uint32_t              bars_replayed;
  uint32_t              n_windows;          // 1 for full/oos head
  double                score;
  int64_t               run_id_db;          // 0 if persist failed
  bool                  ok;                 // false on iteration failure
  char                  err[160];           // populated when !ok
  wm_trade_snapshot_t   trade;

  // WM-LT-7 OOS post-pass output. have_oos = true when this row was
  // selected as top-K and its OOS validation iteration completed.
  bool                  have_oos;
  double                oos_score;
  double                oos_realized;
  uint32_t              oos_n_trades;
  char                  oos_err[160];       // populated when have_oos
                                            // is intended but the
                                            // validation iter failed
} wm_bt_sweep_result_t;

// Score extraction from a snapshot. NaN/inf collapse to 0.0.
double wm_bt_sweep_score_value(const wm_trade_snapshot_t *snap,
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
//     re-iterate the top-K rows against `oos_tail` and patch
//     wm_backtest_run with the OOS columns.

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
// private trade registry, persists the row to wm_backtest_run, and
// records its result into out_results[iter]. Blocks until every worker
// finishes. Returns SUCCESS when the orchestration completes; per-
// iteration failures are recorded in result.ok / .err.
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
// re-runs each one against `oos_tail`, and patches wm_backtest_run
// with the OOS columns via wm_backtest_persist_oos_update. Stamps
// each top-K result's `oos_*` fields in `results` so the renderer
// can show them inline.
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

// ----------------------------------------------------------------------- //
// Render                                                                  //
// ----------------------------------------------------------------------- //
//
// Sort + render the top-K rows under the active score. The render path
// emits one header row + one per-result row to the cmd ctx.

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
