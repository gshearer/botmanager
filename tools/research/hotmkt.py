#!/usr/bin/env python3
"""hotmkt.py — WM-HOTMKT-1: hot-flag historical validation.

PRE-REGISTRATION (written before any result was computed — this docstring
is the hypothesis record; do not edit it to fit results):

  Question. MW-3 already emits `mw.<exch>.hot.*`; before ANY automation
  (WM-HOTMKT-2), is a HOT flag a fade or a chase, at what horizon?
  Chasing spikes is adverse selection until proven otherwise — the null
  hypothesis is "announce-only".

  Thesis being tested (two-sided by design): post-flag forward returns
  are nonzero net of costs in SOME direction at SOME horizon. "Chase"
  = long cells pass; "fade" = short cells pass; neither = announce-only.

DETECTOR RECONSTRUCTION (faithful to mw.c at `6e0d294`; live KVs checked
2026-07-18 — every threshold KV is 0 = "use default", so the reconstruction
pins the compile-time defaults; mw.coinbase.enabled is currently 0, i.e.
this study decides whether the detector EARNS its enable):

  Cadence: 1-minute bars = the live 60s poll (MW_POLL_SEC_DEFAULT).
  Price proxy: 1m close (live uses last-trade ticker price). Missing
  minutes: close forward-fills (a stale ticker also repeats its price),
  high/low fill with the ffilled close, volume fills 0.

  Signals, exactly the five MW_TRIG_* bits:
    PCT_24H : |close[t]/close[t-1440] - 1| * 100 >= 5.00
    VELOCITY: |close[t]/close[t-10]   - 1| * 100 >= 2.00   (10-min window)
    BRK_HI  : high[t] > max(high[t-1440..t-1]) and close[t] >= that max
    BRK_LO  : low[t]  < min(low [t-1440..t-1]) and close[t] <= that min
    VOL_Z   : qvol24[t] z-scored vs the previous 59 minute-samples
              (>=10 finite, sample stdev, stdev<1e-9 -> undefined),
              positive side only, z >= 3.00
  where qvol24 = 24h rolling sum of (volume * close) — quote volume
  approximated from base-unit candle volume; the live detector reads the
  exchange's own 24h quote-vol field. Deviation disclosed.

  Gate + state machine (mw_detect_pair verbatim): a COLD pair with 24h
  quote vol < $1M cannot flag; COLD->HOT on any trigger bit; HOT->COLD
  only after >=300s dwell AND every signal below HALF its threshold
  (50% hysteresis; BRK bits carry no threshold and simply require a new
  extreme each minute). Events in this study = COLD->HOT entries only.
  Warmup: no bit fires before 1440 minutes of history (min_periods) —
  live needs its ring + the exchange's own 24h fields, same spirit.

  Integer-floor threshold comparisons in C ((uint64)(fabs(x)*100) >=
  thresh) are reproduced as plain float >= — equivalent up to 1e-2
  rounding at the boundary; disclosed, not material.

DATA + CUTOFF. Per-pair 1m candles pulled straight from wm_candles_<id>
(the EDGE-1 coinbase-USD universe from wm_market, same query as
export_candles.universe). The WM-RIGOR-6 research cutoff is enforced IN
THE SQL (ts < 2025-03-31 00:00:00+00, asserted again after parse); this
module has NO holdout path at all — post-cutoff bars never reach it.

EXECUTION MODEL (the ROUND 6 lesson is not optional):
  Lag-1 from the start: the flag is seen on bar t; entry is the close of
  the first REAL traded 1m bar in (t, t+5]; no real bar within 5 minutes
  drops the event (a fill you could not have gotten is not a fill).
  Forward return at horizon h in {5m, 1h, 4h, 1d} = close[entry+h] /
  close[entry] - 1 on the ffilled minute calendar; events whose window
  crosses the pair's data end are dropped at that horizon. The
  free-look variant (entry at the flag bar's own close) is computed as a
  DIAGNOSTIC ONLY — its delta vs lag-1 measures how much of any apparent
  edge is unfillable spike, and it is not gate-eligible.

  Overlap policy, per (pair, horizon): events are taken first-come;
  an event whose entry falls before the previous taken event's horizon
  end is dropped (one position per pair per horizon, like a real book).
  Cross-pair simultaneity is kept and reported (breadth is information).

COSTS. Round trip = 2 sides x (5 fee + 5 slip) bps x friction; frictions
{1x, 2x, 4x}, judged at 1x. Shorts additionally pay borrow at
{5, 10, 15} bps/day pro-rated by horizon, judged at 10 (the EDGE-4
convention). Net long = fwd - rt_cost; net short = -fwd - rt_cost -
borrow*h/1d.

FOLDS + JUDGED CELLS + GATE (the only gate-eligible cells; everything
else below is descriptive and CANNOT pass this chunk):

  Folds: rig.fold_windows over the union daily calendar of the universe
  (official recipe: 365d burn-in, 120d test, 120d step); events assign
  by entry date. Per-fold statistic: MEAN NET RETURN PER EVENT in the
  fold (edge per trade). A fold qualifies for a cell iff it holds >= 5
  events; a cell is judgeable iff it has >= 10 qualifying folds AND
  >= 200 pooled events (else "insufficient sample" — cannot pass).

  Judged cells: direction {long, short} x horizon {5m, 1h, 4h, 1d} = 8
  cells, ALL-trigger pooled events, at 1x friction + 10 bps/day borrow.

  GATE per cell (ALL required):
    robust_ratio(fold means) >= 0.3
    pos_frac >= 0.75
    pooled net mean > 0
    stability: over 200 seeded half-splits (rig's seed) of the
      qualifying folds, fraction with BOTH half-means positive >= 0.70
  Any cell passing => verdict "exploitable (<direction>@<horizon>)" and
  WM-HOTMKT-2 designs around it. No cell passing => "announce-only".
  Multiplicity is disclosed by construction: all 8 judged cells + every
  stress cell land in the census (family "hotmkt-mw3").

DESCRIPTIVE-ONLY OUTPUTS (explicitly not gate-eligible; a shiny subset
here becomes a NEW pre-registered hypothesis, never a promotion of this
one): per-trigger-class breakdown (single-bit classes + "multi"),
per-pair means, free-look-vs-lag-1 delta, HOT episode duration stats,
events/day distribution, 2x/4x friction and 5/15 borrow stress.

Usage:
    tools/research/.venv/bin/python tools/research/hotmkt.py
        [--ids 1,2,...] [--smoke] [--out btdata/research]

  --smoke prints MECHANICAL integrity only (bar/event/drop counts,
  episode durations, trigger classes) and suppresses every return
  number — so plumbing can be debugged without peeking at results.
"""

import argparse
import gc
import io
import json
import os
import sys

import numpy as np
import pandas as pd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig                                              # noqa: E402
from export_candles import read_db_conf, psql_csv, universe  # noqa: E402

# Detector constants — mw.c defaults, live-verified all-default 2026-07-18.
PCT_24H_THRESH = 5.00        # %
VEL_THRESH = 2.00            # %
VEL_WINDOW_MIN = 10
VOL_Z_THRESH = 3.00          # sigma
VOL_Z_BASELINE = 59          # ring_n 60 minus the just-pushed sample
VOL_Z_MIN_SAMPLES = 10
MIN_VOL_USD = 1_000_000.0
COOLDOWN_MIN = 5             # 300 s
HYST_NUM, HYST_DEN = 1, 2
DAY_MIN = 1440

TRIG_PCT, TRIG_VEL, TRIG_HI, TRIG_LO, TRIG_VZ = 1, 2, 4, 8, 16
TRIG_NAMES = {TRIG_PCT: "pct_24h", TRIG_VEL: "velocity",
              TRIG_HI: "brk_hi", TRIG_LO: "brk_lo", TRIG_VZ: "vol_z"}

HORIZONS = {"5m": 5, "1h": 60, "4h": 240, "1d": DAY_MIN}
FRICTIONS = (1.0, 2.0, 4.0)
BORROWS_BPS_DAY = (5.0, 10.0, 15.0)
JUDGED_FRICTION = 1.0
JUDGED_BORROW = 10.0
ENTRY_SEARCH_MIN = 5         # real-bar window for the lag-1 fill

MIN_EVENTS_PER_FOLD = 5
MIN_QUALIFYING_FOLDS = 10
MIN_POOLED_EVENTS = 200
GATE_RR = 0.3
GATE_POS_FRAC = 0.75
STAB_SPLITS = 200
STAB_BOTH_POS = 0.70

CUTOFF_SQL = "2025-03-31 00:00:00+00"


# ---------------------------------------------------------------- loading

def load_pair_1m(conf, market_id):
    """Raw pre-cutoff 1m bars for one market, ts-indexed UTC-naive."""
    csv = psql_csv(conf, (
        f"select ts at time zone 'UTC' as ts, open, high, low, close, "
        f"volume from wm_candles_{market_id} "
        f"where ts < timestamptz '{CUTOFF_SQL}' order by ts"
    ))
    frame = pd.read_csv(io.StringIO(csv), parse_dates=["ts"])
    frame = frame.set_index("ts")
    assert frame.index.max() < rig.RESEARCH_CUTOFF, \
        f"market {market_id}: post-cutoff bar leaked through the SQL guard"
    return frame


# --------------------------------------------------------------- detector

def replay_pair(bars):
    """Replay the MW-3 state machine over one pair's 1m history.

    Returns (events, episodes, cal, close, real_mask) where events is a
    list of (flag_idx, trigger_bits) into the minute calendar `cal`,
    episodes is a list of HOT episode lengths in minutes, close is the
    ffilled minute-close series and real_mask marks true traded bars.
    """
    cal = pd.date_range(bars.index.min(), bars.index.max(), freq="min")
    close = bars["close"].reindex(cal).ffill()
    high = bars["high"].reindex(cal).fillna(close)
    low = bars["low"].reindex(cal).fillna(close)
    vol = bars["volume"].reindex(cal).fillna(0.0)
    real_mask = np.zeros(len(cal), dtype=bool)
    real_mask[cal.get_indexer(bars.index)] = True

    pct24 = (close / close.shift(DAY_MIN) - 1.0) * 100.0
    velocity = (close / close.shift(VEL_WINDOW_MIN) - 1.0) * 100.0
    prev_hi = high.rolling(DAY_MIN, min_periods=DAY_MIN).max().shift(1)
    prev_lo = low.rolling(DAY_MIN, min_periods=DAY_MIN).min().shift(1)
    qvol24 = (vol * close).rolling(DAY_MIN, min_periods=DAY_MIN).sum()

    base = qvol24.shift(1)
    mean_b = base.rolling(VOL_Z_BASELINE,
                          min_periods=VOL_Z_MIN_SAMPLES).mean()
    std_b = base.rolling(VOL_Z_BASELINE,
                         min_periods=VOL_Z_MIN_SAMPLES).std(ddof=1)
    vol_z = ((qvol24 - mean_b) / std_b).where(std_b >= 1e-9)

    def trig_bits(scale):
        bits = np.zeros(len(cal), dtype=np.uint8)
        bits |= np.where(pct24.abs() >= PCT_24H_THRESH * scale,
                         TRIG_PCT, 0).astype(np.uint8)
        bits |= np.where(velocity.abs() >= VEL_THRESH * scale,
                         TRIG_VEL, 0).astype(np.uint8)
        bits |= np.where((high > prev_hi) & (close >= prev_hi),
                         TRIG_HI, 0).astype(np.uint8)
        bits |= np.where((low < prev_lo) & (close <= prev_lo),
                         TRIG_LO, 0).astype(np.uint8)
        bits |= np.where((vol_z > 0.0) & (vol_z >= VOL_Z_THRESH * scale),
                         TRIG_VZ, 0).astype(np.uint8)
        return bits

    full = trig_bits(1.0)
    hyst = trig_bits(HYST_NUM / HYST_DEN)
    vol_ok = (qvol24 >= MIN_VOL_USD).to_numpy()

    events, episodes = [], []
    state_hot = False
    changed = 0
    for i in range(len(cal)):
        if not state_hot:
            if full[i] and vol_ok[i]:
                state_hot = True
                changed = i
                events.append((i, int(full[i])))
        else:
            if (i - changed) >= COOLDOWN_MIN and hyst[i] == 0:
                episodes.append(i - changed)
                state_hot = False
                changed = i
    return events, episodes, cal, close, real_mask


def event_records(pair, events, cal, close, real_mask):
    """Per-event rows: entry fill + gross forward returns per horizon."""
    n = len(cal)
    close_np = close.to_numpy()
    rows = []
    for flag_idx, bits in events:
        entry_idx = -1
        for j in range(flag_idx + 1,
                      min(flag_idx + 1 + ENTRY_SEARCH_MIN, n)):
            if real_mask[j]:
                entry_idx = j
                break
        if entry_idx < 0:
            rows.append({"pair": pair, "ts_flag": cal[flag_idx],
                         "trigger": bits, "filled": False})
            continue
        row = {"pair": pair, "ts_flag": cal[flag_idx],
               "ts_entry": cal[entry_idx], "trigger": bits, "filled": True}
        entry_px = close_np[entry_idx]
        flag_px = close_np[flag_idx]
        for label, h in HORIZONS.items():
            if entry_idx + h < n:
                row[f"fwd_{label}"] = close_np[entry_idx + h] / entry_px - 1.0
            if flag_idx + h < n:          # free-look diagnostic
                row[f"look_{label}"] = close_np[flag_idx + h] / flag_px - 1.0
        rows.append(row)
    return rows


# ---------------------------------------------------------------- scoring

def round_trip_cost(friction):
    return 2.0 * (rig.FEE_BPS + rig.SLIP_BPS) * friction / 1e4


def net_returns(gross, direction, horizon_min, friction, borrow_bps_day):
    cost = round_trip_cost(friction)
    if direction == "long":
        return gross - cost
    borrow = borrow_bps_day / 1e4 * (horizon_min / DAY_MIN)
    return -gross - cost - borrow


def dedup_overlap(events, horizon_min):
    """First-come, one position per pair per horizon."""
    kept = []
    horizon = pd.Timedelta(minutes=horizon_min)
    last_exit = {}
    for row in events.sort_values("ts_entry").itertuples():
        if row.pair in last_exit and row.ts_entry < last_exit[row.pair]:
            continue
        last_exit[row.pair] = row.ts_entry + horizon
        kept.append(row.Index)
    return events.loc[kept]


def stability(fold_means, seed=1743379200):
    """Fraction of seeded half-splits with BOTH half-means positive."""
    values = np.asarray(fold_means, dtype=float)
    n = len(values)
    if n < 4:
        return 0.0
    rng = np.random.default_rng(seed)
    hits = 0
    for _ in range(STAB_SPLITS):
        perm = rng.permutation(n)
        a, b = values[perm[: n // 2]], values[perm[n // 2:]]
        hits += (a.mean() > 0.0) and (b.mean() > 0.0)
    return hits / STAB_SPLITS


def score_cell(events, folds, direction, label, horizon_min,
               friction, borrow):
    """Fold-level scoring of one (direction, horizon, cost) cell."""
    col = f"fwd_{label}"
    usable = events[events["filled"] & events[col].notna()].copy()
    usable = dedup_overlap(usable, horizon_min)
    usable["net"] = net_returns(usable[col].to_numpy(), direction,
                                horizon_min, friction, borrow)
    fold_means, fold_counts = [], []
    for start, end in folds:
        mask = (usable["ts_entry"] >= start) & (usable["ts_entry"] < end)
        count = int(mask.sum())
        if count >= MIN_EVENTS_PER_FOLD:
            fold_means.append(float(usable.loc[mask, "net"].mean()))
            fold_counts.append(count)
    pooled = len(usable)
    cell = {
        "direction": direction, "horizon": label,
        "friction": friction, "borrow_bps_day": borrow,
        "n_events": pooled,
        "n_qualifying_folds": len(fold_means),
        "gross_mean": float(usable[col].mean()) if pooled else None,
        "gross_median": float(usable[col].median()) if pooled else None,
        "net_mean": float(usable["net"].mean()) if pooled else None,
    }
    if fold_means:
        cell.update(rig.fold_metrics(fold_means))
        cell["stability_both_pos"] = stability(fold_means)
    judgeable = (len(fold_means) >= MIN_QUALIFYING_FOLDS
                 and pooled >= MIN_POOLED_EVENTS)
    cell["judgeable"] = judgeable
    cell["gate_pass"] = bool(
        judgeable
        and cell.get("robust_ratio", -1.0) >= GATE_RR
        and cell.get("pos_frac", 0.0) >= GATE_POS_FRAC
        and (cell.get("net_mean") or 0.0) > 0.0
        and cell.get("stability_both_pos", 0.0) >= STAB_BOTH_POS)
    return cell


# ------------------------------------------------------------ description

def trigger_class(bits):
    names = [name for bit, name in TRIG_NAMES.items() if bits & bit]
    return names[0] if len(names) == 1 else "multi"


def describe(events, episodes_all):
    filled = events[events["filled"]]
    per_class = filled.groupby(
        filled["trigger"].map(trigger_class)).size().to_dict()
    per_pair = filled.groupby("pair").size().to_dict()
    per_day = filled.groupby(filled["ts_entry"].dt.floor("D")).size()
    episodes = np.asarray(episodes_all, dtype=float)
    return {
        "n_flags": int(len(events)),
        "n_filled": int(len(filled)),
        "n_unfillable": int((~events["filled"]).sum()),
        "trigger_classes": per_class,
        "events_per_pair": per_pair,
        "events_per_active_day": {
            "mean": float(per_day.mean()), "max": int(per_day.max()),
            "days_with_events": int(len(per_day)),
        } if len(per_day) else {},
        "episode_minutes": {
            "n": int(len(episodes)),
            "median": float(np.median(episodes)) if len(episodes) else None,
            "p90": float(np.percentile(episodes, 90))
            if len(episodes) else None,
        },
    }


def freelook_delta(events):
    """Diagnostic: mean free-look minus lag-1 gross, per horizon (bps)."""
    filled = events[events["filled"]]
    out = {}
    for label in HORIZONS:
        both = filled[filled[f"fwd_{label}"].notna()
                      & filled[f"look_{label}"].notna()]
        if len(both):
            delta = (both[f"look_{label}"] - both[f"fwd_{label}"]).mean()
            out[label] = float(delta * 1e4)
    return out


# ------------------------------------------------------------------- main

def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ids", default="",
                        help="comma-separated market ids (default: universe)")
    parser.add_argument("--smoke", action="store_true",
                        help="mechanical integrity only — no return numbers")
    parser.add_argument("--out", default="btdata/research")
    args = parser.parse_args()

    conf = read_db_conf()
    markets = universe(conf)
    if args.ids:
        keep = {int(x) for x in args.ids.split(",")}
        markets = [m for m in markets if m[0] in keep]

    all_rows, episodes_all = [], []
    span_min, span_max = None, None
    for market_id, pair in markets:
        bars = load_pair_1m(conf, market_id)
        if len(bars) < DAY_MIN * 2:
            print(f"  {pair:<10} {len(bars):>9} bars — too short, skipped")
            continue
        events, episodes, cal, close, real_mask = replay_pair(bars)
        rows = event_records(pair, events, cal, close, real_mask)
        all_rows.extend(rows)
        episodes_all.extend(episodes)
        span_min = cal.min() if span_min is None else min(span_min, cal.min())
        span_max = cal.max() if span_max is None else max(span_max, cal.max())
        print(f"  {pair:<10} {len(bars):>9} bars "
              f"({cal.min():%Y-%m-%d}..{cal.max():%Y-%m-%d})  "
              f"flags={len(events):>5}  episodes={len(episodes):>5}")
        del bars, events, episodes, cal, close, real_mask
        gc.collect()

    events = pd.DataFrame(all_rows)
    for label in HORIZONS:
        for prefix in ("fwd", "look"):
            col = f"{prefix}_{label}"
            if col not in events.columns:
                events[col] = np.nan

    calendar = pd.date_range(span_min.floor("D"), span_max.floor("D"),
                             freq="D")
    folds = rig.fold_windows(calendar)
    description = describe(events, episodes_all)

    if args.smoke:
        print("\nSMOKE (mechanical only — return numbers suppressed)")
        print(json.dumps(description, indent=2, default=str))
        print(f"folds available: {len(folds)} "
              f"({calendar.min():%Y-%m-%d}..{calendar.max():%Y-%m-%d})")
        return

    judged, stress = [], []
    for direction in ("long", "short"):
        for label, horizon_min in HORIZONS.items():
            for friction in FRICTIONS:
                for borrow in (BORROWS_BPS_DAY if direction == "short"
                               else (0.0,)):
                    cell = score_cell(events, folds, direction, label,
                                      horizon_min, friction, borrow)
                    is_judged = (friction == JUDGED_FRICTION
                                 and (direction == "long"
                                      or borrow == JUDGED_BORROW))
                    cell["cell_class"] = ("judged" if is_judged
                                          else "stress")
                    (judged if is_judged else stress).append(cell)
                    rig.census_append("hotmkt-mw3", {
                        "direction": direction, "horizon": label,
                        "friction": friction, "borrow": borrow,
                        "cell_class": cell["cell_class"],
                    }, {k: v for k, v in cell.items()
                        if k not in ("direction", "horizon", "friction",
                                     "borrow_bps_day", "cell_class")})

    passing = [c for c in judged if c["gate_pass"]]
    verdict = ("exploitable: " + ", ".join(
        f"{c['direction']}@{c['horizon']}" for c in passing)
        if passing else "announce-only")

    # Descriptive per-trigger-class gross means (NOT gate-eligible).
    filled = events[events["filled"]].copy()
    filled["klass"] = filled["trigger"].map(trigger_class)
    class_table = {}
    for klass, group in filled.groupby("klass"):
        class_table[klass] = {
            "n": int(len(group)),
            **{label: float(group[f"fwd_{label}"].mean() * 1e4)
               for label in HORIZONS
               if group[f"fwd_{label}"].notna().any()},
        }

    result = {
        "chunk": "WM-HOTMKT-1",
        "detector": {
            "source": "mw.c @ 6e0d294, all-default thresholds "
                      "(live KVs verified 0=default 2026-07-18)",
            "pct_24h": PCT_24H_THRESH, "vel_pct": VEL_THRESH,
            "vel_window_min": VEL_WINDOW_MIN, "vol_z": VOL_Z_THRESH,
            "min_vol_usd": MIN_VOL_USD, "cooldown_min": COOLDOWN_MIN,
        },
        "universe": [pair for _, pair in markets],
        "span": [str(span_min), str(span_max)],
        "n_folds": len(folds),
        "description": description,
        "freelook_minus_lag1_bps": freelook_delta(events),
        "judged_cells": judged,
        "stress_cells": stress,
        "trigger_class_gross_bps_descriptive": class_table,
        "verdict": verdict,
    }
    os.makedirs(args.out, exist_ok=True)
    out_path = os.path.join(args.out, "hotmkt_results.json")
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(result, fh, indent=2, default=str)

    print(f"\nforward-return table (judged cells, 1x friction"
          f"{', 10bps/d borrow' if any(c['direction'] == 'short' for c in judged) else ''}):")
    header = (f"{'cell':<10} {'n_ev':>6} {'folds':>5} {'gross_mu':>9} "
              f"{'net_mu':>9} {'rr':>7} {'pos':>5} {'stab':>5}  gate")
    print(header)
    for cell in judged:
        name = f"{cell['direction']}@{cell['horizon']}"
        print(f"{name:<10} {cell['n_events']:>6} "
              f"{cell['n_qualifying_folds']:>5} "
              f"{(cell['gross_mean'] or 0) * 1e4:>8.1f}b "
              f"{(cell['net_mean'] or 0) * 1e4:>8.1f}b "
              f"{cell.get('robust_ratio', float('nan')):>7.3f} "
              f"{cell.get('pos_frac', float('nan')):>5.2f} "
              f"{cell.get('stability_both_pos', float('nan')):>5.2f}  "
              f"{'PASS' if cell['gate_pass'] else 'fail'}")
    print(f"\nVERDICT: {verdict}")
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
