#!/usr/bin/env python3
"""export_candles.py — WM-EDGE-2 step 1: universe OHLCV export to Parquet.

Aggregates the 1-minute candle tables (`wm_candles_<id>`) server-side to
1h and 1d bars (UTC buckets) and writes two long-format Parquet files:

    btdata/research/ohlcv_1h.parquet
    btdata/research/ohlcv_1d.parquet

Columns: market_id, pair, ts (UTC), open, high, low, close, volume, n_1m
(count of 1m bars in the bucket — a data-quality signal; a 1h bar built
from 3 minutes of data is not a real bar).

The export carries FULL history including post-cutoff bars; the WM-RIGOR-6
research cutoff is enforced at LOAD time by rig.py (mirroring the C-side
design: full corpus on disk, guard at run). Do not add a cutoff here.

DB credentials come from ~/.config/botmanager/botman.conf (DBHOST= style
lines); nothing is hardcoded. Aggregation happens in Postgres via psql
COPY CSV (one table at a time to bound remote load) and lands in pandas
only for the Parquet write.

Usage:
    tools/research/.venv/bin/python tools/research/export_candles.py
    ... [--grains 1h,1d] [--ids 1,2,4] [--out btdata/research]

The universe is every enabled-or-not coinbase/USD row in wm_market; other
exchanges and non-USD quotes are excluded (research scope = the EDGE-1
Coinbase USD universe).
"""

import argparse
import io
import os
import re
import subprocess
import sys

import pandas as pd

CONF_PATH = os.path.expanduser("~/.config/botmanager/botman.conf")
GRAIN_TRUNC = {"1h": "hour", "1d": "day"}


def read_db_conf(path=CONF_PATH):
    """Parse the KEY="value" lines botman.conf uses for DB settings."""
    conf = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            m = re.match(r'^(DB[A-Z]+)="?([^"\n]*)"?\s*$', line)
            if m:
                conf[m.group(1)] = m.group(2)
    missing = {"DBHOST", "DBPORT", "DBNAME", "DBUSER", "DBPASS"} - set(conf)
    if missing:
        sys.exit(f"botman.conf missing keys: {sorted(missing)}")
    return conf


def psql_csv(conf, query):
    """Run one read-only query through psql, return its CSV output."""
    env = dict(os.environ, PGPASSWORD=conf["DBPASS"])
    cmd = [
        "psql",
        "-h", conf["DBHOST"], "-p", conf["DBPORT"],
        "-U", conf["DBUSER"], "-d", conf["DBNAME"],
        "-Atc", f"copy ({query}) to stdout with csv header",
    ]
    out = subprocess.run(cmd, env=env, check=True, capture_output=True)
    return out.stdout.decode()


def universe(conf):
    """[(market_id, pair)] for every coinbase/USD market, id order."""
    csv = psql_csv(conf, (
        "select id, base_asset || '-' || quote_asset as pair from wm_market "
        "where exchange = 'coinbase' and quote_asset = 'usd' order by id"
    ))
    frame = pd.read_csv(io.StringIO(csv))
    return list(frame.itertuples(index=False, name=None))


def export_grain(conf, markets, grain, out_dir):
    """One Parquet file for one grain, aggregated table by table."""
    trunc = GRAIN_TRUNC[grain]
    frames = []
    for market_id, pair in markets:
        csv = psql_csv(conf, (
            f"select date_trunc('{trunc}', ts at time zone 'UTC') as ts, "
            f"(array_agg(open order by ts))[1] as open, "
            f"max(high) as high, min(low) as low, "
            f"(array_agg(close order by ts desc))[1] as close, "
            f"sum(volume) as volume, count(*) as n_1m "
            f"from wm_candles_{market_id} group by 1 order by 1"
        ))
        frame = pd.read_csv(io.StringIO(csv), parse_dates=["ts"])
        frame.insert(0, "market_id", market_id)
        frame.insert(1, "pair", pair)
        frames.append(frame)
        print(f"  {pair:<10} {grain}: {len(frame):>7} bars "
              f"({frame.ts.min():%Y-%m-%d} .. {frame.ts.max():%Y-%m-%d})")
    combined = pd.concat(frames, ignore_index=True)
    path = os.path.join(out_dir, f"ohlcv_{grain}.parquet")
    combined.to_parquet(path, index=False)
    print(f"wrote {path}: {len(combined)} rows, {len(frames)} pairs")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--grains", default="1h,1d")
    parser.add_argument("--ids", default="",
                        help="comma-separated market ids (default: universe)")
    parser.add_argument("--out", default="btdata/research")
    args = parser.parse_args()

    conf = read_db_conf()
    markets = universe(conf)
    if args.ids:
        keep = {int(x) for x in args.ids.split(",")}
        markets = [m for m in markets if m[0] in keep]
    os.makedirs(args.out, exist_ok=True)

    print(f"exporting {len(markets)} pairs: "
          + " ".join(pair for _, pair in markets))
    for grain in args.grains.split(","):
        if grain not in GRAIN_TRUNC:
            sys.exit(f"unknown grain '{grain}' (use 1h,1d)")
        export_grain(conf, markets, grain, args.out)


if __name__ == "__main__":
    main()
