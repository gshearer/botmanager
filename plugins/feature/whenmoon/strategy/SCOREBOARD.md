# Strategy Competition Scoreboard

**Competition — full-history (~10yr) BTC-USD + ETH-USD robustness contest.**
Entry point / full rules: **`COMPSTART.md`** at the repo root.

Markets: **coinbase BTC-USD** (~11.5yr) and **coinbase ETH-USD** (~10yr).
Scoring: **walk-forward over the entire history** (`--walk-forward
train=365:test=120:step=120`) on full-history corpora `/tmp/btc-comp.wm`
+ `/tmp/eth-comp.wm` — every regime, hard to overfit.
Fixed economics for every scored run: start cash **$10,000/market**,
`size_frac 0.25`, `--fee-bps 5`, `--slip-bps 5`.

---

## Score definition (the two numbers that decide the contest)

From the top-level **`metrics`** object of each market's
`iterations.jsonl` after a **fixed-param walk-forward** run on each full
corpus (see `COMPSTART.md §4` for the exact commands). Read
`metrics.final_equity`, `metrics.trades`, and `n_windows` per market
(each market starts from $10k; walk-forward's `oos` object is *not*
populated — read `metrics`). Dollar-profit is meaningless over a decade
(compounding → millions), so the score is a **compounding-neutral
monthly rate**, shown as $/month on a $10k stake:

```
months_m    = n_windows_m * 120 / 30.44                       # per market
mo_return_m = (final_equity_m / 10000) ** (1/months_m) - 1    # geometric monthly return
mo_profit_m = mo_return_m * 10000                             # $/mo on a fixed $10k stake

avg_month_profit = (mo_profit_btc + mo_profit_eth) / 2        # PRIMARY  ($/mo per $10k)
avg_month_trades = trades_btc/months_btc + trades_eth/months_eth  # SECONDARY (trades/mo)
```

- **PRIMARY = `avg_month_profit`** (higher wins; a strategy ending below
  $10k has negative monthly return — profit is the gate).
- **SECONDARY = `avg_month_trades`** (higher is better; rewards a
  strategy that actively works the market to earn its profit).

Reproduce any row: on the full-history corpora, run the strategy
**fixed-param** with `--walk-forward train=365:test=120:step=120` and
default economics, read `final_equity`/`trades`/`n_windows` from each
market's `iterations.jsonl`.

---

## Entry format

One row per posted result, most-recent last. **At the end of every
turn**, append your best validated config so far. Keep prior rows (this
is a running log). Never edit or delete a peer's rows. Announce each new
row to **#cabal** (see `COMPSTART.md §6`).

Per-market cells are the walk-forward aggregates:
`eq` = `final_equity` (decade-compounded from $10k), `tr` = `trades`,
`w` = `n_windows`.

```
TIMESTAMP(UTC) | cp<N> | <strategy-name> | avg$/mo=<X> | trades/mo=<Y> | BTC eq=$.. tr=.. w=.. | ETH eq=$.. tr=.. w=.. | params: <fixed param string> | <one-line thesis + how validated (broad ridge across regimes)>
```

Example (illustrative — not a real result):
```
2026-07-04 12:00:00 UTC | cp1 | myrocket | avg$/mo=516 | trades/mo=15.9 | BTC eq=$2376686 tr=1015 w=32 | ETH eq=$13666665 tr=987 w=32 | params: entry_mode=1 exit_mode=0 chand_atr=6 regime_grain=1 regime_ma=0 | regime-gated 1h momentum swing; net-positive across every regime, broad param ridge
```

---

## Scores

_(No entries yet — competition just started. Competitors: add your rows below.)_

---

## Incident & recovery ledger

The daemon is **shared** — a `SIGSEGV`/abort/hang in one competitor's
strategy module takes the whole daemon down, and with it every peer's
work and (usually) `#cabal` and `botmanctl` too. This file is on disk,
so it is the **one coordination channel that survives a daemon crash**.
Use it as the source of truth for "is a recovery in progress?".

**The rule: whoever breaks it, fixes it.** The other competitors
**wait**. Full recovery procedure is in **`COMPSTART.md §3.5 "If you
crash the daemon"`** — the short version:

1. Daemon down? Read this ledger **first**. If there's an open
   `RECOVERING` claim, you are not the owner — **hold**: don't touch the
   daemon, don't relaunch, don't `ninja`. Poll until the `RESOLVED` line
   appears.
2. If the crash is yours (or ownership is unclear and you're first to
   notice) and no claim is open: append a `RECOVERING` line, then fix
   your module → `ninja -C build` (must succeed) → **plain** relaunch
   (`cd build && core/botman`; **never** `freshstart.sh` — it wipes the
   candle DB + `/tmp/*.wm` corpora) → verify `show status` → append a
   `RESOLVED` line. Announce both to `#cabal` if it's reachable.
3. Stuck (repeat crashes, DB unreachable, unclear cause)? Append a
   `NEEDS-OPERATOR` line, ping the operator, and stop.

**Ledger line format** (append, newest last; never delete a line — this
is the incident history):

```
TIMESTAMP(UTC) | cp<N> | RECOVERING|RESOLVED|NEEDS-OPERATOR | <what broke / what you did / current state>
```

Example (illustrative — not a real incident):
```
2026-07-04 16:30:00 UTC | cp2 | RECOVERING     | SIGSEGV in cp2 on_bar (unguarded grain_arr deref); daemon down, fixing + relaunching. Peers hold.
2026-07-04 16:41:00 UTC | cp2 | RESOLVED       | fix built, daemon relaunched, show status OK, /tmp/*.wm corpora intact. Resume.
```

### Incidents

_(None yet.)_
