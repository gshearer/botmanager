#!/usr/bin/env python3
"""rotation.py — WM-EDGE-2 step 3: cross-sectional rotation prototype.

PRE-REGISTRATION (written before any result was computed — this docstring
is the hypothesis record; do not edit it to fit results):

  Thesis: cross-sectional momentum — rotating into the strongest few
  assets of a broad universe — is the best-documented persistent crypto
  anomaly and a DIFFERENT return source from single-asset timing: it
  harvests relative strength, not market direction. Long-only-spot
  native: when nothing has positive risk-adjusted momentum the book
  de-risks to cash by construction.

  Expected fold signature: positive active return concentrated in
  dispersed/rotating regimes (alt seasons, sector rotations); flat-to-
  negative active in BTC-dominance melt-ups where the benchmark IS the
  winner; drawdown folds should beat the benchmark via the cash fallback.

  Fixed design (not swept): weekly rebalance on the Monday UTC close;
  hold top-k of the point-in-time-eligible universe by risk-adjusted
  momentum (mean/std of daily returns over the lookback); only assets
  with score > 0 are held; deployment scales n_held/k (breadth-scaled),
  remainder in cash; liquidity floor 300 traded minutes/day (median,
  trailing 30d); universe anchor = first day >= 6 pairs eligible at the
  LONGEST lookback so every config shares identical folds.

  Pre-registered grid (18 configs, nothing outside it will be reported):
  k in {2,3,5} x lookback in {30,60,90}d x weighting in {equal, invvol}.
  Frictions 1x/2x/4x on every config. Gate (WM-EDGE-2, set in the TODO
  before this file existed): port to C only if pooled active
  robust_ratio >= 0.3 AND rank-stability median rho > 0.5.

Benchmarks: (a) equal-weight buy-and-hold of the fold-start eligible
universe, frictionless — active return is measured against this; (b)
btc-usd buy-and-hold, reported for context. Costs: engine economics,
5+5 bps per side x friction, charged on rebalance turnover.

Usage:
    tools/research/.venv/bin/python tools/research/rotation.py
"""

import itertools
import json
import os
import sys

import numpy as np
import pandas as pd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402

GRID_K = (2, 3, 5)
GRID_LOOKBACK = (30, 60, 90)
GRID_WEIGHTING = ("equal", "invvol")
FRICTIONS = (1.0, 2.0, 4.0)
MIN_UNIVERSE = 6
REBALANCE_FREQ = "W-MON"
GATE_ACTIVE_RR = 0.3
GATE_MEDIAN_RHO = 0.5
RESULTS_PATH = os.path.join(rig.DATA_DIR, "rotation_results.json")


def momentum_score(close, lookback):
    """Risk-adjusted momentum: mean/std of daily returns over lookback."""
    rets = close.pct_change()
    mean = rets.rolling(lookback).mean()
    std = rets.rolling(lookback).std()
    return mean / std


def run_config(close, elig, k, lookback, weighting, friction):
    """Daily equity curve for one config. Returns (equity, turnover_sum)."""
    rets = close.pct_change().fillna(0.0)
    score = momentum_score(close, lookback)
    vol = rets.rolling(lookback).std()
    rebalance_days = set(close.resample(REBALANCE_FREQ).last().index)

    equity = pd.Series(1.0, index=close.index)
    weights = pd.Series(0.0, index=close.columns)
    value, turnover_total = 1.0, 0.0

    for day in close.index[1:]:
        day_ret = float((weights * rets.loc[day]).sum())
        value *= 1.0 + day_ret
        # weights drift with returns between rebalances
        if weights.abs().sum() > 0:
            grown = weights * (1.0 + rets.loc[day])
            weights = grown / (1.0 + day_ret)
        if day in rebalance_days:
            candidates = score.loc[day].where(elig.loc[day]).dropna()
            held = candidates[candidates > 0].nlargest(k)
            target = pd.Series(0.0, index=close.columns)
            if len(held):
                deploy = len(held) / k
                if weighting == "invvol":
                    inv = 1.0 / vol.loc[day, held.index]
                    target[held.index] = deploy * inv / inv.sum()
                else:
                    target[held.index] = deploy / len(held)
            delta = (target - weights).abs().sum()
            cost = rig.turnover_cost(target - weights, friction)
            value *= 1.0 - cost
            turnover_total += delta
            weights = target
        equity.loc[day] = value
    return equity, turnover_total


def fold_returns(series, folds):
    """Per-fold simple returns read off a daily curve (equity or index)."""
    out = []
    for start, end in folds:
        window = series.loc[start:end]
        out.append(float(window.iloc[-1] / window.iloc[0] - 1.0))
    return out


def bench_curves(close, elig, folds):
    """Frictionless EW-universe and BTC buy-and-hold per-fold returns."""
    ew, btc = [], []
    for start, end in folds:
        members = elig.loc[:start].iloc[-1]
        members = members[members].index
        window = close.loc[start:end, members]
        ew.append(float((window.iloc[-1] / window.iloc[0] - 1.0).mean()))
        btc_window = close.loc[start:end, "btc-usd"]
        btc.append(float(btc_window.iloc[-1] / btc_window.iloc[0] - 1.0))
    return ew, btc


def main():
    frame = rig.load("1d")
    close, _volume, n_1m = rig.daily_panel(frame)

    # shared eligibility + anchor at the LONGEST lookback -> identical folds
    elig_anchor = rig.eligible(close, n_1m,
                               min_history_days=max(GRID_LOOKBACK) + 35)
    rich = elig_anchor.sum(axis=1) >= MIN_UNIVERSE
    anchor = rich[rich].index[0]
    span = close.loc[anchor:]
    folds = rig.fold_windows(span.index)
    print(f"universe anchor {anchor.date()} (>= {MIN_UNIVERSE} pairs); "
          f"{len(folds)} folds, first test {folds[0][0].date()}, "
          f"last {folds[-1][1].date()}")

    ew_bench, btc_bench = bench_curves(close, elig_anchor, folds)
    print(f"bench per-fold mean: EW {np.mean(ew_bench):+.2%}  "
          f"BTC {np.mean(btc_bench):+.2%}")

    results, active_matrix = [], []
    for k, lookback, weighting in itertools.product(
            GRID_K, GRID_LOOKBACK, GRID_WEIGHTING):
        elig = rig.eligible(close, n_1m, min_history_days=lookback + 35)
        row = {"k": k, "lookback": lookback, "weighting": weighting}
        for friction in FRICTIONS:
            equity, turnover = run_config(
                close, elig, k, lookback, weighting, friction)
            strat = fold_returns(equity.loc[anchor:], folds)
            active = [s - b for s, b in zip(strat, ew_bench)]
            metrics = rig.fold_metrics(active)
            metrics["net_mean_fold"] = float(np.mean(strat))
            metrics["mtm_max_dd"] = rig.max_drawdown(equity.loc[anchor:])
            metrics["turnover_per_year"] = turnover / (len(span) / 365.25)
            tag = {1.0: "1x", 2.0: "2x", 4.0: "4x"}[friction]
            row[tag] = metrics
            rig.census_append("rotation-topk", {**row, "friction": tag},
                              {kk: metrics[kk] for kk in
                               ("robust_ratio", "mean_fold", "pos_frac")})
            if friction == 1.0:
                active_matrix.append(active)
        results.append(row)
        m1 = row["1x"]
        print(f"k={k} L={lookback:>2} {weighting:<6} | active rr "
              f"{m1['robust_ratio']:+.3f} mean {m1['mean_fold']:+.2%} "
              f"pos {m1['pos_frac']:.2f} worst {m1['worst_fold']:+.2%} "
              f"dd {m1['mtm_max_dd']:.1%} | 4x rr "
              f"{row['4x']['robust_ratio']:+.3f}")

    median_rho, _ = rig.rank_stability(np.array(active_matrix))
    best = max(results, key=lambda r: r["1x"]["robust_ratio"])
    gate = (best["1x"]["robust_ratio"] >= GATE_ACTIVE_RR
            and median_rho > GATE_MEDIAN_RHO)
    summary = {
        "anchor": str(anchor.date()), "n_folds": len(folds),
        "ew_bench_mean_fold": float(np.mean(ew_bench)),
        "btc_bench_mean_fold": float(np.mean(btc_bench)),
        "rank_stability_median_rho": median_rho,
        "best_config": best,
        "gate_active_rr": GATE_ACTIVE_RR, "gate_median_rho": GATE_MEDIAN_RHO,
        "gate_verdict": "PORT-TO-C" if gate else "KILL",
        "results": results,
    }
    with open(RESULTS_PATH, "w", encoding="utf-8") as fh:
        json.dump(summary, fh, indent=2)
    print(f"\nrank-stability median rho: {median_rho:+.3f} "
          f"(gate > {GATE_MEDIAN_RHO})")
    print(f"best config: k={best['k']} L={best['lookback']} "
          f"{best['weighting']} active rr {best['1x']['robust_ratio']:+.3f} "
          f"(gate >= {GATE_ACTIVE_RR})")
    print(f"VERDICT: {summary['gate_verdict']}  -> {RESULTS_PATH}")


if __name__ == "__main__":
    main()
