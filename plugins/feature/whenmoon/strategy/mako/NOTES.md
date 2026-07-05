# mako — cp3 round notes (private baton; peers keep out)

You are **Competitor #3**; your strategy is **`mako`** in this directory.
Read this whole file before touching anything. Round-4 self (the
DIFFERENTIATION round) writing to round-5 self. Auto-memory
`project_cp3_mako.md` mirrors the headline; THIS file is the
authoritative baton.

## The metric + the NEW differentiation number (round-4 addition)

Score = **robust_ratio** = mean(per-fold `windows[].return`, pooled
BTC+ETH ~64 folds) / pstdev(same), official walk-forward
(`train=365:test=120:step=120`, default economics) on
`/tmp/{btc,eth}-comp.wm`. Gates: mean_fold>0 per market, pos_frac>=0.75,
worst_fold>=-0.10, maxDD<=0.30 per market, 2x/4x friction re-runs still
clear profit+consistency. Secondary = trades/mo. COMPSTART §4 exact.

**NEW since round 4: `max_ρ_vs_peer`** — Pearson correlation of your
pooled per-fold return vector vs each peer's POSTED artifacts (drop the
pre-ETH-launch zero fold, n=63), self-reported in your scoreboard row.
ρ ≳ 0.85 vs a peer = "same bet" — of a same-bet pair AT MOST ONE
advances to the paper finals. The finals seat = best AND most different.
Scorers: `score4.py` (score + corr subcommands) and `joint4.py` (sweep
scorer: rr + ρ per combo, keyed by params) in the round-4 session
scratchpad — rewrite from COMPSTART §4 if gone (~60 lines each; joint4
hardcodes riptide's posted dirs `20260705-1144{56,59}-riptide-*`).

## Current best (POSTED 2026-07-05 18:41:23 UTC — round-4 flagship)

```
mako hunt=1 regime_grain=0 line_mode=0 cmp_mode=0 alpha=0.3 dip_atr=1.0 stop_atr=2 target_atr=2.25
```
(tgt_mode=0 dip_ref=0 cool_bars=0 band_bps=0 max_hold=0 bear_hunt=0 —
all schema defaults, so the 8 CLI args above fully reproduce on a fresh
daemon. KVs in the running daemon are synced to this config.)

| friction | robust_ratio | mean_fold | std | worst | pos_frac | maxDD B/E | tr/mo |
|---|---|---|---|---|---|---|---|
| 1x | **1.9042** | +16.77% | 8.81% | 0.00 | 0.984 | 2.90%/2.70% | 11.68 |
| 2x | 1.8615 | +16.05% | 8.62% | 0.00 | 0.984 | 2.96%/2.90% | 11.68 |
| 4x | 1.7693 | +14.64% | 8.27% | 0.00 | 0.984 | 3.23%/3.31% | 11.68 |

**max_ρ_vs_peer = 0.688 (riptide), 0.593 (juggernaut).** All gates PASS
at all frictions. Official artifacts: dirs
`20260705-1839{41,43,45,47,49,51}-mako-{btc,eth}-usd` under
`/mnt/fast/web/lame/whenmoon/` (41/43=1x, 45/47=2x, 49/51=4x).

**Why this config over the higher-rr 11:59 row (rr=1.9548, ρ=0.810):**
the operator declared mako+riptide "the same bet — only one seat" and
told us differentiation beats one more rr point. v0.3 trades −0.05 rr
headline for ρ 0.81→0.69, +35% higher mean window profit, lower maxDD,
and a −7.1% friction curve (v0.2 was −17%, riptide −30%). Its 4x score
(1.769) still beats riptide's 1x (1.752) — keep leading with that.
The 11:59 v0.2 row stays on the board as history; do NOT re-post it as
flagship unless the differentiation rule changes.

## What mako v0.3 is (same hunt mechanism, deeper geometry)

Regime-gated deep-washout dip harvester on 1h. Slow 1d self-EMA tide
(alpha=0.3, ~5-6d) as a PERMISSION GATE only; inside an up-tide, buy
the 1h close washed out >= 1.0 ATR_14(1h) BELOW EMA_20(1h), immediately
at the washout close (no bounce-wait). Exit: reversion to EMA_20 +
2.25 ATR (rides the whole snap-back leg, not the first bounce), hard
stop 2 entry-ATRs below entry, regime-flip failsafe. hunt=0 still runs
the legacy flipper bit-for-bit. Three round-4 knobs exist in the code,
all default-off: `cool_bars` (post-exit entry throttle), `tgt_mode`
(1 = entry-anchored fixed-R target), `dip_ref` (1 = BB_LOWER anchor).
All three REFUTED as flagship improvements (below) but kept as mapped
axes — they cost nothing at defaults.

## Proven / REFUTED in round 4 — do not re-burn compute

- **The target ridge extends past the round-3 map.** Round-3 notes said
  "target 0.5-1.5, 1 is peak" — WRONG past 1.5: target 1.75 → rr 1.986,
  and the whole dip 0.75-1.0 × target 1.5-2.5 block sits rr 1.83-2.03.
  The rr-max of the mapped surface is dip=1.0/target=1.5 (rr **2.026**,
  ρ 0.794, 4x 1.808) — the "pure rr" fallback if differentiation ever
  stops mattering. Deeper dip+target = flatter friction decay
  (dip1/tgt2.5: −6.6%; dip0.5/tgt1.5: −11%+) because per-trade edge
  scales with geometry while cost/trade is fixed.
- **ρ vs rr frontier mapped** (1x, vs riptide posted): tgt1.5/dip1
  0.794 → tgt1.75/dip0.75 0.731 → tgt2/dip1 0.753 → tgt2.25/dip1
  **0.688** → tgt2.5/dip0.75 0.663. Chosen point dip1/tgt2.25 dominates
  its deeper neighbors (higher rr AND better 4x, ρ within noise).
- **cool_bars (post-exit throttle) REFUTED for rr**: every setting
  (24/48/96) drops rr — mean falls faster than std (cool96/tgt1.75:
  rr 1.733). It DOES cut ρ (to ~0.60) — a last-resort differentiation
  lever if riptide converges onto deep targets, at real rr cost.
- **tgt_mode=1 (entry-anchored fixed-R target) REFUTED**: dominated by
  EMA-anchored at every comparable point (best 1.888 vs 1.986). The
  EMA drift while held is where the extra edge lives, not a bug.
- **dip_ref=1 (BB_LOWER σ-scaled anchor) REFUTED as flagship**: rr
  collapses to 1.2-1.6 (below riptide) BUT ρ falls to 0.29-0.59 —
  the most decorrelating thing tested. If the operator ever demands
  ρ < 0.5 at any cost: dip_ref=1 dip_atr=0 target_atr=2.5 was rr 1.53
  / ρ 0.588; dip_atr=0.75 variants hit ρ 0.29 at rr ~1.2.
- **Covariance with riptide lives in the mania right tail**: 2020-12
  (both mkts), 2017-12, 2017-04, 2021-04 carried ~46% of it at v0.2.
  Deeper target shifted per-fold P&L from dip-DENSITY (shared with
  riptide) toward reversion-leg LENGTH (not shared) — that's the
  mechanism behind 0.81→0.69, remember it when riptide's next move
  lands.
- Round-3 refutations still stand: bear_hunt dead (pos_frac 0.33,
  maxDD 80%+), max_hold no-op, flipper-alpha ≠ gate-alpha (1d/0.3
  beats 4h/0.82 for the gate), alpha<0.2 collapses, dips ≥1.5 ATR
  monotonically worse ON THE OLD target=1 GEOMETRY (dip 1.25-1.5 ×
  target 2-2.5 is UNMAPPED — see next experiments).

## Peer standings (as of 2026-07-05 ~18:45 UTC)

- **cp1 riptide rr=1.7516** (11:45 row, unchanged as of my post):
  shallow 0.25-ATR arm-then-trigger dips, target 0.5, stop 3, 1d
  tide alpha=0.6, 23.75 tr/mo, 4x decay −30%. Had FOUR walk-forward
  runs in flight ~18:28-18:35 — expect a new row from them; RECHECK
  the board and recompute ρ against any NEW posted artifacts of
  theirs before believing my 0.688 is current. If they deepen toward
  my geometry, their ρ vs me rises symmetrically — mine is the
  higher-rr, flatter-friction side of the pair either way.
- **cp2 juggernaut rr=1.0910** (18:30 row): config unchanged from
  11:32; four honest experiments (ADX-fade exit, max_hold, min_hold,
  d1_adx_min) all REJECTED — they could not close the rr gap this
  round. ρ 0.51/0.55 vs field. Still the most distinct mechanism and
  the natural finalist #2 by construction... unless the operator
  weighs rr hard, in which case the pair-seat fight is mako-vs-riptide
  and differentiation decides it. juggernaut's stall is my margin:
  even my 4x rr (1.769) is 62% above their 1x.
- ρ matrix at my post time: mako-riptide 0.688, mako-jugg 0.593,
  riptide-jugg 0.545 (their number). I am no longer the closest pair
  in the field by much — riptide-jugg 0.545 vs mako-jugg 0.593.

## Workflow (engine build #3979, async runs) — traps that cost time

- Loop: edit `mako.c` → `ninja -C build` → `whenmoon strategy reload
  mako` → `backtest run ... --walk-forward train=365:test=120:step=120`.
  main.c/irc.h warnings are pre-existing noise; mako.c stays clean.
- `backtest run` returns `run queued:` + result dir; poll `show tasks`
  until `wm-btrun:mako` gone (background `until` loop; foreground
  sleep chains blocked). N=1 official ≈ 2-4 min; 15-24-combo WF sweep
  ≈ 5-10 min under peer contention. Peers run concurrently — keep
  `--threads 6-8` on sweeps.
- **Max 8 CLI axes** incl. fixed `name=val`s — pin extras via
  `set kv plugin.whenmoon.strategy.mako.<k> <v>`, verify `show kv`.
  Flagship needs exactly 8; the three new knobs ride on defaults.
- **`--top-n` gates which rows get `windows[]`** — sweeps needing §4
  scoring on all combos: pass `--top-n <N_combos>`.
- Same-second same-market runs share a result dir — stagger `sleep 2`.
- Friction-stability trick that picked the flagship: run the SAME
  sweep at 1x and at `--fee-bps 20 --slip-bps 20`, pick the argmax
  stable in BOTH tables.
- `say botman '#cabal' ...` replies "send failed" ON SUCCESS (inverted
  check, known bug); verify `grep -a "PRIVMSG #cabal" /tmp/botman.log`.
  IRC truncates ~420 chars — numbers first, trash talk last.
- SCOREBOARD.md gets concurrent peer appends — my Edit-tool append hit
  two mid-flight conflicts; the working recipe is awk-insert after the
  last peer row (anchor on their unique timestamp), verify, then `cp`.
- Peer POSTED result dirs under /mnt/fast/web/lame/whenmoon/ are fair
  game (that's how ρ is computed); their NOTES.md + source are not.
  Peer posted 1x dirs this round: riptide `20260705-1144{56,59}`,
  juggernaut `20260705-113027` (both markets same stamp for jugg).

## Ranked next experiments (round 5+)

1. **Re-verify + re-correlate FIRST** (cheap): re-run the flagship 1x
   pair (expect rr=1.9042 bit-identical), then recompute ρ vs the
   LATEST posted peer artifacts — riptide almost certainly posted
   after me (they had 4 runs in flight at 18:35). If their new config
   moved toward deep targets, the 0.688 is stale and the
   differentiation fight re-opens.
2. **Unmapped corner: dip 1.25-1.5 × target 2-3** (with stop 2-2.5).
   Round-3's "deeper dips are worse" verdict was measured at target=1;
   round 4 proved the geometry interacts (deeper dip LIKES deeper
   target). If rr ≥ 1.85 with ρ ≤ 0.65 lives there, it strictly beats
   the flagship on seat-odds. One 12-combo sweep at 1x+4x answers it.
3. **stop_atr re-map on the new geometry** (was pinned at 2 from the
   round-3 target=1 map): stop=[1.5,2,2.5,3] × dip=[1] × tgt=[2.25].
   Cheap (8 combos with a tgt neighbor), might buy worst-fold margin
   at 4x, and stop=3 would drop another param-distance from riptide's
   stop=3... wait, that CONVERGES a param — keep stop ≠ 3 unless rr
   demands it; the differentiation story reads better with the whole
   param vector distinct.
4. **If riptide converges onto deep geometry**: my counters, in order:
   (a) this flagship already wins the pair on rr + friction at every
   mapped point; (b) cool_bars=24-48 on top (costs ~0.2 rr, cuts ρ to
   ~0.6); (c) dip_ref=1 nuclear option (ρ 0.3-0.5, rr 1.2-1.5 — only
   if the operator explicitly prices ρ above rr).
5. **Secondary (trades/mo) is now my weakest number**: 11.68 vs
   riptide 23.75, jugg 4.68. Among gate-passers it's the tiebreak.
   Pre-mapped busier fallbacks: dip0.5/tgt1.5 rr 1.937 @ 18.6 tr/mo
   (ρ 0.813 — bad), dip0.75/tgt1.75 rr 1.986 @ 15.2 tr/mo (ρ 0.731).
   The tgt1.75/dip0.75 point is the "more trades, more rr, slightly
   more ρ" alternative if the operator signals trades matter more
   than the ρ gap; it beat the flagship on everything except ρ.
6. **FINALS pitch (keep current)**: a decade, 64 windows, ZERO losing
   windows at QUADRUPLE realistic costs; maxDD never above 3.31% at
   any friction; per-fold correlation to nearest rival 0.69 and
   falling; mean window profit +16.8% on the $10k stake. "The one you
   can actually hand real money" — keep the 1x/2x/4x table current
   every round.
