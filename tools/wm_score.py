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
    python3 tools/wm_score.py --portfolio <sweep_dir> [...] [--json]

--portfolio (WM-EDGE-3) scores a BOOK: each dir is one leg (one
strategy on one market — one live trial session), mixed strategies
expected. Fold windows are calendar-aligned (exact within a market,
index-from-end across markets with the skew disclosed), then the
portfolio fold return is sum(w_i * net_i) under three pre-committed
weightings — equal, inverse-vol, correlation-clustered (pairwise
rho > 0.7 shares one slot) — with the fold-return correlation matrix,
the diversification delta vs the best solo leg on the SAME folds, the
joint daily MTM book (WM-RIGOR-4 equity.jsonl: joint DD, ann vol,
vol-target multipliers), and a suggested per-session quote_alloc_frac
vector. Report-only: the COMPSTART gates stay per-strategy.

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
import datetime
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
        "fill_mode": fixed.get("fill_mode", "close"),
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


# --------------------------------------------------------------------- #
# WM-EDGE-3 — portfolio layer (--portfolio; scores a BOOK of legs, one  #
# result dir per leg = one strategy on one market — exactly one live    #
# trial session. Weights map to production via per-instance             #
# quote_alloc_frac (WM-QUOTE-ALLOC-1) with zero engine changes.)        #
# --------------------------------------------------------------------- #

PORT_RHO_CLUSTER = 0.7          # pairwise rho above this = one shared slot
PORT_SKEW_TOL_DAYS = 20.0       # max tolerated cross-market fold-end skew
PORT_VOL_TARGETS = (0.05, 0.10, 0.15)   # annualized; exposure table only
DAY_MS = 86400000


def leg_name(run):
    """Short leg label: strategy:market with the exchange prefix and
    quote suffix stripped when the market follows <exch>-<base>-<quote>."""
    parts = run["market"].split("-")
    mkt = parts[1] if len(parts) == 3 else run["market"]
    return "%s:%s" % (run["strategy"], mkt)


def port_align(runs):
    """Calendar-align fold windows across legs.

    Within one market every leg must share the identical window list
    (same walk spec) — anything else is a die, not a warning. Across
    markets the walks are NOT on one grid: each corpus anchors its own
    walk (ETH is listing-anchored, WM-RIGOR-2) and keeps a ragged tail
    window, so grids sit a constant ~13d apart while every walk ends at
    its corpus range_end (minutes apart). Pair folds by INDEX FROM THE
    END: the k-th-from-last folds cover ~the same 120d of calendar, with
    a constant skew equal to the tail-length difference. The skew is
    measured and disclosed; above PORT_SKEW_TOL_DAYS the pairing is a
    lie and the tool refuses.

    Returns (n_folds, offsets) where offsets[i] is leg i's window index
    for aligned fold 0 (chronological; aligned fold j uses window
    offsets[i]+j) plus the skew report dict."""
    by_market = {}
    for i, r in enumerate(runs):
        by_market.setdefault(r["market"], []).append(i)

    for mkt, idxs in by_market.items():
        ref = [(w["start_ts_ms"], w["end_ts_ms"])
               for w in runs[idxs[0]]["windows"]]
        for i in idxs[1:]:
            got = [(w["start_ts_ms"], w["end_ts_ms"])
                   for w in runs[i]["windows"]]
            if got != ref:
                die("%s and %s disagree on %s's fold windows — same-market "
                    "legs must come from one walk spec"
                    % (runs[idxs[0]]["dir"], runs[i]["dir"], mkt))

    n_folds = min(len(r["windows"]) for r in runs)
    offsets = [len(r["windows"]) - n_folds for r in runs]

    max_skew = 0.0
    for j in range(n_folds):
        ends = [r["windows"][offsets[i] + j]["end_ts_ms"]
                for i, r in enumerate(runs)]
        skew = (max(ends) - min(ends)) / float(DAY_MS)
        max_skew = max(max_skew, skew)
    if max_skew > PORT_SKEW_TOL_DAYS:
        die("cross-market fold skew %.1fd exceeds %.1fd — these walks "
            "cannot be calendar-paired" % (max_skew, PORT_SKEW_TOL_DAYS))

    dropped = {m: len(runs[idxs[0]]["windows"]) - n_folds
               for m, idxs in by_market.items()
               if len(runs[idxs[0]]["windows"]) > n_folds}
    return n_folds, offsets, {"max_skew_days": max_skew,
                              "dropped_early_folds": dropped}


def port_leg_folds(run, offset, n_folds):
    """One leg's aligned chronological net + active fold vectors."""
    cash, sf = run["start_cash"], run["size_frac"]
    ws = run["windows"][offset:offset + n_folds]
    net = [(w["final_equity"] - cash) / cash for w in ws]
    bench = [w.get("bench_return") for w in ws]
    active = ([n - sf * b for n, b in zip(net, bench)]
              if all(b is not None for b in bench) else None)
    return net, active


def union_find_clusters(n, edges):
    """Connected components over n nodes; edges = iterable of (i, j)."""
    parent = list(range(n))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for i, j in edges:
        parent[find(i)] = find(j)
    comps = {}
    for i in range(n):
        comps.setdefault(find(i), []).append(i)
    return sorted(comps.values(), key=lambda c: c[0])


def port_weight_schemes(names, folds_net, corr):
    """The three pre-committed weightings (chunk spec, fixed before any
    result was seen): equal, inverse-vol, and correlation-clustered —
    legs with pairwise rho > PORT_RHO_CLUSTER are one bet and share one
    slot's weight."""
    n = len(names)
    schemes = {"equal": [1.0 / n] * n}

    inv = []
    for f in folds_net:
        sd = statistics.pstdev(f)
        if sd == 0.0:
            die("leg with zero fold-return vol — inverse-vol undefined")
        inv.append(1.0 / sd)
    tot = sum(inv)
    schemes["inv_vol"] = [x / tot for x in inv]

    edges = [(i, j) for i in range(n) for j in range(i + 1, n)
             if corr[i][j] > PORT_RHO_CLUSTER]
    clusters = union_find_clusters(n, edges)
    w = [0.0] * n
    for c in clusters:
        for i in c:
            w[i] = 1.0 / (len(clusters) * len(c))
    schemes["cluster"] = w
    return schemes, [[names[i] for i in c] for c in clusters]


def port_combine(weights, leg_folds):
    """Portfolio fold vector: return_j = sum_i w_i * leg_i[j]."""
    n_folds = len(leg_folds[0])
    return [sum(w * f[j] for w, f in zip(weights, leg_folds))
            for j in range(n_folds)]


def port_load_equity(run):
    """Daily MTM series from the result dir (WM-RIGOR-4), or None for a
    pre-RIGOR-4 dir. The series is the CONTINUOUS compounded book across
    the whole walk (windows[].final_equity is per-fold re-based; this is
    not) — normalize by start_cash to an index."""
    path = run["dir"] + "/equity.jsonl"
    if not os.path.exists(path):
        warn("%s: no equity.jsonl (pre-WM-RIGOR-4 run) — daily portfolio "
             "skipped" % run["dir"])
        return None
    try:
        rows = [json.loads(l) for l in open(path) if l.strip()]
    except (OSError, ValueError) as e:
        die("%s: cannot read equity.jsonl (%s)" % (path, e))
    return {r["ts_ms"] // DAY_MS: r["equity"] / run["start_cash"]
            for r in rows}


def max_drawdown_series(values):
    peak, dd = values[0], 0.0
    for v in values:
        peak = max(peak, v)
        dd = max(dd, (peak - v) / peak)
    return dd


def port_daily(runs, schemes):
    """Stage 2 — joint daily book on the common calendar span: weighted
    sum of per-leg equity indexes (each re-based to 1.0 at span start),
    joint MTM max DD, annualized vol + daily Sharpe (mean/std x
    sqrt(365), rf=0, population std — backtest.c:736 convention), and
    the exposure multiplier that would hit each PORT_VOL_TARGETS."""
    series = [port_load_equity(r) for r in runs]
    if any(s is None for s in series):
        return None

    lo = max(min(s) for s in series)
    hi = min(max(s) for s in series)
    if hi - lo < 30:
        die("daily series share fewer than 30 common days — no joint book")

    filled = []
    for s in series:
        vals, last = {}, None
        for d in range(min(s), hi + 1):    # roll forward from the leg's
            last = s.get(d, last)          # own start so a missing mark
            if d >= lo:                    # ON day lo still fills
                vals[d] = last
        base = vals[lo]
        filled.append([vals[d] / base for d in range(lo, hi + 1)])

    out = {"span_start_day": lo, "span_end_day": hi,
           "n_days": hi - lo + 1, "schemes": {}}
    for name, w in schemes.items():
        idx = [sum(wi * f[d] for wi, f in zip(w, filled))
               for d in range(hi - lo + 1)]
        rets = [idx[d] / idx[d - 1] - 1.0 for d in range(1, len(idx))]
        mean, sd = statistics.mean(rets), statistics.pstdev(rets)
        ann_vol = sd * math.sqrt(365.0)
        out["schemes"][name] = {
            "final_index": idx[-1],
            "joint_mtm_max_dd": max_drawdown_series(idx),
            "ann_vol": ann_vol,
            "daily_sharpe_ann": (mean / sd * math.sqrt(365.0)) if sd else 0.0,
            "vol_target_mult": {("%.0f%%" % (t * 100.0)):
                                (t / ann_vol if ann_vol else 0.0)
                                for t in PORT_VOL_TARGETS},
        }
    return out


def render_portfolio(res):
    out = []
    out.append("== portfolio: %d legs   fill=%s   fee/slip: %g/%g bps   "
               "start_cash: $%g =="
               % (len(res["legs"]), res["fill_mode"], res["fee_bps"],
                  res["slip_bps"], res["start_cash"]))
    out.append("legs: %s" % "  ".join(res["legs"]))
    al = res["alignment"]
    drop = ("; dropped earliest folds: " + ", ".join(
            "%s -%d" % (m, k) for m, k in al["dropped_early_folds"].items())
            if al["dropped_early_folds"] else "")
    out.append("aligned folds: %d (paired index-from-end; max cross-market "
               "end skew %.1fd%s)"
               % (res["n_folds"], al["max_skew_days"], drop))
    out.append("")

    names = res["legs"]
    wid = max(len(n) for n in names)
    out.append("correlation (net fold returns, aligned):")
    out.append("  %-*s %s" % (wid, "",
               " ".join("%*s" % (wid, n) for n in names)))
    for i, row in enumerate(res["corr_net"]):
        out.append("  %-*s %s" % (wid, names[i],
                   " ".join("%*.2f" % (wid, v) for v in row)))
    out.append("  mean pairwise rho: %.3f" % res["mean_pairwise_rho"])
    out.append("")

    out.append("weights (rho>%.1f clusters: %s):"
               % (PORT_RHO_CLUSTER,
                  "  ".join("{%s}" % ",".join(c) for c in res["clusters"])))
    for scheme in res["scheme_order"]:
        out.append("  %-8s %s" % (scheme,
                   " ".join("%*.3f" % (wid, w)
                            for w in res["weights"][scheme])))
    out.append("")

    out.append("per-leg solo on the aligned folds (full-run mtmDD beside):")
    for i, name in enumerate(names):
        cols = []
        for kind in res["kinds"]:
            s = res["leg_stats"][kind][i]
            cols.append("%s rr=%7.4f worst=%s" % (kind, s["robust_ratio"],
                        pct(s["worst_fold"])))
        dd = res["leg_mtm_max_dd"].get(name)
        out.append("  %-*s %s  mtmDD=%s" % (wid, name, "   ".join(cols),
                   pct(dd, signed=False) if dd is not None else "n/a"))
    out.append("")

    hdr = ("%-8s %-6s %9s %8s %9s %7s %8s %10s %11s"
           % ("scheme", "score", "mean", "std", "worst", "pos%", "rr",
              "solo-rr", "div-delta"))
    out.append(hdr)
    out.append("-" * len(hdr))
    for scheme in res["scheme_order"]:
        blk = res["portfolio"][scheme]
        for kind in res["kinds"]:
            s = blk[kind]
            solo = res["best_solo"][kind]
            out.append("%-8s %-6s %9s %8s %9s %6.1f%% %8.4f %10.4f %+11.4f"
                       % (scheme, kind, pct(s["mean_fold"]),
                          pct(s["std_fold"], signed=False),
                          pct(s["worst_fold"]), s["pos_frac"] * 100.0,
                          s["robust_ratio"], solo["robust_ratio"],
                          s["robust_ratio"] - solo["robust_ratio"]))
    for kind in res["kinds"]:
        out.append("best solo [%-6s] = %s" % (kind,
                   res["best_solo"][kind]["leg"]))
    out.append("(report-only — the COMPSTART gates stay per-strategy; "
               "div-delta = portfolio rr - best solo rr on the SAME "
               "aligned folds)")
    out.append("")

    d = res.get("daily")
    if d:
        f = lambda day: datetime.datetime.fromtimestamp(
            day * 86400, datetime.timezone.utc).date().isoformat()
        out.append("daily joint book (WM-RIGOR-4 series, common span "
                   "%s..%s, %d days):"
                   % (f(d["span_start_day"]), f(d["span_end_day"]),
                      d["n_days"]))
        for scheme in res["scheme_order"]:
            s = d["schemes"][scheme]
            mult = "  ".join("%s->x%.2f" % (t, m)
                             for t, m in s["vol_target_mult"].items())
            out.append("  %-8s final=x%-8.2f jointMTMDD=%s  annvol=%s  "
                       "dailySharpe=%.2f  vol-target %s"
                       % (scheme, s["final_index"],
                          pct(s["joint_mtm_max_dd"], signed=False),
                          pct(s["ann_vol"], signed=False),
                          s["daily_sharpe_ann"], mult))
        out.append("")

    out.append("suggested quote_alloc_frac (cluster weights — correlated "
               "legs share one slot; scale all by a vol-target multiplier "
               "to cap book vol):")
    for name, w, sess in res["alloc_vector"]:
        out.append("  %-34s %.3f" % (sess, w))
    for strat, tot in res["alloc_by_strategy"].items():
        out.append("  strategy %-12s total %.3f" % (strat, tot))
    return "\n".join(out)


def portfolio_main(args):
    runs = [load_run(d) for d in args.dirs]
    if len(runs) < 2:
        die("--portfolio needs at least 2 legs")

    econ = sorted({(r["fee_bps"], r["slip_bps"], r["start_cash"])
                   for r in runs})
    if len(econ) != 1:
        die("legs mix economics (fee/slip/cash) %s — one friction level "
            "per book" % econ)
    fills = sorted({r["fill_mode"] for r in runs})
    if len(fills) != 1:
        die("legs mix fill modes %s — a book fills one way" % fills)
    if fills[0] != "next-open":
        warn("fill=%s — promotions read next-open numbers (WM-RIGOR-5); "
             "treat this book as a comparison, not the official one"
             % fills[0])

    names = [leg_name(r) for r in runs]
    if len(set(names)) != len(names):
        die("duplicate legs %s — one result dir per (strategy, market)"
            % names)

    n_folds, offsets, align = port_align(runs)
    folds_net, folds_active = [], []
    for r, off in zip(runs, offsets):
        net, active = port_leg_folds(r, off, n_folds)
        folds_net.append(net)
        folds_active.append(active)
    have_active = all(a is not None for a in folds_active)
    if not have_active:
        warn("some legs lack bench_return — active portfolio skipped")

    corr = [[pearson(a, b) for b in folds_net] for a in folds_net]
    pairs = [corr[i][j] for i in range(len(runs))
             for j in range(i + 1, len(runs))]

    schemes, clusters = port_weight_schemes(names, folds_net, corr)
    scheme_order = ["equal", "inv_vol", "cluster"]
    kinds = ["net"] + (["active"] if have_active else [])

    portfolio, best_solo, leg_stats = {}, {}, {}
    for kind, legs in (("net", folds_net), ("active", folds_active)):
        if kind not in kinds:
            continue
        soli = [dict(fold_stats(f), leg=names[i])
                for i, f in enumerate(legs)]
        top = max(range(len(runs)), key=lambda i: soli[i]["robust_ratio"])
        best_solo[kind] = soli[top]
        leg_stats[kind] = soli
        for scheme in scheme_order:
            blk = portfolio.setdefault(scheme, {})
            blk[kind] = fold_stats(port_combine(schemes[scheme], legs))

    daily = port_daily(runs, schemes)

    alloc = [(names[i], schemes["cluster"][i],
              "%s@%s" % (runs[i]["market"], runs[i]["strategy"]))
             for i in range(len(runs))]
    by_strat = {}
    for i, r in enumerate(runs):
        by_strat[r["strategy"]] = (by_strat.get(r["strategy"], 0.0)
                                   + schemes["cluster"][i])

    res = {
        "legs": names,
        "dirs": [r["dir"] for r in runs],
        "fill_mode": fills[0],
        "fee_bps": econ[0][0], "slip_bps": econ[0][1],
        "start_cash": econ[0][2],
        "n_folds": n_folds,
        "alignment": align,
        "corr_net": corr,
        "mean_pairwise_rho": statistics.mean(pairs),
        "clusters": clusters,
        "rho_cluster": PORT_RHO_CLUSTER,
        "scheme_order": scheme_order,
        "weights": schemes,
        "kinds": kinds,
        "portfolio": portfolio,
        "best_solo": best_solo,
        "leg_stats": leg_stats,
        "leg_mtm_max_dd": {names[i]: runs[i]["metrics"].get("mtm_max_dd")
                           for i in range(len(runs))},
        "daily": daily,
        "alloc_vector": alloc,
        "alloc_by_strategy": by_strat,
    }

    if args.json:
        payload = dict(res)
        payload["alloc_vector"] = [
            {"leg": n, "session": s, "quote_alloc_frac": w}
            for n, w, s in alloc]
        print(json.dumps(payload, indent=2))
    else:
        print(render_portfolio(res))


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
    ap.add_argument("--portfolio", action="store_true",
                    help="WM-EDGE-3: score the dirs as one BOOK (one "
                         "leg per dir, mixed strategies OK) — aligned "
                         "folds, correlation matrix, three weightings, "
                         "div-delta vs best solo, joint daily MTM book, "
                         "suggested quote_alloc_frac vector")
    ap.add_argument("--report-root", metavar="DIR",
                    help="result-dir root for the trials census "
                         "(default: parent dir(s) of the sweep dirs)")
    args = ap.parse_args()

    if args.overfit and args.portfolio:
        die("--overfit and --portfolio are separate paths — pick one")
    if args.overfit:
        overfit_main(args)
        return
    if args.portfolio:
        portfolio_main(args)
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
