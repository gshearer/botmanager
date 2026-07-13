#!/usr/bin/env python3
"""wm_score.py — official whenmoon walk-forward scorer (COMPSTART.md §4).

Scores one strategy's fixed-config walk-forward runs, one result dir per
market, and reports BOTH scoring conventions side by side:

  gross  fold returns = windows[].return
         (realized_pnl / start_cash — fees are NOT deducted; this is the
         historical COMPSTART §4 recipe, kept as the comparable baseline)
  net    fold returns = (windows[].final_equity - start_cash) / start_cash
         (fold-end mark-to-market equity, net of fees — WM-RIGOR-1)

Per market and pooled across markets: mean_fold, std_fold (population),
worst_fold, pos_frac, robust_ratio = mean/std, plus trades/mo and the
engine max_drawdown, then the COMPSTART §4 eligibility gates evaluated on
both conventions.

Usage:
    python3 tools/wm_score.py <sweep_dir> [<sweep_dir> ...] [--json]

Each <sweep_dir> is a `whenmoon backtest run` result directory (holding
manifest.json + iterations.jsonl) from a fixed-param --walk-forward run.
Pass one dir per market of the SAME strategy at the SAME economics; the
pooled row concatenates every market's folds (~64 for BTC+ETH).

stdlib only — this is a tool, not a build target.
"""

import argparse
import json
import statistics
import sys

DAYS_PER_MONTH = 30.44          # COMPSTART §4 Step C month convention
TEST_WINDOW_DAYS = 120          # official walk-forward test window
DEFAULT_START_CASH = 10000.0    # WM_MARKET_DEFAULT_STARTING_CASH
DEFAULT_FEE_BPS = 5.0
DEFAULT_SLIP_BPS = 5.0

# COMPSTART §4 eligibility gates (fail any => cannot be a finalist)
GATE_POS_FRAC = 0.75            # pooled: >= 3 of 4 windows net-positive
GATE_WORST_FOLD = -0.10         # pooled: no window loses > 10% of stake
GATE_MAX_DRAWDOWN = 0.30        # per market ceiling


def die(msg):
    sys.stderr.write("wm_score: %s\n" % msg)
    sys.exit(1)


def warn(msg):
    sys.stderr.write("wm_score: warning: %s\n" % msg)


def load_run(d):
    """Read one result dir -> dict with manifest fields + the single
    walk-forward iteration row."""
    try:
        manifest = json.load(open(d + "/manifest.json"))
    except (OSError, ValueError) as e:
        die("%s: cannot read manifest.json (%s)" % (d, e))

    try:
        rows = [json.loads(l) for l in open(d + "/iterations.jsonl")
                if l.strip()]
    except (OSError, ValueError) as e:
        die("%s: cannot read iterations.jsonl (%s)" % (d, e))

    if manifest.get("mode") != "walk":
        die("%s: mode is %r, not a --walk-forward run"
            % (d, manifest.get("mode")))
    if len(rows) != 1:
        die("%s: %d iterations — official scoring runs are one fixed "
            "config (sweeps are not scoreable)" % (d, len(rows)))

    row = rows[0]
    windows = row.get("windows") or []
    if not windows:
        die("%s: no windows[] in the iteration row" % d)
    for w in windows:
        if not w.get("ok", True):
            warn("%s: fold %s has ok=false — included anyway (check it)"
                 % (d, w.get("fold")))

    fixed = manifest.get("fixed_params") or {}
    return {
        "dir": d,
        "strategy": manifest.get("strategy"),
        "market": manifest.get("source_market_id"),
        "params": row.get("params") or {},
        "fee_bps": fixed.get("fee_bps", DEFAULT_FEE_BPS),
        "slip_bps": fixed.get("slip_bps", DEFAULT_SLIP_BPS),
        "start_cash": fixed.get("starting_cash", DEFAULT_START_CASH),
        "windows": windows,
        "metrics": row.get("metrics") or {},
        "n_windows": row.get("n_windows", len(windows)),
    }


def fold_stats(folds):
    """The §4 Step C statistics for one list of per-fold returns."""
    mean = statistics.mean(folds)
    std = statistics.pstdev(folds)
    return {
        "n_folds": len(folds),
        "mean_fold": mean,
        "std_fold": std,
        "worst_fold": min(folds),
        "pos_frac": sum(1 for x in folds if x > 0) / len(folds),
        "robust_ratio": (mean / std) if std else 0.0,
    }


def score_market(run):
    """Both conventions' fold vectors + stats for one market's run."""
    cash = run["start_cash"]
    gross = [w["return"] for w in run["windows"]]
    net = [(w["final_equity"] - cash) / cash for w in run["windows"]]
    months = run["n_windows"] * TEST_WINDOW_DAYS / DAYS_PER_MONTH
    trades = run["metrics"].get("trades", 0)
    return {
        "market": run["market"],
        "dir": run["dir"],
        "folds_gross": gross,
        "folds_net": net,
        "gross": fold_stats(gross),
        "net": fold_stats(net),
        "max_drawdown": run["metrics"].get("max_drawdown"),
        "trades": trades,
        "trades_pm": trades / months if months else 0.0,
        "n_folds": len(gross),
    }


def eval_gates(markets, pooled_stats):
    """COMPSTART §4 gates for one convention ('gross' or 'net').

    profit + drawdown are per-market; consistency + worst-fold are pooled.
    Friction re-runs must re-pass profit + consistency ('friction_repass')."""
    profit = all(m[pooled_stats["kind"]]["mean_fold"] > 0 for m in markets)
    consistency = pooled_stats["pos_frac"] >= GATE_POS_FRAC
    worst = pooled_stats["worst_fold"] >= GATE_WORST_FOLD
    drawdown = all(m["max_drawdown"] is not None
                   and m["max_drawdown"] <= GATE_MAX_DRAWDOWN
                   for m in markets)
    return {
        "profit_each_market": profit,
        "consistency": consistency,
        "worst_fold": worst,
        "drawdown_each_market": drawdown,
        "all_pass": profit and consistency and worst and drawdown,
        "friction_repass": profit and consistency,
    }


def pct(x, signed=True):
    return ("%+.2f%%" if signed else "%.2f%%") % (x * 100.0)


def render_table(result):
    """Aligned human table: one row per market per convention + pooled."""
    out = []
    hdr = ("%-18s %-6s %6s %9s %8s %9s %7s %8s"
           % ("market", "score", "folds", "mean", "std", "worst",
              "pos%", "rr"))
    out.append(hdr)
    out.append("-" * len(hdr))
    rows = [(m["market"], m, m["n_folds"]) for m in result["markets"]]
    rows.append(("POOLED", result["pooled"], result["pooled"]["n_folds"]))
    for name, blk, nf in rows:
        for kind in ("gross", "net"):
            s = blk[kind]
            out.append("%-18s %-6s %6d %9s %8s %9s %6.1f%% %8.4f"
                       % (name, kind, nf, pct(s["mean_fold"]),
                          pct(s["std_fold"], signed=False),
                          pct(s["worst_fold"]),
                          s["pos_frac"] * 100.0, s["robust_ratio"]))
    out.append("")
    for m in result["markets"]:
        out.append("%-18s maxDD=%s  trades=%d  trades/mo=%.2f"
                   % (m["market"], pct(m["max_drawdown"], signed=False),
                      m["trades"], m["trades_pm"]))
    out.append("%-18s trades/mo=%.2f (secondary)"
               % ("POOLED", result["pooled"]["trades_pm"]))
    out.append("")
    for kind in ("gross", "net"):
        g = result["gates"][kind]
        flags = "  ".join("%s=%s" % (k, "PASS" if v else "FAIL")
                          for k, v in g.items()
                          if k not in ("all_pass", "friction_repass"))
        out.append("gates[%-5s] %s => %s" % (kind, flags,
                   "ELIGIBLE" if g["all_pass"] else "NOT ELIGIBLE"))
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(
        description="Score whenmoon fixed-config walk-forward runs "
                    "(COMPSTART.md §4) — gross AND net-of-fee.")
    ap.add_argument("dirs", nargs="+", metavar="sweep_dir",
                    help="backtest result dir (one per market, "
                         "same strategy + economics)")
    ap.add_argument("--json", action="store_true",
                    help="emit machine-readable JSON instead of the table")
    args = ap.parse_args()

    runs = [load_run(d) for d in args.dirs]

    strategies = sorted({r["strategy"] for r in runs})
    if len(strategies) != 1:
        die("dirs mix strategies %s — score one strategy at a time"
            % strategies)
    econ = sorted({(r["fee_bps"], r["slip_bps"], r["start_cash"])
                   for r in runs})
    if len(econ) != 1:
        die("dirs mix economics (fee/slip/cash) %s — pool runs from one "
            "friction level only" % econ)
    fee_bps, slip_bps, start_cash = econ[0]

    markets = [score_market(r) for r in runs]

    pooled = {"n_folds": sum(m["n_folds"] for m in markets),
              "trades_pm": sum(m["trades_pm"] for m in markets)}
    for kind in ("gross", "net"):
        allf = [x for m in markets for x in m["folds_" + kind]]
        pooled[kind] = fold_stats(allf)

    gates = {}
    for kind in ("gross", "net"):
        ps = dict(pooled[kind])
        ps["kind"] = kind
        gates[kind] = eval_gates(markets, ps)

    result = {
        "strategy": strategies[0],
        "fee_bps": fee_bps,
        "slip_bps": slip_bps,
        "start_cash": start_cash,
        "params": runs[0]["params"],
        "markets": markets,
        "pooled": pooled,
        "gates": gates,
    }

    if args.json:
        # raw fold vectors are working state, not score — keep JSON lean
        for m in result["markets"]:
            m.pop("folds_gross", None)
            m.pop("folds_net", None)
        print(json.dumps(result, indent=2))
    else:
        print("strategy: %s   fee/slip: %g/%g bps   start_cash: $%g"
              % (result["strategy"], fee_bps, slip_bps, start_cash))
        print("params:   %s" % " ".join(
            "%s=%g" % (k, v) for k, v in result["params"].items()))
        print()
        print(render_table(result))


if __name__ == "__main__":
    main()
