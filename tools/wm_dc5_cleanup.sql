-- WM-DC-5 — one-shot trade-data cleanup.
--
-- Purpose: reset wm_trades_1 + wm_trade_coverage to a known-good
-- baseline after WM-DC-1..4 ship, removing the corruption documented
-- in the 2026-04-29 audit (memory: finding_trade_data_corruption.md).
--
-- Pre-conditions before running:
--   1. WM-DC-1..4 are live in the running daemon.
--   2. The whenmoon market is stopped:
--        /whenmoon market stop coinbase-btc-usd
--      so live WS writes are not racing the DELETEs.
--   3. No download jobs are in-flight (operator pauses or stops the
--      daemon's whenmoon plugin while running this script).
--
-- Run as the database owner (or a role with DELETE privileges on
-- wm_trades_1 + wm_trade_coverage):
--   psql -h <host> -U <user> -d <db> -f tools/wm_dc5_cleanup.sql
--
-- The script wraps deletes in a single transaction so any error
-- aborts cleanly. Re-running on already-clean state is a no-op:
-- the WHERE clauses degrade to "no rows match."
--
-- Cleanup scope (BTC-USD / market_id = 1):
--   * wm_trade_coverage rows for market_id=1: deleted (regenerated
--     from real data via the post-fix download pipeline).
--   * wm_trades_1 source=1 (live WS): deleted (mixed sandbox/prod
--     ID space; WM-DC-1 isolation prevents recurrence).
--   * wm_trades_1 source=0 in [2026-04-23, 2026-04-27): deleted
--     (small pre-WM-DC-1 REST cluster, cheap to re-fetch under the
--     post-fix pipeline).
--   * wm_trades_1 source=0 with ts < 2026-01-01: KEPT (March-2024
--     cluster predates the merge bug events).
--
-- Out-of-scope (operator's call after this runs):
--   * wm_candles_1_60 + wm_candle_coverage — candle data is
--     derived from /products/<id>/candles, not from the trade
--     table; the merge bug could have affected coverage rows
--     but the underlying candles are independent. Operator may
--     truncate + re-download per WM-DC-5 step 6.
--   * wm_backtest_run rows — preserved for run history (timestamps
--     remain meaningful even if trade-derived metrics are stale).
--   * Sandbox markets created by WM-DC-1 — the post-fix pipeline
--     handles them correctly from row zero.

\set ON_ERROR_STOP on

\echo
\echo '== WM-DC-5 pre-cleanup audit =================================='
\echo

\echo '-- wm_market --'
SELECT id, exchange, base_asset, quote_asset, exchange_symbol,
       enabled,
       to_char(first_seen AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS')
         AS first_seen_utc
  FROM wm_market
 ORDER BY id;

\echo
\echo '-- wm_trade_coverage --'
SELECT market_id, first_trade_id, last_trade_id,
       to_char(first_ts AT TIME ZONE 'UTC',
               'YYYY-MM-DD HH24:MI:SS') AS first_ts_utc,
       to_char(last_ts  AT TIME ZONE 'UTC',
               'YYYY-MM-DD HH24:MI:SS') AS last_ts_utc,
       source
  FROM wm_trade_coverage
 ORDER BY market_id, first_trade_id;

\echo
\echo '-- wm_candle_coverage --'
SELECT market_id, granularity,
       to_char(range_start AT TIME ZONE 'UTC',
               'YYYY-MM-DD HH24:MI:SS') AS range_start_utc,
       to_char(range_end   AT TIME ZONE 'UTC',
               'YYYY-MM-DD HH24:MI:SS') AS range_end_utc
  FROM wm_candle_coverage
 ORDER BY market_id, granularity, range_start;

\echo
\echo '-- wm_trades_1 distribution by source --'
SELECT source,
       count(*) AS n,
       min(trade_id) AS min_id,
       max(trade_id) AS max_id,
       to_char(min(ts) AT TIME ZONE 'UTC',
               'YYYY-MM-DD HH24:MI:SS') AS min_ts_utc,
       to_char(max(ts) AT TIME ZONE 'UTC',
               'YYYY-MM-DD HH24:MI:SS') AS max_ts_utc
  FROM wm_trades_1
 GROUP BY source
 ORDER BY source;

\echo
\echo '== WM-DC-5 cleanup transaction ================================'
\echo

BEGIN;

\echo '  step 1: drop wm_trade_coverage rows for market_id=1'
DELETE FROM wm_trade_coverage WHERE market_id = 1;

\echo '  step 2: drop wm_trades_1 source=1 (live WS, mixed sandbox/prod)'
DELETE FROM wm_trades_1 WHERE source = 1;

\echo '  step 3: drop wm_trades_1 source=0 in [2026-04-23, 2026-04-27)'
DELETE FROM wm_trades_1
 WHERE source = 0
   AND ts >= TIMESTAMPTZ '2026-04-23 00:00:00+00'
   AND ts <  TIMESTAMPTZ '2026-04-27 00:00:00+00';

COMMIT;

\echo
\echo '== WM-DC-5 post-cleanup audit ================================='
\echo

\echo '-- wm_trade_coverage --'
SELECT market_id, first_trade_id, last_trade_id,
       to_char(first_ts AT TIME ZONE 'UTC',
               'YYYY-MM-DD HH24:MI:SS') AS first_ts_utc,
       to_char(last_ts  AT TIME ZONE 'UTC',
               'YYYY-MM-DD HH24:MI:SS') AS last_ts_utc,
       source
  FROM wm_trade_coverage
 ORDER BY market_id, first_trade_id;

\echo
\echo '-- wm_trades_1 distribution by source --'
SELECT source,
       count(*) AS n,
       min(trade_id) AS min_id,
       max(trade_id) AS max_id,
       to_char(min(ts) AT TIME ZONE 'UTC',
               'YYYY-MM-DD HH24:MI:SS') AS min_ts_utc,
       to_char(max(ts) AT TIME ZONE 'UTC',
               'YYYY-MM-DD HH24:MI:SS') AS max_ts_utc
  FROM wm_trades_1
 GROUP BY source
 ORDER BY source;

\echo
\echo '== Next steps ================================================='
\echo
\echo '  1. Confirm wm_trades_1 holds only the March-2024 cluster:'
\echo '       source=0, ~429K rows, IDs in 93,798,832..94,230,180,'
\echo '       ts range 2024-03-02..2024-03-13.'
\echo '  2. Restart the market: /whenmoon market start coinbase-btc-usd'
\echo '  3. Regenerate the coverage row:'
\echo '       /whenmoon download trades coinbase-btc-usd 03/01/2024 03/15/2024'
\echo '     The post-fix WM-DC-2/3 pipeline produces a single coverage row'
\echo '     with proportional ID + TS bounds (no 6-year claim from a'
\echo '     2-week window).'
\echo '  4. Verify gaps: /show whenmoon download trades coinbase-btc-usd'
\echo '     The remaining multi-year window must be reported as un-covered.'
\echo
