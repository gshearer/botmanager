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
#include "market_engine.h"
#include "strategy.h"
#include "whenmoon.h"
#include "wm_bt_file.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "db.h"

#include <inttypes.h>
#include <math.h>
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

// ----------------------------------------------------------------------- //
// WM-RIGOR-4: daily mark-to-market tracker                                //
// ----------------------------------------------------------------------- //
//
// The per-fill drawdown in market_engine.c only observes the book when a
// fill lands, so a wide-exit config that rides a 40% dip to a profitable
// close never shows the dip. This tracker marks the book at every 1d bar
// close instead: peak/trough over the marks gives the true MTM max
// drawdown, and a Welford pass over the daily simple returns gives an
// annualized daily Sharpe (mean/std x sqrt(365), rf = 0, population
// variance — the same convention as wm_market_stats_sharpe). The series
// itself is captured only when the caller asks (equity.jsonl emission on
// single-config runs); the scalars are always accumulated.

typedef struct
{
  double              prev_eq;    // last mark; seeds from starting cash
  double              peak;       // running equity peak
  double              max_dd;     // worst (peak - eq) / peak seen
  uint64_t            n_ret;      // Welford sample count (daily returns)
  double              mean;       // Welford running mean
  double              m2;         // Welford running sum of squared devs
  uint32_t            n_marks;    // 1d closes marked
  wm_bt_equity_pt_t  *series;     // lazily allocated when capturing
  uint32_t            s_n;
  uint32_t            s_cap;
  bool                capture;
} wm_bt_mtm_t;

static void
wm_bt_mtm_init(wm_bt_mtm_t *m, double start_cash, bool capture)
{
  memset(m, 0, sizeof(*m));

  m->capture = capture;

  // Seed peak + baseline from starting cash so a first-day loss
  // registers in both drawdown and the first daily return. A
  // non-positive start (defensive) seeds lazily from the first mark.
  if(start_cash > 0.0)
  {
    m->prev_eq = start_cash;
    m->peak    = start_cash;
  }
}

static void
wm_bt_mtm_mark(wm_bt_mtm_t *m, int64_t ts_ms, double eq)
{
  if(m->prev_eq > 0.0)
  {
    double r     = eq / m->prev_eq - 1.0;
    double delta = r - m->mean;

    m->n_ret++;
    m->mean += delta / (double)m->n_ret;
    m->m2   += delta * (r - m->mean);
  }

  m->prev_eq = eq;
  m->n_marks++;

  if(eq > m->peak)
    m->peak = eq;

  if(m->peak > 0.0)
  {
    double dd = (m->peak - eq) / m->peak;

    if(dd > m->max_dd)
      m->max_dd = dd;
  }

  if(m->capture)
  {
    if(m->s_n == m->s_cap)
    {
      uint32_t newcap = m->s_cap ? m->s_cap * 2u : 512u;

      m->series = (m->s_cap == 0)
          ? mem_alloc(WM_BT_CTX, "mtm_equity",
                sizeof(*m->series) * (size_t)newcap)
          : mem_realloc(m->series, sizeof(*m->series) * (size_t)newcap);
      m->s_cap  = newcap;
    }

    m->series[m->s_n].ts_ms  = ts_ms;
    m->series[m->s_n].equity = eq;
    m->s_n++;
  }
}

// Drain every fill the engine appended since the last call into the
// lossless accumulator, before the 256-slot session ring can wrap
// (see the WM-BT-8 rationale at the call sites). Grow-by-doubling;
// the strict allocator aborts on OOM.
static void
wm_bt_fills_drain(wm_market_session_t *sess, wm_market_fill_t **acc,
    uint32_t *acc_n, uint32_t *acc_cap, uint64_t *prev_fn)
{
  uint64_t now_fn = sess->fills_n[WM_MARKET_MODE_PAPER];

  while(*prev_fn < now_fn)
  {
    uint32_t back = (uint32_t)(now_fn - *prev_fn);
    uint32_t ridx = (sess->fills_head[WM_MARKET_MODE_PAPER]
                     + WM_MARKET_FILL_RING_CAP - back)
                    % WM_MARKET_FILL_RING_CAP;

    if(*acc_n == *acc_cap)
    {
      uint32_t newcap = *acc_cap ? *acc_cap * 2u : 256u;

      *acc = (*acc_cap == 0)
          ? mem_alloc(WM_BT_CTX, "iter_fills",
                sizeof(**acc) * (size_t)newcap)
          : mem_realloc(*acc, sizeof(**acc) * (size_t)newcap);
      *acc_cap = newcap;
    }

    (*acc)[(*acc_n)++] = sess->fills[WM_MARKET_MODE_PAPER][ridx];
    (*prev_fn)++;
  }
}

// Publish the accumulated stats into the result and hand over the
// captured series (ownership transfers with it; NULL when capture was
// off or no 1d bar marked).
static void
wm_bt_mtm_finish(wm_bt_mtm_t *m, wm_backtest_result_t *out)
{
  out->mtm_max_dd = m->max_dd;
  out->mtm_days   = m->n_marks;

  if(m->n_ret >= 2)
  {
    double variance = m->m2 / (double)m->n_ret;

    if(variance > 0.0)
    {
      double stddev = sqrt(variance);

      if(isfinite(stddev) && stddev > 0.0)
        out->daily_sharpe_ann = m->mean / stddev * sqrt(365.0);
    }
  }

  out->equity   = m->series;
  out->n_equity = m->s_n;
  m->series     = NULL;
  m->s_n        = 0;
  m->s_cap      = 0;
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
  int      (*init_fn)(wm_strategy_ctx_t *);
  void     (*finalize_fn)(wm_strategy_ctx_t *);
  void     (*on_bar_fn)(wm_strategy_ctx_t *,
               const struct whenmoon_market *,
               wm_gran_t,
               const wm_candle_full_t *);
  uint16_t                        smask;
  char                            sname[WM_STRATEGY_NAME_SZ];
  wm_strategy_ctx_t               ctx;
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
  wm_bt_mtm_t                     mtm;
  bool                            defer;
  wm_strategy_signal_t            pend[8];
  uint32_t                        pend_n        = 0;
  uint32_t                        pend_executed = 0;
  uint32_t                        pend_dropped  = 0;

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

  {
    loaded_strategy_t *ls;

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
    smask       = ls->meta.grains_mask;

    snprintf(sname, sizeof(sname), "%s", ls->name);
  }

  pthread_mutex_unlock(&st->strategies->lock);

  // Build the per-iteration synthetic market. It shares the snapshot's
  // grain rings + product id but carries an independent session and
  // mutex; emit_signal_impl routes signals through
  // wm_market_engine_on_signal_with_mk(synth_mk, ...). The market owns
  // the single position; the strategy is its one advisor (WM-MI-3).
  if(wm_market_create_synthetic(synth_id, &snap->mkt, &synth_mk,
         err_synth, sizeof(err_synth)) != SUCCESS)
  {
    if(err != NULL)
      snprintf(err, err_cap, "synth market: %s",
          err_synth[0] != '\0' ? err_synth : "(no detail)");
    return(FAIL);
  }

  wm_market_session_apply_iter_overrides(&synth_mk->session, params);

  // WM-RIGOR-4: the tracker seeds from the post-override starting cash;
  // the series buffer is captured only on request (single-config runs).
  wm_bt_mtm_init(&mtm,
      synth_mk->session.stats[WM_MARKET_MODE_PAPER].starting_cash,
      params != NULL && params->want_equity_series);

  // WM-RIGOR-5: --fill next-open defers signal execution to the next
  // 1m bar's open. The mechanism is pure replay-layer: ctx->mkt stays
  // NULL below, so wm_strategy_emit_signal_impl records the signal on
  // the ctx (signals_emitted / last_signal — the acted-detection this
  // loop already uses) but never reaches the engine; the loop queues
  // the advice and executes it on the next 1m bar instead.
  defer = params != NULL && params->fill_next_open;

  // Build + init the strategy ctx, pointed at the shared synth market
  // so every emit lands on the one book.
  memset(&ctx, 0, sizeof(ctx));
  snprintf(ctx.market_id_str, sizeof(ctx.market_id_str), "%s", synth_id);
  snprintf(ctx.strategy_name, sizeof(ctx.strategy_name), "%s", sname);
  ctx.mkt = defer ? NULL : synth_mk;

  if(init_fn(&ctx) != 0)
  {
    wm_market_destroy_synthetic(synth_mk);

    if(err != NULL)
      snprintf(err, err_cap, "strategy %s init returned non-zero", sname);
    return(FAIL);
  }

  // Set up cursors over each grain ring; a grain is walked iff the
  // strategy subscribes to it.
  {
    uint32_t g;

    for(g = 0; g < WM_GRAN_MAX; g++)
    {
      cursors[g].idx        = 0;
      cursors[g].cap        = snap->mkt.grain_n[g];
      cursors[g].subscribed = (smask & (uint16_t)(1u << g)) != 0;
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

    // WM-RIGOR-5 (--fill next-open): execute advice deferred from the
    // bar that emitted it at THIS 1m bar's open ± slip, stamped at the
    // bar's open instant. Runs before the bar's own dispatch (an open
    // fill precedes close-time decisions) and regardless of window
    // membership — the signal already committed inside its window;
    // this is merely its execution moment. FIFO order matches the
    // back-to-back execution immediate mode would have produced for a
    // same-ts signal cluster; the engine's own idempotency (buy while
    // long / sell while flat = no-op) applies at execution, exactly as
    // it would have at emit.
    if(defer && pend_n > 0 && g == WM_GRAN_1M)
    {
      int64_t  open_ts = bar->ts_close_ms
                       - (int64_t)wm_gran_seconds[WM_GRAN_1M] * 1000;
      uint32_t pi;

      for(pi = 0; pi < pend_n; pi++)
      {
        pend[pi].ts_ms = open_ts;
        wm_market_engine_on_signal_with_mk(synth_mk, bar->open,
            open_ts, &pend[pi]);
      }

      pend_executed += pend_n;
      pend_n = 0;

      // Deferred fills can land outside the in-window drain below
      // (train gaps, unsubscribed 1m) — drain here so the lossless
      // accumulator never misses them.
      wm_bt_fills_drain(sess, &acc_fills, &acc_n, &acc_cap, &prev_fn);
    }

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
        uint64_t pre_emitted;

        // Single-advisor dispatch, matching wm_strategy_dispatch_bar
        // (WM-MI-3): the subscribed grain's bar updates the ctx mark
        // cache and fires on_bar. A strategy tracks its own internal
        // position across bars.
        ctx.bars_seen++;
        ctx.last_bar_ts_ms = bar->ts_close_ms;
        ctx.last_mark_px   = bar->close;
        ctx.last_mark_ms   = bar->ts_close_ms;

        pre_emitted = ctx.signals_emitted;
        on_bar_fn(&ctx, &snap->mkt, g, bar);

        if(ctx.signals_emitted > pre_emitted &&
           ctx.last_signal.score != 0.0)
        {
          // WM-RIGOR-5: the emit shim recorded the signal on the ctx
          // but never reached the engine (ctx->mkt == NULL) — queue
          // it for execution at the next 1m bar's open. The cap
          // covers the worst same-ts bar cluster (one winning signal
          // per bar, <= 6 bars share a close ts); overflow is counted
          // and reported, never silent.
          if(defer)
          {
            if(pend_n < (uint32_t)(sizeof(pend) / sizeof(pend[0])))
              pend[pend_n++] = ctx.last_signal;
            else
              pend_dropped++;
          }
        }

        bars_replayed++;

        // Drain the (<= 1) fill this bar produced into the lossless
        // accumulator before the 256-slot ring can overwrite it. Only
        // the single winning advisor acts, so the engine adds at most
        // one fill per bar; the drain loop is defensive.
        wm_bt_fills_drain(sess, &acc_fills, &acc_n, &acc_cap, &prev_fn);
      }
    }

    // WM-RIGOR-4: mark the book at every 1d close. The 1d bar's
    // ts_close_ms ties with the day's final lower-grain bars, and ties
    // resolve lower-grain-first, so every fill of the day has already
    // settled cash/position by the time the 1d bar lands here. The hook
    // sits outside the `subscribed` gate — unsubscribed grains still
    // walk through this loop, so the marks fire even when no strategy
    // trades on 1d. Windowed runs mark only in-window days, keeping
    // fold stats undiluted by the flat train/warmup stretches where
    // the strategy never fires.
    if(g == WM_GRAN_1D &&
       (n_windows == 0 ||
        wm_bt_in_any_window(windows, n_windows, bar->ts_close_ms)))
    {
      double eq = sess->stats[WM_MARKET_MODE_PAPER].cash;

      if(sess->position.side == WM_MARKET_POS_LONG)
        eq += sess->position.qty * bar->close;

      wm_bt_mtm_mark(&mtm, bar->ts_close_ms, eq);
    }

    cursors[g].idx++;
  }

  clock_gettime(CLOCK_MONOTONIC, &t1);

  // WM-RIGOR-5: advice emitted on the corpus's final bars has no next
  // 1m bar to fill on — it is dropped, and reported below. Immediate
  // mode would have filled it at the terminal close; the delta is part
  // of what the next-open model measures.
  if(defer && pend_n > 0)
  {
    pend_dropped += pend_n;
    pend_n = 0;
  }

  finalize_fn(&ctx);

  // Snapshot the synth market's session before tearing it down.
  if(wm_market_session_snapshot(synth_mk, &out->trade) != SUCCESS)
  {
    if(acc_fills != NULL)
      mem_free(acc_fills);

    if(mtm.series != NULL)
      mem_free(mtm.series);

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

  // WM-RIGOR-4: publish the MTM stats + transfer the captured equity
  // series (NULL unless params->want_equity_series).
  wm_bt_mtm_finish(&mtm, out);

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
      snap->source_market_id, sname,
      ps->n_trades, ps->n_wins, ps->n_losses, win_rate,
      ps->starting_cash, ps->starting_cash + realized_paper,
      realized_paper, (double)snap->bars_loaded_1m / 1440.0,
      bars_replayed, fills_paper, out->wallclock_ms);

  // WM-RIGOR-5: one accounting line per next-open run. `dropped` > 0
  // means terminal-bar advice with no next 1m bar (or the same-ts
  // cluster overflowed the queue — unheard of).
  if(defer)
    clam(CLAM_INFO, WM_BT_CTX,
        "backtest %s/%s: fill=next-open deferred=%u dropped=%u",
        snap->source_market_id, sname,
        pend_executed, pend_dropped);

  return(SUCCESS);
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

// ----------------------------------------------------------------------- //
// WM-RIGOR-2 — buy-and-hold benchmark return                              //
// ----------------------------------------------------------------------- //

// Index of the first 1m bar in [begin, n) whose ts_close_ms >= ts, or n
// when every bar closes earlier. Bars are ascending by ts_close_ms.
static uint32_t
wm_bt_bars_lower_bound(const wm_candle_full_t *bars, uint32_t begin,
    uint32_t n, int64_t ts)
{
  uint32_t lo = begin;
  uint32_t hi = n;

  while(lo < hi)
  {
    uint32_t mid = lo + (hi - lo) / 2u;

    if(bars[mid].ts_close_ms < ts)
      lo = mid + 1;

    else
      hi = mid;
  }

  return(lo);
}

bool
wm_bt_bench_return(const wm_backtest_snapshot_t *snap,
    const wm_bt_window_t *win, double *out)
{
  const wm_candle_full_t *bars;
  uint32_t                n;
  uint32_t                lo;
  uint32_t                hi;
  double                  entry_close;
  double                  exit_close;

  if(snap == NULL || out == NULL)
    return(FAIL);

  bars = snap->mkt.grain_arr[WM_GRAN_1M];
  n    = snap->mkt.grain_n[WM_GRAN_1M];

  if(bars == NULL || n < 2)
    return(FAIL);

  lo = 0;
  hi = n;

  if(win != NULL)
  {
    lo = wm_bt_bars_lower_bound(bars, 0,  n, win->start_ts_ms);
    hi = wm_bt_bars_lower_bound(bars, lo, n, win->end_ts_ms);
  }

  if(hi - lo < 2)
    return(FAIL);

  entry_close = bars[lo].close;
  exit_close  = bars[hi - 1].close;

  if(entry_close <= 0.0 || exit_close <= 0.0)
    return(FAIL);

  *out = exit_close / entry_close - 1.0;
  return(SUCCESS);
}
