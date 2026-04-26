#ifndef BM_BOT_H
#define BM_BOT_H

#include <stdbool.h>
#include <stdint.h>

#include "method.h"
#include "userns.h"

#define BOT_NAME_SZ       64

typedef enum
{
  BOT_CREATED,    // instance exists, configured but not started
  BOT_RUNNING,    // start() called, subscribed to methods, processing
  BOT_STOPPING    // stop() called, draining in-flight work
} bot_state_t;

typedef struct bot_inst bot_inst_t;

// Functions a bot-behaviour plugin must implement. Stored in
// plugin_desc_t.ext for PLUGIN_METHOD plugins (chat, command) and
// PLUGIN_FEATURE plugins (whenmoon).
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
  uint32_t sessions;          // total active sessions across all instances
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

// Authenticate a user and create an active session. Uses the bot's
// bound user namespace for credential verification. If the sender
// already has a session on this method, it is replaced.
// Anonymous-by-default: no session exists until this succeeds.
// Returns USERNS_AUTH_ERR if no namespace is bound or limits exceeded.
userns_auth_t bot_session_auth(bot_inst_t *inst, method_inst_t *method,
    const char *sender, const char *username, const char *password);

// Create an authenticated session without password verification.
// Used by the autoidentify system when a user's MFA pattern matches
// and their autoidentify flag is enabled. Bot must be RUNNING.
bool bot_session_create(bot_inst_t *inst, method_inst_t *method,
    const char *sender, const char *username);

// Primary lookup used by bots to check if a message sender is
// authenticated. Returns authenticated username, or NULL if anonymous.
const char *bot_session_find(const bot_inst_t *inst,
    const method_inst_t *method, const char *sender);

// Extended session lookup with MFA fallback. First checks for an
// authenticated session (same as bot_session_find). If none is found
// and the bot has a user namespace, attempts MFA pattern matching
// against the sender's full method context (e.g., "nick!user@host").
// is_authed (if non-NULL) is set to true for authenticated sessions,
// false for MFA-matched-but-not-authenticated users.
const char *bot_session_find_ex(const bot_inst_t *inst,
    const method_inst_t *method, const char *sender,
    const char *mfa_string, bool *is_authed);

// Refresh the last_seen timestamp on a session matching a given
// username and method. Called when the bot witnesses activity from
// a user whose MFA string matches an active session, extending
// their identity cache.
bool bot_session_refresh_mfa(bot_inst_t *inst, method_inst_t *method,
    const char *username);

// Attempt user discovery from an MFA string. If the bot has user
// discovery enabled (bot.<name>.userdiscovery != 0), a user namespace
// bound, and the MFA string does not match any existing user, auto-
// creates a user from the handle portion of the MFA string, adds the
// MFA pattern, and returns the new username (static buffer).
const char *bot_discover_user(bot_inst_t *inst, const char *mfa_string);

// Logout — identified by method + sender.
bool bot_session_remove(bot_inst_t *inst, const method_inst_t *method,
    const char *sender);

void bot_session_clear(bot_inst_t *inst);
uint32_t bot_session_count(const bot_inst_t *inst);

typedef void (*bot_session_iter_cb_t)(const char *username,
    const char *method_name, time_t auth_time, time_t last_seen,
    void *data);

// Locks bot_mutex for the duration of the iteration.
void bot_session_iterate(const bot_inst_t *inst,
    bot_session_iter_cb_t cb, void *data);

// Returns namespace name, or empty string if not set.
const char *bot_session_get_userns_cd(const bot_inst_t *inst,
    const method_inst_t *method, const char *sender);

// ns_name NULL or empty clears.
bool bot_session_set_userns_cd(bot_inst_t *inst,
    const method_inst_t *method, const char *sender,
    const char *ns_name);

// Resolves and subscribes to all bound methods, calls driver start().
// Transitions CREATED -> RUNNING.
bool bot_start(bot_inst_t *inst);

// Calls driver stop(), unsubscribes from all methods.
// Transitions RUNNING -> STOPPING -> CREATED.
bool bot_stop(bot_inst_t *inst);

bot_state_t bot_get_state(const bot_inst_t *inst);
const char *bot_state_name(bot_state_t s);

void bot_get_stats(bot_stats_t *out);

typedef void (*bot_iter_cb_t)(const char *name, const char *driver_name,
    bot_state_t state, uint32_t method_count, uint32_t session_count,
    const char *userns_name, uint64_t cmd_count, time_t last_activity,
    void *data);

void bot_iterate(bot_iter_cb_t cb, void *data);

const char *bot_driver_name(const bot_inst_t *inst);
uint32_t bot_method_count(const bot_inst_t *inst);

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

// Called by cmd_dispatch() on successful command dispatch.
void bot_inc_cmd_count(bot_inst_t *inst);

uint64_t bot_cmd_count(const bot_inst_t *inst);
time_t bot_last_activity(const bot_inst_t *inst);

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

// Kind-scoped bot verbs:
// Bot-kind-specific verbs (e.g. llm's "personas" and "memories")
// register as children of "show/bot" or "bot" via cmd_register() with
// a non-NULL kind_filter. Dispatchers in core/bot_cmd.c resolve them
// with cmd_find_child_for_kind() so only children whose filter admits
// the bot's driver kind match.

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

typedef struct bot_session
{
  char                username[USERNS_USER_SZ];    // authenticated username
  method_inst_t      *method;                      // method authenticated on
  char                sender[METHOD_SENDER_SZ];    // protocol-level sender
  time_t              login_time;                   // session creation time
  time_t              auth_time;                    // authentication timestamp
  time_t              last_seen;                    // last activity timestamp
  char                userns_cd[USERNS_NAME_SZ];   // /user cd override (empty = use bot default)
  struct bot_session *next;
} bot_session_t;

struct bot_inst
{
  char                   name[BOT_NAME_SZ];
  const bot_driver_t    *driver;
  void                  *handle;       // driver-specific state
  bot_state_t            state;
  bot_method_t          *methods;      // linked list of bound methods
  uint32_t               method_count;
  userns_t              *userns;       // optional user namespace
  bot_session_t         *sessions;     // active authenticated sessions
  uint32_t               session_count;
  uint64_t               msg_count;    // total messages received
  uint64_t               cmd_count;    // total commands dispatched
  time_t                 last_activity; // last message received
  struct bot_inst       *next;
};

typedef struct
{
  uint32_t max_methods;
  uint32_t max_sessions;
} bot_cfg_t;

static bot_cfg_t bot_cfg = {
  .max_methods  = 16,
  .max_sessions = 256,
};

static bot_inst_t      *bot_list  = NULL;
static pthread_mutex_t  bot_mutex;
static uint32_t         bot_count = 0;
static bool             bot_ready = false;

static bot_method_t    *bot_method_freelist    = NULL;
static uint32_t         bot_method_free_count  = 0;

static bot_session_t   *bot_session_freelist   = NULL;
static uint32_t         bot_session_free_count = 0;

static uint32_t         bot_stat_discoveries   = 0;

#endif // BOT_INTERNAL

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

#endif // BOT_CMD_INTERNAL

#endif // BM_BOT_H
