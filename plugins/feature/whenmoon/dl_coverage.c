// botmanager — MIT
// Whenmoon downloader candle coverage store: atomic merge + gap walk.

#define WHENMOON_INTERNAL
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
wm_cov_advisory_key(int32_t market_id, int32_t gran_secs,
    char *out, size_t cap)
{
  snprintf(out, cap, "%s%" PRId32 ":%" PRId32,
      WM_COV_LOCK_TAG, market_id, gran_secs);
}

// --------------------------------------------------------------------
// iv self-consistency. A page-construction bug could let first/last
// bounds invert; catch the malformed iv at the boundary so it can't
// reach the merge SQL and poison the result via LEAST/GREATEST.
// --------------------------------------------------------------------
static bool
wm_cov_iv_consistent(const wm_coverage_t *iv)
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
// Candles 30-day sanity precheck. A malformed page can drag an
// existing row's bounds across multiple years. Reject the merge if
// any candidate touching row's range extends more than
// WM_COV_TS_SANITY_DAYS outside iv on either side; the widened 2*gran
// touching gap is generous enough for normal pagination while leaving
// the multi-year jump assertion meaningful.
// --------------------------------------------------------------------
static bool
wm_cov_check_sanity(const wm_coverage_t *iv)
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
//   * Any row in wm_candle_coverage that overlapped or touched `iv`
//     has been deleted.
//   * Exactly one row has been inserted whose interval is the union
//     of iv and the deleted rows' intervals.
//   * Per (market, granularity) serialisation held via
//     pg_advisory_xact_lock for the whole BEGIN...COMMIT.
// --------------------------------------------------------------------
static bool
wm_cov_merge_tx(const wm_coverage_t *iv)
{
  db_result_t *res        = NULL;
  char        *e_first_ts = NULL;
  char        *e_last_ts  = NULL;
  char         sql[4096];
  char         lock_key[96];
  int32_t      touch_gap  = iv->granularity * 2;
  bool         ok = FAIL;
  int          n;

  wm_cov_advisory_key(iv->market_id, iv->granularity,
      lock_key, sizeof(lock_key));

  e_first_ts = db_escape(iv->first_ts);
  e_last_ts  = db_escape(iv->last_ts);

  if(e_first_ts == NULL || e_last_ts == NULL)
    goto out;

  // Candles touching predicate (timestamp-only — no ID axis to
  // cross-check). Widened to 2*gran so adjacent paginations merge
  // even when the boundary candle is half-formed; the sanity
  // precheck (wm_cov_check_sanity) has already rejected merges that
  // would jump >30 days.
  //   WHERE market_id = :mid
  //     AND granularity = :gran
  //     AND range_start <= :iv_end   + :gran*2 seconds
  //     AND range_end   >= :iv_start - :gran*2 seconds
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

  if(n < 0 || (size_t)n >= sizeof(sql))
  {
    clam(CLAM_WARN, WM_DL_CTX,
        "coverage merge sql truncated (market=%" PRId32 " gran=%" PRId32 ")",
        iv->market_id, iv->granularity);
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
        "coverage merge failed (market=%" PRId32 " gran=%" PRId32 "): %s",
        iv->market_id, iv->granularity,
        res->error[0] != '\0' ? res->error : "(no driver error)");
  }

out:
  if(res        != NULL) db_result_free(res);
  if(e_first_ts != NULL) mem_free(e_first_ts);
  if(e_last_ts  != NULL) mem_free(e_last_ts);

  return(ok);
}

bool
wm_coverage_add(const wm_coverage_t *iv)
{
  if(iv == NULL || iv->granularity <= 0)
    return(FAIL);

  if(wm_cov_iv_consistent(iv) != SUCCESS)
    return(FAIL);

  if(wm_cov_check_sanity(iv) != SUCCESS)
    return(FAIL);

  return(wm_cov_merge_tx(iv));
}

// --------------------------------------------------------------------
// Gap computation — walker shape:
//
//   rows := SELECT range_start, range_end
//             FROM wm_candle_coverage
//            WHERE market_id   = :mid
//              AND granularity = :gran
//              AND range_start < :range_end
//              AND range_end   > :range_start
//            ORDER BY range_start
//
//   prev_end := range_start
//   for row in rows:
//     if row.range_start > prev_end:
//       emit gap [prev_end, row.range_start)
//     prev_end := MAX(prev_end, row.range_end)
//   if prev_end < range_end:
//     emit gap [prev_end, range_end)
//
// Lexicographic compare on canonical TIMESTAMPTZ UTC strings is
// order-preserving, so a cheap char compare suffices.
// --------------------------------------------------------------------

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
