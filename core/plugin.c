// botmanager — MIT
// Plugin loader: dlopen, descriptor registration, lifecycle dispatch.
#define _GNU_SOURCE  // dladdr / Dl_info — GNU extensions; must precede any header
#define PLUGIN_INTERNAL
#include "plugin.h"
#include "util.h"

// Forward decl — defined alongside plugin_dlsym_cached, called from
// plugin_unload to invalidate dangling shim pointers before dlclose.
static void dlsym_cache_on_plugin_unload(const plugin_rec_t *target);

// Forward decl — defined alongside plugin_audit, which shares its
// mapping lookup. Called from plugin_unload between deinit and dlclose.
static uint32_t plugin_reclaim(const plugin_rec_t *target);

// Forward decls — both share plugin_audit's mapping lookup and
// iterators, and both are called from plugin_unload after the Class-A
// sweep, before dlclose.
static bool plugin_quiesce(const plugin_rec_t *target, uint32_t timeout_ms,
    char *offender, size_t offender_cap);
static void audit_emit_clam(const char *line, void *data);
static void plugin_unmap_broadcast(uintptr_t lo, uintptr_t hi);

static plugin_rec_t *
find_by_name(const char *name)
{
  for(plugin_rec_t *p = plugins; p != NULL; p = p->next)
    if(strcmp(p->desc->name, name) == 0)
      return(p);

  return(NULL);
}

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

// True when a bot is currently bound to the bot_driver_t this plugin
// exports. Bot bindings are runtime state, absent from the
// .provides/.requires graph, so no dependency check sees them — and
// after dlclose inst->driver would dangle into freed .text.
static bool
plugin_driver_bound(const plugin_desc_t *desc, char *bot_name, size_t sz,
    bot_state_t *state)
{
  const bot_driver_t *drv;

  if(desc->ext == NULL
      || (desc->type != PLUGIN_METHOD && desc->type != PLUGIN_FEATURE))
    return(false);

  drv = (const bot_driver_t *)desc->ext;

  if(drv->name == NULL)
    return(false);

  return(bot_find_bound_to_driver(drv->name, bot_name, sz, state));
}

static bool
plugin_provides(const plugin_desc_t *desc, const char *feature)
{
  for(uint32_t i = 0; i < desc->provides_count; i++)
    if(strcmp(desc->provides[i].name, feature) == 0)
      return(true);

  return(false);
}

// Public API

bool
plugin_load(const char *path)
{
  void                *handle;
  const plugin_desc_t *desc;
  const char          *err;
  plugin_rec_t        *rec;

  if(path == NULL || !plugin_ready)
    return(FAIL);

  // dlopen the shared library.
  handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);

  if(handle == NULL)
  {
    clam(CLAM_WARN, "plugin", "dlopen '%s': %s", path, dlerror());
    return(FAIL);
  }

  // Resolve the entry point symbol.
  dlerror();  // clear any prior error
  desc = dlsym(handle, PLUGIN_ENTRY_SYMBOL);
  err = dlerror();

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
  rec = mem_alloc("plugin", "record", sizeof(plugin_rec_t));

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

bool
plugin_unload(const char *name, plugin_unload_report_t *report)
{
  plugin_rec_t  *target;
  plugin_rec_t **pp;

  if(report != NULL)
    memset(report, 0, sizeof(*report));

  if(name == NULL || !plugin_ready)
    return(FAIL);

  target = find_by_name(name);

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

      if(plugin_provides(target->desc, need))
      {
        clam(CLAM_WARN, "plugin", "cannot unload '%s': '%s' depends "
            "on feature '%s'", name, q->desc->name, need);
        return(FAIL);
      }
    }
  }

  // Refuse if a bot is bound to this plugin's driver vtable.
  {
    char        bot_name[BOT_NAME_SZ];
    bot_state_t bot_state;

    if(plugin_driver_bound(target->desc, bot_name, sizeof(bot_name),
        &bot_state))
    {
      clam(CLAM_WARN, "plugin",
          "cannot unload '%s': bot '%s' is bound to its driver "
          "(state=%s); stop and destroy the bot first",
          name, bot_name, bot_state_name(bot_state));
      return(FAIL);
    }
  }

  // Lifecycle teardown based on current state.
  if(target->state == PLUGIN_RUNNING)
  {
    clam(CLAM_DEBUG, "plugin", "stopping '%s' before unload", name);

    // stop() is where a plugin names the Class-B holding core cannot
    // define away: a reader thread that would not join, a sweep still
    // running, a driver vtable someone is still bound to. FAIL there
    // means "unmapping me now is a crash", and it is the plugin's to
    // say — so the unload ends here, with the plugin left running and
    // intact. The residual audit further down catches what a plugin
    // forgot to refuse; this catches what it knew.
    if(target->desc->stop != NULL && target->desc->stop() != SUCCESS)
    {
      clam(CLAM_WARN, "plugin",
          "cannot unload '%s': its stop() refused — see the plugin's own "
          "warning for what is still live", name);
      return(FAIL);
    }

    target->state = PLUGIN_STOPPING;
  }

  if(target->state == PLUGIN_INITIALIZED || target->state == PLUGIN_STOPPING)
  {
    clam(CLAM_DEBUG, "plugin", "deinitializing '%s' before unload", name);

    if(target->desc->deinit != NULL)
      target->desc->deinit();

    target->state = PLUGIN_LOADED;
  }

  // Invalidate cross-plugin dlsym shim caches. Consumers (chat,
  // strategies, …) cache resolved function pointers in static fn_t
  // slots; without invalidation, dlclose'ing this plugin would dangle
  // every consumer's cache. Slots belonging to this plugin (i.e. the
  // consumer is the one being unloaded) are dropped from the registry
  // entirely — their backing memory is about to go away.
  dlsym_cache_on_plugin_unload(target);

  // The plugin has had its opportunity (stop, then deinit). Core now
  // reclaims what it can define centrally — every Class-A registration
  // still naming this mapping — and audits what is left. Both run AFTER
  // deinit() and BEFORE dlclose, and both are about this mapping alone.
  // The same audit against a *running* plugin reports its live surface:
  // that is the worklist (`/plugin audit <name>`), this is the verdict.
  {
    uint32_t reclaimed = plugin_reclaim(target);
    char     offender[PLUGIN_OFFENDER_SZ];
    uint32_t residual;

    if(reclaimed > 0)
      clam(CLAM_WARN, "plugin_audit",
          "'%s': deinit() left %u registration(s); core reclaimed them",
          name, reclaimed);

    else
      clam(CLAM_INFO, "plugin_audit", "'%s': deinit() complete", name);

    // Class B is nobody's default: a running task, an in-flight request
    // or a bound vtable cannot be cancelled centrally without inventing
    // a policy. Waiting for one to end, however, is always correct — so
    // give the transient half its budget before judging what is left.
    plugin_quiesce(target, (uint32_t)kv_get_int(KV_UNLOAD_QUIESCE_MS),
        offender, sizeof(offender));

    residual = plugin_audit(name, NULL, NULL);

    if(report != NULL)
    {
      report->reclaimed = reclaimed;
      report->residual  = residual;
      snprintf(report->offender, sizeof(report->offender), "%s", offender);
    }

    // Refusing here leaves the plugin stopped, deinitialized and still
    // mapped — a zombie: nothing of it works, and nothing dangles. That
    // is strictly better than the SIGSEGV dlclose would hand us, and it
    // is loud enough that nobody mistakes it for a clean unload.
    if(residual > 0)
    {
      clam(CLAM_FATAL, "plugin",
          "refusing to dlclose '%s': %u reference(s) core cannot reclaim "
          "still point into its mapping%s%s; the plugin is now "
          "DEINITIALIZED but still mapped (zombie) — fix its stop() and "
          "restart the daemon", name, residual,
          offender[0] != '\0' ? " — " : "", offender);

      plugin_audit(name, audit_emit_clam, NULL);

      if(report != NULL)
        report->zombie = true;

      // Not LOADED: a LOADED plugin is one init_all() would pick up,
      // and this one has already run its deinit(). Reviving it would
      // re-register a surface over state that is gone.
      target->state = PLUGIN_ZOMBIE;
      return(FAIL);
    }
  }

  // Remove from list.
  pp = &plugins;

  while(*pp != NULL)
  {
    if(*pp == target)
    {
      *pp = target->next;
      clam(CLAM_INFO, "plugin", "unloading '%s'", name);

      if(target->handle != NULL)
        dlclose(target->handle);  // synthetic providers have no handle

      mem_free(target);
      n_plugins--;
      return(SUCCESS);
    }

    pp = &(*pp)->next;
  }

  return(FAIL);  // unreachable
}

// Every loaded plugin that transitively requires a feature `target`
// provides, `target` itself included, in dependency order — which is
// simply list order, since plugin_resolve() keeps `plugins` sorted
// providers-first. Name and path are copied out because the records do
// not survive the unloads this closure exists to drive.
// returns: closure size, or 0 when it exceeds `cap` (a warning is logged).
static uint32_t
plugin_closure(const plugin_rec_t *target, plugin_snap_t *out, uint32_t cap)
{
  plugin_rec_t **arr;
  bool          *in;
  uint32_t       count = n_plugins;
  uint32_t       idx   = 0;
  uint32_t       n     = 0;
  bool           progress = true;

  arr = mem_alloc("plugin", "closure", count * sizeof(plugin_rec_t *));
  in  = mem_alloc("plugin", "closure_in", count * sizeof(bool));

  for(plugin_rec_t *p = plugins; p != NULL; p = p->next, idx++)
  {
    arr[idx] = p;
    in[idx]  = (p == target);
  }

  // Fixpoint rather than a graph walk: a plugin joins as soon as it
  // requires a feature any member provides, and membership only grows.
  // At this size the passes are cheaper than the bookkeeping a worklist
  // would need.
  while(progress)
  {
    progress = false;

    for(uint32_t i = 0; i < count; i++)
    {
      const plugin_desc_t *desc = arr[i]->desc;

      if(in[i])
        continue;

      for(uint32_t r = 0; r < desc->requires_count && !in[i]; r++)
      {
        for(uint32_t j = 0; j < count; j++)
        {
          if(!in[j] || !plugin_provides(arr[j]->desc, desc->requires[r].name))
            continue;

          in[i]    = true;
          progress = true;
          break;
        }
      }
    }
  }

  for(uint32_t i = 0; i < count; i++)
  {
    if(!in[i])
      continue;

    if(n < cap)
    {
      snprintf(out[n].name, sizeof(out[n].name), "%s", arr[i]->desc->name);
      snprintf(out[n].path, sizeof(out[n].path), "%s", arr[i]->path);
    }

    n++;
  }

  mem_free(in);
  mem_free(arr);

  if(n > cap)
  {
    clam(CLAM_WARN, "plugin", "reload: '%s' would cycle %u plugins, over "
        "the closure cap of %u", target->desc->name, n, cap);
    return(0);
  }

  return(n);
}

// Bring a snapshot range back up: load each .so in forward order, then
// run the single resolve/init/start pass the loader owes them. Shared
// by the rollback and the reload proper — coming back is the same job
// either way. `failed`, when given, keeps the first name that would not
// load; it must arrive empty.
// returns: how many of the range loaded.
static uint32_t
plugin_restore(const plugin_snap_t *snap, uint32_t from, uint32_t to,
    char *failed, size_t failed_sz)
{
  uint32_t loaded = 0;

  for(uint32_t i = from; i < to; i++)
  {
    if(plugin_load(snap[i].path) == SUCCESS)
    {
      loaded++;
      continue;
    }

    clam(CLAM_WARN, "plugin", "reload: '%s' would not load back from %s",
        snap[i].name, snap[i].path);

    if(failed != NULL && failed[0] == '\0')
      snprintf(failed, failed_sz, "%s", snap[i].name);
  }

  if(loaded == 0)
    return(0);

  if(plugin_resolve() != SUCCESS)
  {
    clam(CLAM_FATAL, "plugin", "reload: dependency resolution failed after "
        "reloading %u plugin(s)", loaded);
    return(loaded);
  }

  if(plugin_init_all() != SUCCESS || plugin_start_all() != SUCCESS)
    clam(CLAM_FATAL, "plugin", "reload: %u plugin(s) reloaded but did not "
        "come back up", loaded);

  return(loaded);
}

// A bot's two pointers into plugin mappings, and what a reload owes
// each. A bot driver is detached and put back in place — the bot keeps
// its name, its sessions and its protocol connections and merely goes
// deaf for the length of the cycle. A protocol driver cannot be handled
// that gently: its connection lives in the mapping, so the bot goes
// down with it and comes back up on the far side.
// returns: bots affected.
static uint32_t
plugin_detach_bots(const plugin_desc_t *desc)
{
  if(desc->ext == NULL)
    return(0);

  if(desc->type == PLUGIN_METHOD || desc->type == PLUGIN_FEATURE)
  {
    const bot_driver_t *drv = (const bot_driver_t *)desc->ext;

    return(drv->name != NULL ? bot_suspend_driver(drv->name) : 0);
  }

  if(desc->type == PLUGIN_PROTOCOL)
    return(bot_suspend_method(desc->kind));

  return(0);
}

static uint32_t
plugin_reattach_bots(const plugin_desc_t *desc)
{
  if(desc->ext == NULL)
    return(0);

  if(desc->type == PLUGIN_METHOD || desc->type == PLUGIN_FEATURE)
    return(bot_resume_driver((const bot_driver_t *)desc->ext, desc->kind));

  if(desc->type == PLUGIN_PROTOCOL)
    return(bot_resume_method(desc->kind));

  return(0);
}

// Put the closure back together, in dependency order: core hands each
// plugin its bots back, then the plugin restores whatever it wrote down
// in suspend(). `skip` names a plugin to leave alone — the one that
// refused, on the rollback path. Plugins that did not come back are
// silently skipped; there is nothing to resume them into.
// returns: bots rebound.
static uint32_t
plugin_resume_range(const plugin_snap_t *snap, uint32_t from, uint32_t to,
    const char *skip)
{
  uint32_t rebound = 0;

  for(uint32_t i = from; i < to; i++)
  {
    const plugin_rec_t *rec;

    if(skip != NULL && strcmp(snap[i].name, skip) == 0)
      continue;

    rec = find_by_name(snap[i].name);

    if(rec == NULL)
      continue;

    rebound += plugin_reattach_bots(rec->desc);

    if(rec->desc->resume != NULL && rec->desc->resume() != SUCCESS)
      clam(CLAM_WARN, "plugin", "'%s' came back but its resume() refused — "
          "it is running with state the reload did not restore",
          snap[i].name);
  }

  return(rebound);
}

// The mirror image, deepest dependent first: each plugin lets go of what
// it holds, then core takes away the bots it holds on the plugin's
// behalf. Nothing has been unloaded yet, so a hook that refuses costs
// only the suspends already done — which are put straight back.
static bool
plugin_suspend_range(const plugin_snap_t *snap, uint32_t n, const char *name,
    plugin_reload_report_t *report)
{
  for(uint32_t i = n; i > 0; i--)
  {
    const plugin_rec_t *rec = find_by_name(snap[i - 1].name);

    if(rec == NULL)
      continue;

    if(rec->desc->suspend != NULL && rec->desc->suspend() != SUCCESS)
    {
      clam(CLAM_WARN, "plugin", "cannot reload '%s': '%s' refused to "
          "suspend — see its own warning for what it could not put down",
          name, snap[i - 1].name);

      if(report != NULL)
      {
        snprintf(report->failed, sizeof(report->failed), "%s",
            snap[i - 1].name);
        snprintf(report->detail, sizeof(report->detail), "%s",
            "its suspend() refused");
      }

      plugin_resume_range(snap, i, n, NULL);
      return(FAIL);
    }

    plugin_detach_bots(rec->desc);
  }

  return(SUCCESS);
}

bool
plugin_reload(const char *name, plugin_reload_report_t *report)
{
  plugin_rec_t  *target;
  plugin_snap_t *snap;
  uint32_t       n;
  uint32_t       cycled;
  uint32_t       rebound;
  char           failed[PLUGIN_NAME_SZ] = "";

  if(report != NULL)
    memset(report, 0, sizeof(*report));

  if(name == NULL || !plugin_ready)
    return(FAIL);

  target = find_by_name(name);

  if(target == NULL)
  {
    clam(CLAM_WARN, "plugin", "reload: '%s' not found", name);
    return(FAIL);
  }

  // Synthetic core providers are records without a .so behind them:
  // there is nothing to dlclose and nothing to load back.
  if(target->handle == NULL)
  {
    clam(CLAM_WARN, "plugin", "reload: '%s' is a core provider, not a "
        "loadable plugin", name);
    return(FAIL);
  }

  snap = mem_alloc("plugin", "reload",
      PLUGIN_RELOAD_MAX_CLOSURE * sizeof(plugin_snap_t));

  n = plugin_closure(target, snap, PLUGIN_RELOAD_MAX_CLOSURE);

  if(n == 0)
  {
    mem_free(snap);
    return(FAIL);
  }

  // Refuse before touching anything if the cascade cannot be completed:
  // a plugin with no recorded path could not be loaded back. A bot bound
  // to a driver in the closure used to refuse here too; it is rebound
  // now, below.
  for(uint32_t i = 0; i < n; i++)
  {
    if(snap[i].path[0] == '\0')
    {
      clam(CLAM_WARN, "plugin", "cannot reload '%s': '%s' has no recorded "
          "path to load back from", name, snap[i].name);

      if(report != NULL)
        snprintf(report->failed, sizeof(report->failed), "%s", snap[i].name);

      mem_free(snap);
      return(FAIL);
    }
  }

  // Everything the closure holds that will not survive the unload —
  // plugin-side state via suspend(), bot bindings via core — comes off
  // now, while a refusal still costs nothing.
  if(plugin_suspend_range(snap, n, name, report) != SUCCESS)
  {
    mem_free(snap);
    return(FAIL);
  }

  clam(CLAM_INFO, "plugin", "reloading '%s' (%u dependent(s) to cycle)",
      name, n - 1);

  // Past this point a failure costs a teardown, so the report stops
  // being able to say "nothing was touched".
  if(report != NULL)
    report->started = true;

  // Down in reverse dependency order, so nothing is unmapped while
  // something that links against it is still running.
  for(uint32_t i = n; i > 0; i--)
  {
    plugin_unload_report_t urep;

    if(plugin_unload(snap[i - 1].name, &urep) == SUCCESS)
      continue;

    // A refusal is no reason to leave the tree half down: put back
    // everything already taken down and report the one that said no.
    clam(CLAM_WARN, "plugin", "reload of '%s' aborted: '%s' refused to "
        "unload; restoring %u plugin(s) already taken down",
        name, snap[i - 1].name, n - i);

    if(report != NULL)
    {
      report->rolled_back = true;
      report->zombie      = urep.zombie;
      report->dependents  = n - 1;
      snprintf(report->failed, sizeof(report->failed), "%s",
          snap[i - 1].name);
      snprintf(report->detail, sizeof(report->detail), "%s", urep.offender);
    }

    plugin_restore(snap, i, n, NULL, 0);

    // The whole closure was suspended up front, so the resume covers
    // both what just came back and what never went down. The one that
    // refused is skipped: a zombie has nothing to resume into, and a
    // plugin left running and intact was never taken apart.
    {
      uint32_t rebound = plugin_resume_range(snap, 0, n, snap[i - 1].name);

      if(report != NULL)
        report->rebound = rebound;
    }

    mem_free(snap);
    return(FAIL);
  }

  cycled  = plugin_restore(snap, 0, n, failed, sizeof(failed));
  rebound = plugin_resume_range(snap, 0, n, NULL);

  if(report != NULL)
  {
    report->dependents = n - 1;
    report->cycled     = cycled;
    report->rebound    = rebound;
    snprintf(report->failed, sizeof(report->failed), "%s", failed);
  }

  mem_free(snap);

  if(cycled < n)
    return(FAIL);

  if(rebound > 0)
    clam(CLAM_INFO, "plugin", "reloaded '%s' (%u dependent(s) cycled, "
        "%u bot(s) rebound)", name, n - 1, rebound);

  else
    clam(CLAM_INFO, "plugin", "reloaded '%s' (%u dependent(s) cycled)",
        name, n - 1);

  return(SUCCESS);
}

uint32_t
plugin_discover(const char *dir)
{
  DIR           *d;
  uint32_t       loaded = 0;
  struct dirent *ent;

  if(dir == NULL || !plugin_ready)
    return(0);

  d = opendir(dir);

  if(d == NULL)
  {
    clam(CLAM_WARN, "plugin", "cannot open directory '%s': %s",
        dir, strerror(errno));
    return(0);
  }

  while((ent = readdir(d)) != NULL)
  {
    char        path[PLUGIN_PATH_SZ];
    struct stat st;
    size_t      len;

    if(ent->d_name[0] == '.')
      continue;

    snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

    // Recurse into subdirectories.
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
    len = strlen(ent->d_name);

    if(len < 4 || strcmp(ent->d_name + len - 3, ".so") != 0)
      continue;

    if(plugin_load(path) == SUCCESS)
      loaded++;
  }

  closedir(d);
  return(loaded);
}

static bool
resolve_feature_in(const char *feature, plugin_rec_t **arr,
    const uint32_t *indices, uint32_t n)
{
  for(uint32_t j = 0; j < n; j++)
  {
    const plugin_desc_t *pd;

    pd = arr[indices[j]]->desc;

    for(uint32_t k = 0; k < pd->provides_count; k++)
      if(strcmp(pd->provides[k].name, feature) == 0)
        return(true);
  }

  return(false);
}

static bool
resolve_feature_exists(const char *feature, plugin_rec_t **arr,
    uint32_t count, uint32_t skip_idx)
{
  for(uint32_t j = 0; j < count; j++)
  {
    const plugin_desc_t *pd;

    if(j == skip_idx)
      continue;

    pd = arr[j]->desc;

    for(uint32_t k = 0; k < pd->provides_count; k++)
      if(strcmp(pd->provides[k].name, feature) == 0)
        return(true);
  }

  return(false);
}

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

static void
resolve_report_failures(plugin_rec_t **arr, const bool *placed,
    uint32_t count)
{
  bool all_exist = true;

  for(uint32_t i = 0; i < count; i++)
  {
    const plugin_desc_t *desc;

    if(placed[i])
      continue;

    desc = arr[i]->desc;

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

bool
plugin_resolve(void)
{
  uint32_t        count;
  plugin_rec_t  **arr;
  uint32_t        idx = 0;
  bool           *placed;
  uint32_t       *order;
  uint32_t        n_placed = 0;
  bool            progress = true;
  bool            result;

  if(!plugin_ready)
    return(FAIL);

  if(n_plugins == 0)
    return(SUCCESS);

  count = n_plugins;

  // Collect all plugin records into an array for sorting.
  arr = mem_alloc("plugin", "resolve",
      count * sizeof(plugin_rec_t *));

  for(plugin_rec_t *p = plugins; p != NULL; p = p->next)
    arr[idx++] = p;

  placed = mem_alloc("plugin", "placed", count * sizeof(bool));
  order  = mem_alloc("plugin", "order", count * sizeof(uint32_t));

  memset(placed, 0, count * sizeof(bool));

  // Kahn's-style: repeatedly place plugins whose requirements are met
  // by already-placed plugins.
  while(progress && n_placed < count)
  {
    progress = false;

    for(uint32_t i = 0; i < count; i++)
    {
      const plugin_desc_t *desc;
      bool                 satisfied = true;

      if(placed[i])
        continue;

      desc = arr[i]->desc;

      for(uint32_t r = 0; r < desc->requires_count && satisfied; r++)
        if(!resolve_feature_in(desc->requires[r].name, arr, order, n_placed))
          satisfied = false;

      if(satisfied)
      {
        order[n_placed++] = i;
        placed[i] = true;
        progress = true;
      }
    }
  }

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

        // The schema entry lives in the plugin's .rodata, so it is both
        // the declaration that owns the key and a valid address inside
        // the mapping core must reclaim it from.
        if(kv_register_owned(e->key, e->type, e->default_val, e->cb, NULL,
            e->help, e) == SUCCESS && e->nl != NULL)
          kv_register_nl(e->key, e->nl);
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

void
plugin_stop_all(void)
{
  uint32_t       count;
  plugin_rec_t **arr;
  uint32_t       idx = 0;

  if(!plugin_ready || n_plugins == 0)
    return;

  count = n_plugins;
  arr = mem_alloc("plugin", "stop_all",
      count * sizeof(plugin_rec_t *));

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

void
plugin_deinit_all(void)
{
  uint32_t       count;
  plugin_rec_t **arr;
  uint32_t       idx = 0;

  if(!plugin_ready || n_plugins == 0)
    return;

  count = n_plugins;
  arr = mem_alloc("plugin", "deinit_all",
      count * sizeof(plugin_rec_t *));

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

const plugin_desc_t *
plugin_find(const char *name)
{
  plugin_rec_t *rec;

  if(name == NULL)
    return(NULL);

  rec = find_by_name(name);

  return(rec ? rec->desc : NULL);
}

const plugin_desc_t *
plugin_find_feature(const char *feature)
{
  if(feature == NULL)
    return(NULL);

  for(plugin_rec_t *p = plugins; p != NULL; p = p->next)
  {
    for(uint32_t i = 0; i < p->desc->provides_count; i++)
      if(strcmp(p->desc->provides[i].name, feature) == 0)
        return(p->desc);
  }

  return(NULL);
}

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

plugin_state_t
plugin_get_state(const char *name)
{
  plugin_rec_t *rec;

  if(name == NULL)
    return(PLUGIN_UNLOADED);

  rec = find_by_name(name);

  return(rec ? rec->state : PLUGIN_UNLOADED);
}

uint32_t
plugin_count(void)
{
  return(n_plugins);
}

// Resolve a symbol out of a named plugin's .so. Plugins are dlopen'd
// with RTLD_LOCAL, so host code cannot rely on link-time reference to
// plugin-resident helpers — the resolution happens here instead.
//
// returns: symbol pointer or NULL (plugin not loaded, or symbol missing)
void *
plugin_dlsym(const char *plugin_name, const char *symbol)
{
  plugin_rec_t *rec;

  if(plugin_name == NULL || symbol == NULL)
    return(NULL);

  rec = find_by_name(plugin_name);

  if(rec == NULL || rec->handle == NULL)
    return(NULL);

  // Clear any pending dlerror so a NULL return is unambiguous.
  dlerror();
  return(dlsym(rec->handle, symbol));
}

// Look up the .so file path containing `addr` via dladdr(3). Returns
// a mem_alloc'd copy of the path on success, NULL on miss. Caller owns
// the allocation. Used at dlsym-cache registration time to remember
// which plugin's text contains a consumer's cache slot.
static const char *
dlsym_cache_consumer_so(void *addr)
{
  Dl_info     info;
  size_t      len;
  char       *out;

  if(addr == NULL)
    return(NULL);

  if(dladdr(addr, &info) == 0 || info.dli_fname == NULL)
    return(NULL);

  len = strlen(info.dli_fname);
  out = mem_alloc("plugin", "dlsym_consumer", len + 1);
  memcpy(out, info.dli_fname, len + 1);
  return(out);
}

void *
plugin_dlsym_cached(const char *plugin_name, const char *symbol,
    void **slot)
{
  void               *resolved;
  dlsym_cache_rec_t  *r;
  bool                already_registered = false;

  if(slot == NULL)
    return(plugin_dlsym(plugin_name, symbol));

  resolved = plugin_dlsym(plugin_name, symbol);

  if(resolved == NULL)
    return(NULL);

  pthread_mutex_lock(&dlsym_cache_mutex);

  for(r = dlsym_cache_head; r != NULL; r = r->next)
  {
    if(r->slot == slot)
    {
      already_registered = true;
      break;
    }
  }

  if(!already_registered)
  {
    r = mem_alloc("plugin", "dlsym_cache_rec", sizeof(*r));
    r->target_plugin = plugin_name;
    r->slot          = slot;
    r->consumer_so   = dlsym_cache_consumer_so((void *)slot);
    r->next          = dlsym_cache_head;
    dlsym_cache_head = r;
  }

  pthread_mutex_unlock(&dlsym_cache_mutex);

  // Caller does the atomic store via the union-laundered fn_t to keep
  // strict aliasing happy; we don't write `*slot` here.
  return(resolved);
}

// Walk the dlsym-cache list during a plugin unload. For each entry:
//   - If the entry's consumer_so matches the unloaded plugin's path,
//     the slot's memory is about to disappear — drop the entry.
//   - Else, if the entry's target_plugin matches the unloaded
//     plugin's name, NULL the slot so the next caller re-resolves
//     (and gets either the new symbol post-reload or a NULL miss
//     that the shim's FATAL/abort path handles).
// Called from plugin_unload with no caller-side locks held.
static void
dlsym_cache_on_plugin_unload(const plugin_rec_t *target)
{
  dlsym_cache_rec_t **pp;
  const char         *target_name;
  const char         *target_path;

  if(target == NULL || target->desc == NULL)
    return;

  target_name = target->desc->name;
  target_path = target->path;

  pthread_mutex_lock(&dlsym_cache_mutex);

  pp = &dlsym_cache_head;

  while(*pp != NULL)
  {
    dlsym_cache_rec_t *r = *pp;
    bool consumer_match = (r->consumer_so != NULL
        && target_path[0] != '\0'
        && strcmp(r->consumer_so, target_path) == 0);

    if(consumer_match)
    {
      *pp = r->next;

      if(r->consumer_so != NULL)
        mem_free((char *)r->consumer_so);

      mem_free(r);
      continue;
    }

    if(strcmp(r->target_plugin, target_name) == 0)
      __atomic_store_n(r->slot, NULL, __ATOMIC_RELEASE);

    pp = &r->next;
  }

  pthread_mutex_unlock(&dlsym_cache_mutex);
}

// Ownership attribution and the teardown audit
//
// A plugin's mapping is the ground truth for "does this pointer die at
// dlclose". We resolve the extent once per audit with dl_iterate_phdr
// (one loader-lock acquisition, taken before any registry lock is held)
// and then range-test every retained pointer with plain arithmetic —
// so the sweeps below never invert the lock order between the loader
// and cmd/kv/clam/bot/method.

typedef struct
{
  uintptr_t     probe;   // in: a pointer known to be inside the object
  plugin_map_t *out;
  bool          found;
} plugin_map_probe_t;

static int
plugin_map_phdr_cb(struct dl_phdr_info *info, size_t size, void *data)
{
  plugin_map_probe_t *p  = data;
  uintptr_t           lo = UINTPTR_MAX;
  uintptr_t           hi = 0;
  const char         *slash;

  (void)size;

  for(uint16_t i = 0; i < info->dlpi_phnum; i++)
  {
    const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
    uintptr_t         seg_lo;
    uintptr_t         seg_hi;

    if(ph->p_type != PT_LOAD)
      continue;

    seg_lo = (uintptr_t)info->dlpi_addr + (uintptr_t)ph->p_vaddr;
    seg_hi = seg_lo + (uintptr_t)ph->p_memsz;

    if(seg_lo < lo) lo = seg_lo;
    if(seg_hi > hi) hi = seg_hi;
  }

  if(hi == 0 || p->probe < lo || p->probe >= hi)
    return(0);  // not this object; keep walking

  p->out->lo = lo;
  p->out->hi = hi;

  slash = strrchr(info->dlpi_name, '/');
  snprintf(p->out->soname, sizeof(p->out->soname), "%s",
      slash != NULL ? slash + 1 : info->dlpi_name);

  p->found = true;
  return(1);  // stop the walk
}

// Resolve the mapping that contains `rec`'s descriptor — the descriptor
// is a const object in the .so, so it is always a valid probe.
static bool
plugin_map_of(const plugin_rec_t *rec, plugin_map_t *out)
{
  plugin_map_probe_t probe;

  if(rec == NULL || rec->handle == NULL || rec->desc == NULL)
    return(FAIL);

  memset(out, 0, sizeof(*out));
  probe.probe = (uintptr_t)rec->desc;
  probe.out   = out;
  probe.found = false;

  dl_iterate_phdr(plugin_map_phdr_cb, &probe);

  return(probe.found ? SUCCESS : FAIL);
}

bool
plugin_owns_ptr(const char *plugin_name, const void *ptr)
{
  const plugin_rec_t *rec;
  plugin_map_t        map;

  if(plugin_name == NULL || ptr == NULL)
    return(false);

  rec = find_by_name(plugin_name);

  if(rec == NULL || plugin_map_of(rec, &map) != SUCCESS)
    return(false);

  return((uintptr_t)ptr >= map.lo && (uintptr_t)ptr < map.hi);
}

void
plugin_unmap_notify_register(plugin_unmap_cb_t cb, void *data)
{
  if(cb == NULL)
    return;

  pthread_mutex_lock(&plugin_unmap_mutex);

  for(uint32_t i = 0; i < n_unmap_listeners; i++)
  {
    if(plugin_unmap_listeners[i].cb != cb)
      continue;

    plugin_unmap_listeners[i].data = data;   // idempotent re-registration
    pthread_mutex_unlock(&plugin_unmap_mutex);
    return;
  }

  if(n_unmap_listeners >= PLUGIN_UNMAP_MAX_LISTENERS)
  {
    pthread_mutex_unlock(&plugin_unmap_mutex);
    clam(CLAM_WARN, "plugin",
        "unmap listener table full (%u); a holder of foreign pointers "
        "will not be told when a mapping goes away",
        (uint32_t)PLUGIN_UNMAP_MAX_LISTENERS);
    return;
  }

  plugin_unmap_listeners[n_unmap_listeners].cb   = cb;
  plugin_unmap_listeners[n_unmap_listeners].data = data;
  n_unmap_listeners++;

  pthread_mutex_unlock(&plugin_unmap_mutex);
}

void
plugin_unmap_notify_unregister(plugin_unmap_cb_t cb)
{
  pthread_mutex_lock(&plugin_unmap_mutex);

  for(uint32_t i = 0; i < n_unmap_listeners; i++)
  {
    if(plugin_unmap_listeners[i].cb != cb)
      continue;

    plugin_unmap_listeners[i] = plugin_unmap_listeners[--n_unmap_listeners];
    break;
  }

  pthread_mutex_unlock(&plugin_unmap_mutex);
}

// Tell every listener that [lo,hi) is going away, then drop the
// listeners that live in it — a plugin that registered one and forgot to
// unregister would otherwise leave core holding a pointer into freed
// .text, which is the very thing this exists to prevent.
static void
plugin_unmap_broadcast(uintptr_t lo, uintptr_t hi)
{
  plugin_unmap_rec_t snap[PLUGIN_UNMAP_MAX_LISTENERS];
  uint32_t           n = 0;

  pthread_mutex_lock(&plugin_unmap_mutex);

  for(uint32_t i = 0; i < n_unmap_listeners; i++)
    snap[n++] = plugin_unmap_listeners[i];

  for(uint32_t i = n_unmap_listeners; i > 0; i--)
  {
    uintptr_t addr = (uintptr_t)fn_addr(&plugin_unmap_listeners[i - 1].cb);

    if(addr < lo || addr >= hi)
      continue;

    plugin_unmap_listeners[i - 1] =
        plugin_unmap_listeners[--n_unmap_listeners];
  }

  pthread_mutex_unlock(&plugin_unmap_mutex);

  // Unlocked: a listener drops pointers under its own lock and must be
  // free to do so without ours.
  for(uint32_t i = 0; i < n; i++)
  {
    uintptr_t addr = (uintptr_t)fn_addr(&snap[i].cb);

    if(addr >= lo && addr < hi)
      continue;   // the listener itself is going away; do not call it

    snap[i].cb(lo, hi, snap[i].data);
  }
}

// Drop every Class-A registration still owned by `target` — the ones
// core can define a correct default for, because dropping a pure
// registry entry is order-independent and cannot be more wrong than
// leaving it pointing at an unmapped object (root TODO.md §PLIFE-3).
// Runs after deinit(), so what it finds is what the plugin did not
// clean up itself.
// returns: number of registrations reclaimed (0 == the plugin tore
// itself down)
static uint32_t
plugin_reclaim(const plugin_rec_t *target)
{
  plugin_map_t map;
  uint32_t     n = 0;

  // Synthetic core providers carry no mapping; nothing to reclaim.
  if(target == NULL || target->handle == NULL)
    return(0);

  if(plugin_map_of(target, &map) != SUCCESS)
  {
    clam(CLAM_WARN, "plugin_reclaim",
        "'%s': cannot resolve its mapping; nothing reclaimed",
        target->desc->name);
    return(0);
  }

  n += cmd_reclaim_owned(map.lo, map.hi);
  n += kv_reclaim_owned(map.lo, map.hi);
  n += clam_reclaim_owned(map.lo, map.hi);
  n += bot_reclaim_contributors_owned(map.lo, map.hi);

  // What core cannot reach, it can at least announce. Listeners drop
  // their own pointers into this range; whatever they do not drop, the
  // audit below still counts against the unload.
  plugin_unmap_broadcast(map.lo, map.hi);

  return(n);
}

typedef struct
{
  plugin_map_t map;
  uint32_t     leaks;                            // exact, unbounded
  uint32_t     n_lines;                          // recorded, capped
  char       (*lines)[PLUGIN_AUDIT_LINE_SZ];
} plugin_audit_ctx_t;

// Range-test one retained pointer and record it if it dies at dlclose.
static void
audit_note(plugin_audit_ctx_t *ctx, const char *registry,
    const char *subject, const char *field, const void *ptr)
{
  uintptr_t addr = (uintptr_t)ptr;

  if(ptr == NULL || addr < ctx->map.lo || addr >= ctx->map.hi)
    return;

  ctx->leaks++;

  if(ctx->n_lines >= PLUGIN_AUDIT_MAX_LINES)
    return;

  snprintf(ctx->lines[ctx->n_lines], PLUGIN_AUDIT_LINE_SZ,
      "  %-6s %-34s %-12s -> %s", registry, subject, field,
      ctx->map.soname);
  ctx->n_lines++;
}

// One thin adapter per registry: the registry names the subject and the
// field, the adapter names the registry.

static void
audit_cmd_cb(const char *subject, const char *field, const void *ptr,
    void *data)
{
  audit_note(data, "cmd", subject, field, ptr);
}

static void
audit_kv_cb(const char *subject, const char *field, const void *ptr,
    void *data)
{
  audit_note(data, "kv", subject, field, ptr);
}

static void
audit_clam_cb(const char *subject, const char *field, const void *ptr,
    void *data)
{
  audit_note(data, "clam", subject, field, ptr);
}

static void
audit_bot_cb(const char *subject, const char *field, const void *ptr,
    void *data)
{
  audit_note(data, "bot", subject, field, ptr);
}

static void
audit_method_cb(const char *subject, const char *field, const void *ptr,
    void *data)
{
  audit_note(data, "method", subject, field, ptr);
}

static void
audit_task_cb(const task_iter_info_t *info, void *data)
{
  audit_note(data, "task", info->name, "cb", fn_addr(&info->cb));
  audit_note(data, "task", info->name, "data", info->data);
}

static void
audit_curl_cb(const curl_iter_req_t *req, void *data)
{
  const char *state = req->in_flight ? "(in-flight)" : "(queued)";

  audit_note(data, "curl", state, "cb",         fn_addr(&req->cb));
  audit_note(data, "curl", state, "cb_data",    req->cb_data);
  audit_note(data, "curl", state, "chunk_cb",   fn_addr(&req->chunk_cb));
  audit_note(data, "curl", state, "chunk_user", req->chunk_user);
}

// NOT swept: the dlsym-shim cache. Its entries retain a consumer's
// static slot and the target-name literal in that consumer's .rodata —
// both inside the consumer's mapping, so they would show up here — but
// dlsym_cache_on_plugin_unload() drops every one of them before
// dlclose, unconditionally. Counting them would put a floor under the
// audit that no plugin's deinit() could clear, and the whole point of
// this number is that a plugin author can drive it to zero.

uint32_t
plugin_audit(const char *plugin_name, plugin_audit_emit_t emit, void *data)
{
  plugin_audit_ctx_t  ctx;
  const plugin_rec_t *rec;
  const db_driver_t  *db_drv;
  uint32_t            leaks;

  if(plugin_name == NULL)
    return(0);

  rec = find_by_name(plugin_name);

  // Synthetic core providers carry no mapping — nothing can dangle.
  if(rec == NULL || rec->handle == NULL)
    return(0);

  memset(&ctx, 0, sizeof(ctx));

  if(plugin_map_of(rec, &ctx.map) != SUCCESS)
  {
    clam(CLAM_WARN, "plugin_audit",
        "'%s': cannot resolve its mapping; audit skipped", plugin_name);
    return(0);
  }

  ctx.lines = mem_alloc("plugin", "audit_lines",
      PLUGIN_AUDIT_MAX_LINES * PLUGIN_AUDIT_LINE_SZ);

  // Every sweep below runs under its own registry's lock and does
  // nothing but arithmetic and snprintf — see each iterator's contract.
  //
  // Order is deliberate: the rare, high-signal registries go first so
  // that a report truncated at PLUGIN_AUDIT_MAX_LINES still shows the
  // live driver binding or the in-flight request, rather than 256 lines
  // of a big plugin's command and KV surface. The count is unaffected.
  db_drv = db_audit_driver();

  if(db_drv != NULL)
    audit_note(&ctx, "db", "driver", "vtable", db_drv);

  bot_audit_iterate_bindings(audit_bot_cb, &ctx);
  bot_audit_iterate_contributors(audit_bot_cb, &ctx);
  method_audit_iterate(audit_method_cb, &ctx);
  clam_audit_iterate(audit_clam_cb, &ctx);
  task_iterate(audit_task_cb, &ctx);
  curl_iterate_active(audit_curl_cb, &ctx);
  cmd_audit_iterate(audit_cmd_cb, &ctx);
  kv_audit_iterate(audit_kv_cb, &ctx);

  if(emit != NULL)
  {
    for(uint32_t i = 0; i < ctx.n_lines; i++)
      emit(ctx.lines[i], data);

    if(ctx.leaks > ctx.n_lines)
    {
      char more[PLUGIN_AUDIT_LINE_SZ];

      snprintf(more, sizeof(more), "  ... and %u more (report capped)",
          ctx.leaks - ctx.n_lines);
      emit(more, data);
    }
  }

  leaks = ctx.leaks;
  mem_free(ctx.lines);
  return(leaks);
}

// Emitter that puts an audit line in the log rather than in a reply —
// used on the refusal path, where the operator is not necessarily the
// one who typed the command.
static void
audit_emit_clam(const char *line, void *data)
{
  (void)data;

  clam(CLAM_WARN, "plugin_audit", "%s", line);
}

// The quiescence barrier.
//
// Class-B references are not core's to cancel — but most of them are
// merely *transient*: a task between two runs of its own callback, a
// request whose response is still on the wire. Nothing is wrong with
// them except that dlclose is one instruction ahead. So core waits, and
// refuses only what is still holding the mapping when the budget runs
// out (root TODO.md §PLIFE-6).

typedef struct
{
  plugin_map_t map;
  uint32_t     holders;                    // this pass only; reset per poll
  char         offender[PLUGIN_OFFENDER_SZ];
} plugin_quiesce_ctx_t;

static bool
quiesce_in_map(const plugin_quiesce_ctx_t *ctx, const void *ptr)
{
  uintptr_t addr = (uintptr_t)ptr;

  return(ptr != NULL && addr >= ctx->map.lo && addr < ctx->map.hi);
}

// The first holder of each pass is the one the refusal names; the rest
// only raise the count. A bare "busy" would leave the operator with
// nothing to fix.
static void
quiesce_hold(plugin_quiesce_ctx_t *ctx, const char *kind,
    const char *subject, const char *state)
{
  ctx->holders++;

  if(ctx->offender[0] != '\0')
    return;

  snprintf(ctx->offender, sizeof(ctx->offender), "%s '%s' (%s)", kind,
      subject != NULL ? subject : "(unnamed)", state);
}

static void
quiesce_task_cb(const task_iter_info_t *info, void *data)
{
  plugin_quiesce_ctx_t *ctx = data;

  if(!quiesce_in_map(ctx, fn_addr(&info->cb))
      && !quiesce_in_map(ctx, info->data))
    return;

  quiesce_hold(ctx, "task", info->name, task_state_name(info->state));
}

static void
quiesce_curl_cb(const curl_iter_req_t *req, void *data)
{
  plugin_quiesce_ctx_t *ctx = data;

  if(!quiesce_in_map(ctx, fn_addr(&req->cb))
      && !quiesce_in_map(ctx, req->cb_data)
      && !quiesce_in_map(ctx, fn_addr(&req->chunk_cb))
      && !quiesce_in_map(ctx, req->chunk_user))
    return;

  quiesce_hold(ctx, "request", req->url,
      req->in_flight ? "in-flight" : "queued");
}

// Blocks the unloading thread — an operator/command thread — for at
// most `timeout_ms`. Never called from a worker: plugin_unload's own
// callers are command handlers and the whenmoon strategy reload.
// returns: SUCCESS when nothing in the queues names the mapping any
// more, FAIL on timeout (caller refuses the unload).
static bool
plugin_quiesce(const plugin_rec_t *target, uint32_t timeout_ms,
    char *offender, size_t offender_cap)
{
  static const struct timespec nap =
      { 0, (long)PLUGIN_QUIESCE_POLL_MS * 1000L * 1000L };

  plugin_quiesce_ctx_t ctx;
  struct timespec      start;

  if(offender != NULL && offender_cap > 0)
    offender[0] = '\0';

  // Synthetic core providers carry no mapping; nothing can be inside it.
  if(target == NULL || target->handle == NULL)
    return(SUCCESS);

  memset(&ctx, 0, sizeof(ctx));

  if(plugin_map_of(target, &ctx.map) != SUCCESS)
  {
    clam(CLAM_WARN, "plugin", "'%s': cannot resolve its mapping; "
        "quiescence unverified", target->desc->name);
    return(SUCCESS);
  }

  clock_gettime(CLOCK_MONOTONIC, &start);

  for(;;)
  {
    ctx.holders     = 0;
    ctx.offender[0] = '\0';

    task_iterate(quiesce_task_cb, &ctx);
    curl_iterate_active(quiesce_curl_cb, &ctx);

    if(ctx.holders == 0)
      return(SUCCESS);

    if(util_ms_since(&start) >= (uint64_t)timeout_ms)
      break;

    nanosleep(&nap, NULL);
  }

  if(offender != NULL && offender_cap > 0)
    snprintf(offender, offender_cap, "%s", ctx.offender);

  clam(CLAM_WARN, "plugin", "'%s': %u Class-B reference(s) still name its "
      "mapping after %u ms; first is %s", target->desc->name, ctx.holders,
      timeout_ms, ctx.offender);

  return(FAIL);
}

void
plugin_get_stats(plugin_stats_t *out)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->loaded = n_plugins;
}

const char *
plugin_type_name(plugin_type_t t)
{
  switch(t)
  {
    case PLUGIN_CORE:        return("core");
    case PLUGIN_DB:          return("db");
    case PLUGIN_PROTOCOL:    return("protocol");
    case PLUGIN_METHOD:      return("method");
    case PLUGIN_SERVICE:     return("service");
    case PLUGIN_MISC:        return("misc");
    case PLUGIN_PERSONALITY: return("personality");
    case PLUGIN_FEATURE:     return("feature");
    case PLUGIN_STRATEGY:    return("strategy");
  }

  return("unknown");
}

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
    case PLUGIN_ZOMBIE:      return("zombie");
  }

  return("unknown");
}

// KV schema group helpers

const plugin_kv_group_t *
plugin_kv_group_find(const char *plugin_name, const char *group_name)
{
  const plugin_desc_t *pd;

  if(plugin_name == NULL || group_name == NULL)
    return(NULL);

  pd = plugin_find(plugin_name);

  if(pd == NULL || pd->kv_groups == NULL || pd->kv_groups_count == 0)
    return(NULL);

  for(uint32_t i = 0; i < pd->kv_groups_count; i++)
    if(strcmp(pd->kv_groups[i].name, group_name) == 0)
      return(&pd->kv_groups[i]);

  return(NULL);
}

uint32_t
plugin_kv_group_register(const plugin_kv_group_t *group, ...)
{
  char     prefix[KV_KEY_SZ];
  va_list  args;
  uint32_t registered = 0;

  if(group == NULL || group->schema == NULL || group->schema_count == 0)
    return(0);

  // Build the concrete prefix from the format pattern + varargs.
  va_start(args, group);
  vsnprintf(prefix, sizeof(prefix), group->key_prefix, args);
  va_end(args);

  for(uint32_t i = 0; i < group->schema_count; i++)
  {
    const plugin_kv_entry_t *e = &group->schema[i];
    char                     key[KV_KEY_SZ];

    if(e->key == NULL)
      continue;

    snprintf(key, sizeof(key), "%s%s", prefix, e->key);

    // Owned by the group declaration, not by this loop — see
    // kv_register_owned().
    if(kv_register_owned(key, e->type, e->default_val, e->cb, NULL,
        e->help, e) == SUCCESS)
      registered++;
  }

  return(registered);
}

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

// Plugin iteration

void
plugin_iterate(plugin_iterate_cb_t cb, void *data)
{
  if(cb == NULL)
    return;

  for(plugin_rec_t *p = plugins; p != NULL; p = p->next)
    cb(p->desc->name, p->desc->version, p->path,
        p->desc->type, p->desc->kind, p->state, data);
}

// /show plugin command

// Upper bound on rows the /show plugin table can hold. Must stay
// comfortably above the number of plugins the daemon loads — when the
// live count exceeds this, plugin_show_iter_cb silently drops the
// overflow (and the "N loaded" header undercounts to match), which is
// how a running-but-invisible plugin happens. Bumped past 64 once the
// tree crossed 65 plugins; keep headroom as the tree grows.
#define PLUGIN_SHOW_MAX 128

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
  plugin_mem_match_t *m = data;

  (void)name;
  (void)timestamp;

  if(strcmp(module, m->module) == 0)
    m->total += sz;
}

// plugin_iterate callback: collect one row per loaded plugin.
static void
plugin_show_iter_cb(const char *name, const char *version,
    const char *path, plugin_type_t type, const char *kind,
    plugin_state_t state, void *data)
{
  plugin_show_state_t *st = data;
  plugin_show_row_t   *r;
  plugin_mem_match_t   mm;

  (void)path;

  if(st->count >= PLUGIN_SHOW_MAX)
    return;

  r = &st->rows[st->count];

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
  mm = (plugin_mem_match_t){ .module = kind[0] != '\0' ? kind : name,
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

static bool
plugin_is_loaded_path(const char *path)
{
  for(plugin_rec_t *p = plugins; p != NULL; p = p->next)
    if(strcmp(p->path, path) == 0)
      return(true);

  return(false);
}

static void
plugin_show_scan_available(const char *dir, const cmd_ctx_t *ctx,
    uint32_t *count)
{
  DIR           *d;
  struct dirent *ent;

  d = opendir(dir);

  if(d == NULL)
    return;

  while((ent = readdir(d)) != NULL)
  {
    char        path[PLUGIN_PATH_SZ];
    struct stat st;
    size_t      len;
    char        line[256];
    void       *handle;

    if(ent->d_name[0] == '.')
      continue;

    snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

    if(stat(path, &st) != 0)
      continue;

    if(S_ISDIR(st.st_mode))
    {
      plugin_show_scan_available(path, ctx, count);
      continue;
    }

    if(!S_ISREG(st.st_mode))
      continue;

    len = strlen(ent->d_name);

    if(len < 4 || strcmp(ent->d_name + len - 3, ".so") != 0)
      continue;

    if(plugin_is_loaded_path(path))
      continue;

    // Temporarily dlopen to read the plugin descriptor.
    handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);

    if(handle != NULL)
    {
      const plugin_desc_t *desc;

      dlerror();
      desc = dlsym(handle, PLUGIN_ENTRY_SYMBOL);

      if(desc != NULL && dlerror() == NULL)
        snprintf(line, sizeof(line),
            "  %-16s %-8s %-11s %-16s "
            CLR_YELLOW "%-11s" CLR_RESET " %8s",
            desc->name, desc->version,
            plugin_type_name(desc->type), desc->kind,
            "available", "0B");

      else
      {
        // Strip "lib" prefix and ".so" extension for fallback display.
        const char *base = ent->d_name;
        size_t      base_len  = len - 3;
        char        display[PLUGIN_NAME_SZ];

        if(strncmp(base, "lib", 3) == 0)
        {
          base     += 3;
          base_len -= 3;
        }

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
      size_t      base_len  = len - 3;
      char        display[PLUGIN_NAME_SZ];

      if(strncmp(base, "lib", 3) == 0)
      {
        base     += 3;
        base_len -= 3;
      }

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
static bool
plugin_find_so_by_name(const char *dir, const char *name,
    char *out_path, size_t out_sz)
{
  DIR           *d;
  struct dirent *ent;
  bool           found = false;

  d = opendir(dir);

  if(d == NULL)
    return(false);

  while((ent = readdir(d)) != NULL && !found)
  {
    char                 path[PLUGIN_PATH_SZ];
    struct stat          st;
    size_t               len;
    void                *handle;
    const plugin_desc_t *desc;

    if(ent->d_name[0] == '.')
      continue;

    snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

    if(stat(path, &st) != 0)
      continue;

    if(S_ISDIR(st.st_mode))
    {
      found = plugin_find_so_by_name(path, name, out_path, out_sz);
      continue;
    }

    if(!S_ISREG(st.st_mode))
      continue;

    len = strlen(ent->d_name);

    if(len < 4 || strcmp(ent->d_name + len - 3, ".so") != 0)
      continue;

    handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);

    if(handle == NULL)
      continue;

    dlerror();
    desc = dlsym(handle, PLUGIN_ENTRY_SYMBOL);

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

// A KV key is display-sensitive if it lives in the `creds` tier or its
// trailing segment names a credential — such values are never printed
// verbatim in /show, only as a set/unset indicator.
static bool
plugin_kv_key_sensitive(const char *key)
{
  const char *dot;
  const char *tail;

  if(kv_is_secret_key(key))
    return(true);

  dot  = strrchr(key, '.');
  tail = (dot != NULL) ? dot + 1 : key;

  return(strstr(tail, "apikey")   != NULL
      || strstr(tail, "api_key")  != NULL
      || strstr(tail, "secret")   != NULL
      || strstr(tail, "token")    != NULL
      || strstr(tail, "password") != NULL
      || strstr(tail, "passwd")   != NULL);
}

// Render a schema key's *live* value for /show plugin (the schema only
// carries the compile-time default, which is misleading once an operator
// has configured the key). Sensitive keys collapse to a set/unset badge
// so a live credential never reaches the reply.
static void
plugin_kv_value_display(const plugin_kv_entry_t *e, char *out, size_t sz)
{
  if(plugin_kv_key_sensitive(e->key))
  {
    const char *v   = kv_get_str(e->key);
    bool        set = v != NULL && v[0] != '\0'
        && v != KV_REDACTED_VALUE && strcmp(v, KV_REDACTED_VALUE) != 0;

    snprintf(out, sz, "%s", set ? CLR_GREEN "(set)" CLR_RESET
                                : CLR_GRAY  "(unset)" CLR_RESET);
    return;
  }

  switch(e->type)
  {
    case KV_STR:
    {
      const char *v = kv_get_str(e->key);

      if(v == NULL || v[0] == '\0')
        snprintf(out, sz, CLR_GRAY "(empty)" CLR_RESET);
      else
        snprintf(out, sz, "%s", v);
      break;
    }

    case KV_BOOL:
      snprintf(out, sz, "%s", kv_get_int(e->key) != 0 ? "true" : "false");
      break;

    case KV_FLOAT:
    case KV_DOUBLE:
    case KV_LDOUBLE:
      snprintf(out, sz, "%g", kv_get_double(e->key));
      break;

    default:  // the integer tiers
      snprintf(out, sz, "%lld", (long long)kv_get_int(e->key));
      break;
  }
}

// Emit the detail view for a plugin descriptor. When loaded is true,
// the plugin is active in the system; when false, it was temporarily
// opened from disk and runtime fields (memory, state) are unavailable.
static void
plugin_show_detail_emit(const cmd_ctx_t *ctx,
    const plugin_desc_t *pd, bool loaded)
{
  plugin_state_t  state = loaded ? plugin_get_state(pd->name)
                                 : PLUGIN_DISCOVERED;
  char            line[512];
  const char     *state_str;
  const char     *state_clr;

  // Title.
  snprintf(line, sizeof(line),
      CLR_BOLD "%s" CLR_RESET " %s", pd->name, pd->version);
  cmd_reply(ctx, line);

  // State (colorized).

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

    {
      char mem_str[16];

      mem_iterate(plugin_mem_sum_cb, &mm);
      util_fmt_bytes(mm.total, mem_str, sizeof(mem_str));
      snprintf(line, sizeof(line),
          "  " CLR_GRAY "memory:" CLR_RESET "   %s", mem_str);
      cmd_reply(ctx, line);
    }
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
      char val[KV_STR_SZ + 32];

      plugin_kv_value_display(e, val, sizeof(val));

      snprintf(line, sizeof(line),
          "    %-24s " CLR_CYAN "%s" CLR_RESET " = %s",
          e->key, kv_type_name(e->type), val);
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
      char val[KV_STR_SZ + 32];

      plugin_kv_value_display(e, val, sizeof(val));

      snprintf(line, sizeof(line),
          "    %-24s " CLR_CYAN "%s" CLR_RESET " = %s",
          e->key, kv_type_name(e->type), val);
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

  {
  // Lifecycle callbacks present.
  char   cbs[64] = "";
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
}

// Recursively scan a directory for a .so whose descriptor name matches
// the given name. If found, emit the detail view and return true.
static bool
plugin_show_find_available(const char *dir, const cmd_ctx_t *ctx,
    const char *name)
{
  DIR           *d;
  struct dirent *ent;
  bool           found = false;

  d = opendir(dir);

  if(d == NULL)
    return(false);

  while((ent = readdir(d)) != NULL && !found)
  {
    char                 path[PLUGIN_PATH_SZ];
    struct stat          st;
    size_t               len;
    void                *handle;
    const plugin_desc_t *desc;

    if(ent->d_name[0] == '.')
      continue;

    snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

    if(stat(path, &st) != 0)
      continue;

    if(S_ISDIR(st.st_mode))
    {
      found = plugin_show_find_available(path, ctx, name);
      continue;
    }

    if(!S_ISREG(st.st_mode))
      continue;

    len = strlen(ent->d_name);

    if(len < 4 || strcmp(ent->d_name + len - 3, ".so") != 0)
      continue;

    handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);

    if(handle == NULL)
      continue;

    dlerror();
    desc = dlsym(handle, PLUGIN_ENTRY_SYMBOL);

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
  const plugin_desc_t *pd;
  const char          *plugin_dir;

  // Try loaded plugins first.
  pd = plugin_find(name);

  if(pd != NULL)
  {
    plugin_show_detail_emit(ctx, pd, true);
    return;
  }

  // Fall back to scanning the plugin directory for unloaded .so files.
  plugin_dir = bconf_get("PLUGIN_PATH");

  if(plugin_dir == NULL)
    plugin_dir = "./plugins";

  if(plugin_show_find_available(plugin_dir, ctx, name))
    return;

  {
    char buf[PLUGIN_NAME_SZ + 32];

    snprintf(buf, sizeof(buf), "unknown plugin: %s", name);
    cmd_reply(ctx, buf);
  }
}

// Command handler for /show plugin.
static void
plugin_cmd_show(const cmd_ctx_t *ctx)
{
  bool                show_all = false;
  plugin_show_state_t st;
  char                hdr[128];
  char                line[256];

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
  memset(&st, 0, sizeof(st));
  plugin_iterate(plugin_show_iter_cb, &st);

  qsort(st.rows, st.count, sizeof(plugin_show_row_t), plugin_show_cmp);

  // Header.
  snprintf(hdr, sizeof(hdr),
      CLR_BOLD "plugins:" CLR_RESET " %u loaded", st.count);
  cmd_reply(ctx, hdr);

  // Table header.
  snprintf(line, sizeof(line),
      "  " CLR_BOLD "%-16s %-8s %-11s %-16s %-11s %8s" CLR_RESET,
      "NAME", "VERSION", "TYPE", "KIND", "STATE", "MEM");
  cmd_reply(ctx, line);

  // Data rows.
  for(uint32_t i = 0; i < st.count; i++)
  {
    plugin_show_row_t *r = &st.rows[i];
    char               mem_str[16];
    const char        *state_clr = CLR_RESET;

    util_fmt_bytes(r->mem_bytes, mem_str, sizeof(mem_str));

    // Color the state column based on lifecycle.

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
    const char *plugin_dir;
    uint32_t    avail = 0;

    plugin_dir = bconf_get("PLUGIN_PATH");

    if(plugin_dir == NULL)
      plugin_dir = "./plugins";

    plugin_show_scan_available(plugin_dir, ctx, &avail);

    if(avail == 0 && st.count == 0)
      cmd_reply(ctx, "  (none)");
  }

  else if(st.count == 0)
    cmd_reply(ctx, "  (none)");
}

// /plugin command — load/unload plugins at runtime

// Argument descriptor for /plugin load and /plugin unload.
static const cmd_arg_desc_t ad_plugin_cmd_name[] = {
  { "name", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, PLUGIN_NAME_SZ, NULL },
};

// /plugin parent handler: display usage when no subcommand given.
static void
plugin_cmd_plugin(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /plugin <subcommand> ...");
}

// /plugin load <name> — find, load, resolve, init, and start a plugin.
static void
plugin_cmd_load(const cmd_ctx_t *ctx)
{
  const char *name = ctx->parsed->argv[0];
  char        buf[PLUGIN_NAME_SZ + PLUGIN_PATH_SZ + 64];
  const char *plugin_dir;
  char        path[PLUGIN_PATH_SZ];

  // Already loaded?
  if(plugin_find(name) != NULL)
  {
    snprintf(buf, sizeof(buf), "plugin " CLR_BOLD "%s" CLR_RESET
        " is already loaded", name);
    cmd_reply(ctx, buf);
    return;
  }

  // Locate the .so file.
  plugin_dir = bconf_get("PLUGIN_PATH");

  if(plugin_dir == NULL)
    plugin_dir = "./plugins";

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
    plugin_unload(name, NULL);
    return;
  }

  // Initialize (only processes PLUGIN_LOADED plugins).
  if(plugin_init_all() != SUCCESS)
  {
    snprintf(buf, sizeof(buf), CLR_RED "initialization failed" CLR_RESET
        " for " CLR_BOLD "%s" CLR_RESET "; unloading", name);
    cmd_reply(ctx, buf);
    plugin_unload(name, NULL);
    return;
  }

  // Start (only processes PLUGIN_INITIALIZED plugins).
  if(plugin_start_all() != SUCCESS)
  {
    snprintf(buf, sizeof(buf), CLR_RED "start failed" CLR_RESET
        " for " CLR_BOLD "%s" CLR_RESET "; unloading", name);
    cmd_reply(ctx, buf);
    plugin_unload(name, NULL);
    return;
  }

  snprintf(buf, sizeof(buf), CLR_GREEN "loaded" CLR_RESET " "
      CLR_BOLD "%s" CLR_RESET, name);
  cmd_reply(ctx, buf);
}

// /plugin audit — report registrations that would dangle after dlclose

static void
plugin_audit_reply(const char *line, void *data)
{
  cmd_reply((const cmd_ctx_t *)data, line);
}

static void
plugin_audit_all_cb(const char *name, const char *version, const char *path,
    plugin_type_t type, const char *kind, plugin_state_t state, void *data)
{
  const cmd_ctx_t *ctx = data;
  uint32_t         leaks;
  char             line[PLUGIN_NAME_SZ + 96];

  (void)version; (void)path; (void)type; (void)kind; (void)state;

  leaks = plugin_audit(name, NULL, NULL);

  snprintf(line, sizeof(line), "  %-24s %s%u" CLR_RESET " leaked "
      "reference(s)", name, leaks > 0 ? CLR_YELLOW : CLR_GREEN, leaks);
  cmd_reply(ctx, line);
}

// /plugin audit <name> | all — report-only; never refuses anything.
static void
plugin_cmd_audit(const cmd_ctx_t *ctx)
{
  const char *name = ctx->parsed->argv[0];
  uint32_t    leaks;
  char        buf[PLUGIN_NAME_SZ + 96];

  if(strcmp(name, "all") == 0)
  {
    cmd_reply(ctx, CLR_BOLD "plugin teardown audit:" CLR_RESET);
    plugin_iterate(plugin_audit_all_cb, (void *)ctx);
    return;
  }

  if(plugin_find(name) == NULL)
  {
    snprintf(buf, sizeof(buf), "plugin " CLR_BOLD "%s" CLR_RESET
        " is not loaded", name);
    cmd_reply(ctx, buf);
    return;
  }

  leaks = plugin_audit(name, plugin_audit_reply, (void *)ctx);

  if(leaks == 0)
  {
    snprintf(buf, sizeof(buf), "%s: " CLR_GREEN "clean" CLR_RESET
        " — no live references into its mapping", name);
    cmd_reply(ctx, buf);
    return;
  }

  snprintf(buf, sizeof(buf), "%s: " CLR_YELLOW "%u" CLR_RESET
      " leaked reference(s) — listed above", name, leaks);
  cmd_reply(ctx, buf);
}

// /plugin unload <name> — stop, deinit, and unload a plugin.
static void
plugin_cmd_unload(const cmd_ctx_t *ctx)
{
  const char            *name = ctx->parsed->argv[0];
  plugin_unload_report_t report;
  char                   buf[PLUGIN_NAME_SZ * 2 + PLUGIN_OFFENDER_SZ + 384];

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

  // Pre-check bot bindings. Same test as plugin_unload — mirrored here
  // for a friendly user-facing error before the loader fires.
  {
    char        bot_name[BOT_NAME_SZ];
    bot_state_t bot_state;

    if(plugin_driver_bound(pd, bot_name, sizeof(bot_name), &bot_state))
    {
      snprintf(buf, sizeof(buf),
          "cannot unload " CLR_BOLD "%s" CLR_RESET
          ": bot " CLR_BOLD "%s" CLR_RESET
          " is bound to its driver (state=%s); stop and destroy it first",
          name, bot_name, bot_state_name(bot_state));
      cmd_reply(ctx, buf);
      return;
    }
  }

  // Unload (handles stop, deinit, the Class-A reclamation, the residual
  // audit, and dlclose).
  if(plugin_unload(name, &report) != SUCCESS)
  {
    // Two different failures, and the difference is the whole story: a
    // plugin that refused up front is untouched, one refused after its
    // teardown is a zombie the operator must restart out of.
    if(report.zombie)
      snprintf(buf, sizeof(buf), CLR_RED "refused to unload" CLR_RESET " "
          CLR_BOLD "%s" CLR_RESET " — " CLR_YELLOW "%u reference(s)"
          CLR_RESET " core cannot reclaim still point into its mapping%s%s"
          ". It is now stopped and deinitialized but still mapped "
          "(nothing dangles, nothing works); restart the daemon. The log "
          "lists every one.", name, report.residual,
          report.offender[0] != '\0' ? ": " : "", report.offender);

    else
      snprintf(buf, sizeof(buf), CLR_RED "failed to unload" CLR_RESET " "
          CLR_BOLD "%s" CLR_RESET " — it is still running and intact; the "
          "log names what it is still holding", name);

    cmd_reply(ctx, buf);
    return;
  }

  if(report.reclaimed > 0)
  {
    snprintf(buf, sizeof(buf), CLR_GREEN "unloaded" CLR_RESET " "
        CLR_BOLD "%s" CLR_RESET " — its deinit() left " CLR_YELLOW "%u"
        CLR_RESET " registration(s), reclaimed by core",
        name, report.reclaimed);
    cmd_reply(ctx, buf);
    return;
  }

  snprintf(buf, sizeof(buf), CLR_GREEN "unloaded" CLR_RESET " "
      CLR_BOLD "%s" CLR_RESET " (teardown clean)", name);
  cmd_reply(ctx, buf);
}

// /plugin reload <name> — cycle a plugin and everything that requires it.
static void
plugin_cmd_reload(const cmd_ctx_t *ctx)
{
  const char            *name = ctx->parsed->argv[0];
  plugin_reload_report_t report;
  char                   buf[PLUGIN_NAME_SZ * 2 + PLUGIN_OFFENDER_SZ + 384];

  if(plugin_find(name) == NULL)
  {
    snprintf(buf, sizeof(buf), "plugin " CLR_BOLD "%s" CLR_RESET
        " is not loaded", name);
    cmd_reply(ctx, buf);
    return;
  }

  if(plugin_reload(name, &report) == SUCCESS)
  {
    char bots[96] = "";

    if(report.rebound > 0)
      snprintf(bots, sizeof(bots), ", " CLR_CYAN "%u" CLR_RESET
          " bot%s rebound", report.rebound,
          report.rebound == 1 ? "" : "s");

    if(report.dependents == 0)
      snprintf(buf, sizeof(buf), CLR_GREEN "reloaded" CLR_RESET " "
          CLR_BOLD "%s" CLR_RESET "%s%s%s", name,
          bots[0] != '\0' ? " (" : "",
          bots[0] != '\0' ? bots + 2 : "",
          bots[0] != '\0' ? ")" : "");

    else
      snprintf(buf, sizeof(buf), CLR_GREEN "reloaded" CLR_RESET " "
          CLR_BOLD "%s" CLR_RESET " (" CLR_CYAN "%u" CLR_RESET
          " dependent%s cycled%s)", name, report.dependents,
          report.dependents == 1 ? "" : "s", bots);

    cmd_reply(ctx, buf);
    return;
  }

  // Three distinguishable failures, and the operator needs the
  // difference: nothing happened, nothing happened but a plugin was
  // taken down and put back, or the tree is down a plugin.
  if(report.rolled_back && report.zombie)
    snprintf(buf, sizeof(buf), CLR_RED "reload aborted" CLR_RESET " for "
        CLR_BOLD "%s" CLR_RESET " — " CLR_BOLD "%s" CLR_RESET " could not "
        "be unmapped%s%s and is now a " CLR_YELLOW "zombie" CLR_RESET
        ": stopped, deinitialized, still mapped. Everything else was put "
        "back, but that one needs a daemon restart. The log lists every "
        "reference.", name, report.failed,
        report.detail[0] != '\0' ? " — " : "", report.detail);

  else if(report.rolled_back)
    snprintf(buf, sizeof(buf), CLR_RED "reload aborted" CLR_RESET " for "
        CLR_BOLD "%s" CLR_RESET " — " CLR_BOLD "%s" CLR_RESET " refused to "
        "unload%s%s and is still running. Everything already taken down "
        "was restored; the system is as it was.", name, report.failed,
        report.detail[0] != '\0' ? ": " : "", report.detail);

  else if(report.started)
    snprintf(buf, sizeof(buf), CLR_RED "reload of" CLR_RESET " "
        CLR_BOLD "%s" CLR_RESET " " CLR_RED "incomplete" CLR_RESET
        " — %u of %u plugin(s) came back; " CLR_BOLD "%s" CLR_RESET
        " did not%s%s. The log names what failed.",
        name, report.cycled, report.dependents + 1, report.failed,
        report.detail[0] != '\0' ? ": " : "", report.detail);

  else if(report.failed[0] != '\0')
    snprintf(buf, sizeof(buf), CLR_RED "cannot reload" CLR_RESET " "
        CLR_BOLD "%s" CLR_RESET " — " CLR_BOLD "%s" CLR_RESET
        " blocks the cascade%s%s. Nothing was touched.",
        name, report.failed,
        report.detail[0] != '\0' ? ": " : "", report.detail);

  else
    snprintf(buf, sizeof(buf), CLR_RED "cannot reload" CLR_RESET " "
        CLR_BOLD "%s" CLR_RESET " — nothing was touched; the log says why",
        name);

  cmd_reply(ctx, buf);
}

// KV configuration: autoload list

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
      plugin_autoload_changed, NULL,
      "Comma-separated list of plugins to load automatically at startup");

  kv_register(KV_UNLOAD_QUIESCE_MS, KV_UINT32, "5000", NULL, NULL,
      "How long an unload waits for a plugin's tasks and requests to "
      "finish before refusing to dlclose it (milliseconds)");
}

uint32_t
plugin_load_autoload(const char *plugin_dir)
{
  const char *list = kv_get_str("core.plugin.autoload");
  char        buf[KV_STR_SZ];
  uint32_t    loaded = 0;
  char       *saveptr = NULL;

  if(list == NULL || list[0] == '\0')
    return(0);

  // Work on a mutable copy for tokenization.
  strncpy(buf, list, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  for(char *tok = strtok_r(buf, ",", &saveptr); tok != NULL;
      tok = strtok_r(NULL, ",", &saveptr))
  {
    char *end;
    char  path[PLUGIN_PATH_SZ];

    // Trim leading whitespace.
    while(*tok == ' ' || *tok == '\t')
      tok++;

    // Trim trailing whitespace.
    end = tok + strlen(tok) - 1;

    while(end > tok && (*end == ' ' || *end == '\t'))
      *end-- = '\0';

    if(tok[0] == '\0')
      continue;

    // Skip if already loaded.
    if(plugin_find(tok) != NULL)
      continue;

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

// Command registration

// Register plugin commands. Must be called after admin_init().
void
plugin_register_commands(void)
{
  // /show plugin — read-only subcommand of /show.
  cmd_register("plugin", "plugin",
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
      plugin_cmd_show, NULL, "show", "plug", ad_show_plugin, 1, NULL, NULL);

  // /plugin — root command for plugin management.
  cmd_register("plugin", "plugin",
      "plugin <subcommand> ...",
      "Manage plugins",
      NULL,
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
      plugin_cmd_plugin, NULL, NULL, "plug", NULL, 0, NULL, NULL);

  cmd_register("plugin", "load",
      "plugin load <name>",
      "Load a plugin",
      "Loads a plugin by name from the plugin directory. The .so\n"
      "file is located by matching the embedded plugin name against\n"
      "the requested name. After loading, the plugin is resolved,\n"
      "initialized, and started automatically.",
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
      plugin_cmd_load, NULL, "plugin", NULL, ad_plugin_cmd_name, 1, NULL, NULL);

  cmd_register("plugin", "unload",
      "plugin unload <name>",
      "Unload a plugin",
      "Unloads a plugin by name. The plugin is stopped, deinitialized,\n"
      "and removed from memory. Fails if another loaded plugin depends\n"
      "on a feature this plugin provides.",
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
      plugin_cmd_unload, NULL, "plugin", NULL, ad_plugin_cmd_name, 1, NULL, NULL);

  cmd_register("plugin", "reload",
      "plugin reload <name>",
      "Reload a plugin and everything that requires it",
      "Unloads and loads a plugin back from the same .so, cycling\n"
      "every loaded plugin that transitively requires a feature it\n"
      "provides — a strategy cannot outlive the whenmoon it links\n"
      "against, so unloading one alone is refused and always was.\n\n"
      "Dependents come down in reverse dependency order and go back\n"
      "up in forward order. If any of them refuses to unload, the\n"
      "ones already taken down are loaded back and nothing is\n"
      "reloaded: a partial cascade is worse than none.\n\n"
      "Refused up front if a bot is bound to a driver in the closure\n"
      "(destroy the bot first) or the plugin is a synthetic core\n"
      "provider. KV rows survive the cycle — the persisted row is the\n"
      "durable value and registration re-reads it.",
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
      plugin_cmd_reload, NULL, "plugin", NULL, ad_plugin_cmd_name, 1, NULL, NULL);

  cmd_register("plugin", "audit",
      "plugin audit <name> | all",
      "Report registrations that would dangle after unload",
      "Sweeps every registry that retains a pointer — commands, KV,\n"
      "clam subscribers, bot KV contributors and driver bindings,\n"
      "method drivers, tasks, in-flight curl requests, the dlsym\n"
      "shim cache, and the DB driver — and reports each one that\n"
      "points into the named plugin's mapping. Those are exactly the\n"
      "references that would dangle once the plugin is dlclose'd, so\n"
      "a non-zero count is a bug in that plugin's deinit().\n\n"
      "Attribution is by the object's load address, not by any name\n"
      "the plugin registers under.\n\n"
      "Report-only: this command never unloads or refuses anything.\n"
      "Use /plugin audit all for a one-line summary per plugin.",
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
      plugin_cmd_audit, NULL, "plugin", NULL, ad_plugin_cmd_name, 1, NULL, NULL);
}

// Synthetic core providers.
//
// Core subsystems are not plugins, but plugins can declare
// `.requires = { "core_<feature>" }` for self-documentation and so
// that a future minimal build stripping a core service fails to
// resolve its dependents.  We insert one synthetic record per
// stable core feature before `plugin_discover()` runs.  Synthetic
// records carry no dlopen handle, no lifecycle callbacks, and sit
// at PLUGIN_RUNNING so `plugin_init_all()` / `plugin_start_all()`
// skip them.
static const char *const BM_CORE_PROVIDERS[] =
{
  "core_alloc",     "core_bot",       "core_botmanctl", "core_clam",
  "core_cmd",       "core_curl",      "core_db",        "core_kv",
  "core_method",    "core_plugin",    "core_pool",      "core_resolve",
  "core_sig",       "core_sock",      "core_task",      "core_userns",
  "core_util",      "core_json",      "core_sse",
};

#define BM_CORE_PROVIDER_COUNT                                    \
    (sizeof(BM_CORE_PROVIDERS) / sizeof(BM_CORE_PROVIDERS[0]))

static plugin_desc_t bm_core_descs[BM_CORE_PROVIDER_COUNT];

static bool
plugin_register_synthetic(uint32_t slot, const char *feature_name)
{
  plugin_desc_t *d;
  plugin_rec_t  *rec;

  if(slot >= BM_CORE_PROVIDER_COUNT)
    return(FAIL);

  d = &bm_core_descs[slot];

  memset(d, 0, sizeof(*d));
  d->api_version = PLUGIN_API_VERSION;
  d->type        = PLUGIN_CORE;
  snprintf(d->name,    sizeof(d->name),    "%s", feature_name);
  snprintf(d->version, sizeof(d->version), "core");
  snprintf(d->kind,    sizeof(d->kind),    "%s", feature_name);
  snprintf(d->provides[0].name,
      sizeof(d->provides[0].name), "%s", feature_name);
  d->provides_count = 1;
  d->requires_count = 0;

  rec = mem_alloc("plugin", "synthetic",
      sizeof(plugin_rec_t));

  memset(rec, 0, sizeof(*rec));
  rec->desc   = d;
  rec->handle = NULL;         // synthetic: no .so backing
  rec->state  = PLUGIN_RUNNING; // synthetic: always "running"

  rec->next = plugins;
  plugins   = rec;
  n_plugins++;
  return(SUCCESS);
}

static void
plugin_register_core_providers(void)
{
  for(uint32_t i = 0; i < BM_CORE_PROVIDER_COUNT; i++)
    if(plugin_register_synthetic(i, BM_CORE_PROVIDERS[i]) != SUCCESS)
      clam(CLAM_WARN, "plugin",
          "failed to register synthetic provider '%s'",
          BM_CORE_PROVIDERS[i]);

  clam(CLAM_DEBUG, "plugin", "registered %u synthetic core providers",
      (uint32_t)BM_CORE_PROVIDER_COUNT);
}

void
plugin_init(void)
{
  plugins      = NULL;
  n_plugins    = 0;
  plugin_ready = true;

  // Core-provided features must exist before `plugin_discover()`
  // so that discovered plugins can resolve `.requires = "core_*"`.
  plugin_register_core_providers();

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
      plugin_rec_t *r = arr[i - 1];

      clam(CLAM_DEBUG, "plugin", "unloading '%s'", r->desc->name);

      if(r->handle != NULL)
        dlclose(r->handle);  // synthetic providers have no handle

      mem_free(r);
    }

    mem_free(arr);
  }

  plugins      = NULL;
  n_plugins    = 0;
  plugin_ready = false;
  clam(CLAM_INFO, "plugin", "subsystem shut down");
}
