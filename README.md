# BotManager

> A microkernel for bots — minimal, asynchronous, KV-driven, written in
> modern C for Linux.

BotManager is not a bot. It is the substrate on which bots are built.
Core provides infrastructure — task scheduling, threading, database
access, an event-priority HTTP/WebSocket layer, a publish-subscribe
message bus, a hierarchical configuration store, and a unified user
namespace — and lets plugins decide everything that *is* a bot:
how it talks to humans, what it remembers, and what it does between
heartbeats.

```
                ┌──────────────────────────────────────┐
                │              core/                   │
                │   tasks · threads · CLAM · KV · DB   │
                │   curl · sockets · userns · botmanctl│
                └─────────────────┬────────────────────┘
                                  │ stable plugin ABI
        ┌─────────────────────────┼─────────────────────────────┐
        │                         │                             │
   protocol/                  method/                       feature/
   IRC, botmanctl       command, chat                whenmoon, exchange
        │                         │                             │
        └─────────── service/ ────┴──── inference/ ──── strategy/ ──┘
                 coinbase, searxng,         llm + knowledge      e.g. testing
                 openweather, ...           + acquire
```

---

## Design pillars

**Microkernel discipline.** A subsystem belongs in core only if
multiple bot kinds consume it. The architectural test is concrete:
*could an agent launched in `plugins/method/<kind>/` be productive
without reading anything outside that directory?* If not, the
boundary is in the wrong place. Inferencing — the LLM client, RAG
store, and acquisition pipeline — used to live in core and was
exiled to `plugins/inference/` when only the chat method consumed it.

**Asynchronous by construction.** No socket-handling thread ever
performs work. It normalizes the wire bytes, submits a `task_t` to
the work queue, and returns to listening. The task system runs an
elastic pool of worker threads driven by an 8-bit priority and a
per-task state machine (`WAITING → RUNNING → SLEEPING → ENDED`),
with linked tasks for chained continuations and timer-driven
sleepers for periodic work.

**Configuration is data, not code.** Operational parameters —
thread counts, timeouts, retry budgets, policy thresholds, IRC
networks, exchange API keys, strategy parameters — live in the
database under a typed, hierarchical KV schema with live
change-callbacks. The owner adjusts them at runtime via `/set`.
Compile-time constants are reserved for values that are structural
or whose runtime mutation would violate safety invariants.

**Authoritative state on disk; ephemeral state in RAM.** KV holds
knobs. The DB holds persistent state. In-memory holds personality-
derived and ephemeral state. Plugins own their own DDL — schema
ships with the subsystem, not with core.

---

## The plugin layer model

Plugins live in strictly-layered directories with downward-only
dependencies. Each plugin exports a `plugin_desc_t`; a topological
sort over `.provides` / `.requires` arrays drives the load order.
Dependency typos fail at `plugin_resolve()` time, not silently at
runtime.

| Layer | Purpose | Examples |
|---|---|---|
| `plugins/db/` | Database engine drivers | postgresql |
| `plugins/protocol/` | Wire protocols | IRC, botmanctl |
| `plugins/method/` | Bot interaction methods (the kinds) | command, chat |
| `plugins/service/` | External API integrations | coinbase, openweather, coinmarketcap, searxng |
| `plugins/cmd/` | Pure command-surface wrappers (no I/O) | `misc_*` |
| `plugins/inference/` | LLM client + knowledge store + acquisition | inference |
| `plugins/feature/` | Cross-cutting capability layers | whenmoon, exchange |
| `plugins/feature/whenmoon/strategy/` | Loadable trading strategies | testing |

**Inter-plugin contracts** live as headers in `plugins/contracts/` —
consumer-agnostic types and function-pointer signatures, no
dispatchers. Both implementer and consumer import the header;
neither imports the other's plugin directory. (Example: protocol
plugins publish optional dossier scorers through
`method_driver_t.dossier_scorer`, void-typed in `method.h` so core
stays free of chat-type dependencies; the chat plugin casts and
binds them at start time.)

---

## The data flow

```
  Socket I/O (core)
    → Method plugin (protocol → normalized message)
      → Bot instance (subscribed to method via callback)
        → Bot decides what to do based on its kind
          → Submits a task to the work queue
            → Worker thread executes the task
              → Response sent back via the originating method
```

A bot instance can carry multiple methods but exactly one user
namespace. A given user can reach the bot through any of its
methods; the bot always replies on the originating method.

---

## Bot kinds

A bot's *kind* names the method plugin it binds. Adding a method
plugin (a future `xmpp` or `telegram`) automatically becomes a
usable bot kind — there is no separate kind taxonomy.

**`command`** — Structured slash-command bot. Hierarchical
subcommands, declarative argument specs (`cmd_arg_desc_t[]`)
that pre-tokenize and validate before the handler is invoked,
per-method prefixes (`!` on IRC, `/` on botmanctl), abbreviations
unique within scope. The default kind for operator-driven bots.

**`chat`** — LLM-powered conversational bot with configurable
persona. Classifies incoming lines as WITNESS or EXCHANGE_IN, logs
them through a memory subsystem (`conversation_log` +
`user_facts`/`dossier_facts` + embedding tables), and decides
whether to speak via a pure speak-policy function. When it replies,
RAG-assembled context — facts, recent conversation, knowledge
chunks — is fenced into the system prompt and a streaming chat
completion drives the reply. A persona is defined by a personality
body and an output contract (no narration, no parentheticals, a
SKIP sentinel for opting out), both loaded from `personalities/*.txt`
at startup.

Cross-cutting capabilities that are *not* bot interactions —
trading, exchange routing — live as **feature** plugins and are
reached as verbs on the unified command registry, with no bot
instance mediating.

---

## A worked example: Whenmoon

Whenmoon is the canonical "feature plugin" — a capability layer,
not a bot. It lives at `plugins/feature/whenmoon/` and is consumed
via `/whenmoon` (abbreviated `/wm`) verbs from any operator session.
It owns:

- **Market state** — symbols, granularity rings, in-flight downloads.
- **Multi-grain candle aggregation** — 1m → 5m → 15m → 1h → 6h → 1d
  fanned out by a single aggregator from a single 1m source.
- **A history downloader** — pre-flight gap detection over
  `wm_trades_*` and `wm_candles_*`, resume on partial completion,
  per-page row commit, and a runtime depth probe that bisects
  Coinbase's per-granularity history caps and caches them in KV.
- **A strategy registry** — loadable `PLUGIN_STRATEGY` plugins
  discovered via `plugin_find_type(PLUGIN_STRATEGY, ...)`. Each
  strategy declares its parameters; whenmoon registers them under
  `plugin.whenmoon.strategy.<name>.<param>` and dispatches bar
  closes via `wm_strategy_on_bar`.
- **A trade-book registry** — per-`(market, strategy)` book with a
  paper-mode synthetic-fill engine, transactional fills against the
  live exchange via `EXCHANGE_PRIO_TRANSACTIONAL`, and a fills ring
  that supports first-principles reconciliation
  (`/show whenmoon trade reconcile <market_id> <strategy>`).
- **A snapshot/replay backtest runtime** — replays
  `wm_candles_<id>_60` through the same aggregator that runs live,
  so strategies see the identical `mkt->grain_arr[g][i]` surface in
  both modes. Trade-tape detail is asymmetric: live and paper get
  sub-bar microstructure (order flow, size-weighted ticks);
  backtest gets aggregated bars only.

### Strategy ABI

The supported view of a market is the per-grain bar ring published
in `plugins/feature/whenmoon/market.h`:

```c
mkt->grain_arr[g][i]   // i ∈ [0, grain_n[g])
                       // newest bar always at grain_arr[g][grain_n[g] - 1]
```

The aggregator shifts-left on overflow, so the index of the newest
slot never drifts. Each `wm_candle_full_t` carries OHLCV plus a
fixed 50-slot `ind[]` block populated once per bar close by
`wm_indicators_compute_bar` — strategies read `bar->ind[WM_IND_*]`
rather than recomputing. The slot enum (SMA-7/20/25/50/200,
EMA-9/12/20/26/50, MACD, RSI, Stochastic, CCI, Bollinger + %B,
VWAP/OBV/MFI/VPT, ATR/TR/NATR/ADX, ROC/MOM/WILLR/PSAR,
microstructure) is versioned via `WM_INDICATOR_SCHEMA_VERSION`;
new slots are appended after `WM_IND_RESERVED_BASE` and never shift
existing ids. Coinbase and aggregator types stay behind
`WHENMOON_INTERNAL` so strategies see opaque pointers and the
public bar surface, never exchange plumbing. Strategy authors can
ship closed-source `.so` files.

---

## Core services in depth

**CLAM** — *Central Logging And Messaging.* A publish-subscribe
message bus where logging is first-class. Severity 0 (FATAL)
through 7 (DEBUG-5). Subscribers register a name, a max-severity
threshold, an optional regex filter, and a callback. The default
`stdinout` subscriber gives color-coded console output; an optional
file subscriber appends timestamped messages if `LOG_PATH` is set
in `botman.conf`. Botmanctl clients can stream CLAM messages live
with per-client severity and regex filters.

**Priority-aware libcurl.** A single thread drives `curl_multi`.
Each request carries a per-request priority byte:

```
CURL_PRIO_TRANSACTIONAL = 0     // order placement, auth — critical path
CURL_PRIO_NORMAL        = 50    // user-facing fetches
CURL_PRIO_BULK          = 254   // history backfill, knowledge ingest
```

Three FIFO sub-queues drain highest-priority first. The shutdown
drain cancels queued and in-flight non-TRANSACTIONAL requests with
`CURLE_ABORTED_BY_CALLBACK` (so callers distinguish abort from
generic transport error) and waits up to `CURL_DRAIN_DEADLINE_MS`
for in-flight TRANSACTIONAL requests to settle before exit.

**Tracked allocator.** All heap allocation goes through
`mem_alloc("module", "label", size)` / `mem_free(ptr)`; raw
`malloc`/`free` is prohibited. Every allocation is journaled with
timestamp, size, module, label, and pointer. The journal detects
null frees, double frees, freeing untracked memory, and heap
corruption, with reuse via a freelist and mutex-protected access.

**Hierarchical KV.** Dot-namespaced (`core.subsystem.key`,
`plugin.name.key`, `bot.botname.key`), typed (INT8 through UINT64,
FLOAT, DOUBLE, LDOUBLE, STR), with change-callbacks for live
reconfiguration. Plugins declare plugin-level (`kv_schema`) and
instance-level (`kv_inst_schema`) schemas in their descriptors.
The bootstrap config (`$HOME/.config/botmanager/botman.conf`)
carries only the initial DB connection parameters — everything
else lives in the database.

**User namespaces.** Identity, authentication, and authorization
in a unit shareable across multiple bot instances. Four built-in
groups per namespace — `owner`, `admin`, `user`, `everyone` —
drive command authorization by group + privilege level. All users
are anonymous until they authenticate; protocol-level identity
(IRC nick, etc.) is *not trusted on its own* but is input to MFA
pattern matching. MFA patterns are `handle!username@hostname` glob
expressions, validated by an in-memory FNV-1a hash table that never
hits the DB during normal operation. Optional autoidentify on MFA
match. Optional userdiscovery for unknown handles. Argon2id for
passwords, with per-user salts and a 10+/mixed-case/digit/symbol
policy.

**Task system.** The central scheduler. WAITING → RUNNING →
SLEEPING → ENDED state machine, types PARENT/THREAD/ANY,
linked-task continuations, sleeping tasks re-evaluated on timer
expiry. Workers from an elastic pool service the highest-priority
ready task; the parent thread registers as worker slot 0 to handle
PARENT-typed tasks.

---

## Operator experience: `botmanctl`

A core-provided method that exposes operator control over a Unix
domain socket at `~/.config/botmanager/botman.sock`. Up to 16
concurrent connections, each in **command mode** or **subscribe
mode**. Commands run as the hard-coded `@owner` user with
`operator_origin` set, bypassing all permission checks. The
`botmanctl` CLI offers three modes:

```
botmanctl <command> <args...>     # one-shot: arg → response → exit
botmanctl                          # attach: interactive loop
botmanctl -S <severity> [-r RE]    # subscribe: stream CLAM live
```

The same command registry serves IRC, botmanctl, and any future
method — registration is method-independent. Service plugins
respond via `cmd_reply()` without ever knowing which transport
delivered the request.

---

## Fault tolerance & lifecycle

Fault tolerance is operational, not infrastructural. A core
subsystem failure is fatal — the program exits gracefully through
CLAM. External failures are expected: HTTP errors, timeouts,
network interruptions, malformed responses are all logged and
handled defensively, with method plugins managing reconnection
through an `AVAILABLE → RUNNING` transition.

```
Startup:  args → bootstrap → daemonize → CLAM → allocator →
          tasks/pool → DB → KV → curl → userns → botmanctl →
          plugin discovery → topo-sort → init in order →
          bot restore → parent enters worker pool

Shutdown: bots → plugins (reverse) → pool drain → KV flush →
          DB close → tasks finalize → botmanctl → allocator
          (with leak report) → CLAM → PID file removal
```

`SIGTERM` and `SIGINT` trigger the shutdown sequence. Every
subsystem exposes a `_get_stats()` returning a typed struct,
surfaced via `/show status` (summary) and `/show <subsystem>`
(detail). New subsystems must ship telemetry as part of their API
contract.

---

## Build

See [`BUILD.md`](BUILD.md). The project targets the modern Linux
kernel and its ecosystem; portability to other operating systems
is explicitly out of scope.

## Deeper reading

- [`DESIGN.md`](DESIGN.md) — full design specification
- [`PLUGIN.md`](PLUGIN.md) — plugin layer rules and the grep audits
  that enforce them
- [`CHATBOT.md`](CHATBOT.md) — chat method internals
- [`KNOWLEDGE.md`](KNOWLEDGE.md), [`ACQUIRE.md`](ACQUIRE.md),
  [`LLM.md`](LLM.md) — inference plugin subsystems

---

*A subsystem belongs in core only if multiple bot kinds consume
it. Everything else is a plugin.*
