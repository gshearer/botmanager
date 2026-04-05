# User Command Architecture

This document is for AI agents implementing new user commands. It covers the registration API, callback lifecycle, async patterns, colorized output, and provides copy-paste templates.

**Working reference implementations** (always up-to-date with actual API):
- Misc plugin: `plugins/misc/math/math.c`
- Service plugin: `plugins/service/openweather/openweather.c`
- Core module: `core/resolve.c` (async) or `core/math_expr.c` (sync)

## Overview

User commands are bot-facing commands (e.g., `!weather`, `!resolve`) dispatched through the command bot plugin. They are registered globally via `cmd_register()` and automatically enabled on all command bot instances via `cmd_enable_all()`. The callback runs on a **worker thread** from the task pool.

User commands live in either:
- **Core modules** (e.g., `core/resolve.c`) -- registered via a `_register_commands()` function called from `main.c`
- **Service plugins** (e.g., `plugins/service/openweather/`) -- registered in the plugin's `init()` callback

Do NOT use `cmd_register_system()` for user commands. System commands are for administrative/console operations (e.g., `/set`, `/bot add`).

## Registration API

```c
bool cmd_register(
    const char *module,       // module name (e.g., "resolve", "openweather")
    const char *name,         // command name, case-insensitive (e.g., "resolve")
    const char *usage,        // one-line usage (e.g., "resolve <target>")
    const char *help,         // brief description (max 256 chars)
    const char *help_long,    // multi-line help (max 1024 chars, may be NULL)
    const char *group,        // permission group (use USERNS_GROUP_EVERYONE for public)
    uint16_t level,           // minimum privilege level (0 for public)
    cmd_scope_t scope,        // CMD_SCOPE_ANY, CMD_SCOPE_PRIVATE, or CMD_SCOPE_PUBLIC
    cmd_cb_t cb,              // callback function
    void *data,               // opaque data (usually NULL)
    const char *parent_name,  // parent command for subcommands (NULL for root)
    const char *abbrev,       // short alias (NULL for none)
    const cmd_arg_desc_t *arg_desc,  // argument descriptors (NULL = no validation)
    uint8_t arg_count         // number of arg descriptors (0 = no validation)
);
```

### Permission Groups

| Group | Constant | Meaning |
|-------|----------|---------|
| `"everyone"` | `USERNS_GROUP_EVERYONE` | Unauthenticated users (level 0 only) |
| `"user"` | `USERNS_GROUP_USER` | Authenticated users |
| `"admin"` | `USERNS_GROUP_ADMIN` | Administrators |
| `"owner"` | `USERNS_GROUP_OWNER` | Owner only |

Most user commands use `USERNS_GROUP_EVERYONE, 0` so anyone can use them.

### Command Scope

| Scope | Constant | Meaning |
|-------|----------|---------|
| Any | `CMD_SCOPE_ANY` | Usable in channels and private messages |
| Private | `CMD_SCOPE_PRIVATE` | Private messages only (e.g., identify) |
| Public | `CMD_SCOPE_PUBLIC` | Public channels only |

Most user commands use `CMD_SCOPE_ANY`.

## Argument Validation

Define a static `cmd_arg_desc_t` array. The framework parses and validates before calling your callback. Pre-parsed arguments are available via `ctx->parsed->argv[]`.

```c
typedef struct {
  const char           *name;    // display name for error messages
  cmd_arg_type_t        type;    // validation type (see below)
  uint8_t               flags;   // CMD_ARG_REQUIRED (0x00), CMD_ARG_OPTIONAL (0x01),
                                 // CMD_ARG_REST (0x02, capture remainder, last arg only)
  size_t                maxlen;  // max length (0 = default 255)
  cmd_arg_validator_t   custom;  // custom validator (CMD_ARG_CUSTOM only, else NULL)
} cmd_arg_desc_t;
```

### Built-in Validation Types

| Type | Validates |
|------|-----------|
| `CMD_ARG_NONE` | Any non-empty string |
| `CMD_ARG_ALNUM` | Alphanumeric + underscores |
| `CMD_ARG_DIGITS` | Digits only |
| `CMD_ARG_HOSTNAME` | Hostname characters (alphanumeric, dots, hyphens, colons) |
| `CMD_ARG_PORT` | Integer 1-65535 |
| `CMD_ARG_CHANNEL` | IRC channel (no spaces/control chars) |
| `CMD_ARG_CUSTOM` | Custom validator function: `bool validator(const char *str)` |

### Examples

**One arg, built-in type:**
```c
static const cmd_arg_desc_t my_ad[] = {
  { "name", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, 30, NULL },
};
```

**One arg, custom validator:**
```c
static bool my_validate(const char *str);

static const cmd_arg_desc_t my_ad[] = {
  { "target", CMD_ARG_CUSTOM, CMD_ARG_REQUIRED, 0, my_validate },
};
```

**Two args, second captures rest of line:**
```c
static const cmd_arg_desc_t my_ad[] = {
  { "username", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, 30, NULL },
  { "password", CMD_ARG_NONE,  CMD_ARG_REQUIRED | CMD_ARG_REST, 0, NULL },
};
```

**No arg validation (manual parsing):**
Pass `NULL, 0` for `arg_desc, arg_count`. Use `ctx->args` for raw argument string.

## Callback Context

```c
typedef struct {
  bot_inst_t          *bot;       // bot instance
  const method_msg_t  *msg;       // originating message (sender, channel, method)
  const char          *args;      // raw argument string (after command name)
  const char          *username;  // authenticated username, or NULL
  const cmd_args_t    *parsed;    // pre-parsed args (NULL if no arg spec)
} cmd_ctx_t;
```

Access parsed arguments: `ctx->parsed->argv[0]`, `ctx->parsed->argv[1]`, etc.
Argument count: `ctx->parsed->argc`.

## Sending Replies

```c
cmd_reply(ctx, "text");   // replies to channel or sender (auto-detected)
```

Supports abstract color markers (see Colorized Output below). Multiple calls send multiple lines.

## Colorized Output

Use `CLR_*` macros from `include/colors.h`. These are 2-byte abstract markers (`\x01` + identifier) that `method_send()` translates to the method's native color codes (ANSI for console, mIRC codes for IRC, etc.).

| Macro | Color |
|-------|-------|
| `CLR_RED` | Red |
| `CLR_GREEN` | Green |
| `CLR_YELLOW` | Yellow |
| `CLR_BLUE` | Blue |
| `CLR_PURPLE` | Purple |
| `CLR_CYAN` | Cyan |
| `CLR_WHITE` | White |
| `CLR_ORANGE` | Orange |
| `CLR_GRAY` | Gray |
| `CLR_BOLD` | Bold |
| `CLR_RESET` | Reset to default |

Always `CLR_RESET` after colored text. Example:

```c
snprintf(line, sizeof(line),
    CLR_CYAN "%-6s" CLR_RESET " " CLR_GREEN "%s" CLR_RESET,
    "A", "192.168.1.1");
cmd_reply(ctx, line);
```

## Synchronous vs Async Commands

### Synchronous (simple)

The callback does its work and replies directly. No special handling needed. The callback already runs on a worker thread, so blocking briefly is acceptable.

```c
static void
my_cmd(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "Hello!");
}
```

### Asynchronous (deferred replies)

If your command initiates async work (e.g., `resolve_lookup()`, `curl_get()`) that completes via callback after `my_cmd()` returns, you must deep-copy the command context. The framework frees its internal copy after your callback returns.

**Required pattern:**

1. Allocate a request struct (freelist-managed) with embedded `cmd_ctx_t` and `method_msg_t`.
2. Deep-copy `ctx->msg` into the request's `method_msg_t`.
3. Rebuild `cmd_ctx_t` pointing to the copy. NULL out `args` and `username` (they point to freed memory).
4. In the async completion callback, use the saved context to call `cmd_reply()`.
5. Release the request back to the freelist.

```c
// Deep-copy pattern:
memcpy(&req->msg, ctx->msg, sizeof(req->msg));
req->ctx.bot      = ctx->bot;
req->ctx.msg      = &req->msg;   // point to OUR copy
req->ctx.args     = NULL;         // original will be freed
req->ctx.username = NULL;         // original will be freed
```

### Freelist Pattern

Reuse request structs to avoid repeated heap allocation. Thread-safe via mutex.

```c
// Alloc: try freelist first, fall back to mem_alloc().
static my_request_t *
my_req_alloc(void)
{
  my_request_t *r = NULL;

  pthread_mutex_lock(&my_free_mu);
  if(my_free != NULL)
  {
    r = my_free;
    my_free = r->next;
  }
  pthread_mutex_unlock(&my_free_mu);

  if(r == NULL)
    r = mem_alloc("mymodule", "request", sizeof(*r));

  memset(r, 0, sizeof(*r));
  return(r);
}

// Release: push back to freelist.
static void
my_req_release(my_request_t *r)
{
  pthread_mutex_lock(&my_free_mu);
  r->next = my_free;
  my_free = r;
  pthread_mutex_unlock(&my_free_mu);
}
```

Drain the freelist in your cleanup function (plugin `deinit()` or core module `_exit()`):

```c
while(my_free != NULL)
{
  my_request_t *r = my_free;
  my_free = r->next;
  mem_free(r);
}
```

## Adding a Command to a Core Module

1. Add the command callback, arg descriptor, and registration function to the module's `.c` file.
2. Add the public `_register_commands()` prototype to the module's `.h` file.
3. If the command needs async state, add the request struct, freelist statics, and validator forward declaration to the `.h` file under the module's `#ifdef MODULE_INTERNAL` guard.
4. Call `_register_commands()` from `main.c` after `cmd_init()`, before `plugin_init()`.
5. Call `cmd_unregister("name")` in the module's `_exit()` function.

## Adding a Command via Service Plugin

1. Create `plugins/service/<name>/` with `<name>.c`, `<name>.h`, and `meson.build`.
2. Add `subdir('service/<name>')` to `plugins/meson.build`.
3. Plugin descriptor: type `PLUGIN_SERVICE`, requires `"bot_command"`.
4. Register commands in the `init()` callback via `cmd_register()`.
5. Unregister commands in `deinit()` via `cmd_unregister()`.

### Minimal meson.build

```meson
my_include = include_directories('.')

my_plugin = shared_library('myplugin',
  'myplugin.c',
  include_directories : [inc_include, my_include],
  override_options : ['b_lundef=false'],
  install : true,
)
```

Add external dependencies (e.g., `json_c_dep`) to `dependencies :` if needed.

### Minimal Plugin Descriptor

```c
const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "myplugin",
  .version         = "1.0",
  .type            = PLUGIN_SERVICE,
  .kind            = "myplugin",
  .provides        = { { .name = "service_myplugin" } },
  .provides_count  = 1,
  .requires        = { { .name = "bot_command" } },
  .requires_count  = 1,
  .kv_schema       = NULL,         // or pointer to plugin_kv_entry_t array
  .kv_schema_count = 0,
  .init            = my_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = my_deinit,
  .ext             = NULL,
};
```

## Template: Synchronous Command (Core Module)

For a complete working example, see `core/math_expr.c` (synchronous) or `core/resolve.c` (async with freelist).

```c
// In module.h, public section:
void mymod_register_commands(void);

// In module.h, under #ifdef MYMOD_INTERNAL:
static const cmd_arg_desc_t mymod_cmd_ad[] = {
  { "thing", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, 64, NULL },
};

// In module.c:
static void
mymod_cmd_handler(const cmd_ctx_t *ctx)
{
  const char *thing = ctx->parsed->argv[0];
  char line[256];

  snprintf(line, sizeof(line),
      CLR_BOLD "%s" CLR_RESET ": some result", thing);
  cmd_reply(ctx, line);
}

void
mymod_register_commands(void)
{
  cmd_register("mymod", "mycmd",
      "mycmd <thing>",
      "Brief description of mycmd",
      "Detailed multi-line help text.\n"
      "\n"
      "Example: !mycmd foo",
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, mymod_cmd_handler, NULL,
      NULL, NULL,
      mymod_cmd_ad, 1);
}

// In main.c, after mem_register_commands():
mymod_register_commands();

// In module _exit():
cmd_unregister("mycmd");
```

## Key Files Reference

| File | Contains |
|------|----------|
| `include/cmd.h` | `cmd_register()`, `cmd_ctx_t`, `cmd_arg_desc_t`, all types and constants |
| `include/colors.h` | `CLR_*` macros, `color_table_t` |
| `include/userns.h` | `USERNS_GROUP_*` constants |
| `include/validate.h` | `validate_alnum()`, `validate_hostname()`, `validate_port()`, etc. |
| `include/mem.h` | `mem_alloc()`, `mem_free()` |
| `core/cmd.c` | `cmd_reply()`, dispatch logic |
| `core/resolve.c` | Reference: core module with async user command |
| `plugins/service/openweather/openweather.c` | Reference: service plugin with async user command |
| `plugins/bot/command/command.c` | Command bot plugin (enables user commands on bot instances) |
