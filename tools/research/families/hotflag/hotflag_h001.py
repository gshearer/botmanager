#!/usr/bin/env python3
"""hotflag_h001.py — H-hotflag-001: velocity-class-only long continuation
at 1d (family hotflag, TESTING NIGHT 2026-07-22).

PRE-REGISTRATION lives in
tools/research/families/hotflag/HYPOTHESES.md §H-hotflag-001 — thesis,
benchmark, expected fold signature and test plan were written there
BEFORE this script ran; this docstring only restates the fixed config so
the script is self-contained: trigger_class == "velocity" (single-bit
MW_TRIG_VELOCITY, no coincident bits), direction long only, horizon
fixed at 1d, frictions 1x/2x/4x. A single config — no grid, no
rank-stability requirement.

Reuse discipline (rig-is-shared-instrument, NOTES.md 2026-07-20): the
detector replay, lag-1 fill model, trigger_class labeling, dedup and
cost helpers are imported from hotmkt.py verbatim, never reimplemented.
Fold/cost/metric primitives come from rig.py. This script's only new
logic is the EW eligible-universe daily benchmark (hotmkt.py judges
against zero; this lane's official recipe judges ACTIVE return against
a pre-registered benchmark) and wiring that benchmark into hotmkt.py's
existing event table.

Benchmark: equal-weight buy-and-hold of the point-in-time eligible
universe, frictionless, over the SAME 1-day window as each event's
forward return (entry date -> entry date + 1d for the lag-1 leg, flag
date -> flag date + 1d for the free-look decay leg) — the exact
apples-to-apples window the hypothesis's benchmark line calls for.

Usage:
    tools/research/.venv/bin/python \
        tools/research/families/hotflag/hotflag_h001.py
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

FAMILY = "hotflag-H-hotflag-001"
TRIGGER_CLASS = "velocity"
DIRECTION = "long"
HORIZON_LABEL = "1d"
HORIZON_MIN = hotmkt.HORIZONS[HORIZON_LABEL]
FRICTIONS = (1.0, 2.0, 4.0)
MIN_UNIVERSE = 6                     # rotation.py convention
MIN_HISTORY_DAYS = 35                # no lookback dependency here
RESULTS_PATH = os.path.join(
    rig.DATA_DIR, "hotflag_H-hotflag-001_results.json")


def ew_benchmark_1d(close, elig):
    """EW eligible-universe next-day return, indexed by date (frictionless)."""
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
    fold_means, fold_counts = fold_means_for(usable, folds, "net_active")
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


def main():
    events_all = pd.read_parquet(
        os.path.join(rig.DATA_DIR, "hotmkt_events.parquet"))
    assert events_all["ts_flag"].max() < rig.RESEARCH_CUTOFF, \
        "hotmkt_events.parquet leaked a post-cutoff row"
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

    cells = {}
    for friction in FRICTIONS:
        tag = {1.0: "1x", 2.0: "2x", 4.0: "4x"}[friction]
        cell = score_lag1(usable, folds, friction)
        cells[tag] = cell
        rig.census_append(
            FAMILY,
            {"trigger_class": TRIGGER_CLASS, "direction": DIRECTION,
             "horizon": HORIZON_LABEL, "friction": friction},
            {k: cell.get(k) for k in
             ("robust_ratio", "mean_fold", "pos_frac", "n_events")})

    # free-look diagnostic (decay statistic only — never gate-eligible),
    # same deduped event set, own-close entry instead of the lag-1 fill.
    look_ok = usable[usable["look_1d"].notna()].copy()
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
    if (active_rr_lag1 is not None and active_rr_freelook not in (None, 0)
            and np.isfinite(active_rr_lag1)
            and np.isfinite(active_rr_freelook)):
        lag_decay = ((active_rr_lag1 - active_rr_freelook)
                     / abs(active_rr_freelook))

    # ---- mechanical gate (VALIDATED requires ALL; see lane prompt) ----
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
    validated = gate_active and gate_pos and gate_2x and gate_4x and gate_decay
    verdict = "validated-port-candidate" if validated else "killed"

    failed = [name for name, ok in (
        ("active", gate_active), ("pos", gate_pos), ("2x", gate_2x),
        ("4x", gate_4x), ("decay", gate_decay)) if not ok]

    results = {
        "hypothesis_id": "H-hotflag-001",
        "trigger_class": TRIGGER_CLASS, "direction": DIRECTION,
        "horizon": HORIZON_LABEL,
        "anchor": str(anchor.date()), "n_folds": len(folds),
        "fold_span": ([str(folds[0][0].date()), str(folds[-1][1].date())]
                      if folds else None),
        "cells": cells,
        "freelook": {
            "n_events": len(look_ok),
            "n_qualifying_folds": len(freelook_fold_means),
            "metrics": freelook_metrics,
        },
        "lag_decay": lag_decay,
        "gates": {"active": gate_active, "pos": gate_pos, "2x": gate_2x,
                  "4x": gate_4x, "decay": gate_decay},
        "verdict": verdict,
        "failed_gates": failed,
    }
    with open(RESULTS_PATH, "w", encoding="utf-8") as fh:
        json.dump(results, fh, indent=2)

    print(f"anchor {anchor.date()} | {len(folds)} folds "
          f"{folds[0][0].date() if folds else None} -> "
          f"{folds[-1][1].date() if folds else None}")
    print(f"n_events (deduped, filled, >=anchor) = {len(usable)}")
    for tag in ("1x", "2x", "4x"):
        c = cells[tag]
        print(f"  {tag}: n={c['n_events']} folds={c['n_qualifying_folds']} "
              f"judgeable={c['judgeable']} "
              f"rr={c.get('robust_ratio')!r} pos={c.get('pos_frac')!r} "
              f"mean_fold={c.get('mean_fold')!r}")
    print(f"freelook: n={len(look_ok)} folds={len(freelook_fold_means)} "
          f"rr={active_rr_freelook!r}")
    print(f"lag_decay = {lag_decay!r}")
    print(f"gates: {results['gates']} -> verdict {verdict} "
          f"(failed: {failed})")
    print(f"wrote {RESULTS_PATH}")


if __name__ == "__main__":
    main()
