# Plugin Architecture

This document is for AI agents implementing new plugins. It covers the plugin descriptor, lifecycle, dependency resolution, KV configuration, driver interfaces, and build integration.

For adding user commands specifically, see USERCMD.md.

**Working reference implementations** (always up-to-date with actual API):
- Misc: `plugins/misc/math/`
- Service: `plugins/service/openweather/`
- Method: `plugins/method/irc/`
- Bot: `plugins/bot/command/`
- DB: `plugins/db/postgresql/`

## Overview

Plugins are shared libraries (`.so`) loaded at runtime via `dlopen()`. Each exports a single symbol `bm_plugin_desc` of type `plugin_desc_t`. The core discovers, validates, resolves dependencies, and manages lifecycles automatically.

Plugins live under `plugins/<type>/<kind>/` and are built as shared libraries via meson.

## Plugin Types

```c
typedef enum {
  PLUGIN_CORE,         // extends core functionality
  PLUGIN_DB,           // database engine driver (ext = db_driver_t*)
  PLUGIN_METHOD,       // human interaction method (ext = method_driver_t*)
  PLUGIN_BOT,          // bot behavior (ext = bot_driver_t*)
  PLUGIN_SERVICE,      // external API integration (ext usually NULL)
  PLUGIN_USERCMD,      // user command extension (registers commands)
  PLUGIN_PERSONALITY   // language/messaging personality
} plugin_type_t;
```

## Plugin Descriptor

Every plugin exports this struct as the `bm_plugin_desc` symbol:

```c
const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,   // must match (currently 8)
  .name            = "myplugin",           // unique name (max 64 chars)
  .version         = "1.0",               // version string (max 32 chars)
  .type            = PLUGIN_SERVICE,       // plugin type
  .kind            = "myplugin",           // kind identifier (max 64 chars)

  .provides        = { { .name = "service_myplugin" } },
  .provides_count  = 1,
  .requires        = { { .name = "bot_command" } },  // or .requires_count = 0
  .requires_count  = 1,

  .kv_schema       = my_kv_schema,         // plugin-level config (or NULL)
  .kv_schema_count = 2,

  .kv_inst_schema       = NULL,            // per-bot instance config (or NULL)
  .kv_inst_schema_count = 0,

  .init            = my_init,              // register state (or NULL)
  .start           = NULL,                 // begin operation (or NULL)
  .stop            = NULL,                 // drain work (or NULL)
  .deinit          = my_deinit,            // final cleanup (or NULL)

  .ext             = NULL,                 // driver vtable for METHOD/BOT/DB types

  .kv_groups       = NULL,                 // dynamic entity schemas (or NULL)
  .kv_groups_count = 0,
};
```

### Size Limits

| Constant | Value | Used For |
|----------|-------|----------|
| `PLUGIN_API_VERSION` | 8 | Must match at load time |
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
| Method | `method_<kind>` | `method_irc` |
| Bot | `bot_<kind>` | `bot_command` |
| Service | `service_<kind>` | `service_openweather` |
| Misc | `misc_<kind>` | `misc_misc_math` |
| DB | `db_<kind>` | `db_postgresql` |

### Common Dependency Patterns

**Service plugin depending on command bot:**
```c
.requires = { { .name = "bot_command" } },
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

Only relevant for `PLUGIN_METHOD` plugins. Service and bot plugins typically use plugin-level KV only.

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

### Method Driver (`PLUGIN_METHOD`)

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

### Bot Driver (`PLUGIN_BOT`)

Set `ext = &my_bot_driver` in the descriptor.

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

Typically set `ext = NULL`. Service plugins register commands in `init()` and do their work via those command callbacks. They depend on `bot_command` to make their commands available.

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
#include "mem.h"
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
#include "mem.h"
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
  if(cmd_register("newplugin", "hello",
      "hello",
      "Say hello",
      NULL,
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, np_cmd_hello, NULL,
      NULL, NULL,
      NULL, 0) != SUCCESS)
    return(FAIL);

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
  .requires        = { { .name = "bot_command" } },
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
7. Test: start botmanager, verify plugin loads and commands appear in `!help`

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
| `plugins/method/irc/` | Reference: method plugin with driver, instance KV, KV groups |
| `plugins/bot/command/` | Reference: bot plugin with driver, command dispatch |
