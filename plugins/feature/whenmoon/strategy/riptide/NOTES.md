# riptide — competitor handoff notes (cp1)

**Read this AFTER `COMPSTART.md` + `strategy/AGENTS.md`. You are competitor
#1 (cp1). Your strategy is `riptide`. This file is the letter your
previous-round self left you — trust it, but re-judge every conclusion
against the CURRENT scoring recipe + the CURRENT scoreboard before acting
(the recipe changed once already, and peers move every round).**

Files (all working tree): `riptide.c`, `meson.build`, one line
`subdir('feature/whenmoon/strategy/riptide')` in `plugins/meson.build`.
The daemon has riptide loaded; the `.so` is on disk so a restart restores it.

---

## 0. ⚠️ WHAT CHANGED THIS ROUND (round 3) — read first

The scoring recipe was STABLE (robust_ratio + gates, unchanged from round 2).
The round-3 theme was **DIFFERENTIATION**, now MEASURED and self-reported:
compute Pearson **ρ** of your pooled per-fold return vector vs each peer's
(from their **posted** artifacts under `/mnt/fast/web/lame/whenmoon/`, n=63
after dropping the pre-ETH-launch zero fold), and post `max_ρ_vs_peer`.
ρ ≳ 0.85 = "same bet"; finals seat AT MOST ONE of a same-bet pair.

**The problem I inherited:** round 2 I posted rr=1.7516 with `regime_grain=0`
(1d tide) — but **mako ALSO used a 1d tide**, so ρ(riptide,mako)=0.81. I was
the redundant twin, and the WORSE one (mako 1.955 rr, lower DD, flatter
friction). I was on track to be CUT.

**What I did:** kept the mean-reversion mechanism but **moved off the shared
1d tide onto the 4h intraday tide (`regime_grain=1`)** and **deepened the
entry to 0.75 ATR**. The tide grain turned out to be the dominant ρ driver
(shared 1d gate ⇒ same active windows ⇒ ρ≈0.81-0.87; a 4h gate ⇒ ρ≈0.46-0.65).
This dropped ρ vs mako to **0.499** while HOLDING every gate and IMPROVING
drawdown + friction. I went from "likely cut" to **most-decorrelated node in
the field**.

---

## 1. CURRENT STANDING (2026-07-05 ~18:51 UTC) — DISTINCT, FIELD #2 rr

Posted scoreboard row (v0.4):

```
robust_ratio = 1.6148 (PRIMARY)   trades/mo = 22.50 (SECONDARY)
pooled: mean_fold=+16.71%  std=10.35%  worst_fold=+0.00%  pos_frac=0.984
BTC  mean_fold=+13.72%  maxDD=2.05%  rr=1.557
ETH  mean_fold=+19.71%  maxDD=2.44%  rr=1.809
friction: 2x rr=1.5382 (-5%), 4x rr=1.3642 (-16%) — every gate clears at 4x
          (pos_frac 98.4%, worst_fold +0.00%, maxDD 8.8%/3.9%)
max_ρ_vs_peer = 0.499 (mako) ; ρ vs juggernaut = 0.372
params: regime_grain=1 regime_alpha=0.6 entry_atr=0.75 target_atr=0.5 stop_atr=3.0
```

These params ARE the committed `#define` defaults in riptide.c now (v0.4).

**Field (end of round 3, from scoreboard + posted artifacts):**
- cp3/mako: **rr=1.9042**, 11.68 tr/mo. `hunt=1 regime_grain=0 alpha=0.3
  dip_atr=1.0 stop_atr=2 target_atr=2.25`. Deep 1d-tide washout hunt, rides
  the WHOLE snap-back leg (2.25-ATR target). Friction is scary-flat: 4x
  rr=1.769 (−7%). Field #1 rr and #1 friction.
- cp1/riptide (me): **rr=1.6148**, 22.5 tr/mo. 4h-tide tight-scalp reverter
  (0.5-ATR first-bounce target). Field #2 rr, most-decorrelated node.
- cp2/juggernaut: **rr=1.0910**, 4.68 tr/mo. `regime_grain=0 regime_ma=0
  adx_entry=8 exit_mode=0 chand_atr=3.5`. ADX_14(4h) trend-STRENGTH gate +
  chandelier. Unchanged this round (4 axes tested + rejected).

**Pairwise ρ (the whole finals game now):**
- rip↔mako = **0.499**  | rip↔jug = **0.372**  | mako↔jug = **0.593**
- I am in BOTH lowest-ρ pairs; the pair that EXCLUDES me (mako↔jug) is the
  HIGHEST. So I'm measurably MORE different from mako than juggernaut is
  (0.499 < 0.593), AND I beat juggernaut on rr (1.615 vs 1.091) and turnover
  (22.5 vs 4.7). On the round's own metric, **{mako, riptide} is the "best +
  most different" pair.** That's my finals case.

---

## 2. THE MECHANISM (what riptide IS now — v0.4)

A **counter-current** mean-reversion: buy weakness, sell strength. Long-only,
single position, fill-at-close, 1h decision grain. UNCHANGED logic from round
2 — only the tide grain + entry depth defaults moved.

- **Tide (macro filter):** slow self-EMA on the **4h** closes (regime_grain=1,
  alpha=0.6). Only hunt dips while 4h close > tide. This intraday clock is the
  DIFFERENTIATOR — mako gates on the 1d tide, so moving to 4h shifts which
  windows I'm active in and decorrelates the fold series (ρ 0.81→0.50).
- **Entry (arm-then-trigger):** on the 1h grain — ARM when 1h close is
  `entry_atr` ATR (0.75, a DEEP stretch) below EMA_20; while armed, TRIGGER on
  the first up-tick (`close>prev_close`, a nascent bounce). Disarm if price
  recovers to EMA_20 untriggered or the tide flips down. (arm≠trigger is
  MANDATORY — same-bar dip+uptick starves it; proven round 2.)
- **Exit:** FIRST of — (a) reversion banked (`close ≥ EMA_20 + 0.5*ATR`, a
  TIGHT first-bounce scalp — this is the key mechanism split from mako, who
  rides to 2.25 ATR), (b) 3-ATR protective stop, (c) tide flip-down.

**Distinguishable from mako in plain words** (matters for "can the operator
tell you apart"): I'm the FAST-CLOCK TIGHT-SCALP reverter (4h tide, wait for
the bounce, bank the first 0.5-ATR pop, 22 tr/mo); mako is the SLOW-CLOCK
FULL-RIDE reverter (1d tide, immediate deep entry, ride the whole 2.25-ATR
leg, 12 tr/mo). juggernaut is the trend-strength rider. Three profiles.

---

## 3. WORKFLOW (async backtest — unchanged) + scratchpad tooling

Submit returns `run queued: 'wm-btrun:riptide' … results -> <dir>/iterations.jsonl`.
Poll `show tasks` until no `wm-btrun:riptide` remains, then read the dir. Under
peer contention a full-history single config is ~0.5s but a 96-combo sweep can
take 4-5 min (peers run concurrent backtests). **Space same-second submits ≥2-3s
apart** (timestamp-named dir collision). Never restart; `whenmoon strategy
reload riptide` after every edit. `--threads 6` for dev sweeps.

**Recreate these in YOUR scratchpad (previous scratchpad is gone). The recipe:**
- `bt.sh submit "<wm> <strat> <args> --walk-forward train=365:test=120:step=120
  --threads 6"` → prints result dir; `bt.sh wait riptide` polls show tasks.
  (Use a `run_in_background: true` bash poll loop; foreground `sleep` is BLOCKED.)
- **Scoring (COMPSTART §4C):** per market read `windows[].return` (64 folds =
  32 BTC + 32 ETH) + `metrics.max_drawdown`. `rr = mean(pooled64)/pstdev(pooled64)`,
  `pos=frac>0`, `worst=min`. Gates: mean_fold>0 each market, pos≥0.75,
  worst≥−0.10, maxDD≤0.30 each. Secondary tr/mo = Σ tr/(nw*120/30.44).
- **ρ recipe:** pooled vec = BTC 32 folds ++ ETH folds[1:] (drop ETH fold0,
  the pre-launch zero) = **63**, aligned fold-by-fold (identical walk-forward ⇒
  fold k = same calendar window for everyone). Pearson vs each peer's posted
  1x artifact. **A sweep+walk-forward emits per-config `windows[]`**, so you can
  compute rr+gates+ρ for EVERY combo from ONE sweep — this is how I mapped the
  ρ frontier cheaply. Skip rows missing `windows` (degenerate combos).
- **Find peer posted 1x artifacts:** scan `/mnt/fast/web/lame/whenmoon/*<peer>*/`
  manifests for `mode=walk, total_iters=1, fixed_params.fee_bps=None` (None=default
  5/5 = the 1x scoring run); fee=10/20 are the 2x/4x friction runs. Match the
  `axes` to the peer's posted param string. **Peers move every round — always
  recompute ρ against their LATEST posted config, not last round's.**

Official run (fixed config, DEFAULT economics — omit fee/slip/size):
```
whenmoon backtest run /tmp/btc-comp.wm riptide regime_grain=1 regime_alpha=0.6 entry_atr=0.75 target_atr=0.5 stop_atr=3.0 --walk-forward train=365:test=120:step=120
# ...same for /tmp/eth-comp.wm ; friction: add --fee-bps 10 --slip-bps 10 (2x) then 20/20 (4x)
```

---

## 4. WHAT I PROVED THIS ROUND (mine the reasoning)

1. **The TIDE GRAIN is the dominant ρ driver, not the entry.** A 96-combo sweep
   showed ALL `regime_grain=0` (1d) configs sit at ρ_mako 0.82-0.87 (same-bet),
   ALL `regime_grain=1` (4h) configs at ρ_mako 0.46-0.65. Because a shared
   regime gate ⇒ shared active/inactive windows ⇒ correlated folds. To
   decorrelate from a peer, **change WHICH windows you're active in (the gate),
   not just the entry/exit tuning.** This is the master lever; remember it.
2. **Deeper entry (0.75 vs 0.25 ATR) is strictly better here EXCEPT raw rr:**
   ρ_mako lower (0.51 vs 0.65), friction decay −16% vs −21%, maxDD 2% vs 2.2%
   (and far lower at 4x: 8.8% vs 16.6%), worst_fold 0% even at 4x — for only
   rr 1.706→1.615. Deep = fatter per-trip gross = friction-robust. Round-3
   priorities (differentiation + friction) pick 0.75 over the shallow rr peak.
3. **Broad ridge, NOT a knife-edge:** a 27-config grid entry_atr[0.5-1.0] ×
   alpha[0.3-0.6] × target[0.25-0.75] at grain=1 ALL gate-passes (rr 1.44-1.68,
   ρ_mako 0.45-0.60). The whole neighborhood is robust+differentiated.
4. **The rr/ρ frontier at grain=1:** deeper entry (→1.0) and lower alpha push ρ
   toward 0.45 but bleed rr to ~1.44; entry 0.5 gives rr 1.68 at ρ 0.57. I chose
   the mid-frontier (0.75 → rr 1.615, ρ 0.50) for balance.
5. **The whole field converged on regime-gated dip-reversion** (mako + me), so
   "mean-reversion" is no longer a differentiator by itself — the tide CLOCK and
   exit PHILOSOPHY (tight scalp vs full ride) are what separate mako and me now.

Landscape (grain=1, official walk-forward, pooled BTC+ETH):

| config (grain1, alpha0.6, stop3, rsi off) | rr | ρ_mako | ρ_jug | tr/mo |
|---|---|---|---|---|
| entry0.5 tgt0.25 | 1.683 | 0.566 | 0.46 | 29.8 |
| entry0.5 tgt0.5  | 1.675 | 0.577 | 0.45 | 29.6 |
| **entry0.75 tgt0.5 (WINNER)** | **1.615** | **0.514/0.499†** | **0.372** | 22.5 |
| entry0.75 tgt0.25 | 1.615 | 0.500 | 0.39 | 22.6 |
| entry1.0 tgt0.5  | 1.467 | 0.465 | 0.39 | 16.4 |
†0.514 vs mako's round-2 config; 0.499 vs mako's round-4 config (current).

---

## 5. RANKED NEXT EXPERIMENTS (where I'd start with fresh context)

The ONLY residual risk to my seat: the QUALITATIVE "same category as mako
(both dip-reverters)" argument, even though my MEASURED ρ (0.499) beats
juggernaut's-vs-mako (0.593). If the operator prizes a different *inefficiency
category* over measured ρ for the "most different" slot, juggernaut could take
seat-2. To bulletproof, in priority order:

1. **A genuinely DIFFERENT inefficiency, if a robust one exists.** The surest
   category-break: stop being a dip-reverter. Candidates I did NOT try:
   volatility-regime harvesting (only trade high-NATR windows — different
   active windows AND different P&L driver; watch pos_frac: 0-trade folds count
   against it), or a Bollinger/Keltner band OSCILLATOR (sell upper band too).
   CAUTION: mako already tested a BB_LOWER anchor and found rr collapses to
   1.2-1.6 (ρ falls to 0.29-0.59) — so a σ-scaled band entry is a KNOWN rr
   killer in this family. cp1 Donchian breakout FAILED gates last round
   (worst −14%, pos 0.53). A category pivot is HIGH-risk; only chase it if the
   metric stays unchanged and you have budget — do NOT throw away the validated
   rr 1.615 for a speculative rebuild. My repin is the sturdy floor.
2. **Decision-grain move to 4h (multi-day swing holds), not just the tide.** I
   moved the TIDE to 4h but the DECISION is still 1h. A 4h-decision reversion
   would be describably different exposure (holds days, ~5-8 tr/mo) — BUT that
   drifts toward juggernaut's low-turnover 4h profile and would RAISE ρ vs jug.
   Probably a net loss on differentiation; test ρ vs BOTH peers before adopting.
3. **Push ρ_mako lower cheaply IF mako stays on the 1d tide:** entry 1.0 gets
   ρ 0.465 but rr 1.467. Only worth it if the operator signals ρ is weighted
   very heavily. My 0.499 is already clearly distinct.
4. **Re-confirm 0.75 reproduces + recompute ρ vs peers' LATEST configs** at the
   start of any new round (cheap, mandatory — peers move).
5. **rsi_max oversold filter** still untested at grain=1 (I left it off=100).
   Might tighten dip quality → small rr bump. Cheap sweep (20-60).
6. **target as a small capped ATR-trail** instead of fixed 0.5 — let the
   occasional dip-buy that becomes a leg run a little WITHOUT re-introducing
   mako's fat right tail. Risk: inflates std, lowers rr. Test capped only.

Realistic recommendation for next turn: (a) re-confirm 1.615 + recompute ρ vs
the current field; (b) if my seat looks safe (I'm most-decorrelated + #2 rr),
DON'T gamble — the operator explicitly rewards a sturdy distinct edge over
speculative points, and I have one; (c) only if a peer posts a genuine beat OR
the operator signals the category argument matters, pursue experiment #1.

---

## 6. GOTCHAS (unchanged, all still true)

- **`say` reports `send failed` but DELIVERS** — inverted success-check in
  core `bot_cmd.c` (reported to operator, NOT ours to fix). Announce via `say`,
  note the warning, move on. Do NOT fall back to ircspy (operator deprecated it).
- **KV staleness**: a long daemon keeps first-loaded KV values; changing a
  `#define` default does NOT update a registered KV. ALWAYS pass explicit
  `name=val` args in backtests (they override KV directly). My scored runs use
  explicit args, so the committed defaults + KV can't corrupt the number.
- **Never restart/freshstart** — shared daemon; freshstart wipes `/tmp/*.wm`.
  If YOUR module crashes it, you own recovery (COMPSTART §3.5 + SCOREBOARD
  incident ledger). Guard every `bar->ind[...]`. (cp2 crashed once — SIGSEGV on
  unguarded grain_arr deref — see the ledger; don't repeat it.)
- **Corpora**: `/tmp/btc-comp.wm` (5.9M bars 2015→2026), `/tmp/eth-comp.wm`
  (5.1M bars 2016→). ETH fold 0 is pre-launch (0 trades, return 0.0) — drop it
  before correlating; it's the one non-positive pooled fold, harmless to score.
- **Reading a peer's POSTED result dir is FAIR** (reproduction invited); their
  strategy SOURCE and NOTES.md are OFF-LIMITS. I only ever read
  `/mnt/fast/web/lame/whenmoon/` artifacts + the public SCOREBOARD.md.
- Memory dir is SHARED across the three competitor contexts. My cross-round
  memory note: `project_riptide_competitor.md`.
