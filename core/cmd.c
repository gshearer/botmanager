#define CMD_INTERNAL
#include "cmd.h"

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

// Forward declaration (defined after def_find_locked, used below).
static cmd_def_t *def_find_child_locked(cmd_def_t *parent, const char *name);

// Internal: resolve a slash-delimited parent path (e.g. "irc/network")
// to a cmd_def_t by walking the command tree. Single-segment paths like
// "bot" are also handled. Caller must hold cmd_mutex.
// returns: the resolved command, or NULL if any segment is not found
static cmd_def_t *
resolve_parent_path_locked(const char *path)
{
  if(path == NULL || path[0] == '\0')
    return(NULL);

  // Copy path so we can tokenize on '/'.
  char buf[CMD_USAGE_SZ];
  strncpy(buf, path, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  // First segment: root-level lookup.
  char *tok = buf;
  char *slash = strchr(tok, '/');

  if(slash != NULL)
    *slash = '\0';

  cmd_def_t *d = def_find_locked(tok);

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
// returns: true if no collision (ok to proceed), false on collision
static bool
reg_check_collisions_locked(const char *name, const char *abbrev,
    cmd_def_t *parent)
{
  if(def_name_collides_locked(name, parent))
  {
    clam(CLAM_WARN, "cmd_register", "duplicate command: '%s'", name);
    return(false);
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
    const char *usage, const char *description,
    const char *help_long, const char *group, uint16_t level,
    cmd_scope_t scope, method_type_t methods, cmd_cb_t cb, void *data,
    const char *abbrev, const cmd_arg_desc_t *arg_desc,
    uint8_t arg_count, cmd_def_t *parent)
{
  cmd_def_t *d = mem_alloc("cmd", "def", sizeof(*d));
  memset(d, 0, sizeof(*d));

  if(module != NULL)
    strncpy(d->module, module, CMD_MODULE_SZ - 1);

  strncpy(d->name, name, CMD_NAME_SZ - 1);

  if(abbrev != NULL && abbrev[0] != '\0')
    strncpy(d->abbrev, abbrev, CMD_NAME_SZ - 1);

  d->usage       = usage;
  d->description = description;
  d->help_long   = help_long;

  strncpy(d->group, group, USERNS_GROUP_SZ - 1);
  d->level   = level;
  d->scope   = scope;
  d->cb      = cb;
  d->data    = data;
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

// Register a command globally.
bool
cmd_register(const char *module, const char *name,
    const char *usage, const char *description,
    const char *help_long, const char *group, uint16_t level,
    cmd_scope_t scope, method_type_t methods, cmd_cb_t cb, void *data,
    const char *parent_path, const char *abbrev,
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

  if(!reg_check_collisions_locked(name, abbrev, parent))
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(FAIL);
  }

  cmd_def_t *d = reg_populate_def(module, name, usage, description,
      help_long, group, level, scope, methods, cb, data, abbrev,
      arg_desc, arg_count, parent);

  pthread_mutex_unlock(&cmd_mutex);

  clam(CLAM_DEBUG, "cmd_register",
      "registered '%s' (module: %s, group: %s, level: %u%s%s)", name,
      module ? module : "(none)", group,
      (unsigned)level,
      d->parent ? ", parent: " : "",
      d->parent ? d->parent->name : "");
  return(SUCCESS);
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

// Clean up per-bot command state (prefix set).
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
        snprintf(buf, sizeof(buf), "usage: /%s", usage);
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
// the prefix and command name, checks permissions, and submits a task
// for async execution.
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

      for(cmd_def_t *p = d; p != NULL && depth < 16; p = p->parent)
        parts[depth++] = p->name;

      size_t off = 0;
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
      cmd_def_t *help_cmd = def_find_locked("help");
      if(help_cmd != NULL && help_cmd->cb != NULL)
      {
        cmd_cb_t help_cb = help_cmd->cb;
        pthread_mutex_unlock(&cmd_mutex);

        // Build a synthetic context with the path as args.
        cmd_ctx_t help_ctx = {
          .bot = inst,
          .msg = msg,
          .args = help_args,
          .username = NULL,
          .parsed = NULL,
          .data = help_cmd->data,
        };

        // For console origin, set username to owner.
        if(msg->console_origin)
          help_ctx.username = USERNS_OWNER_USER;

        help_cb(&help_ctx);
        __atomic_fetch_add(&cmd_stat_dispatches, 1, __ATOMIC_RELAXED);
        return(SUCCESS);
      }
      pthread_mutex_unlock(&cmd_mutex);
      return(FAIL);
    }
  }

  // Method type check: reject if the command is not visible on this method.
  method_type_t inst_type = method_inst_type(msg->inst);

  if(inst_type != 0 && !(d->methods & inst_type))
  {
    pthread_mutex_unlock(&cmd_mutex);
    return(FAIL);
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
  const char *usage = d->usage;

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
  td->usage     = usage;

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

// Permission check: can the caller see this command?
static bool
help_check_access(const cmd_ctx_t *ctx, const cmd_def_t *d)
{
  // Console bypasses all checks.
  if(ctx->msg != NULL && ctx->msg->console_origin)
    return true;

  // Method type filter.
  method_type_t mt = (ctx->msg != NULL && ctx->msg->inst != NULL)
      ? method_inst_type(ctx->msg->inst) : METHOD_T_ANY;
  if(mt != 0 && !(d->methods & mt))
    return false;

  // Owner sees everything.
  if(ctx->username != NULL
      && strcmp(ctx->username, USERNS_OWNER_USER) == 0)
    return true;

  // "everyone" at level 0 is always visible.
  if(strcmp(d->group, USERNS_GROUP_EVERYONE) == 0 && d->level == 0)
    return true;

  // Authenticated: check group membership.
  if(ctx->username != NULL && ctx->bot != NULL)
  {
    userns_t *ns = bot_get_userns(ctx->bot);
    if(ns != NULL)
    {
      int32_t ulevel = userns_member_level(ns, ctx->username, d->group);
      if(ulevel >= 0 && (uint16_t)ulevel >= d->level)
        return true;
    }
  }

  return false;
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
  pthread_mutex_unlock(&cmd_mutex);

  return count;
}

// Built-in: help -- list all available commands, or show verbose help
// for a specific command.
//
// Usage:
//   /help            -- list all root commands
//   /help <command>  -- show usage and subcommands
//   /help -v <cmd>   -- verbose help (description + help_long)
static void
cmd_builtin_help(const cmd_ctx_t *ctx)
{
  // No arguments: list all root commands.
  if(ctx->args == NULL || ctx->args[0] == '\0')
  {
    cmd_reply(ctx, "Available commands:");

    char hdr[256];
    snprintf(hdr, sizeof(hdr), "  %-20s %-12s %s",
        "COMMAND", "ABBREV", "DESCRIPTION");
    cmd_reply(ctx, hdr);
    uint32_t count = 0;

    pthread_mutex_lock(&cmd_mutex);
    for(cmd_def_t *d = cmd_list; d != NULL; d = d->next)
    {
      if(d->parent != NULL)
        continue;
      if(!help_check_access(ctx, d))
        continue;

      const char *name = d->name;
      const char *abbrev = (d->abbrev[0] != '\0') ? d->abbrev : "-";
      const char *desc = d->description ? d->description : "";
      char line[256];
      snprintf(line, sizeof(line), "  %-20s %-12s %s", name, abbrev, desc);

      pthread_mutex_unlock(&cmd_mutex);
      cmd_reply(ctx, line);
      pthread_mutex_lock(&cmd_mutex);
      count++;
    }
    pthread_mutex_unlock(&cmd_mutex);

    char count_line[32];
    snprintf(count_line, sizeof(count_line), "%u command(s)", count);
    cmd_reply(ctx, count_line);
    cmd_reply(ctx, "Use /help <command> for detailed information.");
    return;
  }

  // Parse -v flag.
  const char *args = ctx->args;
  bool verbose = false;

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

  // Resolve command path (e.g., "bot add" -> root "bot", child "add").
  char cmd_path[CMD_USAGE_SZ] = {0};
  const char *hp = args;
  size_t hi = 0;

  while(hp[hi] != '\0' && hp[hi] != ' ' && hp[hi] != '\t'
      && hi < CMD_NAME_SZ - 1)
  {
    cmd_path[hi] = hp[hi];
    hi++;
  }

  const char *rest = hp + hi;
  while(*rest == ' ' || *rest == '\t')
    rest++;

  pthread_mutex_lock(&cmd_mutex);
  cmd_def_t *d = def_find_locked(cmd_path);

  if(d == NULL || d->parent != NULL)
  {
    pthread_mutex_unlock(&cmd_mutex);
    char buf[CMD_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "unknown command: %s", args);
    cmd_reply(ctx, buf);
    return;
  }

  // Resolve subcommand path, building full path string.
  while(d->children != NULL && rest[0] != '\0')
  {
    char tok[CMD_NAME_SZ] = {0};
    size_t ti = 0;

    while(rest[ti] != '\0' && rest[ti] != ' ' && rest[ti] != '\t'
        && ti < CMD_NAME_SZ - 1)
    {
      tok[ti] = rest[ti];
      ti++;
    }

    cmd_def_t *child = def_find_child_locked(d, tok);
    if(child == NULL)
      break;

    size_t plen = strlen(cmd_path);
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

  // Copy fields while holding the lock.
  const char *usage = d->usage;
  const char *description = d->description;
  const char *help_long = d->help_long;
  bool has_children = (d->children != NULL);

  pthread_mutex_unlock(&cmd_mutex);

  // Always show usage line.
  if(usage != NULL && usage[0] != '\0')
  {
    char line[CMD_USAGE_SZ + 16];
    snprintf(line, sizeof(line), "usage: /%s", usage);
    cmd_reply(ctx, line);
  }

  // Verbose: show description, help_long.
  if(verbose)
  {
    if(description != NULL && description[0] != '\0')
      cmd_reply(ctx, description);

    if(help_long != NULL && help_long[0] != '\0')
    {
      cmd_reply(ctx, "");
      // Send help_long line-by-line (split on newlines).
      size_t len = strlen(help_long);
      char *buf = mem_alloc("cmd", "help_buf", len + 1);
      memcpy(buf, help_long, len + 1);

      char *text = buf;
      char *nl;
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
    cmd_reply(ctx, "");

    char shdr[CMD_USAGE_SZ + 32];
    snprintf(shdr, sizeof(shdr), "subcommands of /%s:", cmd_path);
    cmd_reply(ctx, shdr);

    uint32_t child_count = help_show_children(ctx, d);

    char count_buf[32];
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

// Get the single-line description for a command definition.
// returns: description string, or NULL if def is NULL
// def: command definition
const char *
cmd_get_description(const cmd_def_t *def)
{
  return(def != NULL ? def->description : NULL);
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

// Iterate top-level root commands (parent == NULL).
// cb: callback invoked for each root command
// data: opaque user data passed to cb
void
cmd_iterate_root(cmd_iter_cb_t cb, void *data)
{
  if(cb == NULL)
    return;

  pthread_mutex_lock(&cmd_mutex);

  for(cmd_def_t *d = cmd_list; d != NULL; d = d->next)
  {
    if(d->parent != NULL)
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

// Dispatch a command as @owner on a given method instance.
// Executes synchronously.
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
  const char *usage = d->usage;

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

  // Register core built-in commands.
  cmd_register("core", "help", "help [-v] [command ...]",
      "Command reference",
      "Lists all commands available on this bot instance.\n"
      "Use /help <command> to see usage and subcommands.\n"
      "Use /help -v <command> for verbose help.",
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_builtin_help, NULL, NULL, "h", NULL, 0);

  cmd_register("core", "show", "show <subcommand> ...",
      "Show system information", NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_builtin_show, NULL, NULL, "sh", NULL, 0);

  cmd_register("core", "set", "set <subcommand> ...",
      "Configure system settings", NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_builtin_set, NULL, NULL, NULL, NULL, 0);

  cmd_register("core", "version", "version",
      "Show program version", NULL,
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_builtin_version, NULL, NULL, NULL, NULL, 0);

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
      "shutting down (%u commands, %u bot sets, %u freelisted sets)",
      cmd_def_count, set_count, cmd_set_free_count);

  cmd_ready = false;

  // Free all per-bot command sets.
  cmd_set_t *s = cmd_sets;

  while(s != NULL)
  {
    cmd_set_t *snext = s->next;
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
