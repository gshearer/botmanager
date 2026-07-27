#!/usr/bin/env python3
"""rig.py — WM-EDGE-2 step 2: the Python research rig core.

Prototype-in-Python, productionize-in-C only for survivors. This module is
the shared foundation every Python bet-family prototype builds on:

  load()          Parquet loader with the WM-RIGOR-6 research cutoff GUARD
  daily_panel()   wide close/volume/quality matrices on a full UTC calendar
  eligible()      point-in-time universe membership (no survivorship bias)
  fold_windows()  walk-forward test folds aligned to the official recipe
                  (train=365 : test=120 : step=120 — folds are the 120d
                  test windows; fixed-config strategies have no train step)
  turnover_cost() engine-economics cost model (fee+slip bps per side)
  fold_metrics()  robust_ratio family: mean/pstd/rr/pos_frac/worst
  rank_stability()RIGOR-3-style seeded half-split Spearman rho
  census_append() every scored config is counted — no silent trials

Scoring conventions match tools/wm_score.py: population stdev, robust
ratio = mean/pstd of per-fold returns, active return vs benchmark on the
same window, drawdown from the daily equity path (mark-to-market — the
per-fill flattery problem does not exist here because everything is
daily-marked by construction).

THE CUTOFF GUARD (WM-RIGOR-6). Research ends at 2025-03-31 00:00 UTC
(`WM_BT_HOLDOUT_CUTOFF_MS`). load() silently clamps to the cutoff by
default; asking for later data requires holdout=True, which appends an
audit line to strategy/HOLDOUT_LOG.md exactly like the C guard. One shot
per family — treat holdout access as spending something you cannot get
back.
"""

import datetime as dt
import getpass
import json
import os

import numpy as np
import pandas as pd

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
DATA_DIR = os.path.join(REPO_ROOT, "btdata", "research")
HOLDOUT_LOG = os.path.join(
    REPO_ROOT, "plugins", "extension", "whenmoon", "strategy",
    "HOLDOUT_LOG.md")
CENSUS_PATH = os.path.join(DATA_DIR, "census.jsonl")

RESEARCH_CUTOFF = pd.Timestamp("2025-03-31 00:00:00")   # UTC, exclusive
FEE_BPS = 5.0                                           # engine default
SLIP_BPS = 5.0                                          # engine default
TRAIN_DAYS = 365                                        # official recipe
TEST_DAYS = 120
STEP_DAYS = 120


# ---------------------------------------------------------------- loading

def load(grain="1d", end=None, holdout=False, data_dir=DATA_DIR):
    """Load the exported OHLCV panel, guarded by the research cutoff.

    end=None means "everything permitted": the research cutoff normally,
    the full export when holdout=True. Requesting end > cutoff without
    holdout=True raises — that data is the one-shot final exam
    (COMPSTART §Holdout discipline).
    """
    frame = pd.read_parquet(os.path.join(data_dir, f"ohlcv_{grain}.parquet"))
    frame["ts"] = pd.to_datetime(frame["ts"])

    limit = RESEARCH_CUTOFF if end is None else pd.Timestamp(end)
    if limit > RESEARCH_CUTOFF:
        if not holdout:
            raise PermissionError(
                f"requested end {limit} is past the research cutoff "
                f"{RESEARCH_CUTOFF} (WM-RIGOR-6). Post-cutoff data is the "
                f"one-shot holdout; pass holdout=True ONLY for a final, "
                f"operator-witnessed exam run (access is audit-logged).")
        _holdout_audit(limit, grain)
    elif holdout:
        raise ValueError("holdout=True with a pre-cutoff end is a mistake: "
                         "nothing here needs the flag — drop it.")
    return frame[frame["ts"] < limit].copy()


def _holdout_audit(limit, grain):
    """Append a HOLDOUT_LOG.md line in the C guard's field layout."""
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    user = getpass.getuser()
    line = (f"{stamp} | @{user} | py-rig | btdata/research/ohlcv_{grain}"
            f".parquet | load end={limit} --holdout\n")
    with open(HOLDOUT_LOG, "a", encoding="utf-8") as fh:
        fh.write(line)


def daily_panel(frame):
    """Wide matrices on a full UTC daily calendar.

    Returns (close, volume, n_1m) DataFrames indexed by date with one
    column per pair. Prices forward-fill across exchange gaps (price
    persistence); volume and bar-count fill 0 (no trading is knowledge,
    not a hole).
    """
    close = frame.pivot(index="ts", columns="pair", values="close")
    volume = frame.pivot(index="ts", columns="pair", values="volume")
    n_1m = frame.pivot(index="ts", columns="pair", values="n_1m")
    calendar = pd.date_range(close.index.min(), close.index.max(), freq="D")
    close = close.reindex(calendar).ffill()
    volume = volume.reindex(calendar).fillna(0.0)
    n_1m = n_1m.reindex(calendar).fillna(0.0)
    return close, volume, n_1m


def eligible(close, n_1m, min_history_days, min_activity_1m=300):
    """Point-in-time universe membership mask (same shape as close).

    A pair is eligible on day t iff it has min_history_days of real
    (non-ffilled) prices behind it AND its trailing-30d median 1m-bar
    count clears min_activity_1m (a liquidity floor: ~300 traded minutes
    a day; below that, fills at our costs are fiction). Eligibility uses
    only information available at t — pairs ENTER the universe when they
    qualify, exactly as a live deployment would have seen it.
    """
    has_price = n_1m.gt(0)
    history = has_price.cumsum()
    seasoned = history.ge(min_history_days)
    active = n_1m.rolling(30, min_periods=30).median().ge(min_activity_1m)
    return seasoned & active.fillna(False)


# ------------------------------------------------------------------ folds

def fold_windows(index, train_days=TRAIN_DAYS, test_days=TEST_DAYS,
                 step_days=STEP_DAYS):
    """[(test_start, test_end)] tiling the official walk-forward recipe.

    The first test window opens train_days after the calendar start; each
    subsequent window steps step_days. Only FULL test windows count — a
    ragged tail fold would not be fold-comparable.
    """
    start, end = index.min(), index.max()
    folds = []
    t0 = start + pd.Timedelta(days=train_days)
    while t0 + pd.Timedelta(days=test_days) <= end:
        folds.append((t0, t0 + pd.Timedelta(days=test_days)))
        t0 += pd.Timedelta(days=step_days)
    return folds


# ------------------------------------------------------------------ costs

def turnover_cost(weight_deltas, friction=1.0,
                  fee_bps=FEE_BPS, slip_bps=SLIP_BPS):
    """Cost (as a return drag) of trading |Δweight| book fraction.

    Each unit of weight change is ONE side of a trade (engine economics:
    fee_bps + slip_bps per side). friction 2.0/4.0 reproduces the 2x/4x
    stress convention.
    """
    per_side = (fee_bps + slip_bps) * friction / 1e4
    return weight_deltas.abs().sum() * per_side


# ---------------------------------------------------------------- metrics

def max_drawdown(equity):
    """Peak-to-trough on a daily equity series (mark-to-market)."""
    running_peak = equity.cummax()
    return float(((running_peak - equity) / running_peak).max())


def fold_metrics(fold_returns):
    """The wm_score.py robust-ratio family over per-fold returns."""
    values = [float(v) for v in fold_returns]
    n = len(values)
    mean = sum(values) / n
    pstd = float(np.std(values))            # population, like wm_score
    return {
        "n_folds": n,
        "mean_fold": mean,
        "std_fold": pstd,
        "robust_ratio": mean / pstd if pstd > 0 else float("inf"),
        "pos_frac": sum(v > 0 for v in values) / n,
        "worst_fold": min(values),
    }


def rank_stability(matrix, n_splits=200, seed=1743379200):
    """RIGOR-3 rank-stability: median Spearman rho over seeded half-splits.

    matrix: configs x folds array of per-fold returns. Each split halves
    the FOLD axis, ranks configs by mean return on each half, and
    correlates the rankings. A real ridge survives the split; an
    overfit ranking does not. Returns (median_rho, rho_list).
    """
    from scipy.stats import spearmanr
    matrix = np.asarray(matrix, dtype=float)
    _, n_folds = matrix.shape
    rng = np.random.default_rng(seed)
    rhos = []
    for _ in range(n_splits):
        perm = rng.permutation(n_folds)
        half_a, half_b = perm[: n_folds // 2], perm[n_folds // 2:]
        rank_a = matrix[:, half_a].mean(axis=1)
        rank_b = matrix[:, half_b].mean(axis=1)
        rho = spearmanr(rank_a, rank_b).statistic
        if not np.isnan(rho):
            rhos.append(float(rho))
    return float(np.median(rhos)), rhos


# ----------------------------------------------------------------- census

def census_append(family, config, metrics, path=CENSUS_PATH):
    """One line per scored config — the trials census input.

    Every configuration that produced a number a human might select on
    gets counted, so the expected-max-of-T haircut stays honest. Never
    filter before appending.
    """
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    line = {"ts": stamp, "family": family, "config": config,
            "metrics": metrics}
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "a", encoding="utf-8") as fh:
        fh.write(json.dumps(line) + "\n")
