# BotManager Design

BotManager is a modular framework written in C for Linux. It functions as a microkernel for bots: a minimal, stable core provides infrastructure services while plugins supply all bot behavior, interaction methods, and extended functionality. Support for other platforms is strictly out-of-scope; this program will leverage features of the modern Linux kernel and its ecosystem.

This file describes general design goals and will change as the project develops.

# Goals

BotManager aims to be a general-purpose bot framework where:

- The core is infrastructure only — it never interprets user input or makes decisions about message content.
- Bot "kinds" are defined entirely by plugins. The core provides the services they need to operate.
- Core functionality can be extended by plugins.
- Bots can interact with humans via multiple methods (IRC, XMPP, Telegram, Slack, and more in the future).
- A bot instance can have multiple methods but only one user namespace. A given user can interact with the bot via any of its methods at any time. The bot always replies on the method the message came in from.
- The entire system is asynchronous and non-blocking. Bots are always responsive because no operation blocks the thread that receives user input.

## Use Cases

The following use cases define the scope and priorities of this project:

### Command Bot (kind: command) — in scope

A bot that interacts with humans via its methods using structured user commands (e.g., `!help`, `!status`). All bots have a small set of built-in commands (help, version, status). Additional commands can be defined by plugins, but a bot does not automatically inherit plugin-defined commands — each must be explicitly enabled in the bot instance's configuration.

When a user issues a command, the bot does not process it on the input thread. Instead, it submits a task to the work queue. A worker thread picks up the task, gathers whatever data is needed (potentially from other subsystems), and sends the response back via the originating method. The input thread is immediately free to handle the next message. This non-blocking pattern is fundamental to the architecture.

The command bot and its first method plugin (IRC) are the initial implementation targets for this project.

### LLM-Powered Conversational Bot (kind: mimick) — separate project

A bot that leverages large language models to behave as closely as possible to "just another human" in a group chat. This bot would maintain persistent memory about conversations and participants, integrating context into future interactions. It can translate natural language input into user commands when appropriate.

This bot kind will be developed as a separate project and is not in scope for BotManager core development. However, the core framework must be capable of supporting it. This means:

- The plugin API must support bots that process raw message content, not just structured commands.
- The DB abstraction must support bot plugins that need their own persistent storage beyond simple KV configuration (e.g., conversation history, per-user memory).
- The libcurl subsystem must support interaction with LLM inferencing APIs (e.g., Ollama).
- The method abstraction must deliver full message context (sender, channel/group, timestamp, raw text) to the bot, not just parsed commands.

### Asset Trading Bot — separate project

A bot that leverages REST and WebSocket APIs of asset trading platforms (e.g., Coinbase, Schwab) to perform algorithmic trading on behalf of its user. This is an ambitious project that will be maintained entirely separately from BotManager.

The core framework must be capable of supporting it. This means:

- The libcurl subsystem must handle concurrent REST and WebSocket connections reliably.
- Operational fault tolerance must be robust — transient API failures, network interruptions, and unexpected responses must be handled gracefully without terminating the program.
- The task system must support high-frequency, low-latency work scheduling.
- The DB abstraction must support persistent state storage for trading strategies and position tracking.

# Data Flow

The fundamental data flow pattern in BotManager is a subscriber chain where no link in the chain blocks:

```
Socket I/O (core service)
  → Method plugin (translates protocol-specific data to normalized messages)
    → Bot instance (subscribed to method, receives messages via callback)
      → Bot decides what to do based on its kind
        → Submits a task to the work queue
          → Worker thread executes the task
            → Response sent back via the originating method
```

The core provides socket I/O. Method plugins subscribe to sockets and handle protocol details. Bot instances subscribe to methods and receive normalized messages. The bot — not the core, not the method — decides how to interpret the message. A command bot checks for a command prefix. A mimick bot would feed the message to an LLM. A trading bot might ignore human messages entirely and process only API data.

The input-handling thread never performs the work itself. It submits a task and returns to listening immediately.

# Configuration Philosophy

Operational parameters — thread counts, buffer sizes, timeouts, session limits, policy thresholds, and similar tunables — must be stored in the database via the KV subsystem rather than hardcoded as `#define` macros or literal constants. When the program first runs, these settings are established in the KV store with reasonable defaults, and the owner can adjust them at runtime via `/set` without recompiling.

Compile-time constants are appropriate only when a value is truly structural (e.g., array sizing that determines memory layout, hash table bucket counts) or when changing it at runtime would violate safety invariants (e.g., cryptographic parameters that must remain consistent with existing stored data). Even then, structural constants should serve as upper bounds while the KV setting controls the effective value within that bound.

The guiding rule: if an operator might reasonably want to tune a parameter for their deployment, it belongs in the database.

# Telemetry and Observability

Every subsystem and plugin should collect data that helps the operator understand what the program is doing, how it's performing, and whether anything needs attention. Telemetry is not an afterthought — it's a core design requirement on par with fault tolerance.

## Principles

- All subsystems expose a `_get_stats()` function returning a typed struct.
- Per-instance counters (messages in/out, errors, connection events) are maintained by the subsystem, not by the caller.
- Statistics are accessible via `/status` (summary dashboard), `/show <subsystem>` (detailed view), and programmatic APIs.
- Telemetry never blocks the hot path — counters use atomic increments or are updated under existing locks.
- New subsystems and plugins must provide stats as part of their API contract.

## Existing Infrastructure

Several subsystems already follow the `_get_stats()` pattern: `mem_get_stats()`, `task_get_stats()`, `pool_get_stats()`, `method_get_stats()`, `bot_get_stats()`, `curl_get_stats()`, and `sock_get_stats()`. The `/status` command provides a summary dashboard, and the `/show` subcommand tree provides detailed views per subsystem.

## Method Type Registry

Each method type (console, botmanctl, irc, etc.) has a human-friendly description string registered in a static table in `method.c`. This table is used by `method_type_bit()`, `method_type_desc()`, and `method_iterate_types()` to map between driver names, type bitmasks, and descriptions. The `/show methods` command displays all known method types with their descriptions and lists active method instances with per-instance statistics (messages in/out, subscriber count, state).

# Core

The "core" of BotManager provides the foundational infrastructure upon which all plugins and bot instances operate. Source code for the core is in the "core" subfolder.

- Heavily threaded framework leveraging hardware-accelerated atomics for thread synchronization where appropriate.
- Asynchronous design such that services provided by the core are always available on a predictable schedule with sub-second accuracy.
- A task-based work scheduler that performs most all work required by the core itself and plugins (See Task System below).
- Database access via an abstracted API. Plugins provide database-specific compatibility such as MySQL, PostgreSQL, SQLite, etc. (See DB below).
- A subscriber-based Central Logging and Messaging (CLAM) system that serves as the message bus of the entire system (See CLAM below).
- A libcurl-based API for access to services such as REST and WebSocket APIs.
- A high-performance socket communication API for non-libcurl-based I/O with TLS support.
- A universal key/value DB schema for managing configuration and persistent data (See DB below).
- A universal user authentication/authorization system that leverages the DB subsystem (See User Namespaces below).
- The method "console" is provided by the core for direct operator interaction (See Plugin Type: Method below). All other methods are provided by plugins.
- Runs in the foreground and requires the console method.
- A minimal bootstrap configuration system that:
  - Leverages a simple key="value" text file stored in $HOME/.config/botmanager/botman.conf, overridable with a command-line option.
  - Defines one DB plugin and credentials/connectivity information necessary for that plugin.
  - Defines configuration options specific to the console method.
- All other configuration elements are stored in the database, most of which will be adjustable by the owner during runtime.

# Task System

The task system is the central work scheduler for BotManager. Nearly all background work — in the core and in plugins — is performed by submitting tasks to this system.

- Tasks are submitted to a queue and serviced by worker threads from the thread pool.
- Each task has a state machine: WAITING → RUNNING → SLEEPING → ENDED or FATAL.
- Task types control where a task may execute:
  - PARENT: runs only on the parent thread.
  - THREAD: runs only on worker threads.
  - ANY: runs on either.
- Tasks have a uint8_t priority where 0 is the highest and 254 is the lowest. Workers service the highest priority available tasks first.
- Tasks can be linked: when a task completes, its linked task is automatically promoted to WAITING. This enables sequenced multi-step operations without explicit synchronization.
- Tasks can sleep for a specified duration and be re-evaluated when their timer expires, enabling periodic work (e.g., idle connection cleanup, scheduled polling).
- Tasks set their own state before returning from their callback, giving the task control over whether it should be re-run, linked, or finalized.
- Task statistics (running, waiting, sleeping, linked counts) are tracked globally.

# Thread Pool

The thread pool provides an elastic worker architecture that services the task system.

- Configurable via core KV settings: maximum threads, minimum threads, and minimum spare threads (defaults: 64, 1, 1).
- The minimum number of threads is created at boot time. Additional threads are created on demand when spare threads drop below the minimum spare threshold, up to the maximum.
- Worker threads look for tasks in the work queue as their main loop. If no work is found, they sleep for a configurable interval (default: 1000ms).
- If a worker thread is idle for longer than "maxidletime", it will join the parent thread unless doing so would drop the number of sleeping threads below the minimum spare threads setting.
- Each worker tracks how many tasks it has serviced during its lifetime (uint64_t counter).
- The parent thread registers itself as worker slot 0 and participates in the task system, handling PARENT-type tasks.

# CLAM (Central Logging and Messaging)

CLAM is the publish-subscribe message bus for the entire system. It serves as the unified mechanism for logging, debugging, inter-plugin communication, and core-to-plugin messaging.

- Severity levels range from 0 (FATAL) through 7 (DEBUG level 5), providing fine-grained control over message verbosity.
- Subscribers register with:
  - A name identifying the subscriber.
  - A maximum severity threshold (messages above this level are not delivered).
  - An optional regex pattern for content-based filtering.
  - A callback function invoked for each matching message.
- Thread-safe with mutex protection on the subscriber list.
- Subscriber structs are reused via a freelist to avoid allocation churn.
- If no subscribers are registered, output defaults to stdout.
- Logging is not a separate system — it is simply a set of CLAM subscribers (stdinout, file, etc.).

## Built-in stdinout Subscriber

CLAM includes a built-in "stdinout" subscriber that provides color-coded severity output to stdout. This subscriber is internal to CLAM and is registered automatically during `clam_init()`, making it available before any other subsystem starts. Output uses the format: `HH:MM:SS SEVERITY CONTEXT message`.

The stdinout subscriber is configurable at runtime via KV settings (registered in `clam_register_config()`, called after KV initialization):

- `core.clam.stdinout.severity` (UINT8, default 7): Maximum severity level delivered to stdout. Messages above this level are suppressed. Setting to 0 shows only FATAL messages; setting to 7 shows all messages including DEBUG5.
- `core.clam.stdinout.regex` (STR, default ".*"): Regex pattern for content-based filtering. Only messages matching this pattern are displayed.

Both settings support KV change callbacks, so the operator can adjust verbosity and filtering at runtime via `/set` without restarting.

The console method is **not** a CLAM subscriber. Console output for command replies uses its own `console_print()` function. The separation ensures that the stdinout subscriber works independently of whether the console method is registered, and that console input/output is not entangled with the logging system.

# Memory Management

BotManager uses a custom memory allocation layer for all heap allocations in the core and plugins.

- All allocations are made via `mem_alloc("module", "purpose_name", size)` and freed via `mem_free(ptr)`. Direct use of libc malloc/free is prohibited.
- Every allocation is tracked in a journal with metadata: timestamp, size, module name, descriptive name (for debugging), and pointer.
- The journal enables detection of:
  - Null pointer frees.
  - Freeing untracked memory (e.g., double free or freeing a stack address).
  - Heap size corruption (freeing more than allocated).
  - Out-of-memory conditions (fatal).
- Journal entry structs are reused via a freelist to minimize allocation overhead for the tracking system itself.
- Total heap size is tracked globally.
- Thread-safe with mutex protection.
- The named allocation pattern provides immediate context when diagnosing leaks or corruption in a long-running multi-threaded daemon.

# DB

The core DB service provides a hierarchical key/value schema and an abstracted query API for the core, plugins, and bot instances.

## DB Abstraction Layer

- The abstraction layer defines a set of low-level operations that DB plugins must implement for a specific database engine.
- Supports connection pooling: a configurable number of connections are maintained, each tracked as ACTIVE, IDLE, or FAIL. Idle connections are periodically reaped after a configurable timeout.
- Two execution modes for queries:
  - Async: a callback is provided and the query executes as a task, invoking the callback upon completion.
  - Blocking: the caller blocks until the query completes, then receives the result directly.
- Query results are structured as row/column data. Callers must copy results before releasing the connection.
- SQL escaping is handled by the DB plugin using engine-appropriate functions.
- The abstraction layer also supports direct query execution for plugins that need their own tables and data beyond the KV schema (e.g., a conversational bot storing message history, or a trading bot storing position data).

## Hierarchical Key/Value Schema

- Plugins declare KV entries at two levels: plugin-level keys (registered once globally) and instance-level keys (cloned per consumer at bind time). Both are declared in the plugin descriptor and automatically registered by the core.
- Keys use dot-separated hierarchical namespacing:
  - `core.subsystem.keyname` — core settings.
  - `plugin.pluginname.category.keyname` — plugin globals, applied across all instances using this plugin.
  - `bot.botname.category.keyname` — per-bot-instance settings, created when the instance is first instantiated.
- K/V values are typed: INT8, UINT8, INT16, UINT16, INT32, UINT32, INT64, UINT64, FLOAT, DOUBLE, LDOUBLE, and STR.
- Values support update callbacks: when a value is changed (including at runtime by the operator), a registered callback is invoked. This is the mechanism that makes runtime reconfiguration work.
- Configuration is loaded from the database during startup via a task, and changes made during runtime are persisted back to the database.

## Bootstrap Configuration

- A flat file ($HOME/.config/botmanager/botman.conf) provides only what is needed to establish the initial database connection: DB plugin name, host, port, database name, user, and password.
- This file uses a simple KEY="VALUE" format.
- The path can be overridden via command-line argument.
- Once the database connection is established, all further configuration is loaded from and managed by the DB.

# User Namespaces

User namespaces store identity, authentication, and authorization data. They are shared resources that exist independently of any single bot instance.

- There can be multiple user namespaces. A bot instance links to exactly one user namespace, but multiple bot instances may share the same namespace.
- A user namespace is populated in the DB when first requested by a bot instance.
- Four built-in groups exist in every namespace: **owner**, **admin**, **user**, and **everyone**. These are created automatically when a namespace is first loaded or created, and cannot be deleted. Additional groups can be created by users with appropriate permissions.
- Each group has a 30-character alphanumeric name and a 100-character description.
- A user can be a member of multiple groups. Each group membership has a privilege level (uint16_t, 0-65535). Higher levels grant access to more commands.
- Group membership is the primary method of command authorization. To use a command, a user must be a member of the command's group with at least the required privilege level.
- All users are anonymous on all methods until they explicitly authenticate. No assumptions are made from protocol-level identity (IRC nick, Telegram username, etc.) — these are untrusted. Unauthenticated users are treated as having membership in the "everyone" group at level 0, meaning they can only access commands in the "everyone" group that require level 0.
- A hard-coded internal user known as "@owner" exists in every namespace. This user bypasses all permission checks and is a member of all built-in groups with privilege level 65535. The "@" prefix prevents collision with normal alphanumeric usernames. Commands issued via the console are always tied to the @owner user.
- New users are automatically added to the "everyone" group (level 0) and the "user" group (level 0) upon creation.
- Authentication is per-method and initiated by the user. For example, on IRC: `!identify username password`. The specific authentication mechanism is method-dependent (other methods TBD).
- Method plugins can contribute contextual information to the authentication request (e.g., IRC provides the user's hostname/IP). Where available, this serves as a second factor.
- Usernames are unique within a namespace. A user who has authenticated on one method can simultaneously authenticate on another method of the same bot instance using the same credentials.
- User credentials and profile:
  - A unique 30-character alphanumeric username.
  - A UUID (auto-generated via libuuid) for stable external identification.
  - A 100-character description field.
  - A 100-character passphrase field.
  - A password meeting minimum criteria:
    - 10 characters or longer.
    - At least one uppercase and one lowercase letter.
    - At least one number.
    - At least one symbol from: !@#$%^&*(){}[]';:"|,./\
  - Passwords are never stored in plaintext. A strong one-way hash (argon2id) with per-user salt is used.

## Multi-MFA Pattern Matching

Each user can have multiple MFA patterns stored in a dedicated `user_mfa` table (replacing the old single-field approach). MFA patterns use the format `handle!username@hostname` and support glob matching for handle and hostname components.

- **Pattern format**: `handle!username@hostname` — the username portion must match exactly (no globbing); handle and hostname support `*` (zero or more characters) and `?` (exactly one character) glob matching.
- **Security constraints**: The handle must have at least 3 non-glob characters, the hostname at least 6 non-glob characters. All-glob patterns are rejected. These constraints prevent overly broad patterns that could match unintended users.
- **Maximum patterns**: Configurable via `core.userns.max_mfa` (UINT32, default 10).
- **Pattern matching**: `userns_mfa_match(ns, mfa_string)` takes a raw MFA string (e.g., "doc!docdrow@laptop.example.com") and returns the matching username or NULL. The input is parsed into handle, username, and hostname components and matched against all patterns in the namespace. First match wins.
- **Admin commands**: `/mfa add <ns> <user> <pattern>`, `/mfa del <ns> <user> <pattern>`, `/mfa list <ns> <user>`.

## Authentication Timestamps and Idle Timeout

- `auth_time` and `last_seen` timestamps are tracked per session in `bot_session_t`.
- `auth_time` is set when a user successfully authenticates.
- `last_seen` is refreshed on every message from the authenticated user (via `bot_session_find()`).
- `bot.<botname>.maxidleauth` (UINT32, default 3600 seconds, 0 = never expire): configures the idle timeout per bot instance. A periodic task scans all active sessions and expires any where `time(NULL) - last_seen > maxidleauth`. Expired sessions are removed and logged via CLAM.

## User Discovery

When enabled, the bot automatically creates user accounts for unknown individuals encountered on its methods.

- `bot.<botname>.userdiscovery` (UINT8, default 0 / disabled): enables auto-creation per bot instance.
- When an MFA string is received that doesn't match any user in the namespace, a new user is auto-created: username derived from the handle portion (truncated, with numeric suffix for collisions), UUID auto-generated, no password set.
- The triggering MFA string is added as the user's first MFA pattern.
- The user is added to "everyone" and "user" groups at level 0.
- For IRC, encounters include PRIVMSG from unknown users, JOINs to channels the bot is in, and NAMES (353) responses after joining.
- A `register` command allows discovered (password-less) users to set their initial password and complete account setup.

## In-Memory User Cache

Each namespace maintains an in-memory cache for fast MFA lookups without DB round-trips.

- The cache is a hash table (FNV-1a on the raw MFA pattern string) mapping patterns to user records (UUID, username, pattern list).
- Populated from the DB when a namespace is first loaded via `userns_get()`.
- Cache entries are invalidated on user/MFA mutations (`userns_user_create`, `userns_user_delete`, `userns_user_add_mfa`, `userns_user_remove_mfa`).
- `userns_mfa_match()` uses the cache exclusively — it never hits the DB during normal operation.
- Protected by a per-namespace rwlock (`pthread_rwlock_t`) for concurrent read access.
- Cache memory is freed on `userns_exit()`.

# Bot Namespace

Each bot instance has three tiers of configuration and state:

1. **Plugin globals** (`plugin.pluginname.*`) — Settings defined by the plugin that apply globally. For method plugins, this includes shared resource definitions (e.g., IRC network definitions under `plugin.irc.network.*`) that any bot instance can reference. For bot plugins, these are settings shared across all instances of that bot kind.

2. **User namespace** (`users.namespacename.*`) — The shared authentication and authorization store this bot instance is linked to. Contains user credentials, group definitions, and group memberships. Persists independently — deleting a bot instance does not affect the user namespace.

3. **Bot instance namespace** (`bot.botname.*`) — Settings and runtime state specific to this one bot instance. This includes:
   - Bot-level configuration (which commands are enabled, which methods are bound, etc.).
   - Active sessions: which users are currently authenticated, on which method, from what source.
   - Per-command authorization requirements (group membership, privilege levels).
   - Any other state the bot plugin needs to persist for this instance.
   - When a bot instance is deleted, its entire namespace is deleted. Everything in it is forgotten.

# Plugins

BotManager is not very useful without its plugins as they provide its purpose(s) and extend functionality.

- Source code for plugins is stored in their own folder: plugins/plugintype/pluginname
- Plugins run independently of the core and from each other, typically using at least one worker task.
- Plugins can be dynamically loaded and unloaded at runtime via dlopen/dlclose.
- Plugins are dependency-based similar to a modern Linux package system. Every plugin provides at least one "feature" and can optionally require features provided by other plugins. A plugin cannot be unloaded if other active plugins depend on features it provides. If a plugin is loaded that requires features from other plugins, those dependencies are automatically loaded and initialized in the appropriate order.
- The plugin API provides mechanisms to advertise features, services, and endpoints to each other.
- Plugins can register commands. A bot instance does not automatically inherit plugin-defined commands — each must be explicitly enabled in the bot's configuration.

## Plugin API

- Each plugin defines a plugin descriptor that the core reads upon loading. The descriptor includes:
  - Plugin metadata: name, version, type, kind.
  - Features provided and features required (for dependency resolution).
  - KV schema: configuration keys declared at two levels:
    - **Plugin-level schema** (`kv_schema`): keys registered once under `plugin.<kind>.*`. These are global settings that apply across all instances of this plugin.
    - **Instance-level schema** (`kv_inst_schema`): keys that are cloned into a consumer's namespace when it binds to this plugin. For method plugins, these are registered under `bot.<botname>.method.<kind>.*` by the core when a bot binds to the method. The instance schema uses bare suffixes (e.g., `nick`, `network`) — the core prepends the appropriate namespace prefix automatically.
  - Lifecycle callbacks: init, start, stop, deinit.
  - Command registrations (if any).
- The core loads plugins via dlopen and resolves a known entry point symbol to obtain the descriptor.
- API versioning: the descriptor includes the plugin API version it was built against. The core rejects plugins built against incompatible API versions.

## Plugin Lifecycle

Plugins move through defined states:

1. **Discovered**: the shared object file exists in the plugin directory.
2. **Loaded**: dlopen has succeeded and the descriptor has been read.
3. **Initialized**: the init callback has been called. The plugin has registered its KV schema and features.
4. **Running**: the start callback has been called. The plugin is actively servicing work.
5. **Stopping**: the stop callback has been called. The plugin is draining in-flight work.
6. **Unloaded**: the deinit callback has been called and dlclose has been invoked.

If any module fails in a way that cannot be recovered, the entire program will gracefully exit and log the reason via CLAM.

## Plugin Types

- **core**: Extends core functionality. Can provide new APIs/frameworks for other plugins to leverage.
- **db**: Provides low-level functions required by the DB abstraction layer for a specific database engine. Has a "kind" sub-category defining the engine type (e.g., PostgreSQL).
- **method**: Provides a method of interacting with humans (e.g., IRC, Slack, Telegram, XMPP). See Plugin Type: Method below.
- **bot**: Provides BotManager with purpose. Has a "kind" sub-category. See Plugin Type: Bot below.
- **service**: Integrates with external APIs and data sources (REST, WebSocket, etc.). Registers user commands that any command bot can enable. See Plugin Type: Service below.
- **personality**: Defines a personality for human interaction. Personalities provide language-specific messaging including standard error/warning messages, command usage/syntax, and other user-facing text.

# Plugin Type: Method

Methods are the abstraction layer for human interaction. A method plugin provides a way for bots to communicate with humans through a specific platform or interface, without the bot or the core needing to know protocol-level details.

- Methods use an abstraction layer similar to the DB system: the core defines a common interface that all method plugins implement, allowing bot instances to interact with humans through any method using the same API.
- Methods are asynchronous and callback-driven. When a user sends a message (e.g., from IRC), a callback is executed on the bot — there is no polling.
- A bot instance may have more than one method defined, enabling it to serve users across multiple platforms simultaneously.
- Method plugins deliver full message context to the bot: sender identity (as known to the protocol), channel or group, timestamp, raw message text, and any method-specific metadata. The bot decides what to do with this information.
- Method plugins have three states visible to the rest of the system:
  - **Enabled**: the method plugin is loaded, instance configured.
  - **Running**: the method is active and processing (e.g., connecting to a server).
  - **Available**: the method has connected/logged in to its platform and can successfully interact with users.
- The first method plugin to be implemented will be IRC.
- The console is provided by the core and registers itself as a method instance (implementing `method_driver_t`), just like any other method. The console is a pure interactive input/output module — it is not a CLAM subscriber (see CLAM section above). Commands issued via the console are always tied to the hard-coded @owner user unless the console is attached to a bot with a different associated user identity.

### Console Attach/Detach

The console supports attaching to a running bot instance, enabling the operator to interact with the bot as if they were a user on one of the bot's methods. This is useful for testing commands, debugging, and administration.

- `/console attach <botname>` — attaches the console to a running bot instance. While attached, input is routed through `cmd_dispatch()` on the attached bot instead of the system command path. The `console_origin` flag on the message bypasses all permission checks regardless of the associated user.
- `/console unattach` — detaches from the bot and returns to system command mode. Resets the associated user to @owner.
- `/console associate <username>` — sets the user identity for commands dispatched to the attached bot. The username must exist in the bot's user namespace. This allows the operator to test permission behavior as a specific user.
- `/console unassociate` — resets the associated user back to @owner.

When attached, the console reads the per-method command prefix from `bot.<botname>.method.console.prefix` (default "/") and uses it when constructing synthetic messages for dispatch. If the prefix KV key doesn't exist, it is registered automatically on attach. System commands (`/quit`, `/console`, `/status`, etc.) remain accessible even while attached — if a command is not found on the bot, dispatch falls through to the system command path.

### Console Readline Integration

The console uses GNU Readline for interactive input, providing shell-like conveniences:

- **Line editing**: full emacs-mode key bindings (Ctrl+A/E for line start/end, Ctrl+W for word delete, Ctrl+K/U for line kill, Alt+B/F for word movement, etc.).
- **Persistent command history**: history is saved to `core.console.history.file` (default `~/.config/botmanager/history`) and loaded on startup. Maximum size is controlled by `core.console.history.size` (default 1000). History expansion is supported: `!!` (repeat last), `!prefix` (repeat last matching), `^old^new` (quick substitution), `!$` (last argument), `!:p` (print without executing). Duplicate suppression prevents repeated commands from flooding history.
- **Tab completion**: context-sensitive completions for command names, subcommands, bot names, KV keys, usernames, group names, namespace names, and method kinds. At the start of a line, all system commands (and bot commands when attached) are offered. After a parent command, its subcommands are completed. Specific commands have argument-aware completers (e.g., `/console attach` completes running bot names, `/set` completes KV keys).
- **History management**: `/history list [count]` shows recent entries, `/history clear` wipes all history, `/history search <pattern>` finds matching entries.
- **Screen clearing**: `/clear` (abbreviation `cls`) clears the terminal screen and redraws the prompt.
- **Ctrl+C behavior**: clears the current input line instead of triggering shutdown. Use `/quit` or send SIGTERM from another terminal to shut down.
- **Ctrl+D behavior**: on an empty line, prints a hint ("Use /quit to exit.") instead of terminating. Prevents accidental exits.
- **Dynamic prompt**: controlled by `core.console.prompt.format` (default `{bot}:{user}> `) with token substitution: `{bot}` (attached bot name), `{user}` (associated username, empty for @owner), `{time}` (current HH:MM:SS). Color-coded via `core.console.prompt.color` (default 1/enabled): bot name in green, username in cyan.
- **Thread-safe output**: CLAM log messages and `console_print()` calls from other threads coordinate with readline via `console_output_lock()`/`console_output_unlock()`, saving and restoring the prompt and input line to prevent display corruption.

- The botmanctl method is provided by the core for out-of-band operator control via a Unix domain socket. See Botmanctl Method below.

## Botmanctl Method

The botmanctl method provides programmatic operator control over a Unix domain socket. It is a core-provided method (like the console) that enables external tools, scripts, and AI agents to issue system commands without access to stdin/stdout.

- Listens on a Unix domain socket (default path: `~/.config/botmanager/botman.sock`, configurable via `core.botmanctl.sockpath`).
- Accepts client connections and reads newline-delimited commands.
- Commands are dispatched through the same system command path as the console, always executing as the `@owner` identity.
- Responses are sent back over the socket connection. Each response is terminated with a null byte (`\0`) so the client knows when the response is complete.
- Uses a persist task with `poll()` to monitor the listener socket and any connected client, following the same pattern as the console's stdin reader.
- One client connection is served at a time. Commands are executed synchronously — the client sends a command, receives the full response, and may then send another command or disconnect.
- The socket file is created on startup and unlinked on shutdown.

### Botmanctl Protocol

The protocol is line-oriented and minimal:

1. The client connects to the Unix domain socket.
2. The client sends a command as a single line terminated by `\n`. Commands do not include a `/` prefix — the method prepends it internally before dispatching (matching how the console strips the `/` from operator input).
3. The server executes the command synchronously and sends the response as one or more lines, each terminated by `\n`.
4. The server sends a null byte (`\0`) to signal end-of-response.
5. The client may send another command (goto step 2) or disconnect.

```
Client sends:  "status\n"
Server sends:  "Uptime: 2h 15m\nThreads: 4\n...\n\0"
Client sends:  "bot list\n"
Server sends:  "mybot  command  running\n\0"
```

### Botmanctl CLI Utility

A companion standalone utility (`botmanctl`) is provided for interacting with the botmanctl method from the command line. It is a simple Unix socket client with no dependency on core libraries.

- **One-shot mode** (arguments provided): joins all arguments into a single command string, sends it, reads the response until `\0`, prints it, and exits. Example: `botmanctl status`, `botmanctl bot list`.
- **Interactive mode** (no arguments): presents a prompt and loops, sending each line as a command and printing the response.
- The socket path can be overridden with `-s <path>`.

## Method Instance Lifecycle: Demand-Driven

Method plugins do NOT create connections at startup. A method plugin's role at startup is limited to:
1. Registering its driver with the method subsystem.
2. Registering any system commands it provides.
3. Making its plugin-level and instance-level KV schemas available via the plugin descriptor.

Method instances are created on demand by the bot subsystem when a bot instance is configured and started. The flow is:

1. The owner defines shared resources at the plugin level (e.g., IRC network definitions under `plugin.irc.network.*`).
2. The owner configures a bot instance, specifying one or more methods and their instance-level settings (e.g., `bot.mybot.method.irc.network`, `bot.mybot.method.irc.nick`).
3. When `bot_start()` is called, it creates a method instance for each configured method by calling the method plugin's driver.
4. The method instance loads its instance-level configuration from the bot's KV namespace and resolves any referenced plugin-level resources (e.g., looking up the named IRC network to get the server list).
5. The method instance initiates its connection and progresses through ENABLED → RUNNING → AVAILABLE.
6. The bot subscribes to the method instance and begins receiving messages.

A bot instance supports at most one method instance of each kind. If a bot needs to be present on two IRC networks simultaneously, the owner creates two bot instances (one per network). Bot instances are lightweight and can share a user namespace, so this adds minimal overhead while keeping configuration simple.

This means:
- Multiple bot instances can each have their own independent IRC connection (different servers, nicks, channels).
- A method plugin with no bot instances using it consumes zero network resources.
- On a fresh installation with no bots configured, no connections are attempted — the program starts cleanly and waits for the owner to configure via the console.

## Method Configuration Namespace

Method configuration is split across two levels:

### Plugin-Level Configuration

Plugin-level settings live under `plugin.<kind>.*` and are shared across all bot instances that use this method. These are typically resource definitions that multiple bots may reference.

For IRC, this means network definitions. Each network has an ordered list of servers and network-level settings:

```
plugin.irc.network.libera.server.0.host = irc.libera.chat
plugin.irc.network.libera.server.0.port = 6697
plugin.irc.network.libera.server.1.host = irc2.libera.chat
plugin.irc.network.libera.server.1.port = 6697
plugin.irc.network.libera.pass =
plugin.irc.network.libera.reconnect_delay = 30

plugin.irc.network.efnet.server.0.host = irc.efnet.org
plugin.irc.network.efnet.server.0.port = 6667
plugin.irc.network.efnet.reconnect_delay = 60
```

Network definitions are dynamic — they are created and managed at runtime via direct KV manipulation. The IRC plugin's plugin-level KV schema defines only the structural template, not actual network entries. Servers within a network are tried in order (0, 1, 2, ...) during connection and failover.

### Instance-Level Configuration

Instance-level settings live under the bot's KV namespace:

```
bot.<botname>.method.<kind>.<key>
```

These are registered automatically by the core when a bot binds to a method, using the method plugin's instance-level KV schema (`kv_inst_schema`). They define identity and behavior specific to this bot instance.

For IRC:
```
bot.mybot.method.irc.network = libera
bot.mybot.method.irc.nick = MyBot
bot.mybot.method.irc.nick2 = MyBot_
bot.mybot.method.irc.nick3 = MyBot__
bot.mybot.method.irc.user = mybot
bot.mybot.method.irc.realname = BotManager
```

The `network` key references a plugin-level network definition by name. The `nick`, `nick2`, and `nick3` keys provide ordered nickname preferences — IRC does not allow duplicate nicknames, so if the preferred nick is in use, the method tries the fallbacks in order.

### Per-Channel Configuration

Per-channel settings for IRC are stored dynamically under the bot's KV namespace:

```
bot.<botname>.method.irc.chan.<channel>.<key>
```

The `#` prefix is stripped from the channel name in the KV key (the code prepends it when joining). Channel entries are created and managed at runtime via direct KV manipulation — there is no static channel list.

```
bot.mybot.method.irc.chan.general.autojoin = 1
bot.mybot.method.irc.chan.general.key =
bot.mybot.method.irc.chan.general.announce = 1
bot.mybot.method.irc.chan.general.announcetext = Hello, I'm ${name} v${version}

bot.mybot.method.irc.chan.secret.autojoin = 1
bot.mybot.method.irc.chan.secret.key = hunter2
bot.mybot.method.irc.chan.secret.announce = 0

bot.mybot.method.irc.chan.bottest.autojoin = 0
bot.mybot.method.irc.chan.bottest.announce = 0
```

Per-channel keys:
- **autojoin** (UINT8): If enabled, the bot joins this channel automatically after connecting to the server.
- **key** (STR): Channel key (password) required to join, if any.
- **announce** (UINT8): If enabled, the bot sends a message to the channel upon successfully joining.
- **announcetext** (STR): The announcement message. Supports variable substitution using `${var}` syntax: `${name}` (bot instance name), `${version}` (BotManager version), `${nick}` (current nick), `${channel}` (channel name).

On connect, the IRC method iterates `bot.<botname>.method.irc.chan.*`, finds all channels with `autojoin = 1`, and joins them — passing the `key` if set. After a successful join, if `announce` is enabled, the `announcetext` is expanded and sent to the channel.

# Plugin Type: Bot

Bot plugins define bot behavior. The bot's kind determines how it interprets incoming messages — the core and method plugins are uninvolved in this decision.

- Bot plugins have a "kind" sub-category that defines their purpose at a high level.
- All bot kinds that interact with humans share a set of built-in commands: help and version. Additional commands can be registered by other plugins and enabled per bot instance. Administrative commands (set, show, status, quit, help) are registered by the admin module (`admin.c`) as system-level commands that are always dispatchable.
- Command registrations include: default group name, default permission level (uint16_t), a brief one-line usage syntax, a brief help description, an optional verbose help string (up to 1024 characters), an optional parent command name (for subcommands), and an optional abbreviation. Command code must be non-blocking and thread-safe.

### Hierarchical Subcommands

Commands support a parent/child hierarchy via the `parent_name` parameter in `cmd_register()` and `cmd_register_system()`. When `parent_name` is non-NULL, the new command is linked as a child of the specified parent. During dispatch, if a matched command has children, the next token from the argument string is checked against the parent's children. If a child matches, dispatch continues to the child with the remaining arguments. If no child matches, the parent's callback is invoked with the original arguments.

Subcommand resolution is iterative, supporting multi-level nesting (e.g., `/show irc networks` resolves `show` → `irc` → `networks`). Parent commands typically implement a handler that lists available subcommands when invoked without arguments.

Name collision detection is scope-aware: root-level commands only collide with other root-level commands, and subcommands only collide with siblings under the same parent. This allows a subcommand to share a name with a root-level command (e.g., "status" can exist as both a root command and a subcommand of "show").

### Command Abbreviations

Each command can declare an optional abbreviation (e.g., "show" → "sh", "status" → "stat"). Abbreviations are checked during command lookup: exact name match takes priority, then abbreviation match. Abbreviation uniqueness is enforced within the same scope (root-level or siblings). The help system displays abbreviations alongside command names.

### Per-Method Command Prefix

The command prefix (the character(s) that indicate a command, e.g., "!" or "/") is configurable per method per bot instance via KV:

```
bot.<botname>.method.<kind>.prefix
```

For example, `bot.mybot.method.irc.prefix` defaults to "!" and `bot.mybot.method.console.prefix` defaults to "/". This allows different prefixes on different methods of the same bot.

During `cmd_dispatch()`, the prefix is resolved from the method-specific KV key. If the key doesn't exist or is empty, the bot-level prefix (set via `cmd_set_prefix()`) is used as the fallback. The existing `cmd_set_prefix()` / `cmd_get_prefix()` API sets the bot-level default.
- Examples of bot kinds:
  - **command**: Interacts with humans via structured commands (e.g., `!help`, `!status`). Checks for a command prefix, dispatches to the appropriate handler via the task system, and responds on the originating method. This is the initial bot kind for this project.
  - **scraper**: Uses HTTP/HTTPS to scrape data from a website.
  - **rest**: Interacts with a REST API.
  - **websocket**: Interacts with a WebSocket API.
  - Other kinds TBD — additional bot kinds (e.g., LLM-powered conversational bots, algorithmic trading bots) will be developed as separate projects that build plugins against BotManager's plugin API.

# Plugin Type: Service

Service plugins integrate with external APIs and data sources. They are distinct from other plugin types:

- **Bot plugins** define behavior — how to interpret incoming messages and what to do with them.
- **Method plugins** provide communication channels — how to reach humans on a specific platform.
- **Service plugins** provide data and capabilities — they connect to external APIs and make the results available as user commands.

Service plugins are method-independent. They register user commands (e.g., `!weather`, `!forecast`) via `cmd_register()`, and any command bot instance can enable those commands. When a user invokes a service command, the service plugin processes the request and responds via `cmd_reply()`, which routes the response back through whichever method the user is on. The service plugin never knows or cares whether the user is on IRC, Telegram, or the console.

## Service Plugin Architecture

- Service plugins declare `PLUGIN_SERVICE` as their plugin type and have a "kind" sub-category identifying the specific service (e.g., "openweather", "stocks").
- Service plugins require the "bot_command" feature, ensuring the command bot plugin is loaded before they are.
- Service plugins use the core's libcurl subsystem for HTTP/WebSocket communication with external APIs. They do not manage their own network connections.
- API keys and service-specific configuration are stored in the KV namespace under `plugin.<servicename>.*` (e.g., `plugin.openweather.apikey`, `plugin.openweather.units`).
- Service plugins process requests asynchronously. When a user command is dispatched, the service plugin submits work to the task system. HTTP requests are made via libcurl callbacks. The originating method's input thread is never blocked.
- Multi-step API flows (e.g., resolve a zipcode to coordinates, then query weather for those coordinates) are handled via linked tasks or chained libcurl callbacks.
- Service plugins use `cmd_reply()` to send results back to the user. Response formatting is the service plugin's responsibility.

## Service Plugin Lifecycle

1. **init**: Register user commands via `cmd_register()`. Declare KV schema for API keys and configuration. Validate that required configuration (e.g., API key) is present.
2. **start**: Begin accepting command dispatches. Optionally perform startup validation (e.g., test API connectivity).
3. **stop**: Drain in-flight API requests. Cancel pending tasks.
4. **deinit**: Unregister commands. Release resources.

## Misc Plugin Type

Plugins that only provide user commands (i.e., they have no external API integration, no KV configuration, and no async state — they simply register commands) use the `misc` plugin type and live under `plugins/misc/`.

- Directory: `plugins/misc/<name>/`
- Plugin name: `misc_<name>` (underscore for KV namespace compatibility)
- Examples: `math`, `misc-dice`, `misc-convert`

Service plugins that integrate with external APIs (e.g., OpenWeather, stock APIs) use the `service` plugin type.

## Service Plugin Directory

Service plugins are stored under `plugins/service/<servicename>/` following the standard plugin directory convention.

# Bot Instances

- Bots are instantiated. At least one bot instance is required for meaningful operation, but the system starts cleanly with zero bots on a fresh installation.
- Each bot instance has a unique name, and this name is used as the root of its KV namespace (bot.botname.*).
- A bot instance binds together: a bot plugin, one or more method instances, optionally a user namespace, and optionally a personality.
- Each method instance is owned by the bot that created it. The bot's KV namespace contains the method's instance-level configuration (e.g., `bot.mybot.method.irc.network`, `bot.mybot.method.irc.nick`). The method resolves any referenced plugin-level resources (e.g., looking up the named IRC network for server addresses). When the bot starts, it creates and connects its method instances. When the bot stops, its method instances are disconnected and destroyed.
- Bots can be instantiated at program boot time, manually by the operator, or programmatically by another bot.
- There can be multiple instances of the same bot plugin, each with its own unique configuration.
- There can also be multiple instances of different bot plugins running simultaneously.
- On a fresh installation with no bots configured, the program starts, initializes all subsystems, and presents the owner with a welcome message and command index via the console. The owner then configures the first bot instance using `/set` commands.

## Per-Bot KV Settings

Each bot instance has configurable KV settings under `bot.<botname>.*`:

- `bot.<botname>.maxidleauth` (UINT32, default 3600 seconds, 0 = never expire): Controls how long an authenticated session can remain idle before being automatically expired. A periodic task scans sessions and removes any where `time(NULL) - last_seen` exceeds this threshold.
- `bot.<botname>.userdiscovery` (UINT8, default 0 / disabled): When enabled, the bot auto-creates user accounts for unknown individuals encountered on its methods. See User Namespaces § User Discovery for details.
- `bot.<botname>.method.<kind>.prefix` (STR): Per-method command prefix. See Command system § Per-Method Command Prefix for details.

# Fault Tolerance

Fault tolerance in BotManager applies at the operational layer, not the infrastructure layer. If a core subsystem or plugin module fails in a way that compromises system integrity, the program will gracefully exit and log the reason.

Operational fault tolerance means that bots handle the unpredictable nature of external services gracefully:

- API calls that fail (HTTP errors, timeouts, connection refused) are handled without terminating the program. Bots will log the failure, back off, and retry as appropriate.
- Network interruptions to external services (REST APIs, WebSocket connections, IRC servers) are treated as transient. The bot or method plugin is responsible for reconnection logic.
- Malformed or unexpected responses from external services are logged and handled defensively. The bot continues operating.
- libcurl operations include configurable timeouts and error handling. A single failed request does not cascade.
- Method plugins that lose their connection (e.g., IRC disconnect) transition from "available" back to "running" and attempt reconnection. Bots using that method are notified of the state change.

The guiding principle: external failures are expected and normal. BotManager keeps running.

# Error Handling

- Core functions use a consistent return convention: SUCCESS (0/false) and FAIL (1/true).
- Fatal errors (e.g., out of memory, core subsystem failure, plugin module failure) trigger a graceful shutdown via the shutdown sequence, with the reason logged through CLAM.
- Non-fatal errors (e.g., a failed API call, a malformed message) are logged and handled locally. They do not propagate upward to terminate the program.
- POSIX signals are caught and handled: SIGTERM and SIGINT trigger graceful shutdown. Other signals are handled as appropriate.

# Input Validation

All user-facing commands — whether received via IRC, botmanctl, the console, or any future method — MUST validate their input before acting on it. User input arrives from untrusted sources and must never be passed unchecked to API calls, URL construction, file operations, shell commands, or database queries.

## Principles

- **Validate at the boundary.** Every command handler that accepts user arguments must validate those arguments as its first action, before any processing or API calls. Invalid input is rejected immediately with a clear error message.
- **Allowlist, not blocklist.** Validate that input matches what IS expected (e.g., a zipcode is 5-10 digits optionally followed by a country code) rather than trying to filter out what ISN'T expected. Attackers are creative; the set of bad inputs is unbounded.
- **Minimal acceptance.** Accept only the narrowest input that makes sense for the command. If a command expects a US zipcode, reject anything that isn't digits of the right length. If a command expects a username, reject characters outside the allowed set.
- **Fail clearly.** When input is rejected, the error message should tell the user what was expected (e.g., "Usage: weather <zipcode>" or "Invalid zipcode: must be 5 digits") without echoing back the raw invalid input verbatim (to avoid injection into the response channel).
- **Defense in depth.** Input validation in the command handler is the primary defense, but functions that construct URLs, SQL queries, or other structured strings should also be hardened against unexpected content. This is not a substitute for handler-level validation — it is an additional safety net.

## Scope

This principle applies universally to all current and future commands that accept user-provided arguments. Specifically:

- Plugin commands registered via `cmd_register()` that accept user arguments (e.g., `!weather <zipcode>`, `!forecast [-h] <zipcode>`).
- System commands registered via `cmd_register_system()` that accept arguments (e.g., `/set`, `/bot add`, `/useradd`).
- Any future command, service plugin, or bot kind that processes external input.

System commands invoked via the console or botmanctl operate in a trusted context (the operator), but should still validate arguments to prevent accidental misconfiguration and to maintain consistent defensive coding practices.

## Validation Helpers

`include/validate.h` and `core/validate.c` provide reusable validation functions for common input patterns. Plugin authors should use these helpers rather than writing ad-hoc validation logic.

| Function | Description |
|---|---|
| `validate_alnum(str, maxlen)` | Accepts alphanumeric characters and underscores. Returns false if NULL, empty, or exceeds maxlen (0 = no limit). Use for bot names, usernames, namespace names. |
| `validate_digits(str, minlen, maxlen)` | Accepts ASCII digits only, within length bounds. Use for numeric-only fields. |
| `validate_hostname(str)` | Accepts alphanumeric characters, dots, hyphens, and colons (IPv6). Use for server hostnames and IP addresses. |
| `validate_port(str, *out)` | Parses a decimal port string and validates range 1-65535. Writes the parsed value to `*out` if non-NULL. |
| `validate_irc_channel(str)` | Rejects spaces, control characters (0x00-0x20, 0x07), and commas. Use for IRC channel names (without the leading `#`). |

KV keys use a dot-separated namespace format (`core.sock.timeout`, `bot.mybot.method.irc.host`) and are validated inline as alphanumeric + dots + underscores.

## Declarative Argument Specs

Commands can declare their expected arguments at registration time via `cmd_arg_desc_t` descriptors. When a command has an arg spec, the framework automatically tokenizes, validates, and rejects invalid input *before* calling the handler callback. This eliminates the manual parsing boilerplate that was previously duplicated across every handler.

### Registration

`cmd_register()` and `cmd_register_system()` accept two additional parameters: `const cmd_arg_desc_t *arg_desc` (array of descriptors, or NULL) and `uint8_t arg_count` (number of entries, or 0). Commands that pass NULL/0 work exactly as before — the handler receives `ctx->args` as a raw string and `ctx->parsed` is NULL.

### Argument Descriptor

Each `cmd_arg_desc_t` entry describes one positional argument:

| Field | Type | Description |
|---|---|---|
| `name` | `const char *` | Display name for error messages (e.g., "username") |
| `type` | `cmd_arg_type_t` | Validation type (see below) |
| `flags` | `uint8_t` | `CMD_ARG_REQUIRED` (default), `CMD_ARG_OPTIONAL`, `CMD_ARG_REST` |
| `maxlen` | `size_t` | Maximum length (0 = CMD_ARG_SZ - 1) |
| `custom` | `cmd_arg_validator_t` | Custom validator function (CMD_ARG_CUSTOM only) |

### Validation Types

| Type | Validator | Description |
|---|---|---|
| `CMD_ARG_NONE` | (none) | Any non-empty string |
| `CMD_ARG_ALNUM` | `validate_alnum()` | Alphanumeric + underscores |
| `CMD_ARG_DIGITS` | `validate_digits()` | Digits only |
| `CMD_ARG_HOSTNAME` | `validate_hostname()` | Hostname characters |
| `CMD_ARG_PORT` | `validate_port()` | Port number 1-65535 |
| `CMD_ARG_CHANNEL` | `validate_irc_channel()` | IRC channel name |
| `CMD_ARG_CUSTOM` | user-supplied | Custom validator function |

### Flags

- `CMD_ARG_REQUIRED` (0x00): argument must be present. All required args must precede optional args.
- `CMD_ARG_OPTIONAL` (0x01): argument may be omitted. The handler checks `ctx->parsed->argc` to know which optional args were provided.
- `CMD_ARG_REST` (0x02): capture the remainder of the input line as a single argument instead of tokenizing at whitespace. Only valid on the last descriptor. Useful for passwords, descriptions, and MFA patterns.

### Pre-Parsed Arguments

When an arg spec is present and validation passes, the handler receives `ctx->parsed` pointing to a `cmd_args_t`:

```c
typedef struct {
  const char *argv[CMD_MAX_ARGS];  // pointers into parsed token buffers
  uint8_t     argc;                // number of arguments actually parsed
} cmd_args_t;
```

Handlers access arguments as `ctx->parsed->argv[0]`, `ctx->parsed->argv[1]`, etc. The raw `ctx->args` string is still available for backward compatibility.

### Error Handling

When validation fails, the framework automatically replies with an error message and does *not* call the handler:
- Missing required args: `"Usage: <usage_string>"`
- Validation failure: `"invalid <name> (<reason>)"` (e.g., "invalid username (alphanumeric and underscores only)")

### Example

```c
static const cmd_arg_desc_t ad_useradd[] = {
  { "namespace", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_NAME_SZ, NULL },
  { "username",  CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_USER_SZ, NULL },
  { "password",  CMD_ARG_NONE,  CMD_ARG_REQUIRED | CMD_ARG_REST, 0, NULL },
};

cmd_register_system("admin", "useradd",
    "useradd <namespace> <username> <password>",
    "Create a user in a namespace", NULL,
    USERNS_GROUP_ADMIN, 100, admin_cmd_useradd, NULL, NULL, NULL,
    ad_useradd, 3);

// Handler — no manual parsing needed:
static void admin_cmd_useradd(const cmd_ctx_t *ctx)
{
  const char *ns_name  = ctx->parsed->argv[0];
  const char *username = ctx->parsed->argv[1];
  const char *password = ctx->parsed->argv[2];
  // ... business logic only ...
}
```

# Startup and Shutdown

## Startup Sequence

1. Parse command-line arguments.
2. Read bootstrap configuration file.
3. Initialize core subsystems in dependency order:
   1. CLAM (logging must be available first; built-in stdinout subscriber provides immediate stdout output).
   2. Memory management.
   3. Console method (pure input/output module; CLAM's stdinout subscriber handles log output independently).
   4. Thread pool and task system.
   5. DB subsystem (establishes connection pool using bootstrap config).
   6. Configuration (loads KV settings from database).
   7. Plugin loader (discovers, loads, and initializes plugins per dependency order).
   8. Bot instances (instantiates bots defined in configuration).
4. Parent thread enters the task system as worker slot 0.

## Shutdown Sequence

Shutdown is the reverse of startup, ensuring resources are released cleanly:

1. Bot instances are stopped (in-flight work is drained).
2. Plugins are stopped and unloaded in reverse dependency order.
3. Thread pool is drained (workers complete current tasks, then retire and join).
4. Configuration is flushed.
5. DB connections are closed and pool is destroyed.
6. Task system is finalized.
7. Console method is stopped.
8. Memory management reports any unfreed allocations (leak detection) and shuts down.
9. CLAM is shut down last.

# Example Use Case

A command bot on IRC providing weather data:

- A plugin provides access to a public weather service REST API and registers user commands (`!weather`, `!forecast`).
- A bot instance of kind "command" is created with an IRC method, linked to a user namespace.
- A user on IRC authenticates with `!identify alice secretpass123!`.
- The user types `!weather 90210`. The bot recognizes the command prefix, verifies the user has permission, submits a task to the work queue, and immediately returns to listening.
- A worker thread picks up the task, calls the weather API via libcurl, formats the response, and sends it back to the user via the IRC method.
- If the weather API is unreachable, the bot responds with an error message and continues operating.

Due to the extensible nature of BotManager, a single instance might manage hundreds of bots of various kinds across multiple methods simultaneously.
