// botmanager — MIT
// User-command registration, parsing, and dispatch to registered handlers.
#define CMD_INTERNAL
#include "cmd.h"

// Command set freelist management.

static cmd_set_t *
set_get(void)
{
  cmd_set_t *s;

  if(cmd_set_freelist != NULL)
  {
    s = cmd_set_freelist;
    cmd_set_freelist = s->next;
    cmd_set_free_count--;
    memset(s, 0, sizeof(*s));
  }

  else
  {
    s = mem_alloc("cmd", "set", sizeof(*s));
    memset(s, 0, sizeof(*s));
  }

  strlcpy(s->prefix, "!", CMD_PREFIX_SZ);
  return(s);
}

static void
set_put(cmd_set_t *s)
{
  s->next = cmd_set_freelist;
  cmd_set_freelist = s;
  cmd_set_free_count++;
}

// Internal: find or create the cmd_set for a bot instance.
// Caller must hold cmd_mutex.

// Find the cmd_set for an instance. Returns NULL if none exists.
static cmd_set_t *
set_find_locked(const bot_inst_t *inst)
{
  for(cmd_set_t *s = cmd_sets; s != NULL; s = s->next)
    if(s->inst == inst)
      return(s);

  return(NULL);
}

// Find or create the cmd_set for an instance.
static cmd_set_t *
set_ensure_locked(const bot_inst_t *inst)
{
  cmd_set_t *s = set_find_locked(inst);

  if(s != NULL)
    return(s);

  s = set_get();
  s->inst = inst;
  s->next = cmd_sets;
  cmd_sets = s;
  return(s);
}

// Internal: find a command definition by name or abbreviation.
// Exact name match takes priority, then abbreviation match.
// Caller must hold cmd_mutex.
static cmd_def_t *
def_find_locked(const char *name)
{
  cmd_def_t *root_name    = NULL;   // name match with parent == NULL
  cmd_def_t *name_match   = NULL;   // any name match
  cmd_def_t *root_abbrev  = NULL;   // abbrev match with parent == NULL
  cmd_def_t *abbrev_match = NULL;   // any abbrev match

  for(cmd_def_t *d = cmd_list; d != NULL; d = d->next)
  {
    if(strncasecmp(d->name, name, CMD_NAME_SZ) == 0)
    {
      if(root_name == NULL && d->parent == NULL)
        root_name = d;

      if(name_match == NULL)
        name_match = d;
    }

    if(d->abbrev[0] != '\0'
        && strncasecmp(d->abbrev, name, CMD_NAME_SZ) == 0)
    {
      if(root_abbrev == NULL && d->parent == NULL)
        root_abbrev = d;

      if(abbrev_match == NULL)
        abbrev_match = d;
    }
  }

  if(root_name != NULL)
    return(root_name);

  if(name_match != NULL)
    return(name_match);

  if(root_abbrev != NULL)
    return(root_abbrev);

  return(abbrev_match);
}

// True if two kind_filter arrays share at least one common kind
// (case-insensitive). A NULL filter matches everything, so either being
// NULL forces overlap = true. This is the basis for allowing same-named
// siblings under a common parent when their kind filters are disjoint
// (e.g. ":default" under show/bot registered once per bot driver kind).
// Caller must hold cmd_mutex.
static bool
kind_filters_overlap(const char *const *a, const char *const *b)
{
  if(a == NULL || b == NULL)
    return(true);

  for(size_t i = 0;
      i < CMD_KIND_FILTER_MAX && a[i] != NULL; i++)
  {
    for(size_t j = 0;
        j < CMD_KIND_FILTER_MAX && b[j] != NULL; j++)
      if(strcasecmp(a[i], b[j]) == 0)
        return(true);
  }

  return(false);
}

// Collision check that honors kind_filter. Returns true if any existing
// sibling of `parent` has the same name or abbrev AND a kind_filter
// that overlaps `kind_filter`. Siblings whose filters are disjoint are
// treated as non-colliding, enabling one verb name per bot kind under
// the same parent (e.g. ":default" per driver kind under show/bot).
static bool
def_name_collides_for_kind_locked(const char *str, cmd_def_t *parent,
    const char *const *kind_filter)
{
  for(cmd_def_t *d = cmd_list; d != NULL; d = d->next)
  {
    bool name_hit;

    if(d->parent != parent)
      continue;

    name_hit = (strncasecmp(d->name, str, CMD_NAME_SZ) == 0)
        || (d->abbrev[0] != '\0'
            && strncasecmp(d->abbrev, str, CMD_NAME_SZ) == 0);

    if(!name_hit)
      continue;

    if(kind_filters_overlap(d->kind_filter, kind_filter))
      return(true);
  }

  return(false);
}

// Forward declaration (defined after def_find_locked, used below).
static cmd_def_t *def_find_child_locked(cmd_def_t *parent, const char *name);

// Internal: resolve a slash-delimited parent path (e.g. "irc/network")
// to a cmd_def_t by walking the command tree. Single-segment paths like
// "bot" are also handled. Caller must hold cmd_mutex.
// returns: the resolved command, or NULL if any segment is not found
static cmd_def_t *
resolve_parent_path_locked(const char *path)
{
  char buf[CMD_USAGE_SZ];
  char *tok;
  char *slash;
  cmd_def_t *d;

  if(path == NULL || path[0] == '\0')
    return(NULL);

  // Copy path so we can tokenize on '/'.
  strlcpy(buf, path, sizeof(buf));

  // First segment: root-level lookup.
  tok = buf;
  slash = strchr(tok, '/');

  if(slash != NULL)
    *slash = '\0';

  d = def_find_locked(tok);

  if(d == NULL)
    return(NULL);

  // Subsequent segments: child lookups.
  while(slash != NULL)
  {
    tok = slash + 1;
    slash = strchr(tok, '/');

    if(slash != NULL)
      *slash = '\0';

    if(tok[0] == '\0')
      continue;

    d = def_find_child_locked(d, tok);

    if(d == NULL)
      return(NULL);
  }

  return(d);
}

// Global command registration

// Validate an argument descriptor array for a command being registered.
// returns: true if valid, false on error (logs warning)
static bool
reg_validate_args(const char *name, const cmd_arg_desc_t *arg_desc,
    uint8_t arg_count)
{
  bool seen_optional = false;

  if(arg_count > CMD_MAX_ARGS)
  {
    clam(CLAM_WARN, "cmd_register",
        "'%s': too many arg descriptors (%u, max %u)",
        name, (unsigned)arg_count, CMD_MAX_ARGS);
    return(false);
  }

  for(uint8_t i = 0; i < arg_count; i++)
  {
    if(arg_desc[i].flags & CMD_ARG_OPTIONAL)
      seen_optional = true;
    else if(seen_optional)
    {
      clam(CLAM_WARN, "cmd_register",
          "'%s': required arg '%s' follows optional arg",
          name, arg_desc[i].name ? arg_desc[i].name : "?");
      return(false);
    }

    if((arg_desc[i].flags & CMD_ARG_REST) && i != arg_count - 1)
    {
      clam(CLAM_WARN, "cmd_register",
          "'%s': CMD_ARG_REST only allowed on last arg", name);
      return(false);
    }

    if(arg_desc[i].type == CMD_ARG_CUSTOM && arg_desc[i].custom == NULL)
    {
      clam(CLAM_WARN, "cmd_register",
          "'%s': CMD_ARG_CUSTOM requires a validator function", name);
      return(false);
    }
  }

  return(true);
}

// Check name and abbreviation collisions for a command being registered.
// Caller must hold cmd_mutex.
static bool
reg_check_collisions_locked(const char *name, const char *abbrev,
    cmd_def_t *parent, const char *const *kind_filter)
{
  if(def_name_collides_for_kind_locked(name, parent, kind_filter))
  {
    clam(CLAM_WARN, "cmd_register", "duplicate command: '%s'", name);
    return(false);
  }

  // Check abbreviation collision within the same kind scope.
  if(abbrev != NULL && abbrev[0] != '\0')
  {
    if(def_name_collides_for_kind_locked(abbrev, parent, kind_filter))
    {
      clam(CLAM_WARN, "cmd_register",
          "'%s': abbreviation '%s' collides with existing command",
          name, abbrev);
      return(false);
    }
  }

  return(true);
}

// Allocate, populate, and link a new cmd_def_t into the global list.
// Caller must hold cmd_mutex.
static cmd_def_t *
reg_populate_def(const char *module, const char *name,
    const char *usage, const char *description,
    const char *help_long, const char *group, uint16_t level,
    cmd_scope_t scope, method_type_t methods, cmd_cb_t cb, void *data,
    const char *abbrev, const cmd_arg_desc_t *arg_desc,
    uint8_t arg_count, const char *const *kind_filter,
    const cmd_nl_t *nl, cmd_def_t *parent)
{
  cmd_def_t *d = mem_alloc("cmd", "def", sizeof(*d));
  memset(d, 0, sizeof(*d));

  if(module != NULL)
    strlcpy(d->module, module, CMD_MODULE_SZ);

  strlcpy(d->name, name, CMD_NAME_SZ);

  if(abbrev != NULL && abbrev[0] != '\0')
    strlcpy(d->abbrev, abbrev, CMD_NAME_SZ);

  d->usage       = usage;
  d->description = description;
  d->help_long   = help_long;

  strlcpy(d->group, group, USERNS_GROUP_SZ);
  d->level   = level;
  d->scope   = scope;
  d->cb      = cb;
  d->data    = data;
  d->arg_desc    = arg_desc;
  d->arg_count   = arg_count;
  d->kind_filter = kind_filter;
  d->nl          = nl;

  // Link to parent if specified.
  if(parent != NULL)
  {
    d->parent = parent;
    d->sibling = parent->children;
    parent->children = d;
  }

  // Method type scoping. Subcommands inherit their parent's bitmask
  // when they pass METHOD_T_ANY; explicit bitmasks are ANDed with
  // the parent to prevent a child from widening visibility.
  if(parent != NULL && methods == METHOD_T_ANY)
    d->methods = parent->methods;
  else if(parent != NULL)
    d->methods = methods & parent->methods;
  else
    d->methods = methods;

  // Prepend to global list.
  d->next = cmd_list;
  cmd_list = d;
  cmd_def_count++;

  return(d);
}

// Register a command globally.
bool
cmd_register(const char *module, const char *name,
    const char *usage, const char *description,
    const char *help_long, const char *group, uint16_t level,
    cmd_scope_t scope, method_type_t methods, cmd_cb_t cb, void *data,
    const char *parent_path, const char *abbrev,
    const cmd_arg_desc_t *arg_desc, uint8_t arg_count,
    const char *const *kind_filter,
    const cmd_nl_t *nl)
{
  // The owning object is whoever called us. Core is link_whole'd into
  // botman with export_dynamic, so a plugin reaches this symbol through
  // the PLT and the return address lands in the plugin's own mapping.
  const void *owner_pc = __builtin_return_address(0);

  if(name == NULL || name[0] == '\0' || cb == NULL)
  {
    clam(CLAM_WARN, "cmd_register", "invalid arguments");
    return(FAIL);
  }

  if(group == NULL || group[0] == '\0')
  {
    clam(CLAM_WARN, "cmd_register",
        "'%s': group name is required", name);
    return(FAIL);
  }

  // Validate arg spec if provided.
  if(arg_desc != NULL && arg_count > 0
      && !reg_validate_args(name, arg_desc, arg_count))
    return(FAIL);

  // Validate NL hint invariants if provided.
  if(nl != NULL)
  {
    if(nl->when == NULL || nl->syntax == NULL)
    {
      clam(CLAM_WARN, "cmd_register",
          "'%s': nl without when/syntax; rejecting", name);
      return(FAIL);
    }

    if(nl->example_count < 2 || nl->examples == NULL)
    {
      clam(CLAM_WARN, "cmd_register",
          "'%s': nl requires >=2 examples; got %u",
          name, (unsigned)nl->example_count);
      return(FAIL);
    }

    if(nl->slot_count > CMD_MAX_ARGS)
    {
      clam(CLAM_WARN, "cmd_register",
          "'%s': nl slot_count %u exceeds CMD_MAX_ARGS %d",
          name, (unsigned)nl->slot_count, CMD_MAX_ARGS);
      return(FAIL);
    }
  }

  {
    cmd_def_t *parent = NULL;
    cmd_def_t *d;

    pthread_mutex_lock(&cmd_mutex);

    // Resolve parent if specified (needed for scoped collision check).
    if(parent_path != NULL && parent_path[0] != '\0')
    {
      parent = resolve_parent_path_locked(parent_path);

      if(parent == NULL)
      {
        pthread_mutex_unlock(&cmd_mutex);
        clam(CLAM_WARN, "cmd_register",
            "'%s': parent command '%s' not found", name, parent_path);
        return(FAIL);
      }
    }

    if(!reg_check_collisions_locked(name, abbrev, parent, kind_filter))
    {
      pthread_mutex_unlock(&cmd_mutex);
      return(FAIL);
    }

    d = reg_populate_def(module, name, usage, description,
        help_long, group, level, scope, methods, cb, data, abbrev,
        arg_desc, arg_count, kind_filter, nl, parent);

    d->owner_pc = owner_pc;

    pthread_mutex_unlock(&cmd_mutex);

    clam(CLAM_DEBUG, "cmd_register",
        "registered '%s' (module: %s, group: %s, level: %u%s%s)", name,
        module ? module : "(none)", group,
        (unsigned)level,
        d->parent ? ", parent: " : "",
        d->parent ? d->parent->name : "");
  }
  return(SUCCESS);
}

// Collect `d` and every descendant into `out` in post-order, so a parent
// never precedes one of its children. Caller must hold cmd_mutex.
// returns: total collected (always >= 1), or 0 if the subtree exceeds cap
static uint32_t
def_collect_subtree_locked(cmd_def_t *d, cmd_def_t **out, uint32_t cap,
    uint32_t n)
{
  for(cmd_def_t *c = d->children; c != NULL; c = c->sibling)
  {
    n = def_collect_subtree_locked(c, out, cap, n);

    if(n == 0)
      return(0);
  }

  if(n >= cap)
    return(0);

  out[n++] = d;
  return(n);
}

// Reconstruct `d`'s slash-joined registration path (e.g.
// "irc/network/list"). Caller must hold cmd_mutex.
static void
def_path_locked(const cmd_def_t *d, char *buf, size_t cap)
{
  const cmd_def_t *chain[CMD_PATH_MAX_DEPTH];
  uint32_t         n   = 0;
  size_t           len = 0;

  buf[0] = '\0';

  for(const cmd_def_t *p = d; p != NULL && n < CMD_PATH_MAX_DEPTH;
      p = p->parent)
    chain[n++] = p;

  // chain[] runs leaf -> root; emit it back to front.
  while(n > 0 && len + 1 < cap)
  {
    n--;
    len += (size_t)snprintf(buf + len, cap - len, "%s%s",
        len > 0 ? "/" : "", chain[n]->name);

    if(len >= cap)
    {
      buf[cap - 1] = '\0';
      return;
    }
  }
}

static bool
def_in_set(const cmd_def_t *d, cmd_def_t *const *set, uint32_t n)
{
  for(uint32_t i = 0; i < n; i++)
    if(set[i] == d)
      return(true);

  return(false);
}

// Collect `d`'s subtree into `out` and unlink every member from both the
// command tree and the global list. The definitions themselves are left
// for the caller to free once cmd_mutex is dropped -- mem_free() logs,
// and logging under this lock is how the clam re-entry guard earns its
// keep. Caller must hold cmd_mutex.
// returns: number detached, or 0 if the subtree exceeds cap
static uint32_t
def_detach_subtree_locked(cmd_def_t *d, cmd_def_t **out, uint32_t cap)
{
  cmd_def_t *prev;
  cmd_def_t *cur;
  uint32_t   n;

  n = def_collect_subtree_locked(d, out, cap, 0);

  if(n == 0)
    return(0);

  // Unlink the top node from its parent's sibling chain. Descendants
  // need no such unlink -- their parents are freed alongside them.
  if(d->parent != NULL)
  {
    cmd_def_t **pp = &d->parent->children;

    while(*pp != NULL)
    {
      if(*pp == d)
      {
        *pp = d->sibling;
        break;
      }

      pp = &(*pp)->sibling;
    }
  }

  // A single pass over the global list drops every victim.
  prev = NULL;
  cur  = cmd_list;

  while(cur != NULL)
  {
    cmd_def_t *next = cur->next;

    if(def_in_set(cur, out, n))
    {
      if(prev != NULL)
        prev->next = next;
      else
        cmd_list = next;

      cmd_def_count--;
    }

    else
      prev = cur;

    cur = next;
  }

  return(n);
}

// Unregister a command subtree addressed by its registration path.
uint32_t
cmd_unregister_path(const char *path)
{
  cmd_def_t *victims[CMD_UNREG_MAX_SUBTREE];
  cmd_def_t *d;
  uint32_t   n;

  if(path == NULL || path[0] == '\0')
    return(0);

  // Every plugin unregisters its verbs from deinit(), which main.c runs
  // at step 6 -- long after cmd_exit() (step 1b) has freed every
  // definition and destroyed cmd_mutex. Locking it there is undefined
  // behaviour that glibc happens to tolerate, and it is loud: TSan
  // counted 89 reports in one shutdown. There is nothing left to
  // unregister once the subsystem is down, so say so before the lock.
  if(!cmd_ready)
    return(0);

  pthread_mutex_lock(&cmd_mutex);
  d = resolve_parent_path_locked(path);

  // An unresolved path is not a warning: teardown runs on partially
  // registered plugins and must be idempotent.
  if(d == NULL)
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(0);
  }

  n = def_detach_subtree_locked(d, victims, CMD_UNREG_MAX_SUBTREE);
  pthread_mutex_unlock(&cmd_mutex);

  if(n == 0)
  {
    clam(CLAM_WARN, "cmd_unregister",
        "'%s': subtree exceeds %u definitions; refusing to unregister",
        path, (unsigned)CMD_UNREG_MAX_SUBTREE);
    return(0);
  }

  for(uint32_t i = 0; i < n; i++)
    mem_free(victims[i]);

  clam(CLAM_DEBUG, "cmd_unregister",
      "unregistered '%s' (%u definition(s))", path, (unsigned)n);
  return(n);
}

static bool
def_owned_by(const cmd_def_t *d, uintptr_t lo, uintptr_t hi)
{
  uintptr_t pc = (uintptr_t)d->owner_pc;

  return(pc >= lo && pc < hi);
}

// Find the shallowest definition owned by [lo,hi) -- one whose parent is
// owned by somebody else, so that taking it takes a whole subtree rather
// than orphaning the nodes above it. Caller must hold cmd_mutex.
static cmd_def_t *
def_next_owned_root_locked(uintptr_t lo, uintptr_t hi)
{
  for(cmd_def_t *d = cmd_list; d != NULL; d = d->next)
    if(def_owned_by(d, lo, hi)
        && (d->parent == NULL || !def_owned_by(d->parent, lo, hi)))
      return(d);

  return(NULL);
}

// Reclaim every definition registered from inside one loaded object.
uint32_t
cmd_reclaim_owned(uintptr_t lo, uintptr_t hi)
{
  cmd_def_t *victims[CMD_UNREG_MAX_SUBTREE];
  char       path[CMD_USAGE_SZ];
  uint32_t   total = 0;

  if(lo >= hi)
    return(0);

  // One subtree per pass: the list is rewritten each time, so the scan
  // restarts rather than carrying stale pointers across the mutation.
  for(;;)
  {
    cmd_def_t *root;
    uint32_t   foreign = 0;
    uint32_t   n;

    pthread_mutex_lock(&cmd_mutex);
    root = def_next_owned_root_locked(lo, hi);

    if(root == NULL)
    {
      pthread_mutex_unlock(&cmd_mutex);
      break;
    }

    def_path_locked(root, path, sizeof(path));
    n = def_detach_subtree_locked(root, victims, CMD_UNREG_MAX_SUBTREE);

    for(uint32_t i = 0; i < n; i++)
      if(!def_owned_by(victims[i], lo, hi))
        foreign++;

    pthread_mutex_unlock(&cmd_mutex);

    if(n == 0)
    {
      clam(CLAM_WARN, "cmd_reclaim",
          "'%s': subtree exceeds %u definitions; leaving it registered",
          path, (unsigned)CMD_UNREG_MAX_SUBTREE);
      break;
    }

    for(uint32_t i = 0; i < n; i++)
      mem_free(victims[i]);

    // A foreign child under a reclaimed parent means some other object
    // hung its command off this one's node. It goes with the parent --
    // it has nowhere else to live -- but it is a layering bug and the
    // owner deserves to hear about it.
    if(foreign > 0)
      clam(CLAM_WARN, "cmd_reclaim",
          "'%s': %u definition(s) in this subtree belong to another "
          "object; reclaimed with the parent", path, foreign);

    total += n;
  }

  if(total > 0)
    clam(CLAM_DEBUG, "cmd_reclaim", "reclaimed %u definition(s)", total);

  return(total);
}

bool
cmd_set_help_extender(const char *name, const char *child,
    cmd_help_extender_t ext)
{
  cmd_def_t *d;

  if(name == NULL || name[0] == '\0' || ext == NULL)
    return(FAIL);

  pthread_mutex_lock(&cmd_mutex);
  d = def_find_locked(name);

  if(d != NULL && child != NULL && child[0] != '\0')
    d = def_find_child_locked(d, child);

  if(d == NULL)
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(FAIL);
  }

  d->help_ext = ext;
  pthread_mutex_unlock(&cmd_mutex);
  return(SUCCESS);
}

// Find a registered command.
const cmd_def_t *
cmd_find(const char *name)
{
  cmd_def_t *d;

  if(name == NULL || name[0] == '\0')
    return(NULL);

  pthread_mutex_lock(&cmd_mutex);
  d = def_find_locked(name);
  pthread_mutex_unlock(&cmd_mutex);
  return(d);
}

// Find a child command under a given parent, by name or abbreviation.
const cmd_def_t *
cmd_find_child(const cmd_def_t *parent, const char *name)
{
  cmd_def_t *c;

  if(parent == NULL || name == NULL || name[0] == '\0')
    return(NULL);

  pthread_mutex_lock(&cmd_mutex);
  c = def_find_child_locked((cmd_def_t *)parent, name);
  pthread_mutex_unlock(&cmd_mutex);
  return(c);
}

// The definition's own allowlist, verbatim. Deciding whether a bot may
// reach a verb is core/bot_cmd.c's job -- what a method kind means is
// knowledge this registry deliberately does not carry. No lock: the
// array is caller-owned static storage, never mutated after
// registration, and the pointer field is written once under cmd_mutex
// before the definition is ever reachable.
const char *const *
cmd_kind_filter_of(const cmd_def_t *def)
{
  return(def != NULL ? def->kind_filter : NULL);
}

// Get global command count.
uint32_t
cmd_count(void)
{
  return(cmd_def_count);
}

// Clean up per-bot command state (prefix set).
void
cmd_bot_cleanup(bot_inst_t *inst)
{
  cmd_set_t *s = NULL;
  cmd_set_t *prev = NULL;

  if(inst == NULL)
    return;

  pthread_mutex_lock(&cmd_mutex);

  for(s = cmd_sets; s != NULL; prev = s, s = s->next)
    if(s->inst == inst)
      break;

  if(s == NULL)
  {
    pthread_mutex_unlock(&cmd_mutex);
    return;
  }

  // Unlink from list.
  if(prev != NULL)
    prev->next = s->next;
  else
    cmd_sets = s->next;

  set_put(s);

  pthread_mutex_unlock(&cmd_mutex);

  clam(CLAM_DEBUG, "cmd_bot_cleanup",
      "'%s': cleaned up command state", bot_inst_name(inst));
}

// Command prefix

// Set the command prefix for a bot instance.
bool
cmd_set_prefix(bot_inst_t *inst, const char *prefix)
{
  cmd_set_t *s;

  if(inst == NULL || prefix == NULL || prefix[0] == '\0')
    return(FAIL);

  if(strlen(prefix) >= CMD_PREFIX_SZ)
  {
    clam(CLAM_WARN, "cmd_set_prefix",
        "'%s': prefix too long (max %d)", bot_inst_name(inst),
        CMD_PREFIX_SZ - 1);
    return(FAIL);
  }

  pthread_mutex_lock(&cmd_mutex);

  s = set_ensure_locked(inst);
  strlcpy(s->prefix, prefix, CMD_PREFIX_SZ);

  pthread_mutex_unlock(&cmd_mutex);

  clam(CLAM_DEBUG, "cmd_set_prefix",
      "'%s': prefix set to '%s'", bot_inst_name(inst), prefix);
  return(SUCCESS);
}

// Get the command prefix for a bot instance.
const char *
cmd_get_prefix(const bot_inst_t *inst)
{
  cmd_set_t *s;
  const char *prefix;

  if(inst == NULL)
    return("!");

  pthread_mutex_lock(&cmd_mutex);

  s = set_find_locked(inst);
  prefix = (s != NULL) ? s->prefix : "!";

  pthread_mutex_unlock(&cmd_mutex);
  return(prefix);
}

// Internal: find a child command by name under a parent.
// Caller must hold cmd_mutex.
static cmd_def_t *
def_find_child_locked(cmd_def_t *parent, const char *name)
{
  cmd_def_t *abbrev_match = NULL;

  for(cmd_def_t *c = parent->children; c != NULL; c = c->sibling)
  {
    if(strncasecmp(c->name, name, CMD_NAME_SZ) == 0)
      return(c);

    if(abbrev_match == NULL && c->abbrev[0] != '\0'
        && strncasecmp(c->abbrev, name, CMD_NAME_SZ) == 0)
      abbrev_match = c;
  }

  return(abbrev_match);
}

// Internal: resolve subcommand from args if parent has children.
// On match, updates *def to the child and *args to the remaining args.
// Caller must hold cmd_mutex.
static void
resolve_subcmd_locked(cmd_def_t **def, const char **args)
{
  // Iteratively resolve nested subcommands (e.g., show → irc → networks).
  for(;;)
  {
    char sub_name[CMD_NAME_SZ] = {0};
    const char *p;
    const char *rest;
    cmd_def_t *child;
    size_t i = 0;

    if((*def)->children == NULL || *args == NULL || (*args)[0] == '\0')
      return;

    // Parse the first token from args as a potential subcommand name.
    p = *args;

    while(p[i] != '\0' && p[i] != ' ' && p[i] != '\t'
        && i < CMD_NAME_SZ - 1)
    {
      sub_name[i] = p[i];
      i++;
    }

    child = def_find_child_locked(*def, sub_name);

    if(child == NULL)
      return;

    // Advance args past the subcommand name and whitespace.
    rest = p + i;

    while(*rest == ' ' || *rest == '\t')
      rest++;

    *def  = child;
    *args = rest;
  }
}

// Argument parsing and validation

// Type-specific error reason strings.
static const char *
cmd_arg_type_reason(cmd_arg_type_t type)
{
  switch(type)
  {
    case CMD_ARG_ALNUM:    return "alphanumeric and underscores only";
    case CMD_ARG_DIGITS:   return "digits only";
    case CMD_ARG_HOSTNAME: return "valid hostname expected";
    case CMD_ARG_PORT:     return "must be 1-65535";
    case CMD_ARG_CHANNEL:  return "no spaces, control characters, or commas";
    case CMD_ARG_CUSTOM:   return "invalid format";
    default:               return "invalid";
  }
}

static bool
cmd_arg_validate(const cmd_arg_desc_t *desc, const char *str)
{
  size_t maxlen = desc->maxlen > 0 ? desc->maxlen : CMD_ARG_SZ - 1;

  switch(desc->type)
  {
    case CMD_ARG_NONE:
      return true;

    case CMD_ARG_ALNUM:
      return validate_alnum(str, maxlen);

    case CMD_ARG_DIGITS:
      return validate_digits(str, 1, maxlen);

    case CMD_ARG_HOSTNAME:
      return validate_hostname(str);

    case CMD_ARG_PORT:
      return validate_port(str, NULL);

    case CMD_ARG_CHANNEL:
      return validate_irc_channel(str);

    case CMD_ARG_CUSTOM:
      return desc->custom != NULL && desc->custom(str);
  }

  return false;
}

// Parse and validate arguments according to an arg spec. Tokenizes the
// raw argument string into pre-allocated buffers, checks required arg
// count, and validates each token. On failure, sends an error reply via
// cmd_reply() and returns false.
//
static bool
cmd_parse_args(const char *args, const cmd_arg_desc_t *desc,
    uint8_t count, char bufs[][CMD_ARG_SZ], cmd_args_t *parsed,
    const cmd_ctx_t *ctx, const char *usage)
{
  const char *p = args;

  memset(parsed, 0, sizeof(*parsed));

  for(uint8_t i = 0; i < count; i++)
  {
    // Skip leading whitespace.
    while(*p == ' ' || *p == '\t')
      p++;

    if(*p == '\0')
    {
      // No more input. Check if remaining args are optional.
      if(!(desc[i].flags & CMD_ARG_OPTIONAL))
      {
        char buf[CMD_USAGE_SZ + 16];
        snprintf(buf, sizeof(buf), "usage: /%s", usage);
        cmd_reply(ctx, buf);
        return false;
      }

      break;
    }

    if(desc[i].flags & CMD_ARG_REST)
    {
      // Optional quoting: if the remainder starts with a double-quote,
      // extract only the content between the quotes so that trailing
      // whitespace and other special characters are preserved exactly.
      if(*p == '"')
      {
        const char *start = p + 1;
        const char *end   = strrchr(start, '"');

        if(end != NULL && end > start)
        {
          size_t len = (size_t)(end - start);

          if(len >= CMD_ARG_SZ)
            len = CMD_ARG_SZ - 1;

          memcpy(bufs[i], start, len);
          bufs[i][len] = '\0';
        }

        else
        {
          // No closing quote — treat entire remainder literally.
          strlcpy(bufs[i], p, CMD_ARG_SZ);
        }
      }

      else
        strlcpy(bufs[i], p, CMD_ARG_SZ);

      parsed->argv[i] = bufs[i];
      parsed->argc = i + 1;

      // Validate the rest-of-line token.
      if(desc[i].type != CMD_ARG_NONE
          && !cmd_arg_validate(&desc[i], bufs[i]))
      {
        char buf[CMD_ARG_SZ + 64];
        snprintf(buf, sizeof(buf), "invalid %s (%s)",
            desc[i].name ? desc[i].name : "argument",
            cmd_arg_type_reason(desc[i].type));
        cmd_reply(ctx, buf);
        return false;
      }

      return true;
    }

    // Normal tokenization: extract until whitespace.
    // Optional quoting: a leading double-quote causes extraction of
    // the content up to the next double-quote, preserving spaces.
    {
      size_t maxlen = desc[i].maxlen > 0 ? desc[i].maxlen : CMD_ARG_SZ - 1;
      size_t j = 0;

      if(*p == '"')
      {
        p++;  // skip opening quote

        while(*p != '\0' && *p != '"' && j < maxlen)
          bufs[i][j++] = *p++;

        if(*p == '"')
          p++;  // skip closing quote
      }

      else
      {
        while(*p != '\0' && *p != ' ' && *p != '\t' && j < maxlen)
          bufs[i][j++] = *p++;

        // If there are more non-whitespace chars, the token was too long.
        // Consume the rest of this token to keep parsing consistent.
        if(*p != '\0' && *p != ' ' && *p != '\t')
          while(*p != '\0' && *p != ' ' && *p != '\t')
            p++;
      }

      bufs[i][j] = '\0';
      parsed->argv[i] = bufs[i];
      parsed->argc = i + 1;
    }

    // Validate.
    if(desc[i].type != CMD_ARG_NONE
        && !cmd_arg_validate(&desc[i], bufs[i]))
    {
      char buf[CMD_ARG_SZ + 64];
      snprintf(buf, sizeof(buf), "invalid %s (%s)",
          desc[i].name ? desc[i].name : "argument",
          cmd_arg_type_reason(desc[i].type));
      cmd_reply(ctx, buf);
      return false;
    }
  }

  return true;
}

// True iff the resolved caller is an admin in the bot's userns. Used to
// arm the thread-local admin context so secret-tier KVs deredact for the
// duration of the command callback.
static bool
cmd_caller_is_admin(bot_inst_t *bot, const char *username)
{
  userns_t *ns;

  if(bot == NULL || username == NULL || username[0] == '\0')
    return(false);

  ns = bot_get_userns(bot);

  if(ns == NULL)
    return(false);

  return(userns_member_check(ns, username, USERNS_GROUP_ADMIN));
}

// True iff secret-tier (`.creds.*`) KV values may be de-redacted for the
// duration of this command. The gate is deliberately narrow: the caller
// must be an admin AND the command must have arrived over the botmanctl
// control socket. Every other method (IRC, chat, ...) — even for an
// admin — sees `[CENSORED]` in place of credential values. botmanctl is
// the operator's local Unix-socket channel and the sole path to view
// stored credentials.
static bool
cmd_creds_visible(const method_inst_t *inst, bool admin)
{
  return(admin && inst != NULL
      && method_inst_type(inst) == METHOD_T_BOTMANCTL);
}

// In-flight accounting
//
// The barrier that guards a dlclose walks the task and curl queues and
// the bot registry (core/plugin.c, plugin_quiesce). A command handler
// is in none of them: the task's own callback is cmd_task_cb, which
// lives in core, and cmd_dispatch_as runs the handler with no task at
// all. So the three dispatchers below record the handler themselves,
// and cmd_inflight_owned() answers for the mapping it lives in.

static void
cmd_inflight_enter(cmd_inflight_t *node, cmd_cb_t cb, const char *name)
{
  node->cb = cb;
  strlcpy(node->name, name != NULL ? name : "(unnamed)", sizeof(node->name));

  pthread_mutex_lock(&cmd_inflight_mutex);
  node->next        = cmd_inflight_list;
  cmd_inflight_list = node;
  pthread_mutex_unlock(&cmd_inflight_mutex);
}

static void
cmd_inflight_leave(cmd_inflight_t *node)
{
  cmd_inflight_t **pp;

  pthread_mutex_lock(&cmd_inflight_mutex);

  for(pp = &cmd_inflight_list; *pp != NULL; pp = &(*pp)->next)
  {
    if(*pp != node)
      continue;

    *pp = node->next;
    break;
  }

  pthread_mutex_unlock(&cmd_inflight_mutex);
}

uint32_t
cmd_inflight_owned(uintptr_t lo, uintptr_t hi, char *out, size_t out_cap)
{
  uint32_t n = 0;

  if(out != NULL && out_cap > 0)
    out[0] = '\0';

  if(lo >= hi)
    return(0);

  pthread_mutex_lock(&cmd_inflight_mutex);

  for(const cmd_inflight_t *c = cmd_inflight_list; c != NULL; c = c->next)
  {
    uintptr_t addr = (uintptr_t)fn_addr(&c->cb);

    if(addr < lo || addr >= hi)
      continue;

    if(n == 0 && out != NULL && out_cap > 0)
      strlcpy(out, c->name, out_cap);

    n++;
  }

  pthread_mutex_unlock(&cmd_inflight_mutex);
  return(n);
}

// A dispatched command outlives the delivery that produced it — the
// message is copied by value and the callback runs on a task thread —
// so the copy carries its own reference to the originating method
// instance. Without it, a `/plugin reload` between dispatch and reply
// unregisters the instance the reply is about to be sent on.
static void
cmd_task_data_free(cmd_task_data_t *d)
{
  method_release(d->msg.inst);
  mem_free(d);
}

// Task callback for async command execution. Parses args if the command
// has an arg spec, then invokes the command callback.
static void
cmd_task_cb(task_t *t)
{
  cmd_task_data_t *d = (cmd_task_data_t *)t->data;
  const char      *uname;
  bool             admin;
  bool             creds;
  cmd_inflight_t   node;

  cmd_ctx_t ctx = {
    .bot      = d->bot,
    .msg      = &d->msg,
    .args     = d->args,
    .username = d->username[0] != '\0' ? d->username : NULL,
    .parsed   = NULL,
    .data     = d->cb_data,
  };

  // Opens before the parse, not before the call: arg_desc is the
  // plugin's too, and 4 of the 53 plugin descriptor tables name a
  // validator cmd_parse_args reaches through.
  cmd_inflight_enter(&node, d->cb, d->name);

  // Pre-parse and validate arguments if the command has an arg spec.
  cmd_args_t parsed;

  if(d->arg_desc != NULL && d->arg_count > 0)
  {
    if(!cmd_parse_args(d->args, d->arg_desc, d->arg_count,
        d->arg_bufs, &parsed, &ctx, d->usage))
    {
      cmd_task_data_free(d);
      cmd_inflight_leave(&node);
      t->state = TASK_ENDED;
      return;
    }

    ctx.parsed = &parsed;
  }

  uname = (d->username[0] != '\0') ? d->username : NULL;
  admin = cmd_caller_is_admin(d->bot, uname);
  creds = cmd_creds_visible(d->msg.inst, admin);

  if(creds)
    kv_admin_context_set(true);

  d->cb(&ctx);

  if(creds)
    kv_admin_context_set(false);

  cmd_task_data_free(d);
  cmd_inflight_leave(&node);

  t->state = TASK_ENDED;
}

// Command dispatch

// Dispatch a message as a potential command for a bot instance. Parses
// the prefix and command name, checks permissions, and submits a task
// for async execution.
// returns: SUCCESS if dispatched, FAIL if not a command or denied

// Unified permission formula — single source of truth. Shared by
// cmd_dispatch, cmd_dispatch_as, and help visibility.
static bool check_permission(userns_t *ns, const char *username,
    const char *req_group, uint16_t req_level);

// Pure predicate form of the cmd_dispatch gate. Mirrors the method-type
// / scope / group+level checks cmd_dispatch applies, but emits no log
// line and sends no reply — callers (NL bridge preflight, prompt
// command-block filter) use this to decide silently whether a command
// would be accepted. cmd_dispatch itself keeps its in-place denial
// replies so anonymous users typing a gated command still get the
// specific error messages; the three predicates here are the
// predicate-only twin of that flow.
bool
cmd_permits(bot_inst_t *inst, const method_msg_t *msg, const cmd_def_t *def)
{
  method_type_t inst_type;
  const char *username;
  char ubuf[USERNS_USER_SZ];
  userns_t   *ns;

  if(inst == NULL || msg == NULL || def == NULL)
    return(false);

  // Method-type gate: if the message carries a method instance, the
  // command's method mask must include that type. A NULL inst (synthetic
  // preflight with no wire-level method) skips this check, matching
  // cmd_dispatch's `inst_type != 0` guard.
  inst_type = (msg->inst != NULL)
      ? method_inst_type(msg->inst) : 0;

  if(inst_type != 0 && !(def->methods & inst_type))
    return(false);

  // Scope gate: CMD_SCOPE_PRIVATE means DM-only, CMD_SCOPE_PUBLIC means
  // channel-only, CMD_SCOPE_ANY always passes. The "no method instance"
  // branch matches cmd_dispatch, which only enforces scope when msg->inst
  // is non-NULL.
  if(def->scope != CMD_SCOPE_ANY && msg->inst != NULL)
  {
    bool is_public = (msg->channel[0] != '\0');

    if(def->scope == CMD_SCOPE_PRIVATE && is_public)
      return(false);

    if(def->scope == CMD_SCOPE_PUBLIC && !is_public)
      return(false);
  }

  // Permission gate: same formula as cmd_dispatch. Unauthenticated
  // callers are implicit members of "everyone" at level 0; authenticated
  // callers resolve through the bot's userns.
  username = bot_identity_resolve(inst, msg->inst, msg->sender,
      msg->metadata, ubuf, sizeof(ubuf)) ? ubuf : NULL;
  ns       = (username != NULL) ? bot_get_userns(inst) : NULL;

  return(check_permission(ns, username, def->group, def->level));
}

bool
cmd_dispatch(bot_inst_t *inst, const method_msg_t *msg)
{
  const char *prefix = NULL;
  cmd_set_t *s;
  size_t pfx_len;
  const char *start;
  char cmd_name[CMD_NAME_SZ] = {0};
  size_t i = 0;
  const char *args;
  cmd_def_t *d;
  method_type_t inst_type;
  cmd_cb_t cb;
  void *cb_data;
  uint16_t req_level;
  cmd_scope_t scope;
  char group[USERNS_GROUP_SZ];
  const cmd_arg_desc_t *arg_desc;
  uint8_t arg_count;
  const char *usage;
  const char *username;
  char ubuf[USERNS_USER_SZ];
  userns_t *ns_for_check;
  cmd_task_data_t *td;
  char task_name[TASK_NAME_SZ];
  task_t *t;

  if(inst == NULL || msg == NULL || msg->text[0] == '\0')
    return(FAIL);

  // Resolve per-method prefix from KV, falling back to bot-level prefix.
  // Must happen before taking the mutex (kv_get_str has its own locking).
  if(msg->inst != NULL)
  {
    const char *kind = method_inst_kind(msg->inst);
    const char *bname = bot_inst_name(inst);

    if(kind != NULL && bname != NULL)
    {
      const char *kv_pfx;

      kv_pfx = kv_get_bot_method_str(bname, kind, "prefix");

      if(kv_pfx != NULL && kv_pfx[0] != '\0')
        prefix = kv_pfx;
    }
  }

  pthread_mutex_lock(&cmd_mutex);

  // Fall back to bot-level prefix if per-method not available.
  s = set_find_locked(inst);

  if(prefix == NULL)
    prefix = (s != NULL) ? s->prefix : "!";

  pfx_len = strlen(prefix);

  // Check if message starts with the prefix.
  if(strncmp(msg->text, prefix, pfx_len) != 0)
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(FAIL);
  }

  // Parse command name (characters after prefix, up to whitespace).
  start = msg->text + pfx_len;

  if(*start == '\0' || *start == ' ' || *start == '\t')
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(FAIL);
  }

  while(start[i] != '\0' && start[i] != ' ' && start[i] != '\t'
      && i < CMD_NAME_SZ - 1)
  {
    cmd_name[i] = start[i];
    i++;
  }

  // Find the arguments (skip whitespace after command name).
  args = start + i;

  while(*args == ' ' || *args == '\t')
    args++;

  // Look up the command definition. Skip commands that are
  // subcommands — they are only reachable through their parent.
  d = def_find_locked(cmd_name);

  if(d == NULL || d->parent != NULL)
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(FAIL);
  }

  // Resolve subcommand if the matched command has children.
  resolve_subcmd_locked(&d, &args);

  // Check for "help" trailing keyword — redirect to help system.
  {
    const char *ha = args;
    while(*ha == ' ' || *ha == '\t') ha++;
    if(strncasecmp(ha, "help", 4) == 0
        && (ha[4] == '\0' || ha[4] == ' ' || ha[4] == '\t'))
    {
      // Build the resolved command path for the help system.
      // Walk up the parent chain to build the path.
      char help_args[METHOD_TEXT_SZ] = {0};
      char *parts[16];
      int depth = 0;
      size_t off = 0;
      cmd_def_t *help_cmd;

      for(cmd_def_t *p = d; p != NULL && depth < 16; p = p->parent)
        parts[depth++] = p->name;

      for(int pi = depth - 1; pi >= 0; pi--)
      {
        size_t nlen = strlen(parts[pi]);
        if(off + nlen + 1 < sizeof(help_args))
        {
          if(off > 0)
            help_args[off++] = ' ';
          memcpy(help_args + off, parts[pi], nlen);
          off += nlen;
        }
      }
      help_args[off] = '\0';

      // Find the help command and dispatch to it.
      help_cmd = def_find_locked("help");
      if(help_cmd != NULL && help_cmd->cb != NULL)
      {
        cmd_cb_t help_cb = help_cmd->cb;
        cmd_ctx_t help_ctx;

        pthread_mutex_unlock(&cmd_mutex);

        // Build a synthetic context with the path as args.
        help_ctx = (cmd_ctx_t){
          .bot = inst,
          .msg = msg,
          .args = help_args,
          .username = NULL,
          .parsed = NULL,
          .data = help_cmd->data,
        };

        help_cb(&help_ctx);
        __atomic_fetch_add(&cmd_stat_dispatches, 1, __ATOMIC_RELAXED);
        bot_inc_cmd_count(inst);
        return(SUCCESS);
      }
      pthread_mutex_unlock(&cmd_mutex);
      return(FAIL);
    }
  }

  // Method type check: reject if the command is not visible on this method.
  inst_type = method_inst_type(msg->inst);

  if(inst_type != 0 && !(d->methods & inst_type))
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(FAIL);
  }

  // Copy what we need from the definition before releasing the lock.
  cb = d->cb;
  cb_data = d->data;
  req_level = d->level;
  scope = d->scope;
  memset(group, 0, sizeof(group));
  memcpy(group, d->group, USERNS_GROUP_SZ);
  arg_desc = d->arg_desc;
  arg_count = d->arg_count;
  usage = d->usage;

  pthread_mutex_unlock(&cmd_mutex);

  // Permission checking (done outside lock).
  username = bot_identity_resolve(inst, msg->inst, msg->sender,
      msg->metadata, ubuf, sizeof(ubuf)) ? ubuf : NULL;

  // Scope enforcement: reject commands used in the wrong context.
  // Private-only commands must not be used in public channels, and
  // public-only commands must not be used in private messages.
  if(scope != CMD_SCOPE_ANY && msg->inst != NULL)
  {
    bool is_public = (msg->channel[0] != '\0');

    if(scope == CMD_SCOPE_PRIVATE && is_public)
    {
      const char *target = msg->channel;

      clam(CLAM_WARN, "cmd_dispatch",
          "'%s': denied '%s': reason=private_only "
          "[sender=%s source=%s]",
          bot_inst_name(inst), cmd_name,
          msg->sender, target);

      method_send(msg->inst, target,
          "This command can only be used in private messages.");
      __atomic_add_fetch(&cmd_stat_denials, 1, __ATOMIC_RELAXED);
      return(FAIL);
    }

    if(scope == CMD_SCOPE_PUBLIC && !is_public)
    {
      clam(CLAM_WARN, "cmd_dispatch",
          "'%s': denied '%s': reason=public_only "
          "[sender=%s source=private]",
          bot_inst_name(inst), cmd_name, msg->sender);

      method_send(msg->inst, msg->sender,
          "This command can only be used in public channels.");
      __atomic_add_fetch(&cmd_stat_denials, 1, __ATOMIC_RELAXED);
      return(FAIL);
    }
  }

  // Unified permission check: caller must be a member of the required
  // group at >= required level. Authenticated callers resolve through
  // the bot's userns; unauthenticated callers are implicit members of
  // "everyone" at level 0. The owner principal has no special path —
  // @owner passes because userns instantiation seeds membership in all
  // default groups at max level.
  ns_for_check = (username != NULL) ? bot_get_userns(inst) : NULL;
  if(!check_permission(ns_for_check, username, group, req_level))
  {
    const char *reason;
    if(username == NULL)                  reason = "not_authenticated";
    else if(ns_for_check == NULL)         reason = "no_namespace";
    else                                  reason = "not_in_group_or_level";

    clam(CLAM_WARN, "cmd_dispatch",
        "'%s': denied '%s': reason=%s "
        "(group '%s' need level %u) "
        "[sender=%s user=%s host=%s source=%s]",
        bot_inst_name(inst), cmd_name, reason, group,
        (unsigned)req_level,
        msg->sender, username != NULL ? username : "(anon)",
        msg->metadata[0] != '\0' ? msg->metadata : "n/a",
        msg->channel[0] != '\0' ? msg->channel : "private");

    if(msg->inst != NULL)
    {
      const char *target = msg->channel[0] != '\0'
          ? msg->channel : msg->sender;
      method_send(msg->inst, target, "Permission denied.");
    }

    __atomic_add_fetch(&cmd_stat_denials, 1, __ATOMIC_RELAXED);
    return(FAIL);
  }

  // Build task data.
  td = mem_alloc("cmd", "task_data", sizeof(*td));
  memset(td, 0, sizeof(*td));
  td->cb = cb;
  td->cb_data = cb_data;
  td->bot = inst;
  memcpy(&td->msg, msg, sizeof(method_msg_t));
  method_hold(td->msg.inst);
  strlcpy(td->args, args, METHOD_TEXT_SZ);

  // td is zeroed above, so copying just the characters terminates the
  // field for free — and skips strncpy's zero-fill of the whole buffer
  // on a path every command takes.
  if(username != NULL)
    memcpy(td->username, username, strnlen(username, USERNS_USER_SZ - 1));

  td->arg_desc  = arg_desc;
  td->arg_count = arg_count;
  td->usage     = usage;
  strlcpy(td->name, cmd_name, sizeof(td->name));

  // Submit task.
  snprintf(task_name, sizeof(task_name), "cmd:%s", cmd_name);

  t = task_add(task_name, TASK_THREAD, 128, cmd_task_cb, td);

  if(t == NULL)
  {
    clam(CLAM_WARN, "cmd_dispatch",
        "'%s': failed to submit task for '%s'",
        bot_inst_name(inst), cmd_name);
    cmd_task_data_free(td);
    return(FAIL);
  }

  __atomic_add_fetch(&cmd_stat_dispatches, 1, __ATOMIC_RELAXED);

  // Per-bot mirror of the process-wide counter above. Counted here, at
  // the point of actual dispatch, so denials and unknown verbs stay out
  // of it — core's deliver path can't make this call because only the
  // cmd layer knows a line became a command.
  bot_inc_cmd_count(inst);

  clam(CLAM_DEBUG, "cmd_dispatch",
      "'%s': dispatched '%s' from %s on %s",
      bot_inst_name(inst), cmd_name,
      msg->sender, method_inst_name(msg->inst));
  return(SUCCESS);
}

// Reply sinks

uint64_t
cmd_sink_register(cmd_sink_fn_t fn, void *data)
{
  cmd_sink_t *s;
  uint64_t    id;

  if(fn == NULL)
    return(0);

  s = mem_alloc("cmd", "sink", sizeof(*s));

  pthread_mutex_lock(&cmd_sink_mutex);
  id        = cmd_sink_next_id++;
  s->id     = id;
  s->fn     = fn;
  s->data   = data;
  s->next   = cmd_sinks;
  cmd_sinks = s;
  pthread_mutex_unlock(&cmd_sink_mutex);

  return(id);
}

void
cmd_sink_unregister(uint64_t id)
{
  cmd_sink_t *dead = NULL;

  if(id == 0)
    return;

  // Same window as cmd_unregister_path: a plugin retracts its sinks
  // from stop() (main.c step 2), by which point cmd_exit() has already
  // freed the registry and destroyed cmd_sink_mutex.
  if(!cmd_ready)
    return;

  pthread_mutex_lock(&cmd_sink_mutex);

  for(cmd_sink_t **pp = &cmd_sinks; *pp != NULL; pp = &(*pp)->next)
  {
    if((*pp)->id == id)
    {
      dead = *pp;
      *pp  = dead->next;
      break;
    }
  }

  pthread_mutex_unlock(&cmd_sink_mutex);

  if(dead != NULL)
    mem_free(dead);
}

bool
cmd_sink_deliver(uint64_t id, const char *line)
{
  bool diverted = false;

  if(id == 0 || line == NULL)
    return(false);

  pthread_mutex_lock(&cmd_sink_mutex);

  for(cmd_sink_t *s = cmd_sinks; s != NULL; s = s->next)
  {
    if(s->id == id)
    {
      // Invoked under the lock on purpose: unregistration cannot
      // complete while a delivery is inside the callback, so after
      // cmd_sink_unregister returns the owner may tear down whatever
      // `data` points at with no callback in flight.
      s->fn(s->data, line);
      diverted = true;
      break;
    }
  }

  pthread_mutex_unlock(&cmd_sink_mutex);

  return(diverted);
}

// Reply helper

// A reply may be minutes younger than the message that asked for it: an
// async command copies the whole method_msg_t and answers once its HTTP
// round trip lands, by which time a `/plugin reload irc` can have
// unregistered the instance the copy points at. So the send resolves by
// NAME (method.h §method_msg_t) — the copy holds no lifetime, and an
// instance that has gone away answers NULL here instead of a freed
// pointer's driver.
bool
cmd_reply(const cmd_ctx_t *ctx, const char *text)
{
  const char    *target;
  method_inst_t *inst;
  bool           rc;

  if(ctx == NULL || text == NULL)
    return(FAIL);

  if(ctx->msg == NULL || ctx->msg->inst_name[0] == '\0')
    return(FAIL);

  // Reply-sink divert: a registered collector owns this command's
  // output. A stale id (owner already unregistered) falls through to
  // the wire, so a collector torn down mid-flight degrades to normal
  // delivery rather than silence.
  if(ctx->msg->reply_sink_id != 0
      && cmd_sink_deliver(ctx->msg->reply_sink_id, text))
    return(SUCCESS);

  inst = method_find(ctx->msg->inst_name);

  if(inst == NULL)
    return(FAIL);

  // A driver-private route outranks both, and only a driver that
  // serves more than one session ever sets one: it is the difference
  // between naming the session that asked and naming whoever the
  // driver is serving at the instant the answer exists.
  if(ctx->msg->reply_route[0] != '\0')
    target = ctx->msg->reply_route;

  else if(ctx->msg->channel[0] != '\0')
    target = ctx->msg->channel;

  else
    target = ctx->msg->sender;

  rc = method_send(inst, target, text);

  method_release(inst);

  return(rc);
}

// Built-in commands

// Permission check: can the caller see this command in help listings?
static bool
help_check_access(const cmd_ctx_t *ctx, const cmd_def_t *d)
{
  // Method type filter.
  method_type_t mt = (ctx->msg != NULL && ctx->msg->inst != NULL)
      ? method_inst_type(ctx->msg->inst) : METHOD_T_ANY;
  if(mt != 0 && !(d->methods & mt))
    return(false);

  {
    userns_t *ns = (ctx->bot != NULL) ? bot_get_userns(ctx->bot) : NULL;
    return(check_permission(ns, ctx->username, d->group, d->level));
  }
}

// Print child table for a command. Returns number of children shown.
static uint32_t
help_show_children(const cmd_ctx_t *ctx, const cmd_def_t *d)
{
  uint32_t count = 0;
  bool header_sent = false;

  pthread_mutex_lock(&cmd_mutex);
  for(cmd_def_t *c = d->children; c != NULL; c = c->sibling)
  {
    if(!help_check_access(ctx, c))
      continue;

    {
    const char *cname = c->name;
    const char *cabbrev = (c->abbrev[0] != '\0') ? c->abbrev : "-";
    const char *cdesc = c->description ? c->description : "";
    char line[256];

    if(!header_sent)
    {
      snprintf(line, sizeof(line), "  %-20s %-12s %s",
          "COMMAND", "ABBREV", "DESCRIPTION");
      pthread_mutex_unlock(&cmd_mutex);
      cmd_reply(ctx, line);
      pthread_mutex_lock(&cmd_mutex);
      header_sent = true;
    }

    snprintf(line, sizeof(line), "  %-20s %-12s %s", cname, cabbrev, cdesc);
    pthread_mutex_unlock(&cmd_mutex);
    cmd_reply(ctx, line);
    pthread_mutex_lock(&cmd_mutex);
    count++;
    }
  }
  pthread_mutex_unlock(&cmd_mutex);

  return count;
}

// /help kv [name] — display help text for a KV configuration key.
static void
cmd_help_kv(const cmd_ctx_t *ctx, const char *name)
{
  const char *type_name;
  const char *help;
  char val_buf[KV_STR_SZ];
  char line[512];

  if(name[0] == '\0')
  {
    cmd_reply(ctx, "usage: /help kv <key>");
    cmd_reply(ctx, "Show the description and type of a configuration key.");
    return;
  }

  type_name = kv_get_type_name(name);

  if(type_name == NULL)
  {
    char buf[KV_KEY_SZ + 32];
    snprintf(buf, sizeof(buf), "unknown key: %s", name);
    cmd_reply(ctx, buf);
    return;
  }

  help = kv_get_help(name);

  snprintf(line, sizeof(line), "key:  %s", name);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line), "type: %s", type_name);
  cmd_reply(ctx, line);

  if(kv_get_val_str(name, val_buf, sizeof(val_buf)) == SUCCESS)
  {
    snprintf(line, sizeof(line), "val:  %s", val_buf);
    cmd_reply(ctx, line);
  }

  cmd_reply(ctx, "");

  if(help != NULL && help[0] != '\0')
  {
    // Send help text line-by-line (split on newlines).
    size_t len = strlen(help);
    char *buf2 = mem_alloc("cmd", "kv_help", len + 1);
    char *text = buf2;
    char *nl;

    memcpy(buf2, help, len + 1);

    while((nl = strchr(text, '\n')) != NULL)
    {
      *nl = '\0';
      cmd_reply(ctx, text);
      text = nl + 1;
    }

    if(*text != '\0')
      cmd_reply(ctx, text);

    mem_free(buf2);
  }

  else
    cmd_reply(ctx, "(no description available)");
}

// Built-in: help -- list all available commands, or show verbose help
// for a specific command.
//
// Usage:
//   /help            -- list all root commands
//   /help <command>  -- show usage and subcommands
//   /help -v <cmd>   -- verbose help (description + help_long)
//   /help kv <key>   -- show help for a KV configuration key
static void
cmd_builtin_help(const cmd_ctx_t *ctx)
{
  const char *args;
  const char *hp;
  const char *rest;
  cmd_def_t *d;
  cmd_help_extender_t ext;
  const char *usage;
  const char *description;
  const char *help_long;
  bool has_children;
  char cmd_path[CMD_USAGE_SZ] = {0};
  size_t hi = 0;
  bool verbose = false;

  // No arguments: list all root commands.
  if(ctx->args == NULL || ctx->args[0] == '\0')
  {
    char hdr[256];
    char count_line[32];
    uint32_t count = 0;

    cmd_reply(ctx, "Available commands:");

    snprintf(hdr, sizeof(hdr), "  %-20s %-12s %s",
        "COMMAND", "ABBREV", "DESCRIPTION");
    cmd_reply(ctx, hdr);

    pthread_mutex_lock(&cmd_mutex);
    for(cmd_def_t *d = cmd_list; d != NULL; d = d->next)
    {
      const char *name;
      const char *abbrev;
      const char *desc;
      char line[256];

      if(d->parent != NULL)
        continue;
      if(!help_check_access(ctx, d))
        continue;

      name = d->name;
      abbrev = (d->abbrev[0] != '\0') ? d->abbrev : "-";
      desc = d->description ? d->description : "";
      snprintf(line, sizeof(line), "  %-20s %-12s %s", name, abbrev, desc);

      pthread_mutex_unlock(&cmd_mutex);
      cmd_reply(ctx, line);
      pthread_mutex_lock(&cmd_mutex);
      count++;
    }
    pthread_mutex_unlock(&cmd_mutex);

    snprintf(count_line, sizeof(count_line), "%u command(s)", count);
    cmd_reply(ctx, count_line);
    cmd_reply(ctx, "Use /help <command> for detailed information.");
    return;
  }

  // Parse -v flag.
  args = ctx->args;

  if(strncmp(args, "-v ", 3) == 0 || strncmp(args, "-v\t", 3) == 0)
  {
    verbose = true;
    args += 3;
    while(*args == ' ' || *args == '\t')
      args++;
  }

  else if(strcmp(args, "-v") == 0)
  {
    // -v with no command argument: error.
    cmd_reply(ctx, "usage: /help [-v] [command ...]");
    return;
  }

  // Special case: /help kv <name> — show KV help text.
  if(strncasecmp(args, "kv", 2) == 0
      && (args[2] == '\0' || args[2] == ' ' || args[2] == '\t'))
  {
    const char *kv_args = args + 2;

    while(*kv_args == ' ' || *kv_args == '\t')
      kv_args++;

    cmd_help_kv(ctx, kv_args);
    return;
  }

  // Resolve command path (e.g., "bot add" -> root "bot", child "add").
  hp = args;

  while(hp[hi] != '\0' && hp[hi] != ' ' && hp[hi] != '\t'
      && hi < CMD_NAME_SZ - 1)
  {
    cmd_path[hi] = hp[hi];
    hi++;
  }

  rest = hp + hi;
  while(*rest == ' ' || *rest == '\t')
    rest++;

  pthread_mutex_lock(&cmd_mutex);
  d = def_find_locked(cmd_path);

  if(d == NULL || d->parent != NULL)
  {
    char buf[CMD_NAME_SZ + 32];
    pthread_mutex_unlock(&cmd_mutex);
    snprintf(buf, sizeof(buf), "unknown command: %s", args);
    cmd_reply(ctx, buf);
    return;
  }

  // Resolve subcommand path, building full path string.
  while(d->children != NULL && rest[0] != '\0')
  {
    char tok[CMD_NAME_SZ] = {0};
    size_t ti = 0;
    cmd_def_t *child;
    size_t plen;

    while(rest[ti] != '\0' && rest[ti] != ' ' && rest[ti] != '\t'
        && ti < CMD_NAME_SZ - 1)
    {
      tok[ti] = rest[ti];
      ti++;
    }

    child = def_find_child_locked(d, tok);
    if(child == NULL)
      break;

    plen = strlen(cmd_path);
    if(plen + 1 + ti < sizeof(cmd_path))
    {
      cmd_path[plen] = ' ';
      memcpy(cmd_path + plen + 1, tok, ti);
      cmd_path[plen + 1 + ti] = '\0';
    }

    d = child;
    rest += ti;
    while(*rest == ' ' || *rest == '\t')
      rest++;
  }

  // The listings omit a command the caller may not run; the detail view
  // must answer the same way, or /help <cmd> reports the existence,
  // usage and subcommand tree of everything it hides. Unknown and
  // forbidden read alike on purpose.
  if(!help_check_access(ctx, d))
  {
    char buf[CMD_NAME_SZ + 32];
    pthread_mutex_unlock(&cmd_mutex);
    snprintf(buf, sizeof(buf), "unknown command: %s", args);
    cmd_reply(ctx, buf);
    return;
  }

  // If the command has a help extender, delegate to it. This handles
  // both trailing tokens (e.g. /help show bot pacman llm personas)
  // and bare invocations (e.g. /help show bot) where the extender
  // can list dynamic verbs that aren't in the static child tree.
  ext = d->help_ext;
  if(ext != NULL)
  {
    const char *usage_copy = d->usage;
    pthread_mutex_unlock(&cmd_mutex);

    if(usage_copy != NULL && usage_copy[0] != '\0')
    {
      char line[CMD_USAGE_SZ + 16];
      snprintf(line, sizeof(line), "usage: /%s", usage_copy);
      cmd_reply(ctx, line);
    }

    ext(ctx, rest);
    return;
  }

  // Copy fields while holding the lock.
  usage = d->usage;
  description = d->description;
  help_long = d->help_long;
  has_children = (d->children != NULL);

  pthread_mutex_unlock(&cmd_mutex);

  // Always show usage line.
  if(usage != NULL && usage[0] != '\0')
  {
    char line[CMD_USAGE_SZ + 16];
    snprintf(line, sizeof(line), "usage: /%s", usage);
    cmd_reply(ctx, line);
  }

  if(verbose)
  {
    if(description != NULL && description[0] != '\0')
      cmd_reply(ctx, description);

    if(help_long != NULL && help_long[0] != '\0')
    {
      size_t len = strlen(help_long);
      char *buf = mem_alloc("cmd", "help_buf", len + 1);
      char *text = buf;
      char *nl;

      cmd_reply(ctx, "");
      // Send help_long line-by-line (split on newlines).
      memcpy(buf, help_long, len + 1);

      while((nl = strchr(text, '\n')) != NULL)
      {
        *nl = '\0';
        cmd_reply(ctx, text);
        text = nl + 1;
      }
      if(*text != '\0')
        cmd_reply(ctx, text);

      mem_free(buf);
    }
  }

  // Show child table.
  if(has_children)
  {
    char shdr[CMD_USAGE_SZ + 32];
    char count_buf[32];
    uint32_t child_count;

    cmd_reply(ctx, "");

    snprintf(shdr, sizeof(shdr), "subcommands of /%s:", cmd_path);
    cmd_reply(ctx, shdr);

    child_count = help_show_children(ctx, d);

    snprintf(count_buf, sizeof(count_buf), "%u subcommand(s)", child_count);
    cmd_reply(ctx, count_buf);
  }
}

// Built-in: version -- show program version.
static void
cmd_builtin_version(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, BM_VERSION_STR);
}

// Built-in: show -- container for /show subcommands.
static void
cmd_builtin_show(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /show <subcommand> ...");
}

// Built-in: set -- container for /set subcommands.
static void
cmd_builtin_set(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /set <subcommand> ...");
}

// Command definition accessors

const char *
cmd_get_module(const cmd_def_t *def)
{
  return(def != NULL ? def->module : NULL);
}

const char *
cmd_get_usage(const cmd_def_t *def)
{
  return(def != NULL ? def->usage : NULL);
}

const char *
cmd_get_description(const cmd_def_t *def)
{
  return(def != NULL ? def->description : NULL);
}

const char *
cmd_get_help_long(const cmd_def_t *def)
{
  return(def != NULL ? def->help_long : NULL);
}

const char *
cmd_get_name(const cmd_def_t *def)
{
  return(def != NULL ? def->name : NULL);
}

bool
cmd_has_children(const cmd_def_t *def)
{
  return(def != NULL && def->children != NULL);
}

bool
cmd_is_child(const cmd_def_t *def)
{
  return(def != NULL && def->parent != NULL);
}

const cmd_def_t *
cmd_get_parent(const cmd_def_t *def)
{
  return(def != NULL ? def->parent : NULL);
}

const char *
cmd_get_abbrev(const cmd_def_t *def)
{
  return(def != NULL ? def->abbrev : NULL);
}

// Get the required group for a command.
const char *
cmd_get_group(const cmd_def_t *def)
{
  return(def != NULL ? def->group : NULL);
}

// Get the required privilege level for a command.
uint16_t
cmd_get_level(const cmd_def_t *def)
{
  return(def != NULL ? def->level : 0);
}

method_type_t
cmd_get_methods(const cmd_def_t *def)
{
  return(def != NULL ? def->methods : METHOD_T_ANY);
}

cmd_scope_t
cmd_get_scope(const cmd_def_t *def)
{
  return(def != NULL ? def->scope : CMD_SCOPE_ANY);
}

const cmd_nl_t *
cmd_get_nl(const cmd_def_t *def)
{
  return(def != NULL ? def->nl : NULL);
}

// Direct invocation of a command's callback. Permission/scope/method
// checks are skipped; the caller is expected to have been authorized
// by the surrounding dispatcher.
//
// If the command declares an arg spec and the incoming ctx has no
// parsed args (i.e. came through a sub-dispatcher that left
// parsed=NULL), parse ctx->args here so leaf handlers can rely on
// ctx->parsed unconditionally -- matching cmd_task_cb and
// cmd_dispatch_as.
void
cmd_invoke(const cmd_def_t *def, const cmd_ctx_t *ctx)
{
  cmd_inflight_t node;

  if(def == NULL || def->cb == NULL || ctx == NULL)
    return;

  // The third way a plugin's handler runs, and the one that needs the
  // record most: the OUTER command here is core's (`/bot <name> <verb>`
  // in core/bot_cmd.c), so the enclosing dispatcher's node names core's
  // mapping and the leaf's would go uncounted.
  cmd_inflight_enter(&node, def->cb, def->name);

  if(def->arg_desc != NULL && def->arg_count > 0 && ctx->parsed == NULL)
  {
    cmd_args_t parsed;
    char       arg_bufs[CMD_MAX_ARGS][CMD_ARG_SZ];
    cmd_ctx_t  sub;

    if(cmd_parse_args(ctx->args, def->arg_desc, def->arg_count,
        arg_bufs, &parsed, ctx, def->usage))
    {
      sub = *ctx;
      sub.parsed = &parsed;
      (def->cb)(&sub);
    }
  }

  else
    (def->cb)(ctx);

  cmd_inflight_leave(&node);
}

// Command iteration

// Both iterators collect the nodes to visit while cmd_mutex is held and
// call back with it released. Two things force the release and one
// forces the snapshot.
//
// The release: a callback may re-enter cmd_iterate_children — /help's
// per-bot verb listing and the chat plugin's NL vocabulary builder both
// recurse — on a mutex that is not recursive, and it may cmd_reply(),
// which reaches method_send() -> clam().
//
// The snapshot: walking the list across that release instead, which is
// the shape this replaced, re-read `->next` out of a node a concurrent
// /plugin reload had already freed in the gap (cmd_reclaim_owned() and
// cmd_unregister_path() free cmd_list nodes under this same lock) and
// followed it. Copying the pointers first cannot leave the list, which
// bounds the exposure to the one node the callback is handed. Closing
// that last window means not handing out node pointers at all — a
// change to cmd_iter_cb_t's contract, not to this function.

// Call `cb` for each of `n` collected nodes with no lock held, then
// release the snapshot if it was heap-allocated.
static void
cmd_iterate_fanout(const cmd_def_t **snap, uint32_t n, const cmd_def_t **stack,
    cmd_iter_cb_t cb, void *data)
{
  for(uint32_t i = 0; i < n; i++)
    cb(snap[i], data);

  if(snap != stack)
    mem_free(snap);
}

// Nodes held on the stack before the snapshot spills to the heap. Sized
// past the tree's root count so the common walk allocates nothing.
#define CMD_ITER_STACK_MAX 64

void
cmd_iterate_root(cmd_iter_cb_t cb, void *data)
{
  const cmd_def_t  *stack[CMD_ITER_STACK_MAX];
  const cmd_def_t **snap  = stack;
  uint32_t          count = 0;
  uint32_t          n     = 0;

  if(cb == NULL)
    return;

  pthread_mutex_lock(&cmd_mutex);

  for(const cmd_def_t *d = cmd_list; d != NULL; d = d->next)
    if(d->parent == NULL)
      count++;

  if(count > CMD_ITER_STACK_MAX)
    snap = mem_alloc("cmd", "iter_snap", count * sizeof(*snap));

  for(const cmd_def_t *d = cmd_list; d != NULL && n < count; d = d->next)
    if(d->parent == NULL)
      snap[n++] = d;

  pthread_mutex_unlock(&cmd_mutex);

  cmd_iterate_fanout(snap, n, stack, cb, data);
}

void
cmd_iterate_children(const cmd_def_t *parent, cmd_iter_cb_t cb, void *data)
{
  const cmd_def_t  *stack[CMD_ITER_STACK_MAX];
  const cmd_def_t **snap  = stack;
  uint32_t          count = 0;
  uint32_t          n     = 0;

  if(parent == NULL || cb == NULL)
    return;

  pthread_mutex_lock(&cmd_mutex);

  for(const cmd_def_t *c = parent->children; c != NULL; c = c->sibling)
    count++;

  if(count > CMD_ITER_STACK_MAX)
    snap = mem_alloc("cmd", "iter_snap", count * sizeof(*snap));

  for(const cmd_def_t *c = parent->children; c != NULL && n < count;
      c = c->sibling)
    snap[n++] = c;

  pthread_mutex_unlock(&cmd_mutex);

  cmd_iterate_fanout(snap, n, stack, cb, data);
}

void
cmd_audit_iterate(cmd_audit_cb_t cb, void *data)
{
  char path[CMD_USAGE_SZ];

  if(cb == NULL)
    return;

  pthread_mutex_lock(&cmd_mutex);

  for(cmd_def_t *d = cmd_list; d != NULL; d = d->next)
  {
    def_path_locked(d, path, sizeof(path));

    cb(path, "cb",          fn_addr(&d->cb),       data);
    cb(path, "help_ext",    fn_addr(&d->help_ext), data);
    cb(path, "data",        d->data,               data);
    cb(path, "usage",       d->usage,              data);
    cb(path, "description", d->description,        data);
    cb(path, "help_long",   d->help_long,          data);
    cb(path, "arg_desc",    d->arg_desc,           data);
    cb(path, "kind_filter", d->kind_filter,        data);
    cb(path, "nl",          d->nl,                 data);
  }

  pthread_mutex_unlock(&cmd_mutex);
}

// Asserted-identity command dispatch

// Evaluate the group+level check. Returns true if allowed.
//
// The literal owner principal (USERNS_OWNER_USER) is always granted.
// It is the operator's emergency identity — asserted over botmanctl's
// unix socket, which is itself filesystem-gated to operator access.
// Bypassing the userns lookup here makes operator-only commands
// (currently /quit) a reliable kill switch: they cannot be denied by
// corrupt, half-seeded, or temporarily-unreachable userns state. Any
// principal wanting reduced authority can downgrade via the AS
// session command before dispatch.
static bool
check_permission(userns_t *ns, const char *username,
    const char *req_group, uint16_t req_level)
{
  if(username == NULL)
  {
    // Anonymous principal: implicit member of "everyone" at level 0.
    // The group name is the userns default; if the user renames their
    // anon-group they should adjust commands accordingly.
    if(strcmp(req_group, USERNS_GROUP_EVERYONE) == 0 && req_level == 0)
      return(true);
    return(false);
  }

  if(strcmp(username, USERNS_OWNER_USER) == 0)
    return(true);

  if(ns == NULL)
    return(false);

  {
    int32_t ulevel = userns_member_level(ns, username, req_group);
    return(ulevel >= 0 && (uint16_t)ulevel >= req_level);
  }
}

// Dispatch a command on a method instance asserting an explicit caller
// identity. Runs the normal permission formula against that identity
// (no bypasses). Executes synchronously.
bool
cmd_dispatch_as(const char *cmd_name, const char *args,
    method_inst_t *inst, userns_t *ns, const char *username,
    const char *reply_route)
{
  cmd_def_t *d;
  method_type_t inst_type;
  cmd_cb_t cb;
  const cmd_arg_desc_t *ad;
  uint8_t ac;
  const char *usage;
  uint16_t req_level;
  char req_group[USERNS_GROUP_SZ];
  method_msg_t msg;
  cmd_ctx_t ctx;
  cmd_args_t parsed;
  char arg_bufs[CMD_MAX_ARGS][CMD_ARG_SZ];
  cmd_inflight_t node;
  char cmd_leaf[CMD_NAME_SZ];

  if(cmd_name == NULL || cmd_name[0] == '\0')
    return(FAIL);

  if(args == NULL)
    args = "";

  if(inst == NULL)
    return(FAIL);

  pthread_mutex_lock(&cmd_mutex);

  d = def_find_locked(cmd_name);

  if(d == NULL || d->parent != NULL)
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(FAIL);
  }

  // Resolve subcommand if the matched command has children.
  resolve_subcmd_locked(&d, &args);

  // Method type check.
  inst_type = method_inst_type(inst);
  if(inst_type != 0 && !(d->methods & inst_type))
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(FAIL);
  }

  cb = d->cb;
  ad = d->arg_desc;
  ac = d->arg_count;
  usage = d->usage;
  req_level = d->level;
  memcpy(req_group, d->group, USERNS_GROUP_SZ);
  strlcpy(cmd_leaf, d->name, sizeof(cmd_leaf));

  pthread_mutex_unlock(&cmd_mutex);

  // Synthetic message (drives reply routing + logs).
  memset(&msg, 0, sizeof(msg));
  method_msg_bind(&msg, inst);
  snprintf(msg.sender, METHOD_SENDER_SZ, "%s",
      username != NULL ? username : "(anon)");
  msg.timestamp = time(NULL);

  if(reply_route != NULL)
    strlcpy(msg.reply_route, reply_route, sizeof(msg.reply_route));

  // Permission check — same formula as cmd_dispatch.
  if(!check_permission(ns, username, req_group, req_level))
  {
    clam(CLAM_WARN, "cmd_dispatch_as",
        "denied '%s': user='%s' req=(group='%s' level=%u)",
        cmd_name,
        username != NULL ? username : "(anon)",
        req_group, (unsigned)req_level);

    // Surface the denial on the originating method so interactive
    // tools (botmanctl, etc.) see the rejection.
    method_send(inst,
        msg.reply_route[0] != '\0' ? msg.reply_route : msg.sender,
        "Permission denied.");

    __atomic_add_fetch(&cmd_stat_denials, 1, __ATOMIC_RELAXED);
    return(SUCCESS);
  }

  ctx = (cmd_ctx_t){
    .bot      = NULL,
    .msg      = &msg,
    .args     = args,
    .username = username,
    .parsed   = NULL,
  };

  // From here on a pointer into the plugin's mapping is live on this
  // thread — and on this path there is no task for the barrier to find
  // even in principle, so the record is the only thing that names it.
  cmd_inflight_enter(&node, cb, cmd_leaf);

  if(ad != NULL && ac > 0)
  {
    memset(arg_bufs, 0, sizeof(arg_bufs));
    if(!cmd_parse_args(args, ad, ac, arg_bufs, &parsed, &ctx, usage))
    {
      cmd_inflight_leave(&node);
      return(SUCCESS);    // validation failed, error already sent
    }

    ctx.parsed = &parsed;
  }

  {
    bool admin = (ns != NULL && username != NULL && username[0] != '\0' &&
        userns_member_check(ns, username, USERNS_GROUP_ADMIN));
    bool creds = cmd_creds_visible(inst, admin);

    if(creds)
      kv_admin_context_set(true);

    cb(&ctx);

    if(creds)
      kv_admin_context_set(false);
  }

  cmd_inflight_leave(&node);

  // No bot_inc_cmd_count here, and that is not an oversight: this path
  // asserts a caller identity against a method instance with no bot
  // instance behind it (botmanctl, NL bridges), so there is nothing to
  // attribute the dispatch to. Process-wide counter only.
  __atomic_add_fetch(&cmd_stat_dispatches, 1, __ATOMIC_RELAXED);
  return(SUCCESS);
}

// Dispatch a pre-resolved command definition, bypassing the root-walk
// + subcommand resolution cmd_dispatch performs. Permission / scope /
// method-type checks are SKIPPED here — the caller is responsible for
// having authorized the call via cmd_permits (or equivalent) against
// the leaf's own perms. Submits a task to the worker pool so the
// command body runs off the caller's thread.
//
// Use case: the NL bridge needs to invoke a subcommand leaf whose
// intermediate parent (e.g. show/bot) is admin-gated but whose leaf
// (e.g. show bot <name> model) is intentionally everyone-gated. The
// generic subcommand walker cannot skip over the <name> positional,
// so cmd_dispatch stops at the admin-gated parent and denies. This
// entry point takes the already-resolved leaf and runs its callback
// directly, preserving the task-pool handoff so bounded blocking
// inside the command body stays off the caller's thread.
bool
cmd_dispatch_resolved(bot_inst_t *inst, const method_msg_t *msg,
    const cmd_def_t *def, const char *args)
{
  cmd_task_data_t *td;
  task_t          *t;
  char             task_name[CMD_NAME_SZ + 8];
  const char      *username;
  char             ubuf[USERNS_USER_SZ];

  if(inst == NULL || msg == NULL || def == NULL || def->cb == NULL)
    return(FAIL);

  username = bot_identity_resolve(inst, msg->inst, msg->sender,
      msg->metadata, ubuf, sizeof(ubuf)) ? ubuf : NULL;

  td = mem_alloc("cmd", "task_data", sizeof(*td));
  memset(td, 0, sizeof(*td));
  td->cb        = def->cb;
  td->cb_data   = def->data;
  td->bot       = inst;
  memcpy(&td->msg, msg, sizeof(method_msg_t));
  method_hold(td->msg.inst);

  if(args != NULL)
    strlcpy(td->args, args, METHOD_TEXT_SZ);

  // See cmd_dispatch: td is zeroed, so the characters alone are enough.
  if(username != NULL)
    memcpy(td->username, username, strnlen(username, USERNS_USER_SZ - 1));

  td->arg_desc  = def->arg_desc;
  td->arg_count = def->arg_count;
  td->usage     = def->usage;
  strlcpy(td->name, def->name, sizeof(td->name));

  snprintf(task_name, sizeof(task_name), "cmd:%s", def->name);

  t = task_add(task_name, TASK_THREAD, 128, cmd_task_cb, td);

  if(t == NULL)
  {
    clam(CLAM_WARN, "cmd_dispatch_resolved",
        "'%s': failed to submit task for '%s'",
        bot_inst_name(inst), def->name);
    cmd_task_data_free(td);
    return(FAIL);
  }

  __atomic_add_fetch(&cmd_stat_dispatches, 1, __ATOMIC_RELAXED);
  bot_inc_cmd_count(inst);

  clam(CLAM_DEBUG, "cmd_dispatch_resolved",
      "'%s': dispatched leaf '%s' from %s on %s",
      bot_inst_name(inst), def->name,
      msg->sender, method_inst_name(msg->inst));
  return(SUCCESS);
}

// Subsystem lifecycle

// NL hint for /help. K2: pass-through.
static const cmd_nl_slot_t help_slots[] = {
  { .name  = "verb",
    .type  = CMD_NL_ARG_FREE,
    .flags = CMD_NL_SLOT_OPTIONAL | CMD_NL_SLOT_REMAINDER },
};

static const cmd_nl_example_t help_examples[] = {
  { .utterance  = "how do I use weather?",
    .invocation = "/help weather" },
  { .utterance  = "what does the dice command do?",
    .invocation = "/help dice" },
};

static const cmd_nl_t help_nl = {
  .when          = "User asks how to use a specific command or verb.",
  .syntax        = "/help [<verb>]",
  .slots         = help_slots,
  .slot_count    = (uint8_t)(sizeof(help_slots) / sizeof(help_slots[0])),
  .examples      = help_examples,
  .example_count = (uint8_t)(sizeof(help_examples) / sizeof(help_examples[0])),
};

static const cmd_nl_example_t version_examples[] = {
  { .utterance  = "what version are you?",
    .invocation = "/version" },
  { .utterance  = "what build of botmanager is this?",
    .invocation = "/version" },
};

static const cmd_nl_t version_nl = {
  .when          = "User asks which version, build, or release of the "
                   "botmanager software is running.",
  .syntax        = "/version",
  .slots         = NULL,
  .slot_count    = 0,
  .examples      = version_examples,
  .example_count = (uint8_t)(sizeof(version_examples)
                             / sizeof(version_examples[0])),
};

// Initialize the command subsystem. Sets up the mutex and registers
// built-in commands (help, version).
void
cmd_init(void)
{
  pthread_mutex_init(&cmd_mutex, NULL);
  pthread_mutex_init(&cmd_sink_mutex, NULL);
  pthread_mutex_init(&cmd_inflight_mutex, NULL);

  // Register core built-in commands.
  cmd_register("cmd", "help",
      "help [-v] [command ...] | help kv <key>",
      "Command reference",
      "Lists all commands available on this bot instance.\n"
      "Use /help <command> to see usage and subcommands.\n"
      "Use /help -v <command> for verbose help.\n"
      "Use /help kv <key> for configuration key help.",
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_builtin_help, NULL, NULL, "h", NULL, 0, NULL, &help_nl);

  cmd_register("cmd", "show", "show <subcommand> ...",
      "Show system information", NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_builtin_show, NULL, NULL, "sh", NULL, 0, NULL, NULL);

  cmd_register("cmd", "set", "set <subcommand> ...",
      "Configure system settings", NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_builtin_set, NULL, NULL, NULL, NULL, 0, NULL, NULL);

  cmd_register("cmd", "version", "version",
      "Show program version", NULL,
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_builtin_version, NULL, NULL, NULL, NULL, 0, NULL, &version_nl);

  cmd_ready = true;

  clam(CLAM_INFO, "cmd_init",
      "command subsystem initialized (%u commands)", cmd_def_count);
}

// Get lifetime command dispatch counters (thread-safe, atomic reads).
// Get command subsystem statistics (thread-safe snapshot).
void
cmd_get_stats(cmd_stats_t *out)
{
  if(out == NULL)
    return;

  pthread_mutex_lock(&cmd_mutex);
  out->registered = cmd_def_count;
  pthread_mutex_unlock(&cmd_mutex);

  out->dispatches = __atomic_load_n(&cmd_stat_dispatches, __ATOMIC_RELAXED);
  out->denials    = __atomic_load_n(&cmd_stat_denials, __ATOMIC_RELAXED);
}

void
cmd_get_dispatch_stats(uint64_t *dispatches, uint64_t *denials)
{
  if(dispatches != NULL)
    *dispatches = __atomic_load_n(&cmd_stat_dispatches, __ATOMIC_RELAXED);

  if(denials != NULL)
    *denials = __atomic_load_n(&cmd_stat_denials, __ATOMIC_RELAXED);
}

// Shut down the command subsystem. Frees all command definitions,
// per-bot bindings, and freelists. Destroys the mutex.
void
cmd_exit(void)
{
  uint32_t set_count = 0;
  cmd_set_t *s;
  cmd_def_t *d;
  cmd_set_t *sf;

  if(!cmd_ready)
    return;

  for(cmd_set_t *sc = cmd_sets; sc != NULL; sc = sc->next)
    set_count++;

  clam(CLAM_INFO, "cmd_exit",
      "shutting down (%u commands, %u bot sets, %u freelisted sets)",
      cmd_def_count, set_count, cmd_set_free_count);

  cmd_ready = false;

  // Free all per-bot command sets.
  s = cmd_sets;

  while(s != NULL)
  {
    cmd_set_t *snext = s->next;
    mem_free(s);
    s = snext;
  }

  cmd_sets = NULL;

  // Free all command definitions.
  d = cmd_list;

  while(d != NULL)
  {
    cmd_def_t *dnext = d->next;
    mem_free(d);
    d = dnext;
  }

  cmd_list = NULL;
  cmd_def_count = 0;

  // Free set freelist.
  sf = cmd_set_freelist;

  while(sf != NULL)
  {
    cmd_set_t *sfnext = sf->next;
    mem_free(sf);
    sf = sfnext;
  }

  cmd_set_freelist = NULL;
  cmd_set_free_count = 0;

  // Any sink still registered here is a plugin that failed to retract
  // in stop() — its callback pointer is (or is about to be) dead. Worth
  // a WARN because the same leak during a reload, rather than shutdown,
  // would have been a crash on the next delivery.
  {
    uint32_t leaked = 0;
    cmd_sink_t *s = cmd_sinks;

    while(s != NULL)
    {
      cmd_sink_t *snext = s->next;

      leaked++;
      mem_free(s);
      s = snext;
    }

    cmd_sinks = NULL;

    if(leaked > 0)
      clam(CLAM_WARN, "cmd_exit",
          "%u reply sink(s) still registered at shutdown", leaked);
  }

  // Nothing to free — every node is a live stack frame — but a
  // non-empty list means a handler outlived the subsystem, which is
  // the one thing this accounting exists to make impossible.
  {
    uint32_t running = 0;

    pthread_mutex_lock(&cmd_inflight_mutex);

    for(const cmd_inflight_t *c = cmd_inflight_list; c != NULL; c = c->next)
      running++;

    pthread_mutex_unlock(&cmd_inflight_mutex);

    if(running > 0)
      clam(CLAM_WARN, "cmd_exit",
          "%u command handler(s) still executing at shutdown", running);
  }

  pthread_mutex_destroy(&cmd_mutex);
  pthread_mutex_destroy(&cmd_sink_mutex);
  pthread_mutex_destroy(&cmd_inflight_mutex);
}
