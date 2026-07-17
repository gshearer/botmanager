#!/usr/bin/env python3
"""wm_score.py — official whenmoon walk-forward scorer (COMPSTART.md §4).

Scores one strategy's fixed-config walk-forward runs, one result dir per
market, and reports BOTH scoring conventions side by side:

  gross  fold returns = windows[].return
         (realized_pnl / start_cash — fees are NOT deducted; this is the
         historical COMPSTART §4 recipe, kept as the comparable baseline)
  net    fold returns = (windows[].final_equity - start_cash) / start_cash
         (fold-end mark-to-market equity, net of fees — WM-RIGOR-1)
  active fold returns = net - size_frac * windows[].bench_return
         (WM-RIGOR-2: net minus the deployment-matched buy-and-hold of
         the same asset over the same fold — a strategy deploying only
         size_frac of the book is measured against parking that same
         fraction in the asset; WM-RIGOR-4's equity.jsonl provides the
         daily series a future time-weighted-exposure refinement needs)
  act_fx fold returns = net - windows[].bench_return
         (full-exposure active return — the hard bound; reported, not
         gated)

Per market and pooled across markets: mean_fold, std_fold (population),
worst_fold, pos_frac, robust_ratio = mean/std, plus trades/mo and the
drawdown, then the COMPSTART §4 eligibility gates evaluated on
gross, net, AND active (active gates need windows[].bench_return, i.e. a
run from a WM-RIGOR-2 binary; older runs score gross/net only). The DD
gate prefers metrics.mtm_max_dd (WM-RIGOR-4 daily mark-to-market) over
the per-fill engine max_drawdown, which only observes fill days; the
table prints both so the flattery gap is visible.

Usage:
    python3 tools/wm_score.py <sweep_dir> [<sweep_dir> ...] [--json]
    python3 tools/wm_score.py --overfit <sweep_dir> [...] [--json]
                              [--report-root DIR]

Each <sweep_dir> is a `whenmoon backtest run` result directory (holding
manifest.json + iterations.jsonl) from a fixed-param --walk-forward run.
Pass one dir per market of the SAME strategy at the SAME economics; the
pooled row concatenates every market's folds (~64 for BTC+ETH).

--overfit (WM-RIGOR-3) is a SEPARATE path from official scoring: it
reads ALL iteration rows of a --walk-forward SWEEP (the per-fold top set
is every row carrying windows[]; widen it with the C flag
--perfold-top N), builds the config x fold net-return matrix, and
reports three overfitting statistics:

  rank-stability  200 seeded half-splits of the fold axis; Spearman rho
                  between the config rankings (by mean net) on the two
                  halves. Real ridge: median rho > 0.5.
  PBO-lite (CSCV) same 200 splits; fraction where the in-sample winner
                  ranks in the bottom half out-of-sample. < 0.5 = some
                  skill, << 0.5 = robust.
  trials census   walks <report-root>/*/manifest.json, sums total_iters
                  per strategy -> T, with the expected-max reference
                  E[max] ~ sqrt(2 ln T) (haircut context, NOT a gate).

stdlib only — this is a tool, not a build target.
"""

import argparse
import glob
import json
import math
import os
import random
import statistics
import sys

DAYS_PER_MONTH = 30.44          # COMPSTART §4 Step C month convention
TEST_WINDOW_DAYS = 120          # official walk-forward test window
DEFAULT_START_CASH = 10000.0    # WM_MARKET_DEFAULT_STARTING_CASH
DEFAULT_FEE_BPS = 5.0
DEFAULT_SLIP_BPS = 5.0
DEFAULT_SIZE_FRAC = 0.25        # WM_MARKET_DEFAULT_SIZE_FRAC

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
    metrics = row.get("metrics") or {}

    # WM-RIGOR-2: the engine emits the RESOLVED size_frac it sized with
    # (metrics.size_frac); fixed_params only echoes a CLI override, and
    # the engine default is the last resort for pre-RIGOR-2 runs.
    size_frac = metrics.get("size_frac",
                            fixed.get("size_frac", DEFAULT_SIZE_FRAC))

    return {
        "dir": d,
        "strategy": manifest.get("strategy"),
        "market": manifest.get("source_market_id"),
        "params": row.get("params") or {},
        "fee_bps": fixed.get("fee_bps", DEFAULT_FEE_BPS),
        "slip_bps": fixed.get("slip_bps", DEFAULT_SLIP_BPS),
        "start_cash": fixed.get("starting_cash", DEFAULT_START_CASH),
        "size_frac": size_frac,
        "windows": windows,
        "metrics": metrics,
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
    """Every convention's fold vectors + stats for one market's run."""
    cash = run["start_cash"]
    sf = run["size_frac"]
    gross = [w["return"] for w in run["windows"]]
    net = [(w["final_equity"] - cash) / cash for w in run["windows"]]
    months = run["n_windows"] * TEST_WINDOW_DAYS / DAYS_PER_MONTH
    trades = run["metrics"].get("trades", 0)
    out = {
        "market": run["market"],
        "dir": run["dir"],
        "folds_gross": gross,
        "folds_net": net,
        "gross": fold_stats(gross),
        "net": fold_stats(net),
        # WM-RIGOR-4: the DD gate prefers the daily mark-to-market
        # drawdown when the run carries it — per-fill max_drawdown only
        # observes the book on fill days, flattering wide-exit configs.
        # Older (pre-RIGOR-4) runs fall back to the per-fill number.
        "max_drawdown": (run["metrics"]["mtm_max_dd"]
                         if run["metrics"].get("mtm_max_dd") is not None
                         else run["metrics"].get("max_drawdown")),
        "dd_source": ("mtm"
                      if run["metrics"].get("mtm_max_dd") is not None
                      else "per_fill"),
        "per_fill_max_drawdown": run["metrics"].get("max_drawdown"),
        "daily_sharpe_ann": run["metrics"].get("daily_sharpe_ann"),
        "trades": trades,
        "trades_pm": trades / months if months else 0.0,
        "n_folds": len(gross),
    }

    # WM-RIGOR-2: active (benchmark-relative) folds, only when every
    # window carries a priced bench_return (a WM-RIGOR-2 binary run).
    bench = [w.get("bench_return") for w in run["windows"]]
    if all(b is not None for b in bench):
        active = [n - sf * b for n, b in zip(net, bench)]
        act_fx = [n - b for n, b in zip(net, bench)]
        out.update({
            "folds_active": active,
            "folds_act_fx": act_fx,
            "folds_bench": bench,
            "active": fold_stats(active),
            "act_fx": fold_stats(act_fx),
            "bench": fold_stats(bench),
        })
    else:
        warn("%s: windows[] missing bench_return (pre-WM-RIGOR-2 run) "
             "— active scoring skipped" % run["dir"])
    return out


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


# --------------------------------------------------------------------- #
# WM-RIGOR-3 — overfitting statistics (--overfit; separate path from    #
# official scoring: multi-row loader, never used for eligibility)       #
# --------------------------------------------------------------------- #

OVERFIT_N_SPLITS = 200
OVERFIT_SEED = 42
RHO_RIDGE_MEDIAN = 0.5          # median rho above this = real ridge
PBO_SKILL = 0.5                 # PBO below this = some skill


def load_overfit_matrix(d):
    """Read one SWEEP result dir -> config x fold net-return matrix.

    Keeps every iteration row with a non-empty windows[] (that is the
    per-fold top set — all rows on --perfold-top runs, top_k otherwise).
    Rows whose fold count disagrees with the majority are dropped with a
    warning rather than corrupting the matrix."""
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
        die("%s: mode is %r — overfit stats need a --walk-forward sweep"
            % (d, manifest.get("mode")))

    fixed = manifest.get("fixed_params") or {}
    cash = fixed.get("starting_cash", DEFAULT_START_CASH)

    folded = [r for r in rows if r.get("windows")]
    if not folded:
        die("%s: no rows carry windows[] — re-run the sweep with "
            "--perfold-top N" % d)

    n_folds = statistics.mode(len(r["windows"]) for r in folded)
    matrix, configs = [], []
    for r in folded:
        if len(r["windows"]) != n_folds:
            warn("%s: iter %s has %d folds (expected %d) — dropped"
                 % (d, r.get("iter"), len(r["windows"]), n_folds))
            continue
        matrix.append([(w["final_equity"] - cash) / cash
                       for w in r["windows"]])
        configs.append(r.get("params") or {})

    return {
        "dir": d,
        "strategy": manifest.get("strategy"),
        "market": manifest.get("source_market_id"),
        "total_iters": manifest.get("total_iters"),
        "perfold_top": manifest.get("perfold_top"),
        "start_cash": cash,
        "n_configs": len(matrix),
        "n_folds": n_folds,
        "matrix": matrix,
        "configs": configs,
    }


def avg_ranks(values):
    """1-based average ranks, descending (best value = rank 1); tied
    values share the mean of the positions they span."""
    order = sorted(range(len(values)), key=lambda i: -values[i])
    ranks = [0.0] * len(values)
    i = 0
    while i < len(order):
        j = i
        while (j + 1 < len(order)
               and values[order[j + 1]] == values[order[i]]):
            j += 1
        for k in range(i, j + 1):
            ranks[order[k]] = (i + j) / 2.0 + 1.0
        i = j + 1
    return ranks


def pearson(x, y):
    n = len(x)
    mx, my = sum(x) / n, sum(y) / n
    sxy = sum((a - mx) * (b - my) for a, b in zip(x, y))
    sxx = sum((a - mx) ** 2 for a in x)
    syy = sum((b - my) ** 2 for b in y)
    if sxx == 0.0 or syy == 0.0:
        return 0.0              # a constant ranking carries no signal
    return sxy / math.sqrt(sxx * syy)


def spearman(x, y):
    return pearson(avg_ranks(x), avg_ranks(y))


def overfit_stats(mat):
    """Rank-stability + PBO-lite over one config x fold matrix, sharing
    the same seeded half-splits of the fold axis (CSCV shape)."""
    n_cfg, n_folds = mat["n_configs"], mat["n_folds"]
    if n_cfg < 2:
        return {"skipped": "need >= 2 configs with windows[] (got %d) "
                           "— run the sweep with --perfold-top N"
                           % n_cfg}
    if n_folds < 4:
        return {"skipped": "need >= 4 folds for half-splits (got %d)"
                           % n_folds}

    rng = random.Random(OVERFIT_SEED)
    idx = list(range(n_folds))
    half = n_folds // 2
    rhos, pbo_hits = [], 0
    for _ in range(OVERFIT_N_SPLITS):
        rng.shuffle(idx)
        a, b = idx[:half], idx[half:]
        mean_a = [sum(row[f] for f in a) / len(a) for row in mat["matrix"]]
        mean_b = [sum(row[f] for f in b) / len(b) for row in mat["matrix"]]
        rhos.append(spearman(mean_a, mean_b))

        # PBO-lite: does half-A's winner rank in half-B's bottom half?
        winner = max(range(n_cfg), key=lambda c: mean_a[c])
        oos_rank = sum(1 for c in range(n_cfg)
                       if mean_b[c] > mean_b[winner])
        if oos_rank >= n_cfg / 2.0:
            pbo_hits += 1

    q1, med, q3 = statistics.quantiles(rhos, n=4)
    pbo = pbo_hits / OVERFIT_N_SPLITS
    return {
        "n_splits": OVERFIT_N_SPLITS,
        "seed": OVERFIT_SEED,
        "rho_median": med,
        "rho_q1": q1,
        "rho_q3": q3,
        "rho_iqr": q3 - q1,
        "ridge": med > RHO_RIDGE_MEDIAN,
        "pbo": pbo,
        "pbo_skill": pbo < PBO_SKILL,
    }


def trials_census(roots):
    """Sum total_iters per strategy across every result dir under the
    given report roots — every config ever tried counts as a trial."""
    by_strat = {}
    for root in roots:
        for mf in sorted(glob.glob(os.path.join(root, "*",
                                                "manifest.json"))):
            try:
                m = json.load(open(mf))
            except (OSError, ValueError) as e:
                warn("%s: unreadable manifest (%s) — skipped" % (mf, e))
                continue
            s = m.get("strategy") or "?"
            ent = by_strat.setdefault(s, {"runs": 0, "trials": 0})
            ent["runs"] += 1
            ent["trials"] += int(m.get("total_iters") or 0)

    for ent in by_strat.values():
        t = ent["trials"]
        ent["e_max"] = math.sqrt(2.0 * math.log(t)) if t > 1 else 0.0
    return by_strat


def render_overfit(blocks, census, roots):
    out = []
    for blk in blocks:
        mat, st = blk["run"], blk["stats"]
        out.append("== overfit: %s ==" % mat["dir"])
        out.append("strategy: %s   market: %s   configs(with folds): "
                   "%d/%s   folds: %d"
                   % (mat["strategy"], mat["market"], mat["n_configs"],
                      mat["total_iters"], mat["n_folds"]))
        if "skipped" in st:
            out.append("  skipped: %s" % st["skipped"])
            out.append("")
            continue
        out.append("  rank-stability (%d half-splits, seed %d): "
                   "median rho=%.3f  IQR=[%.3f, %.3f]"
                   % (st["n_splits"], st["seed"], st["rho_median"],
                      st["rho_q1"], st["rho_q3"]))
        out.append("    verdict: %s (median %s %.1f)"
                   % ("REAL RIDGE" if st["ridge"] else "UNSTABLE",
                      ">" if st["ridge"] else "<=", RHO_RIDGE_MEDIAN))
        out.append("  PBO-lite (CSCV): %.3f"
                   % st["pbo"])
        out.append("    verdict: %s (< %.1f = some skill, << %.1f = "
                   "robust)"
                   % ("SKILL" if st["pbo_skill"] else "OVERFIT",
                      PBO_SKILL, PBO_SKILL))
        out.append("")

    out.append("== trials census: %s ==" % ", ".join(roots))
    hdr = ("%-14s %6s %14s %18s"
           % ("strategy", "runs", "T(trials)", "E[max]~sqrt(2lnT)"))
    out.append(hdr)
    out.append("-" * len(hdr))
    for s in sorted(census):
        ent = census[s]
        out.append("%-14s %6d %14d %18.2f"
                   % (s, ent["runs"], ent["trials"], ent["e_max"]))
    out.append("")
    out.append("E[max] is the expected best Sharpe-like score of T "
               "independent zero-skill trials (in std units) — a "
               "haircut reference beside rr, not a gate.")
    return "\n".join(out)


def overfit_main(args):
    runs = [load_overfit_matrix(d) for d in args.dirs]
    blocks = [{"run": r, "stats": overfit_stats(r)} for r in runs]

    if args.report_root:
        roots = [args.report_root]
    else:
        roots = sorted({os.path.dirname(os.path.abspath(d))
                        for d in args.dirs})
    census = trials_census(roots)

    if args.json:
        payload = {
            "overfit": [dict(stats=b["stats"],
                             **{k: v for k, v in b["run"].items()
                                if k not in ("matrix", "configs")})
                        for b in blocks],
            "census": census,
            "census_roots": roots,
        }
        print(json.dumps(payload, indent=2))
    else:
        print(render_overfit(blocks, census, roots))


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
        for kind in result["kinds"]:
            s = blk[kind]
            out.append("%-18s %-6s %6d %9s %8s %9s %6.1f%% %8.4f"
                       % (name, kind, nf, pct(s["mean_fold"]),
                          pct(s["std_fold"], signed=False),
                          pct(s["worst_fold"]),
                          s["pos_frac"] * 100.0, s["robust_ratio"]))
    out.append("")
    for m in result["markets"]:
        bench = ("  hold/fold=%s" % pct(m["bench"]["mean_fold"])
                 if "bench" in m else "")
        # WM-RIGOR-4: gate DD is MTM when available; show the per-fill
        # number beside it so the flattery gap is visible at a glance.
        if m["dd_source"] == "mtm" and m["per_fill_max_drawdown"] is not None:
            dd = ("mtmDD=%s fillDD=%s"
                  % (pct(m["max_drawdown"], signed=False),
                     pct(m["per_fill_max_drawdown"], signed=False)))
        else:
            dd = "maxDD=%s(per-fill)" % pct(m["max_drawdown"], signed=False)
        out.append("%-18s %s  trades=%d  trades/mo=%.2f%s"
                   % (m["market"], dd,
                      m["trades"], m["trades_pm"], bench))
    out.append("%-18s trades/mo=%.2f (secondary)"
               % ("POOLED", result["pooled"]["trades_pm"]))
    out.append("")
    for kind in result["kinds"]:
        if kind not in result["gates"]:
            out.append("gates[%-6s] (reported hard bound — not gated)"
                       % kind)
            continue
        g = result["gates"][kind]
        flags = "  ".join("%s=%s" % (k, "PASS" if v else "FAIL")
                          for k, v in g.items()
                          if k not in ("all_pass", "friction_repass"))
        out.append("gates[%-6s] %s => %s" % (kind, flags,
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
    ap.add_argument("--overfit", action="store_true",
                    help="WM-RIGOR-3: rank-stability + PBO + trials "
                         "census over a --walk-forward SWEEP dir "
                         "(reads all rows; not official scoring)")
    ap.add_argument("--report-root", metavar="DIR",
                    help="result-dir root for the trials census "
                         "(default: parent dir(s) of the sweep dirs)")
    args = ap.parse_args()

    if args.overfit:
        overfit_main(args)
        return

    runs = [load_run(d) for d in args.dirs]

    strategies = sorted({r["strategy"] for r in runs})
    if len(strategies) != 1:
        die("dirs mix strategies %s — score one strategy at a time"
            % strategies)
    econ = sorted({(r["fee_bps"], r["slip_bps"], r["start_cash"],
                    r["size_frac"]) for r in runs})
    if len(econ) != 1:
        die("dirs mix economics (fee/slip/cash/size_frac) %s — pool runs "
            "from one friction level only" % econ)
    fee_bps, slip_bps, start_cash, size_frac = econ[0]

    markets = [score_market(r) for r in runs]

    # Active conventions pool only when EVERY market priced its bench;
    # a mixed pool would compare active folds against net-only folds.
    have_active = all("active" in m for m in markets)
    kinds = ["gross", "net"] + (["active", "act_fx"] if have_active
                                else [])

    pooled = {"n_folds": sum(m["n_folds"] for m in markets),
              "trades_pm": sum(m["trades_pm"] for m in markets)}
    for kind in kinds:
        allf = [x for m in markets for x in m["folds_" + kind]]
        pooled[kind] = fold_stats(allf)
    if have_active:
        pooled["bench"] = fold_stats(
            [b for m in markets for b in m["folds_bench"]])

    gates = {}
    for kind in kinds:
        if kind == "act_fx":
            continue                 # reported hard bound — not gated
        ps = dict(pooled[kind])
        ps["kind"] = kind
        gates[kind] = eval_gates(markets, ps)

    result = {
        "strategy": strategies[0],
        "fee_bps": fee_bps,
        "slip_bps": slip_bps,
        "start_cash": start_cash,
        "size_frac": size_frac,
        "params": runs[0]["params"],
        "kinds": kinds,
        "markets": markets,
        "pooled": pooled,
        "gates": gates,
    }

    if args.json:
        # raw fold vectors are working state, not score — keep JSON lean
        for m in result["markets"]:
            for kind in kinds + ["bench"]:
                m.pop("folds_" + kind, None)
        print(json.dumps(result, indent=2))
    else:
        print("strategy: %s   fee/slip: %g/%g bps   start_cash: $%g   "
              "size_frac: %g"
              % (result["strategy"], fee_bps, slip_bps, start_cash,
                 size_frac))
        print("params:   %s" % " ".join(
            "%s=%g" % (k, v) for k, v in result["params"].items()))
        print()
        print(render_table(result))


if __name__ == "__main__":
    main()
