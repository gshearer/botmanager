#define CMD_INTERNAL
#include "cmd.h"

// -----------------------------------------------------------------------
// Binding freelist management.
// -----------------------------------------------------------------------

// Get a binding struct from the freelist or allocate a new one.
// returns: zeroed struct
static cmd_binding_t *
bind_get(void)
{
  cmd_binding_t *b;

  if(cmd_bind_freelist != NULL)
  {
    b = cmd_bind_freelist;
    cmd_bind_freelist = b->next;
    cmd_bind_free_count--;
    memset(b, 0, sizeof(*b));
    return(b);
  }

  b = mem_alloc("cmd", "binding", sizeof(*b));
  memset(b, 0, sizeof(*b));
  return(b);
}

// Return a binding struct to the freelist.
// b: struct to recycle
static void
bind_put(cmd_binding_t *b)
{
  b->next = cmd_bind_freelist;
  cmd_bind_freelist = b;
  cmd_bind_free_count++;
}

// -----------------------------------------------------------------------
// Command set freelist management.
// -----------------------------------------------------------------------

// Get a cmd_set struct from the freelist or allocate a new one.
// returns: zeroed struct with default prefix "!"
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

  strncpy(s->prefix, "!", CMD_PREFIX_SZ - 1);
  return(s);
}

// Return a cmd_set struct to the freelist.
// s: struct to recycle
static void
set_put(cmd_set_t *s)
{
  s->next = cmd_set_freelist;
  cmd_set_freelist = s;
  cmd_set_free_count++;
}

// -----------------------------------------------------------------------
// Internal: find or create the cmd_set for a bot instance.
// Caller must hold cmd_mutex.
// -----------------------------------------------------------------------

// Find the cmd_set for an instance. Returns NULL if none exists.
static cmd_set_t *
set_find_locked(const bot_inst_t *inst)
{
  for(cmd_set_t *s = cmd_sets; s != NULL; s = s->next)
  {
    if(s->inst == inst)
      return(s);
  }

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

// -----------------------------------------------------------------------
// Internal: find a command definition by name or abbreviation.
// Exact name match takes priority, then abbreviation match.
// Caller must hold cmd_mutex.
// -----------------------------------------------------------------------
static cmd_def_t *
def_find_locked(const char *name)
{
  cmd_def_t *name_match   = NULL;
  cmd_def_t *abbrev_match = NULL;

  for(cmd_def_t *d = cmd_list; d != NULL; d = d->next)
  {
    if(strncasecmp(d->name, name, CMD_NAME_SZ) == 0)
    {
      // System commands take priority over built-ins on name collision.
      if(d->system)
        return(d);

      if(name_match == NULL)
        name_match = d;
    }

    if(abbrev_match == NULL && d->abbrev[0] != '\0'
        && strncasecmp(d->abbrev, name, CMD_NAME_SZ) == 0)
      abbrev_match = d;
  }

  return(name_match != NULL ? name_match : abbrev_match);
}

// Internal: check if a string collides with any existing command name
// or abbreviation within the same scope. Root commands (parent==NULL)
// only collide with other root commands; subcommands only collide with
// siblings under the same parent.
// Caller must hold cmd_mutex.
// returns: true if collision found
static bool
def_name_collides_locked(const char *str, cmd_def_t *parent)
{
  for(cmd_def_t *d = cmd_list; d != NULL; d = d->next)
  {
    if(d->parent != parent)
      continue;

    if(strncasecmp(d->name, str, CMD_NAME_SZ) == 0)
      return(true);

    if(d->abbrev[0] != '\0'
        && strncasecmp(d->abbrev, str, CMD_NAME_SZ) == 0)
      return(true);
  }

  return(false);
}

// -----------------------------------------------------------------------
// Global command registration
// -----------------------------------------------------------------------

// Validate an argument descriptor array for a command being registered.
// name: command name (for error messages)
// arg_desc: argument descriptor array
// arg_count: number of descriptors
// returns: true if valid, false on error (logs warning)
static bool
reg_validate_args(const char *name, const cmd_arg_desc_t *arg_desc,
    uint8_t arg_count)
{
  if(arg_count > CMD_MAX_ARGS)
  {
    clam(CLAM_WARN, "cmd_register",
        "'%s': too many arg descriptors (%u, max %u)",
        name, (unsigned)arg_count, CMD_MAX_ARGS);
    return(false);
  }

  bool seen_optional = false;

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
// name: command name
// abbrev: abbreviation (NULL or empty for none)
// parent: parent command (NULL for root-level)
// is_system: whether this is a system command
// returns: true if no collision (ok to proceed), false on collision
static bool
reg_check_collisions_locked(const char *name, const char *abbrev,
    cmd_def_t *parent, bool is_system)
{
  // Check name collision within the same scope (siblings or root).
  // System commands are allowed to shadow built-in commands of the
  // same name -- the built-in remains for per-bot dispatch while the
  // system command takes priority in owner/system dispatch paths.
  if(def_name_collides_locked(name, parent))
  {
    // Allow system command to coexist with a same-named built-in.
    bool shadow_ok = false;

    if(is_system && parent == NULL)
    {
      for(cmd_def_t *e = cmd_list; e != NULL; e = e->next)
      {
        if(e->parent == NULL
            && strncasecmp(e->name, name, CMD_NAME_SZ) == 0
            && e->builtin)
        {
          shadow_ok = true;
          break;
        }
      }
    }

    if(!shadow_ok)
    {
      clam(CLAM_WARN, "cmd_register", "duplicate command: '%s'", name);
      return(false);
    }
  }

  // Check abbreviation collision within the same scope.
  if(abbrev != NULL && abbrev[0] != '\0')
  {
    if(def_name_collides_locked(abbrev, parent))
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
// parent: resolved parent command (NULL for root-level)
// returns: the new definition (already linked)
static cmd_def_t *
reg_populate_def(const char *module, const char *name,
    const char *usage, const char *help,
    const char *help_long, const char *group, uint16_t level,
    cmd_scope_t scope, method_type_t methods, cmd_cb_t cb, void *data,
    bool is_system, const char *abbrev, const cmd_arg_desc_t *arg_desc,
    uint8_t arg_count, cmd_def_t *parent)
{
  cmd_def_t *d = mem_alloc("cmd", "def", sizeof(*d));
  memset(d, 0, sizeof(*d));

  if(module != NULL)
    strncpy(d->module, module, CMD_MODULE_SZ - 1);

  strncpy(d->name, name, CMD_NAME_SZ - 1);

  if(abbrev != NULL && abbrev[0] != '\0')
    strncpy(d->abbrev, abbrev, CMD_NAME_SZ - 1);

  if(usage != NULL)
    strncpy(d->usage, usage, CMD_USAGE_SZ - 1);
  if(help != NULL)
    strncpy(d->help, help, CMD_HELP_SZ - 1);
  if(help_long != NULL)
    strncpy(d->help_long, help_long, CMD_HELP_LONG_SZ - 1);

  strncpy(d->group, group, USERNS_GROUP_SZ - 1);
  d->level   = level;
  d->scope   = scope;
  d->cb      = cb;
  d->data    = data;
  d->builtin   = false;
  d->system    = is_system;
  d->arg_desc  = arg_desc;
  d->arg_count = arg_count;

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

// Internal registration helper.
// is_system: if true, command is system-level (always dispatchable)
// parent_name: name of parent command (NULL for root-level commands)
// abbrev: optional abbreviation (NULL or empty for none)
static bool
cmd_register_internal(const char *module, const char *name,
    const char *usage, const char *help,
    const char *help_long, const char *group, uint16_t level,
    cmd_scope_t scope, method_type_t methods, cmd_cb_t cb, void *data,
    bool is_system, const char *parent_name, const char *abbrev,
    const cmd_arg_desc_t *arg_desc, uint8_t arg_count)
{
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
  if(arg_desc != NULL && arg_count > 0)
  {
    if(!reg_validate_args(name, arg_desc, arg_count))
      return(FAIL);
  }

  pthread_mutex_lock(&cmd_mutex);

  // Resolve parent if specified (needed for scoped collision check).
  cmd_def_t *parent = NULL;

  if(parent_name != NULL && parent_name[0] != '\0')
  {
    parent = def_find_locked(parent_name);

    if(parent == NULL)
    {
      pthread_mutex_unlock(&cmd_mutex);
      clam(CLAM_WARN, "cmd_register",
          "'%s': parent command '%s' not found", name, parent_name);
      return(FAIL);
    }
  }

  if(!reg_check_collisions_locked(name, abbrev, parent, is_system))
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(FAIL);
  }

  cmd_def_t *d = reg_populate_def(module, name, usage, help, help_long,
      group, level, scope, methods, cb, data, is_system, abbrev, arg_desc,
      arg_count, parent);

  pthread_mutex_unlock(&cmd_mutex);

  clam(CLAM_DEBUG, "cmd_register",
      "registered '%s' (module: %s, group: %s, level: %u%s%s%s)", name,
      module ? module : "(none)", group,
      (unsigned)level, is_system ? ", system" : "",
      d->parent ? ", parent: " : "",
      d->parent ? d->parent->name : "");
  return(SUCCESS);
}

// Register a command globally.
bool
cmd_register(const char *module, const char *name,
    const char *usage, const char *help,
    const char *help_long, const char *group, uint16_t level,
    cmd_scope_t scope, method_type_t methods, cmd_cb_t cb, void *data,
    const char *parent_name, const char *abbrev,
    const cmd_arg_desc_t *arg_desc, uint8_t arg_count)
{
  return(cmd_register_internal(module, name, usage, help, help_long, group,
      level, scope, methods, cb, data, false, parent_name, abbrev, arg_desc,
      arg_count));
}

// Register a system-level command.
bool
cmd_register_system(const char *module, const char *name,
    const char *usage, const char *help,
    const char *help_long, const char *group, uint16_t level,
    cmd_scope_t scope, method_type_t methods, cmd_cb_t cb, void *data,
    const char *parent_name, const char *abbrev,
    const cmd_arg_desc_t *arg_desc, uint8_t arg_count)
{
  return(cmd_register_internal(module, name, usage, help, help_long, group,
      level, scope, methods, cb, data, true, parent_name, abbrev, arg_desc,
      arg_count));
}

// Unregister a command. Removes it from all bot instance bindings.
bool
cmd_unregister(const char *name)
{
  if(name == NULL || name[0] == '\0')
    return(FAIL);

  pthread_mutex_lock(&cmd_mutex);

  cmd_def_t *d = NULL;
  cmd_def_t *prev = NULL;

  for(d = cmd_list; d != NULL; prev = d, d = d->next)
  {
    if(strncasecmp(d->name, name, CMD_NAME_SZ) == 0)
      break;
  }

  if(d == NULL)
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(FAIL);
  }

  // Remove from all bot instance bindings.
  for(cmd_set_t *s = cmd_sets; s != NULL; s = s->next)
  {
    cmd_binding_t *b = NULL;
    cmd_binding_t *bprev = NULL;

    for(b = s->bindings; b != NULL; bprev = b, b = b->next)
    {
      if(strncasecmp(b->name, name, CMD_NAME_SZ) == 0)
      {
        if(bprev != NULL)
          bprev->next = b->next;
        else
          s->bindings = b->next;

        s->count--;
        bind_put(b);
        break;
      }
    }
  }

  // Unlink from parent's children list if this is a subcommand.
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

  // Reparent any children (they become root-level commands).
  cmd_def_t *child = d->children;

  while(child != NULL)
  {
    cmd_def_t *next_sib = child->sibling;
    child->parent = NULL;
    child->sibling = NULL;
    child = next_sib;
  }

  // Unlink from definition list.
  if(prev != NULL)
    prev->next = d->next;
  else
    cmd_list = d->next;

  cmd_def_count--;

  pthread_mutex_unlock(&cmd_mutex);

  clam(CLAM_DEBUG, "cmd_unregister", "unregistered '%s'", name);
  mem_free(d);
  return(SUCCESS);
}

// Find a registered command.
const cmd_def_t *
cmd_find(const char *name)
{
  if(name == NULL || name[0] == '\0')
    return(NULL);

  pthread_mutex_lock(&cmd_mutex);
  cmd_def_t *d = def_find_locked(name);
  pthread_mutex_unlock(&cmd_mutex);
  return(d);
}

// Get global command count.
uint32_t
cmd_count(void)
{
  return(cmd_def_count);
}

// -----------------------------------------------------------------------
// Per-bot instance command management
// -----------------------------------------------------------------------

// Enable a registered command on a bot instance.
bool
cmd_enable(bot_inst_t *inst, const char *cmd_name)
{
  if(inst == NULL || cmd_name == NULL || cmd_name[0] == '\0')
  {
    clam(CLAM_WARN, "cmd_enable", "invalid arguments");
    return(FAIL);
  }

  pthread_mutex_lock(&cmd_mutex);

  // Verify the command exists.
  cmd_def_t *d = def_find_locked(cmd_name);

  if(d == NULL)
  {
    pthread_mutex_unlock(&cmd_mutex);
    clam(CLAM_WARN, "cmd_enable",
        "'%s': command '%s' not registered",
        bot_inst_name(inst), cmd_name);
    return(FAIL);
  }

  // Built-in commands are always available; enabling is a no-op.
  if(d->builtin)
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(SUCCESS);
  }

  cmd_set_t *s = set_ensure_locked(inst);

  // Check for duplicate.
  for(cmd_binding_t *b = s->bindings; b != NULL; b = b->next)
  {
    if(strncasecmp(b->name, cmd_name, CMD_NAME_SZ) == 0)
    {
      pthread_mutex_unlock(&cmd_mutex);
      clam(CLAM_WARN, "cmd_enable",
          "'%s': command '%s' already enabled",
          bot_inst_name(inst), cmd_name);
      return(FAIL);
    }
  }

  cmd_binding_t *b = bind_get();
  strncpy(b->name, cmd_name, CMD_NAME_SZ - 1);
  b->next = s->bindings;
  s->bindings = b;
  s->count++;

  pthread_mutex_unlock(&cmd_mutex);

  clam(CLAM_DEBUG, "cmd_enable",
      "'%s': enabled command '%s'", bot_inst_name(inst), cmd_name);
  return(SUCCESS);
}

// Enable all registered user commands on a bot instance.
uint32_t
cmd_enable_all(bot_inst_t *inst)
{
  if(inst == NULL)
    return(0);

  uint32_t count = 0;

  pthread_mutex_lock(&cmd_mutex);

  for(cmd_def_t *d = cmd_list; d != NULL; d = d->next)
  {
    // Skip built-in, system, and subcommands.
    if(d->builtin || d->system || d->parent != NULL)
      continue;

    // Check for duplicate (already enabled).
    cmd_set_t *s = set_ensure_locked(inst);
    bool already = false;

    for(cmd_binding_t *b = s->bindings; b != NULL; b = b->next)
    {
      if(strncasecmp(b->name, d->name, CMD_NAME_SZ) == 0)
      {
        already = true;
        break;
      }
    }

    if(already)
      continue;

    cmd_binding_t *b = bind_get();
    strncpy(b->name, d->name, CMD_NAME_SZ - 1);
    b->next = s->bindings;
    s->bindings = b;
    s->count++;
    count++;
  }

  pthread_mutex_unlock(&cmd_mutex);

  clam(CLAM_DEBUG, "cmd_enable_all",
      "'%s': enabled %u command(s)", bot_inst_name(inst), count);
  return(count);
}

// Disable a command on a bot instance.
bool
cmd_disable(bot_inst_t *inst, const char *cmd_name)
{
  if(inst == NULL || cmd_name == NULL || cmd_name[0] == '\0')
    return(FAIL);

  pthread_mutex_lock(&cmd_mutex);

  cmd_set_t *s = set_find_locked(inst);

  if(s == NULL)
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(FAIL);
  }

  cmd_binding_t *b = NULL;
  cmd_binding_t *prev = NULL;

  for(b = s->bindings; b != NULL; prev = b, b = b->next)
  {
    if(strncasecmp(b->name, cmd_name, CMD_NAME_SZ) == 0)
    {
      if(prev != NULL)
        prev->next = b->next;
      else
        s->bindings = b->next;

      s->count--;
      bind_put(b);
      pthread_mutex_unlock(&cmd_mutex);

      clam(CLAM_DEBUG, "cmd_disable",
          "'%s': disabled command '%s'",
          bot_inst_name(inst), cmd_name);
      return(SUCCESS);
    }
  }

  pthread_mutex_unlock(&cmd_mutex);
  return(FAIL);
}

// Check if a command is enabled on a bot instance.
bool
cmd_is_enabled(const bot_inst_t *inst, const char *cmd_name)
{
  if(inst == NULL || cmd_name == NULL || cmd_name[0] == '\0')
    return(false);

  pthread_mutex_lock(&cmd_mutex);

  // Built-in commands are always enabled.
  cmd_def_t *d = def_find_locked(cmd_name);

  if(d != NULL && d->builtin)
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(true);
  }

  cmd_set_t *s = set_find_locked(inst);

  if(s == NULL)
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(false);
  }

  for(cmd_binding_t *b = s->bindings; b != NULL; b = b->next)
  {
    if(strncasecmp(b->name, cmd_name, CMD_NAME_SZ) == 0)
    {
      pthread_mutex_unlock(&cmd_mutex);
      return(true);
    }
  }

  pthread_mutex_unlock(&cmd_mutex);
  return(false);
}

// Get enabled command count for a bot instance.
uint32_t
cmd_enabled_count(const bot_inst_t *inst)
{
  if(inst == NULL)
    return(0);

  pthread_mutex_lock(&cmd_mutex);

  cmd_set_t *s = set_find_locked(inst);
  uint32_t count = (s != NULL) ? s->count : 0;

  pthread_mutex_unlock(&cmd_mutex);
  return(count);
}

// Clean up all per-bot command state.
void
cmd_bot_cleanup(bot_inst_t *inst)
{
  if(inst == NULL)
    return;

  pthread_mutex_lock(&cmd_mutex);

  cmd_set_t *s = NULL;
  cmd_set_t *prev = NULL;

  for(s = cmd_sets; s != NULL; prev = s, s = s->next)
  {
    if(s->inst == inst)
      break;
  }

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

  // Free all bindings.
  cmd_binding_t *b = s->bindings;

  while(b != NULL)
  {
    cmd_binding_t *next = b->next;
    bind_put(b);
    b = next;
  }

  set_put(s);

  pthread_mutex_unlock(&cmd_mutex);

  clam(CLAM_DEBUG, "cmd_bot_cleanup",
      "'%s': cleaned up command state", bot_inst_name(inst));
}

// -----------------------------------------------------------------------
// Command prefix
// -----------------------------------------------------------------------

// Set the command prefix for a bot instance.
bool
cmd_set_prefix(bot_inst_t *inst, const char *prefix)
{
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

  cmd_set_t *s = set_ensure_locked(inst);
  strncpy(s->prefix, prefix, CMD_PREFIX_SZ - 1);
  s->prefix[CMD_PREFIX_SZ - 1] = '\0';

  pthread_mutex_unlock(&cmd_mutex);

  clam(CLAM_DEBUG, "cmd_set_prefix",
      "'%s': prefix set to '%s'", bot_inst_name(inst), prefix);
  return(SUCCESS);
}

// Get the command prefix for a bot instance.
const char *
cmd_get_prefix(const bot_inst_t *inst)
{
  if(inst == NULL)
    return("!");

  pthread_mutex_lock(&cmd_mutex);

  cmd_set_t *s = set_find_locked(inst);
  const char *prefix = (s != NULL) ? s->prefix : "!";

  pthread_mutex_unlock(&cmd_mutex);
  return(prefix);
}

// -----------------------------------------------------------------------
// Internal: find a child command by name under a parent.
// Caller must hold cmd_mutex.
// -----------------------------------------------------------------------
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

// -----------------------------------------------------------------------
// Internal: resolve subcommand from args if parent has children.
// On match, updates *def to the child and *args to the remaining args.
// Caller must hold cmd_mutex.
// -----------------------------------------------------------------------
static void
resolve_subcmd_locked(cmd_def_t **def, const char **args)
{
  // Iteratively resolve nested subcommands (e.g., show → irc → networks).
  for(;;)
  {
    if((*def)->children == NULL || *args == NULL || (*args)[0] == '\0')
      return;

    // Parse the first token from args as a potential subcommand name.
    char sub_name[CMD_NAME_SZ] = {0};
    const char *p = *args;
    size_t i = 0;

    while(p[i] != '\0' && p[i] != ' ' && p[i] != '\t'
        && i < CMD_NAME_SZ - 1)
    {
      sub_name[i] = p[i];
      i++;
    }

    cmd_def_t *child = def_find_child_locked(*def, sub_name);

    if(child == NULL)
      return;

    // Advance args past the subcommand name and whitespace.
    const char *rest = p + i;

    while(*rest == ' ' || *rest == '\t')
      rest++;

    *def  = child;
    *args = rest;
  }
}

// -----------------------------------------------------------------------
// Argument parsing and validation
// -----------------------------------------------------------------------

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

// Validate a single argument against its descriptor.
// returns: true if valid
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
// returns: true if parsing and validation succeeded
// args: raw argument string
// desc: argument descriptor array
// count: number of descriptors
// bufs: token storage [CMD_MAX_ARGS][CMD_ARG_SZ]
// parsed: output struct to populate
// ctx: command context for error replies
// usage: usage string for "Usage:" error messages
static bool
cmd_parse_args(const char *args, const cmd_arg_desc_t *desc,
    uint8_t count, char bufs[][CMD_ARG_SZ], cmd_args_t *parsed,
    const cmd_ctx_t *ctx, const char *usage)
{
  memset(parsed, 0, sizeof(*parsed));

  const char *p = args;

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
        snprintf(buf, sizeof(buf), "Usage: %s", usage);
        cmd_reply(ctx, buf);
        return false;
      }

      break;
    }

    // CMD_ARG_REST: capture the remainder of the line.
    if(desc[i].flags & CMD_ARG_REST)
    {
      strncpy(bufs[i], p, CMD_ARG_SZ - 1);
      bufs[i][CMD_ARG_SZ - 1] = '\0';
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
    size_t maxlen = desc[i].maxlen > 0 ? desc[i].maxlen : CMD_ARG_SZ - 1;
    size_t j = 0;

    while(*p != '\0' && *p != ' ' && *p != '\t' && j < maxlen)
      bufs[i][j++] = *p++;

    // If there are more non-whitespace chars, the token was too long.
    // Consume the rest of this token to keep parsing consistent.
    if(*p != '\0' && *p != ' ' && *p != '\t')
    {
      while(*p != '\0' && *p != ' ' && *p != '\t')
        p++;
    }

    bufs[i][j] = '\0';
    parsed->argv[i] = bufs[i];
    parsed->argc = i + 1;

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

// -----------------------------------------------------------------------
// Task callback for async command execution. Parses args if the command
// has an arg spec, then invokes the command callback.
// t: the task (t->data is a heap-allocated cmd_task_data_t, freed here)
// -----------------------------------------------------------------------
static void
cmd_task_cb(task_t *t)
{
  cmd_task_data_t *d = (cmd_task_data_t *)t->data;

  cmd_ctx_t ctx = {
    .bot      = d->bot,
    .msg      = &d->msg,
    .args     = d->args,
    .username = d->username[0] != '\0' ? d->username : NULL,
    .parsed   = NULL,
  };

  // Pre-parse and validate arguments if the command has an arg spec.
  cmd_args_t parsed;

  if(d->arg_desc != NULL && d->arg_count > 0)
  {
    if(!cmd_parse_args(d->args, d->arg_desc, d->arg_count,
        d->arg_bufs, &parsed, &ctx, d->usage))
    {
      mem_free(d);
      t->state = TASK_ENDED;
      return;
    }

    ctx.parsed = &parsed;
  }

  d->cb(&ctx);
  mem_free(d);

  t->state = TASK_ENDED;
}

// -----------------------------------------------------------------------
// Command dispatch
// -----------------------------------------------------------------------

// Dispatch a message as a potential command for a bot instance. Parses
// the prefix and command name, checks enablement and permissions, and
// submits a task for async execution.
// returns: SUCCESS if dispatched, FAIL if not a command or denied
// inst: bot instance
// msg: full message context
bool
cmd_dispatch(bot_inst_t *inst, const method_msg_t *msg)
{
  if(inst == NULL || msg == NULL || msg->text[0] == '\0')
    return(FAIL);

  // Resolve per-method prefix from KV, falling back to bot-level prefix.
  // Must happen before taking the mutex (kv_get_str has its own locking).
  const char *prefix = NULL;

  if(msg->inst != NULL)
  {
    const char *kind = method_inst_kind(msg->inst);
    const char *bname = bot_inst_name(inst);

    if(kind != NULL && bname != NULL)
    {
      char pfx_key[KV_KEY_SZ];

      snprintf(pfx_key, sizeof(pfx_key),
          "bot.%s.%s.prefix", bname, kind);

      const char *kv_pfx = kv_get_str(pfx_key);

      if(kv_pfx != NULL && kv_pfx[0] != '\0')
        prefix = kv_pfx;
    }
  }

  pthread_mutex_lock(&cmd_mutex);

  // Fall back to bot-level prefix if per-method not available.
  cmd_set_t *s = set_find_locked(inst);

  if(prefix == NULL)
    prefix = (s != NULL) ? s->prefix : "!";

  size_t pfx_len = strlen(prefix);

  // Check if message starts with the prefix.
  if(strncmp(msg->text, prefix, pfx_len) != 0)
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(FAIL);
  }

  // Parse command name (characters after prefix, up to whitespace).
  const char *start = msg->text + pfx_len;

  if(*start == '\0' || *start == ' ' || *start == '\t')
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(FAIL);
  }

  char cmd_name[CMD_NAME_SZ] = {0};
  size_t i = 0;

  while(start[i] != '\0' && start[i] != ' ' && start[i] != '\t'
      && i < CMD_NAME_SZ - 1)
  {
    cmd_name[i] = start[i];
    i++;
  }

  // Find the arguments (skip whitespace after command name).
  const char *args = start + i;

  while(*args == ' ' || *args == '\t')
    args++;

  // Look up the command definition. Skip commands that are
  // subcommands — they are only reachable through their parent.
  cmd_def_t *d = def_find_locked(cmd_name);

  if(d == NULL || d->parent != NULL)
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(FAIL);
  }

  // Resolve subcommand if the matched command has children.
  resolve_subcmd_locked(&d, &args);

  // Method type check: reject if the command is not visible on this method.
  method_type_t inst_type = method_inst_type(msg->inst);

  if(inst_type != 0 && !(d->methods & inst_type))
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(FAIL);
  }

  // Check if command is enabled: built-in and system commands are
  // always available; others must be explicitly enabled per bot.
  if(!d->builtin && !d->system)
  {
    bool enabled = false;

    if(s != NULL)
    {
      for(cmd_binding_t *b = s->bindings; b != NULL; b = b->next)
      {
        if(strncasecmp(b->name, d->name, CMD_NAME_SZ) == 0)
        {
          enabled = true;
          break;
        }
      }
    }

    if(!enabled)
    {
      pthread_mutex_unlock(&cmd_mutex);
      return(FAIL);
    }
  }

  // Copy what we need from the definition before releasing the lock.
  cmd_cb_t cb = d->cb;
  void *cb_data = d->data;
  uint16_t req_level = d->level;
  cmd_scope_t scope = d->scope;
  char group[USERNS_GROUP_SZ];
  memset(group, 0, sizeof(group));
  memcpy(group, d->group, USERNS_GROUP_SZ);
  const cmd_arg_desc_t *arg_desc = d->arg_desc;
  uint8_t arg_count = d->arg_count;
  char usage[CMD_USAGE_SZ];
  memset(usage, 0, sizeof(usage));
  memcpy(usage, d->usage, CMD_USAGE_SZ);

  pthread_mutex_unlock(&cmd_mutex);

  // Permission checking (done outside lock).
  const char *username = bot_session_find(inst, msg->inst, msg->sender);

  // Console origin bypasses all scope and permission checks.
  if(msg->console_origin)
  {
    username = msg->sender;
    goto dispatch;
  }

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

  // Owner bypasses all permission checks.
  if(username != NULL && userns_is_owner(username))
    goto dispatch;

  if(username != NULL)
  {
    // Authenticated user: check group membership + level.
    userns_t *ns = bot_get_userns(inst);

    if(ns == NULL)
    {
      clam(CLAM_WARN, "cmd_dispatch",
          "'%s': denied '%s': reason=no_namespace "
          "[sender=%s host=%s source=%s]",
          bot_inst_name(inst), cmd_name,
          msg->sender,
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

    int32_t user_level = userns_member_level(ns, username, group);

    if(user_level < 0 || (uint16_t)user_level < req_level)
    {
      const char *reason = (user_level < 0)
          ? "not_in_group" : "insufficient_level";

      clam(CLAM_WARN, "cmd_dispatch",
          "'%s': denied '%s': reason=%s "
          "(group '%s' level %d, need %u) "
          "[sender=%s host=%s source=%s]",
          bot_inst_name(inst), cmd_name, reason, group,
          user_level, (unsigned)req_level,
          msg->sender,
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
  }
  else
  {
    // Unauthenticated user: only allowed if group is "everyone"
    // and required level is 0.
    if(strcmp(group, USERNS_GROUP_EVERYONE) != 0 || req_level > 0)
    {
      clam(CLAM_WARN, "cmd_dispatch",
          "'%s': denied '%s': reason=not_authenticated "
          "[sender=%s host=%s source=%s]",
          bot_inst_name(inst), cmd_name,
          msg->sender,
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
  }

dispatch:
  ;   // empty statement required before declaration (pre-C23)

  // Build task data.
  cmd_task_data_t *td = mem_alloc("cmd", "task_data", sizeof(*td));
  memset(td, 0, sizeof(*td));
  td->cb = cb;
  td->cb_data = cb_data;
  td->bot = inst;
  memcpy(&td->msg, msg, sizeof(method_msg_t));
  strncpy(td->args, args, METHOD_TEXT_SZ - 1);

  if(username != NULL)
    strncpy(td->username, username, USERNS_USER_SZ - 1);

  td->arg_desc  = arg_desc;
  td->arg_count = arg_count;
  memcpy(td->usage, usage, CMD_USAGE_SZ);

  // Submit task.
  char task_name[TASK_NAME_SZ];
  snprintf(task_name, sizeof(task_name), "cmd:%s", cmd_name);

  task_t *t = task_add(task_name, TASK_THREAD, 128, cmd_task_cb, td);

  if(t == NULL)
  {
    clam(CLAM_WARN, "cmd_dispatch",
        "'%s': failed to submit task for '%s'",
        bot_inst_name(inst), cmd_name);
    mem_free(td);
    return(FAIL);
  }

  __atomic_add_fetch(&cmd_stat_dispatches, 1, __ATOMIC_RELAXED);

  clam(CLAM_DEBUG, "cmd_dispatch",
      "'%s': dispatched '%s' from %s on %s",
      bot_inst_name(inst), cmd_name,
      msg->sender, method_inst_name(msg->inst));
  return(SUCCESS);
}

// -----------------------------------------------------------------------
// Reply helper
// -----------------------------------------------------------------------

// Send a reply to the originator of a command. Replies to the channel
// if the message came from one, otherwise replies to the sender directly.
// returns: SUCCESS or FAIL
// ctx: command context
// text: reply text
bool
cmd_reply(const cmd_ctx_t *ctx, const char *text)
{
  if(ctx == NULL || text == NULL)
    return(FAIL);

  if(ctx->msg == NULL || ctx->msg->inst == NULL)
    return(FAIL);

  const char *target = ctx->msg->channel[0] != '\0'
      ? ctx->msg->channel
      : ctx->msg->sender;

  return(method_send(ctx->msg->inst, target, text));
}

// -----------------------------------------------------------------------
// Built-in commands
// -----------------------------------------------------------------------

// Show verbose help for a single command.
// Looks up the command by name (with subcommand resolution), checks
// accessibility, and sends detailed usage/help/permissions to the caller.
// ctx: command context
// args: argument string naming the command (e.g. "bot add")
static void
help_show_command(const cmd_ctx_t *ctx, const char *args)
{
  // Parse first token as command name.
  char help_name[CMD_NAME_SZ] = {0};
  const char *hp = args;
  size_t hi = 0;

  while(hp[hi] != '\0' && hp[hi] != ' ' && hp[hi] != '\t'
      && hi < CMD_NAME_SZ - 1)
  {
    help_name[hi] = hp[hi];
    hi++;
  }

  const char *help_rest = hp + hi;

  while(*help_rest == ' ' || *help_rest == '\t')
    help_rest++;

  pthread_mutex_lock(&cmd_mutex);
  cmd_def_t *d = def_find_locked(help_name);

  // Resolve subcommand if remaining args and parent has children.
  if(d != NULL && d->children != NULL && help_rest[0] != '\0')
    resolve_subcmd_locked(&d, &help_rest);

  if(d == NULL)
  {
    pthread_mutex_unlock(&cmd_mutex);
    char buf[CMD_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "unknown command: %s", args);
    cmd_reply(ctx, buf);
    return;
  }

  // Method type check: reject commands not visible on this method.
  method_type_t mt = (ctx->msg != NULL && ctx->msg->inst != NULL)
      ? method_inst_type(ctx->msg->inst) : METHOD_T_ANY;

  if(mt != 0 && !(d->methods & mt))
  {
    pthread_mutex_unlock(&cmd_mutex);
    char buf[CMD_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "unknown command: %s", args);
    cmd_reply(ctx, buf);
    return;
  }

  // Check that command is accessible (built-in, system, or enabled).
  if(!d->builtin && !d->system)
  {
    cmd_set_t *s = set_find_locked(ctx->bot);
    bool enabled = false;

    if(s != NULL)
    {
      for(cmd_binding_t *b = s->bindings; b != NULL; b = b->next)
      {
        if(strncasecmp(b->name, args, CMD_NAME_SZ) == 0)
        {
          enabled = true;
          break;
        }
      }
    }

    if(!enabled)
    {
      pthread_mutex_unlock(&cmd_mutex);
      char buf[CMD_NAME_SZ + 32];
      snprintf(buf, sizeof(buf), "unknown command: %s", args);
      cmd_reply(ctx, buf);
      return;
    }
  }

  // Copy fields while holding the lock.
  char module_name[CMD_MODULE_SZ];
  char usage[CMD_USAGE_SZ];
  char help[CMD_HELP_SZ];
  char help_long[CMD_HELP_LONG_SZ];
  char group[USERNS_GROUP_SZ];
  uint16_t level = d->level;

  strncpy(module_name, d->module, CMD_MODULE_SZ);
  strncpy(usage, d->usage, CMD_USAGE_SZ);
  strncpy(help, d->help, CMD_HELP_SZ);
  strncpy(help_long, d->help_long, CMD_HELP_LONG_SZ);
  strncpy(group, d->group, USERNS_GROUP_SZ);

  pthread_mutex_unlock(&cmd_mutex);

  // Display command details.
  char line[CMD_USAGE_SZ + 16];
  snprintf(line, sizeof(line), "usage: %s", usage);
  cmd_reply(ctx, line);
  cmd_reply(ctx, help);

  if(help_long[0] != '\0')
  {
    // Send verbose help line-by-line (newlines delimit lines).
    char *text = help_long;
    char *nl;

    while((nl = strchr(text, '\n')) != NULL)
    {
      *nl = '\0';
      cmd_reply(ctx, text);
      text = nl + 1;
    }

    // Send any remaining text after the last newline.
    if(*text != '\0')
      cmd_reply(ctx, text);
  }

  char module_line[CMD_MODULE_SZ + 16];
  snprintf(module_line, sizeof(module_line), "module: %s",
      module_name[0] != '\0' ? module_name : "core");
  cmd_reply(ctx, module_line);

  char perms[USERNS_GROUP_SZ + 32];
  snprintf(perms, sizeof(perms), "requires: %s (level %u)",
      group, (unsigned)level);
  cmd_reply(ctx, perms);
}

// Collect unique module names from available commands into an array.
// Caller must hold cmd_mutex.
// bot: bot instance (for enabled-command lookup)
// mt: caller's method type bitmask (filter out non-matching commands)
// modules: output array of module name buffers
// returns: number of unique modules found
static uint32_t
help_collect_modules(const bot_inst_t *bot, method_type_t mt,
    char modules[][CMD_MODULE_SZ])
{
  uint32_t mod_count = 0;

  // Scan built-in commands for modules. Skip subcommands (they are
  // listed under their parent) and method-filtered commands.
  for(cmd_def_t *d = cmd_list; d != NULL; d = d->next)
  {
    if(!d->builtin || d->parent != NULL)
      continue;

    if(mt != 0 && !(d->methods & mt))
      continue;

    const char *mod = d->module[0] != '\0' ? d->module : "core";
    bool found = false;

    for(uint32_t i = 0; i < mod_count; i++)
    {
      if(strcmp(modules[i], mod) == 0)
      {
        found = true;
        break;
      }
    }

    if(!found && mod_count < 64)
    {
      snprintf(modules[mod_count], CMD_MODULE_SZ, "%s", mod);
      mod_count++;
    }
  }

  // Scan enabled commands for modules (skip subcommands).
  cmd_set_t *s = set_find_locked(bot);

  if(s != NULL)
  {
    for(cmd_binding_t *b = s->bindings; b != NULL; b = b->next)
    {
      cmd_def_t *d = def_find_locked(b->name);

      if(d == NULL || d->parent != NULL)
        continue;

      if(mt != 0 && !(d->methods & mt))
        continue;

      const char *mod = d->module[0] != '\0' ? d->module : "core";
      bool found = false;

      for(uint32_t i = 0; i < mod_count; i++)
      {
        if(strcmp(modules[i], mod) == 0)
        {
          found = true;
          break;
        }
      }

      if(!found && mod_count < 64)
      {
        snprintf(modules[mod_count], CMD_MODULE_SZ, "%s", mod);
        mod_count++;
      }
    }
  }

  return(mod_count);
}

// Print commands for a single module group. Lists built-in commands
// first, then enabled commands for the bot instance.
// Caller must hold cmd_mutex.
// ctx: command context (for replies)
// mod_name: module name to filter on (bounded to CMD_MODULE_SZ)
// mt: caller's method type bitmask (filter out non-matching commands)
// s: bot's command set (may be NULL)
static void
help_list_module_locked(const cmd_ctx_t *ctx,
    const char mod_name[CMD_MODULE_SZ], method_type_t mt,
    const cmd_set_t *s)
{
  char hdr[CMD_MODULE_SZ + 4];
  snprintf(hdr, sizeof(hdr), "[%s]", mod_name);

  pthread_mutex_unlock(&cmd_mutex);
  cmd_reply(ctx, hdr);
  pthread_mutex_lock(&cmd_mutex);

  // List built-in commands in this module (skip subcommands).
  for(cmd_def_t *d = cmd_list; d != NULL; d = d->next)
  {
    if(!d->builtin || d->parent != NULL)
      continue;

    if(mt != 0 && !(d->methods & mt))
      continue;

    const char *mod = d->module[0] != '\0' ? d->module : "core";

    if(strcmp(mod, mod_name) != 0)
      continue;

    char line[CMD_USAGE_SZ + CMD_HELP_SZ + 8];
    snprintf(line, sizeof(line), "  %s — %s", d->usage, d->help);

    pthread_mutex_unlock(&cmd_mutex);
    cmd_reply(ctx, line);
    pthread_mutex_lock(&cmd_mutex);
  }

  // List enabled commands in this module (skip subcommands).
  if(s != NULL)
  {
    for(const cmd_binding_t *b = s->bindings; b != NULL; b = b->next)
    {
      cmd_def_t *d = def_find_locked(b->name);

      if(d == NULL || d->parent != NULL)
        continue;

      if(mt != 0 && !(d->methods & mt))
        continue;

      const char *mod = d->module[0] != '\0' ? d->module : "core";

      if(strcmp(mod, mod_name) != 0)
        continue;

      char line[CMD_USAGE_SZ + CMD_HELP_SZ + 8];
      snprintf(line, sizeof(line), "  %s — %s", d->usage, d->help);

      pthread_mutex_unlock(&cmd_mutex);
      cmd_reply(ctx, line);
      pthread_mutex_lock(&cmd_mutex);
    }
  }
}

// Built-in: help -- list all available commands, or show verbose help
// for a specific command.
//
// Usage:
//   !help            -- list all available commands
//   !help <command>   -- show verbose help for a specific command
static void
cmd_builtin_help(const cmd_ctx_t *ctx)
{
  // If an argument was provided, show verbose help for that command.
  if(ctx->args != NULL && ctx->args[0] != '\0')
  {
    help_show_command(ctx, ctx->args);
    return;
  }

  // No argument: list all available commands, grouped by module.
  pthread_mutex_lock(&cmd_mutex);

  cmd_reply(ctx, "Available commands:");

  method_type_t mt = (ctx->msg != NULL && ctx->msg->inst != NULL)
      ? method_inst_type(ctx->msg->inst) : METHOD_T_ANY;

  char modules[64][CMD_MODULE_SZ];
  uint32_t mod_count = help_collect_modules(ctx->bot, mt, modules);

  cmd_set_t *s = set_find_locked(ctx->bot);

  for(uint32_t m = 0; m < mod_count; m++)
    help_list_module_locked(ctx, modules[m], mt, s);

  pthread_mutex_unlock(&cmd_mutex);

  cmd_reply(ctx, "Use help <command> for detailed information.");
}

// Built-in: version — show program version.
static void
cmd_builtin_version(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, BM_VERSION_STR);
}

// Register a built-in command (internal helper).
// module: providing module name
// name: command name
// usage: usage string
// help: help text
// help_long: verbose help text (may be NULL)
// group: default group
// level: default level
// cb: callback
static void
register_builtin(const char *module, const char *name,
    const char *usage, const char *help,
    const char *help_long, const char *group, uint16_t level, cmd_cb_t cb)
{
  cmd_def_t *d = mem_alloc("cmd", "def_builtin", sizeof(*d));
  memset(d, 0, sizeof(*d));

  if(module != NULL)
    strncpy(d->module, module, CMD_MODULE_SZ - 1);

  strncpy(d->name, name, CMD_NAME_SZ - 1);
  strncpy(d->usage, usage, CMD_USAGE_SZ - 1);
  strncpy(d->help, help, CMD_HELP_SZ - 1);

  if(help_long != NULL)
    strncpy(d->help_long, help_long, CMD_HELP_LONG_SZ - 1);

  strncpy(d->group, group, USERNS_GROUP_SZ - 1);
  d->level = level;
  d->cb = cb;
  d->builtin = true;
  d->methods = METHOD_T_ANY;

  d->next = cmd_list;
  cmd_list = d;
  cmd_def_count++;
}

// -----------------------------------------------------------------------
// Command definition accessors
// -----------------------------------------------------------------------

// Get the module name for a command definition.
// returns: module string, or NULL if def is NULL
// def: command definition
const char *
cmd_get_module(const cmd_def_t *def)
{
  return(def != NULL ? def->module : NULL);
}

// Get the usage string for a command definition.
// returns: usage string, or NULL if def is NULL
// def: command definition
const char *
cmd_get_usage(const cmd_def_t *def)
{
  return(def != NULL ? def->usage : NULL);
}

// Get the brief help text for a command definition.
// returns: help string, or NULL if def is NULL
// def: command definition
const char *
cmd_get_help(const cmd_def_t *def)
{
  return(def != NULL ? def->help : NULL);
}

// Get the verbose help text for a command definition.
// returns: help_long string (empty if not set), or NULL if def is NULL
// def: command definition
const char *
cmd_get_help_long(const cmd_def_t *def)
{
  return(def != NULL ? def->help_long : NULL);
}

// Get the name of a command definition.
// returns: name string, or NULL if def is NULL
// def: command definition
const char *
cmd_get_name(const cmd_def_t *def)
{
  return(def != NULL ? def->name : NULL);
}

// Check if a command has subcommands.
// returns: true if the command has at least one child
// def: command definition
bool
cmd_has_children(const cmd_def_t *def)
{
  return(def != NULL && def->children != NULL);
}

// Check if a command is a subcommand (has a parent).
// returns: true if the command has a parent
// def: command definition
bool
cmd_is_child(const cmd_def_t *def)
{
  return(def != NULL && def->parent != NULL);
}

// Get the parent command definition for a subcommand.
// returns: parent cmd_def_t pointer, or NULL if def is NULL or root-level
// def: command definition
const cmd_def_t *
cmd_get_parent(const cmd_def_t *def)
{
  return(def != NULL ? def->parent : NULL);
}

// Get the abbreviation for a command.
// returns: abbreviation string (empty if none), or NULL if def is NULL
// def: command definition
const char *
cmd_get_abbrev(const cmd_def_t *def)
{
  return(def != NULL ? def->abbrev : NULL);
}

// Get the required group for a command.
// returns: group string, or NULL if def is NULL
// def: command definition
const char *
cmd_get_group(const cmd_def_t *def)
{
  return(def != NULL ? def->group : NULL);
}

// Get the required privilege level for a command.
// returns: level value, or 0 if def is NULL
// def: command definition
uint16_t
cmd_get_level(const cmd_def_t *def)
{
  return(def != NULL ? def->level : 0);
}

// Get the method type bitmask for a command.
// returns: method bitmask, or METHOD_T_ANY if def is NULL
// def: command definition
method_type_t
cmd_get_methods(const cmd_def_t *def)
{
  return(def != NULL ? def->methods : METHOD_T_ANY);
}

// returns: scope value, or CMD_SCOPE_ANY if def is NULL
cmd_scope_t
cmd_get_scope(const cmd_def_t *def)
{
  return(def != NULL ? def->scope : CMD_SCOPE_ANY);
}

// -----------------------------------------------------------------------
// Command iteration
// -----------------------------------------------------------------------

// Iterate top-level system commands, invoking cb for each.
// cb: callback invoked for each system command
// data: opaque user data passed to cb
void
cmd_iterate_system(cmd_iter_cb_t cb, void *data)
{
  if(cb == NULL)
    return;

  pthread_mutex_lock(&cmd_mutex);

  for(cmd_def_t *d = cmd_list; d != NULL; d = d->next)
  {
    if(!d->system || d->parent != NULL)
      continue;

    pthread_mutex_unlock(&cmd_mutex);
    cb(d, data);
    pthread_mutex_lock(&cmd_mutex);
  }

  pthread_mutex_unlock(&cmd_mutex);
}

// Iterate all system commands including subcommands.
// cb: callback invoked for each system command
// data: opaque user data passed to cb
void
cmd_iterate_system_all(cmd_iter_cb_t cb, void *data)
{
  if(cb == NULL)
    return;

  pthread_mutex_lock(&cmd_mutex);

  for(cmd_def_t *d = cmd_list; d != NULL; d = d->next)
  {
    if(!d->system)
      continue;

    pthread_mutex_unlock(&cmd_mutex);
    cb(d, data);
    pthread_mutex_lock(&cmd_mutex);
  }

  pthread_mutex_unlock(&cmd_mutex);
}

// Iterate children (subcommands) of a specific parent command.
// parent: parent command definition
// cb: callback invoked for each child
// data: opaque user data passed to cb
void
cmd_iterate_children(const cmd_def_t *parent, cmd_iter_cb_t cb, void *data)
{
  if(parent == NULL || cb == NULL)
    return;

  pthread_mutex_lock(&cmd_mutex);

  for(cmd_def_t *c = ((cmd_def_t *)parent)->children; c != NULL;
      c = c->sibling)
  {
    pthread_mutex_unlock(&cmd_mutex);
    cb(c, data);
    pthread_mutex_lock(&cmd_mutex);
  }

  pthread_mutex_unlock(&cmd_mutex);
}

// Iterate top-level commands enabled on a bot instance.
// Includes built-in commands and explicitly enabled plugin commands.
// Skips subcommands.
// inst: bot instance
// cb: callback invoked for each enabled command
// data: opaque user data passed to cb
void
cmd_iterate_bot(const bot_inst_t *inst, cmd_iter_cb_t cb, void *data)
{
  if(inst == NULL || cb == NULL)
    return;

  pthread_mutex_lock(&cmd_mutex);

  // Yield built-in commands (always available on all bots).
  for(cmd_def_t *d = cmd_list; d != NULL; d = d->next)
  {
    if(!d->builtin || d->parent != NULL)
      continue;

    pthread_mutex_unlock(&cmd_mutex);
    cb(d, data);
    pthread_mutex_lock(&cmd_mutex);
  }

  // Yield explicitly enabled commands.
  cmd_set_t *s = set_find_locked(inst);

  if(s != NULL)
  {
    for(cmd_binding_t *b = s->bindings; b != NULL; b = b->next)
    {
      cmd_def_t *d = def_find_locked(b->name);

      if(d == NULL || d->parent != NULL || d->builtin)
        continue;

      pthread_mutex_unlock(&cmd_mutex);
      cb(d, data);
      pthread_mutex_lock(&cmd_mutex);
    }
  }

  pthread_mutex_unlock(&cmd_mutex);
}

// -----------------------------------------------------------------------
// Console method integration
// -----------------------------------------------------------------------

// Set the console method instance for system command reply routing.
// inst: console method instance
void
cmd_set_console_inst(method_inst_t *inst)
{
  cmd_console_inst = inst;
}

// -----------------------------------------------------------------------
// System command dispatch (owner path)
// -----------------------------------------------------------------------

// Dispatch a system or built-in command as @owner on a given method instance.
// Executes synchronously. Only system and built-in commands are allowed.
// returns: SUCCESS if command was found and executed, FAIL otherwise
// cmd_name: command name (without prefix)
// args: argument string (may be empty or NULL)
// inst: method instance for reply routing
bool
cmd_dispatch_owner(const char *cmd_name, const char *args,
    method_inst_t *inst)
{
  if(cmd_name == NULL || cmd_name[0] == '\0')
    return(FAIL);

  if(args == NULL)
    args = "";

  if(inst == NULL)
    return(FAIL);

  pthread_mutex_lock(&cmd_mutex);

  cmd_def_t *d = def_find_locked(cmd_name);

  if(d == NULL || d->parent != NULL)
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(FAIL);
  }

  // Only system and built-in commands are dispatchable via this path.
  if(!d->system && !d->builtin)
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(FAIL);
  }

  // Resolve subcommand if the matched command has children.
  resolve_subcmd_locked(&d, &args);

  // Method type check: reject if the command is not visible on this method.
  method_type_t inst_type = method_inst_type(inst);

  if(inst_type != 0 && !(d->methods & inst_type))
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(FAIL);
  }

  cmd_cb_t cb = d->cb;
  const cmd_arg_desc_t *ad = d->arg_desc;
  uint8_t ac = d->arg_count;
  char usage[CMD_USAGE_SZ];
  memset(usage, 0, sizeof(usage));
  memcpy(usage, d->usage, CMD_USAGE_SZ);

  pthread_mutex_unlock(&cmd_mutex);

  // Build a synthetic message context for the given method instance.
  method_msg_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.inst = inst;
  snprintf(msg.sender, METHOD_SENDER_SZ, "%s", USERNS_OWNER_USER);
  msg.timestamp = time(NULL);

  cmd_ctx_t ctx = {
    .bot      = NULL,
    .msg      = &msg,
    .args     = args,
    .username = USERNS_OWNER_USER,
    .parsed   = NULL,
  };

  // Pre-parse and validate arguments if the command has an arg spec.
  cmd_args_t parsed;
  char arg_bufs[CMD_MAX_ARGS][CMD_ARG_SZ];

  if(ad != NULL && ac > 0)
  {
    memset(arg_bufs, 0, sizeof(arg_bufs));

    if(!cmd_parse_args(args, ad, ac, arg_bufs, &parsed, &ctx, usage))
      return(SUCCESS);    // validation failed, error already sent

    ctx.parsed = &parsed;
  }

  // Execute synchronously.
  cb(&ctx);

  return(SUCCESS);
}

// Dispatch a system command via the console method instance.
// Convenience wrapper around cmd_dispatch_owner().
// returns: SUCCESS if command was found and executed, FAIL otherwise
// cmd_name: command name (without prefix)
// args: argument string (may be empty)
bool
cmd_dispatch_system(const char *cmd_name, const char *args)
{
  return(cmd_dispatch_owner(cmd_name, args, cmd_console_inst));
}

// -----------------------------------------------------------------------
// Subsystem lifecycle
// -----------------------------------------------------------------------

// Initialize the command subsystem. Sets up the mutex and registers
// built-in commands (help, version).
void
cmd_init(void)
{
  pthread_mutex_init(&cmd_mutex, NULL);

  // Register built-in commands (always available per-bot).
  register_builtin("core", "help", "help [command]",
      "List available commands",
      "Lists all commands available on this bot instance, including\n"
      "built-in and enabled commands. Use help <command> to see\n"
      "detailed usage, description, and permission requirements\n"
      "for a specific command.",
      USERNS_GROUP_EVERYONE, 0, cmd_builtin_help);
  register_builtin("core", "version", "version", "Show program version",
      NULL,
      USERNS_GROUP_EVERYONE, 0, cmd_builtin_version);

  cmd_ready = true;

  clam(CLAM_INFO, "cmd_init",
      "command subsystem initialized (%u commands)", cmd_def_count);
}

// Get lifetime command dispatch counters (thread-safe, atomic reads).
// dispatches: output for successful dispatch count (may be NULL)
// denials: output for permission denial count (may be NULL)
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
  if(!cmd_ready)
    return;

  uint32_t set_count = 0;

  for(cmd_set_t *sc = cmd_sets; sc != NULL; sc = sc->next)
    set_count++;

  clam(CLAM_INFO, "cmd_exit",
      "shutting down (%u commands, %u bot sets, "
      "%u freelisted bindings, %u freelisted sets)",
      cmd_def_count, set_count,
      cmd_bind_free_count, cmd_set_free_count);

  cmd_ready = false;

  // Free all per-bot command sets.
  cmd_set_t *s = cmd_sets;

  while(s != NULL)
  {
    cmd_set_t *snext = s->next;

    // Free bindings.
    cmd_binding_t *b = s->bindings;

    while(b != NULL)
    {
      cmd_binding_t *bnext = b->next;
      mem_free(b);
      b = bnext;
    }

    mem_free(s);
    s = snext;
  }

  cmd_sets = NULL;

  // Free all command definitions.
  cmd_def_t *d = cmd_list;

  while(d != NULL)
  {
    cmd_def_t *dnext = d->next;
    mem_free(d);
    d = dnext;
  }

  cmd_list = NULL;
  cmd_def_count = 0;

  // Free binding freelist.
  cmd_binding_t *b = cmd_bind_freelist;

  while(b != NULL)
  {
    cmd_binding_t *bnext = b->next;
    mem_free(b);
    b = bnext;
  }

  cmd_bind_freelist = NULL;
  cmd_bind_free_count = 0;

  // Free set freelist.
  cmd_set_t *sf = cmd_set_freelist;

  while(sf != NULL)
  {
    cmd_set_t *sfnext = sf->next;
    mem_free(sf);
    sf = sfnext;
  }

  cmd_set_freelist = NULL;
  cmd_set_free_count = 0;

  cmd_console_inst = NULL;

  pthread_mutex_destroy(&cmd_mutex);
}
