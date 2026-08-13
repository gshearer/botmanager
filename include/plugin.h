#ifndef BM_PLUGIN_H
#define BM_PLUGIN_H

#include <stdbool.h>
#include <stdint.h>

#include "kv.h"

// Plugins built against a different version are rejected at load time.
#define PLUGIN_API_VERSION   15

// Entry point symbol that every plugin must export.
#define PLUGIN_ENTRY_SYMBOL  "bm_plugin_desc"

#define PLUGIN_NAME_SZ       64
#define PLUGIN_VER_SZ        32
#define PLUGIN_FEATURE_SZ    64
#define PLUGIN_MAX_FEATURES  16

typedef enum
{
  PLUGIN_CORE,          // extends core functionality
  PLUGIN_DB,            // database engine driver
  PLUGIN_METHOD,        // how a bot meets humans (IRC, voice, etc.)
  PLUGIN_BOT,           // the mind that drives a bot (chat, etc.)
  PLUGIN_SERVICE,       // external API integration (REST, WebSocket, etc.)
  PLUGIN_MISC,          // miscellaneous user command extension (registers commands)
  PLUGIN_PERSONALITY,   // language/messaging personality
  PLUGIN_FEATURE,       // capability layer composed atop methods (whenmoon, etc.)
  PLUGIN_STRATEGY       // trading strategy module (whenmoon-attached, etc.)
} plugin_type_t;

typedef enum
{
  PLUGIN_DISCOVERED,    // .so file found on disk
  PLUGIN_LOADED,        // dlopen'd, descriptor read and validated
  PLUGIN_INITIALIZED,   // init callback called
  PLUGIN_RUNNING,       // start callback called
  PLUGIN_STOPPING,      // stop callback called
  PLUGIN_UNLOADED,      // dlclose'd

  // Stopped and deinitialized, but a Class-B reference kept it mapped
  // and the dlclose was refused. Nothing of it works and nothing of it
  // dangles; only a daemon restart clears it. Its own state is gone, so
  // it is never initialized, started or torn down again.
  PLUGIN_ZOMBIE
} plugin_state_t;

typedef struct
{
  char name[PLUGIN_FEATURE_SZ];
} plugin_feature_t;

// A configuration key the plugin declares.
typedef struct
{
  const char *key;
  kv_type_t   type;
  const char *default_val;
  const char *help;          // human-readable help (static, may be NULL)
  kv_cb_t     cb;            // optional change callback (NULL for none)
  const kv_nl_t *nl;         // optional NL responder; NULL = not NL-visible
} plugin_kv_entry_t;

// Named KV schema group: describes the configuration keys for a
// dynamically-created entity (e.g., an IRC channel, a server).
// Plugins declare these so the system can introspect and present
// available properties to users before entity creation.
typedef struct
{
  const char              *name;         // machine name (e.g., "channel")
  const char              *description;  // human-readable description
  const char              *key_prefix;   // printf pattern (e.g., "bot.%s.irc.chan.%s.")
  uint8_t                  prefix_args;  // number of %s placeholders
  const char              *cmd_name;     // managing command name
  const plugin_kv_entry_t *schema;       // bare suffix entries
  uint32_t                 schema_count;
} plugin_kv_group_t;

// Every plugin exports a const instance of this struct as the
// PLUGIN_ENTRY_SYMBOL symbol.
typedef struct
{
  uint32_t             api_version;
  char                 name[PLUGIN_NAME_SZ];
  char                 version[PLUGIN_VER_SZ];
  plugin_type_t        type;
  char                 kind[PLUGIN_NAME_SZ];

  // Features this plugin provides and requires.
  plugin_feature_t     provides[PLUGIN_MAX_FEATURES];
  uint32_t             provides_count;
  plugin_feature_t     requires[PLUGIN_MAX_FEATURES];
  uint32_t             requires_count;

  // Plugin-level KV schema: registered once under "plugin.<kind>.*".
  const plugin_kv_entry_t *kv_schema;
  uint32_t                 kv_schema_count;

  // Instance-level KV schema: cloned per consumer at bind time.
  // For method plugins, keys are bare suffixes (e.g., "nick", "network")
  // that get prefixed as "bot.<botname>.<kind>.<suffix>".
  const plugin_kv_entry_t *kv_inst_schema;
  uint32_t                 kv_inst_schema_count;

  bool (*init)(void);     // register state, set up internals
  bool (*start)(void);    // begin active operation

  // Drain in-flight work — and *join* what runs in this mapping. A
  // plugin that spawned a persist task must signal it and call
  // task_persist_join(); signalling alone says nothing about whether
  // the thread has left the plugin's .text. FAIL here refuses the
  // unload with the plugin left running and intact — the one refusal
  // that costs nothing. What stop() forgets, the teardown's quiescence
  // barrier catches instead, at the price of a zombie.
  bool (*stop)(void);

  void (*deinit)(void);   // final cleanup

  // A reload takes the mapping away and gives an identical one back.
  // Everything a plugin registers comes back with it — commands, KV,
  // the descriptor itself — but nothing it *holds* does: resolved
  // pointers into peer plugins, attachments, live handles. suspend()
  // is where a plugin lets those go and writes down whatever it wants
  // to find again; resume() is where it re-resolves and restores.
  //
  // Both are optional, and NULL is the honest answer for a plugin whose
  // whole state is registrations (every leaf command plugin, every
  // strategy). The snapshot must NOT live in the plugin's own mapping —
  // it is unmapped between the two calls — so persist it (the DB is
  // already proven for whenmoon's markets) or hand it to core.
  //
  // suspend() runs before stop(), in reverse dependency order across
  // the whole closure, and FAIL there refuses the reload while it is
  // still free: nothing has come down yet. resume() runs after start(),
  // in dependency order; a FAIL there is reported and logged, but the
  // plugin is already back and running.
  bool (*suspend)(void);
  bool (*resume)(void);

  // Type-specific extension data (e.g., db_driver_t* for DB plugins).
  const void *ext;

  // Named KV schema groups for dynamically-created entities.
  const plugin_kv_group_t *kv_groups;
  uint32_t                 kv_groups_count;
} plugin_desc_t;

typedef struct
{
  uint32_t loaded;            // currently loaded plugins
  uint32_t discovered;        // lifetime plugins found during scan
  uint32_t rejected;          // lifetime API version mismatches
  uint32_t load_errors;       // lifetime dlopen/symbol failures
} plugin_stats_t;

void plugin_get_stats(plugin_stats_t *out);
bool plugin_load(const char *path);

// Longest offender description the quiescence barrier prints — a kind,
// a name, and the state it was caught in.
#define PLUGIN_OFFENDER_SZ  160

// What the teardown sweep between deinit() and dlclose found.
//
// `reclaimed` counts the Class-A registrations — commands, KV entries,
// clam subscribers, bot KV contributors — that the plugin's deinit()
// left behind and core dropped on its behalf. Nothing dangles either
// way; the number is the plugin's tidiness, not the daemon's safety.
//
// `residual` counts what core will NOT drop for anyone: Class-B
// references (running tasks, in-flight requests, a bound driver vtable)
// still pointing into the mapping. Those genuinely dangle past dlclose,
// so a non-zero `residual` after the quiescence budget expires refuses
// the unload rather than risking the SIGSEGV.
//
// `zombie` is that refusal: the plugin was stopped and deinitialized
// before the residual was found, so it stays mapped — nothing dangles,
// nothing works — until the daemon restarts. `offender` names the first
// task or request that held the mapping open, empty when the residual
// is something that does not drain (a bound vtable, a driver).
typedef struct
{
  uint32_t reclaimed;
  uint32_t residual;
  bool     zombie;
  char     offender[PLUGIN_OFFENDER_SZ];
} plugin_unload_report_t;

// `report` is optional; callers that face a human should pass one and
// say what it holds.
bool plugin_unload(const char *name, plugin_unload_report_t *report);

// A reload cycles the named plugin *and everything that transitively
// requires it* — a strategy cannot outlive the whenmoon it links
// against, so unloading one alone is refused and always was. The
// cascade is the verb that does the obvious thing.
//
// `dependents` is the closure size excluding the named plugin;
// `cycled` counts what actually came back up. `started` is false while
// the cascade can still be refused for free — nothing has been unloaded
// yet — and `rolled_back` says an unload refused after that, so the
// plugins already taken down were restored and the system is where it
// began; a partial cascade is worse than none. `failed` names the
// plugin that blocked, refused, or would not come back, and `detail`
// carries its reason when there was one to give.
#define PLUGIN_RELOAD_MAX_CLOSURE  64

// `zombie` qualifies `rolled_back`: the refusal landed after the
// plugin's own teardown, so the one that said no is stopped,
// deinitialized and still mapped. Its dependents came back, but it did
// not — only a restart clears it.
//
// `rebound` counts the bots whose driver — or whose bound method —
// the cascade took away and gave back. A bot bound to a plugin in the
// closure used to refuse the reload outright; now it is detached before
// the unload and re-attached after the load, keeping its name, its KV,
// its sessions and, for a driver rebind, its live connections.
typedef struct
{
  uint32_t dependents;
  uint32_t cycled;
  uint32_t rebound;
  bool     started;
  bool     rolled_back;
  bool     zombie;
  char     failed[PLUGIN_NAME_SZ];
  char     detail[PLUGIN_OFFENDER_SZ];
} plugin_reload_report_t;

// `report` is optional. FAIL leaves every plugin it could revive
// running; what it could not is named in the report and the log.
bool plugin_reload(const char *name, plugin_reload_report_t *report);
uint32_t plugin_discover(const char *dir);
const plugin_desc_t *plugin_find(const char *name);
const plugin_desc_t *plugin_find_feature(const char *feature);

const plugin_desc_t *plugin_find_type(plugin_type_t type, const char *kind);

// Returns PLUGIN_UNLOADED if not found.
plugin_state_t plugin_get_state(const char *name);

// Resolve a symbol from a loaded plugin's .so, by plugin name. Plugins
// are dlopen'd with RTLD_LOCAL, so host code that wants to call into a
// plugin-resident helper (e.g. the searxng plugin's sxng_search) has no
// direct link-time reference and must look the symbol up at runtime.
// Returns NULL when the plugin is not loaded or the symbol is absent;
// both cases are silent — the caller decides the severity of a miss.
void *plugin_dlsym(const char *plugin_name, const char *symbol);

// Like plugin_dlsym, but also registers the caller's `slot` (the
// address of its `static fn_t cached`) for invalidation on plugin
// unload. The dlsym-shim pattern (see
// plugins/extension/inference/engine/inference.h,
// plugins/extension/whenmoon/whenmoon_strategy.h) caches the resolved
// function pointer in a static slot inside the consumer; without
// invalidation that pointer would dangle when the target plugin is
// unloaded. The caller keeps its existing union-launder +
// __atomic_store_n(RELEASE) pattern — this function only registers
// the slot and returns the resolved void*. Registration is idempotent.
// Returns NULL on lookup miss (same as plugin_dlsym); shim macros
// log+abort on that path.
void *plugin_dlsym_cached(const char *plugin_name, const char *symbol,
    void **slot);

// True when `ptr` lies inside the mapping of the named loaded plugin.
// Attribution is by the object's own PT_LOAD extent, never by a label
// the plugin chooses (cmd_def_t.module and friends are not plugin
// identity — see root TODO.md §PLIFE-1 "Standing facts").
//
// LIMITATION: only code and static data are attributable. A heap
// pointer belongs to no mapping, so this catches exactly the
// references that SIGSEGV after dlclose, not heap leaks.
bool plugin_owns_ptr(const char *plugin_name, const void *ptr);

// ---------------------------------------------------------------------------
// Unmap notification — for pointers core cannot see
// ---------------------------------------------------------------------------
//
// Core reclaims Class A because it owns the registries. A plugin that
// keeps pointers into ANOTHER plugin's mapping — a completion callback
// handed to it with a request, a vtable it was given — owns a registry
// core cannot walk, so neither the reclaim nor the audit can help: the
// dangle only shows up later, when the callback fires into an address
// that is no longer mapped (measured: an in-flight LLM request whose
// requester was cycled by `/plugin reload`).
//
// A listener registered here is told, once per unload, the address range
// about to disappear. It must drop every pointer it holds inside that
// range before returning; nothing else will. `lo` is inclusive, `hi`
// exclusive. Listeners fire after the plugin's deinit() and before the
// residual audit, on the unloading thread, and must not call any
// plugin_* API.
//
// Register from the holder's init(), unregister from its deinit() —
// though a listener whose own callback lies in an unloading mapping is
// dropped automatically, so a forgotten unregister cannot dangle.
typedef void (*plugin_unmap_cb_t)(uintptr_t lo, uintptr_t hi, void *data);

void plugin_unmap_notify_register(plugin_unmap_cb_t cb, void *data);
void plugin_unmap_notify_unregister(plugin_unmap_cb_t cb);

// Emitter for plugin_audit. `line` is valid only for the call.
typedef void (*plugin_audit_emit_t)(const char *line, void *data);

// Report every registration still pointing into the named plugin's
// mapping — one emitted line per leaked reference. `emit` may be NULL
// to count without reporting.
// returns: number of leaked references (0 == clean).
uint32_t plugin_audit(const char *plugin_name, plugin_audit_emit_t emit,
    void *data);

uint32_t plugin_count(void);

typedef void (*plugin_iterate_cb_t)(const char *name, const char *version,
    const char *path, plugin_type_t type, const char *kind,
    plugin_state_t state, void *data);

void plugin_iterate(plugin_iterate_cb_t cb, void *data);

// Must be called after admin_init().
void plugin_register_commands(void);

void plugin_register_config(void);

// Load plugins listed in core.plugin.autoload that are not already loaded.
// Returns the number of additionally loaded plugins.
uint32_t plugin_load_autoload(const char *plugin_dir);

const char *plugin_type_name(plugin_type_t t);
const char *plugin_state_name(plugin_state_t s);

// Topologically sorts loaded plugins so that providers precede their
// dependents. Validates all required features are satisfied.
bool plugin_resolve(void);

// Calls init callback on each LOADED plugin, transitioning to INITIALIZED.
bool plugin_init_all(void);

// Calls start callback on each INITIALIZED plugin, transitioning to RUNNING.
bool plugin_start_all(void);

// Reverse dependency order; transitions RUNNING -> STOPPING.
void plugin_stop_all(void);

// Reverse dependency order; transitions to LOADED.
void plugin_deinit_all(void);

const plugin_kv_group_t *plugin_kv_group_find(const char *plugin_name,
    const char *group_name);

// Register KV entries for a new entity instance using a schema group.
// Builds keys by applying varargs to the group's key_prefix pattern,
// then appending each schema entry's suffix.
uint32_t plugin_kv_group_register(const plugin_kv_group_t *group, ...);

typedef void (*plugin_kv_group_iter_cb_t)(const plugin_desc_t *plugin,
    const plugin_kv_group_t *group, void *data);

void plugin_kv_group_iterate(plugin_kv_group_iter_cb_t cb, void *data);

void plugin_init(void);

// Stop, deinit, and unload all plugins in reverse dependency order.
void plugin_exit(void);

#ifdef PLUGIN_INTERNAL

#include "common.h"
#include "bconf.h"
#include "bot.h"
#include "clam.h"
#include "cmd.h"
#include "colors.h"
#include "alloc.h"
#include "userns.h"

#include "curl.h"
#include "db.h"
#include "method.h"
#include "task.h"

#include <dlfcn.h>
#include <link.h>
#include <stdarg.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#define PLUGIN_PATH_SZ  512

// Audit report bounds. A plugin with more leaks than this is broken
// beyond the point where more lines help; the count stays exact.
#define PLUGIN_AUDIT_LINE_SZ    200
#define PLUGIN_AUDIT_MAX_LINES  256

// How long the teardown waits for transient Class-B references to end,
// and how often it looks. The budget is a KV knob; the poll interval is
// not — it only trades a little idle CPU for how promptly a plugin that
// went quiet gets unloaded.
#define KV_UNLOAD_QUIESCE_MS     "core.plugin.unload_quiesce_ms"
#define PLUGIN_QUIESCE_POLL_MS   50

// The address range one loaded object occupies, plus the short name to
// print for it. Resolved once per audit, so the per-pointer test is
// arithmetic rather than a loader-lock round trip.
typedef struct
{
  uintptr_t lo;
  uintptr_t hi;
  char      soname[PLUGIN_NAME_SZ];
} plugin_map_t;

// Registry of dlsym-shim cache slots. Each entry pairs a target
// plugin name with the void** slot a consumer is caching its
// resolved pointer in. On plugin unload, the loader walks this
// list and either NULLs the slot (when the unloaded plugin is the
// target — protects against a consumer dispatching into freed
// .text) or drops the entry (when the unloaded plugin is the
// consumer — its slot memory is going away with dlclose).
typedef struct dlsym_cache_rec
{
  const char              *target_plugin;  // string literal in consumer's .rodata
  void                   **slot;           // address of caller's static cache
  const char              *consumer_so;    // owned copy (mem_alloc); dladdr() at registration
  struct dlsym_cache_rec  *next;
} dlsym_cache_rec_t;

// What a reload must remember about a plugin across its own teardown:
// plugin_unload() frees the plugin_rec_t, so the path the .so came from
// has to be copied out BEFORE the unload or there is nothing to load
// back (wm_strategy_reload() learned this the hard way — see
// strategy.c's "unloaded %s but no path captured").
typedef struct
{
  char name[PLUGIN_NAME_SZ];
  char path[PLUGIN_PATH_SZ];
} plugin_snap_t;

// Holders of foreign pointers, told when a mapping goes away. Small,
// static-capacity: this is a handful of subsystems (inference's in-flight
// LLM requests today), not a general event bus.
#define PLUGIN_UNMAP_MAX_LISTENERS  16

typedef struct
{
  plugin_unmap_cb_t cb;
  void             *data;
} plugin_unmap_rec_t;

typedef struct plugin_rec
{
  char                  path[PLUGIN_PATH_SZ];
  void                 *handle;     // dlopen handle
  const plugin_desc_t  *desc;       // points into .so memory
  plugin_state_t        state;
  struct plugin_rec    *next;
} plugin_rec_t;

static plugin_rec_t      *plugins             = NULL;
static uint32_t           n_plugins           = 0;
static bool               plugin_ready        = false;
static dlsym_cache_rec_t *dlsym_cache_head    = NULL;
static pthread_mutex_t    dlsym_cache_mutex   = PTHREAD_MUTEX_INITIALIZER;

static plugin_unmap_rec_t plugin_unmap_listeners[PLUGIN_UNMAP_MAX_LISTENERS];
static uint32_t           n_unmap_listeners = 0;
static pthread_mutex_t    plugin_unmap_mutex = PTHREAD_MUTEX_INITIALIZER;

#endif // PLUGIN_INTERNAL

#endif // BM_PLUGIN_H
