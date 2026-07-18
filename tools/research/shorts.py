#!/usr/bin/env python3
"""shorts.py — WM-EDGE-4: short-side prototypes (mirror + pairs).

PRE-REGISTRATION (written before any result was computed — this docstring
is the hypothesis record; do not edit it to fit results):

FAMILY (a) "mirror" — pure artifact analysis, no new backtests.

  Thesis: if the finalists' long edge is TIMING (entering before
  idiosyncratic up-moves), a mirrored short of the same fill days should
  retain some of that information content when judged against the only
  fair null — shorting the benchmark itself. If the edge is BETA (being
  long a bull decade), the mirror is just an inverted index ride and
  should show no margin over that null. Evidence feeds the mako kill
  review (fill-fiction suspect) and the timing-vs-beta question of
  ROUND 5b; there is NO port gate on this family.

  Expected fold signature: mirror-vs-null margin positive and largest in
  monster bull folds (the null bleeds every day; the mirror is flat most
  days). The sharper, harder read is the held-day-only stat (below): a
  dip-buyer's held days are up-biased by construction, so held-day mirror
  returns should be NEGATIVE; the question is by how much vs −bench on
  those same folds. For mako specifically we expect weak timing content
  (ROUND 6 showed its fills were fiction at next-open).

  Inputs (pinned): the six ROUND 6 close-fill 1× official result dirs
  under /mnt/fast/web/lame/whenmoon/ — mako 20260717-163449 (btc) /
  163452 (eth), riptide 163455/163458, juggernaut 163501/163504. Each
  holds equity.jsonl (daily MTM marks spanning the test region) and one
  iterations.jsonl line whose windows[] carry per-fold return and
  bench_return.

  Method (fixed): daily return r_t from consecutive equity marks;
  held-day mask |r_t| > 1e-9 (paper equity only moves on held/fill
  days); mirror daily return = −r_t − borrow_daily on held days, 0
  otherwise; mirror equity rebuilt by compounding. Folds: a daily
  calendar from windows[0].start to windows[-1].end, tiled by
  rig.fold_windows(train_days=0) — ASSERT fold count AND first/last
  boundaries match the C windows[] before trusting anything. Null per
  fold = −bench_return − borrow_daily × fold_days (an always-on short
  of the benchmark pays borrow every day). Margin per fold =
  mirror_fold − null_fold. Borrow grid {5, 10, 15} bps/day on the short
  notional; 10 is the primary reporting cell. Report per run and pooled
  btc+eth per finalist: rig.fold_metrics over the margin, its median,
  mirror-only metrics, held_frac, and mean held-day mirror return.

  Pre-registered caveats (properties of the method, stated up front):
  (1) this inverts NET long returns — the long's fees ride along,
  ≈ same magnitude as the short's own; first-order acceptable.
  (2) convexity asymmetry: −bench_return is an arithmetic inversion and
  can breach −100% in monster bull folds, while the compounded mirror
  equity cannot — the null is biased DOWN exactly there, flattering the
  margin; the median margin and the held-day stat are the antidotes,
  read them together. (3) the flat-most-days advantage means margin > 0
  alone does NOT prove timing; only the held-day read does.

FAMILY (b) "pairs" — market-neutral reversion, in-rig, 1h closes,
lag-1 execution FROM THE START (signal on close t, execute close t+1 —
the ROUND 6 lesson is not optional here).

  Thesis: log-price ratios of large-cap crypto pairs mean-revert over
  days-to-weeks; a market-neutral long-cheap/short-rich book harvests
  that reversion with near-zero beta, making it the genuinely
  diversifying family (pays in chop, indifferent to direction).

  Expected fold signature: small steady positive folds in ranging
  regimes; worst folds at structural divergences (a leg repricing
  secularly — e.g. sol's 2023-24 run), capped by the z-stop and
  max-hold; costs bite hard at 2×/4× (every round trip is four sides).

  Fixed design (not swept): spreads btc-eth, btc-sol, eth-sol on the
  1h research panel (WM-RIGOR-6 cutoff enforced by rig.load; no
  holdout flag — pairs never touch post-cutoff data). z-score of
  log(P_a/P_b) over a rolling W-hour window (min_periods=W). Entry on
  a CROSSING while flat: |z| < z_entry on the prior decision bar and
  z_entry ≤ |z| < 4 now, both legs eligible that day — enter
  short-rich/long-cheap at 0.5/0.5 notional. Exit when |z| ≤ 0.5
  (converged) OR |z| ≥ 4 (regime break) OR held ≥ 2×W hours (stale).
  The crossing rule is the re-entry cooldown: after any exit, |z| must
  fall below z_entry before a new entry can arm. Every decision
  executes at the NEXT hourly close. Forced flat at end-of-data, cost
  charged. Accounting: constant 0.5/−0.5 weights of current equity
  while held (implicit hourly rebalance — a modeling choice, stated);
  hourly book return = 0.5·r_cheap − 0.5·r_rich − borrow drag; borrow
  accrues on the short leg's 0.5 notional (borrow_daily × 0.5 / 24 of
  equity per held hour). Entry and exit each trade 1.0 total weight →
  rig.turnover_cost engine economics (5+5 bps per side × friction).

  Eligibility/folds: rig.eligible on the DAILY panel with
  min_history_days = 65 (longest W in days, 30, + 35 — the rotation
  convention), liquidity floor 300 traded 1m-bars/day; spread anchor =
  first day both legs eligible; folds = rig.fold_windows on the hourly
  index from the anchor, official recipe (365d burn-in, 120d test,
  120d step), identical for all configs of a spread. Benchmark = cash
  (market-neutral): active = net. MTM drawdown per spread from the
  first fold start; the pooled config's DD = WORST spread (strict).

  Pre-registered grid (nothing outside it will be reported):
  W ∈ {240, 720} hours × z_entry ∈ {1.5, 2.0, 2.5} = 6 configs, pooled
  across the 3 spreads; frictions {1×, 2×, 4×}; borrow {5, 10, 15}
  bps/day. Every scored (config, friction, borrow) cell → census.

  GATE (set in the TODO before this file existed; judged at 1× fees +
  10 bps/day borrow on pooled folds): best-config pooled active
  robust_ratio ≥ 0.3 AND pos_frac ≥ 0.75 AND worst_fold ≥ −0.10 AND
  mtm max DD ≤ 0.30 AND rank-stability median ρ > 0.5 across the
  6-config matrix (n=6 is a SMALL matrix — say so when reporting ρ).
  PASS ⇒ author the C FLAT/SHORT engine chunk. FAIL ⇒ kill-with-why.

Usage:
    tools/research/.venv/bin/python tools/research/shorts.py --family mirror
    tools/research/.venv/bin/python tools/research/shorts.py --family pairs
"""

import argparse
import itertools
import json
import os
import sys

import numpy as np
import pandas as pd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402

ROUND6_ROOT = "/mnt/fast/web/lame/whenmoon"
ROUND6_RUNS = (
    ("mako", "btc-usd", "20260717-163449-mako-btc-usd"),
    ("mako", "eth-usd", "20260717-163452-mako-eth-usd"),
    ("riptide", "btc-usd", "20260717-163455-riptide-btc-usd"),
    ("riptide", "eth-usd", "20260717-163458-riptide-eth-usd"),
    ("juggernaut", "btc-usd", "20260717-163501-juggernaut-btc-usd"),
    ("juggernaut", "eth-usd", "20260717-163504-juggernaut-eth-usd"),
)
BORROW_BPS_DAY = (5.0, 10.0, 15.0)
BORROW_PRIMARY = 10.0

PAIRS_SPREADS = (("btc-usd", "eth-usd"),
                 ("btc-usd", "sol-usd"),
                 ("eth-usd", "sol-usd"))
GRID_W_HOURS = (240, 720)
GRID_Z_ENTRY = (1.5, 2.0, 2.5)
Z_EXIT = 0.5
Z_STOP = 4.0
MAX_HOLD_MULT = 2
FRICTIONS = (1.0, 2.0, 4.0)
PAIRS_MIN_HISTORY_DAYS = 65

GATE_ACTIVE_RR = 0.3
GATE_POS_FRAC = 0.75
GATE_WORST_FOLD = -0.10
GATE_MTM_DD = 0.30
GATE_MEDIAN_RHO = 0.5

MIRROR_RESULTS = os.path.join(rig.DATA_DIR, "shorts_mirror_results.json")
PAIRS_RESULTS = os.path.join(rig.DATA_DIR, "shorts_pairs_results.json")


# ------------------------------------------------------------- family (a)

def _load_run(dir_id):
    """(equity Series indexed by mark ts, windows list) for one result dir."""
    base = os.path.join(ROUND6_ROOT, dir_id)
    with open(os.path.join(base, "iterations.jsonl"), encoding="utf-8") as fh:
        windows = json.loads(fh.readline())["windows"]
    ts, eq = [], []
    with open(os.path.join(base, "equity.jsonl"), encoding="utf-8") as fh:
        for line in fh:
            mark = json.loads(line)
            ts.append(mark["ts_ms"])
            eq.append(mark["equity"])
    equity = pd.Series(eq, index=pd.to_datetime(ts, unit="ms"))
    return equity, windows


def _fold_parity(windows):
    """C-aligned folds via rig.fold_windows, with the parity ASSERTs."""
    w0 = pd.Timestamp(windows[0]["start_ts_ms"], unit="ms")
    w_end = pd.Timestamp(windows[-1]["end_ts_ms"], unit="ms")
    calendar = pd.date_range(w0, w_end, freq="D")
    folds = rig.fold_windows(calendar, train_days=0)
    assert len(folds) == len(windows), (
        f"fold parity broken: rig tiled {len(folds)} folds, "
        f"C run has {len(windows)} windows")
    assert folds[0][0] == w0 and folds[-1][1] == w_end, (
        "fold boundary drift vs the C windows[] — do not trust this run")
    return folds


def _equity_at(equity, when):
    """Last mark at or before `when`; 1.0 (start-normalized) before any."""
    pos = equity.index.searchsorted(when, side="right") - 1
    return float(equity.iloc[pos]) if pos >= 0 else float(equity.iloc[0])


def _mirror_one(equity, windows, borrow_bps_day):
    """Mirror fold returns, null fold returns, and held-day stats."""
    borrow = borrow_bps_day / 1e4
    rets = equity.pct_change().fillna(0.0)
    held = rets.abs() > 1e-9
    mirror_rets = (-rets - borrow).where(held, 0.0)
    mirror_eq = (1.0 + mirror_rets).cumprod()

    folds = _fold_parity(windows)
    mirror_f, null_f = [], []
    for (start, end), win in zip(folds, windows):
        fold_days = (end - start).total_seconds() / 86400.0
        mirror_f.append(_equity_at(mirror_eq, end)
                        / _equity_at(mirror_eq, start) - 1.0)
        null_f.append(-win["bench_return"] - borrow * fold_days)
    margin = [m - n for m, n in zip(mirror_f, null_f)]
    return {
        "mirror_folds": mirror_f,
        "null_folds": null_f,
        "margin_folds": margin,
        "held_frac": float(held.mean()),
        "heldday_mean": float(mirror_rets[held].mean()) if held.any()
                        else 0.0,
    }


def mirror_family():
    runs, pooled = [], {}
    for finalist, market, dir_id in ROUND6_RUNS:
        equity, windows = _load_run(dir_id)
        row = {"finalist": finalist, "market": market, "dir": dir_id,
               "n_folds": len(windows), "borrow": {}}
        for borrow in BORROW_BPS_DAY:
            one = _mirror_one(equity, windows, borrow)
            metrics = rig.fold_metrics(one["margin_folds"])
            metrics["median_margin"] = float(np.median(one["margin_folds"]))
            metrics["mirror_mean_fold"] = float(np.mean(one["mirror_folds"]))
            metrics["null_mean_fold"] = float(np.mean(one["null_folds"]))
            metrics["held_frac"] = one["held_frac"]
            metrics["heldday_mean"] = one["heldday_mean"]
            row["borrow"][f"{borrow:g}"] = metrics
            pooled.setdefault(finalist, {}).setdefault(
                f"{borrow:g}", []).extend(one["margin_folds"])
            rig.census_append(
                "shorts-mirror",
                {"finalist": finalist, "market": market,
                 "borrow_bps_day": borrow},
                {kk: metrics[kk] for kk in
                 ("robust_ratio", "mean_fold", "pos_frac")})
        runs.append(row)
        prim = row["borrow"][f"{BORROW_PRIMARY:g}"]
        print(f"{finalist:<10} {market} | margin-vs-null rr "
              f"{prim['robust_ratio']:+.3f} mean {prim['mean_fold']:+.2%} "
              f"median {prim['median_margin']:+.2%} | mirror mean "
              f"{prim['mirror_mean_fold']:+.2%} null mean "
              f"{prim['null_mean_fold']:+.2%} | held {prim['held_frac']:.2f} "
              f"held-day mean {prim['heldday_mean']:+.4%}")

    pooled_rows = []
    for finalist, by_borrow in pooled.items():
        row = {"finalist": finalist, "borrow": {}}
        for tag, margins in by_borrow.items():
            metrics = rig.fold_metrics(margins)
            metrics["median_margin"] = float(np.median(margins))
            row["borrow"][tag] = metrics
            rig.census_append(
                "shorts-mirror",
                {"finalist": finalist, "market": "pooled-btc-eth",
                 "borrow_bps_day": float(tag)},
                {kk: metrics[kk] for kk in
                 ("robust_ratio", "mean_fold", "pos_frac")})
        pooled_rows.append(row)
        prim = row["borrow"][f"{BORROW_PRIMARY:g}"]
        print(f"POOLED {finalist:<10} | margin-vs-null rr "
              f"{prim['robust_ratio']:+.3f} mean {prim['mean_fold']:+.2%} "
              f"median {prim['median_margin']:+.2%} "
              f"pos {prim['pos_frac']:.2f} worst {prim['worst_fold']:+.2%}")

    summary = {"family": "mirror", "runs": runs, "pooled": pooled_rows,
               "borrow_primary_bps_day": BORROW_PRIMARY,
               "note": "no port gate — evidence for the mako kill review "
                       "and the timing-vs-beta question; see docstring "
                       "caveats before reading the margin as timing"}
    with open(MIRROR_RESULTS, "w", encoding="utf-8") as fh:
        json.dump(summary, fh, indent=2)
    print(f"-> {MIRROR_RESULTS}")


# ------------------------------------------------------------- family (b)

def _pairs_positions(z, elig_hourly, start_i, w_hours, z_entry):
    """One state-machine pass; costs do not feed back into signals.

    Returns (w_a, w_b, turn, held, n_roundtrips): exposure in force
    during the hour ENDING at i, weight turnover executed at close i,
    and the held mask for borrow accrual.
    """
    n = len(z)
    w_a, w_b = np.zeros(n), np.zeros(n)
    turn = np.zeros(n)
    held = np.zeros(n, dtype=bool)
    cur_a = cur_b = 0.0
    pending = None            # ("enter", sign) | ("exit",)
    entry_i = -1
    max_hold = MAX_HOLD_MULT * w_hours
    trips = 0

    for i in range(start_i, n):
        w_a[i], w_b[i] = cur_a, cur_b
        held[i] = cur_a != 0.0

        if pending is not None:
            if pending[0] == "enter":
                sign = pending[1]           # +1: a rich -> short a, long b
                new_a, new_b = -0.5 * sign, 0.5 * sign
                turn[i] = abs(new_a - cur_a) + abs(new_b - cur_b)
                cur_a, cur_b = new_a, new_b
                entry_i = i
                trips += 1
            else:
                turn[i] = abs(cur_a) + abs(cur_b)
                cur_a = cur_b = 0.0
            pending = None

        zi = z[i]
        if cur_a != 0.0:
            stale = entry_i >= 0 and (i - entry_i) >= max_hold
            if np.isfinite(zi) and (abs(zi) <= Z_EXIT or abs(zi) >= Z_STOP):
                pending = ("exit",)
            elif stale:
                pending = ("exit",)
        else:
            zp = z[i - 1] if i > 0 else np.nan
            if (np.isfinite(zi) and np.isfinite(zp) and elig_hourly[i]
                    and abs(zp) < z_entry <= abs(zi) and abs(zi) < Z_STOP):
                pending = ("enter", 1.0 if zi > 0 else -1.0)

    if cur_a != 0.0:                        # forced flat at end-of-data
        turn[n - 1] += abs(cur_a) + abs(cur_b)
        w_a[n - 1], w_b[n - 1] = cur_a, cur_b
    return w_a, w_b, turn, held, trips


def _fold_rets_from_equity(equity, folds):
    return [_equity_at(equity, end) / _equity_at(equity, start) - 1.0
            for start, end in folds]


def pairs_family():
    frame_1h = rig.load("1h")
    frame_1d = rig.load("1d")
    close_1d, _volume, n_1m = rig.daily_panel(frame_1d)
    elig = rig.eligible(close_1d, n_1m,
                        min_history_days=PAIRS_MIN_HISTORY_DAYS)
    close = frame_1h.pivot(index="ts", columns="pair",
                           values="close").sort_index().ffill()

    spreads = []
    for leg_a, leg_b in PAIRS_SPREADS:
        both = elig[leg_a] & elig[leg_b]
        anchor = both[both].index[0]
        hourly = close.index[close.index >= anchor]
        folds = rig.fold_windows(hourly)
        elig_hourly = both.reindex(close.index.normalize()).fillna(
            False).to_numpy()
        spreads.append({"legs": (leg_a, leg_b), "anchor": anchor,
                        "folds": folds, "elig_hourly": elig_hourly})
        print(f"{leg_a}/{leg_b}: anchor {anchor.date()}, {len(folds)} folds "
              f"({folds[0][0].date()} .. {folds[-1][1].date()})")

    index = close.index
    rets = {p: close[p].pct_change().fillna(0.0).to_numpy()
            for p in {leg for s in PAIRS_SPREADS for leg in s}}

    results, rho_matrix = [], []
    for w_hours, z_entry in itertools.product(GRID_W_HOURS, GRID_Z_ENTRY):
        config = {"W": w_hours, "z_entry": z_entry}
        per_spread, pooled_cells = [], {}
        for spread in spreads:
            leg_a, leg_b = spread["legs"]
            ratio = np.log(close[leg_a] / close[leg_b])
            mu = ratio.rolling(w_hours, min_periods=w_hours).mean()
            sd = ratio.rolling(w_hours, min_periods=w_hours).std()
            z = ((ratio - mu) / sd).to_numpy()
            start_i = index.searchsorted(spread["anchor"])
            w_a, w_b, turn, held, trips = _pairs_positions(
                z, spread["elig_hourly"], start_i, w_hours, z_entry)
            pnl_gross = w_a * rets[leg_a] + w_b * rets[leg_b]
            scored_from = spread["folds"][0][0]
            cells = {}
            for friction, borrow in itertools.product(
                    FRICTIONS, BORROW_BPS_DAY):
                per_side = (rig.FEE_BPS + rig.SLIP_BPS) * friction / 1e4
                borrow_hourly = borrow / 1e4 * 0.5 / 24.0
                factor = ((1.0 + pnl_gross - held * borrow_hourly)
                          * (1.0 - turn * per_side))
                equity = pd.Series(np.cumprod(factor), index=index)
                key = (friction, borrow)
                cells[key] = {
                    "folds": _fold_rets_from_equity(equity, spread["folds"]),
                    "mtm_max_dd": rig.max_drawdown(
                        equity.loc[scored_from:]),
                }
                pooled_cells.setdefault(key, {"folds": [], "dd": []})
                pooled_cells[key]["folds"].extend(cells[key]["folds"])
                pooled_cells[key]["dd"].append(cells[key]["mtm_max_dd"])
            gate_cell = cells[(1.0, BORROW_PRIMARY)]
            per_spread.append({
                "legs": f"{leg_a}/{leg_b}", "n_roundtrips": trips,
                "held_frac": float(held[start_i:].mean()),
                "gate_cell_folds": gate_cell["folds"],
                "gate_cell_mtm_max_dd": gate_cell["mtm_max_dd"],
            })

        row = {**config, "spreads": per_spread, "cells": {}}
        for (friction, borrow), cell in pooled_cells.items():
            metrics = rig.fold_metrics(cell["folds"])
            metrics["mtm_max_dd_worst_spread"] = float(max(cell["dd"]))
            tag = f"{friction:g}x-{borrow:g}bd"
            row["cells"][tag] = metrics
            rig.census_append(
                "shorts-pairs",
                {**config, "friction": friction, "borrow_bps_day": borrow},
                {kk: metrics[kk] for kk in
                 ("robust_ratio", "mean_fold", "pos_frac")})
            if friction == 1.0 and borrow == BORROW_PRIMARY:
                rho_matrix.append(cell["folds"])
        results.append(row)
        gate = row["cells"][f"1x-{BORROW_PRIMARY:g}bd"]
        print(f"W={w_hours:>3} z={z_entry} | active rr "
              f"{gate['robust_ratio']:+.3f} mean {gate['mean_fold']:+.2%} "
              f"pos {gate['pos_frac']:.2f} worst {gate['worst_fold']:+.2%} "
              f"dd {gate['mtm_max_dd_worst_spread']:.1%} | 4x rr "
              f"{row['cells'][f'4x-{BORROW_PRIMARY:g}bd']['robust_ratio']:+.3f}")

    median_rho, _ = rig.rank_stability(np.array(rho_matrix))
    gate_tag = f"1x-{BORROW_PRIMARY:g}bd"
    best = max(results, key=lambda r: r["cells"][gate_tag]["robust_ratio"])
    bm = best["cells"][gate_tag]
    gate_pass = (bm["robust_ratio"] >= GATE_ACTIVE_RR
                 and bm["pos_frac"] >= GATE_POS_FRAC
                 and bm["worst_fold"] >= GATE_WORST_FOLD
                 and bm["mtm_max_dd_worst_spread"] <= GATE_MTM_DD
                 and median_rho > GATE_MEDIAN_RHO)
    summary = {
        "family": "pairs",
        "spread_anchors": {f"{s['legs'][0]}/{s['legs'][1]}":
                           {"anchor": str(s["anchor"].date()),
                            "n_folds": len(s["folds"])} for s in spreads},
        "rank_stability_median_rho": median_rho,
        "rank_stability_n_configs": len(rho_matrix),
        "best_config": best,
        "gates": {"active_rr": GATE_ACTIVE_RR, "pos_frac": GATE_POS_FRAC,
                  "worst_fold": GATE_WORST_FOLD, "mtm_dd": GATE_MTM_DD,
                  "median_rho": GATE_MEDIAN_RHO,
                  "cell": "1x fees + 10 bps/day borrow, pooled spreads"},
        "gate_verdict": "PORT-TO-C" if gate_pass else "KILL",
        "results": results,
    }
    with open(PAIRS_RESULTS, "w", encoding="utf-8") as fh:
        json.dump(summary, fh, indent=2)
    print(f"\nrank-stability median rho: {median_rho:+.3f} "
          f"(gate > {GATE_MEDIAN_RHO}; n={len(rho_matrix)} configs — "
          f"small matrix)")
    print(f"best config: W={best['W']} z_entry={best['z_entry']} | "
          f"rr {bm['robust_ratio']:+.3f} pos {bm['pos_frac']:.2f} "
          f"worst {bm['worst_fold']:+.2%} dd "
          f"{bm['mtm_max_dd_worst_spread']:.1%}")
    print(f"VERDICT: {summary['gate_verdict']}  -> {PAIRS_RESULTS}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--family", required=True,
                        choices=("mirror", "pairs"))
    args = parser.parse_args()
    if args.family == "mirror":
        mirror_family()
    else:
        pairs_family()


if __name__ == "__main__":
    main()
