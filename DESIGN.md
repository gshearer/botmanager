# BotManager Design

BotManager is a modular framework written in C for Linux. It functions as a microkernel for bots: a minimal, stable core provides infrastructure services while plugins supply all bot behavior, interaction methods, and extended functionality.

**Microkernel discipline.** A subsystem belongs in core only if multiple bot kinds consume it. Subsystems specific to a single bot kind live alongside that kind's plugin code, not in core. Practical test: could an agent launched in `plugins/method/<kind>/` (or `plugins/feature/<kind>/`) be productive without reading anything outside that directory? If not, the boundary is in the wrong place.

Worked example: the LLM client, knowledge store, and acquisition engine were originally in `core/` but were consumed only by `plugins/method/chat/`. Per the rule above, they moved to a single `plugins/inference/` plugin — consumed today by the chat method, and planned to be shared with the future trading feature. Chat declares `.requires = { "inference" }`; the inference plugin's public header `plugins/inference/inference.h` is a set of dlsym-shim inlines over the plugin's real symbols.

Support for other platforms is strictly out-of-scope; this program targets the modern Linux kernel and its ecosystem.

# Goals

- The core is infrastructure only — it never interprets user input or makes decisions about message content.
- Bot "kinds" are defined entirely by plugins. The core provides the services they need.
- Bots can interact with humans via multiple methods (IRC, XMPP, Telegram, Slack, and more).
- A bot instance can have multiple methods but only one user namespace. A given user can interact with the bot via any of its methods; the bot always replies on the originating method.
- The entire system is asynchronous and non-blocking.

## Use Cases

### Command Bot (kind: command) — in scope

A bot that interacts via structured user commands (e.g., `!help`, `!status`). When a user issues a command, the bot submits a task to the work queue; a worker thread handles it and returns the response via the originating method. The input thread never blocks. This is the initial implementation target.

### LLM-Powered Conversational Bot (kind: chat) — in scope

A bot that drives a configurable persona over a chat method via an LLM. Classifies incoming lines as WITNESS or EXCHANGE_IN, logs them through the memory subsystem, and decides whether to speak via a pure speak-policy function. When it replies, RAG-assembled context (facts + recent conversation + knowledge chunks) is fenced into the system prompt; a streaming chat completion drives the reply.

**Personality and contract.** A persona is defined by a personality body and an output contract (wire-format rules: no narration, no parentheticals, the SKIP sentinel for opting out). Both are loaded from `personalities/*.txt` at startup; the contract is stored inline on the personality row — there is no separate runtime contract entity.

**Memory layers** (chat-plugin-owned): `memory.c` owns `conversation_log` + `user_facts`/`dossier_facts` + embedding tables; `dossier.c` provides identity/mention resolution; `extract.c` converts raw log rows into structured facts via an LLM. These three compose with two core-owned services: `knowledge.c` (per-corpus RAG) and `acquire.c` (topic → SearXNG → digest → ingest pipeline). See `CHATBOT.md`, `plugins/method/chat/MEMSTORE.md`, `KNOWLEDGE.md`, `ACQUIRE.md` for reference detail.

### Asset Trading Bot — separate project

A separate project. The framework must support concurrent REST/WebSocket, operational fault tolerance, high-frequency task scheduling, and plugin-kind-agnostic corpus acquisition via the same `acquire_register_topics` API the chat bot uses.

# Data Flow

```
Socket I/O (core)
  → Method plugin (protocol → normalized message)
    → Bot instance (subscribed to method, receives via callback)
      → Bot decides what to do based on its kind
        → Submits a task to the work queue
          → Worker thread executes the task
            → Response sent back via the originating method
```

The input-handling thread never performs the work itself. It submits a task and returns to listening immediately.

# Configuration Philosophy

Operational parameters — thread counts, buffer sizes, timeouts, session limits, policy thresholds — must be stored in the database via the KV subsystem rather than hardcoded. On first run, settings are established with reasonable defaults; the owner adjusts them at runtime via `/set` without recompiling.

Compile-time constants are appropriate only when a value is truly structural (array sizing, hash table buckets) or when changing it at runtime would violate safety invariants.

# Core

Core provides infrastructure services consumed by two or more bot kinds. Source: `core/`.

- Heavily threaded framework with hardware-accelerated atomics for synchronization.
- Task-based work scheduler (see Task System).
- Database access via an abstracted API; DB plugins provide engine compatibility.
- CLAM: publish-subscribe message bus for logging and inter-plugin communication.
- libcurl-based API for REST and WebSocket. A single thread drives the `curl_multi` event loop; slow callbacks must submit a task.
- High-performance socket API for non-libcurl I/O with TLS support.
- Universal KV schema for runtime configuration.
- Universal user auth/authz via the DB subsystem.
- botmanctl method: operator control via Unix domain socket.

Inferencing (LLM client, knowledge store, acquisition engine) was lifted out of core into `plugins/inference/` — see `LLM.md`, `KNOWLEDGE.md`, `ACQUIRE.md`, and the inference plugin's public header `plugins/inference/inference.h`.

# Task System

The central work scheduler. Nearly all background work submits tasks here.

- Queue serviced by worker threads from the thread pool.
- State machine: WAITING → RUNNING → SLEEPING → ENDED or FATAL.
- Types: PARENT (parent thread only), THREAD (workers only), ANY.
- Priority: uint8_t, 0 = highest. Workers service highest-priority available tasks first.
- Linked tasks: when one completes, its linked task is promoted to WAITING.
- Sleeping tasks: re-evaluated when their timer expires (periodic work pattern).

# Thread Pool

Elastic worker architecture for the task system.

- Configurable via KV: max threads, min threads, min spare threads (defaults: 64, 1, 1).
- Workers are created on demand when spare count drops below min spare, up to max.
- Idle workers sleep for a configurable interval (default: 1000ms); idle beyond `maxidletime` exit unless they'd drop below min spare.
- Parent thread registers as worker slot 0, handling PARENT-type tasks.

# CLAM (Central Logging and Messaging)

Publish-subscribe message bus. Serves as the unified mechanism for logging, debugging, and inter-plugin communication.

- Severity levels 0 (FATAL) through 7 (DEBUG level 5).
- Subscribers register with a name, max severity threshold, optional regex filter, and a callback.
- Thread-safe; subscriber structs are reused via a freelist.
- Defaults to stdout if no subscribers are registered.
- Built-in `stdinout` subscriber provides color-coded output; registered automatically during `clam_init()`.
- File logger: if `LOG_PATH` is in `botman.conf`, a file subscriber appends timestamped plain-text messages.
- Botmanctl subscribe: clients can stream CLAM messages in real time with per-client severity and regex filtering.

# Memory Management

All heap allocations use `mem_alloc("module", "label", size)` / `mem_free(ptr)`. Direct `malloc`/`free` is prohibited.

Every allocation is tracked in a journal with metadata (timestamp, size, module, label, pointer). The journal detects null frees, double frees, freeing untracked memory, and heap size corruption. Journal entries are reused via a freelist. Thread-safe with mutex protection.

# DB

## DB Abstraction Layer

- Defines low-level operations that DB plugins must implement for a specific engine.
- Connection pooling: configurable count of connections tracked as ACTIVE, IDLE, or FAIL.
- Two execution modes: async (callback + task) or blocking.
- **Plugin-owned schema:** each plugin runs `CREATE TABLE IF NOT EXISTS` in its own `init()` callback. Core's `scripts/schema.sql` carries only cross-plugin tables (`kv`, `userns*`, `bot_instances`, `bot_methods`, `llm_models`, `personalities`); moved subsystems' DDL travels with the subsystem.

## Hierarchical Key/Value Schema

Keys use dot-separated namespacing: `core.subsystem.key`, `plugin.name.key`, `bot.botname.key`. Values are typed (INT8–UINT64, FLOAT, DOUBLE, LDOUBLE, STR). Values support change callbacks for live reconfiguration. Plugins declare plugin-level (`kv_schema`) and instance-level (`kv_inst_schema`) schemas in their descriptors.

## Bootstrap Configuration

`$HOME/.config/botmanager/botman.conf` — flat KEY="VALUE" file providing only the initial DB connection parameters. All other configuration lives in the DB.

# User Namespaces

User namespaces store identity, authentication, and authorization data. Multiple bot instances may share one namespace.

- Four built-in groups per namespace: **owner**, **admin**, **user**, **everyone**. Cannot be deleted.
- Group membership drives command authorization: a user must be a member of a command's group with at least the required privilege level.
- All users are anonymous until they authenticate. Protocol-level identity (IRC nick, etc.) is not trusted on its own — it is input to MFA pattern matching only.
- Hard-coded `@owner` user bypasses all permission checks; botmanctl commands always run as `@owner`.
- **MFA patterns:** `handle!username@hostname` with glob matching (`*`, `?`). Each user can have multiple patterns. Security constraints: handle must have ≥ 3 non-glob chars, hostname ≥ 6. `userns_mfa_match()` uses an in-memory FNV-1a hash table and never hits the DB during normal operation. MFA patterns can only be managed by admins.
- **Autoidentify:** when enabled, a session is created automatically on MFA match (no password required). Admin-only toggle.
- **User discovery:** when `bot.<name>.userdiscovery` is enabled, unknown MFA strings auto-create user accounts from the handle portion.
- Credentials: argon2id password hash with per-user salt. 10+ chars, mixed case, digit, symbol.

# Bot Namespace

Each bot instance has three configuration tiers:

1. **Plugin globals** (`plugin.pluginname.*`) — shared resource definitions (e.g., IRC network definitions). Multiple bots may reference them.
2. **User namespace** (`users.namespacename.*`) — shared auth/authz store. Persists independently of any bot instance.
3. **Bot instance namespace** (`bot.botname.*`) — settings, active sessions, per-command auth requirements. Deleted when the bot instance is deleted.

# Plugins

Plugins provide all bot behavior, interaction methods, DB engines, external API integrations, and command extensions.

- Source: `plugins/plugintype/pluginname/`.
- Dynamically loaded/unloaded at runtime via `dlopen`/`dlclose`.
- Dependency-based: each plugin declares `.provides` and `.requires` feature lists. The loader resolves via topological sort (Kahn's algorithm) and initializes in dependency order.

## Plugin Layer Model

Plugins live in strictly-layered categories with downward-only
dependencies: **service** (`plugins/service/*`) and **exchange**
(`plugins/exchange/*`) expose API mechanisms, **command-surface**
(`plugins/cmd/*`) registers user commands that wrap those mechanisms,
**method** (`plugins/method/*`) owns bot-interaction-method state
(chat, command), **feature** (`plugins/feature/*`) owns capability
layers atop methods (whenmoon), with supporting **inference /
protocol / db** plugins beneath. Service plugins register zero user
commands; chat-specific side effects (dossier facts) live in the chat
plugin's NL bridge observer. See `PLUGIN.md §Layer Rules` for the
authoritative rules and grep audits.

## Plugin API

Each plugin exports a `plugin_desc_t` (symbol `bm_plugin_desc`) containing:

- Metadata: name, version, type, kind, API version.
- Feature provides/requires arrays.
- KV schemas: plugin-level (`kv_schema`) and instance-level (`kv_inst_schema`).
- Lifecycle callbacks: `init`, `start`, `stop`, `deinit`.
- Type-specific extension data (`ext` field, e.g., `db_driver_t*` for DB plugins).

## Plugin Dependencies

### Plugin-to-plugin
A plugin's `.requires` entry names a feature that another plugin `.provides`. Service plugins typically require `method_command`. Dependencies are resolved at load time.

### Core-provided features
Core registers synthetic providers at startup (`core_llm`, `core_kv`, `core_db`, `core_userns`, etc.). Plugins can declare these in `.requires`; they resolve as no-op edges in the topo-sort. A typo in a `core_*` name fails `plugin_resolve()` immediately. Full list: `include/AGENTS.md`.

## Inter-Plugin Contracts

When two plugins share a typed interface, it lives in `plugins/contracts/` as a header. Contracts are consumer-agnostic: they define types and function-pointer signatures only, no dispatchers. Both the implementer and consumer import the header; neither side imports the other's plugin directory.

Current contracts:

| Header | Implemented by | Consumed by |
|--------|----------------|-------------|
| `plugins/contracts/dossier_method.h` | Protocol plugins (IRC) | `plugins/method/chat/` |

Protocol plugins publish optional dossier scorers through `method_driver_t.dossier_scorer` / `.dossier_token_scorer` (void-typed in `method.h` to keep core free of chat-type dependencies). The chat plugin casts and registers them at start time.

## Runtime Service Extension

A core service that defers policy to consumers exposes a **named-callback registry**. Consumers register a function during `init()`; the service dispatches to it at decision time.

| Mechanism | Choose when |
|-----------|-------------|
| Compile-time contract | Every implementer wires the same vtable fields; missing = build error. |
| Runtime callback registry | Core is complete without it; policy is optional; consumers come and go. |

## Plugin Lifecycle

DISCOVERED → LOADED → INITIALIZED → RUNNING → STOPPING → UNLOADED

If any module fails in a way that cannot be recovered, the entire program gracefully exits.

## Plugin Types

| Type | Role |
|------|------|
| `core` | Synthetic providers (`core_llm`, etc.) registered by the plugin subsystem. No `.so` on disk. |
| `db` | Low-level DB engine driver. |
| `method` | Human interaction method (IRC, Slack, Telegram, XMPP, etc.). |
| `bot` | Bot behavior (command, llm, scraper, etc.). |
| `service` | External API integration (REST, WebSocket). |
| `misc` | Registers user commands only — no external API, no KV, no async state. |
| `personality` | Language/messaging personality. |

# Plugin Type: Method

Methods are the abstraction layer for human interaction. All methods share a common driver interface; bots interact with any method via the same API.

- Asynchronous and callback-driven — no polling.
- Deliver full message context: sender, channel, timestamp, raw text, `is_action`, optional `prev_metadata`.
- Three states: **Enabled**, **Running**, **Available**.
- A bot instance supports at most one method instance of each kind.

## Botmanctl Method

Core-provided method for operator control via Unix domain socket (`~/.config/botmanager/botman.sock`).

- Up to 16 concurrent client connections, each in **command mode** or **subscribe mode**.
- Commands dispatched as `@owner` with `operator_origin` set, bypassing all privilege checks.
- Protocol: line-oriented, each response terminated with `\0`.
- PID file written to `~/.config/botmanager/botman.pid`.

**CLI utility** (`botmanctl`): one-shot mode (args → command → response), attach mode (interactive loop), subscribe mode (`-S <sev>` with optional `-r <regex>`).

## Method Instance Lifecycle

Method plugins do **not** create connections at startup. Method instances are created on demand by the bot subsystem when a bot starts. The bot's KV namespace contains the instance-level configuration; the method resolves any referenced plugin-level resources (e.g., named IRC network → server list). Flow: ENABLED → RUNNING → AVAILABLE.

## Method Configuration Namespace

- **Plugin-level** (`plugin.<kind>.*`): shared resource definitions (e.g., IRC network definitions with ordered server lists).
- **Instance-level** (`bot.<botname>.<kind>.*`): per-bot identity and behavior (e.g., nick, network reference).
- **Per-channel** (`bot.<botname>.irc.chan.<channel>.*`): autojoin, key, announce, announcetext (supports `${name}`, `${nick}`, `${channel}`, `${version}` substitution).

# Plugin Type: Bot

Bot plugins define how incoming messages are interpreted. All bot kinds share built-in commands (help, version). Additional commands are registered by plugins and enabled per bot instance.

## Command System

- **Hierarchical subcommands** via `parent_path` in `cmd_register()`. Dispatch walks the tree iteratively; parent callback is invoked if no child matches.
- **Abbreviations**: each command can declare a short alias (e.g., "sh" for "show"). Unique within scope.
- **Help system**: `/help`, `/show`, `/set` are root commands. Subsystems register children under these using standard `cmd_register()`.
- **Per-method prefix**: `bot.<botname>.<kind>.prefix` (e.g., "!" for IRC, "/" for botmanctl). Falls back to the bot-level default from `cmd_set_prefix()`.
- **Declarative arg specs**: commands can declare `cmd_arg_desc_t[]` at registration; the framework validates and tokenizes input before invoking the handler, eliminating manual parsing boilerplate. Handlers access pre-parsed args via `ctx->parsed->argv[]`. See `include/cmd.h` for the full API.

## Bot Kinds

| Kind | Description |
|------|-------------|
| `command` | Structured user commands. Initial implementation target. |
| `chat` | LLM-powered conversational bot with configurable persona. |
| `scraper` | HTTP/HTTPS scraper. |
| `rest` | REST API client. |
| `websocket` | WebSocket client. |

# Plugin Type: Service

Service plugins integrate with external APIs and make results available as user commands. They are method-independent: they register commands via `cmd_register()` and respond via `cmd_reply()`, unaware of the originating method.

- Declare `PLUGIN_SERVICE` type; require `bot_command` feature.
- Use core's libcurl for HTTP/WebSocket. Do not manage their own connections.
- Store API keys and config under `plugin.<servicename>.*`.
- Lifecycle: `init` (register commands + KV), `start` (accept dispatches), `stop` (drain), `deinit` (unregister).

## Command-Surface Plugin Type

Plugins that only register commands and have no external API, no KV, no async state. Live under `plugins/cmd/<name>/`. Plugin name: `misc_<name>` (historical; the internal kind remains `misc_<name>` until a future rename).

# Bot Instances

- Each bot instance has a unique name used as the KV root (`bot.botname.*`).
- A bot binds: a bot plugin, one or more method instances, optionally a user namespace, optionally a personality.
- Each method instance is owned by the bot that created it. When the bot stops, its method instances are disconnected.
- Multiple instances of the same plugin kind can run simultaneously with independent configuration.
- On fresh install with no bots configured, the program starts cleanly and waits for operator configuration via botmanctl.

## Per-Bot KV Settings

- `bot.<botname>.maxidleauth` (UINT32, default 3600, 0 = never): idle session expiry.
- `bot.<botname>.userdiscovery` (UINT8, default 0): auto-create users from unknown MFA strings.
- `bot.<botname>.<kind>.prefix` (STR): per-method command prefix.

# Fault Tolerance

Fault tolerance applies at the operational layer, not the infrastructure layer. If a core subsystem fails irrecoverably, the program gracefully exits. External failures are expected and handled:

- Failed API calls (HTTP errors, timeouts) are logged and retried as appropriate without terminating.
- Network interruptions are treated as transient; method plugins handle reconnection.
- Malformed/unexpected responses are logged and handled defensively.
- Method plugins that lose connection transition AVAILABLE → RUNNING and attempt reconnection.

# Error Handling

- Core functions return `SUCCESS` (0/false) or `FAIL` (1/true).
- Fatal errors trigger graceful shutdown via CLAM.
- Non-fatal errors are logged and handled locally.
- SIGTERM and SIGINT trigger graceful shutdown.

# Input Validation

All user-facing commands must validate input before acting. Allowlist over blocklist. Validate at the boundary, before any processing or API calls.

## Shared Validators (`include/validate.h`, `core/validate.c`)

| Function | Use for |
|---|---|
| `validate_alnum(str, maxlen)` | Bot/usernames, namespace names |
| `validate_digits(str, min, max)` | Numeric-only fields |
| `validate_hostname(str)` | Server hostnames and IPs |
| `validate_port(str, *out)` | Port numbers 1-65535 |
| `validate_irc_channel(str)` | IRC channel names |

## Shared Utilities (`include/util.h`, `core/util.c`)

Generic domain-agnostic helpers: PRNG (`util_init`, `util_rand`), formatters (`util_fmt_bytes`, `util_fmt_duration`), string hashes (`util_fnv1a`, `util_fnv1a_ci`, `util_djb2`), bounded string scanning (`util_memstr`, `util_read_int`, `util_skip_to_value`), monotonic timing (`util_ms_since`).

Rule: if a helper is short (< 30 lines), has no subsystem-specific type dependencies, and is (or plausibly will be) useful to more than one module, it belongs in `util.c` with a `util_` prefix.

## JSON (`core/json.c`, `include/json.h`)

Two layers: byte-level primitives (`json_escape`, `json_unescape`) for streaming/partial-buffer scanning; DOM-based extraction via json-c for full response parsing. DOM idioms: declarative spec tables (`json_extract`) for structured responses, one-shot accessors (`json_get_*`) for ad-hoc access. See `plugins/service/coinmarketcap/` (spec table) and `plugins/service/openweather/` (accessors) for reference patterns.

# Telemetry

All subsystems expose a `_get_stats()` function returning a typed struct. Per-instance counters use atomic increments or are updated under existing locks. Statistics are accessible via `/show status` (summary) and `/show <subsystem>` (detail). New subsystems must provide stats as part of their API contract.

# Startup Sequence

1. Parse args; read bootstrap config; daemonize; write PID file.
2. Initialize core subsystems in dependency order: CLAM → allocator → thread pool + tasks → DB → KV → curl + userns → botmanctl method. (LLM, knowledge, and acquisition initialize inside the `inference` plugin's `init()`.)
3. Plugin loader: register synthetic `core_*` providers → discover `.so` files → dependency resolution → initialize in topo order. Chat-plugin subsystems (`memory`, `extract`, `dossier`) initialize here inside the chat plugin's `init()`.
4. Bot restore: register driver KV keys, then create + bind + start previously-running bots.
5. Parent thread enters the task system as worker slot 0.

# Shutdown Sequence

Reverse of startup: bot instances → plugins (reverse order) → thread pool drain → KV flush → DB close → task finalize → botmanctl → allocator (leak report) → CLAM → PID file removal.
