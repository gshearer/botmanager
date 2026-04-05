#ifndef BM_BOT_H
#define BM_BOT_H

#include <stdbool.h>
#include <stdint.h>

#include "method.h"
#include "userns.h"

#define BOT_NAME_SZ       64

// Bot instance states.
typedef enum
{
  BOT_CREATED,    // instance exists, configured but not started
  BOT_RUNNING,    // start() called, subscribed to methods, processing
  BOT_STOPPING    // stop() called, draining in-flight work
} bot_state_t;

// Forward declaration.
typedef struct bot_inst bot_inst_t;

// Driver interface: functions a bot plugin must implement.
// Stored in plugin_desc_t.ext for PLUGIN_BOT plugins.
typedef struct
{
  const char *name;

  // Create instance-specific state. Called during bot_create().
  // inst: the bot instance being created
  // returns: opaque handle, or NULL on failure
  void *(*create)(bot_inst_t *inst);

  // Destroy instance-specific state. Called during bot_destroy().
  // handle: opaque handle from create()
  void (*destroy)(void *handle);

  // Start active operation. The bot is already subscribed to its
  // methods at this point. Called during bot_start().
  // returns: SUCCESS or FAIL
  // handle: opaque handle from create()
  bool (*start)(void *handle);

  // Stop active operation, drain in-flight work. Called during bot_stop().
  // handle: opaque handle from create()
  void (*stop)(void *handle);

  // Incoming message from a subscribed method. This is the main entry
  // point for bot behavior. The bot plugin decides what to do with
  // the message (parse commands, forward to LLM, etc.) and is
  // responsible for submitting tasks to the work queue as needed.
  // handle: opaque handle from create()
  // msg: full message context (valid for duration of callback)
  void (*on_message)(void *handle, const method_msg_t *msg);
} bot_driver_t;

// Bot subsystem statistics.
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

// -----------------------------------------------------------------------
// Instance management
// -----------------------------------------------------------------------

// Create a new bot instance. The driver's create() callback is invoked
// to produce instance-specific state.
// returns: instance pointer, or NULL on failure
// drv: bot driver interface (must not be NULL)
// name: unique instance name (becomes KV prefix bot.name.*)
bot_inst_t *bot_create(const bot_driver_t *drv, const char *name);

// Destroy a bot instance. Stops if running, unsubscribes from all
// methods, invokes driver destroy(), deletes KV namespace bot.name.*
// returns: SUCCESS or FAIL (not found)
// name: instance name
bool bot_destroy(const char *name);

// Find an instance by name.
// returns: instance pointer, or NULL if not found
// name: instance name
bot_inst_t *bot_find(const char *name);

// Get the name of an instance.
// returns: instance name string
// inst: bot instance
const char *bot_inst_name(const bot_inst_t *inst);

// -----------------------------------------------------------------------
// Method binding
// -----------------------------------------------------------------------

// Bind a method instance to this bot. The bot will subscribe to the
// method when bot_start() is called. Can only be called while CREATED.
// returns: SUCCESS or FAIL (duplicate, wrong state, at limit)
// inst: bot instance
// method_name: name of a registered method instance
// method_kind: method plugin kind (e.g., "irc")
bool bot_bind_method(bot_inst_t *inst, const char *method_name,
    const char *method_kind);

// Unbind a method from this bot. Can only be called while CREATED.
// returns: SUCCESS or FAIL (not found, wrong state)
// inst: bot instance
// method_name: method to unbind
bool bot_unbind_method(bot_inst_t *inst, const char *method_name);

// -----------------------------------------------------------------------
// Namespace binding
// -----------------------------------------------------------------------

// Set the user namespace for this bot instance. Can only be called
// while CREATED. Uses userns_get() which creates the namespace if
// it does not exist.
// returns: SUCCESS or FAIL
// inst: bot instance
// ns_name: user namespace name (NULL to clear)
bool bot_set_userns(bot_inst_t *inst, const char *ns_name);

// Get the user namespace bound to this bot instance.
// returns: userns pointer, or NULL if none bound
// inst: bot instance
userns_t *bot_get_userns(const bot_inst_t *inst);

// -----------------------------------------------------------------------
// Session tracking
// -----------------------------------------------------------------------

// Authenticate a user and create an active session. Uses the bot's
// bound user namespace for credential verification. If the sender
// already has a session on this method, it is replaced.
// Anonymous-by-default: no session exists until this succeeds.
// returns: USERNS_AUTH_OK on success, or an auth error code.
//          USERNS_AUTH_ERR if no namespace is bound or limits exceeded.
// inst: bot instance (must be RUNNING, must have a user namespace)
// method: the method instance the user is authenticating from
// sender: protocol-level sender identity (e.g., IRC nick)
// username: credential username
// password: credential password
userns_auth_t bot_session_auth(bot_inst_t *inst, method_inst_t *method,
    const char *sender, const char *username, const char *password);

// Find an active session by method and sender. This is the primary
// lookup used by bots to check if a message sender is authenticated.
// returns: authenticated username, or NULL if anonymous
// inst: bot instance
// method: method instance the message arrived on
// sender: protocol-level sender identity
const char *bot_session_find(const bot_inst_t *inst,
    const method_inst_t *method, const char *sender);

// Extended session lookup with MFA fallback. First checks for an
// authenticated session (same as bot_session_find). If none is found
// and the bot has a user namespace, attempts MFA pattern matching
// against the sender's full method context (e.g., "nick!user@host").
// The is_authed flag (if non-NULL) is set to true for authenticated
// sessions, false for MFA-matched-but-not-authenticated users.
// returns: username, or NULL if anonymous and no MFA match
// inst: bot instance
// method: method instance the message arrived on
// sender: protocol-level sender identity
// mfa_string: full MFA context string (e.g., "nick!user@host"), or NULL
// is_authed: output flag — true if from authenticated session, false if MFA match
const char *bot_session_find_ex(const bot_inst_t *inst,
    const method_inst_t *method, const char *sender,
    const char *mfa_string, bool *is_authed);

// Attempt user discovery from an MFA string. If the bot has user
// discovery enabled (bot.<name>.userdiscovery != 0), a user namespace
// bound, and the MFA string does not match any existing user, auto-
// creates a user from the handle portion of the MFA string, adds the
// MFA pattern, and returns the new username.
// returns: discovered username (static buffer), or NULL if not discovered
// inst: bot instance (must be RUNNING with a user namespace)
// mfa_string: full MFA context (e.g., "nick!user@host")
const char *bot_discover_user(bot_inst_t *inst, const char *mfa_string);

// Remove an active session (logout). Identified by method + sender.
// returns: SUCCESS or FAIL (no such session)
// inst: bot instance
// method: method instance
// sender: protocol-level sender identity
bool bot_session_remove(bot_inst_t *inst, const method_inst_t *method,
    const char *sender);

// Remove all active sessions for a bot instance.
// inst: bot instance
void bot_session_clear(bot_inst_t *inst);

// Get the number of active sessions for a bot instance.
// returns: session count
// inst: bot instance
uint32_t bot_session_count(const bot_inst_t *inst);

// -----------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------

// Start a bot instance. Resolves and subscribes to all bound methods,
// calls driver start(). Transitions CREATED -> RUNNING.
// returns: SUCCESS or FAIL
// inst: bot instance
bool bot_start(bot_inst_t *inst);

// Stop a bot instance. Calls driver stop(), unsubscribes from all
// methods. Transitions RUNNING -> STOPPING -> CREATED.
// returns: SUCCESS or FAIL
// inst: bot instance
bool bot_stop(bot_inst_t *inst);

// -----------------------------------------------------------------------
// State and statistics
// -----------------------------------------------------------------------

// Get current state of an instance.
// returns: current bot state
// inst: bot instance
bot_state_t bot_get_state(const bot_inst_t *inst);

// returns: human-readable name of a bot state
// s: bot state enum value
const char *bot_state_name(bot_state_t s);

// Get bot subsystem statistics (thread-safe snapshot).
// out: destination for the snapshot
void bot_get_stats(bot_stats_t *out);

// -----------------------------------------------------------------------
// Iteration
// -----------------------------------------------------------------------

// Callback for bot instance iteration.
// name: instance name
// driver_name: bot driver name (e.g., "command")
// state: current instance state
// method_count: number of bound methods
// session_count: number of active sessions
// userns_name: bound user namespace name, or NULL
// data: opaque user data
typedef void (*bot_iter_cb_t)(const char *name, const char *driver_name,
    bot_state_t state, uint32_t method_count, uint32_t session_count,
    const char *userns_name, void *data);

// Iterate all bot instances, calling cb for each.
// cb: callback invoked for each instance
// data: opaque user data passed to cb
void bot_iterate(bot_iter_cb_t cb, void *data);

// Get the driver name for a bot instance.
// returns: driver name string
// inst: bot instance
const char *bot_driver_name(const bot_inst_t *inst);

// Get the number of bound methods for a bot instance.
// returns: method count
// inst: bot instance
uint32_t bot_method_count(const bot_inst_t *inst);

// -----------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------

// Initialize the bot subsystem.
void bot_init(void);

// Register per-bot method KV keys by copying the method plugin's KV
// schema into the bot's namespace. Transforms bare suffix keys
// to bot.<botname>.<kind>.* with the same types and defaults.
// returns: number of keys registered (0 if plugin not found or no schema)
// botname: bot instance name
// method_kind: method plugin kind (e.g., "irc")
uint32_t bot_register_method_kv(const char *botname, const char *method_kind);

// Create bot persistence tables in the database.
// Must be called after db_init().
// returns: SUCCESS or FAIL
bool bot_ensure_tables(void);

// Restore bot instances from database. Creates instances, binds
// methods, sets user namespaces, and auto-starts previously running
// bots. Must be called after plugins are started and KV is loaded.
// returns: SUCCESS or FAIL
bool bot_restore(void);

// Register KV configuration keys and load values. Must be called
// after kv_init() and kv_load().
void bot_register_config(void);

// Shut down the bot subsystem. Stops and destroys all instances.
void bot_exit(void);

#ifdef BOT_INTERNAL

#include "common.h"
#include "clam.h"
#include "db.h"
#include "kv.h"
#include "mem.h"
#include "plugin.h"
#include "pool.h"
#include "task.h"

// Per-instance method binding record.
typedef struct bot_method
{
  char               method_name[METHOD_NAME_SZ];
  char               method_kind[PLUGIN_NAME_SZ]; // plugin kind (e.g., "irc")
  method_inst_t     *inst;          // resolved at start time
  bool               subscribed;    // true if currently subscribed
  bool               created_by_bot; // true if bot_start() created the instance
  struct bot_method *next;
} bot_method_t;

// Active authenticated session.
typedef struct bot_session
{
  char                username[USERNS_USER_SZ];    // authenticated username
  method_inst_t      *method;                      // method authenticated on
  char                sender[METHOD_SENDER_SZ];    // protocol-level sender
  time_t              login_time;                   // session creation time
  time_t              auth_time;                    // authentication timestamp
  time_t              last_seen;                    // last activity timestamp
  struct bot_session *next;
} bot_session_t;

// Bot instance.
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
  struct bot_inst       *next;
};

// Cached configuration (loaded from KV).
typedef struct
{
  uint32_t max_methods;
  uint32_t max_sessions;
} bot_cfg_t;

static bot_cfg_t bot_cfg = {
  .max_methods  = 16,
  .max_sessions = 256,
};

// Module state.
static bot_inst_t      *bot_list  = NULL;
static pthread_mutex_t  bot_mutex;
static uint32_t         bot_count = 0;
static bool             bot_ready = false;

// Method binding freelist.
static bot_method_t    *bot_method_freelist    = NULL;
static uint32_t         bot_method_free_count  = 0;

// Session freelist.
static bot_session_t   *bot_session_freelist   = NULL;
static uint32_t         bot_session_free_count = 0;

// Lifetime counters.
static uint32_t         bot_stat_discoveries   = 0;

#endif // BOT_INTERNAL

#endif // BM_BOT_H
