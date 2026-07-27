#!/usr/bin/env python3
"""hotflag_h004.py — H-hotflag-004: event-density regime conditioning on
velocity, long, 1d (family hotflag, TESTING NIGHT 2026-07-25).

PRE-REGISTRATION lives in
tools/research/families/hotflag/HYPOTHESES.md §H-hotflag-004 — thesis,
benchmark, expected fold signature and test plan were written there
BEFORE this script ran; this docstring only restates the fixed config so
the script is self-contained: base class = velocity only (H-hotflag-001's
event set), bucketed into cross-sectional flag-density terciles (low
<=P33 / mid / high >P33, by same-day count of HOT flags across the full
universe — all pairs, all trigger classes — with cutpoints computed ONCE
on the full frozen event calendar), direction long only, horizon fixed
at 1d, frictions 1x/2x/4x per bucket (3 configs, 9 scored cells). Fewer
than 6 configs — no rank-stability requirement; the VALIDATED gate
applies to the grid's best cell (highest 1x active robust_ratio among
the three buckets), the same best-cell convention H-hotflag-003 used for
its larger grid.

Reuse discipline (rig-is-shared-instrument, NOTES.md 2026-07-20): the
detector replay, lag-1 fill model, trigger_class labeling, dedup and cost
helpers are imported from hotmkt.py verbatim; `ew_benchmark_1d` and
`active_returns` are H-hotflag-001/002's helpers verbatim. This script's
only new logic is the density-tercile bucketing: a day -> count map built
from the FULL raw event table (unfiltered by trigger class or fill
status — density is "how many pairs flagged HOT that day", not which of
those flags were later tradeable), with tercile cutpoints fixed once
against that full corpus before dedup_overlap ever runs on the velocity
subset, and before any velocity event is assigned a bucket.

Usage:
    tools/research/.venv/bin/python \
        tools/research/families/hotflag/hotflag_h004.py
"""

import json
import os
import sys

import numpy as np
import pandas as pd

HERE = os.path.dirname(os.path.abspath(__file__))
RESEARCH_DIR = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, RESEARCH_DIR)
import rig                                              # noqa: E402
import hotmkt                                            # noqa: E402

FAMILY = "hotflag-H-hotflag-004"
TRIGGER_CLASS = "velocity"
DIRECTION = "long"
HORIZON_LABEL = "1d"
HORIZON_MIN = hotmkt.HORIZONS[HORIZON_LABEL]
FRICTIONS = (1.0, 2.0, 4.0)
BUCKETS = ("low", "mid", "high")
MIN_UNIVERSE = 6                     # rotation.py / H-hotflag-001 convention
MIN_HISTORY_DAYS = 35                # no lookback dependency here
RESULTS_PATH = os.path.join(
    rig.DATA_DIR, "hotflag_H-hotflag-004_results.json")


def ew_benchmark_1d(close, elig):
    """EW eligible-universe next-day return, indexed by date (frictionless).

    Verbatim from H-hotflag-001/002's helper (reuse discipline).
    """
    fwd = close.shift(-1) / close - 1.0
    return fwd.where(elig).mean(axis=1)


def active_returns(events, ts_col, ret_col, bench):
    dates = events[ts_col].dt.floor("D")
    b = bench.reindex(dates).to_numpy()
    return events[ret_col].to_numpy() - b


def fold_means_for(usable, folds, ret_col):
    means, counts = [], []
    for start, end in folds:
        mask = (usable["ts_entry"] >= start) & (usable["ts_entry"] < end)
        n = int(mask.sum())
        if n >= hotmkt.MIN_EVENTS_PER_FOLD:
            means.append(float(usable.loc[mask, ret_col].mean()))
            counts.append(n)
    return means, counts


def score_lag1(usable, folds, friction):
    cost = hotmkt.round_trip_cost(friction)
    usable = usable.copy()
    usable["net_active"] = usable["active_fwd_1d"] - cost
    fold_means, _ = fold_means_for(usable, folds, "net_active")
    pooled = len(usable)
    cell = {
        "friction": friction,
        "n_events": pooled,
        "n_qualifying_folds": len(fold_means),
        "pooled_active_mean": float(usable["net_active"].mean())
        if pooled else None,
    }
    if fold_means:
        cell.update(rig.fold_metrics(fold_means))
    cell["judgeable"] = (len(fold_means) >= hotmkt.MIN_QUALIFYING_FOLDS
                          and pooled >= hotmkt.MIN_POOLED_EVENTS)
    return cell


def density_bucket_map(events_all_raw):
    """Day -> {"low","mid","high"}, cutpoints fixed on the full raw corpus.

    Density = count of HOT flags (any pair, any trigger class, filled or
    not) on that calendar day, per the pre-registered test plan's "same-
    day count of HOT flags across the universe" / "the full frozen event
    calendar". Returns (day-indexed bucket Series, p33, p67).
    """
    day = events_all_raw["ts_flag"].dt.floor("D")
    daily_count = day.value_counts()
    p33, p67 = daily_count.quantile([1 / 3, 2 / 3])
    bucket = pd.cut(daily_count, bins=[-np.inf, p33, p67, np.inf],
                     labels=list(BUCKETS))
    return bucket, float(p33), float(p67)


def main():
    events_all = pd.read_parquet(
        os.path.join(rig.DATA_DIR, "hotmkt_events.parquet"))
    assert events_all["ts_flag"].max() < rig.RESEARCH_CUTOFF, \
        "hotmkt_events.parquet leaked a post-cutoff row"

    bucket_map, p33, p67 = density_bucket_map(events_all)

    events_all["klass"] = events_all["trigger"].map(hotmkt.trigger_class)
    events = events_all[events_all["klass"] == TRIGGER_CLASS].copy()

    frame = rig.load("1d")
    close, _volume, n_1m = rig.daily_panel(frame)
    elig = rig.eligible(close, n_1m, min_history_days=MIN_HISTORY_DAYS)
    rich = elig.sum(axis=1) >= MIN_UNIVERSE
    anchor = rich[rich].index[0]
    bench = ew_benchmark_1d(close, elig)

    span = close.loc[anchor:]
    folds = rig.fold_windows(span.index)

    filled = events[events["filled"] & events["fwd_1d"].notna()].copy()
    usable = hotmkt.dedup_overlap(filled, HORIZON_MIN)
    usable["active_fwd_1d"] = active_returns(
        usable, "ts_entry", "fwd_1d", bench)
    usable = usable[usable["ts_entry"] >= anchor]
    usable["density_bucket"] = bucket_map.reindex(
        usable["ts_flag"].dt.floor("D")).to_numpy()

    grid_results = {}
    for bkt in BUCKETS:
        sub = usable[usable["density_bucket"] == bkt].copy()

        cells = {}
        for friction in FRICTIONS:
            tag = {1.0: "1x", 2.0: "2x", 4.0: "4x"}[friction]
            cell = score_lag1(sub, folds, friction)
            cells[tag] = cell
            rig.census_append(
                FAMILY,
                {"trigger_class": TRIGGER_CLASS, "direction": DIRECTION,
                 "horizon": HORIZON_LABEL, "density_bucket": bkt,
                 "friction": friction},
                {k: cell.get(k) for k in
                 ("robust_ratio", "mean_fold", "pos_frac", "n_events")})

        # free-look diagnostic (decay statistic only — never gate-eligible)
        look_ok = sub[sub["look_1d"].notna()].copy()
        look_ok["active_look_1d"] = active_returns(
            look_ok, "ts_flag", "look_1d", bench)
        cost_1x = hotmkt.round_trip_cost(1.0)
        look_ok["net_active_look"] = look_ok["active_look_1d"] - cost_1x
        freelook_fold_means, _ = fold_means_for(
            look_ok, folds, "net_active_look")
        freelook_metrics = (rig.fold_metrics(freelook_fold_means)
                             if freelook_fold_means else None)

        active_rr_lag1 = cells["1x"].get("robust_ratio")
        active_rr_freelook = (freelook_metrics["robust_ratio"]
                               if freelook_metrics else None)
        lag_decay = None
        if (active_rr_lag1 is not None
                and active_rr_freelook not in (None, 0)
                and np.isfinite(active_rr_lag1)
                and np.isfinite(active_rr_freelook)):
            lag_decay = ((active_rr_lag1 - active_rr_freelook)
                         / abs(active_rr_freelook))

        c1x, c2x, c4x = cells["1x"], cells["2x"], cells["4x"]
        gate_active = bool(c1x.get("judgeable")
                            and c1x.get("robust_ratio", -1.0) >= 0.3)
        gate_pos = bool(c1x.get("judgeable")
                         and c1x.get("pos_frac", 0.0) >= 0.75)
        gate_2x = bool(c2x.get("mean_fold") is not None
                        and c2x["mean_fold"] > 0.0)
        gate_4x = bool(c4x.get("mean_fold") is not None
                        and c4x["mean_fold"] > 0.0)
        gate_decay = bool(lag_decay is not None and lag_decay > -0.30)

        grid_results[bkt] = {
            "density_bucket": bkt,
            "n_events_deduped": len(sub),
            "cells": cells,
            "freelook": {
                "n_events": len(look_ok),
                "n_qualifying_folds": len(freelook_fold_means),
                "metrics": freelook_metrics,
            },
            "lag_decay": lag_decay,
            "gates": {"active": gate_active, "pos": gate_pos,
                      "2x": gate_2x, "4x": gate_4x, "decay": gate_decay},
        }

    def rr1x(bkt):
        rr = grid_results[bkt]["cells"]["1x"].get("robust_ratio")
        return rr if rr is not None and np.isfinite(rr) else float("-inf")

    best_bucket = max(BUCKETS, key=rr1x)
    best_gates = grid_results[best_bucket]["gates"]
    validated = all(best_gates.values())
    verdict = "validated-port-candidate" if validated else "killed"
    failed = [name for name, ok in best_gates.items() if not ok]

    results = {
        "hypothesis_id": "H-hotflag-004",
        "trigger_class": TRIGGER_CLASS, "direction": DIRECTION,
        "horizon": HORIZON_LABEL,
        "density_cutpoints": {"p33": p33, "p67": p67,
                               "n_days_in_full_corpus": int(
                                   events_all["ts_flag"].dt.floor("D")
                                   .nunique())},
        "anchor": str(anchor.date()), "n_folds": len(folds),
        "fold_span": ([str(folds[0][0].date()), str(folds[-1][1].date())]
                      if folds else None),
        "buckets": grid_results,
        "best_bucket": best_bucket,
        "gates": best_gates,
        "verdict": verdict,
        "failed_gates": failed,
    }
    with open(RESULTS_PATH, "w", encoding="utf-8") as fh:
        json.dump(results, fh, indent=2)

    print(f"anchor {anchor.date()} | {len(folds)} folds "
          f"{folds[0][0].date() if folds else None} -> "
          f"{folds[-1][1].date() if folds else None}")
    print(f"density cutpoints: p33={p33} p67={p67}")
    print(f"n_events (deduped, filled, >=anchor) = {len(usable)}")
    for bkt in BUCKETS:
        g = grid_results[bkt]
        c1x = g["cells"]["1x"]
        print(f"  {bkt:<5} n={c1x['n_events']:>5} "
              f"folds={c1x['n_qualifying_folds']:>2} "
              f"judgeable={c1x['judgeable']!s:<5} "
              f"rr={c1x.get('robust_ratio')!r} pos={c1x.get('pos_frac')!r} "
              f"decay={g['lag_decay']!r}")
    print(f"best bucket: {best_bucket} gates={best_gates} -> "
          f"verdict {verdict} (failed: {failed})")
    print(f"wrote {RESULTS_PATH}")


if __name__ == "__main__":
    main()
