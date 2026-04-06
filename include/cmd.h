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
  void                *data;      // opaque callback data from registration
} cmd_ctx_t;

// Command callback. Invoked on a worker thread via the task system.
// ctx: full command context (valid for duration of callback)
typedef void (*cmd_cb_t)(const cmd_ctx_t *ctx);

// -----------------------------------------------------------------------
// Command registration
// -----------------------------------------------------------------------

// Register a command in the unified command tree.
//
// All commands — show views, set handlers, actions — use this single
// registration function. Tree position is determined by parent_path:
//   NULL       — root-level command
//   "bot"      — child of root command "bot"
//   "irc/network" — child of "network" under root command "irc"
//
// Every command has a default group and permission level. To use the
// command, a user must be a member of the group with at least the
// specified level. Unauthenticated users can only access commands in
// the "everyone" group at level 0.
//
// returns: SUCCESS or FAIL (duplicate name, invalid args, parent not found)
// module: name of the providing module (e.g., "admin", "irc", "command")
// name: command name (case-insensitive, e.g., "bot")
// usage: single-line syntax string (static, e.g., "bot add <name> <kind>")
// description: single-line human-friendly description (static, for table)
// help_long: multi-line verbose help text (static, NULL if none)
// group: default group name (e.g., "everyone", "admin", "user")
// level: default minimum group privilege level (uint16_t)
// scope: CMD_SCOPE_ANY, CMD_SCOPE_PRIVATE, or CMD_SCOPE_PUBLIC
// methods: bitmask of method types this command is visible on
//          (METHOD_T_ANY for all, or e.g. METHOD_T_CONSOLE | METHOD_T_IRC)
// cb: command callback
// data: opaque data passed to callback via ctx
// parent_path: slash-delimited path to parent (NULL for root)
// abbrev: optional abbreviation for the command (NULL for none).
//         Must be unique among siblings.
// arg_desc: array of argument descriptors (NULL = no validation spec)
// arg_count: number of entries in arg_desc (0 = no validation spec)
bool cmd_register(const char *module, const char *name,
    const char *usage, const char *description,
    const char *help_long,
    const char *group, uint16_t level,
    cmd_scope_t scope, method_type_t methods, cmd_cb_t cb, void *data,
    const char *parent_path, const char *abbrev,
    const cmd_arg_desc_t *arg_desc, uint8_t arg_count);

// Unregister a command.
// returns: SUCCESS or FAIL (not found)
// name: command name
bool cmd_unregister(const char *name);

// Find a registered command by name (root-level lookup).
// returns: command definition, or NULL if not found
// name: command name
const cmd_def_t *cmd_find(const char *name);

// Find a child command by name under a specific parent.
// returns: child definition, or NULL if not found
// parent: parent command to search under
// name: child command name or abbreviation
const cmd_def_t *cmd_find_child(const cmd_def_t *parent, const char *name);

// Get the number of globally registered commands.
// returns: command count
uint32_t cmd_count(void);

// Clean up per-bot command state (prefix set). Called when a bot
// instance is destroyed.
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
// the command prefix and name from msg->text, verifies permissions, and
// submits a task to the work queue for non-blocking execution.
//
// returns: SUCCESS if a command was dispatched, FAIL if:
//          - message does not start with the command prefix
//          - command not found
//          - permission denied
//          - task submission failed
// inst: bot instance
// msg: full message context
bool cmd_dispatch(bot_inst_t *inst, const method_msg_t *msg);

// Dispatch a system command directly (used by the console). The command
// is executed synchronously as the @owner identity. Any command in the
// tree is dispatchable; access is purely permission-based.
// returns: SUCCESS if command was found and executed, FAIL otherwise
// cmd_name: command name (without prefix)
// args: argument string (may be empty)
bool cmd_dispatch_system(const char *cmd_name, const char *args);

// Dispatch a command as @owner on an arbitrary method instance.
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

// Get the single-line description for a command.
// returns: description string, or NULL if def is NULL
const char *cmd_get_description(const cmd_def_t *def);

// Get the single-line usage/syntax for a command.
// returns: usage string, or NULL if def is NULL
const char *cmd_get_usage(const cmd_def_t *def);

// Get the multi-line verbose help text for a command.
// returns: help_long string, or NULL if def is NULL or not set
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

// Iterate top-level root commands (parent == NULL).
// cb: callback invoked for each root command
// data: opaque user data passed to cb
void cmd_iterate_root(cmd_iter_cb_t cb, void *data);

// Iterate children (subcommands) of a specific command.
// parent: parent command definition
// cb: callback invoked for each child
// data: opaque user data passed to cb
void cmd_iterate_children(const cmd_def_t *parent, cmd_iter_cb_t cb,
    void *data);

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

// Initialize the command subsystem. Registers the built-in root
// commands (/help, /show, /set). Must be called after bot_init().
void cmd_init(void);

// Command subsystem statistics.
typedef struct
{
  uint32_t registered;        // total command definitions
  uint64_t dispatches;        // lifetime successful dispatches
  uint64_t denials;           // lifetime permission/method-type rejections
} cmd_stats_t;

// Get command subsystem statistics (thread-safe snapshot).
// out: destination for the snapshot
void cmd_get_stats(cmd_stats_t *out);

// Get lifetime command dispatch counters (thread-safe, atomic reads).
// dispatches: output for successful dispatch count (may be NULL)
// denials: output for permission denial count (may be NULL)
void cmd_get_dispatch_stats(uint64_t *dispatches, uint64_t *denials);

// Shut down the command subsystem. Unregisters all commands and frees
// all per-bot prefix sets.
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
  const char *usage;                    // single-line syntax (static)
  const char *description;              // single-line description (static)
  const char *help_long;                // multi-line verbose help (static, may be NULL)
  char        group[USERNS_GROUP_SZ];   // required group
  uint16_t    level;                    // minimum group level
  cmd_scope_t scope;                    // public/private visibility
  cmd_cb_t    cb;
  void       *data;                     // opaque callback data
  method_type_t methods;                // bitmask of method types visible on
  const cmd_arg_desc_t *arg_desc;       // argument descriptors (NULL = none)
  uint8_t     arg_count;                // number of entries in arg_desc
  cmd_def_t  *parent;                   // parent command (NULL for root)
  cmd_def_t  *children;                 // first child (subcommand)
  cmd_def_t  *sibling;                  // next sibling in parent's child list
  cmd_def_t  *next;                     // next in global list
};

// Per-bot command set: prefix for the bot instance.
typedef struct cmd_set
{
  const bot_inst_t  *inst;
  char               prefix[CMD_PREFIX_SZ];
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
  const char    *usage;                      // usage string (static, not copied)
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
static cmd_set_t       *cmd_set_freelist     = NULL;
static uint32_t         cmd_set_free_count   = 0;

// Lifetime dispatch counters (atomic, no lock needed).
static uint64_t         cmd_stat_dispatches  = 0;
static uint64_t         cmd_stat_denials     = 0;

#endif // CMD_INTERNAL

#endif // BM_CMD_H
