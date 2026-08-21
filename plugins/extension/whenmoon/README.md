# whenmoon — Trading Bot Plugin

```
▄▄▌ ▐ ▄▌ ▄ .▄▄▄▄ . ▐ ▄ • ▌ ▄ ·.              ▐ ▄
██· █▌▐███▪▐█▀▄.▀·•█▌▐█·██ ▐███▪▪     ▪     •█▌▐█
██▪▐█▐▐▌██▀▐█▐▀▀▪▄▐█▐▐▌▐█ ▌▐▌▐█· ▄█▀▄  ▄█▀▄ ▐█▐▐▌
▐█▌██▐█▌██▌▐▀▐█▄▄▌██▐█▌██ ██▌▐█▌▐█▌.▐▌▐█▌.▐▌██▐█▌
 ▀▀▀▀ ▀▪▀▀▀ · ▀▀▀ ▀▀ █▪▀▀  █▪▀▀▀ ▀█▄▀▪ ▀█▄▀▪▀▀ █▪

         Cryptocurrency Trading Bot
     Originally by George Shearer (george@shearer.tech)
```

## What this is

**whenmoon** is a candle-based cryptocurrency spot trading bot
rewritten from the ground up as a first-class **botmanager** plugin.
It runs as a feature of kind `whenmoon` — a `PLUGIN_FEATURE` capability
layer. It is a **plugin-global singleton**: the plugin itself owns the
exchange sessions, the watched markets, their strategy attachments and
the order/position lifecycle, and no bot owns any of it. Control and
telemetry flow through the ordinary command tree, so whichever bots are
on IRC reach the same state.

It is not HFT. The smallest decision granularity is a 1-minute candle.
If you need sub-second execution, look elsewhere (e.g. freqtrade).

## Status

Working. Thirty-three `.c` files build into `libwhenmoon.so`, and every
subsystem this README once carried as *planned* has landed: market
sessions with a multi-grain aggregator and TA-Lib indicators, a
strategy registry with its own plugin ABI, an idempotent candle
downloader, a live order/fill engine with account reconciliation, and a
backtest engine with sweep planning and chart output.

Every market carries a **mode** — `manual`, `paper` or `real` — and a
new one starts in `paper`. `/whenmoon manual` is the operator halt: it
flips every market to `manual` regardless of what each was doing, and a
paper session that draws down past its loss-halt fraction flips itself
the same way.

> **Real-money execution is not in use.** `real` mode exists and is
> deliberately unused: the standing rule on this deployment is paper
> first, and lifting it is an operator decision, not a code change.

## Source layout

A flat layout in the style of `plugins/bot/chat/`: many `.c` files
compiled into one `shared_library('whenmoon', …)`, no nested subdirs
except the strategies, which are plugins in their own right.

| Subsystem | Responsibility |
|-----------|----------------|
| `whenmoon.*` | Plugin descriptor, lifecycle, KV schema |
| `market.*`, `market_engine.*`, `market_persist.*` | Market sessions, the decision loop, DB persistence |
| `aggregator.*` | OHLCV roll-up across grains, bar-close fan-out |
| `indicators.*`, `indicators_custom.*` | TA-Lib indicator computation, plus ones TA-Lib lacks |
| `strategy.*`, `strategy_cmds.*` | Strategy registry, per-attachment parameters, attach/detach verbs |
| `whenmoon_strategy.h` | The public ABI a `PLUGIN_STRATEGY` plugin compiles against |
| `dl_*.c` | Candle downloader: schema, coverage map, job table, supervisor, fetch, verbs |
| `warmup.*`, `warm_chain.*` | Bringing a market to READY without blocking a reload |
| `live.*`, `order_cmds.*`, `account.*` | Live order lifecycle, fill reconciliation, balances |
| `mw.*`, `mw_cmds.*` | Marketwatch — per-exchange polling and its telemetry |
| `ws_binding.*` | The plugin's WebSocket subscription set, rebuilt as markets change |
| `backtest.*`, `sweep.*`, `wm_bt_*.c` | Replay engine, parameter sweeps, reports, charts, metrics |
| `wm_exch_query.*`, `exchange_cmds.*` | Queries and verbs against the exchange abstraction |

Strategies are separate `PLUGIN_STRATEGY` plugins under `strategy/`,
each `dlopen`ed by the registry and talking to whenmoon only through
`whenmoon_strategy.h` — they never include a whenmoon-internal header.

Commands live under `/whenmoon` (abbreviated `wm`) with observability
under `/show whenmoon`: `market`, `download`, `strategy`, `order`,
`backtest`, `mw`, `manual`. They stay in this plugin rather than a
sibling command-surface plugin because they primarily mutate whenmoon
state — the same rule chat follows for `/dossier`, `/memory`, `/llm`.

Exchange REST/WS adapters are **not** part of this plugin. Each
exchange lives as its own `plugins/service/<kind>/` plugin (Coinbase
Advanced Trade, Kraken Spot and Gemini each register a vtable today;
new venues land as additional service plugins). Whenmoon's `.c` files
contain zero direct references to `coinbase_*`, `kraken_*` or
`gemini_*` symbols — every
candle, account, order, fill, and WS subscription routes through the
`plugins/feature/exchange/` abstraction
(`exchange_*_async(name, …)`), which dispatches to the matching
exchange vtable behind a priority queue + token bucket. The
KR-2 lift (2026-05-12) completed this seam; the per-exchange policy
KV namespace `plugin.whenmoon.exchange.<name>.*` configures
whenmoon's consumer-side behaviour (account-poll cadence, rate
limit, live kill-switch).

User-facing commands registered into the unified command tree stay
inside this plugin rather than in a sibling command-surface plugin,
because the commands primarily mutate whenmoon state — the same rule
the text method follows for `/dossier`, `/memory`, `/llm`.

## Layering

| Aspect | Value |
|--------|-------|
| Plugin type | `PLUGIN_FEATURE` |
| Plugin kind | `whenmoon` |
| Provides feature | `feature_whenmoon` |
| Requires | `feature_exchange`, `exchange_coinbase` |
| Home directory | `plugins/extension/whenmoon/` |
| Shared library | `libwhenmoon.so` |

Downward-only dependencies apply (core + service + inference). No
includes or `plugin_dlsym` into other `plugins/feature/*/` or
`plugins/method/*/` plugins; no upward references from
`plugins/service/*/` or `plugins/method/*/` into this directory.

## External dependencies

Three, all system libraries — nothing is vendored:

- `ta-lib` — indicator math. It won the slot the original standalone
  gave `tulipindicators`; `indicators_custom.c` covers what it lacks.
- `json-c` — already a core dep
- `libm` — indicator math

Everything else arrives through another plugin rather than a direct
link: HTTP and TLS through core's curl layer, signing and venue auth
inside each exchange service plugin, persistence through
`plugins/db/postgresql/`.

## Note on `old/whenmoon/`

The original standalone whenmoon bot lives at
`/mnt/fast/doc/projects/botmanager/old/whenmoon/`. The project-wide
`AGENTS.md` says to ignore `old/` entirely; this plugin is the one
explicit exception. `old/whenmoon/` is consulted **only for
architectural reference** — subsystem shape, the list of working vs.
broken pieces, the IRC command vocabulary, the strategy-registry
pattern, the candle/indicator model. No code from it is pasted,
adapted, or textually referenced here. Every file in this directory is
written fresh against current botmanager idioms.
