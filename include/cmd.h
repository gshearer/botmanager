#ifndef BM_CMD_H
#define BM_CMD_H

#include <stdbool.h>
#include <stdint.h>

#include "bot.h"
#include "method.h"
#include "nl.h"
#include "userns.h"

#define CMD_NAME_SZ      32
#define CMD_MODULE_SZ    32
#define CMD_USAGE_SZ     128
#define CMD_PREFIX_SZ    8
#define CMD_MAX_ARGS     8
#define CMD_ARG_SZ       256

typedef struct cmd_def cmd_def_t;

// Controls where a command may be used. Scope is enforced during
// dispatch — a violation produces an error reply on the originating
// channel/DM. Operator commands bypass scope checks.
typedef enum
{
  CMD_SCOPE_ANY     = 0,  // usable in both public and private contexts
  CMD_SCOPE_PRIVATE = 1,  // private messages only (e.g., identify)
  CMD_SCOPE_PUBLIC  = 2,  // public channels only
} cmd_scope_t;

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

#define CMD_ARG_REQUIRED  0x00   // argument must be present (default)
#define CMD_ARG_OPTIONAL  0x01   // argument may be omitted
#define CMD_ARG_REST      0x02   // capture remainder of line (last arg only)

typedef bool (*cmd_arg_validator_t)(const char *str);

// One positional argument descriptor.
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

// Natural-language hinting:
// A command becomes visible to the NL bridge by attaching a cmd_nl_t
// to its registration. The struct carries:
//   - an intent phrase ("when should the LLM pick this verb")
//   - a canonical syntax line ("/weather <zipcode|city>")
//   - per-slot semantic typing + optional user-default fallback flag
//   - >=2 example (utterance, invocation) pairs for few-shot priming
// All strings are static / caller-owned. The registry keeps pointers;
// it never copies.
//
// cmd_nl_t is pure declarative metadata. The chat plugin is the sole
// interpreter — it renders these hints into the LLM prompt, parses
// the model's slash line, substitutes any user-scoped defaults for
// an absent slot, and dispatches. Sponsors describe what their
// command does and how it's shaped; they do not get a callback hook
// on the NL path.

typedef enum
{
  CMD_NL_ARG_FREE      = 0,  // any text
  CMD_NL_ARG_NICK      = 1,  // username on the current method
  CMD_NL_ARG_ZIPCODE   = 2,  // 5-digit US zip
  CMD_NL_ARG_CITY      = 3,  // city name (sponsor's command body normalizes)
  CMD_NL_ARG_LOCATION  = 4,  // zip OR city OR landmark (most weather-shaped)
  CMD_NL_ARG_DURATION  = 5,  // "5m", "1h", "90s"
  CMD_NL_ARG_DATE      = 6,  // ISO or natural date
  CMD_NL_ARG_INT       = 7,
  CMD_NL_ARG_URL       = 8,
  CMD_NL_ARG_TOPIC     = 9,  // freeform phrase, e.g., search term
} cmd_nl_arg_type_t;

// cmd_nl_slot_t.flags — OR-combinable bits. uint16_t to leave room for
// future knobs without another widening. Bit 0x0002 is intentionally
// reserved (used to be FROM_DOSSIER); do not reuse without thought.
#define CMD_NL_SLOT_REQUIRED      0x0000  // default (pedagogical alias)
#define CMD_NL_SLOT_OPTIONAL      0x0001  // LLM may omit this slot
#define CMD_NL_SLOT_REMAINDER     0x0004  // consumes the remainder of line
#define CMD_NL_SLOT_USER_DEFAULT  0x0008  // if omitted, a reasonable
                                          // default is derivable from the
                                          // asking user's profile; the
                                          // chat plugin decides how

typedef struct
{
  const char        *name;   // slot label, e.g., "location"
  cmd_nl_arg_type_t  type;
  uint16_t           flags;  // CMD_NL_SLOT_*
} cmd_nl_slot_t;

// Alias for the shared nl_example_t (include/nl.h). Kept as a named
// typedef so existing designated-initializer sites (.utterance /
// .invocation) keep working unchanged.
typedef nl_example_t cmd_nl_example_t;

// Top-level NL hint attached to a command registration.
typedef struct
{
  const char              *when;           // one-line intent phrase
  const char              *syntax;         // LLM-facing usage line
  const cmd_nl_slot_t     *slots;          // may be NULL if no slots
  uint8_t                  slot_count;
  const cmd_nl_example_t  *examples;       // REQUIRED; >=2 entries
  uint8_t                  example_count;

  // NL-bridge dispatch rewrite. Used only when the LLM is taught
  // (via syntax/examples) to emit a leaf-name slash-command whose real
  // dispatcher path is deeper than the root.
  //
  //   NULL     — bridge uses the emitted leaf name + LLM args verbatim
  //              (current behavior; top-level commands leave this NULL).
  //   non-NULL — bridge synthesises "<prefix><dispatch_text> <args>"
  //              into the dispatched text, with "$bot" in dispatch_text
  //              replaced by bot_inst_name(inst). Must parse from the
  //              root via the existing cmd_dispatch subcommand resolver.
  //
  // Kept in sync with the command's parent_path at registration, e.g.:
  //   registered at parent="show/bot", leaf name "model"
  //   dispatch_text = "show bot $bot model"
  const char              *dispatch_text;
} cmd_nl_t;

// Context passed to command callbacks. Contains everything needed
// to process the command and send a response. Tagged struct so other
// headers (e.g., bot.h) can forward-declare it without a cycle.
struct cmd_ctx
{
  bot_inst_t          *bot;       // bot instance
  const method_msg_t  *msg;       // full message context
  const char          *args;      // raw arguments after command name
  const char          *username;  // authenticated username, or NULL
  const cmd_args_t    *parsed;    // pre-parsed args, or NULL if no spec
  void                *data;      // opaque callback data from registration
};
#ifndef BM_CMD_CTX_T_DEFINED
#define BM_CMD_CTX_T_DEFINED
typedef struct cmd_ctx cmd_ctx_t;
#endif

// Invoked on a worker thread via the task system. ctx is valid for
// the duration of the callback.
typedef void (*cmd_cb_t)(const cmd_ctx_t *ctx);

// Help extender callback. Invoked by /help when the resolved command
// has remaining tokens that don't match static children. The extender
// can use the remaining args to produce context-sensitive help (e.g.
// resolving a bot instance name to enumerate kind-specific verbs).
typedef void (*cmd_help_extender_t)(const cmd_ctx_t *ctx,
    const char *rest);

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
// methods is a bitmask of method types this command is visible on
// (METHOD_T_ANY for all, or e.g. METHOD_T_BOTMANCTL | METHOD_T_IRC).
// abbrev must be unique among siblings.
// kind_filter is a NULL-terminated array of bot-kind strings this
// command applies to; NULL = kind-agnostic. Storage is caller-owned
// and must be static -- the registry keeps the pointer.
// nl is an optional static caller-owned natural-language hint.
bool cmd_register(const char *module, const char *name,
    const char *usage, const char *description,
    const char *help_long,
    const char *group, uint16_t level,
    cmd_scope_t scope, method_type_t methods, cmd_cb_t cb, void *data,
    const char *parent_path, const char *abbrev,
    const cmd_arg_desc_t *arg_desc, uint8_t arg_count,
    const char *const *kind_filter,
    const cmd_nl_t *nl);

bool cmd_unregister(const char *name);

// When /help resolves to this command and there are remaining tokens
// that don't match children, the extender is called instead of showing
// "unknown command". child NULL means root-level.
bool cmd_set_help_extender(const char *name, const char *child,
    cmd_help_extender_t ext);

// Root-level lookup.
const cmd_def_t *cmd_find(const char *name);

const cmd_def_t *cmd_find_child(const cmd_def_t *parent, const char *name);

// Find a child whose kind_filter matches the given bot kind. Name
// resolution is identical to cmd_find_child (exact match before abbrev).
// A child with kind_filter == NULL is kind-agnostic and matches any
// bot kind. When bot_kind == NULL, only kind-agnostic children match.
const cmd_def_t *cmd_find_child_for_kind(const cmd_def_t *parent,
    const char *name, const char *bot_kind);

// Return the bot-kind string that a command is filtered to, walking
// upward through the parent chain. Returns NULL for kind-agnostic
// commands. Used by help traversal to decide whether a child applies
// to a particular bot.
const char *cmd_kind_of(const cmd_def_t *def);

uint32_t cmd_count(void);

// Called when a bot instance is destroyed.
void cmd_bot_cleanup(bot_inst_t *inst);

// Default prefix is "!".
bool cmd_set_prefix(bot_inst_t *inst, const char *prefix);

// Returns prefix string, or "!" if not set.
const char *cmd_get_prefix(const bot_inst_t *inst);

// Dispatch a message as a potential command for a bot instance. Parses
// the command prefix and name from msg->text, verifies permissions, and
// submits a task to the work queue for non-blocking execution.
// Returns FAIL if: message does not start with the command prefix,
// command not found, permission denied, or task submission failed.
bool cmd_dispatch(bot_inst_t *inst, const method_msg_t *msg);

// Pure predicate form of the permission / scope / method-type gate that
// cmd_dispatch applies. No logging, no replies, no side effects. Callers
// that want a preflight decision (e.g. the NL bridge deciding whether to
// synthesize a command from LLM output, or the prompt assembler deciding
// whether to advertise a command to the model) use this; the actual
// denial replies remain cmd_dispatch's responsibility.
// Returns true if the caller (resolved via bot_session_find on msg) is
// permitted to run `def` in the context described by `msg`; false on
// any denial or on NULL inputs.
bool cmd_permits(bot_inst_t *inst, const method_msg_t *msg,
    const cmd_def_t *def);

// Dispatch a command on a method instance asserting an explicit caller
// identity. The permission check runs the normal group/level formula
// against that identity — no bypasses. Used for non-user-originated
// dispatch paths (e.g., botmanctl, NL bridges) where identity is
// supplied directly rather than resolved from a chat session.
// Returns SUCCESS if the command was executed (including denials that
// were surfaced as reply messages); FAIL if the command does not exist
// or wire preconditions (missing inst, malformed args) failed.
// `ns` may be NULL only for well-known system identities that the
// command system will accept as pre-authenticated (today: the literal
// owner principal with a synthetic full-membership profile).
bool cmd_dispatch_as(const char *cmd_name, const char *args,
    method_inst_t *inst, userns_t *ns, const char *username);

// Dispatch a pre-resolved command definition. Bypasses cmd_dispatch's
// root-walk + subcommand resolution AND the permission / scope /
// method-type gates: the caller must have authorized the call itself
// (e.g. via cmd_permits against the leaf). Submits a task to the
// worker pool so the command body runs off the caller's thread.
// Use: NL bridge invoking a subcommand leaf whose admin-gated parent
// would otherwise deny the walk before the leaf's own (everyone-0)
// perms apply.
// Returns SUCCESS if a task was submitted, FAIL on NULL inputs or
// task-pool submission failure.
bool cmd_dispatch_resolved(bot_inst_t *inst, const method_msg_t *msg,
    const cmd_def_t *def, const char *args);

// Send a reply to the originator of a command. If the message came from
// a channel, replies to the channel. If it was a DM (empty channel),
// replies to the sender directly.
bool cmd_reply(const cmd_ctx_t *ctx, const char *text);

// Command definition accessors (cmd_def_t is opaque outside cmd.c).

const char *cmd_get_module(const cmd_def_t *def);
const char *cmd_get_description(const cmd_def_t *def);
const char *cmd_get_usage(const cmd_def_t *def);
const char *cmd_get_help_long(const cmd_def_t *def);
const char *cmd_get_name(const cmd_def_t *def);
bool cmd_has_children(const cmd_def_t *def);
bool cmd_is_child(const cmd_def_t *def);
const cmd_def_t *cmd_get_parent(const cmd_def_t *def);
const char *cmd_get_abbrev(const cmd_def_t *def);
const char *cmd_get_group(const cmd_def_t *def);
uint16_t cmd_get_level(const cmd_def_t *def);
method_type_t cmd_get_methods(const cmd_def_t *def);
cmd_scope_t cmd_get_scope(const cmd_def_t *def);

// Returns NULL if the command is not NL-capable.
const cmd_nl_t *cmd_get_nl(const cmd_def_t *def);

// Invoke a command's callback directly with the given context. The
// caller is responsible for populating any fields the callback relies
// on (ctx->bot, ctx->args, ctx->parsed, etc.). This bypasses permission
// checks and the task queue; use it only from dispatchers that have
// already authorized the caller.
void cmd_invoke(const cmd_def_t *def, const cmd_ctx_t *ctx);

typedef void (*cmd_iter_cb_t)(const cmd_def_t *def, void *data);

// Iterate top-level root commands (parent == NULL).
void cmd_iterate_root(cmd_iter_cb_t cb, void *data);

void cmd_iterate_children(const cmd_def_t *parent, cmd_iter_cb_t cb,
    void *data);

// Registers the built-in root commands (/help, /show, /set). Must be
// called after bot_init().
void cmd_init(void);

typedef struct
{
  uint32_t registered;        // total command definitions
  uint64_t dispatches;        // lifetime successful dispatches
  uint64_t denials;           // lifetime permission/method-type rejections
} cmd_stats_t;

void cmd_get_stats(cmd_stats_t *out);

// Atomic reads. Either pointer may be NULL.
void cmd_get_dispatch_stats(uint64_t *dispatches, uint64_t *denials);

// Must be called after cmd_init().
void cmd_show_register(void);

// Must be called after cmd_init().
void cmd_set_register(void);

// Unregisters all commands and frees all per-bot prefix sets.
void cmd_exit(void);

#ifdef CMD_INTERNAL

#include "common.h"
#include "clam.h"
#include "kv.h"
#include "alloc.h"
#include "pool.h"
#include "task.h"
#include "validate.h"
#include "version.h"

// Maximum entries scanned in a kind_filter array during display
// iteration. The array itself is NUL-terminated; this cap exists only
// to bound help-listing loops against pathologically long filters.
#define CMD_KIND_FILTER_MAX 8

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
  cmd_help_extender_t help_ext;         // context-sensitive help (may be NULL)
  void       *data;                     // opaque callback data
  method_type_t methods;                // bitmask of method types visible on
  const cmd_arg_desc_t *arg_desc;       // argument descriptors (NULL = none)
  uint8_t     arg_count;                // number of entries in arg_desc
  const char *const *kind_filter;       // NULL-terminated kind array; NULL = kind-agnostic
  const cmd_nl_t *nl;                   // NL hint (static, caller-owned) or NULL
  cmd_def_t  *parent;                   // parent command (NULL for root)
  cmd_def_t  *children;                 // first child (subcommand)
  cmd_def_t  *sibling;                  // next sibling in parent's child list
  cmd_def_t  *next;                     // next in global list
};

typedef struct cmd_set
{
  const bot_inst_t  *inst;
  char               prefix[CMD_PREFIX_SZ];
  struct cmd_set    *next;
} cmd_set_t;

// Heap-allocated, freed after use.
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

static cmd_def_t       *cmd_list         = NULL;
static uint32_t         cmd_def_count    = 0;
static cmd_set_t       *cmd_sets         = NULL;
static pthread_mutex_t  cmd_mutex;
static bool             cmd_ready        = false;

static cmd_set_t       *cmd_set_freelist     = NULL;
static uint32_t         cmd_set_free_count   = 0;

// Atomic, no lock needed.
static uint64_t         cmd_stat_dispatches  = 0;
static uint64_t         cmd_stat_denials     = 0;

#endif // CMD_INTERNAL

#endif // BM_CMD_H
