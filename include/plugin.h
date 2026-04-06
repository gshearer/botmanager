#ifndef BM_PLUGIN_H
#define BM_PLUGIN_H

#include <stdbool.h>
#include <stdint.h>

#include "kv.h"

// Current plugin API version. Plugins built against a different
// version are rejected at load time.
#define PLUGIN_API_VERSION   11

// Entry point symbol that every plugin must export.
#define PLUGIN_ENTRY_SYMBOL  "bm_plugin_desc"

// Limits.
#define PLUGIN_NAME_SZ       64
#define PLUGIN_VER_SZ        32
#define PLUGIN_FEATURE_SZ    64
#define PLUGIN_MAX_FEATURES  16

// Plugin types.
typedef enum
{
  PLUGIN_CORE,          // extends core functionality
  PLUGIN_DB,            // database engine driver
  PLUGIN_METHOD,        // human interaction method (IRC, Slack, etc.)
  PLUGIN_BOT,           // bot behavior (command, scraper, etc.)
  PLUGIN_SERVICE,       // external API integration (REST, WebSocket, etc.)
  PLUGIN_MISC,          // miscellaneous user command extension (registers commands)
  PLUGIN_PERSONALITY    // language/messaging personality
} plugin_type_t;

// Plugin lifecycle states.
typedef enum
{
  PLUGIN_DISCOVERED,    // .so file found on disk
  PLUGIN_LOADED,        // dlopen'd, descriptor read and validated
  PLUGIN_INITIALIZED,   // init callback called
  PLUGIN_RUNNING,       // start callback called
  PLUGIN_STOPPING,      // stop callback called
  PLUGIN_UNLOADED       // dlclose'd
} plugin_state_t;

// Feature declaration for dependency resolution.
typedef struct
{
  char name[PLUGIN_FEATURE_SZ];
} plugin_feature_t;

// KV schema entry: a configuration key the plugin declares.
typedef struct
{
  const char *key;
  kv_type_t   type;
  const char *default_val;
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

// Plugin descriptor. Every plugin exports a const instance of this
// struct as the PLUGIN_ENTRY_SYMBOL symbol.
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

  // Lifecycle callbacks.
  bool (*init)(void);     // register state, set up internals
  bool (*start)(void);    // begin active operation
  bool (*stop)(void);     // drain in-flight work
  void (*deinit)(void);   // final cleanup

  // Type-specific extension data (e.g., db_driver_t* for DB plugins).
  const void *ext;

  // Named KV schema groups for dynamically-created entities.
  const plugin_kv_group_t *kv_groups;
  uint32_t                 kv_groups_count;
} plugin_desc_t;

// Plugin subsystem statistics.
typedef struct
{
  uint32_t loaded;            // currently loaded plugins
  uint32_t discovered;        // lifetime plugins found during scan
  uint32_t rejected;          // lifetime API version mismatches
  uint32_t load_errors;       // lifetime dlopen/symbol failures
} plugin_stats_t;

// Get plugin subsystem statistics (thread-safe snapshot).
// out: destination for the snapshot
void plugin_get_stats(plugin_stats_t *out);

// returns: SUCCESS or FAIL
// path: file path to the .so shared library
bool plugin_load(const char *path);

// returns: SUCCESS or FAIL (not found)
// name: plugin name to unload
bool plugin_unload(const char *name);

// returns: number of plugins successfully loaded
// dir: directory to scan for .so files (recursive)
uint32_t plugin_discover(const char *dir);

// returns: plugin descriptor, or NULL
// name: plugin name to find
const plugin_desc_t *plugin_find(const char *name);

// returns: descriptor of the plugin providing the feature, or NULL
// feature: feature name to search for
const plugin_desc_t *plugin_find_feature(const char *feature);

// returns: descriptor of matching plugin, or NULL
// type: plugin type to match
// kind: optional kind filter (NULL or "" for any)
const plugin_desc_t *plugin_find_type(plugin_type_t type, const char *kind);

// returns: plugin lifecycle state (PLUGIN_UNLOADED if not found)
// name: plugin name to query
plugin_state_t plugin_get_state(const char *name);

// returns: number of currently loaded plugins
uint32_t plugin_count(void);

// Callback for plugin iteration.
// name: plugin name
// version: plugin version string
// path: file path to the .so
// type: plugin type enum
// kind: plugin kind string
// state: current lifecycle state
// data: opaque user data
typedef void (*plugin_iterate_cb_t)(const char *name, const char *version,
    const char *path, plugin_type_t type, const char *kind,
    plugin_state_t state, void *data);

// Iterate all loaded plugins.
// cb: callback invoked for each plugin
// data: opaque user data passed to callback
void plugin_iterate(plugin_iterate_cb_t cb, void *data);

// Register plugin commands (/show plugin, /plugin load|unload).
// Must be called after admin_init().
void plugin_register_commands(void);

// Register plugin KV keys (core.plugin.autoload).
void plugin_register_config(void);

// Load plugins listed in core.plugin.autoload that are not already loaded.
// returns: number of additionally loaded plugins
// plugin_dir: directory to scan for .so files
uint32_t plugin_load_autoload(const char *plugin_dir);

// returns: human-readable name of a plugin type
// t: plugin type enum value
const char *plugin_type_name(plugin_type_t t);

// returns: human-readable name of a plugin state
// s: plugin state enum value
const char *plugin_state_name(plugin_state_t s);

// Resolve plugin dependency order. Topologically sorts loaded plugins
// so that providers precede their dependents. Validates all required
// features are satisfied.
// returns: SUCCESS or FAIL (unsatisfied or circular dependency)
bool plugin_resolve(void);

// Initialize all loaded plugins in dependency order.
// Calls init callback on each LOADED plugin, transitioning to INITIALIZED.
// returns: SUCCESS or FAIL (if any plugin's init callback fails)
bool plugin_init_all(void);

// Start all initialized plugins in dependency order.
// Calls start callback on each INITIALIZED plugin, transitioning to RUNNING.
// returns: SUCCESS or FAIL (if any plugin's start callback fails)
bool plugin_start_all(void);

// Stop all running plugins in reverse dependency order.
// Calls stop callback on each RUNNING plugin, transitioning to STOPPING.
void plugin_stop_all(void);

// Deinitialize all initialized/stopping plugins in reverse dependency order.
// Calls deinit callback, transitioning to LOADED.
void plugin_deinit_all(void);

// returns: schema group descriptor, or NULL if not found
// plugin_name: name of the plugin to search
// group_name: schema group name (e.g., "channel")
const plugin_kv_group_t *plugin_kv_group_find(const char *plugin_name,
    const char *group_name);

// Register KV entries for a new entity instance using a schema group.
// Builds keys by applying varargs to the group's key_prefix pattern,
// then appending each schema entry's suffix.
// returns: number of entries successfully registered
// group: schema group to instantiate
// ...: string arguments for the key_prefix format placeholders
uint32_t plugin_kv_group_register(const plugin_kv_group_t *group, ...);

// Callback for schema group iteration.
// plugin: descriptor of the plugin owning the group
// group: schema group being visited
// data: opaque user data
typedef void (*plugin_kv_group_iter_cb_t)(const plugin_desc_t *plugin,
    const plugin_kv_group_t *group, void *data);

// Iterate all schema groups across all loaded plugins.
// cb: callback invoked for each group
// data: opaque user data passed to callback
void plugin_kv_group_iterate(plugin_kv_group_iter_cb_t cb, void *data);

// Initialize the plugin subsystem.
void plugin_init(void);

// Shut down: stop, deinit, and unload all plugins in reverse dependency order.
void plugin_exit(void);

#ifdef PLUGIN_INTERNAL

#include "common.h"
#include "bconf.h"
#include "bot.h"
#include "clam.h"
#include "cmd.h"
#include "colors.h"
#include "mem.h"
#include "userns.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#define PLUGIN_PATH_SZ  512

// Internal plugin record.
typedef struct plugin_rec
{
  char                  path[PLUGIN_PATH_SZ];
  void                 *handle;     // dlopen handle
  const plugin_desc_t  *desc;       // points into .so memory
  plugin_state_t        state;
  struct plugin_rec    *next;
} plugin_rec_t;

// Module state.
static plugin_rec_t *plugins      = NULL;
static uint32_t      n_plugins    = 0;
static bool          plugin_ready = false;

// Lifetime counters for statistics.
static uint32_t      n_discovered  = 0;
static uint32_t      n_rejected    = 0;
static uint32_t      n_load_errors = 0;

#endif // PLUGIN_INTERNAL

#endif // BM_PLUGIN_H
