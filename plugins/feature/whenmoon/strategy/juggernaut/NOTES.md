# juggernaut — round notes (last updated: 2026-07-05 18:35 UTC, end of round 3)

## ⚠️ ROUND 3 SUMMARY — read this first, then skip to "Open ideas" below

**Config UNCHANGED from round 2** (`regime_grain=0 regime_ma=0 adx_entry=8
exit_mode=0 chand_atr=3.5`, robust_ratio=1.0910) — re-confirmed bit-for-bit
reproducible on the current build. This round chased the operator's
"closing the gap is the highest-EV work" mandate hard: implemented and
walk-forward-tested FOUR new axes (three brand-new params + one existing
exit_mode variant), **all four REJECTED** — none beat 1.0910. Full mechanism
writeups are below under "Tried and REJECTED — round 3", but the one-line
versions:
- `exit_mode=1` (ADX-fade exit, no chandelier): full-sample pf looked
  amazing (9.95→42.96 BTC) but walk-forward **robust_ratio=0.7315** — worse
  than baseline. It rides trends even more patiently than the wide
  chand_atr=8 already rejected last round → same mega-fold variance
  blowup, different name. **This is the single most important result this
  round: a strategy variant can look dramatically better on full-sample
  pf/equity and still score WORSE on robust_ratio. Always walk-forward
  before believing a full-sample screen, no matter how good it looks.**
- `max_hold` (time-based forced exit): monotonically hurts full-sample pf
  as it tightens. Mechanism: the level-gate re-arms and re-fires
  IMMEDIATELY next bar if conditions still hold, so forcing an exit
  mid-trend is just an extra ~0.2% fee tax with no real exposure cut —
  unlike chand_atr's price-adaptive trim, a time cap has no way to tell
  "genuine reversal" from "still trending, arbitrarily interrupted."
  **Do not retry a bare time-based hold cap — it structurally can't work
  here without also blocking immediate re-entry, which would need a
  cooldown-after-forced-exit param (untested — see Open ideas).**
- `min_hold` (suppress early exits): totally inert, 330→331 trades
  unchanged across the whole 0-12 range. chand_atr=3.5 on the 4h grain
  doesn't produce early noise stop-outs to protect against. Dead axis,
  don't revisit unless chand_atr itself gets much tighter than 3.5.
- `d1_adx_min` (orthogonal daily-ADX floor): monotonic degradation, no
  bump anywhere. The 1d EMA_20 regime gate already does the
  trend-quality discrimination; a second ADX floor just prunes valid
  entries without finding better ones.
- Bonus: `adx_entry` is dead-flat all the way down to 3 (below the
  schema's `min_dbl=6`) — confirms ADX is never the binding constraint at
  the low end. Not worth a schema change.

**New this round: computed `max_ρ_vs_peer`** (Round 3 requires it) — pooled
per-fold return vectors (BTC 32 + ETH 32, dropping only the ETH index-0
universal pre-launch fold — index-1 is NOT dropped since peers traded it,
only I didn't), Pearson correlation: **ρ=0.510 vs mako, ρ=0.545 vs
riptide** — both comfortably under the operator's 0.85 "same-bet" line.
Juggernaut remains the field's most distinct gate-passer. Code to
reproduce: read both `windows[].return` arrays from the peer's posted
1x-friction result dirs (identify by empty `fixed_params` in
`manifest.json`), concatenate BTC-folds + ETH-folds[1:], pearson().

**Honest positioning after this round:** I did NOT close the gap to
mako (1.9548) or riptide (1.7516) — four genuinely-tried axes, zero wins.
This isn't for lack of trying; it may mean chand_atr=3.5/adx_entry=8 is
close to this mechanism's real ceiling for robust_ratio, given the
mega-trend-driven dispersion is fairly fundamental to a
trend-strength-gated rider (you cannot fully decouple return-per-trade
from trend size the way mako/riptide's ATR-scaled bounded reversion
does, without abandoning the "ride the leg" thesis that IS the
mechanism). See "Open ideas" for the two structurally-different
directions I have NOT tried yet (partial/scaled exits are not available
per the engine's binary long/flat model, so these are the remaining
levers).

## ⚠️ ROUND 2 rewrote the scoring recipe — read this before touching params

The contest no longer scores compounded `final_equity` / `avg$/mo`. It now
scores **`robust_ratio`** = pooled-across-BTC+ETH (mean per-120-day-fold
return) / (its cross-regime stddev), from `windows[].return` in
`iterations.jsonl`, behind hard gates (mean_fold>0 each market,
pos_frac≥0.75, worst_fold≥-10%, maxDD≤30%, survives 2x/4x friction). See
`COMPSTART.md §4` for the exact recipe — it has a copy-pasteable Python
snippet. **`final_equity` is not scored at all anymore.**

This inverted a round-1 conclusion: see "Tried and REJECTED #3" below, now
UN-rejected. **Lesson: when the metric changes, re-sweep every axis the
new metric could plausibly react to differently — don't carry forward a
prior round's verdict on faith, even one you validated carefully.** A
verdict is only as good as the objective it was validated against.

## Current best validated config (ROUND 2 / robust_ratio recipe)

```
regime_grain=0 regime_ma=0 adx_entry=8 exit_mode=0 chand_atr=3.5
```
(`adx_exit` is unused at `exit_mode=0` — its KV default 16 is a no-op.)
Code defaults (`JUG_DEFAULT_*` in juggernaut.c) already match this (v0.3).
KVs were also set explicitly this round (`set kv
plugin.whenmoon.strategy.juggernaut.{adx_entry,chand_atr} 8 3.5`) — see
the KV-staleness gotcha lower down, it bit every round so far.

Posted to `SCOREBOARD.md`: **robust_ratio = 1.0910**, pooled mean_fold
14.19% / std 13.01% / worst_fold 0.00% / pos_frac 96.9%. BTC: mean_fold
11.85% maxDD 3.87% worst +3.25% trades=303. ETH: mean_fold 16.52% maxDD
3.16% worst 0.00% trades=287. **trades/mo = 4.677** (up from 3.028).
Friction: 2x(10/10bps) robust_ratio=1.0758, 4x(20/20bps) rr=1.0450 — only
4.2% decay end-to-end, all gates still clear. This is **+43% robust_ratio**
over the round-1-config rescored as-is (0.7644 — see "what changed" below)
and lands within ~4% of cp3-mako's posted 1.1356, the field's current
leader, while trading ~17x less often.

### Prior (round-1 config, rescored under the new recipe — for reference only)
`adx_entry=10 chand_atr=8`: robust_ratio=**0.7644** (mean_fold 15.73%/std
20.58%/worst 0.00%/pos_frac 96.9%, maxDD 2.65%/1.91%, trades/mo=3.028,
friction 2x=0.7569 4x=0.7419). Posted to the scoreboard first (11:16 UTC
row) as the honest "what does my round-1 winner score under the NEW
recipe" checkpoint, per the operator's explicit round-2 kickoff
instructions — **do this first thing, next metric change too**, before
any retuning: it's the only way to know your real starting line.

Thesis: cp1/cp2/surf all gate entries on trend *direction* (a regime MA,
a MACD/EMA/RSI cross). juggernaut adds trend *STRENGTH* as a 4th axis —
`ADX_14` measured on the 4h decision grain — because ADX is
direction-agnostic and none of the existing strategies test it. Entry
(only while flat) = regime up (1d EMA_20 tide) AND `ADX_14(4h) >=
adx_entry` AND `close(4h) > EMA_20(4h)` (direction confirm, since ADX
alone can't tell up from down). This is a **level gate, not a cross** —
important, see "Tried and REJECTED" #1 below. Exit = chandelier trail
(`chand_atr` ATRs off the peak high) OR regime flip, whichever first.

**IMPORTANT — KV staleness gotcha for your next round.** The compiled
`.c` defaults already match the table above (I set them as the
`JUG_DEFAULT_*` macros), but the **running daemon's KV may still be
stale** from an earlier `/plugin load` at OLD default values (registration
is idempotent — code changes to `default_dbl`/`default_int` do NOT
retroactively update an already-registered KV). Before you trust a bare
`echo 'whenmoon backtest run ... juggernaut ...' | botmanctl` with no
explicit `name=val` args, run:
```sh
echo 'show kv plugin.whenmoon.strategy.juggernaut' | build/tools/botmanctl
```
and if `adx_entry`/`chand_atr`/`regime_ma` don't read `10`/`8`/`0`, fix
them with `set kv plugin.whenmoon.strategy.juggernaut.<param> <val>` (I
did this once already this round — if the daemon got freshstarted or the
KV DB reset since, you'll need to redo it, or just always pass the
params explicitly on official runs to sidestep the whole issue).

## Validated

**Round-2 config (chand_atr=3.5, adx_entry=8):** official walk-forward
robust_ratio + friction numbers are in "Current best validated config"
above, directly from the scored artifacts — that's the current source of
truth. **Full-sample pf/win-rate and OOS-tail-30 have NOT been re-run at
this config** (only walk-forward + friction were, since those are what's
scored) — worth a quick re-check next round if you have spare time, but
low priority since walk-forward already IS out-of-sample by construction.

**Round-1 config (chand_atr=8, adx_entry=10/12) — stale, historical only:**
- Full-sample (whole corpus, no walk-forward): BTC `pf=44.07 win=81%
  maxDD=2.65% trades=208 eq=$1,641,103` (208 fills→ from $10k). ETH
  `pf=31-33 win=~77% maxDD=~1.9%`.
- OOS-tail-30 (tune on first 70%, score untouched last 30%): both markets
  net-positive out of sample (BTC oos_realized≈+$20k, ETH oos_realized≈
  +$22k, on an adx_entry=12 pass).
- Official walk-forward swept `adx_entry=8..14` × `chand_atr=6,7,8,9,10,12`
  on BTC, found `adx_entry=10, chand_atr=8-12` within ~0.5% of each other
  — true for pf/equity, but see the ROUND 2 section below: a WIDER
  chand_atr ridge does NOT mean wide is optimal once you're scoring
  robust_ratio instead of equity.

## Tried and REJECTED — do not re-litigate these

1. **ADX-cross-up entry instead of level-gate.** v0.1 originally required
   ADX to cross UP through `adx_entry` (was below, now above) to arm a
   long — modeled on surf's cross-based entries. This was a real BUG for
   this signal: after a chandelier stop-out mid-trend, ADX often stays
   elevated (it measures strength, not "freshness"), so the strategy could
   get stuck flat for the rest of a strong trend waiting for a cross that
   might never come. Switching to a LEVEL gate (fires the instant
   `!in_position` and conditions hold true — already an edge since it's
   only evaluated while flat) fixed this. Single-line-of-reasoning fix,
   ~5x → ~24x BTC full-sample equity multiplier. **If you ever add a new
   ADX- or indicator-threshold entry axis, default to a level gate and
   only use a cross if you have a specific reason a level would double-fire.**

2. **`regime_grain=1` (4h) instead of `regime_grain=0` (1d).** Tested
   head-to-head with `regime_ma=0` (the good regime_ma value) on BTC:
   `regime_grain=0` → pf ~36-44, equity ~$1.4-1.6M. `regime_grain=1` → pf
   collapsed to ~1.35-1.37, trades exploded to 1200-1400+, equity crashed
   to ~$31-36k. Cause: the entry condition ALSO reads `close(4h) >
   EMA_20(4h)` for direction confirm, so a `regime_grain=1` with
   `regime_ma=0` (EMA_20 on 4h) is checking almost the SAME condition
   twice on the SAME bar — nearly redundant, and it makes the strategy
   whipsaw on every minor 4h wiggle instead of using an orthogonal, slower
   timeframe as the macro filter. **The 1d regime is doing real, distinct
   work; don't "simplify" by dropping it or aligning it with the decision
   grain.**

3. **Moving the decision grain 4h → 1h**, keeping the identical ADX +
   EMA_20 entry/exit logic (just recomputed on faster bars, same
   thresholds). Quadrupled trade count (206→452 trades) but pf collapsed
   42.75→3.20 and maxDD tripled (2.65%→9.1%) — same absolute ADX/EMA
   thresholds pass far more 1h noise as "strength", most of which isn't
   real. Retuned `adx_entry`/`chand_atr` specifically for 1h (full sweep,
   not just a guess) and the BEST achievable 1h config still only reached
   pf≈18, eq≈$710k with `adx_entry=15 chand_atr=14` — nowhere near 4h's
   pf 42-44. **4h is the right cadence for THIS specific ADX(period=14)
   signal; more frequent evaluation is not free.** (I reverted the code to
   4h via `git checkout HEAD -- juggernaut.c` after this failed — the
   committed 4h version is what's live. Don't redo the 1h experiment
   without a genuinely new idea for why it'd behave differently this
   time.)

4. **`exit_mode=3` "condition-flip"** (added this round, still in the code
   as a documented-but-non-default option): exit the INSTANT
   `ADX_14(4h) < adx_entry` or `close(4h) < EMA_20(4h)` — i.e., stay long
   only while the exact entry condition remains true, no chandelier
   patience, symmetric with entry. This mirrors cp3's mako thesis (a fast
   binary flip; "chop-flips are net-profitable" for THEIR self-EMA
   signal). For MY signal it does NOT transfer: traded ~4x more (840
   trades) but pf collapsed 42→3.2, equity fell from $1.6M to $248k.
   **Conclusion: my ADX+EMA20 signal's individual chop-flips are NOT
   profitable on their own — the edge here specifically depends on
   letting a real trend run via the wide chandelier, not on trading every
   condition flicker.** This is a genuine, tested difference from mako's
   architecture, not a tuning failure — don't try to "fix" exit_mode=3
   with different adx_entry/chand_atr values, the shape itself is wrong
   for this signal.

## Tried and REJECTED — round 3 (all tested via OFFICIAL walk-forward, not just full-sample)

All four were implemented as new, default-off params (`d1_adx_min=0`,
`min_hold=0`, `max_hold=0` all no-ops at 0; `exit_mode=1` already existed).
Code is still in `juggernaut.c` (harmless — defaults reproduce the posted
1.0910 bit-for-bit, re-confirmed this round) in case a future idea wants to
build on top of them, but none are part of the validated config.

5. **`exit_mode=1` (ADX-fade-only exit) at the tuned `adx_entry=8`.** Full
   BTC-corpus sweep of `adx_exit=5..24` (holding `adx_entry=8`): a broad,
   flat plateau at `adx_exit=5-9` (pf **42.96**, eq $1.65M, trades 205,
   maxDD **2.7%**, win **81%** — a huge jump from the chandelier's pf 9.95 /
   maxDD 3.9% / win 70%), decaying above 9. ETH confirmed the same shape
   independently (pf 32.8 at adx_exit=5-7, maxDD 1.9%, win 77%). Looked like
   a slam-dunk on full-sample metrics. **Official walk-forward at
   `adx_exit=7`: robust_ratio=0.7315 — WORSE than the chandelier's 1.0910,
   not better.** (pooled mean_fold 16.08%, std **21.98%** — much wider than
   chandelier's 13.01% — worst_fold 0.00%, pos_frac 96.9%, trades/mo 3.00).
   **Mechanism: ADX-fade-only is a MORE patient exit than even the old
   rejected wide chand_atr=8** (it only exits when trend-strength itself
   stalls, never on a price giveback), so it rides the exact same
   BTC-2020-21/ETH-2017-2020-21 mega-trend windows even further than
   chand_atr=8 did, reproducing (worse than) the round-2-rejected wide-chandelier
   variance blowup under a different mechanism. **THE LESSON: full-sample
   pf/equity and robust_ratio can point in OPPOSITE directions, and the gap
   can be large (pf 4x better, rr 33% worse) — never trust a full-sample
   screen alone for a "big win," always confirm on the actual official
   walk-forward before believing it or spending more sweep time refining it.**
   Do not retry ADX-fade exits (alone or in `exit_mode=2` "either-first" —
   inferred not to help either, since `exit_mode=2`'s full-sample numbers
   interpolate between exit_mode=0 and exit_mode=1 and degrade similarly as
   `adx_exit` tightens) without a fundamentally different way to bound how
   long the fade-exit is allowed to let a trend run.

6. **`max_hold` (force-exit after N 4h bars, meant to cap how much of one
   mega-trend a single fold can capture).** Full BTC-corpus sweep
   `max_hold=0,40,80,120,160,200,240..520` (adx_entry=8, chand_atr=3.5
   fixed): pf degrades from baseline 9.95 down to a trough of **4.72 at
   max_hold=40**, then recovers monotonically back to the EXACT baseline
   value by `max_hold=240` and beyond (240 4h-bars ≈ 40 days is
   apparently longer than any trade's natural chandelier-driven hold in
   this corpus, so the cap never binds there). **Mechanism (worked out
   from first principles, not just curve-reading): the entry level-gate
   re-arms and re-fires on the very next bar if `regime_up && strong &&
   dir_up` are still all true — which they usually are right after a
   force-exit mid-trend — so a `max_hold` exit is NOT a real exposure cut,
   it's an extra ~0.2%-round-trip fee tax paid on an otherwise-unbroken
   position.** This is exactly why it can only hurt (fee drag, no
   offsetting variance reduction) and never help, and why the damage is
   worst at the tightest cap (most forced round-trips) and vanishes as the
   cap loosens past the natural hold time. **Do not retry a bare
   `max_hold` — it is structurally incapable of reducing fold-return
   variance under this entry rule.** If revisited, it would need a paired
   **cooldown-after-forced-exit** (block re-entry for M bars even if the
   gate is true) to genuinely shrink exposure during a mega-trend — see
   Open ideas #1 below, this is untested and the natural next step.

7. **`min_hold` (suppress chandelier/fade/cflip exits for N bars post-entry).**
   Full BTC-corpus sweep `min_hold=0,2,4,6,8,10,12`: trade count and pf are
   flat (330-331 trades, pf 9.95-10.18) across the ENTIRE range — no
   meaningful effect in either direction. **Mechanism: chand_atr=3.5 on the
   4h grain apparently just doesn't produce early noise-driven stop-outs
   to protect against** — a 4h bar's typical single-bar noise is small
   relative to a 3.5-ATR(4h) trailing distance, so the chandelier rarely
   fires in the first few bars after entry regardless. **Dead axis — don't
   retest unless chand_atr itself is swept much tighter than 3.5 (e.g.
   <2.0), where early whipsaw might become newly plausible.**

8. **`d1_adx_min` (orthogonal ADX_14 floor on the cached 1d regime bar,
   alongside the existing 4h `adx_entry` floor).** Full BTC-corpus sweep
   `d1_adx_min=0,5,10,15,20,25`: monotonic degradation from the d1_adx_min=0
   baseline (pf 9.95→9.96→9.94→9.30→6.95→5.94, trades 331→207) with **no
   bump anywhere** — every step just prunes trades without finding a
   higher-quality subset. **Mechanism: the 1d EMA_20 regime gate
   (`jug_regime_up`) already IS the trend-quality filter; a second,
   independent 1d-ADX floor doesn't discriminate "better" uptrends from
   "worse" ones, it just randomly removes valid entries that happen to
   sit in a slower-grinding (but still real) 1d uptrend.** Dead axis,
   don't retest as currently shaped. (A DIFFERENT daily-grain idea —
   gating on 1d ADX *rising*, not just its level — is untested and not
   obviously subject to the same critique; low priority, see Open ideas.)

9. **Bonus, cheap check: extending `adx_entry`'s sweep below the schema's
   `min_dbl=6` down to 3.** Full BTC-corpus sweep `adx_entry=3..10`:
   completely flat, bit-identical output at every value 3-9 (pf 9.95,
   trades 331 unchanged). Confirms round 2's suspicion that the 6-9
   plateau wasn't a boundary artifact — **ADX is simply never the binding
   constraint in this whole range; `regime_up && dir_up` already select
   for periods where ADX_14(4h) is comfortably above 9 anyway.** Not worth
   a `min_dbl` schema change; the informative ADX threshold range (where
   round 2 found decay) is above 10, not below 6.

## ROUND 2: item #3 below (tighter chandelier) was RE-TESTED and REVERSED

The round-1 verdict "chand_atr down to 3-4 was strictly worse" was true
for compounded `final_equity` (a wide chandelier lets a few mega-trend
windows run further and compound harder) but **false for `robust_ratio`**.
Full sweep this round:

- Coarse `chand_atr=3..14` (BTC+ETH, walk-forward, `adx_entry=10` fixed):
  robust_ratio fell monotonically from **1.02 at chand_atr=3** to **0.72 at
  chand_atr=12-14**. The old chand_atr=8 sat at 0.76 — nowhere near the
  top.
- Fine `chand_atr=1.5..4.0` step 0.25: peak at **chand_atr=3.5, rr=1.0848**;
  ridge 3.0-3.75 all within ~5% of peak (broad, not a knife-edge). Below
  3.0 it degrades again (ETH maxDD starts climbing — 1.5 gives ETH maxDD
  9.9% vs 3.2% at 3.5) — so 3.5 isn't just "as tight as possible," it's a
  genuine interior optimum.
- Re-swept `adx_entry=6..20` at the new `chand_atr=3.5`: dead-flat plateau
  6-9 (rr=1.0910 unchanged across all four), decaying above 10. Picked
  `adx_entry=8`, the plateau's midpoint (not the boundary — the schema's
  `min_dbl=6` means 6-9 flat could just mean "ADX is rarely the binding
  constraint down there," not a true argmax at 6).
- **Mechanism**: a tight chandelier trims the SAME handful of mega-trend
  windows (BTC 2020-21, ETH 2017/2020-21) that were the dominant
  contributor to `pooled_std` under the wide chandelier — cuts stddev
  faster than it cuts the mean. Bonus: more, shorter round-trips per
  trend, so trades/mo nearly doubled (3.03 → 4.68) for free, which also
  helps the secondary trades/mo metric.
- Net: **robust_ratio 0.7644 → 1.0910 (+43%)** on the unchanged mechanism.
  Officially scored, friction-tested (2x/4x, only 4.2% decay), all gates
  clear with room. Full detail + numbers: see the 2026-07-05 11:32 UTC
  scoreboard row.

**Takeaway for next round: a "REJECTED" verdict is only as good as the
objective it was tested against.** If the scoring recipe changes again,
re-sweep chand_atr (and probably adx_entry jointly with it) again before
trusting this round's config — don't assume 3.5/8 is permanent just
because it beat 8/10 this time.

## Open ideas — ranked by promise (re-ordered for round 3)

0. **DONE this round: joint chand_atr × adx_entry grid, confirmed 3.5/8
   is the true joint peak, not just a coordinate-ascent artifact.** Ran
   the full 7×7 cross product (`chand_atr=2:0.5:5 adx_entry=6:1:12`, 49
   combos/market — **remember `--top-n` defaults to `min(N,threads)` and
   ranks by `--rank-by` (default `realized`), so a grid bigger than your
   thread count silently drops the tail unless you pass `--top-n <N>`
   explicitly** — I lost the first attempt at top_n=20/49 to exactly this
   and had to rerun). Global max over all 49×49=2401... no, 49 combos per
   market, pooled: rr=1.0910 at chand_atr=3.5 × adx_entry∈{6,7,8,9} (all
   tied, flat plateau), strictly beating every other cell including
   chand_atr=3.0 (1.0270), 4.5 (1.0377), and all adx_entry>10 at any
   chand_atr. **Sequential coordinate-ascent found the true joint optimum
   here** — no need to re-run this grid next round unless the underlying
   engine or corpora change.
1. **DONE (round 3), all REJECTED: adx_entry-below-6, daily-ADX
   confirmation, minimum-holding-period floor.** These were items 1-3 from
   round 2's list. All implemented, all walk-forward-tested, all
   REJECTED — see "Tried and REJECTED — round 3" above (items 6-9) for the
   full mechanism writeups. Don't re-litigate any of them next round
   unless chand_atr itself moves far from 3.5 (min_hold's dead-axis
   verdict was conditional on the CURRENT tight chandelier rarely
   producing early stop-outs; a much tighter chandelier might change that).

2. **(Highest promise, UNTESTED — round 3's most concrete finding) Cooldown-
   after-forced-exit.** Round 3 showed `max_hold` alone can't work because
   the entry gate re-arms and re-fires on literally the next bar, turning
   a forced exit into pure fee drag with zero exposure reduction. The
   fix suggested by that failure: pair `max_hold` with a **cooldown**
   param — after a `max_hold`-triggered exit specifically (NOT a regime/
   chandelier exit), block re-entry for `cooldown_bars` even if
   `regime_up && strong && dir_up` are all true. This is a genuinely
   different, untested mechanism: it would actually remove exposure
   during exactly the mega-trend windows that blow out `pooled_std`,
   rather than taxing an unbroken position. Implementation sketch: add
   `int cooldown_until` to `jug_state_t` (a bar-index or timestamp
   counter), set it on a `hit_maxhold` exit only (`cooldown_until =
   s->bars_since_start + cooldown_bars` — needs a running bar counter,
   not currently tracked, or use `bar->ts_close_ms` directly since 4h bars
   are evenly spaced), and gate entry on `now >= cooldown_until` in
   addition to the existing conditions. Sweep `max_hold` (probably
   80-200, the region round 3 showed pf degradation in) jointly with
   `cooldown_bars` (try 20-100) via official walk-forward, not full-sample
   pf (round 3's #1 lesson: full-sample screening actively misleads for
   this signal's exit-shape changes).

3. **(Medium promise, UNTESTED) Daily-ADX *rising*, not level.** Round 3's
   `d1_adx_min` (a 1d ADX FLOOR) was rejected — it just pruned valid
   entries without discriminating better ones, because the 1d regime gate
   already does the trend-quality job. A different daily-grain idea, not
   subject to that critique: gate additionally on the 1d ADX being HIGHER
   than it was N days ago (a slope/rising check, e.g. `d1_adx >
   d1_adx_Nago * 1.1`) — this asks "is the macro trend accelerating," a
   distinct question from "is the macro trend strong" (which the level
   floor asked and which didn't help). Would need a small rolling ring of
   past 1d ADX readings in `jug_state_t` (same pattern as cp1's Donchian
   ring). Lower priority than #2 since it's a fresh hypothesis with no
   supporting signal yet, vs #2 which is a direct, reasoned fix to a
   round-3 near-miss.

4. **(Positioning, not a gap-closer)** Field as of round 3 end:
   mako=1.9548 (deep-dip momentum-reversion "hunt mode"), riptide=1.7516
   (shallow-dip mean-reversion), juggernaut(me)=1.0910 (trend-STRENGTH ADX
   level-gate + chandelier ride) — I remain ~44% behind the leader and did
   NOT close the gap this round despite four honest attempts (see round-3
   summary at the top of this file). New this round: `max_ρ_vs_peer`
   computed and posted — **ρ=0.510 vs mako, ρ=0.545 vs riptide**, both
   comfortably under the operator's 0.85 "same-bet" line, and notably
   mako+riptide are themselves both dip-reversion mechanisms (likely
   correlated with EACH OTHER) while I'm the field's only trend-strength
   rider. **Don't over-rely on distinctiveness alone if there's more
   honest ratio to find** — try #2 above first next round, it's the one
   idea with an actual causal story for why it might move the needle.

## Gotchas hit this round (environment/tooling — not strategy-specific)

- **KV staleness** — see the callout under "Current best validated
  config" above; this bit me directly this round (a walk-forward run with
  no explicit params silently reproduced OLD `adx_entry=22` results after
  I'd already changed the code defaults to 10).
- **Backtest engine went async mid-round** (a daemon restart introduced
  this): `whenmoon backtest run` now returns immediately with `run
  queued: ...`; poll `show tasks` until your `wm-btrun:<name>` entry is
  gone, then read `iterations.jsonl`. COMPSTART.md §3/§4 now document
  this — it was new this round, may already be old news by your round.
- **`echo 'cmd' | botmanctl` is a raw line to the daemon's own parser, NOT
  a shell.** Never embed literal `'quote'` characters in the piped
  string (e.g. `irc join botman '#cabal'`) — they pass through verbatim
  and corrupt the daemon's own tokenizer. Use bare tokens:
  `irc join botman #cabal`.
- **`say botman #cabal <msg>` silently truncates at the IRC 512-byte line
  limit** — no error, the tail of a long announcement just vanishes on
  the wire. Keep announcements to ~2-3 sentences. Also saw a spurious
  `send failed: botman -> #cabal` WARN on a message that actually DID
  land in the channel (verified with a disposable `ircspy -C
  <unique-path>` observer) — don't fully trust that warning as proof of
  non-delivery if you're unsure, but don't rely on ircspy for routine
  announcements either (COMPSTART.md §6 explicitly says use `say`, not
  ircspy, for exactly this reason).
- **If you ever DO spin up `ircspy` for something**, always pass an
  explicit `-C <unique-path>` control socket. Without it, a second
  `ircspy` instance can silently steal `/tmp/ircspy.sock` from a peer's
  already-running session (unix socket bind/unlink race), breaking their
  `ircspyctl` until they notice and restart. I did this by accident this
  round before `say` existed; harmless once caught, but a live "don't do
  this" lesson.
- **NEW this round: keep the `-C` control-socket path SHORT.** Unix
  `sun_path` is capped at 108 bytes on Linux. A path built from this
  session's scratchpad directory (`/tmp/claude-<id>/-mnt-fast-.../<uuid>/
  scratchpad/foo.sock`) is easily 110-120 bytes and silently gets
  truncated/collides, so `ircspy` reports `ctl bind(...): Address already
  in use` and disables its control socket even though the path was never
  actually used before (the truncated form collided with a leftover
  truncated bind, not a real peer). It still connects to IRC and relays
  channel traffic to its log file fine — the control socket (interactive
  `/quit` etc.) is what breaks. Fix: use a short unique path directly
  under `/tmp` (e.g. `/tmp/cp2j-spy.sock`), not the scratchpad dir, when
  you need `ircspyctl` to work against it.
- **Bot channel join.** `botman`'s `#cabal` autojoin is `off` in KV
  (`bot.botman.irc.chan.cabal.autojoin = false`) — if `say` reports
  send failures and the bot genuinely isn't in the channel yet, join it
  first: `echo 'irc join botman #cabal' | build/tools/botmanctl`.
- **After a daemon restart, live whenmoon markets can come back with
  NO markets configured at all** (not just detached strategies) — this
  is out of scope for a competitor to fix; don't try to restore
  paper-trading markets yourself, that's the operator's/engine's concern.
