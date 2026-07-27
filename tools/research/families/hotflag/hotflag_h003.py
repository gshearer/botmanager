#!/usr/bin/env python3
"""hotflag_h003.py — H-hotflag-003: trigger-class x horizon grid
(mechanism-conditioned), long, {4h,1d} (family hotflag, TESTING NIGHT
2026-07-24).

PRE-REGISTRATION lives in
tools/research/families/hotflag/HYPOTHESES.md §H-hotflag-003 — thesis,
benchmark, expected fold signature and test plan were written there
BEFORE this script ran; this docstring only restates the fixed config so
the script is self-contained: grid = trigger_class in {brk_hi, brk_lo,
multi, pct_24h, velocity, vol_z} x horizon in {4h, 1d}, direction long
only, frictions 1x/2x/4x on every config (12 configs, 36 scored cells).
A >=6-config sweep — rank-stability median rho is required alongside the
per-cell gates. Same anchor and daily fold calendar as
H-hotflag-001/002 (shared calendar); the 4h benchmark and active-return
bucketing are new (H-001/002 only needed 1d), built directly on
`rig.load("1h")` + the shared daily eligibility mask — never a
reimplementation of fold/cost/metric logic, which stays in `rig.py`.

Reuse discipline (rig-is-shared-instrument, NOTES.md 2026-07-20): the
detector replay, lag-1 fill model, trigger_class labeling, dedup and
cost helpers are imported from hotmkt.py verbatim. The 1d EW benchmark
is H-hotflag-002's `ew_benchmark_1d` verbatim; the 4h EW benchmark
follows the identical frictionless-forward-return recipe on an hourly
close panel, with the daily eligibility mask broadcast onto hourly bars
via forward-fill (a pair's point-in-time eligibility does not change
intra-day).

Rank-stability fold-fill convention (disclosed, gate-neutral): a
(config, fold) cell below hotmkt.MIN_EVENTS_PER_FOLD contributes 0.0
(sat out, no position, no active return) to the rank_stability matrix
only, so every config presents an equal-length fold vector across the
shared calendar; the VALIDATED gates below never read this filled
matrix, only each cell's own qualifying-fold subset exactly as
H-hotflag-001/002 computed it.

Usage:
    tools/research/.venv/bin/python \
        tools/research/families/hotflag/hotflag_h003.py
"""

import itertools
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

FAMILY = "hotflag-H-hotflag-003"
TRIGGER_CLASSES = ("brk_hi", "brk_lo", "multi", "pct_24h", "velocity",
                    "vol_z")
HORIZONS = ("4h", "1d")
DIRECTION = "long"
FRICTIONS = (1.0, 2.0, 4.0)
MIN_UNIVERSE = 6                     # rotation.py / H-hotflag-001 convention
MIN_HISTORY_DAYS = 35                # no lookback dependency here
GATE_MEDIAN_RHO = 0.5                # lane prompt: >=6-config grid requirement
RESULTS_PATH = os.path.join(
    rig.DATA_DIR, "hotflag_H-hotflag-003_results.json")


def ew_benchmark_1d(close, elig):
    """EW eligible-universe next-day return, indexed by date (frictionless).

    Verbatim from H-hotflag-002's helper (reuse discipline).
    """
    fwd = close.shift(-1) / close - 1.0
    return fwd.where(elig).mean(axis=1)


def hourly_close_panel(frame):
    """Wide hourly close matrix on a full UTC hourly calendar (ffilled)."""
    close = frame.pivot(index="ts", columns="pair", values="close")
    calendar = pd.date_range(close.index.min(), close.index.max(), freq="h")
    return close.reindex(calendar).ffill()


def ew_benchmark_4h(close_1h, elig_daily):
    """EW eligible-universe forward-4h return, indexed by hour (frictionless).

    Same frictionless-forward-return recipe as ew_benchmark_1d, on an
    hourly grid; the daily eligibility mask is broadcast onto hourly
    bars via forward-fill (point-in-time membership does not change
    intra-day).
    """
    fwd = close_1h.shift(-4) / close_1h - 1.0
    elig_hourly = elig_daily.reindex(close_1h.index, method="ffill")
    return fwd.where(elig_hourly).mean(axis=1)


def active_returns(events, ts_col, ret_col, bench, floor_freq):
    dates = events[ts_col].dt.floor(floor_freq)
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


def fold_fill_for_matrix(usable, folds, ret_col):
    """Per-fold mean active return, 0.0-filled below MIN_EVENTS_PER_FOLD.

    Rank-stability matrix input only (see module docstring convention);
    VALIDATED gates never read this.
    """
    out = []
    for start, end in folds:
        mask = (usable["ts_entry"] >= start) & (usable["ts_entry"] < end)
        n = int(mask.sum())
        if n >= hotmkt.MIN_EVENTS_PER_FOLD:
            out.append(float(usable.loc[mask, ret_col].mean()))
        else:
            out.append(0.0)
    return out


def score_lag1(usable, folds, friction, ret_col):
    cost = hotmkt.round_trip_cost(friction)
    usable = usable.copy()
    usable["net_active"] = usable[ret_col] - cost
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


def main():
    events_all = pd.read_parquet(
        os.path.join(rig.DATA_DIR, "hotmkt_events.parquet"))
    assert events_all["ts_flag"].max() < rig.RESEARCH_CUTOFF, \
        "hotmkt_events.parquet leaked a post-cutoff row"
    events_all["klass"] = events_all["trigger"].map(hotmkt.trigger_class)

    frame_1d = rig.load("1d")
    close_1d, _volume, n_1m = rig.daily_panel(frame_1d)
    elig = rig.eligible(close_1d, n_1m, min_history_days=MIN_HISTORY_DAYS)
    rich = elig.sum(axis=1) >= MIN_UNIVERSE
    anchor = rich[rich].index[0]
    bench_1d = ew_benchmark_1d(close_1d, elig)

    frame_1h = rig.load("1h")
    close_1h = hourly_close_panel(frame_1h)
    bench_4h = ew_benchmark_4h(close_1h, elig)

    span = close_1d.loc[anchor:]
    folds = rig.fold_windows(span.index)

    BENCH = {"4h": (bench_4h, "fwd_4h", "look_4h", "h"),
              "1d": (bench_1d, "fwd_1d", "look_1d", "D")}

    grid_results = {}
    active_matrix, matrix_labels = [], []

    for klass, horizon in itertools.product(TRIGGER_CLASSES, HORIZONS):
        bench, fwd_col, look_col, floor_freq = BENCH[horizon]
        horizon_min = hotmkt.HORIZONS[horizon]

        events = events_all[events_all["klass"] == klass].copy()
        filled = events[events["filled"] & events[fwd_col].notna()].copy()
        usable = hotmkt.dedup_overlap(filled, horizon_min)
        usable[f"active_{fwd_col}"] = active_returns(
            usable, "ts_entry", fwd_col, bench, floor_freq)
        usable = usable[usable["ts_entry"] >= anchor]

        cells = {}
        for friction in FRICTIONS:
            tag = {1.0: "1x", 2.0: "2x", 4.0: "4x"}[friction]
            cell = score_lag1(usable, folds, friction, f"active_{fwd_col}")
            cells[tag] = cell
            rig.census_append(
                FAMILY,
                {"trigger_class": klass, "direction": DIRECTION,
                 "horizon": horizon, "friction": friction},
                {k: cell.get(k) for k in
                 ("robust_ratio", "mean_fold", "pos_frac", "n_events")})

        # free-look diagnostic (decay statistic only — never gate-eligible)
        look_ok = usable[usable[look_col].notna()].copy()
        look_ok[f"active_{look_col}"] = active_returns(
            look_ok, "ts_flag", look_col, bench, floor_freq)
        cost_1x = hotmkt.round_trip_cost(1.0)
        look_ok["net_active_look"] = look_ok[f"active_{look_col}"] - cost_1x
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

        key = f"{klass}@{horizon}"
        grid_results[key] = {
            "trigger_class": klass, "horizon": horizon,
            "n_events_deduped": len(usable),
            "cells": cells,
            "freelook": {
                "n_events": len(look_ok),
                "n_qualifying_folds": len(freelook_fold_means),
                "metrics": freelook_metrics,
            },
            "lag_decay": lag_decay,
            "gates_no_rho": {"active": gate_active, "pos": gate_pos,
                              "2x": gate_2x, "4x": gate_4x,
                              "decay": gate_decay},
        }

        active_matrix.append(
            fold_fill_for_matrix(usable, folds, f"active_{fwd_col}"))
        matrix_labels.append(key)

    median_rho, _ = rig.rank_stability(np.array(active_matrix))
    gate_rho = bool(median_rho > GATE_MEDIAN_RHO)

    def rr1x(key):
        rr = grid_results[key]["cells"]["1x"].get("robust_ratio")
        return rr if rr is not None and np.isfinite(rr) else float("-inf")

    best_key = max(grid_results, key=rr1x)
    best = grid_results[best_key]
    best_gates = dict(best["gates_no_rho"])
    best_gates["rho"] = gate_rho
    validated = all(best_gates.values())
    verdict = "validated-port-candidate" if validated else "killed"
    failed = [name for name, ok in best_gates.items() if not ok]

    results = {
        "hypothesis_id": "H-hotflag-003",
        "direction": DIRECTION,
        "trigger_classes": list(TRIGGER_CLASSES), "horizons": list(HORIZONS),
        "anchor": str(anchor.date()), "n_folds": len(folds),
        "fold_span": ([str(folds[0][0].date()), str(folds[-1][1].date())]
                      if folds else None),
        "grid": grid_results,
        "rank_stability_median_rho": median_rho,
        "gate_median_rho": GATE_MEDIAN_RHO,
        "best_cell": best_key,
        "gates": best_gates,
        "verdict": verdict,
        "failed_gates": failed,
    }
    with open(RESULTS_PATH, "w", encoding="utf-8") as fh:
        json.dump(results, fh, indent=2)

    print(f"anchor {anchor.date()} | {len(folds)} folds "
          f"{folds[0][0].date() if folds else None} -> "
          f"{folds[-1][1].date() if folds else None}")
    for key in matrix_labels:
        c1x = grid_results[key]["cells"]["1x"]
        print(f"  {key:<16} n={c1x['n_events']:>6} "
              f"folds={c1x['n_qualifying_folds']:>2} "
              f"judgeable={c1x['judgeable']!s:<5} "
              f"rr={c1x.get('robust_ratio')!r} pos={c1x.get('pos_frac')!r}")
    print(f"rank-stability median rho = {median_rho!r} "
          f"(gate > {GATE_MEDIAN_RHO})")
    print(f"best cell: {best_key} gates={best_gates} -> verdict {verdict} "
          f"(failed: {failed})")
    print(f"wrote {RESULTS_PATH}")


if __name__ == "__main__":
    main()
