# Plugin Architecture

This document is for AI agents implementing new plugins. It covers the plugin descriptor, lifecycle, dependency resolution, KV configuration, driver interfaces, and build integration.

For adding user commands specifically, see CMD.md.

**Working reference implementations** (always up-to-date with actual API):
- Command-surface: `plugins/cmd/math/`
- Service: `plugins/service/openweather/`
- Protocol: `plugins/protocol/irc/`
- Method: `plugins/method/command/`
- Feature: `plugins/feature/whenmoon/`
- DB: `plugins/db/postgresql/`

## Overview

Plugins are shared libraries (`.so`) loaded at runtime via `dlopen()`. Each exports a single symbol `bm_plugin_desc` of type `plugin_desc_t`. The core discovers, validates, resolves dependencies, and manages lifecycles automatically.

Plugins live under `plugins/<type>/<kind>/` and are built as shared libraries via meson.

## Layer Rules

botmanager plugins are organised into layers, with strict `#include`
and `plugin_dlsym` directionality. The rule is simple: **dependencies
flow downward only**.

| Layer | Directory | Role | May depend on |
|---|---|---|---|
| Core | `core/`, `include/` | Daemon process + unified command registry + subsystems (bot, method, cmd, task, kv, sock, db, clam, ...) | itself |
| Service | `plugins/service/*/` | Pure API connectivity (HTTP / REST / transport to an external provider). Exposes a mechanism API in `<name>_api.h`. | core |
| Exchange | `plugins/exchange/*/` | Exchange-facing protocols (Coinbase, Kraken, ...). Exposes a mechanism API in `<name>_api.h`. | core + service |
| Command-surface | `plugins/cmd/*/` | Registers user commands that wrap service-plugin mechanisms or contain self-contained domain logic (math, smallgames). | core + any service plugin's `*_api.h` |
| Method | `plugins/method/*/` | Owns bot-interaction-method-specific state and the commands that mutate it (chat plugin's `/dossier`, `/llm`, `/memory`; command plugin's slash dispatcher). | core + service + feature |
| Feature | `plugins/feature/*/` | Capability layers composed atop methods (whenmoon trading, future inference-as-feature). | core + service + exchange |
| Inference | `plugins/inference/` | LLM, knowledge, acquire mechanism APIs (dlsym shims). | core + service |
| Protocol | `plugins/protocol/*/` | Wire protocols (IRC, botmanctl). | core + service |
| DB | `plugins/db/*/` | Database drivers (postgresql, ...). | core + service |

Hard rules:

1. **Service plugins register zero user commands.** Move the command
   surface into `plugins/cmd/<name>/` and `plugin_dlsym` *down* into
   the service plugin's `<name>_api.h`. Rationale: as soon as a
   service plugin owns a command, it is tempted to produce
   chat-specific side effects (dossier facts, memory upserts) after
   the command runs — a structural violation of downward-only
   dependencies. Keeping service plugins mechanism-only prevents the
   whole category. See `plugins/service/AGENTS.md`.
2. **No upward includes or `plugin_dlsym`.** Service, protocol, db,
   and inference plugins must not `#include` headers from method or
   feature plugins, must not reference method/feature-plugin types,
   and must not resolve method/feature-plugin symbols via
   `plugin_dlsym`. A leak in that direction is an architectural bug.
3. **Chat-specific side effects live in the chat plugin's NL bridge.**
   If the requirement is "after `/foo` runs, record a dossier fact
   about the sender", the observer belongs in
   `plugins/method/chat/nl_observe.c`, attached to the typed slot
   metadata carried on `cmd_nl_t`. See the `CMD_NL_ARG_LOCATION`
   observer as the reference implementation.

Grep audits (from `plugins/service/AGENTS.md`) that must stay clean:

```sh
# Service / protocol / db plugins must not reference method-plugin state.
grep -rn 'dossier\|memory_\|chatbot\|chat_user' \
    plugins/service/ plugins/protocol/ plugins/db/
# expected: 0 matches (outside prose comments explaining the rule)

# Service plugins must not register commands.
grep -rn 'cmd_register\|cmd_unregister' plugins/service/
# expected: 0 matches

# Downward-only plugin_dlsym: nothing upstream resolves "chat".
grep -rn 'plugin_dlsym[[:space:]]*(.*"chat"' \
    plugins/service/ plugins/protocol/ plugins/db/ plugins/inference/
# expected: 0 matches

# Cross-plugin includes: service/protocol/db/inference must not include
# method-plugin headers.
grep -rn '#include[[:space:]]*"dossier.h"\|#include[[:space:]]*"memory.h"\|#include[[:space:]]*"chatbot.h"' \
    plugins/service/ plugins/protocol/ plugins/db/ plugins/inference/
# expected: 0 matches
```

`AGENTS.md §Plugin Layers` is a quick-reference summary of this
section.

## Plugin Types

```c
typedef enum {
  PLUGIN_CORE,         // extends core functionality
  PLUGIN_DB,           // database engine driver (ext = db_driver_t*)
  PLUGIN_PROTOCOL,     // human interaction protocol (ext = method_driver_t*)
  PLUGIN_METHOD,       // bot interaction method (ext = bot_driver_t*)
  PLUGIN_SERVICE,      // external API integration (ext usually NULL)
  PLUGIN_MISC,         // miscellaneous user command extension (registers commands)
  PLUGIN_PERSONALITY,  // language/messaging personality
  PLUGIN_FEATURE,      // capability layer composed atop methods (ext = bot_driver_t*)
  PLUGIN_EXCHANGE      // exchange-facing protocol (ext usually NULL)
} plugin_type_t;
```

## Plugin Descriptor

Every plugin exports this struct as the `bm_plugin_desc` symbol:

```c
const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,   // must match (currently 13)
  .name            = "myplugin",           // unique name (max 64 chars)
  .version         = "1.0",               // version string (max 32 chars)
  .type            = PLUGIN_SERVICE,       // plugin type
  .kind            = "myplugin",           // kind identifier (max 64 chars)

  .provides        = { { .name = "service_myplugin" } },
  .provides_count  = 1,
  .requires        = { { .name = "method_command" } },  // or .requires_count = 0
  .requires_count  = 1,

  .kv_schema       = my_kv_schema,         // plugin-level config (or NULL)
  .kv_schema_count = 2,

  .kv_inst_schema       = NULL,            // per-bot instance config (or NULL)
  .kv_inst_schema_count = 0,

  .init            = my_init,              // register state (or NULL)
  .start           = NULL,                 // begin operation (or NULL)
  .stop            = NULL,                 // drain work (or NULL)
  .deinit          = my_deinit,            // final cleanup (or NULL)

  .ext             = NULL,                 // driver vtable for PROTOCOL/METHOD/FEATURE/DB types

  .kv_groups       = NULL,                 // dynamic entity schemas (or NULL)
  .kv_groups_count = 0,
};
```

### Size Limits

| Constant | Value | Used For |
|----------|-------|----------|
| `PLUGIN_API_VERSION` | 13 | Must match at load time |
| `PLUGIN_NAME_SZ` | 64 | name, kind |
| `PLUGIN_VER_SZ` | 32 | version |
| `PLUGIN_FEATURE_SZ` | 64 | feature names |
| `PLUGIN_MAX_FEATURES` | 16 | max provides/requires |

## Lifecycle

Plugins transition through these states:

```
DISCOVERED -> LOADED -> INITIALIZED -> RUNNING -> STOPPING -> LOADED -> UNLOADED
```

### Callback Invocation Order

| Phase | Function | Direction | Callback | Transition |
|-------|----------|-----------|----------|------------|
| Load | `plugin_discover()` | -- | -- | DISCOVERED -> LOADED |
| Init | `plugin_init_all()` | dependency order | `init()` | LOADED -> INITIALIZED |
| Start | `plugin_start_all()` | dependency order | `start()` | INITIALIZED -> RUNNING |
| Stop | `plugin_stop_all()` | **reverse** dependency order | `stop()` | RUNNING -> STOPPING |
| Deinit | `plugin_deinit_all()` | **reverse** dependency order | `deinit()` | STOPPING -> LOADED |
| Unload | `plugin_exit()` | reverse order | `dlclose()` | LOADED -> UNLOADED |

Providers initialize before dependents. Dependents stop/deinit before providers.

### What Each Callback Should Do

- **`init()`**: Register commands, allocate module state, initialize mutexes. KV schema is already registered by the framework before `init()` is called. Return `SUCCESS` or `FAIL`.
- **`start()`**: Begin active operation (e.g., make a driver available). Often NULL for service plugins. Return `SUCCESS` or `FAIL`.
- **`stop()`**: Signal active work to drain. Often NULL for service plugins.
- **`deinit()`**: Unregister commands, free state, destroy mutexes.

## Dependency Resolution

Plugins declare features they provide and features they require. The core uses topological sort (Kahn's algorithm) to determine initialization order.

### Feature Naming Convention

| Plugin Type | Provides Pattern | Example |
|-------------|-----------------|---------|
| Protocol | `protocol_<kind>` | `protocol_irc` |
| Method | `method_<kind>` | `method_chat`, `method_command` |
| Feature | `feature_<kind>` | `feature_whenmoon` |
| Exchange | `exchange_<kind>` | `exchange_coinbase` |
| Service | `service_<kind>` | `service_openweather` |
| Misc | `misc_<kind>` | `misc_misc_math` |
| DB | `db_<kind>` | `db_postgresql` |

The `core_` prefix is **reserved** for synthetic providers
registered by the core binary itself.

### Core-provided features

The core binary registers a fixed set of synthetic providers at
startup so plugins can declare core dependencies in the same
`.requires` list they use for plugin-to-plugin dependencies.

Declaring `.requires = { { .name = "core_llm" } }` has two
effects:

1. **Self-documentation.** An agent reading the descriptor sees
   which core services the plugin consumes without grepping.
2. **Future-proofing.** If a future build strips out a core
   service (e.g. a minimal build without `llm`), `plugin_resolve()`
   will refuse to load dependents.

Synthetic providers carry no `.so` handle and no lifecycle
callbacks — they are pure dependency-graph markers. They appear
in `/show plugin` with `type=core`, an empty `kind=core_<feature>`,
state `running`, and no path.

Currently registered:

```
core_alloc     core_bot       core_botmanctl  core_clam
core_cmd       core_curl      core_db         core_json
core_kv        core_method    core_plugin     core_pool
core_resolve   core_sig       core_sock       core_sse
core_task      core_userns    core_util
```

Full list with subsystem notes: `include/AGENTS.md`. The former
`core_llm`, `core_knowledge`, and `core_acquire` entries were retired
when those subsystems moved into the `plugins/inference/` plugin;
dependents now declare `.requires = { "inference" }` instead.

### Common Dependency Patterns

**Service plugin depending on command method:**
```c
.requires = { { .name = "method_command" } },
.requires_count = 1,
```

**No dependencies:**
```c
.requires_count = 0,
```

**Multiple provides (rare):**
```c
.provides = { { .name = "feature_a" }, { .name = "feature_b" } },
.provides_count = 2,
```

Circular dependencies are detected and rejected at resolve time.

### Plugin-to-Plugin Calls (dlsym shims)

Once plugin A declares `.requires = { "B" }`, the loader guarantees
that B's `init()` has run by the time A's `init()` runs. Calling
from A into B is then a matter of resolving B's symbols across the
`RTLD_LOCAL` boundary. The project convention is a **dlsym-shim
public header**: B ships a single header in its plugin directory
whose entries are `static inline` wrappers that resolve the real
symbol on first call and cache it atomically.

The worked example is `plugins/inference/inference.h`. Callers
`#include "inference/inference.h"` and call `llm_chat_submit(...)` /
`knowledge_retrieve_top_k(...)` / `acquire_register_topics(...)` as
if they were direct function calls. Under the hood, each shim looks
like this (reference: `plugins/inference/inference.h`, and the
original pattern from
`plugins/inference/acquire_reactive.c:acq_sxng_resolve`):

```c
static inline RET
<public_symbol>(ARGS)
{
  typedef RET (*fn_t)(ARGS);
  static fn_t cached = NULL;
  fn_t fn = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;
    u.obj = plugin_dlsym("inference", "<public_symbol>");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference",
          "dlsym failed: <public_symbol>");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(ARGS_UNPACKED));
}
```

Notes:
- `plugin_dlsym` (see `core/plugin.c:plugin_dlsym`) does the actual
  cross-plugin symbol resolution.
- The `union` launders the `void*`↔function-pointer conversion
  that strict POSIX forbids via a plain cast.
- Two threads entering a cold shim race benignly: both resolve,
  both `__atomic_store_n` the same pointer.
- Same-plugin translation units inside `plugins/inference/` don't
  want the shim block (it would collide with the real function
  bodies). The inference header gates that block behind an
  `INFERENCE_INTERNAL` macro which the plugin's own `*_priv.h`
  files define before including `inference.h`.

No new framework mechanism was added to support this pattern — it is
the same `plugin_dlsym` + `.requires`/`.provides` + topologically
ordered init that was already used for the `core_*` synthetic
providers. The inference plugin is just the first plugin-to-plugin
consumer of it at significant scale.

## KV Configuration

### Plugin-Level KV Schema

Global settings registered once. Keys use the `plugin.<kind>.*` namespace.

```c
static const plugin_kv_entry_t my_kv_schema[] = {
  { "plugin.myplugin.apikey",  KV_STR,    ""         },
  { "plugin.myplugin.timeout", KV_UINT32, "30"       },
  { "plugin.myplugin.enabled", KV_UINT8,  "1"        },
};
```

The framework registers these automatically before calling `init()`. Access values at runtime via `kv_get_str()`, `kv_get_uint()`, etc.

### Instance-Level KV Schema

Per-bot settings cloned when a bot binds to this method. Keys are bare suffixes that get prefixed as `bot.<botname>.<kind>.<suffix>`.

```c
static const plugin_kv_entry_t my_inst_kv_schema[] = {
  { "host",      KV_STR,    ""           },
  { "port",      KV_UINT16, "6667"       },
  { "nick",      KV_STR,    ""           },
  { "prefix",    KV_STR,    "!"          },
};
```

When bot "mybot" binds to method kind "myplugin", these become `bot.mybot.myplugin.host`, `bot.mybot.myplugin.port`, etc.

Relevant for `PLUGIN_PROTOCOL` plugins (per-bot connection identity, e.g. nick/network/channels) and `PLUGIN_METHOD` plugins (per-bot method state). Service and feature plugins typically use plugin-level KV only.

### KV Types

```c
typedef enum {
  KV_INT8,    KV_UINT8,
  KV_INT16,   KV_UINT16,
  KV_INT32,   KV_UINT32,
  KV_INT64,   KV_UINT64,
  KV_FLOAT,   KV_DOUBLE,   KV_LDOUBLE,
  KV_STR
} kv_type_t;
```

### KV Groups (Dynamic Entities)

For plugins that manage dynamic collections (e.g., IRC channels, servers), declare `plugin_kv_group_t` entries to make them introspectable:

```c
static const plugin_kv_group_t my_kv_groups[] = {
  {
    .name         = "channel",
    .description  = "Per-channel configuration",
    .key_prefix   = "bot.%s.myplugin.chan.%s.",
    .prefix_args  = 2,            // number of %s placeholders
    .cmd_name     = "channel",    // managing command name
    .schema       = chan_kv_schema,
    .schema_count = 4,
  },
};
```

## Driver Interfaces

### Protocol Driver (`PLUGIN_PROTOCOL`)

Set `ext = &my_method_driver` in the descriptor.

```c
typedef struct {
  const char *name;
  const color_table_t *colors;   // abstract-to-native color mapping

  void *(*create)(const char *inst_name);                          // allocate state
  void  (*destroy)(void *handle);                                  // free state
  bool  (*connect)(void *handle);                                  // initiate connection
  void  (*disconnect)(void *handle);                               // close connection
  bool  (*send)(void *handle, const char *target, const char *text); // send message
  bool  (*get_context)(void *handle, const char *sender,           // get MFA context
            char *ctx, size_t ctx_sz);
} method_driver_t;
```

Each callback receives the opaque `handle` returned by `create()`. Multiple instances can exist (one per bot that binds the method).

Provide a `color_table_t` to map abstract `CLR_*` markers to native codes:

```c
static const color_table_t my_colors = {
  .red = "\033[0;31m", .green = "\033[0;32m", .yellow = "\033[0;33m",
  .blue = "\033[0;34m", .purple = "\033[0;35m", .cyan = "\033[0;36m",
  .white = "\033[0;37m", .orange = "\033[0;33m", .gray = "\033[0;90m",
  .bold = "\033[1m", .reset = "\033[0m",
};
```

### Bot-Behaviour Driver (`PLUGIN_METHOD` / `PLUGIN_FEATURE`)

Set `ext = &my_bot_driver` in the descriptor. Methods (chat, command)
and features (whenmoon) share the same vtable shape.

```c
typedef struct {
  const char *name;

  void *(*create)(bot_inst_t *inst);                     // allocate state
  void  (*destroy)(void *handle);                        // free state
  bool  (*start)(void *handle);                          // begin operation
  void  (*stop)(void *handle);                           // stop operation
  void  (*on_message)(void *handle, const method_msg_t *msg);  // incoming message
} bot_driver_t;
```

The `on_message` callback is the main entry point for bot behavior. The command bot plugin dispatches to `cmd_dispatch()` here.

### DB Driver (`PLUGIN_DB`)

Set `ext = &my_db_driver` in the descriptor. See `include/db.h` for `db_driver_t`.

### Service Plugins (`PLUGIN_SERVICE`)

Typically set `ext = NULL`. Service plugins register commands in `init()` and do their work via those command callbacks. They depend on `method_command` to make their commands available.

## Build System

### Plugin meson.build Template

```meson
# Minimal (no extra dependencies, no local headers):
my_plugin = shared_library('myplugin',
  'myplugin.c',
  include_directories : [inc_include],
  override_options : ['b_lundef=false'],
  install : true,
)
```

```meson
# With local header and external dependency:
my_include = include_directories('.')

my_plugin = shared_library('myplugin',
  'myplugin.c',
  include_directories : [inc_include, my_include],
  dependencies : [json_c_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
```

```meson
# Multiple source files:
my_plugin = shared_library('myplugin',
  'myplugin.c',
  'myplugin_commands.c',
  include_directories : [gen_include, inc_include],
  override_options : ['b_lundef=false'],
  install : true,
)
```

Notes:
- `override_options : ['b_lundef=false']` is required -- plugins reference symbols from the host binary resolved at runtime.
- `inc_include` provides `include/*.h` (core headers).
- `gen_include` provides generated headers (e.g., version.h) -- only needed if your plugin uses `BM_VERSION_STR` or similar.
- Add `subdir('type/kind')` to `plugins/meson.build`.

### Directory Layout

```
plugins/
  meson.build                    # lists all plugin subdirs
  db/postgresql/
    pg.c, meson.build
  method/irc/
    irc.c, irc.h, irc_commands.c, meson.build
  bot/command/
    command.c, command.h, meson.build
  service/openweather/
    openweather.c, openweather.h, meson.build
  service/newplugin/             # your new plugin
    newplugin.c, newplugin.h, meson.build
```

### Header Convention

Plugin internal declarations go in a `.h` file under a `#ifdef PLUGIN_INTERNAL` guard:

```c
// newplugin.h
#ifndef BM_NEWPLUGIN_H
#define BM_NEWPLUGIN_H

// No public API -- loaded via dlopen.

#ifdef NEWPLUGIN_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "alloc.h"
#include "plugin.h"

// Constants, structs, statics, forward declarations here.

#endif // NEWPLUGIN_INTERNAL
#endif // BM_NEWPLUGIN_H
```

The `.c` file starts with:

```c
#define NEWPLUGIN_INTERNAL
#include "newplugin.h"
```

## Template: Minimal Service Plugin

```c
// newplugin.h
#ifndef BM_NEWPLUGIN_H
#define BM_NEWPLUGIN_H

#ifdef NEWPLUGIN_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "alloc.h"
#include "plugin.h"

#define NP_CTX "newplugin"

#endif
#endif
```

```c
// newplugin.c
#define NEWPLUGIN_INTERNAL
#include "newplugin.h"

static void
np_cmd_hello(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "Hello from newplugin!");
}

static bool
np_init(void)
{
  // Kind-agnostic: trailing kind_filter = NULL.
  if(cmd_register("newplugin", "hello",
      "hello",                                           // usage
      "Say hello",                                       // description
      NULL,                                              // help_long
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      np_cmd_hello, NULL, NULL, NULL,
      NULL, 0,                                           // arg_desc, arg_count
      NULL) != SUCCESS)                                  // kind_filter
    return(FAIL);

  // Kind-filtered variant (chat-kind bots only). `chat_kinds` is
  // static so the registry can keep the pointer.
  //
  //   static const char *const chat_kinds[] = { "chat", NULL };
  //   cmd_register(..., NULL, 0, chat_kinds);

  clam(CLAM_INFO, NP_CTX, "newplugin initialized");
  return(SUCCESS);
}

static void
np_deinit(void)
{
  cmd_unregister("hello");
  clam(CLAM_INFO, NP_CTX, "newplugin deinitialized");
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "newplugin",
  .version         = "1.0",
  .type            = PLUGIN_SERVICE,
  .kind            = "newplugin",
  .provides        = { { .name = "service_newplugin" } },
  .provides_count  = 1,
  .requires        = { { .name = "method_command" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = np_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = np_deinit,
  .ext             = NULL,
};
```

```meson
# meson.build
newplugin_plugin = shared_library('newplugin',
  'newplugin.c',
  include_directories : [inc_include],
  override_options : ['b_lundef=false'],
  install : true,
)
```

Then add to `plugins/meson.build`:
```meson
subdir('service/newplugin')
```

## Checklist: Adding a New Plugin

1. Create `plugins/<type>/<kind>/` directory
2. Create `<kind>.h` with internal guard and declarations
3. Create `<kind>.c` with descriptor, lifecycle callbacks, and command handlers
4. Create `meson.build` (shared_library, include dirs, dependencies)
5. Add `subdir('<type>/<kind>')` to `plugins/meson.build`
6. Build with `ninja -C build` and verify zero warnings from your files
7. Test: start botmanager, verify plugin loads and commands appear in `/help`

## Key Files Reference

| File | Contains |
|------|----------|
| `include/plugin.h` | `plugin_desc_t`, all plugin types/states/structs, `PLUGIN_API_VERSION` |
| `include/method.h` | `method_driver_t`, `method_msg_t`, `color_table_t` |
| `include/bot.h` | `bot_driver_t`, `bot_inst_t` |
| `include/db.h` | `db_driver_t` |
| `include/kv.h` | `kv_type_t`, `kv_register()`, `kv_get_*()` |
| `include/colors.h` | `CLR_*` macros, `color_table_t` |
| `core/plugin.c` | Discovery, loading, resolution, lifecycle management |
| `plugins/meson.build` | Plugin subdirectory list |
| `plugins/service/openweather/` | Reference: service plugin with KV config, commands, async work |
| `plugins/protocol/irc/` | Reference: protocol plugin with driver, instance KV, KV groups |
| `plugins/method/command/` | Reference: method plugin with driver, command dispatch |
| `plugins/feature/whenmoon/` | Reference: feature plugin with bot driver, market lifecycle |
