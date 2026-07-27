#!/usr/bin/env python3
"""crosssec_h001.py — H-crosssec-001: absolute-qualified risk-adjusted
rotation, long-only with a first-class cash book (family crosssec /
ROTATION-1, TESTING NIGHT 2026-07-23).

PRE-REGISTRATION lives in tools/research/families/crosssec/HYPOTHESES.md
§H-crosssec-001 — thesis, named differences vs the WM-EDGE-2 killed
family, benchmark, expected fold signature and the 8-config grid were
written there BEFORE this script existed, by a different (generation)
context. This docstring records how the pre-registered language is
operationalized, written BEFORE the first run — it is part of the
pre-registration, not commentary on results:

  Eligibility   rig.eligible() verbatim (point-in-time, liquidity floor
                trailing-30d median >= 300 traded 1m-bars/day). Per
                config, min_history_days = max(sma_len, rank_L) + 35
                (rotation.py convention). Universe anchor + folds are
                SHARED across all 8 configs, taken at the strictest
                history need (SMA 200 + 35 = 235d, >= 6 pairs).
  Qualifier     close > own SMA(sma_len), daily, min_periods = sma_len.
  Ranking       among qualifiers only: rank_L-day total return divided
                by std of daily returns over rank_L. No score-sign
                constraint — the absolute-trend qualifier IS the
                absolute filter (it replaces EDGE-2's score>0 rule).
  Book          top-k qualifiers at 1/k weight each (breadth-scaled
                deploy, EDGE-2 convention): j < k qualifiers -> j/k
                deployed, remainder cash; zero qualifiers -> all cash.
  Rebalance     every 5th calendar day from the anchor (full UTC daily
                calendar; "5 trading days" on a 24/7 market is 5
                calendar days). Decision at close.
  Exits         daily between rebalances: a HELD asset closing below
                0.90 * its qualifier SMA is sold at the next open,
                proceeds parked in cash until the next scheduled
                rebalance (an off-schedule redeploy would be a hidden
                extra rebalance). A holding below SMA but above the
                0.90 break waits for the next rebalance.
  Fills         decision at close(t), filled at open(t+1) — lag-1
                next-open from the start (standing rule). Daily accrual
                is split-leg: old weights earn close(t)->open(t+1),
                the fill and its turnover cost land at open(t+1), new
                weights earn open(t+1)->close(t+1). A no-trade day's
                missing open is the prior close (price persistence).
  Costs         rig.turnover_cost engine economics (5+5 bps per side)
                x friction 1x/2x/4x.
  Benchmark     rotation.bench_curves verbatim: frictionless EW
                buy-and-hold of the fold-start eligible universe at the
                shared anchor eligibility (+ btc-usd context).
  Folds         rig.fold_windows over the anchored span (120d tests);
                WM-RIGOR-6 cutoff enforced by rig.load.

  Grid (pre-committed, census reports all 24 friction-cells):
  k in {2,3} x rank_L in {60,90}d x sma in {100,200}d.

  Gates (HYPOTHESES.md standing rules, operationalized before the run;
  "best" = highest 1x ACTIVE robust ratio, rotation.py convention; rho
  over the 8-config 1x active fold matrix):
    active  1x active rr >= 0.3            (best config)
    pos     1x active pos_frac >= 0.75     (best config)
    worst   1x NET worst fold >= -0.10     (best config — the thesis's
            "passes by construction" claim is about the book's own
            fold return, so this gate reads the net curve)
    dd      1x mtm max drawdown <= 0.30    (best config equity path)
    2x/4x   active mean_fold > 0 AND active pos_frac >= 0.5 at that
            friction ("survive profit + consistency")
    rho     rank-stability median rho > 0.5
  VALIDATED requires ALL; any failure -> killed.

Usage:
    tools/research/.venv/bin/python \
        tools/research/families/crosssec/crosssec_h001.py
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
import rotation                                          # noqa: E402

FAMILY = "crosssec-H-crosssec-001"
GRID_K = (2, 3)
GRID_RANK_L = (60, 90)
GRID_SMA = (100, 200)
FRICTIONS = (1.0, 2.0, 4.0)
REBAL_DAYS = 5
HARD_BREAK = 0.90                    # sell next open below 0.90 * SMA
MIN_UNIVERSE = 6                     # rotation.py convention
ANCHOR_HISTORY = max(GRID_SMA) + 35  # strictest config seasons the anchor
RESULTS_PATH = os.path.join(
    rig.DATA_DIR, "crosssec_H-crosssec-001_results.json")


def open_panel(frame, close):
    """Opens on the close panel's calendar; missing open = prior close."""
    opens = frame.pivot(index="ts", columns="pair", values="open")
    opens = opens.reindex(close.index)
    return opens.fillna(close.shift(1))


def signals(close, rank_l, sma_len):
    """(qualifier, hard_break, score) panels for one config."""
    sma = close.rolling(sma_len, min_periods=sma_len).mean()
    qual = close.gt(sma)
    hard = close.lt(HARD_BREAK * sma)
    ret_l = close / close.shift(rank_l) - 1.0
    vol_l = close.pct_change().rolling(rank_l, min_periods=rank_l).std()
    score = (ret_l / vol_l).replace([np.inf, -np.inf], np.nan)
    return qual, hard, score


def run_config(close, opens, elig, qual, hard, score, k, friction, anchor):
    """Daily equity from the anchor for one config x friction.

    Returns (equity Series, turnover_total, hard_exits, deploy_mean).
    """
    idx = close.index
    a0 = idx.get_loc(anchor)
    price_c = close.to_numpy(dtype=float)
    price_o = opens.to_numpy(dtype=float)
    can_hold = (qual & elig & score.notna()).to_numpy(dtype=bool)
    hard_brk = hard.to_numpy(dtype=bool)
    rank = score.to_numpy(dtype=float)

    n_days, n_assets = price_c.shape
    weights = np.zeros(n_assets)
    pending = None                   # ('rebal', target) | ('exit', mask)
    value, turnover, hard_exits, deploy_sum = 1.0, 0.0, 0, 0.0
    equity = np.empty(n_days - a0)
    equity[0] = 1.0

    def rebalance_target(day):
        held = np.where(can_hold[day])[0]
        target = np.zeros(n_assets)
        if len(held):
            top = held[np.argsort(rank[day, held])[::-1][:k]]
            target[top] = 1.0 / k
        return target

    pending = ("rebal", rebalance_target(a0))   # decision at anchor close

    for day in range(a0 + 1, n_days):
        # overnight leg: yesterday's close -> today's open, old weights
        gap = np.nan_to_num(price_o[day] / price_c[day - 1] - 1.0)
        port = float(weights @ gap)
        value *= 1.0 + port
        if weights.any():
            weights = weights * (1.0 + gap) / (1.0 + port)

        if pending is not None:
            kind, data = pending
            target = data if kind == "rebal" else np.where(data, 0.0, weights)
            delta = target - weights
            value *= 1.0 - rig.turnover_cost(pd.Series(delta), friction)
            turnover += float(np.abs(delta).sum())
            weights = target
            pending = None

        # intraday leg: today's open -> today's close, new weights
        intra = np.nan_to_num(price_c[day] / price_o[day] - 1.0)
        port = float(weights @ intra)
        value *= 1.0 + port
        if weights.any():
            weights = weights * (1.0 + intra) / (1.0 + port)
        equity[day - a0] = value
        deploy_sum += float(weights.sum())

        # decision at today's close, filled tomorrow
        if (day - a0) % REBAL_DAYS == 0:
            pending = ("rebal", rebalance_target(day))
        else:
            broken = (weights > 1e-12) & hard_brk[day]
            if broken.any():
                hard_exits += int(broken.sum())
                pending = ("exit", broken)

    series = pd.Series(equity, index=idx[a0:])
    return series, turnover, hard_exits, deploy_sum / (n_days - a0 - 1)


def main():
    frame = rig.load("1d")
    close, _volume, n_1m = rig.daily_panel(frame)
    opens = open_panel(frame, close)

    elig_anchor = rig.eligible(close, n_1m, min_history_days=ANCHOR_HISTORY)
    rich = elig_anchor.sum(axis=1) >= MIN_UNIVERSE
    anchor = rich[rich].index[0]
    span = close.loc[anchor:]
    folds = rig.fold_windows(span.index)
    print(f"universe anchor {anchor.date()} (>= {MIN_UNIVERSE} pairs at "
          f"{ANCHOR_HISTORY}d history); {len(folds)} folds, "
          f"first test {folds[0][0].date()}, last {folds[-1][1].date()}")

    ew_bench, btc_bench = rotation.bench_curves(close, elig_anchor, folds)
    print(f"bench per-fold mean: EW {np.mean(ew_bench):+.2%}  "
          f"BTC {np.mean(btc_bench):+.2%}")

    results, active_matrix = [], []
    for k, rank_l, sma_len in itertools.product(
            GRID_K, GRID_RANK_L, GRID_SMA):
        elig = rig.eligible(close, n_1m,
                            min_history_days=max(sma_len, rank_l) + 35)
        qual, hard, score = signals(close, rank_l, sma_len)
        row = {"k": k, "rank_l": rank_l, "sma": sma_len}
        for friction in FRICTIONS:
            equity, turnover, hard_exits, deploy = run_config(
                close, opens, elig, qual, hard, score, k, friction, anchor)
            strat = rotation.fold_returns(equity, folds)
            active = [s - b for s, b in zip(strat, ew_bench)]
            cell = {
                "active": rig.fold_metrics(active),
                "net": rig.fold_metrics(strat),
                "mtm_max_dd": rig.max_drawdown(equity),
                "turnover_per_year": turnover / (len(span) / 365.25),
                "hard_exits": hard_exits,
                "deploy_mean": deploy,
            }
            tag = {1.0: "1x", 2.0: "2x", 4.0: "4x"}[friction]
            row[tag] = cell
            rig.census_append(
                FAMILY, {"k": k, "rank_l": rank_l, "sma": sma_len,
                         "friction": tag},
                {"active_rr": cell["active"]["robust_ratio"],
                 "active_mean_fold": cell["active"]["mean_fold"],
                 "active_pos_frac": cell["active"]["pos_frac"],
                 "net_worst_fold": cell["net"]["worst_fold"],
                 "mtm_max_dd": cell["mtm_max_dd"]})
            if friction == 1.0:
                active_matrix.append(active)
        one = row["1x"]
        print(f"k={k} L={rank_l} sma={sma_len:>3} | active rr "
              f"{one['active']['robust_ratio']:+.3f} "
              f"mean {one['active']['mean_fold']:+.2%} "
              f"pos {one['active']['pos_frac']:.2f} | net worst "
              f"{one['net']['worst_fold']:+.2%} dd {one['mtm_max_dd']:.1%} "
              f"deploy {one['deploy_mean']:.2f} | 4x active rr "
              f"{row['4x']['active']['robust_ratio']:+.3f}")
        results.append(row)

    median_rho, _ = rig.rank_stability(np.array(active_matrix))
    best = max(results, key=lambda r: r["1x"]["active"]["robust_ratio"])
    b1, b2, b4 = best["1x"], best["2x"], best["4x"]
    gates = {
        "active": b1["active"]["robust_ratio"] >= 0.3,
        "pos": b1["active"]["pos_frac"] >= 0.75,
        "worst": b1["net"]["worst_fold"] >= -0.10,
        "dd": b1["mtm_max_dd"] <= 0.30,
        "2x": (b2["active"]["mean_fold"] > 0.0
               and b2["active"]["pos_frac"] >= 0.5),
        "4x": (b4["active"]["mean_fold"] > 0.0
               and b4["active"]["pos_frac"] >= 0.5),
        "rho": median_rho > 0.5,
    }
    verdict = "validated-port-candidate" if all(gates.values()) else "killed"
    failed = [name for name, ok in gates.items() if not ok]

    summary = {
        "hypothesis_id": "H-crosssec-001",
        "anchor": str(anchor.date()), "n_folds": len(folds),
        "fold_span": [str(folds[0][0].date()), str(folds[-1][1].date())],
        "ew_bench_mean_fold": float(np.mean(ew_bench)),
        "btc_bench_mean_fold": float(np.mean(btc_bench)),
        "rank_stability_median_rho": median_rho,
        "best_config": best,
        "gates": gates, "verdict": verdict, "failed_gates": failed,
        "results": results,
    }
    with open(RESULTS_PATH, "w", encoding="utf-8") as fh:
        json.dump(summary, fh, indent=2)

    print(f"\nrank-stability median rho: {median_rho:+.3f} (gate > 0.5)")
    print(f"best config: k={best['k']} L={best['rank_l']} "
          f"sma={best['sma']} | 1x active rr "
          f"{b1['active']['robust_ratio']:+.3f} pos "
          f"{b1['active']['pos_frac']:.2f} | net worst "
          f"{b1['net']['worst_fold']:+.2%} dd {b1['mtm_max_dd']:.1%}")
    print(f"gates: {gates} -> VERDICT {verdict} (failed: {failed})")
    print(f"wrote {RESULTS_PATH}")


if __name__ == "__main__":
    main()
