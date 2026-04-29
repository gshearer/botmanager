#ifndef BM_PLUGIN_H
#define BM_PLUGIN_H

#include <stdbool.h>
#include <stdint.h>

#include "kv.h"

// Plugins built against a different version are rejected at load time.
#define PLUGIN_API_VERSION   14

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
  PLUGIN_PROTOCOL,      // human interaction protocol (IRC, Slack, etc.)
  PLUGIN_METHOD,        // bot interaction method (chat, command, etc.)
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
  PLUGIN_UNLOADED       // dlclose'd
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
  bool (*stop)(void);     // drain in-flight work
  void (*deinit)(void);   // final cleanup

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
bool plugin_unload(const char *name);
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

#include <dlfcn.h>
#include <stdarg.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#define PLUGIN_PATH_SZ  512

typedef struct plugin_rec
{
  char                  path[PLUGIN_PATH_SZ];
  void                 *handle;     // dlopen handle
  const plugin_desc_t  *desc;       // points into .so memory
  plugin_state_t        state;
  struct plugin_rec    *next;
} plugin_rec_t;

static plugin_rec_t *plugins      = NULL;
static uint32_t      n_plugins    = 0;
static bool          plugin_ready = false;

static uint32_t      n_discovered  = 0;
static uint32_t      n_rejected    = 0;
static uint32_t      n_load_errors = 0;

#endif // PLUGIN_INTERNAL

#endif // BM_PLUGIN_H
