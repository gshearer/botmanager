// botmanager — MIT
// Whenmoon backtest snapshot + single-iteration replay.
//
// One backtest run = (a) pre-flight gap check on the 1m candle
// coverage; (b) snapshot construction by replaying 1m candles from
// `wm_candles_<id>` through a dedicated aggregator with strategy
// fanout disabled; (c) one iteration that walks the snapshot bars
// chronologically across every grain the strategy subscribes to,
// firing wm_strategy_on_bar through a per-iteration synthetic market
// (synthetic id "bt:<n>") in PAPER mode. WM-BT-1 retired the
// wm_backtest_run DB persistence surface; disk-based persistence
// lands in WM-BT-6.
//
// The reuse story (post WM-MK-5):
//   * Aggregator + indicator pass: same code path as live. The flag
//     `dispatch_strategies` flips fanout off during warmup.
//   * Per-iteration synthetic market: a heap-owned `whenmoon_market_t`
//     that shares the snapshot's grain rings + product id but carries
//     a fresh session, mutex, and PAPER mode. Strategy emit routes
//     through `wm_market_engine_on_signal_with_mk(ctx->mkt, ...)` —
//     the same fill engine production paper trading uses.
//   * Sweep parallelism: each worker creates its own synth market
//     per iteration. No shared registry, no cross-thread contention.

#define WHENMOON_INTERNAL
#include "backtest.h"

#include "aggregator.h"
#include "dl_coverage.h"
#include "dl_schema.h"
#include "market.h"
#include "strategy.h"
#include "whenmoon.h"
#include "wm_bt_file.h"

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

// A market's candle history legitimately has holes, and not just
// scattered ones: early, illiquid history (e.g. Coinbase BTC-USD in
// Jan-2015) can go *days* without printing a minute, and re-running
// /whenmoon download can never conjure trades the exchange never had.
// Gap size is therefore the wrong thing to gate on — a 38h hole at
// the dawn of a market is as unfillable as a 2-minute one. The
// snapshot builder replays around holes regardless, so the only
// genuine error this pre-flight needs to catch is asking to compile a
// window that holds *no* rows at all (a typo'd market, or a range
// entirely before the data exists), which would yield an empty
// snapshot.
//
// wm_gap_largest_missing reports the whole requested window as the
// single gap exactly when the range is empty; any present row splits
// that window, so the widest gap can only equal the full span when
// there is nothing there. That equality is the empty-range signal.
bool
wm_backtest_preflight_gap(int32_t market_id_db,
    const char *market_id_str,
    const char *range_start, const char *range_end,
    char *err, size_t err_cap)
{
  wm_coverage_t  gap;
  time_t         rs;
  time_t         re;
  time_t         gs;
  time_t         ge;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(market_id_db < 0 || range_start == NULL || range_end == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "bad pre-flight inputs");
    return(FAIL);
  }

  // Authoritative check: walk the actual candle rows, not the
  // coverage-attempt store. The two can disagree — rows can be present
  // while the attempt store is empty/stale — and the rows are what the
  // snapshot builder actually replays. No gap at all → fully covered.
  if(wm_gap_largest_missing(market_id_db, range_start, range_end,
         &gap) == 0)
    return(SUCCESS);

  // If any timestamp fails to parse, fail open rather than block a
  // compile on a formatting quirk.
  if(wm_bt_parse_ts(range_start,  &rs) != SUCCESS ||
     wm_bt_parse_ts(range_end,    &re) != SUCCESS ||
     wm_bt_parse_ts(gap.first_ts, &gs) != SUCCESS ||
     wm_bt_parse_ts(gap.last_ts,  &ge) != SUCCESS)
    return(SUCCESS);

  // Some rows present (the widest gap is a strict sub-window) → the
  // holes are real-but-tolerable; let the compile proceed.
  if((int64_t)(ge - gs) < (int64_t)(re - rs))
    return(SUCCESS);

  // Whole requested window is empty — nothing to compile.
  if(err != NULL)
  {
    char start_date[16];
    char end_date[16];

    // YYYY-MM-DD slice; wm_dl_parse_date accepts this ISO form (as
    // well as MM/dd/yyyy), so the suggested command parses as printed.
    snprintf(start_date, sizeof(start_date), "%.10s", gap.first_ts);
    snprintf(end_date,   sizeof(end_date),   "%.10s", gap.last_ts);

    snprintf(err, err_cap,
        "no 1m candles in %s..%s; run"
        " /whenmoon download %s %s %s",
        gap.first_ts, gap.last_ts,
        market_id_str != NULL ? market_id_str : "<market_id>",
        start_date, end_date);
  }

  return(FAIL);
}

// ----------------------------------------------------------------------- //
// Snapshot construction                                                   //
// ----------------------------------------------------------------------- //

// Free the rings + aggregator + lock owned by the stub market in
// `snap`. Only invoked on snapshots whose mutex_init + aggregator_init
// both succeeded (early-failure paths in build clean up directly).
//
// WM-BT-2: mmap-loaded snapshots (`is_mapped == true`) borrow their
// grain rings from the mapped region; the close path lives in
// wm_bt_file_close which munmap's, closes the fd, and frees the
// struct. Heap-built snapshots own everything and tear down inline.
static void
wm_bt_snapshot_teardown(wm_backtest_snapshot_t *snap)
{
  if(snap == NULL)
    return;

  if(snap->is_mapped)
  {
    wm_bt_file_close(snap);
    return;
  }

  wm_aggregator_destroy(&snap->mkt);
  pthread_mutex_destroy(&snap->mkt.lock);
  mem_free(snap);
}

void
wm_backtest_snapshot_free(wm_backtest_snapshot_t *snap)
{
  wm_bt_snapshot_teardown(snap);
}

// Per-row streaming context for wm_backtest_snapshot_build. The callback
// runs synchronously on the build thread with snap->mkt.lock held.
typedef struct
{
  whenmoon_market_t *mkt;
  uint32_t           replayed;
} wm_bt_stream_ctx_t;

// Per-row sink for the streamed candle history (DB-STREAM-1). Cols
// 0..5 = ts_ms, low, high, open, close, volume. Mirrors the old
// materialized loop body; caller holds snap->mkt.lock across the stream.
static bool
wm_bt_snapshot_row_cb(uint32_t row, uint32_t cols,
    const char *const *values, void *data)
{
  wm_bt_stream_ctx_t *ctx = data;
  wm_candle_full_t    bar;
  int64_t             ts_open_ms;

  (void)row;

  if(cols < 6 ||
     values[0] == NULL || values[1] == NULL || values[2] == NULL ||
     values[3] == NULL || values[4] == NULL || values[5] == NULL)
    return(true);   // skip malformed / NULL row, keep streaming

  ts_open_ms = (int64_t)strtoll(values[0], NULL, 10);

  memset(&bar, 0, sizeof(bar));
  bar.ts_close_ms = ts_open_ms + 60000;
  bar.low         = strtod(values[1], NULL);
  bar.high        = strtod(values[2], NULL);
  bar.open        = strtod(values[3], NULL);
  bar.close       = strtod(values[4], NULL);
  bar.volume      = strtod(values[5], NULL);

  wm_aggregator_replay_bar(ctx->mkt, WM_GRAN_1M, &bar);
  ctx->replayed++;
  return(true);
}

wm_backtest_snapshot_t *
wm_backtest_snapshot_build(int32_t market_id_db,
    const char *source_market_id,
    const char *range_start, const char *range_end,
    uint32_t min_history_1d,
    char *err, size_t err_cap)
{
  wm_backtest_snapshot_t *snap = NULL;
  char                    table[WM_DL_TABLE_SZ];
  char                   *e_start = NULL;
  char                   *e_end   = NULL;
  char                    sql[1024];
  uint32_t                history_days;
  uint32_t                replayed = 0;
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

  if(wm_candle_table_name(market_id_db, table, sizeof(table)) != SUCCESS)
  {
    if(err != NULL)
      snprintf(err, err_cap, "candle table name overflow");
    return(NULL);
  }

  // Idempotent — the table may not exist if no candles have ever been
  // downloaded (the pre-flight should have caught this, but stay
  // defensive).
  if(wm_candle_table_ensure(market_id_db) != SUCCESS)
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

  // WM-BT-2: heap-built snapshot — not file-backed. map_fd is -1 to
  // mirror the closed-fd sentinel used by wm_bt_file_open's failure
  // paths; range_*_ms stay 0 (heap path uses range_start/range_end
  // strings; WM-BT-3 will fill these in when the compile verb derives
  // them from the warmup query).
  snap->map_base       = NULL;
  snap->map_size       = 0;
  snap->map_fd         = -1;
  snap->is_mapped      = false;
  snap->range_start_ms = 0;
  snap->range_end_ms   = 0;

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

  // Stream the candle history row-by-row - never materialize the whole
  // result (DB-STREAM-1: a full-history db_result_t storms mem_mutex and
  // wedges the daemon). snap->mkt.lock is private to this unpublished
  // stub market, so holding it across the streamed read is uncontended.
  {
    wm_bt_stream_ctx_t sctx = { .mkt = &snap->mkt, .replayed = 0 };
    bool               ok;

    pthread_mutex_lock(&snap->mkt.lock);
    ok = db_query_stream(sql, wm_bt_snapshot_row_cb, &sctx, err, err_cap);
    pthread_mutex_unlock(&snap->mkt.lock);

    if(ok != SUCCESS)
      goto fail;   // err already filled by db_query_stream

    if(sctx.replayed == 0)
    {
      if(err != NULL)
        snprintf(err, err_cap,
            "no 1m candles in %s..%s; run"
            " /whenmoon download <market> first",
            range_start, range_end);
      goto fail;
    }

    replayed = sctx.replayed;
  }

  snap->bars_loaded_1m = replayed;

  clam(CLAM_INFO, WM_BT_CTX,
      "snapshot built: %s [%s..%s] 1m_bars=%u history=%u days"
      " (5m=%u 15m=%u 1h=%u 4h=%u 1d=%u)",
      source_market_id, range_start, range_end,
      replayed, history_days,
      snap->mkt.grain_n[WM_GRAN_5M], snap->mkt.grain_n[WM_GRAN_15M],
      snap->mkt.grain_n[WM_GRAN_1H], snap->mkt.grain_n[WM_GRAN_4H],
      snap->mkt.grain_n[WM_GRAN_1D]);

  mem_free(e_start);
  mem_free(e_end);

  return(snap);

fail:
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

// Apply CLI param overrides directly to the synth-market session.
// Each `have_*` flag selects whether the matching field is
// overwritten. starting_cash also re-seeds PAPER-mode cash so the
// iteration begins from the override.
static void
wm_market_session_apply_iter_overrides(wm_market_session_t *s,
    const wm_backtest_params_t *params)
{
  if(s == NULL || params == NULL)
    return;

  if(params->have_fee_bps)
    s->fee_bps  = params->fee_bps;

  if(params->have_slip_bps)
    s->slip_bps = params->slip_bps;

  if(params->have_size_frac)
    s->size_frac = params->size_frac;

  if(params->have_starting_cash)
  {
    s->stats[WM_MARKET_MODE_PAPER].starting_cash = params->starting_cash;
    s->stats[WM_MARKET_MODE_PAPER].cash          = params->starting_cash;
  }
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
wm_backtest_run_iteration_multi(whenmoon_state_t *st,
    wm_backtest_snapshot_t *snap,
    const char *const *strategy_names, uint32_t n_strats,
    const char *synth_id,
    const wm_backtest_params_t *params,
    const wm_bt_window_t *windows, uint32_t n_windows,
    wm_backtest_result_t *out,
    char *err, size_t err_cap)
{
  // Per-strategy resolved entry points + identity. Priority order ==
  // array order: strategy_names[0] is polled first (lowest priority
  // number in the live model), mirroring wm_strategy_dispatch_bar.
  int      (*init_fn[WM_BT_MAX_LINKED])(wm_strategy_ctx_t *);
  void     (*finalize_fn[WM_BT_MAX_LINKED])(wm_strategy_ctx_t *);
  void     (*on_bar_fn[WM_BT_MAX_LINKED])(wm_strategy_ctx_t *,
               const struct whenmoon_market *,
               wm_gran_t,
               const wm_candle_full_t *);
  uint16_t                        smask[WM_BT_MAX_LINKED];
  char                            sname[WM_BT_MAX_LINKED][WM_STRATEGY_NAME_SZ];
  wm_strategy_ctx_t               ctx[WM_BT_MAX_LINKED];
  bool                            inited[WM_BT_MAX_LINKED] = { false };
  char                            strat_label[WM_STRATEGY_NAME_SZ
                                      * WM_BT_MAX_LINKED + WM_BT_MAX_LINKED]
                                      = {0};
  uint16_t                        union_mask               = 0;
  uint32_t                        si;
  whenmoon_market_t              *synth_mk = NULL;
  char                            err_synth[128];
  wm_bt_cursor_t                  cursors[WM_GRAN_MAX];
  const wm_candle_full_t         *rings[WM_GRAN_MAX];
  uint32_t                        bars_replayed = 0;
  uint64_t                        fills_paper;
  double                          realized_paper;
  struct timespec                 t0, t1;
  wm_market_session_t            *sess;
  wm_market_fill_t               *acc_fills;
  uint32_t                        acc_n;
  uint32_t                        acc_cap;
  uint64_t                        prev_fn;
  const wm_market_stats_t        *ps;
  double                          win_rate;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(out != NULL)
    memset(out, 0, sizeof(*out));

  if(st == NULL || snap == NULL || strategy_names == NULL ||
     n_strats == 0 || n_strats > WM_BT_MAX_LINKED ||
     synth_id == NULL || synth_id[0] == '\0' || out == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "bad iteration inputs");
    return(FAIL);
  }

  // Resolve every linked strategy + cache its function pointers under the
  // registry lock, then release. The function pointers are stable for
  // the lifetime of the loaded_strategy_t — a reload would invalidate
  // them; sweep callers gate reload via the active-counter in sweep.c.
  pthread_mutex_lock(&st->strategies->lock);

  for(si = 0; si < n_strats; si++)
  {
    loaded_strategy_t *ls;

    if(strategy_names[si] == NULL)
    {
      pthread_mutex_unlock(&st->strategies->lock);

      if(err != NULL)
        snprintf(err, err_cap, "null strategy name (#%u)", si);
      return(FAIL);
    }

    ls = wm_strategy_find_loaded(st, strategy_names[si]);

    if(ls == NULL || ls->init_fn == NULL || ls->finalize_fn == NULL ||
       ls->on_bar_fn == NULL)
    {
      pthread_mutex_unlock(&st->strategies->lock);

      if(err != NULL)
        snprintf(err, err_cap, "strategy %s not loaded", strategy_names[si]);
      return(FAIL);
    }

    init_fn[si]     = ls->init_fn;
    finalize_fn[si] = ls->finalize_fn;
    on_bar_fn[si]   = ls->on_bar_fn;
    smask[si]       = ls->meta.grains_mask;
    union_mask     |= ls->meta.grains_mask;

    snprintf(sname[si], sizeof(sname[si]), "%s", ls->name);
  }

  pthread_mutex_unlock(&st->strategies->lock);

  // Build the "a+b" label for the summary log line. The snprintf return
  // value drives the offset so truncation is handled explicitly (and
  // -Wformat-truncation stays quiet); the buffer is sized for the full
  // worst-case concatenation regardless.
  {
    size_t off = 0;

    for(si = 0; si < n_strats && off < sizeof(strat_label); si++)
    {
      int w = snprintf(strat_label + off, sizeof(strat_label) - off,
          "%s%.*s", si == 0 ? "" : "+",
          (int)WM_STRATEGY_NAME_SZ - 1, sname[si]);

      if(w < 0)
        break;

      off += (size_t)w;
    }
  }

  // Build the per-iteration synthetic market. It shares the snapshot's
  // grain rings + product id but carries an independent session and
  // mutex; emit_signal_impl routes signals through
  // wm_market_engine_on_signal_with_mk(synth_mk, ...). EVERY linked
  // strategy shares this one market — the market owns the single
  // position, strategies are advisors (whenmoon_market_model.md).
  if(wm_market_create_synthetic(synth_id, &snap->mkt, &synth_mk,
         err_synth, sizeof(err_synth)) != SUCCESS)
  {
    if(err != NULL)
      snprintf(err, err_cap, "synth market: %s",
          err_synth[0] != '\0' ? err_synth : "(no detail)");
    return(FAIL);
  }

  wm_market_session_apply_iter_overrides(&synth_mk->session, params);

  // Build + init each strategy ctx. All point mkt at the shared synth
  // market so every emit lands on the one book. A failed init unwinds the
  // already-inited contexts before tearing the market down.
  for(si = 0; si < n_strats; si++)
  {
    memset(&ctx[si], 0, sizeof(ctx[si]));
    snprintf(ctx[si].market_id_str, sizeof(ctx[si].market_id_str), "%s",
        synth_id);
    // Precision-capped: reading from a 2D-array row loses the per-row
    // length bound, so cap explicitly to keep -Wformat-truncation quiet.
    snprintf(ctx[si].strategy_name, sizeof(ctx[si].strategy_name), "%.*s",
        (int)sizeof(ctx[si].strategy_name) - 1, sname[si]);
    ctx[si].mkt = synth_mk;

    if(init_fn[si](&ctx[si]) != 0)
    {
      uint32_t j;

      for(j = 0; j < si; j++)
        finalize_fn[j](&ctx[j]);

      wm_market_destroy_synthetic(synth_mk);

      if(err != NULL)
        snprintf(err, err_cap, "strategy %s init returned non-zero",
            sname[si]);
      return(FAIL);
    }

    inited[si] = true;
  }

  // Set up cursors over each grain ring. A grain is walked if ANY linked
  // strategy subscribes to it (union mask); per-bar each strategy is then
  // polled only for the grains it individually subscribes to.
  {
    uint32_t g;

    for(g = 0; g < WM_GRAN_MAX; g++)
    {
      cursors[g].idx        = 0;
      cursors[g].cap        = snap->mkt.grain_n[g];
      cursors[g].subscribed = (union_mask & (uint16_t)(1u << g)) != 0;
      rings[g]              = snap->mkt.grain_arr[g];
    }
  }

  // Lossless fill capture for the chart emitter. The session's PAPER
  // fills ring only retains the last WM_MARKET_FILL_RING_CAP (256)
  // fills, so reading it once at the end silently drops the oldest
  // trades from the charts on any run with > 256 fills. Instead we
  // drain new fills into this growable buffer right after each
  // callback, before the ring can wrap. The engine records <= 1 fill
  // per on_bar, so this loses nothing. acc_fills stays NULL (and
  // acc_n 0) when the iteration trades nothing — the caller's free
  // path guards NULL.
  sess      = &synth_mk->session;
  acc_fills = NULL;
  acc_n     = 0;
  acc_cap   = 0;
  prev_fn   = sess->fills_n[WM_MARKET_MODE_PAPER];

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
    // but the strategy never sees them, so the synth market records
    // fills only during windowed ranges.
    if(cursors[g].subscribed)
    {
      bool in_window = (n_windows == 0) ||
          wm_bt_in_any_window(windows, n_windows, bar->ts_close_ms);

      if(in_window)
      {
        bool acted = false;

        // Priority-walk advisor dispatch, bit-identical to
        // wm_strategy_dispatch_bar: poll the linked strategies in array
        // (priority) order, but only those subscribing to THIS grain;
        // the FIRST to emit a non-zero signal wins and the remaining
        // strategies are not polled for this bar. Higher grains (4h/1d)
        // never emit (they only cache regime context), so every
        // subscriber there runs; the 1h decision grain is where the
        // break actually bites. A strategy tracks its own internal
        // position, so on a bar it does not run it simply keeps its
        // prior state — exactly as in the live walk.
        for(si = 0; si < n_strats && !acted; si++)
        {
          uint64_t pre_emitted;

          if((smask[si] & (uint16_t)(1u << g)) == 0)
            continue;

          ctx[si].bars_seen++;
          ctx[si].last_bar_ts_ms = bar->ts_close_ms;
          ctx[si].last_mark_px   = bar->close;
          ctx[si].last_mark_ms   = bar->ts_close_ms;

          pre_emitted = ctx[si].signals_emitted;
          on_bar_fn[si](&ctx[si], &snap->mkt, g, bar);

          if(ctx[si].signals_emitted > pre_emitted &&
             ctx[si].last_signal.score != 0.0)
            acted = true;
        }

        bars_replayed++;

        // Drain the (<= 1) fill this bar produced into the lossless
        // accumulator before the 256-slot ring can overwrite it. Only
        // the single winning advisor acts, so the engine adds at most one
        // fill per bar; the while loop is defensive.
        {
          uint64_t now_fn = sess->fills_n[WM_MARKET_MODE_PAPER];

          while(prev_fn < now_fn)
          {
            uint32_t back = (uint32_t)(now_fn - prev_fn);
            uint32_t ridx = (sess->fills_head[WM_MARKET_MODE_PAPER]
                             + WM_MARKET_FILL_RING_CAP - back)
                            % WM_MARKET_FILL_RING_CAP;

            if(acc_n == acc_cap)
            {
              uint32_t newcap = acc_cap ? acc_cap * 2u : 256u;

              acc_fills = (acc_cap == 0)
                  ? mem_alloc("whenmoon.backtest", "iter_fills",
                        sizeof(*acc_fills) * (size_t)newcap)
                  : mem_realloc(acc_fills,
                        sizeof(*acc_fills) * (size_t)newcap);
              acc_cap = newcap;
            }

            acc_fills[acc_n++] = sess->fills[WM_MARKET_MODE_PAPER][ridx];
            prev_fn++;
          }
        }
      }
    }

    cursors[g].idx++;
  }

  clock_gettime(CLOCK_MONOTONIC, &t1);

  // Finalize every linked strategy attachment.
  for(si = 0; si < n_strats; si++)
    if(inited[si])
      finalize_fn[si](&ctx[si]);

  // Snapshot the synth market's session before tearing it down.
  if(wm_market_session_snapshot(synth_mk, &out->trade) != SUCCESS)
  {
    if(acc_fills != NULL)
      mem_free(acc_fills);

    wm_market_destroy_synthetic(synth_mk);

    if(err != NULL)
      snprintf(err, err_cap, "synth snapshot failed");
    return(FAIL);
  }

  // Hand the lossless fill accumulator (every PAPER fill, oldest-to-
  // newest) to the result for the chart emitter. NULL/0 when the
  // iteration traded nothing; the caller's free path guards NULL.
  out->fills   = acc_fills;
  out->n_fills = acc_n;

  fills_paper    =
      out->trade.stats[WM_MARKET_MODE_PAPER].lifetime_fills_count;
  realized_paper =
      out->trade.stats[WM_MARKET_MODE_PAPER].realized_pnl_lifetime;

  out->bars_replayed = bars_replayed;
  out->wallclock_ms  = (uint64_t)((int64_t)(t1.tv_sec - t0.tv_sec) * 1000
                     + (int64_t)(t1.tv_nsec - t0.tv_nsec) / 1000000);

  wm_market_destroy_synthetic(synth_mk);

  // One summary line per backtest. Counters come from the PAPER ledger
  // (the synth market trades paper-only). win_rate is the hit rate over
  // closed round-trip trades (n_wins + n_losses == n_trades). Currency
  // is realized-only: end == start + profit exactly, so any position
  // still open at the final bar contributes no unrealized PnL here.
  // data_days reports the span of 1m history loaded into the snapshot
  // (1440 1m bars = one day).
  ps       = &out->trade.stats[WM_MARKET_MODE_PAPER];
  win_rate = ps->n_trades > 0
      ? (double)ps->n_wins / (double)ps->n_trades * 100.0 : 0.0;

  clam(CLAM_INFO, WM_BT_CTX,
      "backtest %s/%s: trades=%u wins=%u losses=%u win_rate=%.1f%%"
      " start=%.2f end=%.2f profit=%+.2f data_days=%.1f"
      " bars=%u fills=%" PRIu64 " wallclock_ms=%" PRIu64,
      snap->source_market_id, strat_label,
      ps->n_trades, ps->n_wins, ps->n_losses, win_rate,
      ps->starting_cash, ps->starting_cash + realized_paper,
      realized_paper, (double)snap->bars_loaded_1m / 1440.0,
      bars_replayed, fills_paper, out->wallclock_ms);

  return(SUCCESS);
}

// Thin n==1 wrapper so every existing single-strategy caller (the sweep
// worker, OOS + walk-forward layers) runs the identical engine path as a
// linked run and stays directly comparable to one.
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
  const char *names[1] = { strategy_name };

  return(wm_backtest_run_iteration_multi(st, snap, names, 1u, synth_id,
      params, windows, n_windows, out, err, err_cap));
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
      [WM_GRAN_4H]   = 14400LL * 1000LL,
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
              g == WM_GRAN_4H  ? "4h"  : "1d",
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
