// botmanager — MIT
// Whenmoon backtest snapshot + single-iteration replay (WM-LT-5).
//
// One backtest run = (a) pre-flight gap check on the 1m candle
// coverage; (b) snapshot construction by replaying 1m candles from
// `wm_candles_<id>_60` through a dedicated aggregator with strategy
// fanout disabled; (c) one iteration that walks the snapshot bars
// chronologically across every grain the strategy subscribes to,
// firing wm_strategy_on_bar through a backtest-private trade book
// (synthetic id "bt:<n>") in PAPER mode; (d) persistence of the
// resulting metrics into `wm_backtest_run`.
//
// The reuse story:
//   * Aggregator + indicator pass: same code path as live. The flag
//     `dispatch_strategies` flips fanout off during warmup.
//   * Trade book + sizer + paper-fill engine: same code path as live.
//     The synthetic market_id keeps the backtest book in the global
//     trade registry but isolated from live (market, strategy) keys.
//   * PnL accumulators: same struct as live; metrics are read out of
//     the trade snapshot.
//
// Single-iteration runs serialize on the trade-book registry lock —
// fine for WM-LT-5. WM-LT-6 will swap in a private per-iteration
// registry to enable parallel sweeps.

#define WHENMOON_INTERNAL
#include "backtest.h"

#include "aggregator.h"
#include "dl_coverage.h"
#include "dl_schema.h"
#include "market.h"
#include "order.h"
#include "pnl.h"
#include "strategy.h"
#include "whenmoon.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "db.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WM_BT_CTX  "whenmoon.backtest"

// ----------------------------------------------------------------------- //
// Synthetic-id allocator                                                  //
// ----------------------------------------------------------------------- //
//
// Each iteration produces a fresh id "bt:<n>" where n is a monotonic
// counter local to this daemon. The id lives only for the iteration
// lifetime; after persistence the book is removed from the trade
// registry. Persistence carries the actual wm_market.id of the source
// market, not the synthetic id, so DB consumers see the real market
// reference.

static atomic_uint_fast64_t g_bt_iter_counter = 0;

void
wm_backtest_alloc_synthetic_id(char *out, size_t cap)
{
  uint64_t n;

  if(out == NULL || cap == 0)
    return;

  n = atomic_fetch_add(&g_bt_iter_counter, 1) + 1;
  snprintf(out, cap, WM_BACKTEST_ID_PREFIX "%" PRIu64, n);
}

// ----------------------------------------------------------------------- //
// Date helpers                                                            //
// ----------------------------------------------------------------------- //

// Parse "YYYY-MM-DD HH:MM:SS+00" into epoch seconds (UTC). Returns
// FAIL on any parse error.
static bool
wm_bt_parse_ts(const char *in, time_t *out)
{
  struct tm tm;
  unsigned  yyyy, mo, dd, hh, mm, ss;
  int       consumed = 0;

  if(in == NULL || out == NULL)
    return(FAIL);

  if(sscanf(in, "%u-%u-%u %u:%u:%u%n",
        &yyyy, &mo, &dd, &hh, &mm, &ss, &consumed) != 6 || consumed < 19)
    return(FAIL);

  memset(&tm, 0, sizeof(tm));
  tm.tm_year = (int)yyyy - 1900;
  tm.tm_mon  = (int)mo - 1;
  tm.tm_mday = (int)dd;
  tm.tm_hour = (int)hh;
  tm.tm_min  = (int)mm;
  tm.tm_sec  = (int)ss;

  *out = timegm(&tm);

  if(*out == (time_t)-1)
    return(FAIL);

  return(SUCCESS);
}

static uint32_t
wm_bt_days_between(const char *start_ts, const char *end_ts)
{
  time_t s, e;

  if(wm_bt_parse_ts(start_ts, &s) != SUCCESS ||
     wm_bt_parse_ts(end_ts,   &e) != SUCCESS ||
     e <= s)
    return(1);

  return((uint32_t)(((int64_t)(e - s) + 86399) / 86400));
}

// ----------------------------------------------------------------------- //
// Pre-flight                                                              //
// ----------------------------------------------------------------------- //

bool
wm_backtest_preflight_gap(int32_t market_id_db,
    const char *range_start, const char *range_end,
    char *err, size_t err_cap)
{
  wm_coverage_t  gap;
  uint32_t       n;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(market_id_db < 0 || range_start == NULL || range_end == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "bad pre-flight inputs");
    return(FAIL);
  }

  n = wm_coverage_gaps_candles(market_id_db, 60,
      range_start, range_end, &gap, 1);

  if(n == 0)
    return(SUCCESS);

  if(err != NULL)
  {
    char start_date[16];
    char end_date[16];

    snprintf(start_date, sizeof(start_date), "%.10s", gap.first_ts);
    snprintf(end_date,   sizeof(end_date),   "%.10s", gap.last_ts);

    snprintf(err, err_cap,
        "missing 1m coverage in %s..%s; run"
        " /whenmoon download candles <market_id> %s %s",
        gap.first_ts, gap.last_ts, start_date, end_date);
  }

  return(FAIL);
}

// ----------------------------------------------------------------------- //
// Snapshot construction                                                   //
// ----------------------------------------------------------------------- //

// Free the rings + aggregator + lock owned by the stub market in
// `snap`. Only invoked on snapshots whose mutex_init + aggregator_init
// both succeeded (early-failure paths in build clean up directly).
static void
wm_bt_snapshot_teardown(wm_backtest_snapshot_t *snap)
{
  if(snap == NULL)
    return;

  wm_aggregator_destroy(&snap->mkt);
  pthread_mutex_destroy(&snap->mkt.lock);
  mem_free(snap);
}

void
wm_backtest_snapshot_free(wm_backtest_snapshot_t *snap)
{
  wm_bt_snapshot_teardown(snap);
}

wm_backtest_snapshot_t *
wm_backtest_snapshot_build(int32_t market_id_db,
    const char *source_market_id,
    const char *range_start, const char *range_end,
    uint32_t min_history_1d,
    char *err, size_t err_cap)
{
  wm_backtest_snapshot_t *snap = NULL;
  db_result_t            *res  = NULL;
  char                    table[WM_DL_TABLE_SZ];
  char                   *e_start = NULL;
  char                   *e_end   = NULL;
  char                    sql[1024];
  uint32_t                history_days;
  uint32_t                replayed = 0;
  uint32_t                i;
  int                     n;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(market_id_db < 0 || source_market_id == NULL ||
     range_start == NULL || range_end == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "bad snapshot inputs");
    return(NULL);
  }

  // History sizing: cover the requested range with headroom so the
  // shift-on-full path inside push_bar never trims the most-recent
  // bars during warmup.
  history_days = wm_bt_days_between(range_start, range_end)
               + WM_BACKTEST_HISTORY_HEADROOM_DAYS;

  if(min_history_1d > history_days)
    history_days = min_history_1d + WM_BACKTEST_HISTORY_HEADROOM_DAYS;

  if(wm_candle_table_name(market_id_db, 60, table, sizeof(table))
     != SUCCESS)
  {
    if(err != NULL)
      snprintf(err, err_cap, "candle table name overflow");
    return(NULL);
  }

  // Idempotent — the table may not exist if no candles have ever been
  // downloaded (the pre-flight should have caught this, but stay
  // defensive).
  if(wm_candle_table_ensure(market_id_db, 60) != SUCCESS)
  {
    if(err != NULL)
      snprintf(err, err_cap, "candle table ensure failed (%s)", table);
    return(NULL);
  }

  snap = mem_alloc("whenmoon.backtest", "snapshot", sizeof(*snap));

  if(snap == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "out of memory");
    return(NULL);
  }

  memset(snap, 0, sizeof(*snap));

  snap->market_id_db = market_id_db;
  snprintf(snap->source_market_id, sizeof(snap->source_market_id),
      "%s", source_market_id);
  snprintf(snap->range_start, sizeof(snap->range_start),
      "%s", range_start);
  snprintf(snap->range_end, sizeof(snap->range_end),
      "%s", range_end);

  // Stub market plumbing — only the fields the aggregator + indicator
  // pass + strategy callback read.
  snprintf(snap->mkt.market_id_str, sizeof(snap->mkt.market_id_str),
      "%s", source_market_id);
  snprintf(snap->mkt.product_id, sizeof(snap->mkt.product_id),
      "%s", source_market_id);
  snap->mkt.market_id = market_id_db;

  if(pthread_mutex_init(&snap->mkt.lock, NULL) != 0)
  {
    mem_free(snap);

    if(err != NULL)
      snprintf(err, err_cap, "snapshot lock init failed");
    return(NULL);
  }

  if(wm_aggregator_init(&snap->mkt, history_days) != SUCCESS)
  {
    pthread_mutex_destroy(&snap->mkt.lock);
    mem_free(snap);

    if(err != NULL)
      snprintf(err, err_cap, "aggregator init failed (history=%u days)",
          history_days);
    return(NULL);
  }

  // Disable live strategy fanout for the warmup window. Backtest
  // iteration fires strategy callbacks manually.
  snap->mkt.aggregator->dispatch_strategies = false;

  // Query candles in [range_start, range_end] ascending. The escape
  // path matches the convention used elsewhere in the downloader.
  e_start = db_escape(range_start);
  e_end   = db_escape(range_end);

  if(e_start == NULL || e_end == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "escape failed");
    goto fail;
  }

  n = snprintf(sql, sizeof(sql),
      "SELECT (EXTRACT(EPOCH FROM ts)::BIGINT * 1000) AS ts_ms,"
      "       low, high, open, close, volume"
      "  FROM %s"
      " WHERE ts >= TIMESTAMPTZ '%s'"
      "   AND ts <  TIMESTAMPTZ '%s'"
      " ORDER BY ts ASC",
      table, e_start, e_end);

  if(n < 0 || (size_t)n >= sizeof(sql))
  {
    if(err != NULL)
      snprintf(err, err_cap, "snapshot query overflow");
    goto fail;
  }

  res = db_result_alloc();

  if(res == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "db_result_alloc failed");
    goto fail;
  }

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    if(err != NULL)
      snprintf(err, err_cap, "snapshot query failed: %s",
          res->error[0] != '\0' ? res->error : "(no driver error)");
    goto fail;
  }

  if(res->rows == 0)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "no 1m candles in %s..%s; run"
          " /whenmoon download candles ... first",
          range_start, range_end);
    goto fail;
  }

  pthread_mutex_lock(&snap->mkt.lock);

  for(i = 0; i < res->rows; i++)
  {
    const char       *s_ts     = db_result_get(res, i, 0);
    const char       *s_low    = db_result_get(res, i, 1);
    const char       *s_high   = db_result_get(res, i, 2);
    const char       *s_open   = db_result_get(res, i, 3);
    const char       *s_close  = db_result_get(res, i, 4);
    const char       *s_volume = db_result_get(res, i, 5);
    wm_candle_full_t  bar;
    int64_t           ts_open_ms;

    if(s_ts == NULL || s_low == NULL || s_high == NULL ||
       s_open == NULL || s_close == NULL || s_volume == NULL)
      continue;

    ts_open_ms = (int64_t)strtoll(s_ts, NULL, 10);

    memset(&bar, 0, sizeof(bar));
    bar.ts_close_ms = ts_open_ms + 60000;
    bar.low         = strtod(s_low,    NULL);
    bar.high        = strtod(s_high,   NULL);
    bar.open        = strtod(s_open,   NULL);
    bar.close       = strtod(s_close,  NULL);
    bar.volume      = strtod(s_volume, NULL);

    wm_aggregator_replay_bar(&snap->mkt, WM_GRAN_1M, &bar);
    replayed++;
  }

  pthread_mutex_unlock(&snap->mkt.lock);

  snap->bars_loaded_1m = replayed;

  clam(CLAM_INFO, WM_BT_CTX,
      "snapshot built: %s [%s..%s] 1m_bars=%u history=%u days"
      " (5m=%u 15m=%u 1h=%u 6h=%u 1d=%u)",
      source_market_id, range_start, range_end,
      replayed, history_days,
      snap->mkt.grain_n[WM_GRAN_5M], snap->mkt.grain_n[WM_GRAN_15M],
      snap->mkt.grain_n[WM_GRAN_1H], snap->mkt.grain_n[WM_GRAN_6H],
      snap->mkt.grain_n[WM_GRAN_1D]);

  db_result_free(res);
  mem_free(e_start);
  mem_free(e_end);

  return(snap);

fail:
  if(res     != NULL) db_result_free(res);
  if(e_start != NULL) mem_free(e_start);
  if(e_end   != NULL) mem_free(e_end);

  // wm_bt_snapshot_teardown handles the partially-built case.
  wm_bt_snapshot_teardown(snap);
  return(NULL);
}

// ----------------------------------------------------------------------- //
// Iteration                                                               //
// ----------------------------------------------------------------------- //

// Per-grain cursor for the merged-walk loop. We advance the cursor
// whose next bar has the smallest ts_close_ms; equal-ts ties resolve
// in grain order (1m fires before 5m before 15m, mirroring the live
// cascade order).
typedef struct
{
  uint32_t  idx;        // next index to dispatch
  uint32_t  cap;        // grain_n[g] at iteration start
  bool      subscribed; // strategy.grains_mask & (1u << g)
} wm_bt_cursor_t;

// Find the next grain to fire. Returns WM_GRAN_MAX when all cursors
// are exhausted.
static wm_gran_t
wm_bt_next_grain(const wm_bt_cursor_t *cursors,
    const wm_candle_full_t *const *rings)
{
  wm_gran_t best = WM_GRAN_MAX;
  int64_t   best_ts = INT64_MAX;
  uint32_t  g;

  for(g = 0; g < WM_GRAN_MAX; g++)
  {
    int64_t ts;

    if(cursors[g].idx >= cursors[g].cap)
      continue;

    ts = rings[g][cursors[g].idx].ts_close_ms;

    if(ts < best_ts)
    {
      best_ts = ts;
      best    = (wm_gran_t)g;
    }
  }

  return(best);
}

bool
wm_backtest_run_iteration(whenmoon_state_t *st,
    wm_backtest_snapshot_t *snap,
    const char *strategy_name,
    const wm_backtest_params_t *params,
    wm_backtest_result_t *out,
    char *err, size_t err_cap)
{
  char synth_id[WM_MARKET_ID_STR_SZ];

  wm_backtest_alloc_synthetic_id(synth_id, sizeof(synth_id));
  return(wm_backtest_run_iteration_with_id(st, snap, strategy_name,
      synth_id, params, NULL, 0, out, err, err_cap));
}

// Return true when ts_ms falls inside at least one window. n_windows
// is small (<= WM_BT_WALK_MAX_WINDOWS = 256), and windows are sorted
// chronologically; a tighter binary search is unnecessary at v1
// scale, but the linear scan still short-circuits on the first hit.
static inline bool
wm_bt_in_any_window(const wm_bt_window_t *w, uint32_t n, int64_t ts_ms)
{
  uint32_t i;

  for(i = 0; i < n; i++)
  {
    if(ts_ms >= w[i].start_ts_ms && ts_ms < w[i].end_ts_ms)
      return(true);

    // Windows sorted ascending — bail once we pass the bar's ts.
    if(w[i].start_ts_ms > ts_ms)
      return(false);
  }

  return(false);
}

bool
wm_backtest_run_iteration_with_id(whenmoon_state_t *st,
    wm_backtest_snapshot_t *snap,
    const char *strategy_name,
    const char *synth_id,
    const wm_backtest_params_t *params,
    const wm_bt_window_t *windows, uint32_t n_windows,
    wm_backtest_result_t *out,
    char *err, size_t err_cap)
{
  loaded_strategy_t              *ls;
  int                           (*init_fn)(wm_strategy_ctx_t *);
  void                          (*finalize_fn)(wm_strategy_ctx_t *);
  void                          (*on_bar_fn)(wm_strategy_ctx_t *,
                                    const struct whenmoon_market *,
                                    wm_gran_t,
                                    const wm_candle_full_t *);
  uint16_t                        grains_mask;
  wm_strategy_ctx_t               ctx;
  wm_trade_book_t                *book;
  char                            strat_copy[WM_STRATEGY_NAME_SZ];
  wm_bt_cursor_t                  cursors[WM_GRAN_MAX];
  const wm_candle_full_t         *rings[WM_GRAN_MAX];
  uint32_t                        bars_replayed = 0;
  struct timespec                 t0, t1;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(out != NULL)
    memset(out, 0, sizeof(*out));

  if(st == NULL || snap == NULL || strategy_name == NULL ||
     synth_id == NULL || synth_id[0] == '\0' || out == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "bad iteration inputs");
    return(FAIL);
  }

  // Resolve the strategy + cache its function pointers under the
  // registry lock, then release. The function pointers are stable for
  // the lifetime of the loaded_strategy_t — a reload would invalidate
  // them; sweep callers gate reload via the active-counter in sweep.c.
  pthread_mutex_lock(&st->strategies->lock);

  ls = wm_strategy_find_loaded(st, strategy_name);

  if(ls == NULL || ls->init_fn == NULL || ls->finalize_fn == NULL ||
     ls->on_bar_fn == NULL)
  {
    pthread_mutex_unlock(&st->strategies->lock);

    if(err != NULL)
      snprintf(err, err_cap, "strategy %s not loaded", strategy_name);
    return(FAIL);
  }

  init_fn     = ls->init_fn;
  finalize_fn = ls->finalize_fn;
  on_bar_fn   = ls->on_bar_fn;
  grains_mask = ls->meta.grains_mask;

  snprintf(strat_copy, sizeof(strat_copy), "%s", ls->name);

  pthread_mutex_unlock(&st->strategies->lock);

  // Use the caller-supplied synthetic id; prime the backtest book in
  // the active trade registry. PAPER mode reuses the live fill engine.
  // The sweep worker has bound a private registry via
  // wm_trade_engine_use_registry beforehand.
  book = wm_trade_book_get_or_create(synth_id, strat_copy);

  if(book == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "trade book create failed");
    return(FAIL);
  }

  if(params != NULL)
    wm_trade_book_override_params(synth_id, strat_copy,
        params->have_fee_bps,        params->fee_bps,
        params->have_slip_bps,       params->slip_bps,
        params->have_size_frac,      params->size_frac,
        params->have_starting_cash,  params->starting_cash);

  if(wm_trade_book_set_mode(synth_id, strat_copy, WM_TRADE_MODE_PAPER)
     != SUCCESS)
  {
    wm_trade_book_remove(synth_id, strat_copy);

    if(err != NULL)
      snprintf(err, err_cap, "trade book set_mode failed");
    return(FAIL);
  }

  // Build the strategy ctx. The market_id_str on the ctx is what
  // wm_strategy_emit_signal_impl forwards to wm_trade_engine_on_signal,
  // so it must match the synthetic id we just registered the book under.
  memset(&ctx, 0, sizeof(ctx));
  snprintf(ctx.market_id_str, sizeof(ctx.market_id_str), "%s", synth_id);
  snprintf(ctx.strategy_name, sizeof(ctx.strategy_name), "%s", strat_copy);
  ctx.mkt = &snap->mkt;

  if(init_fn(&ctx) != 0)
  {
    wm_trade_book_remove(synth_id, strat_copy);

    if(err != NULL)
      snprintf(err, err_cap, "strategy init returned non-zero");
    return(FAIL);
  }

  // Set up cursors over each grain ring.
  {
    uint32_t g;

    for(g = 0; g < WM_GRAN_MAX; g++)
    {
      cursors[g].idx        = 0;
      cursors[g].cap        = snap->mkt.grain_n[g];
      cursors[g].subscribed = (grains_mask & (uint16_t)(1u << g)) != 0;
      rings[g]              = snap->mkt.grain_arr[g];
    }
  }

  clock_gettime(CLOCK_MONOTONIC, &t0);

  // Merged chronological walk. Equal-ts ties resolve in grain order
  // (the cursor with the lower g index wins), mirroring the live
  // cascade where 1m's push fires before 5m's.
  for(;;)
  {
    wm_gran_t                g;
    const wm_candle_full_t  *bar;

    g = wm_bt_next_grain(cursors, rings);

    if(g == WM_GRAN_MAX)
      break;

    bar = &rings[g][cursors[g].idx];

    // Match wm_strategy_dispatch_bar: ctx fields update only for
    // grains the strategy subscribes to (un-subscribed grains keep
    // the cursor moving but never touch the strategy's mark cache).
    //
    // Walk-forward / OOS gating: when a window set is supplied, the
    // strategy callback fires only for bars whose ts falls inside a
    // test window. Bars outside the windows still advance their
    // cursor — the aggregator and ctx mark caches stay coherent —
    // but the strategy never sees them, so the trade book records
    // fills only during windowed ranges.
    if(cursors[g].subscribed)
    {
      bool in_window = (n_windows == 0) ||
          wm_bt_in_any_window(windows, n_windows, bar->ts_close_ms);

      if(in_window)
      {
        ctx.bars_seen++;
        ctx.last_bar_ts_ms = bar->ts_close_ms;
        ctx.last_mark_px   = bar->close;
        ctx.last_mark_ms   = bar->ts_close_ms;

        on_bar_fn(&ctx, &snap->mkt, g, bar);
        bars_replayed++;
      }
    }

    cursors[g].idx++;
  }

  clock_gettime(CLOCK_MONOTONIC, &t1);

  // Finalize the strategy attachment.
  finalize_fn(&ctx);

  // Snapshot the book before tearing it down.
  if(wm_trade_book_snapshot(synth_id, strat_copy, &out->trade) != SUCCESS)
  {
    wm_trade_book_remove(synth_id, strat_copy);

    if(err != NULL)
      snprintf(err, err_cap, "book snapshot failed");
    return(FAIL);
  }

  out->bars_replayed = bars_replayed;
  out->wallclock_ms  = (uint64_t)((int64_t)(t1.tv_sec - t0.tv_sec) * 1000
                     + (int64_t)(t1.tv_nsec - t0.tv_nsec) / 1000000);

  // Drop the synthetic book — its data has been captured in `out->trade`.
  wm_trade_book_remove(synth_id, strat_copy);

  clam(CLAM_INFO, WM_BT_CTX,
      "iter %s/%s: bars=%u trades=%u realized=%+.4f"
      " sharpe=%.3f wallclock_ms=%" PRIu64,
      snap->source_market_id, strat_copy, bars_replayed,
      out->trade.metrics.n_trades, out->trade.metrics.realized_pnl,
      out->trade.metrics.sharpe, out->wallclock_ms);

  return(SUCCESS);
}

// ----------------------------------------------------------------------- //
// Persistence                                                             //
// ----------------------------------------------------------------------- //

bool
wm_backtest_persist_run(const wm_backtest_record_t *rec,
    const char *params_json, const char *metrics_json,
    int64_t *out_run_id)
{
  db_result_t *res = NULL;
  char        *e_strategy   = NULL;
  char        *e_start      = NULL;
  char        *e_end        = NULL;
  char        *e_window_knd = NULL;
  char        *e_params     = NULL;
  char        *e_metrics    = NULL;
  const char  *window_kind;
  uint32_t     n_windows;
  char         sql[2048];
  int          n;
  bool         ok = FAIL;

  if(rec == NULL || metrics_json == NULL)
    return(FAIL);

  // Defaults preserve WM-LT-5 behaviour for callers that don't set
  // these fields (e.g. anything pre-WM-LT-7 that zeros the record).
  window_kind = (rec->window_kind[0] != '\0') ? rec->window_kind : "full";
  n_windows   = (rec->n_windows > 0)          ? rec->n_windows  : 1;

  e_strategy   = db_escape(rec->strategy_name);
  e_start      = db_escape(rec->range_start);
  e_end        = db_escape(rec->range_end);
  e_window_knd = db_escape(window_kind);
  e_metrics    = db_escape(metrics_json);

  if(e_strategy == NULL || e_start == NULL || e_end == NULL ||
     e_window_knd == NULL || e_metrics == NULL)
    goto out;

  if(params_json != NULL)
  {
    e_params = db_escape(params_json);

    if(e_params == NULL)
      goto out;
  }

  // The denormalised scalar columns let /show whenmoon backtest list
  // render without a JSONB round-trip; full metrics live in the JSONB
  // column for the detail view. OOS columns insert as SQL NULL — the
  // post-pass UPDATE (wm_backtest_persist_oos_update) populates them
  // for top-K rows that completed the validation iteration.
  n = snprintf(sql, sizeof(sql),
      "INSERT INTO wm_backtest_run"
      " (market_id, strategy_name, range_start, range_end,"
      "  bars_replayed, n_trades, realized_pnl, final_equity,"
      "  max_drawdown, sharpe, sortino, wallclock_ms,"
      "  window_kind, n_windows,"
      "  params, metrics)"
      " VALUES (%" PRId32 ", '%s', TIMESTAMPTZ '%s', TIMESTAMPTZ '%s',"
      " %u, %u, %.10g, %.10g, %.10g, %.10g, %.10g, %" PRId64 ","
      " '%s', %u,"
      " %s%s%s, '%s'::jsonb)"
      " RETURNING run_id",
      rec->market_id, e_strategy, e_start, e_end,
      rec->bars_replayed, rec->n_trades,
      rec->realized_pnl, rec->final_equity,
      rec->max_drawdown, rec->sharpe, rec->sortino,
      rec->wallclock_ms,
      e_window_knd, n_windows,
      e_params != NULL ? "'" : "NULL",
      e_params != NULL ? e_params : "",
      e_params != NULL ? "'::jsonb" : "",
      e_metrics);

  if(n < 0 || (size_t)n >= sizeof(sql))
  {
    clam(CLAM_WARN, WM_BT_CTX, "persist sql overflow");
    goto out;
  }

  res = db_result_alloc();

  if(res == NULL)
    goto out;

  if(db_query(sql, res) == SUCCESS && res->ok && res->rows == 1)
  {
    const char *s = db_result_get(res, 0, 0);

    if(s != NULL && out_run_id != NULL)
      *out_run_id = (int64_t)strtoll(s, NULL, 10);

    ok = SUCCESS;
  }

  else
  {
    clam(CLAM_WARN, WM_BT_CTX, "wm_backtest_run insert failed: %s",
        (res != NULL && res->error[0] != '\0') ? res->error
                                               : "(no driver error)");
  }

out:
  if(res          != NULL) db_result_free(res);
  if(e_strategy   != NULL) mem_free(e_strategy);
  if(e_start      != NULL) mem_free(e_start);
  if(e_end        != NULL) mem_free(e_end);
  if(e_window_knd != NULL) mem_free(e_window_knd);
  if(e_params     != NULL) mem_free(e_params);
  if(e_metrics    != NULL) mem_free(e_metrics);

  return(ok);
}

bool
wm_backtest_persist_oos_update(int64_t run_id,
    double oos_score, double oos_realized, uint32_t oos_n_trades)
{
  db_result_t *res = NULL;
  char         sql[512];
  int          n;
  bool         ok = FAIL;

  if(run_id <= 0)
    return(FAIL);

  // Top-K OOS validation row: flip window_kind to 'oos' (the head
  // sweep had already inserted under 'oos' too — this is idempotent
  // and matches the post-pass invariant).
  n = snprintf(sql, sizeof(sql),
      "UPDATE wm_backtest_run"
      "   SET oos_score    = %.10g,"
      "       oos_realized = %.10g,"
      "       oos_n_trades = %u,"
      "       window_kind  = 'oos'"
      " WHERE run_id = %" PRId64,
      oos_score, oos_realized, oos_n_trades, run_id);

  if(n < 0 || (size_t)n >= sizeof(sql))
  {
    clam(CLAM_WARN, WM_BT_CTX, "oos update sql overflow");
    return(FAIL);
  }

  res = db_result_alloc();

  if(res == NULL)
    return(FAIL);

  if(db_query(sql, res) == SUCCESS && res->ok)
    ok = SUCCESS;
  else
    clam(CLAM_WARN, WM_BT_CTX, "wm_backtest_run oos update failed: %s",
        (res != NULL && res->error[0] != '\0') ? res->error
                                               : "(no driver error)");

  db_result_free(res);
  return(ok);
}

// Common SELECT projection used by both list + detail.
#define WM_BT_SELECT_COLS                                              \
    "run_id, market_id, strategy_name,"                                \
    " to_char(range_start, 'YYYY-MM-DD HH24:MI:SS+00') AS range_start," \
    " to_char(range_end,   'YYYY-MM-DD HH24:MI:SS+00') AS range_end,"  \
    " bars_replayed, n_trades, realized_pnl, final_equity,"            \
    " max_drawdown, sharpe, sortino, wallclock_ms,"                    \
    " window_kind, n_windows, oos_score, oos_realized, oos_n_trades,"  \
    " to_char(created_at,  'YYYY-MM-DD HH24:MI:SS+00') AS created_at"

static void
wm_bt_record_from_row(const db_result_t *res, uint32_t row,
    wm_backtest_record_t *out)
{
  const char *s;
  size_t      slen;

  memset(out, 0, sizeof(*out));

  s = db_result_get(res, row, 0);
  if(s != NULL) out->run_id    = (int64_t)strtoll(s, NULL, 10);

  s = db_result_get(res, row, 1);
  if(s != NULL) out->market_id = (int32_t)strtol(s, NULL, 10);

  s = db_result_get(res, row, 2);
  if(s != NULL)
  {
    slen = strnlen(s, sizeof(out->strategy_name) - 1);
    memcpy(out->strategy_name, s, slen);
    out->strategy_name[slen] = '\0';
  }

  s = db_result_get(res, row, 3);
  if(s != NULL)
  {
    slen = strnlen(s, sizeof(out->range_start) - 1);
    memcpy(out->range_start, s, slen);
    out->range_start[slen] = '\0';
  }

  s = db_result_get(res, row, 4);
  if(s != NULL)
  {
    slen = strnlen(s, sizeof(out->range_end) - 1);
    memcpy(out->range_end, s, slen);
    out->range_end[slen] = '\0';
  }

  s = db_result_get(res, row, 5);
  if(s != NULL) out->bars_replayed = (uint32_t)strtoul(s, NULL, 10);

  s = db_result_get(res, row, 6);
  if(s != NULL) out->n_trades = (uint32_t)strtoul(s, NULL, 10);

  s = db_result_get(res, row, 7);
  if(s != NULL) out->realized_pnl = strtod(s, NULL);

  s = db_result_get(res, row, 8);
  if(s != NULL) out->final_equity = strtod(s, NULL);

  s = db_result_get(res, row, 9);
  if(s != NULL) out->max_drawdown = strtod(s, NULL);

  s = db_result_get(res, row, 10);
  if(s != NULL) out->sharpe = strtod(s, NULL);

  s = db_result_get(res, row, 11);
  if(s != NULL) out->sortino = strtod(s, NULL);

  s = db_result_get(res, row, 12);
  if(s != NULL) out->wallclock_ms = (int64_t)strtoll(s, NULL, 10);

  s = db_result_get(res, row, 13);
  if(s != NULL)
  {
    slen = strnlen(s, sizeof(out->window_kind) - 1);
    memcpy(out->window_kind, s, slen);
    out->window_kind[slen] = '\0';
  }

  s = db_result_get(res, row, 14);
  if(s != NULL) out->n_windows = (uint32_t)strtoul(s, NULL, 10);

  s = db_result_get(res, row, 15);
  if(s != NULL)
  {
    out->oos_score = strtod(s, NULL);
    out->have_oos  = true;
  }

  s = db_result_get(res, row, 16);
  if(s != NULL) out->oos_realized = strtod(s, NULL);

  s = db_result_get(res, row, 17);
  if(s != NULL) out->oos_n_trades = (uint32_t)strtoul(s, NULL, 10);

  s = db_result_get(res, row, 18);
  if(s != NULL)
  {
    slen = strnlen(s, sizeof(out->created_at) - 1);
    memcpy(out->created_at, s, slen);
    out->created_at[slen] = '\0';
  }
}

uint32_t
wm_backtest_recent_runs(wm_backtest_record_t *out, uint32_t cap)
{
  db_result_t *res;
  char         sql[1024];
  uint32_t     n_out = 0;
  uint32_t     i;
  int          n;

  if(out == NULL || cap == 0)
    return(0);

  res = db_result_alloc();

  if(res == NULL)
    return(0);

  n = snprintf(sql, sizeof(sql),
      "SELECT " WM_BT_SELECT_COLS
      "  FROM wm_backtest_run"
      " ORDER BY run_id DESC"
      " LIMIT %u",
      cap);

  if(n < 0 || (size_t)n >= sizeof(sql))
  {
    db_result_free(res);
    return(0);
  }

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    db_result_free(res);
    return(0);
  }

  for(i = 0; i < res->rows && n_out < cap; i++)
    wm_bt_record_from_row(res, i, &out[n_out++]);

  db_result_free(res);
  return(n_out);
}

// Heap-allocate a copy of one cell. Returns NULL when the cell is
// SQL NULL or on OOM.
static char *
wm_bt_dup_cell(const db_result_t *res, uint32_t row, uint32_t col)
{
  const char *src;
  size_t      n;
  char       *out;

  src = db_result_get(res, row, col);

  if(src == NULL)
    return(NULL);

  n   = strlen(src) + 1;
  out = mem_alloc("whenmoon.backtest", "json_cell", n);

  if(out == NULL)
    return(NULL);

  memcpy(out, src, n);
  return(out);
}

bool
wm_backtest_lookup_run(int64_t run_id, wm_backtest_record_t *out,
    char **out_metrics_json, char **out_params_json)
{
  db_result_t *res;
  char         sql[1024];
  bool         ok = FAIL;
  int          n;

  if(out == NULL)
    return(FAIL);

  if(out_metrics_json != NULL) *out_metrics_json = NULL;
  if(out_params_json  != NULL) *out_params_json  = NULL;

  res = db_result_alloc();

  if(res == NULL)
    return(FAIL);

  n = snprintf(sql, sizeof(sql),
      "SELECT " WM_BT_SELECT_COLS ", metrics::text, params::text"
      "  FROM wm_backtest_run"
      " WHERE run_id = %" PRId64,
      run_id);

  if(n < 0 || (size_t)n >= sizeof(sql))
  {
    db_result_free(res);
    return(FAIL);
  }

  if(db_query(sql, res) == SUCCESS && res->ok && res->rows == 1)
  {
    wm_bt_record_from_row(res, 0, out);

    if(out_metrics_json != NULL)
      *out_metrics_json = wm_bt_dup_cell(res, 0, 19);

    if(out_params_json != NULL)
      *out_params_json = wm_bt_dup_cell(res, 0, 20);

    ok = SUCCESS;
  }

  db_result_free(res);
  return(ok);
}

// ----------------------------------------------------------------------- //
// Walk-forward + OOS windowing helpers                                    //
// ----------------------------------------------------------------------- //
//
// Walk-forward: window i has train range [start + i*step, start + i*step
// + train], test range [train_end, train_end + test]. A window is
// admitted only if its test range fits inside the snapshot's range
// AND covers at least WM_BT_WALK_MIN_TEST_BARS 1m bars.
//
// 1m bars in a window = floor(window_duration / 60s). The 60-second
// minimum is conservative — a partial bar at the boundary contributes
// no real signal.

static const int64_t WM_BT_DAY_MS = 86400LL * 1000LL;
static const int64_t WM_BT_MIN_MS = 60LL    * 1000LL;

bool
wm_bt_walk_build_windows(const wm_bt_walk_spec_t *spec,
    int64_t range_start_ms, int64_t range_end_ms,
    const loaded_strategy_t *ls,
    wm_bt_window_set_t *out, char *err, size_t err_cap)
{
  int64_t  train_ms;
  int64_t  test_ms;
  int64_t  step_ms;
  int64_t  cur;
  uint32_t n = 0;
  uint32_t bars_per_test;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(spec == NULL || out == NULL || range_end_ms <= range_start_ms)
  {
    if(err != NULL)
      snprintf(err, err_cap, "bad walk-forward inputs");
    return(FAIL);
  }

  if(spec->train_days == 0 || spec->test_days == 0 || spec->step_days == 0)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "walk-forward train/test/step all must be > 0");
    return(FAIL);
  }

  train_ms = (int64_t)spec->train_days * WM_BT_DAY_MS;
  test_ms  = (int64_t)spec->test_days  * WM_BT_DAY_MS;
  step_ms  = (int64_t)spec->step_days  * WM_BT_DAY_MS;

  // Strategy min_history pre-flight. The 1d grain is the most
  // restrictive; convert each grain's required bars back to days
  // worth of training. WM-LT-7 v1 only enforces against subscribed
  // grains — un-subscribed grains have no min_history requirement.
  if(ls != NULL)
  {
    static const int64_t bar_ms[WM_GRAN_MAX] = {
      [WM_GRAN_1M]   = 60LL    * 1000LL,
      [WM_GRAN_5M]   = 300LL   * 1000LL,
      [WM_GRAN_15M]  = 900LL   * 1000LL,
      [WM_GRAN_1H]   = 3600LL  * 1000LL,
      [WM_GRAN_6H]   = 21600LL * 1000LL,
      [WM_GRAN_1D]   = 86400LL * 1000LL,
    };

    uint32_t g;

    for(g = 0; g < WM_GRAN_MAX; g++)
    {
      int64_t  needed_ms;
      uint32_t needed_days;

      if((ls->meta.grains_mask & (uint16_t)(1u << g)) == 0)
        continue;

      if(ls->meta.min_history[g] == 0)
        continue;

      needed_ms   = (int64_t)ls->meta.min_history[g] * bar_ms[g];
      needed_days = (uint32_t)((needed_ms + WM_BT_DAY_MS - 1) / WM_BT_DAY_MS);

      if(spec->train_days < needed_days)
      {
        if(err != NULL)
          snprintf(err, err_cap,
              "train=%ud insufficient: strategy needs %u %s bars"
              " (~%ud)", spec->train_days,
              ls->meta.min_history[g],
              g == WM_GRAN_1M  ? "1m"  : g == WM_GRAN_5M  ? "5m"  :
              g == WM_GRAN_15M ? "15m" : g == WM_GRAN_1H  ? "1h"  :
              g == WM_GRAN_6H  ? "6h"  : "1d",
              needed_days);
        return(FAIL);
      }
    }
  }

  bars_per_test = (uint32_t)(test_ms / WM_BT_MIN_MS);

  if(bars_per_test < WM_BT_WALK_MIN_TEST_BARS)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "test window too short: %ud yields %u 1m bars (min %u)",
          spec->test_days, bars_per_test,
          (unsigned)WM_BT_WALK_MIN_TEST_BARS);
    return(FAIL);
  }

  // Walk: each iter builds [train_start, train_end] then
  // [train_end, train_end + test], advancing by step until the next
  // test would extend past range_end.
  cur = range_start_ms;

  while(n < WM_BT_WALK_MAX_WINDOWS)
  {
    int64_t train_start = cur;
    int64_t train_end   = train_start + train_ms;
    int64_t test_start  = train_end;
    int64_t test_end    = test_start + test_ms;

    // Last partial window: clip test_end against range_end and drop
    // if the clipped window contains < MIN_TEST_BARS.
    if(test_end > range_end_ms)
      test_end = range_end_ms;

    if(test_end <= test_start)
      break;

    if((uint32_t)((test_end - test_start) / WM_BT_MIN_MS)
       < WM_BT_WALK_MIN_TEST_BARS)
      break;

    if(train_end > range_end_ms)
      break;

    out->windows[n].start_ts_ms = test_start;
    out->windows[n].end_ts_ms   = test_end;
    n++;

    cur += step_ms;

    // Sanity: if step is so small the next window's test range
    // overlaps the previous one's end heavily, we still admit it —
    // overlapping test ranges are a deliberate walk-forward design
    // choice (when step < test). The cap handles the runaway case.
  }

  if(n == 0)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "walk-forward produced no windows; range too short for"
          " train=%ud test=%ud step=%ud",
          spec->train_days, spec->test_days, spec->step_days);
    return(FAIL);
  }

  if(n >= WM_BT_WALK_MAX_WINDOWS)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "walk-forward window count cap (%u) hit; raise step or"
          " shorten range", (unsigned)WM_BT_WALK_MAX_WINDOWS);
    return(FAIL);
  }

  out->n = n;
  return(SUCCESS);
}

bool
wm_bt_oos_split_range(const wm_bt_oos_spec_t *spec,
    int64_t range_start_ms, int64_t range_end_ms,
    wm_bt_window_t *out_head, wm_bt_window_t *out_tail,
    char *err, size_t err_cap)
{
  int64_t  range_ms;
  int64_t  tail_ms;
  int64_t  split_ms;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(spec == NULL || out_head == NULL || out_tail == NULL ||
     range_end_ms <= range_start_ms)
  {
    if(err != NULL)
      snprintf(err, err_cap, "bad oos-tail inputs");
    return(FAIL);
  }

  if(spec->pct == 0 || spec->pct > 50)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "oos-tail %u%% out of range (expected 1..50)", spec->pct);
    return(FAIL);
  }

  range_ms = range_end_ms - range_start_ms;
  tail_ms  = range_ms * (int64_t)spec->pct / 100;
  split_ms = range_end_ms - tail_ms;

  if(split_ms <= range_start_ms || split_ms >= range_end_ms)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "oos-tail %u%% leaves zero-duration head or tail", spec->pct);
    return(FAIL);
  }

  out_head->start_ts_ms = range_start_ms;
  out_head->end_ts_ms   = split_ms;
  out_tail->start_ts_ms = split_ms;
  out_tail->end_ts_ms   = range_end_ms;

  return(SUCCESS);
}
