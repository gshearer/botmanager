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
bool wm_backtest_run_iteration(struct whenmoon_state *st,
    wm_backtest_snapshot_t *snap,
    const char *strategy_name,
    const wm_backtest_params_t *params,
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
  char     created_at[40];
} wm_backtest_record_t;

// Insert one wm_backtest_run row. params_json is optional (pass NULL
// to insert SQL NULL); metrics_json is required. On SUCCESS,
// *out_run_id is populated with the assigned BIGSERIAL.
bool wm_backtest_persist_run(const wm_backtest_record_t *rec,
    const char *params_json, const char *metrics_json,
    int64_t *out_run_id);

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
