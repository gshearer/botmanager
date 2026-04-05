#ifndef BM_CMD_H
#define BM_CMD_H

#include <stdbool.h>
#include <stdint.h>

#include "bot.h"
#include "method.h"
#include "userns.h"

#define CMD_NAME_SZ      32
#define CMD_MODULE_SZ    32
#define CMD_USAGE_SZ     128
#define CMD_HELP_SZ      256
#define CMD_HELP_LONG_SZ 1024
#define CMD_PREFIX_SZ    8
#define CMD_MAX_ARGS     8
#define CMD_ARG_SZ       256

// Forward declaration.
typedef struct cmd_def cmd_def_t;

// -----------------------------------------------------------------------
// Command scope (public/private visibility)
// -----------------------------------------------------------------------

// Controls where a command may be used.  Scope is enforced during
// dispatch — a violation produces an error reply on the originating
// channel/DM.  Console commands bypass scope checks.
typedef enum
{
  CMD_SCOPE_ANY     = 0,  // usable in both public and private contexts
  CMD_SCOPE_PRIVATE = 1,  // private messages only (e.g., identify)
  CMD_SCOPE_PUBLIC  = 2,  // public channels only
} cmd_scope_t;

// -----------------------------------------------------------------------
// Argument validation types and descriptors
// -----------------------------------------------------------------------

// Built-in argument validation types.
typedef enum
{
  CMD_ARG_NONE,       // no validation (any non-empty string)
  CMD_ARG_ALNUM,      // alphanumeric + underscores (validate_alnum)
  CMD_ARG_DIGITS,     // digits only (validate_digits)
  CMD_ARG_HOSTNAME,   // hostname chars (validate_hostname)
  CMD_ARG_PORT,       // port number 1-65535 (validate_port)
  CMD_ARG_CHANNEL,    // IRC channel name (validate_irc_channel)
  CMD_ARG_CUSTOM      // custom validator function
} cmd_arg_type_t;

// Argument flags.
#define CMD_ARG_REQUIRED  0x00   // argument must be present (default)
#define CMD_ARG_OPTIONAL  0x01   // argument may be omitted
#define CMD_ARG_REST      0x02   // capture remainder of line (last arg only)

// Custom argument validator callback.
// returns: true if the argument is valid
// str: NUL-terminated argument string
typedef bool (*cmd_arg_validator_t)(const char *str);

// Per-argument descriptor. Declares name, type, and constraints for
// one positional argument.
typedef struct
{
  const char           *name;      // display name for error messages
  cmd_arg_type_t        type;      // validation type
  uint8_t               flags;     // CMD_ARG_REQUIRED, CMD_ARG_OPTIONAL, CMD_ARG_REST
  size_t                maxlen;    // max length (0 = CMD_ARG_SZ - 1)
  cmd_arg_validator_t   custom;    // validator (CMD_ARG_CUSTOM only)
} cmd_arg_desc_t;

// Pre-parsed argument result passed to the command callback.
typedef struct
{
  const char *argv[CMD_MAX_ARGS];  // pointers into parsed token buffers
  uint8_t     argc;                // number of arguments actually parsed
} cmd_args_t;

// Context passed to command callbacks. Contains everything needed
// to process the command and send a response.
typedef struct
{
  bot_inst_t          *bot;       // bot instance
  const method_msg_t  *msg;       // full message context
  const char          *args;      // raw arguments after command name
  const char          *username;  // authenticated username, or NULL
  const cmd_args_t    *parsed;    // pre-parsed args, or NULL if no spec
} cmd_ctx_t;

// Command callback. Invoked on a worker thread via the task system.
// ctx: full command context (valid for duration of callback)
typedef void (*cmd_cb_t)(const cmd_ctx_t *ctx);

// -----------------------------------------------------------------------
// Global command registration (called by plugins)
// -----------------------------------------------------------------------

// Register a command globally. Commands must be enabled per bot instance
// before they can be dispatched (unless system or built-in).
//
// Every command has a default group and permission level. To use the
// command, a user must be a member of the group with at least the
// specified level. Unauthenticated users can only access commands in
// the "everyone" group at level 0.
//
// returns: SUCCESS or FAIL (duplicate name, invalid args)
// module: name of the providing module (e.g., "admin", "irc", "command")
// name: command name (case-insensitive, e.g., "weather")
// usage: brief one-line usage syntax (e.g., "weather <zipcode>")
// help: brief description (e.g., "Get current weather for a location")
// help_long: multi-line verbose help text (may be NULL)
// group: default group name (e.g., "everyone", "admin", "user")
// level: default minimum group privilege level (uint16_t)
// scope: CMD_SCOPE_ANY, CMD_SCOPE_PRIVATE, or CMD_SCOPE_PUBLIC
// methods: bitmask of method types this command is visible on
//          (METHOD_T_ANY for all, or e.g. METHOD_T_CONSOLE | METHOD_T_IRC)
// cb: command callback
// data: opaque data passed to callback via ctx
// parent_name: name of parent command for subcommand registration
//              (NULL for root-level commands)
// abbrev: optional abbreviation for the command (NULL for none).
//         Must be unique across all names and abbreviations.
// arg_desc: array of argument descriptors (NULL = no validation spec)
// arg_count: number of entries in arg_desc (0 = no validation spec)
bool cmd_register(const char *module, const char *name,
    const char *usage, const char *help,
    const char *help_long, const char *group, uint16_t level,
    cmd_scope_t scope, method_type_t methods, cmd_cb_t cb, void *data,
    const char *parent_name, const char *abbrev,
    const cmd_arg_desc_t *arg_desc, uint8_t arg_count);

// Register a system-level command. System commands are always dispatchable
// (like built-ins) without needing per-bot enablement. They are accessible
// from the console and from any method if the user has sufficient privileges.
// returns: SUCCESS or FAIL (duplicate name, invalid args)
// module: name of the providing module (e.g., "admin", "irc")
// name: command name (e.g., "set", "status")
// usage: brief one-line usage syntax (e.g., "set <key> <value>")
// help: brief description
// help_long: multi-line verbose help text (may be NULL)
// group: default group name
// level: default minimum group privilege level (uint16_t)
// scope: CMD_SCOPE_ANY, CMD_SCOPE_PRIVATE, or CMD_SCOPE_PUBLIC
// methods: bitmask of method types this command is visible on
// cb: command callback
// data: opaque data passed to callback via ctx
// parent_name: name of parent command for subcommand registration
//              (NULL for root-level commands)
// abbrev: optional abbreviation for the command (NULL for none).
//         Must be unique across all names and abbreviations.
// arg_desc: array of argument descriptors (NULL = no validation spec)
// arg_count: number of entries in arg_desc (0 = no validation spec)
bool cmd_register_system(const char *module, const char *name,
    const char *usage, const char *help,
    const char *help_long, const char *group, uint16_t level,
    cmd_scope_t scope, method_type_t methods, cmd_cb_t cb, void *data,
    const char *parent_name, const char *abbrev,
    const cmd_arg_desc_t *arg_desc, uint8_t arg_count);

// Unregister a command. Automatically disables it on all bot instances.
// returns: SUCCESS or FAIL (not found)
// name: command name
bool cmd_unregister(const char *name);

// Find a registered command by name.
// returns: command definition, or NULL if not found
// name: command name
const cmd_def_t *cmd_find(const char *name);

// Get the number of globally registered commands.
// returns: command count
uint32_t cmd_count(void);

// -----------------------------------------------------------------------
// Per-bot instance command management
// -----------------------------------------------------------------------

// Enable a registered command on a bot instance. The command must be
// registered globally before it can be enabled.
// returns: SUCCESS or FAIL (not found, already enabled)
// inst: bot instance
// cmd_name: command name to enable
bool cmd_enable(bot_inst_t *inst, const char *cmd_name);

// Disable a command on a bot instance.
// returns: SUCCESS or FAIL (not enabled)
// inst: bot instance
// cmd_name: command name to disable
bool cmd_disable(bot_inst_t *inst, const char *cmd_name);

// Check if a command is enabled on a bot instance.
// returns: true if enabled (or if the command is built-in)
// inst: bot instance
// cmd_name: command name
bool cmd_is_enabled(const bot_inst_t *inst, const char *cmd_name);

// Get the number of commands enabled on a bot instance (excluding
// built-in commands).
// returns: enabled command count
// inst: bot instance
uint32_t cmd_enabled_count(const bot_inst_t *inst);

// Enable all registered user commands (non-builtin, non-system,
// top-level) on a bot instance.
// returns: number of commands enabled
// inst: bot instance
uint32_t cmd_enable_all(bot_inst_t *inst);

// Clean up all per-bot command state. Called when a bot instance is
// destroyed. Frees all bindings for this instance.
// inst: bot instance
void cmd_bot_cleanup(bot_inst_t *inst);

// -----------------------------------------------------------------------
// Command prefix
// -----------------------------------------------------------------------

// Set the command prefix for a bot instance. Default is "!".
// returns: SUCCESS or FAIL (prefix too long)
// inst: bot instance
// prefix: command prefix string (e.g., "!", ".", "!!")
bool cmd_set_prefix(bot_inst_t *inst, const char *prefix);

// Get the command prefix for a bot instance.
// returns: prefix string, or "!" if not set
// inst: bot instance
const char *cmd_get_prefix(const bot_inst_t *inst);

// -----------------------------------------------------------------------
// Command dispatch
// -----------------------------------------------------------------------

// Dispatch a message as a potential command for a bot instance. Parses
// the command prefix and name from msg->text, checks if the command is
// enabled (or built-in or system), verifies permissions, and submits a
// task to the work queue for non-blocking execution.
//
// returns: SUCCESS if a command was dispatched, FAIL if:
//          - message does not start with the command prefix
//          - command not found or not enabled
//          - permission denied
//          - task submission failed
// inst: bot instance
// msg: full message context
bool cmd_dispatch(bot_inst_t *inst, const method_msg_t *msg);

// Dispatch a system command directly (used by the console). The command
// is executed synchronously as the @owner identity. System and built-in
// commands are dispatchable; bot-specific commands are not.
// returns: SUCCESS if command was found and executed, FAIL otherwise
// cmd_name: command name (without prefix)
// args: argument string (may be empty)
bool cmd_dispatch_system(const char *cmd_name, const char *args);

// Dispatch a system command as @owner on an arbitrary method instance.
// Identical to cmd_dispatch_system() but routes replies through the
// specified method instance instead of the console.
// returns: SUCCESS if command was found and executed, FAIL otherwise
// cmd_name: command name (without prefix)
// args: argument string (may be empty)
// inst: method instance for reply routing
bool cmd_dispatch_owner(const char *cmd_name, const char *args,
    method_inst_t *inst);

// -----------------------------------------------------------------------
// Reply helper
// -----------------------------------------------------------------------

// Send a reply to the originator of a command. If the message came from
// a channel, replies to the channel. If it was a DM (empty channel),
// replies to the sender directly.
// returns: SUCCESS or FAIL
// ctx: command context
// text: reply text
bool cmd_reply(const cmd_ctx_t *ctx, const char *text);

// -----------------------------------------------------------------------
// Command definition accessors (cmd_def_t is opaque outside cmd.c)
// -----------------------------------------------------------------------

// Get the module name for a command.
// returns: module string, or NULL if def is NULL
const char *cmd_get_module(const cmd_def_t *def);

// Get the usage string for a command.
// returns: usage string, or NULL if def is NULL
const char *cmd_get_usage(const cmd_def_t *def);

// Get the brief help text for a command.
// returns: help string, or NULL if def is NULL
const char *cmd_get_help(const cmd_def_t *def);

// Get the verbose help text for a command.
// returns: help_long string (empty if not set), or NULL if def is NULL
const char *cmd_get_help_long(const cmd_def_t *def);

// Get the name for a command.
// returns: name string, or NULL if def is NULL
const char *cmd_get_name(const cmd_def_t *def);

// Check if a command has subcommands.
// returns: true if the command has at least one child
bool cmd_has_children(const cmd_def_t *def);

// Check if a command is a subcommand (has a parent).
// returns: true if the command has a parent
bool cmd_is_child(const cmd_def_t *def);

// Get the parent command definition.
// returns: parent cmd_def_t pointer, or NULL
const cmd_def_t *cmd_get_parent(const cmd_def_t *def);

// Get the abbreviation for a command.
// returns: abbreviation string (empty if none), or NULL if def is NULL
const char *cmd_get_abbrev(const cmd_def_t *def);

// Get the required group for a command.
// returns: group string, or NULL if def is NULL
const char *cmd_get_group(const cmd_def_t *def);

// Get the required privilege level for a command.
// returns: level value, or 0 if def is NULL
uint16_t cmd_get_level(const cmd_def_t *def);

// Get the method type bitmask for a command.
// returns: method bitmask, or METHOD_T_ANY if def is NULL
method_type_t cmd_get_methods(const cmd_def_t *def);

// Get the scope for a command.
// returns: scope value, or CMD_SCOPE_ANY if def is NULL
cmd_scope_t cmd_get_scope(const cmd_def_t *def);

// -----------------------------------------------------------------------
// Command iteration
// -----------------------------------------------------------------------

// Callback for command iteration.
// def: command definition (valid for duration of callback)
// data: opaque user data
typedef void (*cmd_iter_cb_t)(const cmd_def_t *def, void *data);

// Iterate top-level system commands (skips subcommands).
// cb: callback invoked for each system command
// data: opaque user data passed to cb
void cmd_iterate_system(cmd_iter_cb_t cb, void *data);

// Iterate all system commands including subcommands.
// cb: callback invoked for each system command
// data: opaque user data passed to cb
void cmd_iterate_system_all(cmd_iter_cb_t cb, void *data);

// Iterate children (subcommands) of a specific command.
// parent: parent command definition
// cb: callback invoked for each child
// data: opaque user data passed to cb
void cmd_iterate_children(const cmd_def_t *parent, cmd_iter_cb_t cb,
    void *data);

// Iterate top-level commands enabled on a bot instance.
// Includes built-in commands (help, version) and explicitly
// enabled plugin commands. Skips subcommands.
// inst: bot instance
// cb: callback invoked for each enabled command
// data: opaque user data passed to cb
void cmd_iterate_bot(const bot_inst_t *inst, cmd_iter_cb_t cb, void *data);

// -----------------------------------------------------------------------
// Console method integration
// -----------------------------------------------------------------------

// Set the console method instance for reply routing. Called by
// console_register_method() after the console method is registered.
// inst: console method instance
void cmd_set_console_inst(method_inst_t *inst);

// -----------------------------------------------------------------------
// Subsystem lifecycle
// -----------------------------------------------------------------------

// Initialize the command subsystem. Registers built-in commands
// (help, version). Must be called after bot_init().
void cmd_init(void);

// Get lifetime command dispatch counters (thread-safe, atomic reads).
// dispatches: output for successful dispatch count (may be NULL)
// denials: output for permission denial count (may be NULL)
void cmd_get_dispatch_stats(uint64_t *dispatches, uint64_t *denials);

// Shut down the command subsystem. Unregisters all commands and frees
// all per-bot bindings.
void cmd_exit(void);

// -----------------------------------------------------------------------
// Internal structures (visible to cmd.c only)
// -----------------------------------------------------------------------

#ifdef CMD_INTERNAL

#include "common.h"
#include "clam.h"
#include "kv.h"
#include "mem.h"
#include "pool.h"
#include "task.h"
#include "validate.h"
#include "version.h"

// Global command definition.
struct cmd_def
{
  char        module[CMD_MODULE_SZ];    // providing module name
  char        name[CMD_NAME_SZ];
  char        abbrev[CMD_NAME_SZ];      // abbreviation (may be empty)
  char        usage[CMD_USAGE_SZ];
  char        help[CMD_HELP_SZ];
  char        help_long[CMD_HELP_LONG_SZ];
  char        group[USERNS_GROUP_SZ];   // required group
  uint16_t    level;                    // minimum group level
  cmd_scope_t scope;                    // public/private visibility
  cmd_cb_t    cb;
  void       *data;                     // opaque callback data
  bool        builtin;                  // true for help/version
  bool        system;                   // true for system-level commands
  method_type_t methods;                // bitmask of method types visible on
  const cmd_arg_desc_t *arg_desc;       // argument descriptors (NULL = none)
  uint8_t     arg_count;                // number of entries in arg_desc
  cmd_def_t  *parent;                   // parent command (NULL for root)
  cmd_def_t  *children;                 // first child (subcommand)
  cmd_def_t  *sibling;                  // next sibling in parent's child list
  cmd_def_t  *next;                     // next in global list
};

// Per-bot enabled command entry.
typedef struct cmd_binding
{
  char                name[CMD_NAME_SZ];   // references a cmd_def by name
  struct cmd_binding *next;
} cmd_binding_t;

// Per-bot command set: prefix + enabled commands.
typedef struct cmd_set
{
  const bot_inst_t  *inst;
  char               prefix[CMD_PREFIX_SZ];
  cmd_binding_t     *bindings;
  uint32_t           count;
  struct cmd_set    *next;
} cmd_set_t;

// Task data for async command dispatch (heap-allocated, freed after use).
typedef struct
{
  cmd_cb_t       cb;
  void          *cb_data;
  bot_inst_t    *bot;
  method_msg_t   msg;                        // copy of the message
  char           args[METHOD_TEXT_SZ];        // parsed arguments
  char           username[USERNS_USER_SZ];   // authenticated username
  char           usage[CMD_USAGE_SZ];        // usage string for error replies
  const cmd_arg_desc_t *arg_desc;            // argument descriptors (NULL = none)
  uint8_t        arg_count;                  // number of arg descriptors
  char           arg_bufs[CMD_MAX_ARGS][CMD_ARG_SZ]; // token storage
} cmd_task_data_t;

// Module state.
static cmd_def_t       *cmd_list         = NULL;
static uint32_t         cmd_def_count    = 0;
static cmd_set_t       *cmd_sets         = NULL;
static pthread_mutex_t  cmd_mutex;
static bool             cmd_ready        = false;

// Console method instance for reply routing.
static method_inst_t   *cmd_console_inst = NULL;

// Freelists.
static cmd_binding_t   *cmd_bind_freelist    = NULL;
static uint32_t         cmd_bind_free_count  = 0;
static cmd_set_t       *cmd_set_freelist     = NULL;
static uint32_t         cmd_set_free_count   = 0;

// Lifetime dispatch counters (atomic, no lock needed).
static uint64_t         cmd_stat_dispatches  = 0;
static uint64_t         cmd_stat_denials     = 0;

#endif // CMD_INTERNAL

#endif // BM_CMD_H
