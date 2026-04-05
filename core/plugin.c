#define PLUGIN_INTERNAL
#include "plugin.h"
#include "util.h"

// -----------------------------------------------------------------------
// Find a plugin record by name.
// returns: plugin record pointer, or NULL
// name: plugin name to find
// -----------------------------------------------------------------------
static plugin_rec_t *
find_by_name(const char *name)
{
  for(plugin_rec_t *p = plugins; p != NULL; p = p->next)
  {
    if(strcmp(p->desc->name, name) == 0)
      return(p);
  }

  return(NULL);
}

// -----------------------------------------------------------------------
// Validate a plugin descriptor.
// returns: SUCCESS or FAIL
// desc: descriptor to validate
// path: file path (for diagnostics)
// -----------------------------------------------------------------------
static bool
validate_desc(const plugin_desc_t *desc, const char *path)
{
  if(desc->api_version != PLUGIN_API_VERSION)
  {
    clam(CLAM_WARN, "plugin", "%s: API version mismatch "
        "(plugin: %u, expected: %u)", path, desc->api_version,
        PLUGIN_API_VERSION);
    return(FAIL);
  }

  if(desc->name[0] == '\0')
  {
    clam(CLAM_WARN, "plugin", "%s: empty plugin name", path);
    return(FAIL);
  }

  if(desc->version[0] == '\0')
  {
    clam(CLAM_WARN, "plugin", "%s: empty plugin version", path);
    return(FAIL);
  }

  if(desc->provides_count == 0)
  {
    clam(CLAM_WARN, "plugin", "%s: must provide at least one feature",
        path);
    return(FAIL);
  }

  if(desc->provides_count > PLUGIN_MAX_FEATURES)
  {
    clam(CLAM_WARN, "plugin", "%s: provides_count %u exceeds max %u",
        path, desc->provides_count, PLUGIN_MAX_FEATURES);
    return(FAIL);
  }

  if(desc->requires_count > PLUGIN_MAX_FEATURES)
  {
    clam(CLAM_WARN, "plugin", "%s: requires_count %u exceeds max %u",
        path, desc->requires_count, PLUGIN_MAX_FEATURES);
    return(FAIL);
  }

  if(find_by_name(desc->name) != NULL)
  {
    clam(CLAM_WARN, "plugin", "%s: plugin '%s' already loaded",
        path, desc->name);
    return(FAIL);
  }

  return(SUCCESS);
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

// returns: SUCCESS or FAIL
// path: file path to the .so shared library
bool
plugin_load(const char *path)
{
  if(path == NULL || !plugin_ready)
    return(FAIL);

  // dlopen the shared library.
  void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);

  if(handle == NULL)
  {
    clam(CLAM_WARN, "plugin", "dlopen '%s': %s", path, dlerror());
    return(FAIL);
  }

  // Resolve the entry point symbol.
  dlerror();  // clear any prior error
  const plugin_desc_t *desc = dlsym(handle, PLUGIN_ENTRY_SYMBOL);
  const char *err = dlerror();

  if(err != NULL || desc == NULL)
  {
    clam(CLAM_WARN, "plugin", "'%s': symbol '%s' not found%s%s",
        path, PLUGIN_ENTRY_SYMBOL,
        err ? ": " : "", err ? err : "");
    dlclose(handle);
    return(FAIL);
  }

  // Validate the descriptor.
  if(validate_desc(desc, path) != SUCCESS)
  {
    dlclose(handle);
    return(FAIL);
  }

  // Create a plugin record.
  plugin_rec_t *rec = mem_alloc("plugin", "record", sizeof(plugin_rec_t));

  strncpy(rec->path, path, PLUGIN_PATH_SZ - 1);
  rec->path[PLUGIN_PATH_SZ - 1] = '\0';
  rec->handle = handle;
  rec->desc   = desc;
  rec->state  = PLUGIN_LOADED;

  // Prepend to list.
  rec->next = plugins;
  plugins   = rec;
  n_plugins++;

  clam(CLAM_INFO, "plugin", "loaded '%s' v%s (%s%s%s) from %s",
      desc->name, desc->version, plugin_type_name(desc->type),
      desc->kind[0] ? ":" : "", desc->kind, path);

  for(uint32_t i = 0; i < desc->provides_count; i++)
    clam(CLAM_DEBUG, "plugin", "  provides: %s", desc->provides[i].name);

  for(uint32_t i = 0; i < desc->requires_count; i++)
    clam(CLAM_DEBUG, "plugin", "  requires: %s", desc->requires[i].name);

  return(SUCCESS);
}

// returns: SUCCESS or FAIL (not found, or has active dependents)
// name: plugin name to unload
bool
plugin_unload(const char *name)
{
  if(name == NULL || !plugin_ready)
    return(FAIL);

  plugin_rec_t *target = find_by_name(name);

  if(target == NULL)
  {
    clam(CLAM_WARN, "plugin", "unload: '%s' not found", name);
    return(FAIL);
  }

  // Refuse if any other loaded plugin depends on features we provide.
  for(plugin_rec_t *q = plugins; q != NULL; q = q->next)
  {
    if(q == target)
      continue;

    for(uint32_t r = 0; r < q->desc->requires_count; r++)
    {
      const char *need = q->desc->requires[r].name;

      for(uint32_t k = 0; k < target->desc->provides_count; k++)
      {
        if(strcmp(need, target->desc->provides[k].name) == 0)
        {
          clam(CLAM_WARN, "plugin", "cannot unload '%s': '%s' depends "
              "on feature '%s'", name, q->desc->name, need);
          return(FAIL);
        }
      }
    }
  }

  // Lifecycle teardown based on current state.
  if(target->state == PLUGIN_RUNNING)
  {
    clam(CLAM_DEBUG, "plugin", "stopping '%s' before unload", name);

    if(target->desc->stop != NULL)
      target->desc->stop();

    target->state = PLUGIN_STOPPING;
  }

  if(target->state == PLUGIN_INITIALIZED || target->state == PLUGIN_STOPPING)
  {
    clam(CLAM_DEBUG, "plugin", "deinitializing '%s' before unload", name);

    if(target->desc->deinit != NULL)
      target->desc->deinit();

    target->state = PLUGIN_LOADED;
  }

  // Remove from list.
  plugin_rec_t **pp = &plugins;

  while(*pp != NULL)
  {
    if(*pp == target)
    {
      *pp = target->next;
      clam(CLAM_INFO, "plugin", "unloading '%s'", name);
      dlclose(target->handle);
      mem_free(target);
      n_plugins--;
      return(SUCCESS);
    }

    pp = &(*pp)->next;
  }

  return(FAIL);  // unreachable
}

// returns: number of plugins successfully loaded
// dir: directory to scan for .so files (recursive)
uint32_t
plugin_discover(const char *dir)
{
  if(dir == NULL || !plugin_ready)
    return(0);

  DIR *d = opendir(dir);

  if(d == NULL)
  {
    clam(CLAM_WARN, "plugin", "cannot open directory '%s': %s",
        dir, strerror(errno));
    return(0);
  }

  uint32_t loaded = 0;
  struct dirent *ent;

  while((ent = readdir(d)) != NULL)
  {
    if(ent->d_name[0] == '.')
      continue;

    char path[PLUGIN_PATH_SZ];

    snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

    // Recurse into subdirectories.
    struct stat st;

    if(stat(path, &st) != 0)
      continue;

    if(S_ISDIR(st.st_mode))
    {
      loaded += plugin_discover(path);
      continue;
    }

    if(!S_ISREG(st.st_mode))
      continue;

    // Only consider .so files.
    size_t len = strlen(ent->d_name);

    if(len < 4 || strcmp(ent->d_name + len - 3, ".so") != 0)
      continue;

    if(plugin_load(path) == SUCCESS)
      loaded++;
  }

  closedir(d);
  return(loaded);
}

// Check if a feature is provided by any plugin in a subset.
// returns: true if found
// feature: feature name to search for
// arr: array of plugin records
// indices: array of indices into arr to search
// n: number of indices
static bool
resolve_feature_in(const char *feature, plugin_rec_t **arr,
    const uint32_t *indices, uint32_t n)
{
  for(uint32_t j = 0; j < n; j++)
  {
    const plugin_desc_t *pd = arr[indices[j]]->desc;

    for(uint32_t k = 0; k < pd->provides_count; k++)
    {
      if(strcmp(pd->provides[k].name, feature) == 0)
        return(true);
    }
  }

  return(false);
}

// Check if a feature is provided by any plugin in the full array,
// excluding the plugin at skip_idx.
// returns: true if found
// feature: feature name to search for
// arr: array of plugin records
// count: total plugins
// skip_idx: index to skip (the requester itself)
static bool
resolve_feature_exists(const char *feature, plugin_rec_t **arr,
    uint32_t count, uint32_t skip_idx)
{
  for(uint32_t j = 0; j < count; j++)
  {
    if(j == skip_idx)
      continue;

    const plugin_desc_t *pd = arr[j]->desc;

    for(uint32_t k = 0; k < pd->provides_count; k++)
    {
      if(strcmp(pd->provides[k].name, feature) == 0)
        return(true);
    }
  }

  return(false);
}

// Reorder the global plugin list to match the resolved order and log it.
// arr: array of plugin records
// order: resolved index order
// count: total plugins
static void
resolve_apply_order(plugin_rec_t **arr, const uint32_t *order,
    uint32_t count)
{
  plugins = arr[order[0]];

  for(uint32_t i = 0; i < count - 1; i++)
    arr[order[i]]->next = arr[order[i + 1]];

  arr[order[count - 1]]->next = NULL;

  clam(CLAM_DEBUG, "plugin", "dependency order resolved (%u plugins):",
      count);

  for(uint32_t i = 0; i < count; i++)
    clam(CLAM_DEBUG, "plugin", "  %u. %s", i + 1,
        arr[order[i]]->desc->name);
}

// Report unsatisfied dependencies and detect circular dependencies
// among unplaced plugins.
// arr: array of plugin records
// placed: per-plugin placement flags
// count: total plugins
static void
resolve_report_failures(plugin_rec_t **arr, const bool *placed,
    uint32_t count)
{
  bool all_exist = true;

  for(uint32_t i = 0; i < count; i++)
  {
    if(placed[i])
      continue;

    const plugin_desc_t *desc = arr[i]->desc;

    for(uint32_t r = 0; r < desc->requires_count; r++)
    {
      const char *need = desc->requires[r].name;

      if(!resolve_feature_exists(need, arr, count, i))
      {
        clam(CLAM_WARN, "plugin", "'%s' requires feature '%s' "
            "which no loaded plugin provides", desc->name, need);
        all_exist = false;
      }
    }
  }

  if(all_exist)
    clam(CLAM_WARN, "plugin", "circular dependency detected");
}

// -----------------------------------------------------------------------
// Resolve plugin dependency order.
// Topologically sorts loaded plugins so providers precede dependents.
// returns: SUCCESS or FAIL (unsatisfied or circular dependency)
// -----------------------------------------------------------------------
bool
plugin_resolve(void)
{
  if(!plugin_ready)
    return(FAIL);

  if(n_plugins == 0)
    return(SUCCESS);

  uint32_t count = n_plugins;

  // Collect all plugin records into an array for sorting.
  plugin_rec_t **arr = mem_alloc("plugin", "resolve",
      count * sizeof(plugin_rec_t *));

  uint32_t idx = 0;

  for(plugin_rec_t *p = plugins; p != NULL; p = p->next)
    arr[idx++] = p;

  bool     *placed = mem_alloc("plugin", "placed", count * sizeof(bool));
  uint32_t *order  = mem_alloc("plugin", "order", count * sizeof(uint32_t));
  uint32_t  n_placed = 0;

  memset(placed, 0, count * sizeof(bool));

  // Kahn's-style: repeatedly place plugins whose requirements are met
  // by already-placed plugins.
  bool progress = true;

  while(progress && n_placed < count)
  {
    progress = false;

    for(uint32_t i = 0; i < count; i++)
    {
      if(placed[i])
        continue;

      const plugin_desc_t *desc = arr[i]->desc;
      bool satisfied = true;

      for(uint32_t r = 0; r < desc->requires_count && satisfied; r++)
      {
        if(!resolve_feature_in(desc->requires[r].name, arr, order, n_placed))
          satisfied = false;
      }

      if(satisfied)
      {
        order[n_placed++] = i;
        placed[i] = true;
        progress = true;
      }
    }
  }

  bool result;

  if(n_placed == count)
  {
    resolve_apply_order(arr, order, count);
    result = SUCCESS;
  }

  else
  {
    resolve_report_failures(arr, placed, count);
    result = FAIL;
  }

  mem_free(order);
  mem_free(placed);
  mem_free(arr);
  return(result);
}

// -----------------------------------------------------------------------
// Lifecycle: init all loaded plugins (dependency order, head to tail).
// returns: SUCCESS or FAIL
// -----------------------------------------------------------------------
bool
plugin_init_all(void)
{
  if(!plugin_ready)
    return(FAIL);

  for(plugin_rec_t *p = plugins; p != NULL; p = p->next)
  {
    if(p->state != PLUGIN_LOADED)
      continue;

    clam(CLAM_INFO, "plugin", "initializing '%s'", p->desc->name);

    // Register KV schema entries declared by this plugin.
    if(p->desc->kv_schema != NULL && p->desc->kv_schema_count > 0)
    {
      for(uint32_t i = 0; i < p->desc->kv_schema_count; i++)
      {
        const plugin_kv_entry_t *e = &p->desc->kv_schema[i];
        kv_register(e->key, e->type, e->default_val, NULL, NULL);
      }

      clam(CLAM_DEBUG, "plugin", "'%s' registered %u KV key(s)",
          p->desc->name, p->desc->kv_schema_count);
    }

    if(p->desc->init != NULL && p->desc->init() != SUCCESS)
    {
      clam(CLAM_FATAL, "plugin", "'%s' init callback failed",
          p->desc->name);
      return(FAIL);
    }

    p->state = PLUGIN_INITIALIZED;
  }

  return(SUCCESS);
}

// -----------------------------------------------------------------------
// Lifecycle: start all initialized plugins (dependency order, head to tail).
// returns: SUCCESS or FAIL
// -----------------------------------------------------------------------
bool
plugin_start_all(void)
{
  if(!plugin_ready)
    return(FAIL);

  for(plugin_rec_t *p = plugins; p != NULL; p = p->next)
  {
    if(p->state != PLUGIN_INITIALIZED)
      continue;

    clam(CLAM_INFO, "plugin", "starting '%s'", p->desc->name);

    if(p->desc->start != NULL && p->desc->start() != SUCCESS)
    {
      clam(CLAM_FATAL, "plugin", "'%s' start callback failed",
          p->desc->name);
      return(FAIL);
    }

    p->state = PLUGIN_RUNNING;
  }

  return(SUCCESS);
}

// -----------------------------------------------------------------------
// Lifecycle: stop all running plugins (reverse dependency order).
// -----------------------------------------------------------------------
void
plugin_stop_all(void)
{
  if(!plugin_ready || n_plugins == 0)
    return;

  uint32_t count = n_plugins;
  plugin_rec_t **arr = mem_alloc("plugin", "stop_all",
      count * sizeof(plugin_rec_t *));

  uint32_t idx = 0;

  for(plugin_rec_t *p = plugins; p != NULL; p = p->next)
    arr[idx++] = p;

  for(uint32_t i = count; i > 0; i--)
  {
    plugin_rec_t *p = arr[i - 1];

    if(p->state != PLUGIN_RUNNING)
      continue;

    clam(CLAM_INFO, "plugin", "stopping '%s'", p->desc->name);

    if(p->desc->stop != NULL)
      p->desc->stop();

    p->state = PLUGIN_STOPPING;
  }

  mem_free(arr);
}

// -----------------------------------------------------------------------
// Lifecycle: deinit all initialized/stopping plugins (reverse dependency
// order).
// -----------------------------------------------------------------------
void
plugin_deinit_all(void)
{
  if(!plugin_ready || n_plugins == 0)
    return;

  uint32_t count = n_plugins;
  plugin_rec_t **arr = mem_alloc("plugin", "deinit_all",
      count * sizeof(plugin_rec_t *));

  uint32_t idx = 0;

  for(plugin_rec_t *p = plugins; p != NULL; p = p->next)
    arr[idx++] = p;

  for(uint32_t i = count; i > 0; i--)
  {
    plugin_rec_t *p = arr[i - 1];

    if(p->state != PLUGIN_INITIALIZED && p->state != PLUGIN_STOPPING)
      continue;

    clam(CLAM_INFO, "plugin", "deinitializing '%s'", p->desc->name);

    if(p->desc->deinit != NULL)
      p->desc->deinit();

    p->state = PLUGIN_LOADED;
  }

  mem_free(arr);
}

// returns: plugin descriptor, or NULL
// name: plugin name to find
const plugin_desc_t *
plugin_find(const char *name)
{
  if(name == NULL)
    return(NULL);

  plugin_rec_t *rec = find_by_name(name);

  return(rec ? rec->desc : NULL);
}

// returns: descriptor of the plugin providing the feature, or NULL
// feature: feature name to search for
const plugin_desc_t *
plugin_find_feature(const char *feature)
{
  if(feature == NULL)
    return(NULL);

  for(plugin_rec_t *p = plugins; p != NULL; p = p->next)
  {
    for(uint32_t i = 0; i < p->desc->provides_count; i++)
    {
      if(strcmp(p->desc->provides[i].name, feature) == 0)
        return(p->desc);
    }
  }

  return(NULL);
}

// returns: descriptor of matching plugin, or NULL
// type: plugin type to match
// kind: optional kind filter (NULL or "" for any)
const plugin_desc_t *
plugin_find_type(plugin_type_t type, const char *kind)
{
  for(plugin_rec_t *p = plugins; p != NULL; p = p->next)
  {
    if(p->desc->type != type)
      continue;

    if(kind == NULL || kind[0] == '\0' || strcmp(p->desc->kind, kind) == 0)
      return(p->desc);
  }

  return(NULL);
}

// returns: plugin lifecycle state (PLUGIN_UNLOADED if not found)
// name: plugin name to query
plugin_state_t
plugin_get_state(const char *name)
{
  if(name == NULL)
    return(PLUGIN_UNLOADED);

  plugin_rec_t *rec = find_by_name(name);

  return(rec ? rec->state : PLUGIN_UNLOADED);
}

// returns: number of currently loaded plugins
uint32_t
plugin_count(void)
{
  return(n_plugins);
}

// returns: human-readable name of a plugin type
// t: plugin type enum value
const char *
plugin_type_name(plugin_type_t t)
{
  switch(t)
  {
    case PLUGIN_CORE:        return("core");
    case PLUGIN_DB:          return("db");
    case PLUGIN_METHOD:      return("method");
    case PLUGIN_BOT:         return("bot");
    case PLUGIN_SERVICE:     return("service");
    case PLUGIN_MISC:        return("misc");
    case PLUGIN_PERSONALITY: return("personality");
  }

  return("unknown");
}

// returns: human-readable name of a plugin state
// s: plugin state enum value
const char *
plugin_state_name(plugin_state_t s)
{
  switch(s)
  {
    case PLUGIN_DISCOVERED:  return("discovered");
    case PLUGIN_LOADED:      return("loaded");
    case PLUGIN_INITIALIZED: return("initialized");
    case PLUGIN_RUNNING:     return("running");
    case PLUGIN_STOPPING:    return("stopping");
    case PLUGIN_UNLOADED:    return("unloaded");
  }

  return("unknown");
}

// -----------------------------------------------------------------------
// KV schema group helpers
// -----------------------------------------------------------------------

// returns: schema group descriptor, or NULL if not found
// plugin_name: name of the plugin to search
// group_name: schema group name (e.g., "channel")
const plugin_kv_group_t *
plugin_kv_group_find(const char *plugin_name, const char *group_name)
{
  if(plugin_name == NULL || group_name == NULL)
    return(NULL);

  const plugin_desc_t *pd = plugin_find(plugin_name);

  if(pd == NULL || pd->kv_groups == NULL || pd->kv_groups_count == 0)
    return(NULL);

  for(uint32_t i = 0; i < pd->kv_groups_count; i++)
  {
    if(strcmp(pd->kv_groups[i].name, group_name) == 0)
      return(&pd->kv_groups[i]);
  }

  return(NULL);
}

// returns: number of entries successfully registered
// group: schema group to instantiate
// ...: string arguments for the key_prefix format placeholders
uint32_t
plugin_kv_group_register(const plugin_kv_group_t *group, ...)
{
  if(group == NULL || group->schema == NULL || group->schema_count == 0)
    return(0);

  // Build the concrete prefix from the format pattern + varargs.
  char prefix[KV_KEY_SZ];
  va_list args;

  va_start(args, group);
  vsnprintf(prefix, sizeof(prefix), group->key_prefix, args);
  va_end(args);

  uint32_t registered = 0;

  for(uint32_t i = 0; i < group->schema_count; i++)
  {
    const plugin_kv_entry_t *e = &group->schema[i];

    if(e->key == NULL)
      continue;

    char key[KV_KEY_SZ];

    snprintf(key, sizeof(key), "%s%s", prefix, e->key);

    if(kv_register(key, e->type, e->default_val, NULL, NULL) == SUCCESS)
      registered++;
  }

  return(registered);
}

// Iterate all schema groups across all loaded plugins.
// cb: callback invoked for each group
// data: opaque user data passed to callback
void
plugin_kv_group_iterate(plugin_kv_group_iter_cb_t cb, void *data)
{
  if(cb == NULL)
    return;

  for(plugin_rec_t *p = plugins; p != NULL; p = p->next)
  {
    if(p->desc == NULL || p->desc->kv_groups == NULL)
      continue;

    for(uint32_t i = 0; i < p->desc->kv_groups_count; i++)
      cb(p->desc, &p->desc->kv_groups[i], data);
  }
}

// -----------------------------------------------------------------------
// Plugin iteration
// -----------------------------------------------------------------------

// Iterate all loaded plugins.
// cb: callback invoked for each plugin
// data: opaque user data passed to callback
void
plugin_iterate(plugin_iterate_cb_t cb, void *data)
{
  if(cb == NULL)
    return;

  for(plugin_rec_t *p = plugins; p != NULL; p = p->next)
  {
    cb(p->desc->name, p->desc->version, p->path,
        p->desc->type, p->desc->kind, p->state, data);
  }
}

// -----------------------------------------------------------------------
// /show plugin command
// -----------------------------------------------------------------------

#define PLUGIN_SHOW_MAX 64

// Row for the plugin table.
typedef struct
{
  char           name[PLUGIN_NAME_SZ];
  char           version[PLUGIN_VER_SZ];
  char           type[16];
  char           kind[PLUGIN_NAME_SZ];
  char           state[16];
  size_t         mem_bytes;
} plugin_show_row_t;

// Collection state for building the table.
typedef struct
{
  plugin_show_row_t rows[PLUGIN_SHOW_MAX];
  uint32_t          count;
} plugin_show_state_t;

// Memory aggregation state for per-plugin memory lookup.
typedef struct
{
  const char *module;
  size_t      total;
} plugin_mem_match_t;

// mem_iterate callback: sum allocations matching a module name.
static void
plugin_mem_sum_cb(const char *module, const char *name,
    size_t sz, time_t timestamp, void *data)
{
  (void)name;
  (void)timestamp;

  plugin_mem_match_t *m = data;

  if(strcmp(module, m->module) == 0)
    m->total += sz;
}

// plugin_iterate callback: collect one row per loaded plugin.
static void
plugin_show_iter_cb(const char *name, const char *version,
    const char *path, plugin_type_t type, const char *kind,
    plugin_state_t state, void *data)
{
  (void)path;

  plugin_show_state_t *st = data;

  if(st->count >= PLUGIN_SHOW_MAX)
    return;

  plugin_show_row_t *r = &st->rows[st->count];

  strncpy(r->name, name, PLUGIN_NAME_SZ - 1);
  r->name[PLUGIN_NAME_SZ - 1] = '\0';
  strncpy(r->version, version, PLUGIN_VER_SZ - 1);
  r->version[PLUGIN_VER_SZ - 1] = '\0';
  strncpy(r->type, plugin_type_name(type), sizeof(r->type) - 1);
  r->type[sizeof(r->type) - 1] = '\0';
  strncpy(r->kind, kind, PLUGIN_NAME_SZ - 1);
  r->kind[PLUGIN_NAME_SZ - 1] = '\0';
  strncpy(r->state, plugin_state_name(state), sizeof(r->state) - 1);
  r->state[sizeof(r->state) - 1] = '\0';

  // Sum memory allocations whose module matches the plugin kind.
  plugin_mem_match_t mm = { .module = kind[0] != '\0' ? kind : name,
                            .total  = 0 };

  mem_iterate(plugin_mem_sum_cb, &mm);
  r->mem_bytes = mm.total;

  st->count++;
}


// qsort comparator: sort plugin rows by name.
static int
plugin_show_cmp(const void *a, const void *b)
{
  const plugin_show_row_t *ra = a;
  const plugin_show_row_t *rb = b;

  return(strcmp(ra->name, rb->name));
}

// Check if a .so filename corresponds to a loaded plugin by comparing
// against loaded plugin paths.
// returns: true if the .so is already loaded
static bool
plugin_is_loaded_path(const char *path)
{
  for(plugin_rec_t *p = plugins; p != NULL; p = p->next)
  {
    if(strcmp(p->path, path) == 0)
      return(true);
  }

  return(false);
}

// Recursively scan a directory for .so files not currently loaded and
// emit them as "available" rows.
// dir: directory to scan
// ctx: command context for reply
// count: running count of available plugins found
static void
plugin_show_scan_available(const char *dir, const cmd_ctx_t *ctx,
    uint32_t *count)
{
  DIR *d = opendir(dir);

  if(d == NULL)
    return;

  struct dirent *ent;

  while((ent = readdir(d)) != NULL)
  {
    if(ent->d_name[0] == '.')
      continue;

    char path[PLUGIN_PATH_SZ];

    snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

    struct stat st;

    if(stat(path, &st) != 0)
      continue;

    if(S_ISDIR(st.st_mode))
    {
      plugin_show_scan_available(path, ctx, count);
      continue;
    }

    if(!S_ISREG(st.st_mode))
      continue;

    size_t len = strlen(ent->d_name);

    if(len < 4 || strcmp(ent->d_name + len - 3, ".so") != 0)
      continue;

    if(plugin_is_loaded_path(path))
      continue;

    // Temporarily dlopen to read the plugin descriptor.
    char line[256];
    void *handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);

    if(handle != NULL)
    {
      dlerror();
      const plugin_desc_t *desc = dlsym(handle, PLUGIN_ENTRY_SYMBOL);

      if(desc != NULL && dlerror() == NULL)
      {
        snprintf(line, sizeof(line),
            "  %-16s %-8s %-11s %-16s "
            CLR_YELLOW "%-11s" CLR_RESET " %8s",
            desc->name, desc->version,
            plugin_type_name(desc->type), desc->kind,
            "available", "0B");
      }
      else
      {
        // Strip "lib" prefix and ".so" extension for fallback display.
        const char *base = ent->d_name;
        size_t base_len  = len - 3;

        if(strncmp(base, "lib", 3) == 0)
        {
          base     += 3;
          base_len -= 3;
        }

        char display[PLUGIN_NAME_SZ];

        snprintf(display, sizeof(display), "%.*s", (int)base_len, base);
        snprintf(line, sizeof(line),
            "  %-16s %-8s %-11s %-16s "
            CLR_RED "%-11s" CLR_RESET " %8s",
            display, "\xe2\x80\x94", "\xe2\x80\x94", "\xe2\x80\x94",
            "error", "\xe2\x80\x94");
      }

      dlclose(handle);
    }
    else
    {
      // dlopen failed — show filename with error state.
      const char *base = ent->d_name;
      size_t base_len  = len - 3;

      if(strncmp(base, "lib", 3) == 0)
      {
        base     += 3;
        base_len -= 3;
      }

      char display[PLUGIN_NAME_SZ];

      snprintf(display, sizeof(display), "%.*s", (int)base_len, base);
      snprintf(line, sizeof(line),
          "  %-16s %-8s %-11s %-16s "
          CLR_RED "%-11s" CLR_RESET " %8s",
          display, "\xe2\x80\x94", "\xe2\x80\x94", "\xe2\x80\x94",
          "error", "\xe2\x80\x94");
    }

    cmd_reply(ctx, line);
    (*count)++;
  }

  closedir(d);
}

// Argument descriptor for /show plugin.
static const cmd_arg_desc_t ad_show_plugin[] = {
  { "name", CMD_ARG_ALNUM, CMD_ARG_OPTIONAL, PLUGIN_NAME_SZ, NULL },
};

// Recursively scan a directory for a .so whose descriptor name matches.
// Copies the full path into out_path on success.
// returns: true if found
// dir: directory to scan
// name: plugin name to match (case-insensitive)
// out_path: buffer to receive the matched .so path
// out_sz: size of out_path buffer
static bool
plugin_find_so_by_name(const char *dir, const char *name,
    char *out_path, size_t out_sz)
{
  DIR *d = opendir(dir);

  if(d == NULL)
    return(false);

  struct dirent *ent;
  bool found = false;

  while((ent = readdir(d)) != NULL && !found)
  {
    if(ent->d_name[0] == '.')
      continue;

    char path[PLUGIN_PATH_SZ];

    snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

    struct stat st;

    if(stat(path, &st) != 0)
      continue;

    if(S_ISDIR(st.st_mode))
    {
      found = plugin_find_so_by_name(path, name, out_path, out_sz);
      continue;
    }

    if(!S_ISREG(st.st_mode))
      continue;

    size_t len = strlen(ent->d_name);

    if(len < 4 || strcmp(ent->d_name + len - 3, ".so") != 0)
      continue;

    void *handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);

    if(handle == NULL)
      continue;

    dlerror();
    const plugin_desc_t *desc = dlsym(handle, PLUGIN_ENTRY_SYMBOL);

    if(desc != NULL && dlerror() == NULL
        && strcasecmp(desc->name, name) == 0)
    {
      strncpy(out_path, path, out_sz - 1);
      out_path[out_sz - 1] = '\0';
      found = true;
    }

    dlclose(handle);
  }

  closedir(d);
  return(found);
}

// Emit the detail view for a plugin descriptor. When loaded is true,
// the plugin is active in the system; when false, it was temporarily
// opened from disk and runtime fields (memory, state) are unavailable.
static void
plugin_show_detail_emit(const cmd_ctx_t *ctx,
    const plugin_desc_t *pd, bool loaded)
{
  plugin_state_t state = loaded ? plugin_get_state(pd->name)
                                : PLUGIN_DISCOVERED;
  char line[512];

  // Title.
  snprintf(line, sizeof(line),
      CLR_BOLD "%s" CLR_RESET " %s", pd->name, pd->version);
  cmd_reply(ctx, line);

  // State (colorized).
  const char *state_str;
  const char *state_clr;

  if(!loaded)
  {
    state_str = "available";
    state_clr = CLR_YELLOW;
  }
  else
  {
    state_str = plugin_state_name(state);
    state_clr = CLR_RESET;

    if(state == PLUGIN_RUNNING)
      state_clr = CLR_GREEN;
    else if(state == PLUGIN_LOADED || state == PLUGIN_INITIALIZED)
      state_clr = CLR_CYAN;
    else if(state == PLUGIN_STOPPING)
      state_clr = CLR_YELLOW;
  }

  snprintf(line, sizeof(line),
      "  " CLR_GRAY "state:" CLR_RESET "    %s%s" CLR_RESET,
      state_clr, state_str);
  cmd_reply(ctx, line);

  // Type and kind.
  snprintf(line, sizeof(line),
      "  " CLR_GRAY "type:" CLR_RESET "     %s",
      plugin_type_name(pd->type));
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  " CLR_GRAY "kind:" CLR_RESET "     %s",
      pd->kind[0] != '\0' ? pd->kind : "(none)");
  cmd_reply(ctx, line);

  // API version.
  snprintf(line, sizeof(line),
      "  " CLR_GRAY "api:" CLR_RESET "      %u", pd->api_version);
  cmd_reply(ctx, line);

  // Memory usage (only meaningful for loaded plugins).
  if(loaded)
  {
    plugin_mem_match_t mm = {
      .module = pd->kind[0] != '\0' ? pd->kind : pd->name,
      .total  = 0
    };

    mem_iterate(plugin_mem_sum_cb, &mm);

    char mem_str[16];

    util_fmt_bytes(mm.total, mem_str, sizeof(mem_str));
    snprintf(line, sizeof(line),
        "  " CLR_GRAY "memory:" CLR_RESET "   %s", mem_str);
    cmd_reply(ctx, line);
  }

  // Provides.
  if(pd->provides_count > 0)
  {
    cmd_reply(ctx,
        "  " CLR_GRAY "provides:" CLR_RESET);

    for(uint32_t i = 0; i < pd->provides_count; i++)
    {
      snprintf(line, sizeof(line),
          "    " CLR_GREEN "\xe2\x80\xa2" CLR_RESET " %s",
          pd->provides[i].name);
      cmd_reply(ctx, line);
    }
  }

  // Requires.
  if(pd->requires_count > 0)
  {
    cmd_reply(ctx,
        "  " CLR_GRAY "requires:" CLR_RESET);

    for(uint32_t i = 0; i < pd->requires_count; i++)
    {
      // Check if the dependency is satisfied.
      const plugin_desc_t *dep = plugin_find_feature(
          pd->requires[i].name);
      const char *dep_clr = dep != NULL ? CLR_GREEN : CLR_RED;

      snprintf(line, sizeof(line),
          "    %s\xe2\x80\xa2" CLR_RESET " %s",
          dep_clr, pd->requires[i].name);
      cmd_reply(ctx, line);
    }
  }

  // Plugin-level KV schema.
  if(pd->kv_schema_count > 0)
  {
    snprintf(line, sizeof(line),
        "  " CLR_GRAY "config keys:" CLR_RESET " %u",
        pd->kv_schema_count);
    cmd_reply(ctx, line);

    for(uint32_t i = 0; i < pd->kv_schema_count; i++)
    {
      const plugin_kv_entry_t *e = &pd->kv_schema[i];

      snprintf(line, sizeof(line),
          "    %-24s " CLR_CYAN "%s" CLR_RESET " = %s",
          e->key, kv_type_name(e->type),
          (e->default_val && e->default_val[0])
              ? e->default_val : CLR_GRAY "(empty)" CLR_RESET);
      cmd_reply(ctx, line);
    }
  }

  // Instance-level KV schema.
  if(pd->kv_inst_schema_count > 0)
  {
    snprintf(line, sizeof(line),
        "  " CLR_GRAY "instance keys:" CLR_RESET " %u",
        pd->kv_inst_schema_count);
    cmd_reply(ctx, line);

    for(uint32_t i = 0; i < pd->kv_inst_schema_count; i++)
    {
      const plugin_kv_entry_t *e = &pd->kv_inst_schema[i];

      snprintf(line, sizeof(line),
          "    %-24s " CLR_CYAN "%s" CLR_RESET " = %s",
          e->key, kv_type_name(e->type),
          (e->default_val && e->default_val[0])
              ? e->default_val : CLR_GRAY "(empty)" CLR_RESET);
      cmd_reply(ctx, line);
    }
  }

  // Entity schema groups.
  if(pd->kv_groups_count > 0)
  {
    snprintf(line, sizeof(line),
        "  " CLR_GRAY "schema groups:" CLR_RESET " %u",
        pd->kv_groups_count);
    cmd_reply(ctx, line);

    for(uint32_t i = 0; i < pd->kv_groups_count; i++)
    {
      const plugin_kv_group_t *g = &pd->kv_groups[i];

      snprintf(line, sizeof(line),
          "    " CLR_BOLD "%s" CLR_RESET
          " " CLR_GRAY "(%u keys, cmd: /%s %s)" CLR_RESET
          " \xe2\x80\x94 %s",
          g->name, g->schema_count, pd->name, g->cmd_name,
          g->description);
      cmd_reply(ctx, line);
    }
  }

  // Lifecycle callbacks present.
  char cbs[64] = "";
  size_t cbs_len = 0;

  if(pd->init != NULL)
    cbs_len += (size_t)snprintf(cbs + cbs_len,
        sizeof(cbs) - cbs_len, "init ");
  if(pd->start != NULL)
    cbs_len += (size_t)snprintf(cbs + cbs_len,
        sizeof(cbs) - cbs_len, "start ");
  if(pd->stop != NULL)
    cbs_len += (size_t)snprintf(cbs + cbs_len,
        sizeof(cbs) - cbs_len, "stop ");
  if(pd->deinit != NULL)
    cbs_len += (size_t)snprintf(cbs + cbs_len,
        sizeof(cbs) - cbs_len, "deinit");

  if(cbs_len > 0)
  {
    snprintf(line, sizeof(line),
        "  " CLR_GRAY "lifecycle:" CLR_RESET " %s", cbs);
    cmd_reply(ctx, line);
  }
}

// Recursively scan a directory for a .so whose descriptor name matches
// the given name. If found, emit the detail view and return true.
static bool
plugin_show_find_available(const char *dir, const cmd_ctx_t *ctx,
    const char *name)
{
  DIR *d = opendir(dir);

  if(d == NULL)
    return(false);

  struct dirent *ent;
  bool found = false;

  while((ent = readdir(d)) != NULL && !found)
  {
    if(ent->d_name[0] == '.')
      continue;

    char path[PLUGIN_PATH_SZ];

    snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

    struct stat st;

    if(stat(path, &st) != 0)
      continue;

    if(S_ISDIR(st.st_mode))
    {
      found = plugin_show_find_available(path, ctx, name);
      continue;
    }

    if(!S_ISREG(st.st_mode))
      continue;

    size_t len = strlen(ent->d_name);

    if(len < 4 || strcmp(ent->d_name + len - 3, ".so") != 0)
      continue;

    void *handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);

    if(handle == NULL)
      continue;

    dlerror();
    const plugin_desc_t *desc = dlsym(handle, PLUGIN_ENTRY_SYMBOL);

    if(desc != NULL && dlerror() == NULL
        && strcasecmp(desc->name, name) == 0)
    {
      plugin_show_detail_emit(ctx, desc, false);
      found = true;
    }

    dlclose(handle);
  }

  closedir(d);
  return(found);
}

// Show detailed information for a single named plugin.
static void
plugin_show_detail(const cmd_ctx_t *ctx, const char *name)
{
  // Try loaded plugins first.
  const plugin_desc_t *pd = plugin_find(name);

  if(pd != NULL)
  {
    plugin_show_detail_emit(ctx, pd, true);
    return;
  }

  // Fall back to scanning the plugin directory for unloaded .so files.
  const char *plugin_dir = bconf_get("PLUGIN_PATH");

  if(plugin_dir == NULL)
    plugin_dir = "./plugins";

  if(plugin_show_find_available(plugin_dir, ctx, name))
    return;

  char buf[PLUGIN_NAME_SZ + 32];

  snprintf(buf, sizeof(buf), "unknown plugin: %s", name);
  cmd_reply(ctx, buf);
}

// Command handler for /show plugin.
static void
plugin_cmd_show(const cmd_ctx_t *ctx)
{
  bool show_all = false;

  if(ctx->parsed && ctx->parsed->argc > 0)
  {
    if(strcasecmp(ctx->parsed->argv[0], "all") == 0)
      show_all = true;
    else
    {
      plugin_show_detail(ctx, ctx->parsed->argv[0]);
      return;
    }
  }

  // Collect loaded plugins.
  plugin_show_state_t st;

  memset(&st, 0, sizeof(st));
  plugin_iterate(plugin_show_iter_cb, &st);

  qsort(st.rows, st.count, sizeof(plugin_show_row_t), plugin_show_cmp);

  // Header.
  char hdr[128];

  snprintf(hdr, sizeof(hdr),
      CLR_BOLD "plugins:" CLR_RESET " %u loaded", st.count);
  cmd_reply(ctx, hdr);

  // Table header.
  char line[256];

  snprintf(line, sizeof(line),
      "  " CLR_BOLD "%-16s %-8s %-11s %-16s %-11s %8s" CLR_RESET,
      "NAME", "VERSION", "TYPE", "KIND", "STATE", "MEM");
  cmd_reply(ctx, line);

  // Data rows.
  for(uint32_t i = 0; i < st.count; i++)
  {
    plugin_show_row_t *r = &st.rows[i];
    char mem_str[16];

    util_fmt_bytes(r->mem_bytes, mem_str, sizeof(mem_str));

    // Color the state column based on lifecycle.
    const char *state_clr = CLR_RESET;

    if(strcmp(r->state, "running") == 0)
      state_clr = CLR_GREEN;
    else if(strcmp(r->state, "loaded") == 0
        || strcmp(r->state, "initialized") == 0)
      state_clr = CLR_CYAN;
    else if(strcmp(r->state, "stopping") == 0)
      state_clr = CLR_YELLOW;

    snprintf(line, sizeof(line),
        "  %-16s %-8s %-11s %-16s %s%-11s" CLR_RESET " %8s",
        r->name, r->version, r->type, r->kind,
        state_clr, r->state, mem_str);

    cmd_reply(ctx, line);
  }

  // Show available (unloaded) .so files when "all" is specified.
  if(show_all)
  {
    const char *plugin_dir = bconf_get("PLUGIN_PATH");

    if(plugin_dir == NULL)
      plugin_dir = "./plugins";

    uint32_t avail = 0;

    plugin_show_scan_available(plugin_dir, ctx, &avail);

    if(avail == 0 && st.count == 0)
      cmd_reply(ctx, "  (none)");
  }
  else if(st.count == 0)
  {
    cmd_reply(ctx, "  (none)");
  }
}

// -----------------------------------------------------------------------
// /plugin command — load/unload plugins at runtime
// -----------------------------------------------------------------------

// Argument descriptor for /plugin load and /plugin unload.
static const cmd_arg_desc_t ad_plugin_cmd_name[] = {
  { "name", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, PLUGIN_NAME_SZ, NULL },
};

// /plugin parent handler: display usage when no subcommand given.
static void
plugin_cmd_plugin(const cmd_ctx_t *ctx)
{
  (void)ctx;
  cmd_reply(ctx, "usage: /plugin <subcommand> ...");
  cmd_reply(ctx, "  load <name>    — load a plugin from disk");
  cmd_reply(ctx, "  unload <name>  — unload a running plugin");
}

// State for collecting bot names during iteration.
#define PLUGIN_BOT_COLLECT_MAX 32

typedef struct
{
  char names[PLUGIN_BOT_COLLECT_MAX][BOT_NAME_SZ];
  uint32_t count;
} plugin_bot_collect_t;

// bot_iterate callback: collect bot names.
static void
plugin_collect_bot_cb(const char *name, const char *driver_name,
    bot_state_t state, uint32_t method_count, uint32_t session_count,
    const char *userns_name, void *data)
{
  (void)driver_name;
  (void)state;
  (void)method_count;
  (void)session_count;
  (void)userns_name;

  plugin_bot_collect_t *c = data;

  if(c->count < PLUGIN_BOT_COLLECT_MAX)
  {
    strncpy(c->names[c->count], name, BOT_NAME_SZ - 1);
    c->names[c->count][BOT_NAME_SZ - 1] = '\0';
    c->count++;
  }
}

// Re-enable user commands on all active bot instances.
// Called after a plugin is loaded at runtime so that any commands
// the plugin registered during init become available.
static void
plugin_enable_bot_commands(void)
{
  // Collect bot names first (bot_iterate holds bot_mutex, so we
  // cannot call bot_find or cmd_enable_all inside the callback).
  plugin_bot_collect_t bots;

  memset(&bots, 0, sizeof(bots));
  bot_iterate(plugin_collect_bot_cb, &bots);

  for(uint32_t i = 0; i < bots.count; i++)
  {
    bot_inst_t *inst = bot_find(bots.names[i]);

    if(inst != NULL)
      cmd_enable_all(inst);
  }
}

// /plugin load <name> — find, load, resolve, init, and start a plugin.
static void
plugin_cmd_load(const cmd_ctx_t *ctx)
{
  const char *name = ctx->parsed->argv[0];
  char buf[PLUGIN_NAME_SZ + 128];

  // Already loaded?
  if(plugin_find(name) != NULL)
  {
    snprintf(buf, sizeof(buf), "plugin " CLR_BOLD "%s" CLR_RESET
        " is already loaded", name);
    cmd_reply(ctx, buf);
    return;
  }

  // Locate the .so file.
  const char *plugin_dir = bconf_get("PLUGIN_PATH");

  if(plugin_dir == NULL)
    plugin_dir = "./plugins";

  char path[PLUGIN_PATH_SZ];

  if(!plugin_find_so_by_name(plugin_dir, name, path, sizeof(path)))
  {
    snprintf(buf, sizeof(buf), "plugin " CLR_BOLD "%s" CLR_RESET
        " not found in %s", name, plugin_dir);
    cmd_reply(ctx, buf);
    return;
  }

  // Load.
  if(plugin_load(path) != SUCCESS)
  {
    snprintf(buf, sizeof(buf), "failed to load " CLR_BOLD "%s" CLR_RESET
        " from %s", name, path);
    cmd_reply(ctx, buf);
    return;
  }

  // Resolve dependencies.
  if(plugin_resolve() != SUCCESS)
  {
    snprintf(buf, sizeof(buf), CLR_RED "dependency resolution failed"
        CLR_RESET " for " CLR_BOLD "%s" CLR_RESET
        "; unloading", name);
    cmd_reply(ctx, buf);
    plugin_unload(name);
    return;
  }

  // Initialize (only processes PLUGIN_LOADED plugins).
  if(plugin_init_all() != SUCCESS)
  {
    snprintf(buf, sizeof(buf), CLR_RED "initialization failed" CLR_RESET
        " for " CLR_BOLD "%s" CLR_RESET "; unloading", name);
    cmd_reply(ctx, buf);
    plugin_unload(name);
    return;
  }

  // Start (only processes PLUGIN_INITIALIZED plugins).
  if(plugin_start_all() != SUCCESS)
  {
    snprintf(buf, sizeof(buf), CLR_RED "start failed" CLR_RESET
        " for " CLR_BOLD "%s" CLR_RESET "; unloading", name);
    cmd_reply(ctx, buf);
    plugin_unload(name);
    return;
  }

  // Re-enable user commands on all active bot instances.
  // The newly loaded plugin may have registered commands via
  // cmd_register() during init; those need bindings on each bot.
  plugin_enable_bot_commands();

  snprintf(buf, sizeof(buf), CLR_GREEN "loaded" CLR_RESET " "
      CLR_BOLD "%s" CLR_RESET, name);
  cmd_reply(ctx, buf);
}

// /plugin unload <name> — stop, deinit, and unload a plugin.
static void
plugin_cmd_unload(const cmd_ctx_t *ctx)
{
  const char *name = ctx->parsed->argv[0];
  char buf[PLUGIN_NAME_SZ * 2 + 128];

  // Check if loaded.
  const plugin_desc_t *pd = plugin_find(name);

  if(pd == NULL)
  {
    snprintf(buf, sizeof(buf), "plugin " CLR_BOLD "%s" CLR_RESET
        " is not loaded", name);
    cmd_reply(ctx, buf);
    return;
  }

  // Pre-check dependencies to give a user-friendly error message.
  // Iterate all loaded plugins and check if any require a feature
  // that this plugin provides.
  for(plugin_rec_t *q = plugins; q != NULL; q = q->next)
  {
    if(strcmp(q->desc->name, pd->name) == 0)
      continue;

    for(uint32_t r = 0; r < q->desc->requires_count; r++)
    {
      for(uint32_t p = 0; p < pd->provides_count; p++)
      {
        if(strcmp(q->desc->requires[r].name,
            pd->provides[p].name) == 0)
        {
          snprintf(buf, sizeof(buf),
              "cannot unload " CLR_BOLD "%s" CLR_RESET
              ": " CLR_BOLD "%s" CLR_RESET
              " depends on feature " CLR_CYAN "%s" CLR_RESET,
              name, q->desc->name, pd->provides[p].name);
          cmd_reply(ctx, buf);
          return;
        }
      }
    }
  }

  // Unload (handles stop, deinit, dlclose).
  if(plugin_unload(name) != SUCCESS)
  {
    snprintf(buf, sizeof(buf), CLR_RED "failed to unload" CLR_RESET " "
        CLR_BOLD "%s" CLR_RESET, name);
    cmd_reply(ctx, buf);
    return;
  }

  snprintf(buf, sizeof(buf), CLR_GREEN "unloaded" CLR_RESET " "
      CLR_BOLD "%s" CLR_RESET, name);
  cmd_reply(ctx, buf);
}

// -----------------------------------------------------------------------
// KV configuration: autoload list
// -----------------------------------------------------------------------

// KV change callback (no-op; autoload is only consulted at startup).
static void
plugin_autoload_changed(const char *key, void *data)
{
  (void)key;
  (void)data;
}

// Register plugin KV keys. Called after kv_init(); the pending list
// mechanism ensures DB values are applied even if called after kv_load().
void
plugin_register_config(void)
{
  kv_register("core.plugin.autoload", KV_STR, "",
      plugin_autoload_changed, NULL);
}

// Load plugins listed in core.plugin.autoload that are not already loaded.
// returns: number of additionally loaded plugins
// plugin_dir: directory to scan for .so files
uint32_t
plugin_load_autoload(const char *plugin_dir)
{
  const char *list = kv_get_str("core.plugin.autoload");

  if(list == NULL || list[0] == '\0')
    return(0);

  // Work on a mutable copy for tokenization.
  char buf[KV_STR_SZ];

  strncpy(buf, list, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  uint32_t loaded = 0;
  char *saveptr = NULL;

  for(char *tok = strtok_r(buf, ",", &saveptr); tok != NULL;
      tok = strtok_r(NULL, ",", &saveptr))
  {
    // Trim leading whitespace.
    while(*tok == ' ' || *tok == '\t')
      tok++;

    // Trim trailing whitespace.
    char *end = tok + strlen(tok) - 1;

    while(end > tok && (*end == ' ' || *end == '\t'))
      *end-- = '\0';

    if(tok[0] == '\0')
      continue;

    // Skip if already loaded.
    if(plugin_find(tok) != NULL)
      continue;

    char path[PLUGIN_PATH_SZ];

    if(!plugin_find_so_by_name(plugin_dir, tok, path, sizeof(path)))
    {
      clam(CLAM_WARN, "plugin", "autoload: '%s' not found in %s",
          tok, plugin_dir);
      continue;
    }

    if(plugin_load(path) == SUCCESS)
      loaded++;
    else
      clam(CLAM_WARN, "plugin", "autoload: failed to load '%s'", tok);
  }

  return(loaded);
}

// -----------------------------------------------------------------------
// Command registration
// -----------------------------------------------------------------------

// Register plugin commands. Must be called after admin_init().
void
plugin_register_commands(void)
{
  // /show plugin — read-only subcommand of /show.
  cmd_register_system("plugin", "plugin",
      "show plugin [all | <name>]",
      "List installed plugins or show plugin details",
      "Shows loaded plugins with type, kind, state, and approximate\n"
      "memory usage. Memory is estimated by matching each plugin's\n"
      "kind against tracked allocation module names.\n\n"
      "Use /show plugin all to also list available .so files in\n"
      "the plugin directory that are not currently loaded.\n\n"
      "Use /show plugin <name> to show detailed information about\n"
      "a specific plugin including features, config keys, schema\n"
      "groups, and lifecycle callbacks.",
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
      plugin_cmd_show, NULL, "show", "plug", ad_show_plugin, 1);

  // /plugin — root command for plugin management.
  cmd_register_system("plugin", "plugin",
      "plugin <subcommand> ...",
      "Manage plugins",
      NULL,
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
      plugin_cmd_plugin, NULL, NULL, "plug", NULL, 0);

  cmd_register_system("plugin", "load",
      "plugin load <name>",
      "Load a plugin",
      "Loads a plugin by name from the plugin directory. The .so\n"
      "file is located by matching the embedded plugin name against\n"
      "the requested name. After loading, the plugin is resolved,\n"
      "initialized, and started automatically.",
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
      plugin_cmd_load, NULL, "plugin", NULL, ad_plugin_cmd_name, 1);

  cmd_register_system("plugin", "unload",
      "plugin unload <name>",
      "Unload a plugin",
      "Unloads a plugin by name. The plugin is stopped, deinitialized,\n"
      "and removed from memory. Fails if another loaded plugin depends\n"
      "on a feature this plugin provides.",
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
      plugin_cmd_unload, NULL, "plugin", NULL, ad_plugin_cmd_name, 1);
}

void
plugin_init(void)
{
  plugins      = NULL;
  n_plugins    = 0;
  plugin_ready = true;
  clam(CLAM_DEBUG, "plugin", "subsystem initialized");
}

void
plugin_exit(void)
{
  if(!plugin_ready)
    return;

  // Lifecycle teardown: stop running plugins, deinit initialized ones.
  plugin_stop_all();
  plugin_deinit_all();

  // Dlclose in reverse dependency order (dependents before providers).
  if(n_plugins > 0)
  {
    uint32_t count = n_plugins;
    plugin_rec_t **arr = mem_alloc("plugin", "exit",
        count * sizeof(plugin_rec_t *));

    uint32_t idx = 0;

    for(plugin_rec_t *p = plugins; p != NULL; p = p->next)
      arr[idx++] = p;

    for(uint32_t i = count; i > 0; i--)
    {
      clam(CLAM_DEBUG, "plugin", "unloading '%s'",
          arr[i - 1]->desc->name);
      dlclose(arr[i - 1]->handle);
      mem_free(arr[i - 1]);
    }

    mem_free(arr);
  }

  plugins      = NULL;
  n_plugins    = 0;
  plugin_ready = false;
  clam(CLAM_INFO, "plugin", "subsystem shut down");
}
