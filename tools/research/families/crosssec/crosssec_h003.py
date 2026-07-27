#!/usr/bin/env python3
"""crosssec_h003.py — H-crosssec-003: dispersion-gated deployment of the
H-001 machine (family crosssec / ROTATION-1, TESTING NIGHT 2026-07-26,
WKND-1 duty-officer wake — run after the 002 kill per the baton's weigh
rule, operator-delegated this morning).

PRE-REGISTRATION lives in tools/research/families/crosssec/HYPOTHESES.md
§H-crosssec-003 — written BEFORE any test by the generation context.
This docstring records how the pre-registered language is
operationalized, written BEFORE the first run — part of the
pre-registration, not commentary on results.

CONTRAST DESIGN: H-001's 8-config grid, signals, fills, costs, folds,
anchor and benchmark are reproduced EXACTLY (signals(), open_panel(),
run_config() imported from crosssec_h001.py unchanged; same
ANCHOR_HISTORY=235, same per-config eligibility, same grid k x rank_L x
sma). ONE new element: a cross-sectional dispersion gate on the
DECISION TO ROTATE. Gate-off numbers are NOT re-run — they are H-001's
RECORDED results (btdata/research/crosssec_H-crosssec-001_results.json,
killed data on disk), per the pre-registered contrast design.

  Dispersion    disp(t) = POPULATION std, across pairs eligible at t
                (shared anchor-eligibility panel, 235d convention),
                of the 90d total return close(t)/close(t-90) - 1.
                Defined only where >= 4 eligible pairs have a valid
                90d return (degenerate 2-3 pair stds excluded).
  Gate          "upper half of its trailing 2y range", fixed as:
                gate(t) = disp(t) >= midpoint of [rolling min, rolling
                max] of disp over the trailing 730d (min_periods=365 —
                evaluable once a year of dispersion history exists,
                which is BEFORE H-001's first fold; an unevaluable or
                NaN gate reads OFF = cash, the conservative side of
                "deploy only while"). "Range" midpoint (min+max)/2 is
                chosen over the median reading because the
                pre-registered sentence says RANGE; decided pre-run.
  Application   the gate acts at DECISION points only (the 5d
                rebalances), matching the pre-registered phrase
                "regime conditioning of the DECISION TO ROTATE":
                gate ON  -> rebalance target = H-001's top-k book,
                gate OFF -> rebalance target = all cash.
                Implemented by masking the score panel on gated-off
                days (score NaN -> can_hold False -> empty target);
                run_config is bit-identical to H-001's. Between
                rebalances holdings ride exactly as in H-001 (daily
                0.90*SMA hard-break exits unchanged) — the gate adds
                no new intraweek exit path and no new knobs.
  Grid          H-001's 8 configs x gate {on}: k in {2,3} x rank_L in
                {60,90} x sma in {100,200}. Zero re-tuning; census
                reports all 24 friction-cells.

  Verdict rule (fixed pre-run). VALIDATED requires BOTH:
    (A) the standing 7 absolute gates on the best gated config
        (identical block to h001/h002: active>=0.3, pos>=0.75, net
        worst>=-0.10, dd<=0.30, 2x/4x profit+consistency, rho>0.5);
    (B) the pre-registered CONTRAST — the gate must RAISE
          (B1) robust ratio: median per-config delta of 1x active rr
               (gated minus H-001 recorded, 8 pairs) > 0, AND
          (B2) rank stability: median rho of the gated 8-config 1x
               active matrix > H-001's recorded 0.083.
    Anything less -> killed ("if the gate only removes random folds
    the contrast shows no improvement and the hypothesis dies even if
    the gated book passes gates in absolute terms" — HYPOTHESES.md).
    The contrast deltas are reported cell-by-cell either way.

Usage:
    tools/research/.venv/bin/python \
        tools/research/families/crosssec/crosssec_h003.py
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
sys.path.insert(0, HERE)
import rig                                                # noqa: E402
import rotation                                           # noqa: E402
from crosssec_h001 import (                               # noqa: E402
    ANCHOR_HISTORY, GRID_K, GRID_RANK_L, GRID_SMA, MIN_UNIVERSE,
    open_panel, run_config, signals)

FAMILY = "crosssec-H-crosssec-003"
FRICTIONS = (1.0, 2.0, 4.0)
RET_DAYS = 90
RANGE_DAYS = 730
RANGE_MIN_PERIODS = 365
MIN_DISP_PAIRS = 4
H001_RESULTS = os.path.join(
    rig.DATA_DIR, "crosssec_H-crosssec-001_results.json")
RESULTS_PATH = os.path.join(
    rig.DATA_DIR, "crosssec_H-crosssec-003_results.json")


def dispersion_gate(close, elig_anchor):
    """(gate Series, disp Series) per the docstring's fixed definition."""
    ret90 = close / close.shift(RET_DAYS) - 1.0
    pool = ret90.where(elig_anchor)
    n = pool.notna().sum(axis=1)
    disp = pool.std(axis=1, ddof=0).where(n >= MIN_DISP_PAIRS)
    lo = disp.rolling(RANGE_DAYS, min_periods=RANGE_MIN_PERIODS).min()
    hi = disp.rolling(RANGE_DAYS, min_periods=RANGE_MIN_PERIODS).max()
    gate = disp.ge((lo + hi) / 2.0).fillna(False)
    return gate, disp


def main():
    frame = rig.load("1d")
    close, _volume, n_1m = rig.daily_panel(frame)
    opens = open_panel(frame, close)

    elig_anchor = rig.eligible(close, n_1m, min_history_days=ANCHOR_HISTORY)
    rich = elig_anchor.sum(axis=1) >= MIN_UNIVERSE
    anchor = rich[rich].index[0]
    span = close.loc[anchor:]
    folds = rig.fold_windows(span.index)
    print(f"universe anchor {anchor.date()}; {len(folds)} folds, "
          f"first test {folds[0][0].date()}, last {folds[-1][1].date()}")

    recorded = json.load(open(H001_RESULTS, encoding="utf-8"))
    assert recorded["anchor"] == str(anchor.date()), \
        "anchor drifted vs H-001's recorded run — contrast would be invalid"
    h001_by_cfg = {(r["k"], r["rank_l"], r["sma"]): r
                   for r in recorded["results"]}

    gate, disp = dispersion_gate(close, elig_anchor)
    in_span = gate.loc[anchor:]
    print(f"gate ON {in_span.mean():.1%} of span days "
          f"(disp defined from {disp.dropna().index[0].date()}, "
          f"gate evaluable from "
          f"{gate.loc[gate.index >= disp.dropna().index[0]].index[0].date()})")

    ew_bench, btc_bench = rotation.bench_curves(close, elig_anchor, folds)
    print(f"bench per-fold mean: EW {np.mean(ew_bench):+.2%}  "
          f"BTC {np.mean(btc_bench):+.2%}")

    results, active_matrix, deltas = [], [], []
    for k, rank_l, sma_len in itertools.product(
            GRID_K, GRID_RANK_L, GRID_SMA):
        elig = rig.eligible(close, n_1m,
                            min_history_days=max(sma_len, rank_l) + 35)
        qual, hard, score = signals(close, rank_l, sma_len)
        gated_score = score.copy()
        gated_score.loc[~gate] = np.nan
        row = {"k": k, "rank_l": rank_l, "sma": sma_len, "gate": "on"}
        for friction in FRICTIONS:
            equity, turnover, hard_exits, deploy = run_config(
                close, opens, elig, qual, hard, gated_score, k, friction,
                anchor)
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
                         "gate": "on", "friction": tag},
                {"active_rr": cell["active"]["robust_ratio"],
                 "active_mean_fold": cell["active"]["mean_fold"],
                 "active_pos_frac": cell["active"]["pos_frac"],
                 "net_worst_fold": cell["net"]["worst_fold"],
                 "mtm_max_dd": cell["mtm_max_dd"]})
            if friction == 1.0:
                active_matrix.append(active)
        off = h001_by_cfg[(k, rank_l, sma_len)]
        delta = (row["1x"]["active"]["robust_ratio"]
                 - off["1x"]["active"]["robust_ratio"])
        deltas.append(delta)
        row["delta_active_rr_1x_vs_h001"] = delta
        one = row["1x"]
        print(f"k={k} L={rank_l} sma={sma_len:>3} gate=on | active rr "
              f"{one['active']['robust_ratio']:+.3f} "
              f"(off {off['1x']['active']['robust_ratio']:+.3f}, "
              f"delta {delta:+.3f}) pos {one['active']['pos_frac']:.2f} | "
              f"net worst {one['net']['worst_fold']:+.2%} "
              f"dd {one['mtm_max_dd']:.1%} deploy {one['deploy_mean']:.2f}")
        results.append(row)

    median_rho, _ = rig.rank_stability(np.array(active_matrix))
    rho_off = recorded["rank_stability_median_rho"]
    median_delta = float(np.median(deltas))
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
        "contrast_rr": median_delta > 0.0,
        "contrast_rho": median_rho > rho_off,
    }
    verdict = "validated-port-candidate" if all(gates.values()) else "killed"
    failed = [name for name, ok in gates.items() if not ok]

    summary = {
        "hypothesis_id": "H-crosssec-003",
        "anchor": str(anchor.date()), "n_folds": len(folds),
        "fold_span": [str(folds[0][0].date()), str(folds[-1][1].date())],
        "gate_on_frac_span": float(in_span.mean()),
        "ew_bench_mean_fold": float(np.mean(ew_bench)),
        "rank_stability_median_rho": median_rho,
        "h001_recorded_rho": rho_off,
        "median_delta_active_rr_1x": median_delta,
        "per_config_deltas": deltas,
        "best_config": best,
        "gates": gates, "verdict": verdict, "failed_gates": failed,
        "results": results,
    }
    with open(RESULTS_PATH, "w", encoding="utf-8") as fh:
        json.dump(summary, fh, indent=2)

    print(f"\ncontrast: median delta active rr {median_delta:+.3f} "
          f"(gate B1 > 0); rho {median_rho:+.3f} vs H-001 recorded "
          f"{rho_off:+.3f} (gate B2)")
    print(f"best gated config: k={best['k']} L={best['rank_l']} "
          f"sma={best['sma']} | 1x active rr "
          f"{b1['active']['robust_ratio']:+.3f} pos "
          f"{b1['active']['pos_frac']:.2f} | net worst "
          f"{b1['net']['worst_fold']:+.2%} dd {b1['mtm_max_dd']:.1%}")
    print(f"gates: {gates} -> VERDICT {verdict} (failed: {failed})")
    print(f"wrote {RESULTS_PATH}")


if __name__ == "__main__":
    main()
