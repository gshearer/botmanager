#ifndef BM_BOT_H
#define BM_BOT_H

#include <stdbool.h>
#include <stdint.h>

#include "method.h"
#include "userns.h"

#define BOT_NAME_SZ       64

// Room for every bound method kind of one bot, comma-joined. Sixteen
// bindings is bot_cfg.max_methods and the kinds are short words, so this
// is generous; bot_method_kinds() truncates rather than overflowing.
#define BOT_METHOD_KINDS_SZ  128

typedef enum
{
  BOT_CREATED,    // instance exists, configured but not started
  BOT_RUNNING,    // start() called, subscribed to methods, processing
  BOT_STOPPING    // stop() called, draining in-flight work
} bot_state_t;

typedef struct bot_inst bot_inst_t;

// Functions a bot-behaviour plugin must implement. Stored in
// plugin_desc_t.ext for PLUGIN_BOT plugins (chat).
typedef struct
{
  const char *name;

  // Create instance-specific state. Returns opaque handle, or NULL on failure.
  void *(*create)(bot_inst_t *inst);

  void (*destroy)(void *handle);

  // The bot is already subscribed to its methods at this point.
  bool (*start)(void *handle);

  void (*stop)(void *handle);

  // Incoming message from a subscribed method. The bot plugin decides
  // what to do with the message (parse commands, forward to LLM, etc.)
  // and is responsible for submitting tasks to the work queue as
  // needed. msg is valid for the duration of the callback.
  void (*on_message)(void *handle, const method_msg_t *msg);
} bot_driver_t;

typedef struct
{
  uint32_t instances;         // total bot instances
  uint32_t running;           // instances in BOT_RUNNING state
  uint32_t methods;           // total bound methods across all bots
  uint32_t discovered_users;  // lifetime discovered users
  uint64_t cmd_dispatches;    // lifetime successful command dispatches
  uint64_t cmd_denials;       // lifetime permission denials
} bot_stats_t;

// name becomes KV prefix bot.<name>.*
bot_inst_t *bot_create(const bot_driver_t *drv, const char *name);

// Stops if running, unsubscribes from all methods, invokes driver
// destroy(), deletes KV namespace bot.<name>.*
bool bot_destroy(const char *name);

bot_inst_t *bot_find(const char *name);
const char *bot_inst_name(const bot_inst_t *inst);

// Can only be called while CREATED.
bool bot_bind_method(bot_inst_t *inst, const char *method_name,
    const char *method_kind);

// Can only be called while CREATED.
bool bot_unbind_method(bot_inst_t *inst, const char *method_name);

// Can only be called while CREATED. The namespace must already exist
// (use userns_find). ns_name NULL clears.
bool bot_set_userns(bot_inst_t *inst, const char *ns_name);

userns_t *bot_get_userns(const bot_inst_t *inst);

// Clear the userns pointer on all bots bound to the named namespace.
// Used when a namespace is deleted. Works regardless of bot state.
void bot_clear_userns(const char *ns_name);

// Stateless per-message identity resolution: exact temporary-MFA
// match first (lazy expiry — the one-time "identity expired" notice
// rides this call), then permanent MFA pattern gated on the user's
// autoidentify flag. metadata may be NULL/empty; the method context
// map is consulted for the sender in that case. Fills user_out and
// returns true when identified; false means anonymous.
bool bot_identity_resolve(bot_inst_t *inst, method_inst_t *method,
    const char *sender, const char *metadata,
    char *user_out, size_t user_sz);

// Attempt user discovery from an MFA string. If the bot has user
// discovery enabled (bot.<name>.userdiscovery != 0), a user namespace
// bound, and the MFA string does not match any existing user, auto-
// creates a user from the handle portion of the MFA string, adds the
// MFA pattern, and returns the new username (static buffer).
const char *bot_discover_user(bot_inst_t *inst, const char *mfa_string);

// Resolves and subscribes to all bound methods, calls driver start().
// Transitions CREATED -> RUNNING.
bool bot_start(bot_inst_t *inst);

// Calls driver stop(), unsubscribes from all methods.
// Transitions RUNNING -> STOPPING -> CREATED.
bool bot_stop(bot_inst_t *inst);

bot_state_t bot_get_state(const bot_inst_t *inst);
const char *bot_state_name(bot_state_t s);

void bot_get_stats(bot_stats_t *out);

// `method_kinds` is the bot's bound method kinds, comma-joined by
// bot_method_kinds() — "" when nothing is bound. It names what the bot
// can do, which is what a listing wants; the bot plugin behind it is
// bot_driver_name() and there is only one of those.
typedef void (*bot_iter_cb_t)(const char *name, const char *method_kinds,
    bot_state_t state, uint32_t method_count,
    const char *userns_name, uint64_t cmd_count, time_t last_activity,
    void *data);

void bot_iterate(bot_iter_cb_t cb, void *data);

const char *bot_driver_name(const bot_inst_t *inst);
uint32_t bot_method_count(const bot_inst_t *inst);

// True when `kind` names a method bound to this bot, running or not.
// Case-insensitive, matching bot_resolve_method()'s kind comparison.
//
// bot_resolve_method() cannot answer this question: it hands back the
// method_inst_t, which is resolved at start time and therefore NULL for
// a CREATED bot — so a bot with a bound method reads as not having it.
bool bot_has_method_kind(const bot_inst_t *inst, const char *kind);

// Comma-joined bound method kinds ("irc, botmanctl") into `out`, each
// kind once however many instances of it are bound. Returns the byte
// length written; 0 means nothing is bound and `out` is "".
size_t bot_method_kinds(const bot_inst_t *inst, char *out, size_t out_sz);

// Find the first bot whose driver->name matches `driver_name`. Returns
// true on hit, with out_name (cap out_cap) and out_state populated.
// Used by the plugin loader to refuse unload of method/feature plugins
// while any bot still holds a pointer into the plugin's bot_driver_t.
// out_name / out_state may be NULL if the caller only needs presence.
bool bot_find_bound_to_driver(const char *driver_name,
    char *out_name, size_t out_cap, bot_state_t *out_state);

// ---------------------------------------------------------------------------
// Rebinding a bot across a plugin reload
// ---------------------------------------------------------------------------
//
// A bot holds two pointers into plugin mappings: its bot_driver_t (the
// bot plugin that gives it behaviour, e.g. chat) and, per bound method,
// a method_inst_t whose vtable belongs to a method plugin (e.g. irc).
// Neither is visible in the .provides/.requires graph, so a
// reload of either plugin would unmap code the bot is still pointing at
// — which is why both used to refuse the unload outright.
//
// These four give the loader a way to put the bot down and pick it back
// up instead. They are the reload's business alone; nothing else should
// call them.
//
//   bot_suspend_driver() — stop the driver, destroy its per-bot handle
//     and drop the vtable. Method subscriptions are left alone, so the
//     bot stays connected and merely goes deaf: bot_msg_handler drops
//     messages while the driver is NULL. Sessions survive.
//   bot_resume_driver() — re-register the driver's per-bot instance KV,
//     re-create the handle from `drv`, and start it again if the bot was
//     running. `kind` is the new plugin's kind, for the KV prefix.
//
//   bot_suspend_method() — a method's own state (the socket, the
//     connection) cannot outlive its mapping, so every RUNNING bot with
//     a bound method of that kind is stopped outright, unregistering the
//     method instance with it. A bot that is merely bound is recorded
//     too and left alone: it holds no instance, but its
//     bot.<bot>.<method>.* keys are reclaimed with the mapping just the
//     same, and only the resume puts them back.
//   bot_resume_method() — re-register the per-(bot, method) KV for every
//     suspended bot, and start the ones that were running again, which
//     re-creates the method instance from the reloaded driver and
//     reconnects it.
//
// Each returns the number of bots affected. Resuming a bot that was not
// suspended is a no-op, so a rollback may call them unconditionally.
uint32_t bot_suspend_driver(const char *driver_name);
uint32_t bot_resume_driver(const bot_driver_t *drv, const char *kind);
uint32_t bot_suspend_method(const char *method_kind);
uint32_t bot_resume_method(const char *method_kind);

// Returns the first bound method, or NULL. Useful for driver-side
// helpers (e.g. chatbot volunteer speech) that need a valid
// method_inst_t for an outbound channel send without reconstructing
// the routing layer.
method_inst_t *bot_first_method(const bot_inst_t *inst);

// Resolve a method instance bound to this bot from `key`, which may be
// either a method instance name (e.g. "drow") or a method kind (e.g.
// "irc"). Instance name wins when both interpretations are plausible;
// the kind match returns the first binding of that kind. Intended for
// user-facing destination specs like "<bot>:<key>:<target>" where the
// user shouldn't need to know whether they wrote an instance name or a
// kind.
method_inst_t *bot_resolve_method(const bot_inst_t *inst, const char *key);

// Called by the cmd layer on successful command dispatch: cmd_dispatch()
// (including its help redirect) and cmd_dispatch_resolved(). Not from
// core's deliver path — only the cmd layer knows a line became a command.
// cmd_dispatch_as() is bot-less by design and bumps only the process-wide
// counter.
void bot_inc_cmd_count(bot_inst_t *inst);

uint64_t bot_cmd_count(const bot_inst_t *inst);
time_t bot_last_activity(const bot_inst_t *inst);

// The most recent public (channel) line a bot witnessed, per
// (method-instance, channel). Command invocations (lines beginning with
// the bot's command prefix) are excluded, so consumers see the last
// *conversational* line — e.g. the `quote` feature immortalising whoever
// just spoke. Every bot maintains this automatically; no driver opt-in.
typedef struct
{
  char nickname[METHOD_NICKNAME_SZ];  // speaker's display nick
  char sender  [METHOD_SENDER_SZ];    // speaker's session key
  char text    [METHOD_TEXT_SZ];      // the line itself
  bool is_action;                     // true for emote/action lines
} bot_public_line_t;

// Fill `out` with the last public line witnessed by `inst` on the given
// method instance and channel. Returns false (leaving `out` untouched)
// when nothing has been seen there yet or on NULL inputs. Thread-safe.
bool bot_last_public_line(const bot_inst_t *inst,
    const method_inst_t *method, const char *channel,
    bot_public_line_t *out);

// Opaque handle from driver create(), or NULL. Used by show verb
// handlers that need to pull stats from the driver's private state
// (e.g. llm's in-flight counter).
void *bot_get_handle(const bot_inst_t *inst);

void bot_init(void);

// Register per-bot method KV keys by copying the method plugin's KV
// schema into the bot's namespace. Transforms bare suffix keys
// to bot.<botname>.<kind>.* with the same types and defaults.
// Returns 0 if plugin not found or no schema.
uint32_t bot_register_method_kv(const char *botname, const char *method_kind);

// Register a bot driver's kv_inst_schema (e.g., llm's per-instance
// keys) under "bot.<botname>." after a bot is created.
uint32_t bot_register_driver_kv(const char *botname, const char *bot_kind);

// ---------------------------------------------------------------------------
// Per-bot / per-(bot,method) KV contributors
// ---------------------------------------------------------------------------
//
// Lets a command/feature plugin layer its OWN KV keys onto every bot
// without core hardcoding plugin-specific keys (e.g. the `ask` plugin's
// `bot.<botname>.ask.*` and `bot.<botname>.<method>.ask.*` tiers). This
// keeps the layering clean: core owns the bot registry and merely invites
// registered contributors to decorate each bot / bound method.
//
//   bot_cb    — fires once per bot instance. Register `bot.<botname>.*`.
//   method_cb — fires once per (bot, bound method) pair. Register
//               `bot.<botname>.<method_kind>.*`, where `method_kind` is
//               the method plugin's kind string (e.g. "irc").
//
// Either callback may be NULL. On registration the contributor is
// immediately back-filled against every existing bot and its already-bound
// methods, so a hot-reloaded plugin re-attaches its keys. Thereafter each
// new bot and each freshly-bound method invokes every registered
// contributor. Unregister (by the same `user` cookie) before the plugin
// unloads, or the stored callback pointer dangles.
//
// Contributor callbacks MUST be self-contained (kv_register and friends);
// they must not re-enter bot APIs that take the bot lock.
typedef void (*bot_kv_bot_cb_t)(const char *botname, void *user);
typedef void (*bot_kv_method_cb_t)(const char *botname,
    const char *method_kind, void *user);

void bot_kv_contributor_register(bot_kv_bot_cb_t bot_cb,
    bot_kv_method_cb_t method_cb, void *user);
void bot_kv_contributor_unregister(void *user);

// Drop every contributor registered by code inside the address range
// [lo,hi) — one loaded object's mapping. Contributors are Class A (see
// root TODO.md §PLIFE-3): the table retains two function pointers and a
// cookie, all of which die at dlclose, and the keys a contributor
// layered on already stand on their own.
// Returns the number of contributors removed.
uint32_t bot_reclaim_contributors_owned(uintptr_t lo, uintptr_t hi);

// Audit hooks: yield the pointers the bot registry retains on a
// plugin's behalf — the contributor table's callbacks and cookies, and
// each instance's bound driver vtable.
//
// Invoked UNDER the respective bot locks: the callback must be fast and
// must not re-enter any bot_* API.
typedef void (*bot_audit_cb_t)(const char *subject, const char *field,
    const void *ptr, void *data);

void bot_audit_iterate_contributors(bot_audit_cb_t cb, void *data);
void bot_audit_iterate_bindings(bot_audit_cb_t cb, void *data);

// Must be called after db_init().
bool bot_ensure_tables(void);

// Restore bot instances from database. Creates instances, binds
// methods, sets user namespaces, and auto-starts previously running
// bots. Must be called after plugins are started and KV is loaded.
bool bot_restore(void);

// Must be called after kv_init() and kv_load().
void bot_register_config(void);

// Stops and destroys all instances.
void bot_exit(void);

// Must be called after cmd_init().
void bot_register_commands(void);

// Method-scoped bot verbs:
// A verb registers as a child of "bot" or "show/bot" via cmd_register()
// and is offered to a bot when its kind_filter is NULL or names a
// method the bot has bound. Dispatchers in core/bot_cmd.c resolve them
// with bot_has_method_kind(), so a verb is scoped by what the bot can
// do, never by the plugin that gives it a mind -- every bot shares one
// of those, which is why the mind's own verbs register kind-agnostic.

// Forward decl: cmd_ctx_t lives in cmd.h (tagged struct) and is
// passed through opaque where needed.
struct cmd_ctx;
#ifndef BM_CMD_CTX_T_DEFINED
#define BM_CMD_CTX_T_DEFINED
typedef struct cmd_ctx cmd_ctx_t;
#endif

#ifdef BOT_INTERNAL

#include "common.h"
#include "clam.h"
#include "db.h"
#include "kv.h"
#include "alloc.h"
#include "plugin.h"
#include "pool.h"
#include "task.h"

typedef struct bot_method
{
  char               method_name[METHOD_NAME_SZ];
  char               method_kind[PLUGIN_NAME_SZ]; // plugin kind (e.g., "irc")
  method_inst_t     *inst;          // resolved at start time
  bool               subscribed;    // true if currently subscribed
  bool               created_by_bot; // true if bot_start() created the instance
  struct bot_method *next;
} bot_method_t;

// Per-bot ring of last-witnessed public lines, keyed by (method, channel).
// Bounded and round-robin — a bot spanning more channels than slots keeps
// the most-recently-active ones. Read/written under bot_witness_lock.
#define BOT_WITNESS_MAX  16

// How many bots one reload can put down at once. A cascade that touches
// more than this on a dev instance is not a cascade, it is a restart.
#define BOT_SUSPEND_MAX  64

typedef struct
{
  bool  valid;
  char  method [METHOD_NAME_SZ];
  char  channel[METHOD_CHANNEL_SZ];
  char  nickname[METHOD_NICKNAME_SZ];
  char  sender  [METHOD_SENDER_SZ];
  char  text    [METHOD_TEXT_SZ];
  bool  is_action;
} bot_witness_t;

// What a plugin reload took away from a bot, and what it owes back.
// `driver` and `method` are never both set: a bot loses its behaviour
// or one of its methods, and the loader cycles one plugin at a time.
typedef struct
{
  bool driver;                        // bot_driver_t detached, inst->driver NULL
  bool method;                        // bot stopped for a method reload
  bool was_running;                   // ...and it was RUNNING when it happened
  char driver_name[BOT_NAME_SZ];      // driver to re-attach
  char method_kind[PLUGIN_NAME_SZ];   // method to re-bind
} bot_suspend_t;

struct bot_inst
{
  char                   name[BOT_NAME_SZ];
  const bot_driver_t    *driver;
  void                  *handle;       // driver-specific state
  bot_state_t            state;
  bot_method_t          *methods;      // linked list of bound methods
  uint32_t               method_count;
  userns_t              *userns;       // optional user namespace
  // The three counters below are written from a method's delivery
  // thread and read from a command thread, so every access to them
  // goes through __atomic_* with relaxed ordering — never a plain
  // increment or load, on either side. They are display state; the
  // ordering they do not promise is ordering nothing else needs.
  uint64_t               msg_count;    // total messages received
  uint64_t               cmd_count;    // total commands dispatched
  time_t                 last_activity; // last message received
  bot_witness_t          witness[BOT_WITNESS_MAX]; // last public lines
  uint32_t               witness_next;  // round-robin insert cursor
  bot_suspend_t          susp;         // set only across a plugin reload
  struct bot_inst       *next;
};

typedef struct
{
  uint32_t max_methods;
} bot_cfg_t;

#endif // BOT_INTERNAL

// The registry itself — the one list of bots, its lock, and its counters.
// Separate from BOT_INTERNAL because these are *definitions*, not
// declarations: a second translation unit that defined BOT_INTERNAL would
// get its own private, permanently-empty bot list and its own mutex, and
// nothing would say so. Only bot.c may define this guard.
#ifdef BOT_REGISTRY_INTERNAL

static bot_cfg_t bot_cfg = {
  .max_methods = 16,
};

static bot_inst_t      *bot_list  = NULL;
static pthread_mutex_t  bot_mutex;

// Guards the per-bot witness rings (written on the delivery thread, read
// from command worker threads). Static-initialised — no bot_init hook.
static pthread_mutex_t  bot_witness_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t         bot_count = 0;
static bool             bot_ready = false;

static bot_method_t    *bot_method_freelist    = NULL;
static uint32_t         bot_method_free_count  = 0;

static uint32_t         bot_stat_discoveries   = 0;

#endif // BOT_REGISTRY_INTERNAL

#ifdef BOT_CMD_INTERNAL

#include "common.h"
#include "cmd.h"
#include "colors.h"
#include "db.h"
#include "kv.h"
#include "alloc.h"
#include "plugin.h"
#include "pool.h"
#include "validate.h"

#include <stdio.h>
#include <string.h>

typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         count;
} bot_cmd_list_state_t;

// Accumulator for cmd_find_child_for_bot()'s child iteration. Exact
// name beats abbrev, so both are collected and the exact one wins.
typedef struct
{
  const char         *name;     // verb the caller typed
  const bot_inst_t   *inst;     // bot whose methods gate the match
  const cmd_def_t    *exact;
  const cmd_def_t    *abbrev;
} bot_cmd_child_match_t;

// State threaded through help_list_children_for_bot()'s iteration.
// A NULL `inst` means "list every child", not "a bot with no methods".
typedef struct
{
  const cmd_ctx_t  *ctx;
  const bot_inst_t *inst;
  uint32_t          count;
} help_list_ctx_t;

#endif // BOT_CMD_INTERNAL

#endif // BM_BOT_H
