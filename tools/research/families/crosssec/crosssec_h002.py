#!/usr/bin/env python3
"""crosssec_h002.py — H-crosssec-002: compression-release rotation,
long-only with a first-class cash book (family crosssec / ROTATION-1,
TESTING NIGHT 2026-07-26, WKND-1 duty-officer wake).

PRE-REGISTRATION lives in tools/research/families/crosssec/HYPOTHESES.md
§H-crosssec-002 — thesis, named differences vs the WM-EDGE-2 killed
family, benchmark, expected fold signature and the 8-config grid were
written there BEFORE this script existed, by a different (generation)
context. This docstring records how the pre-registered language is
operationalized, written BEFORE the first run — it is part of the
pre-registration, not commentary on results.

Everything below the ranking signal is crosssec_h001.py VERBATIM — this
module imports open_panel() and run_config() from it unchanged (same
lag-1 next-open split-leg fills, engine turnover costs, breadth-scaled
top-k book, 5d rebalance, 0.90*SMA hard break, rotation.bench_curves EW
benchmark, rig.fold_windows folds, identical gate block). Only signals()
differs.

  Qualifier     close > own SMA(100), daily, min_periods=100. The SMA
                length is NOT in H-002's grid ("same qualifier
                conventions as H-001"); fixed BEFORE this run from
                H-001's RECORDED killed-data census by a stated rule —
                pick the sma dominant on 1x active rr across the four
                (k,L) cells: sma=100 wins 3 of 4 (and the best cell).
                Chosen from data recorded before this session; not
                tuned within this run.
  Compression   vol20(t) = std of daily pct returns over 20d
                (min_periods=20). compressed(t) iff vol20(t) <= the
                q-quantile of its OWN vol20 over the trailing 365d
                (rolling, min_periods=365). q is gridded {0.20, 0.30}
                ("compression quintile" pre-registered as bottom 20% /
                30% of the trailing year).
  Release       fired(t), for release window w in {5,10}d, iff ALL of:
                  compressed(t-w)            — was compressed at window
                                               start (shift fill=False)
                  NOT compressed(t)          — has left compression
                  vol20(t) > vol20(t-w)      — range expanded over the
                                               window
                  close(t) > close(t-w)      — the expansion is upward
                All four conditions derive from the pre-registered
                sentence; no new magnitude thresholds are introduced.
  Recency/decay a release keeps its asset rankable for DECAY_DAYS=10
                daily decisions starting the fire day (days_since 0..9;
                "fires and decays over 10d" is pre-registered and NOT
                gridded). Assets with no release inside the window are
                unrankable (score NaN -> cannot be held; the book may
                be empty because nothing released recently).
  Ranking       score = (10 - days_since) + tie, tie = r/(1+r) where
                r = vol20(fire)/vol20(fire-w), the expansion ratio at
                the most recent fire. tie is bounded in (0,1) so it can
                NEVER flip recency order — it only breaks same-day
                ties (releases cluster) by release strength. A
                non-finite r takes a neutral tie of 0.5.
  Book/exits    run_config verbatim: top-k rankable qualifiers at 1/k
                (j<k -> j/k deployed, rest cash; zero -> all cash);
                rebalance every 5 calendar days; daily hard break at
                0.90*SMA(100) sold next open; a merely-disqualified or
                decayed holding waits for the next rebalance.
  Eligibility   rig.eligible() verbatim. min_history_days = 420 for
                ALL configs (vol-year 365 + vol20 20 + 35-day buffer,
                the h001 "+35" convention; strictest need is shared —
                the grid does not vary any history-bearing length).
                Universe anchor at >= 6 eligible pairs; anchor, folds
                and EW benchmark shared across all 8 configs.
  Data          btdata/research/ohlcv_1d.parquet verified THIS session
                as a 27-pair superset of the research universe
                (jto/ondo/aero/tia all present); rig.load enforces the
                2025-03-31 cutoff.

  Grid (pre-committed, census reports all 24 friction-cells):
  k in {2,3} x q in {0.20,0.30} x w in {5,10}d.

  Gates (identical block to crosssec_h001.py; "best" = highest 1x
  ACTIVE robust ratio; rho over the 8-config 1x active fold matrix):
    active  1x active rr >= 0.3            (best config)
    pos     1x active pos_frac >= 0.75     (best config)
    worst   1x NET worst fold >= -0.10     (best config)
    dd      1x mtm max drawdown <= 0.30    (best config equity path)
    2x/4x   active mean_fold > 0 AND active pos_frac >= 0.5
    rho     rank-stability median rho > 0.5
  VALIDATED requires ALL; any failure -> killed.

Usage:
    tools/research/.venv/bin/python \
        tools/research/families/crosssec/crosssec_h002.py
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
from crosssec_h001 import open_panel, run_config          # noqa: E402

FAMILY = "crosssec-H-crosssec-002"
GRID_K = (2, 3)
GRID_Q = (0.20, 0.30)
GRID_W = (5, 10)
FRICTIONS = (1.0, 2.0, 4.0)
SMA_LEN = 100                        # fixed pre-run (docstring rule)
VOL_LEN = 20
VOL_YEAR = 365
DECAY_DAYS = 10
MIN_UNIVERSE = 6                     # rotation.py convention
ANCHOR_HISTORY = VOL_YEAR + VOL_LEN + 35   # shared by all 8 configs
RESULTS_PATH = os.path.join(
    rig.DATA_DIR, "crosssec_H-crosssec-002_results.json")


def signals(close, q, w):
    """(qualifier, hard_break, score) panels for one (q, w) config.

    score encodes compression-release recency (docstring): rankable only
    within DECAY_DAYS of the most recent release; strength tie-break is
    bounded < 1 so recency order is never flipped.
    """
    sma = close.rolling(SMA_LEN, min_periods=SMA_LEN).mean()
    qual = close.gt(sma)
    hard = close.lt(0.90 * sma)

    vol = close.pct_change().rolling(VOL_LEN, min_periods=VOL_LEN).std()
    floor = vol.rolling(VOL_YEAR, min_periods=VOL_YEAR).quantile(q)
    compressed = vol.le(floor)

    fired = (compressed.shift(w, fill_value=False)
             & ~compressed
             & vol.gt(vol.shift(w))
             & close.gt(close.shift(w)))

    ratio = (vol / vol.shift(w)).where(fired)
    ratio = ratio.replace([np.inf, -np.inf], np.nan)
    tie = (ratio / (1.0 + ratio)).where(ratio.notna(), 0.5).where(fired)

    day_no = np.arange(len(close.index), dtype=float)[:, None]
    fire_day = pd.DataFrame(
        np.where(fired.to_numpy(), day_no, np.nan),
        index=close.index, columns=close.columns).ffill()
    days_since = pd.DataFrame(
        day_no - fire_day.to_numpy(),
        index=close.index, columns=close.columns)

    score = (DECAY_DAYS - days_since) + tie.ffill()
    score = score.where(days_since < DECAY_DAYS)
    return qual, hard, score


def main():
    frame = rig.load("1d")
    close, _volume, n_1m = rig.daily_panel(frame)
    opens = open_panel(frame, close)

    elig = rig.eligible(close, n_1m, min_history_days=ANCHOR_HISTORY)
    rich = elig.sum(axis=1) >= MIN_UNIVERSE
    anchor = rich[rich].index[0]
    span = close.loc[anchor:]
    folds = rig.fold_windows(span.index)
    print(f"universe anchor {anchor.date()} (>= {MIN_UNIVERSE} pairs at "
          f"{ANCHOR_HISTORY}d history); {len(folds)} folds, "
          f"first test {folds[0][0].date()}, last {folds[-1][1].date()}")

    ew_bench, btc_bench = rotation.bench_curves(close, elig, folds)
    print(f"bench per-fold mean: EW {np.mean(ew_bench):+.2%}  "
          f"BTC {np.mean(btc_bench):+.2%}")

    results, active_matrix = [], []
    for k, q, w in itertools.product(GRID_K, GRID_Q, GRID_W):
        qual, hard, score = signals(close, q, w)
        row = {"k": k, "q": q, "w": w}
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
                FAMILY, {"k": k, "q": q, "w": w, "friction": tag},
                {"active_rr": cell["active"]["robust_ratio"],
                 "active_mean_fold": cell["active"]["mean_fold"],
                 "active_pos_frac": cell["active"]["pos_frac"],
                 "net_worst_fold": cell["net"]["worst_fold"],
                 "mtm_max_dd": cell["mtm_max_dd"]})
            if friction == 1.0:
                active_matrix.append(active)
        one = row["1x"]
        print(f"k={k} q={q:.2f} w={w:>2} | active rr "
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
        "hypothesis_id": "H-crosssec-002",
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
    print(f"best config: k={best['k']} q={best['q']:.2f} w={best['w']} | "
          f"1x active rr {b1['active']['robust_ratio']:+.3f} pos "
          f"{b1['active']['pos_frac']:.2f} | net worst "
          f"{b1['net']['worst_fold']:+.2%} dd {b1['mtm_max_dd']:.1%}")
    print(f"gates: {gates} -> VERDICT {verdict} (failed: {failed})")
    print(f"wrote {RESULTS_PATH}")


if __name__ == "__main__":
    main()
