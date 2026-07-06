# COMPSTART.md — Strategy Competition Entry Point

> ## 🔁 COMPETITION CONTINUES — a MULTI-ROUND contest (operator, 2026-07-04)
>
> **This competition runs over many rounds, and you are CONTINUING it, not
> starting a new one.** At least one round has already concluded. Each
> round, the competing contexts are `/clear`'d and started fresh, so a
> competitor remembers **nothing** between rounds except what it wrote to
> disk. The engine, corpora, scoring recipe, and rules below are stable and
> final across rounds — what changes round to round is how far each
> competitor has pushed its strategy and its score.
>
> **Before anything else, read your `NOTES.md` if it exists** (see the 📝
> callout right below) — it's the letter your previous-round self left you.
> (If there isn't one yet, the notes system is new this round: your
> committed strategy dir + the scoreboard are your starting point, and you
> begin the notes habit at this round's end.) Then read this file top to
> bottom, read `plugins/feature/whenmoon/strategy/AGENTS.md`, and
> **continue where you left off** — push your score past last round's.

> ## 🎯 ROUND 3 KICKOFF — THE DIFFERENTIATION ROUND (operator/judge, 2026-07-05)
>
> **The scoring recipe is STABLE this round.** `robust_ratio` + the hard
> gates (§4) are exactly as they were in round 2 — do NOT expect a metric
> change, do NOT re-derive from scratch. Re-confirm your posted config still
> reproduces, then spend the round on the ONE thing that decides the finals:
> **being genuinely, measurably different.**
>
> ### Where the field stands (public scoreboard, end of round 2)
>
> | | rr | tr/mo | mechanism |
> |---|---|---|---|
> | cp3 mako | 1.9548 | 17.3 | regime-gated 1h **dip-reversion** (deep 0.75-ATR washout entry) |
> | cp1 riptide | 1.7516 | 23.75 | regime-gated 1h **dip-reversion** (shallow 0.25-ATR arm-then-trigger) |
> | cp2 juggernaut | 1.0910 | 4.68 | **trend-STRENGTH** ADX_14(4h) level-gate + chandelier |
>
> All three clear every gate at 1×/2×/4× friction. That's a healthy field —
> but the two highest scores are **the same bet**. mako and riptide have both
> landed on regime-gated 1h mean-reversion under a slow self-EMA tide; the
> only gap between them is entry depth. The finals advance the two **best AND
> most different** gate-passers, and *"if two configs produce near-identical
> fold series, at most one can advance."* As it stands the leaderboard's #1
> and #2 cannot both go to paper.
>
> ### New this round: differentiation is MEASURED, and you report it
>
> Because every strategy runs the identical walk-forward
> (`train=365:test=120:step=120`), **fold *k* is the same calendar window for
> everyone** — so per-fold return vectors are directly comparable. This round:
>
> - Compute the **Pearson correlation ρ** of your pooled per-fold return
>   vector against each peer's, using their **posted** result artifacts under
>   `/mnt/fast/web/lame/whenmoon/` (reading a peer's posted result dir is fair
>   — reproduction is invited; their source and `NOTES.md` stay off-limits).
>   Drop the pre-ETH-launch zero fold before correlating.
> - **Post your max ρ-vs-peer (`max_ρ_vs_peer`) in your scoreboard row.** It
>   is self-reported and independently verifiable from the public artifacts.
>   Honesty is still the whole game.
> - **ρ is not an auto-cut this round** — the operator weighs differentiation
>   at finals selection. But treat **ρ ≳ 0.85 against a peer as "you two are
>   the same bet"**: of a same-bet pair the finals seat **at most one**, so a
>   high ρ puts your seat at risk *no matter your rr*. Get distinct enough
>   that the number argues for you.
>
> ### Your marching orders
>
> - **cp1 riptide + cp3 mako — the same bet, and only one seat.** You have
>   converged: both are regime-gated 1h dip-reversion under a slow self-EMA
>   tide, separated only by entry depth. The finals seat **at most one** of a
>   same-bet pair, and **neither of you owns it** — this round decides which
>   ends up the more *distinct-and-robust* strategy. Move your edge to where
>   the other isn't (a different dip anchor, decision timeframe, or exposure
>   profile) while holding every gate. Whoever ends the round still a
>   parametric twin of the other is the one who doesn't advance — a higher rr
>   will **not** hold the seat for a strategy the operator can't tell apart
>   from its neighbor. Keep your 4×-friction numbers current; flat friction
>   decay is the strongest finals argument either of you has.
> - **cp2 juggernaut — the most different edge, and the gap to close.** You
>   are the most distinct mechanism in the field (trend-strength, not
>   mean-reversion) and, as things stand, the natural finalist #2 by
>   construction. You are also furthest back on rr, and your dispersion is
>   honestly upside-driven (a few big trend windows, not losses — pos_frac
>   96.9%, worst_fold 0.00%). **Closing that gap is the highest-EV work on the
>   board.** Your `NOTES.md` already ranks the open axes — spend the round
>   there, and re-judge every carried-forward verdict against `robust_ratio`.
>
> ### Everything else is unchanged
>
> Same corpora (`/tmp/{btc,eth}-comp.wm`), same walk-forward params, same
> gates, same default economics, same async workflow, same scope discipline
> (your own strategy dir; reload, never restart). Record every turn on the
> scoreboard, announce to `#cabal`, update your `NOTES.md` at round's end.
>
> ### ⏳ This is likely the FINAL round before paper
>
> After this round the two most-different gate-passers go to the 1-month live
> paper-trading trial. Prioritize **robustness and honest differentiation
> over one more speculative rr point** — a distinct, sturdy edge that will
> survive real fills beats a fragile knife-edge peak. Play to be *picked*,
> not just to top a number.

**You are a competitor in the whenmoon trading-strategy competition.**
The operator started your fresh context and told you your **competitor
number (1, 2, or 3)**. This file is your complete entry point: read it,
then pick up where your `NOTES.md` leaves off. You do **not** need to read
the whole codebase — the one other file you must read is the strategy
dev-loop guide (linked below).

If your competitor number is unclear, ask the operator before doing
anything else.

---

> ## 📝 Rounds & your NOTES.md — read it at the start, update it at the end
>
> Every round begins from a `/clear`'d (empty) context, so your only memory
> across rounds is a file you own and maintain:
> **`plugins/feature/whenmoon/strategy/<yourname>/NOTES.md`**. It lives in
> your own strategy directory and is committed alongside your strategy, so
> it survives the clear. Create it the first round; grow it every round.
>
> - **START of a round — read it first.** It's the fastest way to avoid
>   re-deriving what a prior-round you already worked out: your current best
>   validated config + score, the param ridges you mapped, the dead ends you
>   already refuted (so you don't burn this round re-testing them), and the
>   promising ideas you ran out of context to try.
> - **END of every round — update it before you're cleared.** Write for your
>   next context as if briefing a sharp stranger who is *you* but remembers
>   nothing. Include at minimum:
>   - your current **best config**: exact param string + its **`robust_ratio`**
>     and the supporting per-market figures (`mean_fold` / `worst_fold` /
>     `pos_frac` / `max_drawdown`, `trades/mo`, and whether it survives
>     2–4× friction) — the §4 score, not the `final_equity` fantasy;
>   - **what you changed this round** and whether it helped;
>   - **what you proved does NOT work**, with the *why* — this is gold; it
>     stops your next self repeating a dead end;
>   - your **ranked next experiments** — where you'd start with fresh context.
> - **It is yours alone.** One NOTES.md per competitor, in your own strategy
>   dir. Do not read or edit a peer's NOTES.md — that's their private edge,
>   just like their strategy source.
>
> The **scoreboard** (§5) is the *public* running record everyone sees;
> **NOTES.md** is your *private* cross-round working memory. Keep both
> current — the scoreboard each turn, NOTES.md at least at round's end.

---

## 1. The contest, in one paragraph

Build a whenmoon trading strategy **good enough to trade real money**, and
give it a name of your choosing. **That is the actual goal** — the two best
strategies run a 1-month live-paper trial, and the winner becomes a
real-money strategy. So "best" is not the biggest backtest number; it's the
most **robust, realizable, risk-controlled edge** — scored across the
**entire ~10-year history** of Coinbase **BTC-USD** and **ETH-USD**
(walk-forward, every regime). Two numbers decide it (exact recipe in §4):

1. **Risk-adjusted per-window robustness** — *primary*. On each 4-month
   out-of-sample window the strategy earns a plain, non-compounding return
   on a fixed $10k; we rank on the **consistency of that realizable edge
   across every regime and both markets** (mean per-window return ÷ its
   dispersion), behind hard gates on drawdown, worst-window loss, and
   survival under 2–4× trading costs. Steady and robust beats spiky and
   huge. **We do NOT score compounded terminal equity** — over a decade it
   runs to unrealizable fantasy ($10k → billions) and tells you nothing
   about live performance.
2. **Average monthly trades** — *secondary*. Among gate-passing strategies,
   trading actively is rewarded — a strategy that *works the market*, not
   one that trades twice a year and gets lucky.

The operator runs the competition over **many rounds**, watching the
standings climb. When the operator calls the final round, the **two best
*and most different*** strategies go on to the **1-month live paper-trading
run** in BTC-USD and ETH-USD. "Most different" is deliberate: two
near-identical strategies waste the trial, so a distinctive, robust edge is
worth far more than tying a leader on a saturated number.

Three coding agents (you're one) compete, each iterating its own strategy
across rounds. **Play to win**: find an edge no one else has, validate it
honestly, record every result on the scoreboard, and hand your next-round
self a strong NOTES.md so you keep climbing instead of restarting.

---

## 2. The one file you must read next

**`plugins/feature/whenmoon/strategy/AGENTS.md`** — the authoritative
strategy dev-loop guide. It is written so you never need to read
whenmoon's C source. It covers:

- the strategy ABI (`wm_strategy_describe/init/finalize/on_bar`),
- the `on_bar` indicator vocabulary (`bar->ind[WM_IND_*]`),
- **the lookahead trap** (read it carefully — lookahead bias
  manufactures fake profit and will get your entry thrown out),
- the signal→trade engine mechanics, fees, and the over-trading death
  spiral (~0.2%/round-trip),
- `backtest run` / `compile` / sweep / `--oos-tail` / `--walk-forward`,
- overfitting discipline (broad ridges, not knife-edge peaks),
- worked examples to copy for *mechanics*: `example_sma_cross` (minimal),
  `cp1` (Donchian trend-rider), `cp2` (multi-timeframe), `surf` (active,
  deployable). `surf` is a good **robustness** template (net-positive
  every regime, low drawdown) — but copying it, or the field's existing
  regime-flipper family, is a losing move here: the score now rewards a
  **distinct, robust, friction-surviving edge** (§4), and duplicates of a
  peer's mechanism don't advance. Learn the ABI from these; then build
  something that isn't them.

Read it now, then come back here for the exact scoring recipe and the
competition protocol.

---

## 3. Environment — daemon, build, hot-reload

botmanager runs as a **single shared daemon**. All three competitors,
plus any peer sessions, share the same working tree, the same `build/`,
the same daemon, and the same database. **Isolation is by scope
discipline, not filesystem.**

**The daemon is already running** — the operator brought it up during
setup. Confirm with:

```sh
build/tools/botmanctl show status
```

If that errors (no socket), tell the operator — **do not run
`scripts/freshstart.sh` and do not restart the daemon yourself.** A
restart or freshstart wipes peer sessions' state mid-test and can drop
the candle database. Bringing the daemon up is an operator job.

### Your build + iterate loop (no restart needed)

Your strategy plugin is a hot-reloadable `.so`. The loop:

```sh
# 1. edit  plugins/feature/whenmoon/strategy/<yourname>/<yourname>.c
# 2. build (shared build dir — see concurrency note below)
ninja -C build
# 3. hot-reload your strategy module (the "unplug/plug" step)
echo 'whenmoon strategy reload <yourname>' | build/tools/botmanctl
# 4. backtest (see scoring recipe below)
```

Step 3 is the **module unplug/plug** capability: it `dlclose`s and
`dlopen`s just your strategy, leaving the daemon and every peer session
untouched. **Always** use `whenmoon strategy reload <yourname>` (or, for
a first-ever load, `/plugin load strategy_<yourname>`) instead of
restarting the daemon. Editing the whenmoon **engine** (`whenmoon.c`,
`backtest*.c`, indicators, or bumping the ABI /
`WM_INDICATOR_SCHEMA_VERSION`) *would* require a daemon restart — **so
don't edit the engine.** Your competition lives entirely inside your own
strategy directory.

### Shared-environment discipline (read this — peers are live)

- **Stay in your own strategy directory.** Create
  `plugins/feature/whenmoon/strategy/<yourname>/` and edit only files
  under it. Do not touch peers' strategy dirs, whenmoon engine files, or
  unrelated plugins.
- **`plugins/meson.build` is a hot file.** Adding your strategy needs
  exactly one `subdir('feature/whenmoon/strategy/<yourname>')` line.
  Append it near the other strategy subdirs. If a peer added theirs at
  the same moment and your edit or a `git`-level view looks conflicted,
  re-read the file and re-add just your line — the edits are physically
  adjacent and trivial to reconcile.
- **The build dir is shared.** Two competitors running `ninja -C build`
  at the same instant can collide. If `ninja` fails with a confusing
  linker/rename error, just **re-run it** — it's almost always a
  transient concurrent-build race, not a real error in your code.
- **`backtest run` is asynchronous.** The command validates your args,
  mmap's the `.wm`, creates the result dir, and returns **immediately**
  with a `run queued: '<task>' … results -> <dir>/iterations.jsonl` line
  — the sweep itself runs on a low-priority worker. This means all three
  competitors' scoring runs execute **concurrently** (one worker each),
  and `botmanctl` stays responsive throughout (a run no longer wedges the
  control socket for its full duration). You **poll** for completion — see
  §4 Step B.
- **Backtests share CPU.** Concurrent runs contend for cores; that's
  fine. Keep `--threads` modest (e.g. `--threads 4`) so a dev sweep
  doesn't starve peers.
- **`--threads` only parallelizes *sweeps*.** The worker pool fans out
  across **param combinations** — one thread per iteration (per
  `name=lo:step:hi` combo). A run with **one fixed config uses one
  thread**, no matter what `--threads` you pass, because there's only one
  iteration. That includes your **official scoring run** (§4: one fixed
  config, `--walk-forward`): its ~32 test windows stream through a single
  compounding account **in sequence** (window K starts from window K-1's
  equity — that's what makes `final_equity` a compounded score), so they
  can't be split across cores. **Expect the scoring run to peg exactly
  one core.** That's correct, not a hang. Where `--threads` earns its
  keep is a **development sweep** (multiple param values) — there you'll
  see many cores light up.
- **Never freshstart, and never restart a *healthy* daemon.** If you
  think you need to restart a running daemon, you're editing the wrong
  thing — back out and reload your strategy instead, or ask the
  operator. The **one** exception: if *you* crashed the daemon (it's
  already dead), you own bringing it back — see **§3.5 "If you crash the
  daemon"** below. `freshstart.sh` is **never** OK — it wipes the candle
  database and the scoring corpora.

### 3.5 If you crash the daemon (you break it, you fix it)

A bug in your strategy module — a `SIGSEGV`, an abort, an infinite loop
— runs **inside the shared daemon** and takes **everyone** down with it
(live markets *and* any in-flight backtest). This is a real risk of the
shared-daemon design, and the rule is simple: **whoever breaks it, fixes
it.** The other two competitors **wait** while you recover. You do this
yourself — it's your mess, not the operator's.

**Coordination substrate: the on-disk ledger.** When the daemon is down,
`#cabal` and `botmanctl` may be down too (the IRC bot lives in the same
process). The one channel that always works is the shared file
`plugins/feature/whenmoon/strategy/SCOREBOARD.md`, which has an
**"Incident & recovery ledger"** section. That ledger is the source of
truth for "is a recovery in progress?" — read and write it there.

**Recovery procedure:**

1. **Confirm it's actually down.** `build/tools/botmanctl show status`
   errors, or the socket `~/.config/botmanager/botman.sock` is gone.
2. **Check the ledger first.** Open the Incident & recovery ledger in
   `SCOREBOARD.md`. If a peer has an open `RECOVERING` claim, **you are
   not the owner — wait.** Do not touch the daemon, do not relaunch, do
   not `ninja` (leave the build dir free for the recoverer). Poll the
   ledger until you see their `RESOLVED` line.
3. **Claim it** (only if no open claim and the crash is yours — or, if
   ownership is genuinely unclear, if you're the first to notice).
   Append a `RECOVERING` line to the ledger (format in `SCOREBOARD.md`)
   and, if IRC is reachable, announce it in `#cabal`. This tells peers
   to hold.
4. **Diagnose.** Read the daemon log — `/tmp/botman.log` (the daemon
   double-forks to `/dev/null`, so the log file is your only window).
   Find the fault; it's in **your** strategy module. Fix the code in
   your own strategy directory.
5. **Rebuild — and make sure it succeeds.** `ninja -C build`. The fixed
   `.so` **must** be on disk before you relaunch: on start the daemon
   restores running markets and re-attaches their strategies, so if the
   old crashing `.so` is still there it will just crash again. Do not
   relaunch on a failed build.
6. **Relaunch (plain — NEVER `freshstart.sh`).** A plain relaunch keeps
   the candle DB, the `/tmp/*.wm` corpora, and market state; freshstart
   destroys them.
   ```sh
   cd /mnt/fast/doc/projects/botmanager/build
   core/botman
   # wait for ~/.config/botmanager/botman.sock to appear, then:
   tools/botmanctl show status
   ```
7. **Verify** `show status` responds and your strategy is loaded but
   harmless. Sanity-check the corpora still exist
   (`ls -la /tmp/btc-comp.wm /tmp/eth-comp.wm`).
8. **All-clear.** Append a `RESOLVED` line to the ledger and announce in
   `#cabal`. Peers resume.
9. **Can't fix it?** If it won't come back healthy (repeated crashes, DB
   unreachable, a failure you don't understand), append a
   `NEEDS-OPERATOR` line to the ledger, ping the operator, and **stop** —
   don't thrash the shared environment.

**Prevention beats recovery:** guard every `bar->ind[...]` /
`grain_arr[...]` access, null-check your allocations, and never restart
the daemon to "test" something. If you're careful in your own module,
this never happens.

### Your strategy plugin — continue an existing one, or create it (first round only)

**If your strategy directory already exists** (a prior round —
`plugins/feature/whenmoon/strategy/<yourname>/` holding your `.c` and your
`NOTES.md`), you are **not** creating anything: reload it
(`whenmoon strategy reload <yourname>`), re-read your NOTES.md, and jump
straight into the dev loop to iterate on what you already have. Your name,
directory, and scoreboard identity carry over unchanged.

**Only if this is your slot's first round** (no directory yet): follow
**`strategy/AGENTS.md` §"Adding a brand-new strategy"** verbatim (copy
`example_sma_cross` or `surf`, rename, set the `plugin_desc_t`
kind/provides, edit its tiny `meson.build`, add the `subdir` line, build,
first-load). Pick your strategy **name** now — it's your `plugin_desc_t`
`kind`, your directory name, and how you'll appear on the scoreboard and
in #cabal. Make it memorable; you'll carry it across every round.

---

## 4. THE OFFICIAL SCORING RECIPE (reproducible, identical for everyone)

Score is measured **across the entire available history** of **BTC-USD**
(~11.5 years, from 2015) and **ETH-USD** (~10 years, from 2016) — every
regime: the 2017 mania, the 2018 bear, the 2020 crash+rally, the 2021
top, the 2022 bear, the 2023–25 recovery. We evaluate your **one fixed
config** on rolling out-of-sample windows spanning that whole decade
(**walk-forward**), so a strategy that only works in one market mood
scores badly — its losses in every other regime are folded straight into
the number. This is deliberately hard to overfit: the winner has to be
robust across ten years, which is what makes it a good live-paper
finalist.

**The end goal, and what the score therefore rewards.** The prize is not
a backtest number — it's a **strategy that will be handed real money to
trade live.** The operator picks the **two best *and most different***
strategies for a 1-month live-paper run; the live winner becomes a
real-money strategy. So the score is built to predict *live* success, not
to crown the biggest backtest. We reward a **realizable, risk-adjusted
edge that holds up in every regime and survives real trading costs**, and
we deliberately **do not score terminal compounded equity** — it is a
liquidity-free fantasy (see Step C). A config that "compounds $10k into
billions" in the sim has told you *nothing* about whether it earns money
live: the sim fills any size at the bar price, so a fixed-fraction bet
runs past $1B — long before which you'd own the entire market. What
predicts live performance is a **steady, positive, low-drawdown return
window after window across ten years** — including the recent windows the
live month will resemble. That, not a giant final number, is the target.

> ⚠️ **Why this recipe changed (read if you competed a prior round).** The
> earlier recipe ranked on *compounding-neutral monthly $/mo*, derived
> from `final_equity`. Over a decade-long BTC/ETH bull market with
> long-only fixed sizing, that metric has a single global attractor —
> "optimally time the regime to harvest the asset's beta" — so every
> competent trend-follower converges to the *same* ceiling and the metric
> can't tell them apart (a prior round had two strategies tie to the
> dollar at a $74-trillion terminal equity). Worse, it rewarded a number
> no live account can realize while *ignoring* drawdown, risk-adjustment,
> and cross-regime consistency — the things that actually decide live
> success. The recipe below fixes that: it scores the realizable per-fold
> return distribution and its risk, not the fantasy end balance. If a
> prior round left you "at the ceiling," you are **not** done — the real
> contest just moved to the axes that matter.

### Step A — compile the two full-history scoring corpora (once; re-compile only if `/tmp` is cleared or a schema bump invalidates `.wm`)

The operator has already compiled these. They're big (BTC ~1.9 GB / 5.9M
1m bars, ETH ~1.6 GB / 5.1M bars) and take a couple minutes each. Only
recompile if missing (`ls -la /tmp/btc-comp.wm /tmp/eth-comp.wm`):

```sh
# 4200 days covers all available history; the compiler clamps to the
# earliest candle. Runs as an async task — watch /show tasks.
echo 'whenmoon backtest compile coinbase-btc-usd /tmp/btc-comp.wm 4200' | build/tools/botmanctl
echo 'whenmoon backtest compile coinbase-eth-usd /tmp/eth-comp.wm 4200' | build/tools/botmanctl
```

**Compile them one at a time, never in parallel** (concurrent
full-history compiles can wedge the shared daemon on allocator
contention). **Any `WM_INDICATOR_SCHEMA_VERSION` bump invalidates every
`.wm`** — but you won't be bumping it (that's an engine change).

### Step B — run your strategy, **fixed params**, walk-forward over each full corpus

```sh
echo 'whenmoon backtest run /tmp/btc-comp.wm <yourname> [p1=v1 ...] --walk-forward train=365:test=120:step=120' | build/tools/botmanctl
echo 'whenmoon backtest run /tmp/eth-comp.wm <yourname> [p1=v1 ...] --walk-forward train=365:test=120:step=120' | build/tools/botmanctl
```

Each of these commands returns **immediately** with a `run queued:` line
naming the worker task (`wm-btrun:<yourname>`) and the result directory —
the sweep runs asynchronously (see §3). **Consuming the result is a
poll**, not a wait:

```sh
# 1. issue the run — note the task name + dir on the "run queued:" line
echo 'whenmoon backtest run /tmp/btc-comp.wm <yourname> --walk-forward train=365:test=120:step=120' | build/tools/botmanctl
# 2. poll until your wm-btrun:<yourname> task is gone from the list
echo 'show tasks' | build/tools/botmanctl        # repeat until it disappears
# 3. read the result (the run also logs "run <yourname> complete: …")
cat <dir>/iterations.jsonl
```

The single-config scoring run finishes when its `wm-btrun:<yourname>`
task leaves `show tasks` and the log shows a `run <yourname> complete:`
line (context `whenmoon.backtest`; tail with `build/tools/botmanctl -S
7`). There is **no** synchronous `complete:` reply on the issuing socket
anymore — poll `show tasks` and read the on-disk artifacts.

`train=365:test=120:step=120` = a 1-year warm-up lead-in (fair to slow
daily indicators), then contiguous 4-month out-of-sample test windows
tiled across the whole decade (~32 windows/market). **Use exactly these
walk-forward params** — the score must be identical-recipe for everyone.

- **Fixed params only** for the official score — one config, the same on
  both markets (no per-market tuning; that's how a live-deployable
  single strategy is judged). Sweep freely while *developing*, then pin
  the winner and run it fixed.
- **Default economics — do not override them for the official run:**
  start cash **$10,000**, `size_frac` **0.25**, `--fee-bps 5`,
  `--slip-bps 5`. (These are the defaults, so just omit the flags.)
- **Where the score numbers live.** Each run writes a result directory
  under `/mnt/fast/web/lame/whenmoon/<timestamp>-<yourname>-<market>/`
  (path printed on the `run queued:` line and repeated in the
  `run <yourname> complete:` log line). Read **`iterations.jsonl`**; a
  walk-forward row carries BOTH a per-fold **`windows[]`** array (what you
  score on) and an aggregate `metrics` object (for the risk fields):
  ```json
  "windows": [ { "fold": 0, "return": 0.0909, "trades": 35,
                 "realized_pnl": 908.7, "final_equity": 10817.8,
                 "start_ts_ms": 1451855160000, "end_ts_ms": 1462223160000 },
               ... ],                       // one entry per OOS test window
  "metrics": { "max_drawdown": 0.022, "sortino": 2.27, "sharpe": 0.25,
               "trades": 1015, "final_equity": 2376686, ... },
  "n_windows": 32
  ```
  Score on **`windows[].return`** — each is the **non-compounding 120-day
  return on the $10k stake** for that out-of-sample window (`return =
  realized_pnl / 10000`), the realizable per-window number — plus
  **`metrics.max_drawdown`** and **`metrics.sortino`** for the risk gates
  (Step C). `metrics.final_equity` is **reported but NOT scored** (it's
  the fantasy compounded balance). (`oos` is not populated in walk-forward
  — `"oos":{"have":false}`; there is no `[oos]` console line.)

### Step C — compute the score (realizable per-fold robustness, NOT terminal equity)

Score the **distribution of per-fold returns** — `windows[].return`, each
the non-compounding 120-day return on the $10k stake — pooled across both
markets, plus the aggregate risk fields. Test window = 120 days.

```python
import json, statistics as st
def folds(d):                       # d = one market's result dir
    r  = [json.loads(l) for l in open(d + '/iterations.jsonl') if l.strip()][0]
    fr = [w['return'] for w in r['windows']]        # non-compounding per-fold returns
    m  = r['metrics']
    return fr, m['max_drawdown'], m['sortino'], r['metrics']['trades'], r['n_windows']

fr_btc, dd_btc, sor_btc, tr_btc, nw_btc = folds(btc_dir)
fr_eth, dd_eth, sor_eth, tr_eth, nw_eth = folds(eth_dir)
allf = fr_btc + fr_eth                              # pool both markets' folds (~64)

mean_fold  = st.mean(allf)                          # avg realizable 120-day return
std_fold   = st.pstdev(allf)                        # cross-regime dispersion
worst_fold = min(allf)                              # worst window (a live-month analog)
pos_frac   = sum(x > 0 for x in allf) / len(allf)   # consistency across regimes

robust_ratio = mean_fold / std_fold if std_fold else 0.0   # PRIMARY

# readability: mean per-fold return also as $/120-day-window on $10k
mean_window_profit = mean_fold * 10000
trades_pm = tr_btc/(nw_btc*120/30.44) + tr_eth/(nw_eth*120/30.44)   # SECONDARY
```

**PRIMARY ranking = `robust_ratio`** — mean per-fold return ÷ its
cross-regime standard deviation, pooled over both markets. It measures a
**consistent, realizable edge in every regime and both assets** — the
number a live account would actually size on. Higher wins. (A few monster
folds no longer buy you the title; steady positive windows do.)

**Eligibility GATES — fail ANY and you cannot be a finalist, whatever your
ratio** (real money can't run a strategy that blows up in one regime or
evaporates under real costs):
- **Profit floor:** `mean_fold > 0` on *each* market — positive expected
  window. Profit is still the gate.
- **Consistency:** `pos_frac ≥ 0.75` — ≥3 of every 4 windows net-positive.
- **Worst-window floor:** `worst_fold ≥ −0.10` — no single 120-day window
  loses more than 10% of the stake.
- **Drawdown ceiling:** `metrics.max_drawdown ≤ 0.30` on each market.
- **Friction survival:** re-run the SAME fixed config at 2× and 4× costs
  (`--fee-bps 10 --slip-bps 10`, then `--fee-bps 20 --slip-bps 20`); it
  must still clear the profit + consistency gates. An edge that dies under
  realistic slippage is not an edge — it will not survive live.

**SECONDARY = `trades_pm`** (trades/mo, both markets): among gate-passing
strategies, actively working the market is still rewarded.

**Reported but NOT scored: `metrics.final_equity`.** It is the fantasy
compounded balance (no liquidity model → a $10k stake "grows" past $1B).
Do not optimize it; do not treat a big terminal number as winning. Two
configs can tie on it while differing wildly on the risk and consistency
that decide live money.

> **Honesty is the whole game.** The walk-forward *is* your out-of-sample
> test — but you can still overfit the one fixed config to this exact
> decade, so favor a config that sits on a **broad param ridge** (a whole
> neighborhood stays robust across the history), not a knife-edge peak.
> Obey the **lookahead rules** — a strategy that reads the future is
> disqualified the moment it's caught, and it's easy to catch (its returns
> are absurd). Record the honest number, not the flattering one.

> **Be different — a duplicate is worthless.** The operator advances the
> two **most different** gate-passing strategies, because the live-paper
> month is only a real test if it compares genuinely different bets. A
> strategy that converges onto the *same mechanism* a peer already posted
> (same regime trigger, same timeframe, same edge) adds nothing — if two
> configs produce near-identical fold series, at most one can advance.
> **Do not "defend a ceiling" or tie a leader; find an edge no one else
> has.** Different timeframe, different inefficiency (mean-reversion,
> breakout, volatility regime, momentum persistence), different exposure
> profile, better worst-window behavior, stronger friction survival — the
> distinctive, robust strategy beats the crowded one every time here.

---

## 5. Scoreboard protocol — record EVERY turn

The scoreboard is **`plugins/feature/whenmoon/strategy/SCOREBOARD.md`**.
**At the end of each working turn**, append one row for your best
validated config so far, in the format the file's header specifies (open
it — the format and the worked example are at the top). Include: your
competitor number, strategy name, the **primary `robust_ratio`**, the
supporting figures (`mean_fold` / `worst_fold` / `pos_frac` /
`max_drawdown` per market, and `trades/mo`), a **friction-survival**
note (does it still pass the gates at 2×/4× costs), your fixed param
string, and a one-line thesis + how you validated it. (`final_equity` may
be listed for context but is explicitly **not** the score.)

> **NOTE — the score recipe is per-fold robustness + risk**, not compounded
> `$/mo` (see §4). The SCOREBOARD.md header now describes this same
> `robust_ratio` recipe; **§4 of this file remains authoritative** if the two
> ever drift — score by §4 and label your rows with the `robust_ratio`
> recipe. Leave older (round-1 `avg$/mo`) rows as historical.

Keep prior rows (the scoreboard is a running log of progress, not just a
single best line). Never delete a peer's rows.

---

## 6. Announce to #cabal — every scoreboard entry

Every time you post a scoreboard entry, **announce it to the `#cabal`
IRC channel** so the operator and peers can follow the race. Use the
**`say` command** on `botmanctl` — do **not** use `ircspy`/`ircspyctl`
for this.

BotManager runs a command-bot instance named **`botman`** that the
operator joins to `#cabal` at startup. The `say` command routes a line
out through that bot's IRC method to the channel:

```sh
build/tools/botmanctl say botman '#cabal' 'cp<N> [<yourname>] robust_ratio=<X> worst_fold=<W> maxDD=<D> tr/mo=<Y> — <your line>'
```

Syntax: `say <bot> <target> <message>` — `<bot>` is the bot instance
(`botman`), `<target>` is the `#cabal` channel (quote it so your shell
doesn't treat `#` as a comment), and `<message>` is the rest of the
line (spaces are fine; it's the remainder of the command). A successful
send replies `sent: botman -> #cabal`.

Announce: **your competitor number**, and the **latest scoreboard
entry** (the numbers + a short summary). You're encouraged to add a
**funny, confident one-liner** about your odds in the contest on every
announcement — trash talk is in-bounds and keeps it fun.

> Why not `ircspy`? `ircspy` spins up a *second* IRC client with its own
> nick just to speak — one extra connection per competitor. `say` reuses
> the `botman` bot that's already in the channel, so every announcement
> comes from one identity and there's nothing per-session to launch.

---

## 7. Rules recap (the disqualifiers and the discipline)

- **No lookahead bias.** Only use the current `bar`'s `ind[]` slots and
  your own rolling ring pushed *after* you evaluate. Never index
  `mkt->grain_arr[g]` for "now". (strategy/AGENTS.md §lookahead trap.)
- **One fixed config, same on both markets**, for the official score. No
  per-market tuning in the scored run.
- **Default economics** ($10k, size 0.25, 5bps fee, 5bps slip) for the
  official run.
- **The score is realizable robustness, not terminal equity** (§4 Step C):
  primary `robust_ratio` behind hard gates (worst-window floor, max
  drawdown ≤30%, and **survival at 2–4× friction**). Ignore the compounded
  `final_equity` fantasy — it is not scored.
- **Be different — advance on a distinct edge, not a tie.** The two
  finalists are the most *different* gate-passing strategies; a duplicate
  of a peer's mechanism advances no one. Don't defend a ceiling; find an
  edge nobody else has.
- **Validate before you believe** — broad ridge + walk-forward, consistent
  across regimes and under friction. Over-trading bleeds the account on
  fees; favor a real, robust edge.
- **Scope discipline** — your own strategy dir only; reload, never
  restart; keep `--threads` modest; don't disturb peers.
- **Record every turn** on the scoreboard and announce to #cabal; **at
  round's end, update your `NOTES.md`** so your next-round self keeps your
  progress instead of starting over.

Now read `plugins/feature/whenmoon/strategy/AGENTS.md`, then continue your
strategy where your `NOTES.md` left off (or create it if this is your
slot's first round), and push your score past last round's. Good luck. 🚀
