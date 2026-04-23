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
It runs as a bot behaviour of kind `whenmoon` — the third `PLUGIN_BOT`
kind after `command` and `chat`. A bot instance bound to it owns
exchange sessions, watched markets, per-market strategy state, and the
order/position lifecycle; control and telemetry flow through the same
method/command layer every other botmanager bot uses.

It is not HFT. The smallest decision granularity is a 1-minute candle.
If you need sub-second execution, look elsewhere (e.g. freqtrade).

## Status

**Scaffolding only.** This directory currently contains:

- A loadable `PLUGIN_BOT` descriptor with a no-op `bot_driver_t` vtable
- Lifecycle hooks (`init`, `deinit`) that emit a single `clam` line each
- No KV knobs, no commands, no exchange code, no market logic

Building the tree produces `libwhenmoon.so` and the plugin loads clean,
but binding a bot to it does nothing observable. All trading behaviour
lands in later chunks.

> **Never use with real money in any form.** Not now. Not at 0.1. Not
> without explicit paper-trade gating, risk caps, and KV-guarded live
> toggles that have not been written yet.

## Architectural Vision

The plan, executed over future chunks, is a flat source layout
mirroring `plugins/bot/chat/`'s style: many `.c` files compiled into a
single `shared_library('whenmoon', …)` rather than nested subdirs.
Expected subsystems, each growing as one or a few source files:

| Subsystem | Responsibility |
|-----------|----------------|
| `whenmoon.c` + `whenmoon.h` | Bot driver vtable, plugin descriptor, lifecycle |
| *(planned)* `market.*` | Candle ingestion, indicator computation, market state |
| *(planned)* `candle.*` | OHLCV roll-up, interval aggregation, history buffer |
| *(planned)* `strategy.*` | Pluggable strategy dispatch (score, macd, sma, hodl, …) |
| *(planned)* `order.*` | Order state machine, paper/live gating, fill tracking |
| *(planned)* `account.*` | Exchange balance reconciliation |
| *(planned)* `commands.*` | `cmd_register` surface for `/market`, `/position`, etc. |
| *(planned)* `persist.*` | DB schema for positions, fills, candle archive |
| *(planned)* `backtest.*` | Replay harness (may instead live as a separate tool) |

Exchange REST/WS adapters are **not** part of this plugin. Each
exchange (Coinbase Advanced Trade first, Kraken + Gemini later) will
land as its own `plugins/service/exchange-<kind>/` plugin exposing a
`<kind>_api.h` dlsym-shim header, consumed here via `.requires`.

User-facing commands registered into the unified command tree stay
inside this plugin rather than a sibling `plugins/cmd/whenmoon/`,
because the commands primarily mutate bot-kind state — the same rule
the chat plugin follows for `/dossier`, `/memory`, `/llm`.

## Layering

| Aspect | Value |
|--------|-------|
| Plugin type | `PLUGIN_BOT` |
| Plugin kind | `whenmoon` |
| Provides feature | `bot_whenmoon` |
| Requires | *(none yet — grows with subsystems)* |
| Home directory | `plugins/bot/whenmoon/` |
| Shared library | `libwhenmoon.so` |

Downward-only dependencies apply (core + service + inference). No
includes or `plugin_dlsym` into other `plugins/bot/*` behaviours; no
upward references from `plugins/service/*/` or `plugins/method/*/`
into this directory.

## External Dependencies (planned, not yet wired)

None today. As subsystems land, expect to depend on:

- `libcurl` — exchange REST calls (already a core dep)
- `json-c` — JSON parsing (already a core dep)
- `libcrypto` / `libssl` — HMAC signing, TLS (already available)
- `libjwt` — Coinbase Advanced Trade ES256 JWT auth (**new**)
- `libm` — indicator math (already standard)
- `libpq` — via `plugins/db/postgresql/` for persistent state
- *(possibly vendored)* `tulipindicators` — 150+ technical indicators,
  carried over from the original standalone if no equivalent is
  already present in the tree
- *(possibly vendored)* `tinyexpr` — already used by `plugins/cmd/math`

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
