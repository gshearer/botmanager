// botmanager — MIT
// Whenmoon downloader coverage store: atomic merge + gap complement.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "dl_coverage.h"
#include "dl_schema.h"   // WM_DL_CTX

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "db.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WM_COV_LOCK_TAG  "wm_cov:"

// Advisory-lock key renderer. Fed to hashtext() via
// pg_advisory_xact_lock(hashtext(...)) inside the merge transaction.
// Postgres hashes the bigint result into the lock space; collisions
// are statistically rare and always harmless (worst case: two
// unrelated locks serialise briefly).
static void
wm_cov_advisory_key(wm_coverage_kind_t kind, int32_t market_id,
    int32_t gran_secs, char *out, size_t cap)
{
  snprintf(out, cap, "%s%d:%" PRId32 ":%" PRId32,
      WM_COV_LOCK_TAG, (int)kind, market_id, gran_secs);
}

// --------------------------------------------------------------------
// iv self-consistency. A page-construction bug could let first/last
// bounds invert; catch the malformed iv at the boundary so it can't
// reach the merge SQL and poison the result via LEAST/GREATEST.
// --------------------------------------------------------------------
static bool
wm_cov_iv_consistent_trades(const wm_coverage_t *iv)
{
  if(iv->first_trade_id > iv->last_trade_id)
  {
    clam(CLAM_WARN, WM_DL_CTX,
        "coverage trades iv ids inverted "
        "(market=%" PRId32 " first=%" PRId64 " last=%" PRId64 ")",
        iv->market_id, iv->first_trade_id, iv->last_trade_id);
    return(FAIL);
  }

  if(strcmp(iv->first_ts, iv->last_ts) > 0)
  {
    clam(CLAM_WARN, WM_DL_CTX,
        "coverage trades iv ts inverted "
        "(market=%" PRId32 " first=%s last=%s)",
        iv->market_id, iv->first_ts, iv->last_ts);
    return(FAIL);
  }

  return(SUCCESS);
}

static bool
wm_cov_iv_consistent_candles(const wm_coverage_t *iv)
{
  if(strcmp(iv->first_ts, iv->last_ts) > 0)
  {
    clam(CLAM_WARN, WM_DL_CTX,
        "coverage candles iv ts inverted "
        "(market=%" PRId32 " gran=%" PRId32 " first=%s last=%s)",
        iv->market_id, iv->granularity, iv->first_ts, iv->last_ts);
    return(FAIL);
  }

  return(SUCCESS);
}

// --------------------------------------------------------------------
// Trades dual-axis precheck. The merge predicate touches in BOTH ID
// and timestamp space; a row that touches in only one dimension is
// corrupted (an earlier malformed page or pre-WM-DC-2 merge poisoned
// the boundary) and the merge would propagate the corruption. Reject
// with CLAM_WARN — WM-DC-5 cleanup resolves persistent mismatches.
// --------------------------------------------------------------------
static bool
wm_cov_check_mismatch_trades(const wm_coverage_t *iv)
{
  db_result_t *res        = NULL;
  char        *e_first_ts = NULL;
  char        *e_last_ts  = NULL;
  char         sql[2048];
  bool         ok         = SUCCESS;
  int          n;

  e_first_ts = db_escape(iv->first_ts);
  e_last_ts  = db_escape(iv->last_ts);

  if(e_first_ts == NULL || e_last_ts == NULL)
  {
    ok = FAIL;
    goto out;
  }

  // Boolean inequality (`<>`) is XOR — matches exactly the rows that
  // touch in one axis but not the other. LIMIT 4 keeps the log
  // bounded; a single mismatch is the actionable signal.
  n = snprintf(sql, sizeof(sql),
      "SELECT first_trade_id, last_trade_id,"
      "       to_char(first_ts AT TIME ZONE 'UTC',"
      "               'YYYY-MM-DD HH24:MI:SS.US') || '+00' AS fts,"
      "       to_char(last_ts  AT TIME ZONE 'UTC',"
      "               'YYYY-MM-DD HH24:MI:SS.US') || '+00' AS lts"
      "  FROM wm_trade_coverage"
      " WHERE market_id = %" PRId32
      "   AND ((first_trade_id <= %" PRId64 " + %d"
      "         AND last_trade_id  >= %" PRId64 " - %d)"
      "        <>"
      "        (first_ts <= TIMESTAMPTZ '%s' + INTERVAL '%d seconds'"
      "         AND last_ts >= TIMESTAMPTZ '%s' - INTERVAL '%d seconds'))"
      " ORDER BY first_trade_id"
      " LIMIT 4",
      iv->market_id,
      iv->last_trade_id,  WM_COV_ID_MERGE_GAP,
      iv->first_trade_id, WM_COV_ID_MERGE_GAP,
      e_last_ts,  WM_COV_TS_MERGE_GAP_SECONDS,
      e_first_ts, WM_COV_TS_MERGE_GAP_SECONDS);

  if(n < 0 || (size_t)n >= sizeof(sql))
  {
    clam(CLAM_WARN, WM_DL_CTX,
        "coverage mismatch sql truncated (market=%" PRId32 ")",
        iv->market_id);
    ok = FAIL;
    goto out;
  }

  res = db_result_alloc();

  if(res == NULL)
  {
    ok = FAIL;
    goto out;
  }

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, WM_DL_CTX,
        "coverage mismatch query failed (market=%" PRId32 "): %s",
        iv->market_id,
        res->error[0] != '\0' ? res->error : "(no driver error)");
    ok = FAIL;
    goto out;
  }

  if(res->rows > 0)
  {
    clam(CLAM_WARN, WM_DL_CTX,
        "coverage merge rejected: %u row(s) touch in one axis only "
        "(market=%" PRId32 " iv ids=%" PRId64 "..%" PRId64
        " iv ts=%s..%s)",
        res->rows, iv->market_id,
        iv->first_trade_id, iv->last_trade_id,
        iv->first_ts, iv->last_ts);

    for(uint32_t i = 0; i < res->rows; i++)
    {
      const char *fid = db_result_get(res, i, 0);
      const char *lid = db_result_get(res, i, 1);
      const char *fts = db_result_get(res, i, 2);
      const char *lts = db_result_get(res, i, 3);

      clam(CLAM_WARN, WM_DL_CTX,
          "  mismatched row: ids=%s..%s ts=%s..%s",
          fid != NULL ? fid : "(null)",
          lid != NULL ? lid : "(null)",
          fts != NULL ? fts : "(null)",
          lts != NULL ? lts : "(null)");
    }

    ok = FAIL;
  }

out:
  if(res        != NULL) db_result_free(res);
  if(e_first_ts != NULL) mem_free(e_first_ts);
  if(e_last_ts  != NULL) mem_free(e_last_ts);

  return(ok);
}

// --------------------------------------------------------------------
// Candles 30-day sanity precheck. Candles have no ID axis to cross-
// check, so a malformed page can drag an existing row's bounds across
// multiple years (the same failure mode bug #1 took on the trade
// table). Reject the merge if any candidate touching row's range
// extends more than WM_COV_TS_SANITY_DAYS outside iv on either side;
// the widened 2*gran touching gap is generous enough for normal
// pagination while leaving the multi-year jump assertion meaningful.
// --------------------------------------------------------------------
static bool
wm_cov_check_sanity_candles(const wm_coverage_t *iv)
{
  db_result_t *res        = NULL;
  char        *e_first_ts = NULL;
  char        *e_last_ts  = NULL;
  char         sql[2048];
  int32_t      touch_gap  = iv->granularity * 2;
  bool         ok         = SUCCESS;
  int          n;

  e_first_ts = db_escape(iv->first_ts);
  e_last_ts  = db_escape(iv->last_ts);

  if(e_first_ts == NULL || e_last_ts == NULL)
  {
    ok = FAIL;
    goto out;
  }

  n = snprintf(sql, sizeof(sql),
      "SELECT to_char(range_start AT TIME ZONE 'UTC',"
      "               'YYYY-MM-DD HH24:MI:SS.US') || '+00' AS rs,"
      "       to_char(range_end   AT TIME ZONE 'UTC',"
      "               'YYYY-MM-DD HH24:MI:SS.US') || '+00' AS re"
      "  FROM wm_candle_coverage"
      " WHERE market_id = %" PRId32
      "   AND granularity = %" PRId32
      "   AND range_start <= TIMESTAMPTZ '%s'"
      "                      + INTERVAL '%" PRId32 " seconds'"
      "   AND range_end   >= TIMESTAMPTZ '%s'"
      "                      - INTERVAL '%" PRId32 " seconds'"
      "   AND (range_start < TIMESTAMPTZ '%s'"
      "                      - INTERVAL '%d days'"
      "        OR range_end > TIMESTAMPTZ '%s'"
      "                       + INTERVAL '%d days')"
      " ORDER BY range_start"
      " LIMIT 4",
      iv->market_id,
      iv->granularity,
      e_last_ts,  touch_gap,
      e_first_ts, touch_gap,
      e_first_ts, WM_COV_TS_SANITY_DAYS,
      e_last_ts,  WM_COV_TS_SANITY_DAYS);

  if(n < 0 || (size_t)n >= sizeof(sql))
  {
    clam(CLAM_WARN, WM_DL_CTX,
        "coverage sanity sql truncated "
        "(market=%" PRId32 " gran=%" PRId32 ")",
        iv->market_id, iv->granularity);
    ok = FAIL;
    goto out;
  }

  res = db_result_alloc();

  if(res == NULL)
  {
    ok = FAIL;
    goto out;
  }

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, WM_DL_CTX,
        "coverage sanity query failed "
        "(market=%" PRId32 " gran=%" PRId32 "): %s",
        iv->market_id, iv->granularity,
        res->error[0] != '\0' ? res->error : "(no driver error)");
    ok = FAIL;
    goto out;
  }

  if(res->rows > 0)
  {
    clam(CLAM_WARN, WM_DL_CTX,
        "coverage candle merge rejected: %u row(s) >%d days outside iv "
        "(market=%" PRId32 " gran=%" PRId32 " iv=%s..%s)",
        res->rows, WM_COV_TS_SANITY_DAYS,
        iv->market_id, iv->granularity,
        iv->first_ts, iv->last_ts);

    for(uint32_t i = 0; i < res->rows; i++)
    {
      const char *rs = db_result_get(res, i, 0);
      const char *re = db_result_get(res, i, 1);

      clam(CLAM_WARN, WM_DL_CTX,
          "  out-of-band row: range=%s..%s",
          rs != NULL ? rs : "(null)",
          re != NULL ? re : "(null)");
    }

    ok = FAIL;
  }

out:
  if(res        != NULL) db_result_free(res);
  if(e_first_ts != NULL) mem_free(e_first_ts);
  if(e_last_ts  != NULL) mem_free(e_last_ts);

  return(ok);
}

// --------------------------------------------------------------------
// wm_cov_merge_tx: one-shot transaction.
//
// The botmanager db pool acquires a fresh connection per db_query
// call — release on return, claim on entry. A naive BEGIN / COMMIT
// split across two db_query calls lands each statement on a different
// pool slot and the transaction (and any FOR UPDATE lock) evaporates
// between statements. The merge therefore submits the whole
// transaction as one semicolon-separated SQL string; PQexec runs the
// script under one implicit transaction-ish context on the single
// connection claimed for that one call.
//
// Post-conditions on success:
//   * Any row in (wm_trade_coverage | wm_candle_coverage) that
//     overlapped or touched `iv` has been deleted.
//   * Exactly one row has been inserted whose interval is the union
//     of iv and the deleted rows' intervals.
//   * Per (market, kind, granularity) serialisation held via
//     pg_advisory_xact_lock for the whole BEGIN...COMMIT.
// --------------------------------------------------------------------
static bool
wm_cov_merge_tx(wm_coverage_kind_t kind, const wm_coverage_t *iv)
{
  db_result_t *res        = NULL;
  char        *e_first_ts = NULL;
  char        *e_last_ts  = NULL;
  char        *e_source   = NULL;
  char         sql[4096];
  char         lock_key[96];
  bool         ok = FAIL;
  int          n;

  wm_cov_advisory_key(kind, iv->market_id,
      kind == WM_COV_CANDLES ? iv->granularity : 0,
      lock_key, sizeof(lock_key));

  e_first_ts = db_escape(iv->first_ts);
  e_last_ts  = db_escape(iv->last_ts);
  e_source   = db_escape(iv->source);

  if(e_first_ts == NULL || e_last_ts == NULL || e_source == NULL)
    goto out;

  if(kind == WM_COV_TRADES)
  {
    // Trades dual-axis touching predicate. Both ID and timestamp
    // axes must agree; the WM-DC-2 precheck (wm_cov_check_mismatch_
    // trades) already rejected the merge if any single-axis row
    // existed, so this WHERE is the post-precheck merge predicate.
    //   WHERE market_id = :mid
    //     AND first_trade_id <= :iv_last  + WM_COV_ID_MERGE_GAP
    //     AND last_trade_id  >= :iv_first - WM_COV_ID_MERGE_GAP
    //     AND first_ts <= :iv_last_ts  + WM_COV_TS_MERGE_GAP_SECONDS
    //     AND last_ts  >= :iv_first_ts - WM_COV_TS_MERGE_GAP_SECONDS
    n = snprintf(sql, sizeof(sql),
      "BEGIN;"
      "SELECT pg_advisory_xact_lock(hashtext('%s'));"
      "WITH touching AS ("
      "  SELECT first_trade_id, last_trade_id, first_ts, last_ts"
      "    FROM wm_trade_coverage"
      "   WHERE market_id = %" PRId32
      "     AND first_trade_id <= %" PRId64 " + %d"
      "     AND last_trade_id  >= %" PRId64 " - %d"
      "     AND first_ts <= TIMESTAMPTZ '%s'"
      "                     + INTERVAL '%d seconds'"
      "     AND last_ts  >= TIMESTAMPTZ '%s'"
      "                     - INTERVAL '%d seconds'"
      "   FOR UPDATE"
      "), merged AS ("
      "  SELECT LEAST(MIN(first_trade_id), %" PRId64 ") AS fid,"
      "         GREATEST(MAX(last_trade_id),  %" PRId64 ") AS lid,"
      "         LEAST(MIN(first_ts), TIMESTAMPTZ '%s')  AS fts,"
      "         GREATEST(MAX(last_ts), TIMESTAMPTZ '%s') AS lts"
      "    FROM touching"
      "), del AS ("
      "  DELETE FROM wm_trade_coverage"
      "   WHERE market_id = %" PRId32
      "     AND first_trade_id IN (SELECT first_trade_id FROM touching)"
      "   RETURNING 1"
      ") "
      "INSERT INTO wm_trade_coverage"
      "   (market_id, first_trade_id, last_trade_id,"
      "    first_ts, last_ts, source) "
      " SELECT %" PRId32 ","
      "        COALESCE((SELECT fid FROM merged), %" PRId64 "),"
      "        COALESCE((SELECT lid FROM merged), %" PRId64 "),"
      "        COALESCE((SELECT fts FROM merged), TIMESTAMPTZ '%s'),"
      "        COALESCE((SELECT lts FROM merged), TIMESTAMPTZ '%s'),"
      "        '%s'"
      " WHERE (SELECT COUNT(*) FROM del) >= 0;"
      "COMMIT;",
      lock_key,
      iv->market_id,
      iv->last_trade_id,  WM_COV_ID_MERGE_GAP,
      iv->first_trade_id, WM_COV_ID_MERGE_GAP,
      e_last_ts,  WM_COV_TS_MERGE_GAP_SECONDS,
      e_first_ts, WM_COV_TS_MERGE_GAP_SECONDS,
      iv->first_trade_id,
      iv->last_trade_id,
      e_first_ts,
      e_last_ts,
      iv->market_id,
      iv->market_id,
      iv->first_trade_id,
      iv->last_trade_id,
      e_first_ts,
      e_last_ts,
      e_source);
  }

  else  // WM_COV_CANDLES
  {
    // Candles touching predicate (timestamp-only — no ID axis to
    // cross-check). Widened to 2*gran so adjacent paginations merge
    // even when the boundary candle is half-formed; the WM-DC-2
    // sanity precheck (wm_cov_check_sanity_candles) has already
    // rejected merges that would jump >30 days.
    //   WHERE market_id = :mid
    //     AND granularity = :gran
    //     AND range_start <= :iv_end   + :gran*2 seconds
    //     AND range_end   >= :iv_start - :gran*2 seconds
    int32_t touch_gap = iv->granularity * 2;

    n = snprintf(sql, sizeof(sql),
      "BEGIN;"
      "SELECT pg_advisory_xact_lock(hashtext('%s'));"
      "WITH touching AS ("
      "  SELECT range_start, range_end"
      "    FROM wm_candle_coverage"
      "   WHERE market_id = %" PRId32
      "     AND granularity = %" PRId32
      "     AND range_start <= TIMESTAMPTZ '%s'"
      "                        + INTERVAL '%" PRId32 " seconds'"
      "     AND range_end   >= TIMESTAMPTZ '%s'"
      "                        - INTERVAL '%" PRId32 " seconds'"
      "   FOR UPDATE"
      "), merged AS ("
      "  SELECT LEAST(MIN(range_start), TIMESTAMPTZ '%s')  AS rs,"
      "         GREATEST(MAX(range_end),  TIMESTAMPTZ '%s') AS re"
      "    FROM touching"
      "), del AS ("
      "  DELETE FROM wm_candle_coverage"
      "   WHERE market_id = %" PRId32
      "     AND granularity = %" PRId32
      "     AND range_start IN (SELECT range_start FROM touching)"
      "   RETURNING 1"
      ") "
      "INSERT INTO wm_candle_coverage"
      "   (market_id, granularity, range_start, range_end) "
      " SELECT %" PRId32 ", %" PRId32 ","
      "        COALESCE((SELECT rs FROM merged), TIMESTAMPTZ '%s'),"
      "        COALESCE((SELECT re FROM merged), TIMESTAMPTZ '%s')"
      " WHERE (SELECT COUNT(*) FROM del) >= 0;"
      "COMMIT;",
      lock_key,
      iv->market_id,
      iv->granularity,
      e_last_ts,  touch_gap,
      e_first_ts, touch_gap,
      e_first_ts,
      e_last_ts,
      iv->market_id,
      iv->granularity,
      iv->market_id,
      iv->granularity,
      e_first_ts,
      e_last_ts);
  }

  if(n < 0 || (size_t)n >= sizeof(sql))
  {
    clam(CLAM_WARN, WM_DL_CTX,
        "coverage merge sql truncated (kind=%d market=%" PRId32 ")",
        (int)kind, iv->market_id);
    goto out;
  }

  res = db_result_alloc();

  if(res == NULL)
    goto out;

  if(db_query(sql, res) == SUCCESS && res->ok)
  {
    ok = SUCCESS;
  }

  else
  {
    // BEGIN ... COMMIT is one payload; if any statement errored the
    // whole batch rolls back at the Postgres level — no explicit
    // ROLLBACK needed. libpq reports the first-offending statement's
    // message.
    clam(CLAM_WARN, WM_DL_CTX,
        "coverage merge failed (kind=%d market=%" PRId32 "): %s",
        (int)kind, iv->market_id,
        res->error[0] != '\0' ? res->error : "(no driver error)");
  }

out:
  if(res        != NULL) db_result_free(res);
  if(e_first_ts != NULL) mem_free(e_first_ts);
  if(e_last_ts  != NULL) mem_free(e_last_ts);
  if(e_source   != NULL) mem_free(e_source);

  return(ok);
}

bool
wm_coverage_add(wm_coverage_kind_t kind, const wm_coverage_t *iv)
{
  if(iv == NULL)
    return(FAIL);

  if(kind == WM_COV_CANDLES && iv->granularity <= 0)
    return(FAIL);

  // WM-DC-2 prechecks: validate iv self-consistency, then assert no
  // existing rows would propagate corruption through the merge.
  if(kind == WM_COV_TRADES)
  {
    if(wm_cov_iv_consistent_trades(iv) != SUCCESS)
      return(FAIL);

    if(wm_cov_check_mismatch_trades(iv) != SUCCESS)
      return(FAIL);
  }

  else
  {
    if(wm_cov_iv_consistent_candles(iv) != SUCCESS)
      return(FAIL);

    if(wm_cov_check_sanity_candles(iv) != SUCCESS)
      return(FAIL);
  }

  return(wm_cov_merge_tx(kind, iv));
}

// --------------------------------------------------------------------
// Gap computation — shared walker shape:
//
//   rows := SELECT ... FROM <coverage_table>
//            WHERE market_id = :mid
//              AND <start_col> < :range_end
//              AND <end_col>   > :range_start
//            [ AND granularity = :gran ]
//            ORDER BY <start_col>
//
//   prev_end := range_start
//   for row in rows:
//     if row.<start_col> > prev_end:
//       emit gap [prev_end, row.<start_col>)
//     prev_end := MAX(prev_end, row.<end_col>)
//   if prev_end < range_end:
//     emit gap [prev_end, range_end)
//
// Lexicographic compare on canonical TIMESTAMPTZ UTC strings is
// order-preserving, so a cheap char compare suffices.
//
// For trades, last_trade_id is INCLUSIVE in coverage rows, so the
// next "not yet covered" id is last_trade_id+1; for candles,
// range_end is EXCLUSIVE already, so the next not-yet-covered instant
// is range_end itself. In both cases we advance prev_end with the
// row's end_col as stored, which is the correct next boundary.
//
// WM-DC-4: trade_id is the authoritative axis. A coverage row is
// only allowed to contribute to the union if both boundary trade_ids
// are physically present in `wm_trades_<market_id>` — otherwise the
// row's timestamp claim is unsubstantiated (the Row B pathology) and
// the gap walker must keep the underlying window open. Legacy rows
// with `first_trade_id == 0` are time-only by construction and bypass
// the probe.
// --------------------------------------------------------------------

// Returns true when the coverage row's first/last trade_ids are both
// present in `wm_trades_<market_id>`. Returns false for: missing per-
// market table, missing boundary row, or any DB error. The caller
// treats false as "row contributes no coverage" so the gap walker
// keeps the window open across an unbacked row.
static bool
wm_cov_row_backed(int32_t market_id, int64_t fid, int64_t lid)
{
  db_result_t *res = NULL;
  char         table[WM_DL_TABLE_SZ];
  char         sql[256];
  bool         ok  = false;
  int64_t      need;
  int          n;

  if(fid <= 0 || lid <= 0)
    return(false);

  if(wm_trade_table_name(market_id, table, sizeof(table)) != SUCCESS)
    return(false);

  // `IN` dedupes equal operands, so the count we need is 1 when the
  // row is a single-trade page and 2 otherwise. The trade table's
  // primary key on trade_id makes this a B-tree probe per id.
  need = (fid == lid) ? 1 : 2;

  n = snprintf(sql, sizeof(sql),
      "SELECT count(*) FROM %s"
      " WHERE trade_id IN (%" PRId64 ", %" PRId64 ")",
      table, fid, lid);

  if(n < 0 || (size_t)n >= sizeof(sql))
    return(false);

  res = db_result_alloc();

  if(res == NULL)
    return(false);

  if(db_query(sql, res) == SUCCESS && res->ok && res->rows == 1)
  {
    const char *v = db_result_get(res, 0, 0);

    if(v != NULL && strtoll(v, NULL, 10) >= need)
      ok = true;
  }

  db_result_free(res);
  return(ok);
}

uint32_t
wm_coverage_gaps_trades(int32_t market_id,
    const char *range_start, const char *range_end,
    wm_coverage_t *out, uint32_t max_out)
{
  db_result_t *res     = NULL;
  char        *e_start = NULL;
  char        *e_end   = NULL;
  char         sql[1024];
  char         prev_end[WM_COV_TS_SZ];
  uint32_t     emitted = 0;
  int          n;

  if(out == NULL || max_out == 0)
    return(0);

  if(range_start == NULL || range_end == NULL)
    return(0);

  if(strcmp(range_end, range_start) <= 0)
    return(0);

  e_start = db_escape(range_start);
  e_end   = db_escape(range_end);

  if(e_start == NULL || e_end == NULL)
    goto out;

  // to_char(...) coerces libpq's default timestamptz output into the
  // canonical "+00" UTC form our wm_coverage_t expects. Without it,
  // the server's timezone setting would leak into the string.
  n = snprintf(sql, sizeof(sql),
      "SELECT first_trade_id, last_trade_id,"
      "       to_char(first_ts AT TIME ZONE 'UTC',"
      "               'YYYY-MM-DD HH24:MI:SS.US') || '+00' AS fts,"
      "       to_char(last_ts  AT TIME ZONE 'UTC',"
      "               'YYYY-MM-DD HH24:MI:SS.US') || '+00' AS lts"
      "  FROM wm_trade_coverage"
      " WHERE market_id = %" PRId32
      "   AND first_ts < TIMESTAMPTZ '%s'"
      "   AND last_ts  > TIMESTAMPTZ '%s'"
      " ORDER BY first_ts",
      market_id, e_end, e_start);

  if(n < 0 || (size_t)n >= sizeof(sql))
    goto out;

  res = db_result_alloc();

  if(res == NULL)
    goto out;

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, WM_DL_CTX,
        "coverage gap query failed (market=%" PRId32 "): %s",
        market_id,
        res->error[0] != '\0' ? res->error : "(no driver error)");
    goto out;
  }

  snprintf(prev_end, sizeof(prev_end), "%s", range_start);

  for(uint32_t i = 0; i < res->rows && emitted < max_out; i++)
  {
    const char *row_fid_str  = db_result_get(res, i, 0);
    const char *row_lid_str  = db_result_get(res, i, 1);
    const char *row_first_ts = db_result_get(res, i, 2);
    const char *row_last_ts  = db_result_get(res, i, 3);
    int64_t     row_fid;
    int64_t     row_lid;

    if(row_first_ts == NULL || row_last_ts == NULL)
      continue;

    row_fid = (row_fid_str != NULL) ? strtoll(row_fid_str, NULL, 10) : 0;
    row_lid = (row_lid_str != NULL) ? strtoll(row_lid_str, NULL, 10) : 0;

    // WM-DC-4 backed-data probe. Rows with a populated ID range must
    // have their boundary trade_ids physically present in the per-
    // market trade table; otherwise the row's timestamp claim is
    // unsubstantiated and we treat it as not covering anything (i.e.
    // do not advance prev_end). Legacy rows with first_trade_id==0
    // pre-date WM-S4's id-axis writes and bypass the probe.
    if(row_fid > 0 && row_lid > 0 &&
       !wm_cov_row_backed(market_id, row_fid, row_lid))
    {
      clam(CLAM_WARN, WM_DL_CTX,
          "coverage row not backed by data, treating as gap "
          "(market=%" PRId32 " ids=%" PRId64 "..%" PRId64
          " ts=%s..%s)",
          market_id, row_fid, row_lid, row_first_ts, row_last_ts);
      continue;
    }

    if(strcmp(row_first_ts, prev_end) > 0)
    {
      wm_coverage_t *g = &out[emitted++];

      memset(g, 0, sizeof(*g));
      g->market_id = market_id;
      snprintf(g->first_ts, WM_COV_TS_SZ, "%s", prev_end);
      snprintf(g->last_ts,  WM_COV_TS_SZ, "%s", row_first_ts);
      // first_trade_id / last_trade_id left at 0; the pre-range gap
      // is expressed in the timestamp window. WM-S4 resolves ids
      // from ts via the trade fetcher's first-page cursor.
    }

    if(strcmp(row_last_ts, prev_end) > 0)
      snprintf(prev_end, sizeof(prev_end), "%s", row_last_ts);
  }

  if(emitted < max_out && strcmp(prev_end, range_end) < 0)
  {
    wm_coverage_t *g = &out[emitted++];

    memset(g, 0, sizeof(*g));
    g->market_id = market_id;
    snprintf(g->first_ts, WM_COV_TS_SZ, "%s", prev_end);
    snprintf(g->last_ts,  WM_COV_TS_SZ, "%s", range_end);
  }

  // Overflow fallback: if the true gap count exceeds max_out, rewrite
  // the last slot's last_ts to range_end so admin callers see one
  // continuous "everything past this point" marker rather than a
  // truncated list.
  if(emitted == max_out && strcmp(prev_end, range_end) < 0)
    snprintf(out[max_out - 1].last_ts, WM_COV_TS_SZ, "%s", range_end);

out:
  if(res     != NULL) db_result_free(res);
  if(e_start != NULL) mem_free(e_start);
  if(e_end   != NULL) mem_free(e_end);

  return(emitted);
}

uint32_t
wm_coverage_gaps_candles(int32_t market_id, int32_t gran_secs,
    const char *range_start, const char *range_end,
    wm_coverage_t *out, uint32_t max_out)
{
  db_result_t *res     = NULL;
  char        *e_start = NULL;
  char        *e_end   = NULL;
  char         sql[1024];
  char         prev_end[WM_COV_TS_SZ];
  uint32_t     emitted = 0;
  int          n;

  if(out == NULL || max_out == 0)
    return(0);

  if(range_start == NULL || range_end == NULL)
    return(0);

  if(gran_secs <= 0)
    return(0);

  if(strcmp(range_end, range_start) <= 0)
    return(0);

  e_start = db_escape(range_start);
  e_end   = db_escape(range_end);

  if(e_start == NULL || e_end == NULL)
    goto out;

  n = snprintf(sql, sizeof(sql),
      "SELECT to_char(range_start AT TIME ZONE 'UTC',"
      "               'YYYY-MM-DD HH24:MI:SS.US') || '+00' AS rs,"
      "       to_char(range_end   AT TIME ZONE 'UTC',"
      "               'YYYY-MM-DD HH24:MI:SS.US') || '+00' AS re"
      "  FROM wm_candle_coverage"
      " WHERE market_id = %" PRId32
      "   AND granularity = %" PRId32
      "   AND range_start < TIMESTAMPTZ '%s'"
      "   AND range_end   > TIMESTAMPTZ '%s'"
      " ORDER BY range_start",
      market_id, gran_secs, e_end, e_start);

  if(n < 0 || (size_t)n >= sizeof(sql))
    goto out;

  res = db_result_alloc();

  if(res == NULL)
    goto out;

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, WM_DL_CTX,
        "coverage gap query failed (market=%" PRId32
        " gran=%" PRId32 "): %s",
        market_id, gran_secs,
        res->error[0] != '\0' ? res->error : "(no driver error)");
    goto out;
  }

  snprintf(prev_end, sizeof(prev_end), "%s", range_start);

  for(uint32_t i = 0; i < res->rows && emitted < max_out; i++)
  {
    const char *row_first_ts = db_result_get(res, i, 0);
    const char *row_last_ts  = db_result_get(res, i, 1);

    if(row_first_ts == NULL || row_last_ts == NULL)
      continue;

    if(strcmp(row_first_ts, prev_end) > 0)
    {
      wm_coverage_t *g = &out[emitted++];

      memset(g, 0, sizeof(*g));
      g->market_id   = market_id;
      g->granularity = gran_secs;
      snprintf(g->first_ts, WM_COV_TS_SZ, "%s", prev_end);
      snprintf(g->last_ts,  WM_COV_TS_SZ, "%s", row_first_ts);
    }

    if(strcmp(row_last_ts, prev_end) > 0)
      snprintf(prev_end, sizeof(prev_end), "%s", row_last_ts);
  }

  if(emitted < max_out && strcmp(prev_end, range_end) < 0)
  {
    wm_coverage_t *g = &out[emitted++];

    memset(g, 0, sizeof(*g));
    g->market_id   = market_id;
    g->granularity = gran_secs;
    snprintf(g->first_ts, WM_COV_TS_SZ, "%s", prev_end);
    snprintf(g->last_ts,  WM_COV_TS_SZ, "%s", range_end);
  }

  if(emitted == max_out && strcmp(prev_end, range_end) < 0)
    snprintf(out[max_out - 1].last_ts, WM_COV_TS_SZ, "%s", range_end);

out:
  if(res     != NULL) db_result_free(res);
  if(e_start != NULL) mem_free(e_start);
  if(e_end   != NULL) mem_free(e_end);

  return(emitted);
}

// --------------------------------------------------------------------
// Admin iteration — read-only walk over existing coverage rows.
// Not under any lock; callers must be tolerant of rows appearing /
// disappearing mid-walk (the merge path can be running concurrently).
// --------------------------------------------------------------------
void
wm_coverage_iterate(wm_coverage_kind_t kind, int32_t market_id,
    int32_t gran_secs, wm_coverage_iter_cb_t cb, void *user)
{
  db_result_t *res = NULL;
  char         sql[512];
  int          n;

  if(cb == NULL)
    return;

  if(kind == WM_COV_TRADES)
  {
    n = snprintf(sql, sizeof(sql),
        "SELECT first_trade_id, last_trade_id,"
        "       to_char(first_ts AT TIME ZONE 'UTC',"
        "               'YYYY-MM-DD HH24:MI:SS.US') || '+00',"
        "       to_char(last_ts  AT TIME ZONE 'UTC',"
        "               'YYYY-MM-DD HH24:MI:SS.US') || '+00',"
        "       source"
        "  FROM wm_trade_coverage"
        " WHERE market_id = %" PRId32
        " ORDER BY first_trade_id",
        market_id);
  }

  else
  {
    if(gran_secs <= 0)
      return;

    n = snprintf(sql, sizeof(sql),
        "SELECT to_char(range_start AT TIME ZONE 'UTC',"
        "               'YYYY-MM-DD HH24:MI:SS.US') || '+00',"
        "       to_char(range_end   AT TIME ZONE 'UTC',"
        "               'YYYY-MM-DD HH24:MI:SS.US') || '+00'"
        "  FROM wm_candle_coverage"
        " WHERE market_id = %" PRId32
        "   AND granularity = %" PRId32
        " ORDER BY range_start",
        market_id, gran_secs);
  }

  if(n < 0 || (size_t)n >= sizeof(sql))
    return;

  res = db_result_alloc();

  if(res == NULL)
    return;

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, WM_DL_CTX,
        "coverage iterate query failed (kind=%d market=%" PRId32 "): %s",
        (int)kind, market_id,
        res->error[0] != '\0' ? res->error : "(no driver error)");
    db_result_free(res);
    return;
  }

  for(uint32_t i = 0; i < res->rows; i++)
  {
    wm_coverage_t row;
    const char   *v;

    memset(&row, 0, sizeof(row));
    row.market_id = market_id;

    if(kind == WM_COV_TRADES)
    {
      v = db_result_get(res, i, 0);
      if(v != NULL) row.first_trade_id = strtoll(v, NULL, 10);

      v = db_result_get(res, i, 1);
      if(v != NULL) row.last_trade_id  = strtoll(v, NULL, 10);

      v = db_result_get(res, i, 2);
      if(v != NULL) snprintf(row.first_ts, WM_COV_TS_SZ, "%s", v);

      v = db_result_get(res, i, 3);
      if(v != NULL) snprintf(row.last_ts,  WM_COV_TS_SZ, "%s", v);

      v = db_result_get(res, i, 4);
      if(v != NULL) snprintf(row.source, WM_COV_SOURCE_SZ, "%s", v);
    }

    else
    {
      row.granularity = gran_secs;

      v = db_result_get(res, i, 0);
      if(v != NULL) snprintf(row.first_ts, WM_COV_TS_SZ, "%s", v);

      v = db_result_get(res, i, 1);
      if(v != NULL) snprintf(row.last_ts,  WM_COV_TS_SZ, "%s", v);
    }

    cb(&row, user);
  }

  db_result_free(res);
}
