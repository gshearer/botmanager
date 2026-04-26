-- candle_upsample.sql -- whenmoon 1m-candle upsampler.
--
-- Requires Postgres 14+ for date_bin(). For older Postgres, swap the
-- bucket expression to the epoch-arithmetic form noted in the fallback
-- comment block at the bottom of this file.
--
-- Signature:
--   wm_candle_upsample(p_table_name TEXT,
--                      p_gran_secs  INT,
--                      p_start_ts   TIMESTAMPTZ,
--                      p_end_ts     TIMESTAMPTZ)
--   RETURNS TABLE (ts TIMESTAMPTZ, low/high/open/close/volume DOUBLE)
--
-- Invariants:
--   * p_table_name must be an existing table that matches the
--     wm_candles_<market_id>_60 shape (ts, low, high, open, close,
--     volume). %I-quoted identifiers in EXECUTE protect against
--     injection.
--   * p_gran_secs must be one of 60, 300, 900, 3600, 21600, 86400.
--     Any other value raises; callers must pre-validate.
--   * Window is [p_start_ts, p_end_ts) -- end exclusive. Rows outside
--     the window are filtered BEFORE grouping, so partial edge buckets
--     aggregate only the 1m rows that fall inside the window.
--   * Output ordering is ascending ts; no caller-side ORDER BY needed.

CREATE OR REPLACE FUNCTION wm_candle_upsample(
  p_table_name TEXT,
  p_gran_secs  INT,
  p_start_ts   TIMESTAMPTZ,
  p_end_ts     TIMESTAMPTZ
) RETURNS TABLE (
  ts     TIMESTAMPTZ,
  low    DOUBLE PRECISION,
  high   DOUBLE PRECISION,
  open   DOUBLE PRECISION,
  close  DOUBLE PRECISION,
  volume DOUBLE PRECISION
) LANGUAGE plpgsql AS $$
BEGIN
  IF p_gran_secs NOT IN (60, 300, 900, 3600, 21600, 86400) THEN
    RAISE EXCEPTION
      'wm_candle_upsample: gran_secs=% not in Coinbase ladder',
      p_gran_secs;
  END IF;

  IF p_gran_secs = 60 THEN
    RETURN QUERY EXECUTE format(
      'SELECT ts, low, high, open, close, volume'
      '  FROM %I'
      ' WHERE ts >= $1 AND ts < $2'
      ' ORDER BY ts',
      p_table_name
    ) USING p_start_ts, p_end_ts;
    RETURN;
  END IF;

  RETURN QUERY EXECUTE format(
    'SELECT date_bin(make_interval(secs => $3), c.ts,'
    '                TIMESTAMPTZ ''1970-01-01 00:00:00+00'') AS bucket_ts,'
    '       MIN(c.low)                                     AS low,'
    '       MAX(c.high)                                    AS high,'
    '       (array_agg(c.open  ORDER BY c.ts ASC))[1]      AS open,'
    '       (array_agg(c.close ORDER BY c.ts DESC))[1]     AS close,'
    '       SUM(c.volume)                                  AS volume'
    '  FROM %I c'
    ' WHERE c.ts >= $1 AND c.ts < $2'
    ' GROUP BY bucket_ts'
    ' ORDER BY bucket_ts',
    p_table_name
  ) USING p_start_ts, p_end_ts, p_gran_secs;
END;
$$;

-- Fallback for Postgres < 14 (no date_bin). Replace the bucket
-- expression in the second RETURN QUERY EXECUTE above with:
--
--   to_timestamp(floor(extract(epoch from c.ts) / $3) * $3)
--       AT TIME ZONE 'UTC' AS bucket_ts
--
-- Semantics match date_bin() anchored at the 1970-01-01 UTC epoch.
