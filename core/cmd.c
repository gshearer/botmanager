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

  strncpy(s->prefix, "!", CMD_PREFIX_SZ - 1);
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
  strncpy(buf, path, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

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

// Unregister a command. Removes it from all bot instance bindings.
bool
cmd_unregister(const char *name)
{
  cmd_def_t *d = NULL;
  cmd_def_t *prev = NULL;
  cmd_def_t *child;

  if(name == NULL || name[0] == '\0')
    return(FAIL);

  pthread_mutex_lock(&cmd_mutex);

  for(d = cmd_list; d != NULL; prev = d, d = d->next)
    if(strncasecmp(d->name, name, CMD_NAME_SZ) == 0)
      break;

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
  child = d->children;

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

// True if the child's kind_filter accepts bot_kind. Callers in this
// file may hold cmd_mutex; no locking needed -- kind_filter arrays are
// caller-owned static storage, never mutated after registration.
// NOTE: file-local; declared static.
static bool
kind_filter_matches(const char *const *kind_filter, const char *bot_kind)
{
  if(kind_filter == NULL)
    return(true);           // kind-agnostic child matches anything

  if(bot_kind == NULL)
    return(false);          // kind-agnostic context, filtered child skipped

  for(size_t i = 0;
      i < CMD_KIND_FILTER_MAX && kind_filter[i] != NULL; i++)
    if(strcasecmp(kind_filter[i], bot_kind) == 0)
      return(true);

  return(false);
}

// Find a kind-scoped child. Name resolution matches cmd_find_child
// (exact-name then abbrev). On a name match, the child's kind_filter
// is tested; if the filter rejects the bot_kind, iteration continues.
const cmd_def_t *
cmd_find_child_for_kind(const cmd_def_t *parent, const char *name,
    const char *bot_kind)
{
  cmd_def_t *exact_match  = NULL;
  cmd_def_t *abbrev_match = NULL;

  if(parent == NULL || name == NULL || name[0] == '\0')
    return(NULL);

  pthread_mutex_lock(&cmd_mutex);

  for(cmd_def_t *c = parent->children; c != NULL; c = c->sibling)
  {
    if(!kind_filter_matches(c->kind_filter, bot_kind))
      continue;

    if(exact_match == NULL
        && strncasecmp(c->name, name, CMD_NAME_SZ) == 0)
      exact_match = c;
    else if(abbrev_match == NULL && c->abbrev[0] != '\0'
        && strncasecmp(c->abbrev, name, CMD_NAME_SZ) == 0)
      abbrev_match = c;
  }

  pthread_mutex_unlock(&cmd_mutex);
  return(exact_match != NULL ? exact_match : abbrev_match);
}

// Walk upward through parents, returning the first kind_filter's
// leading entry. NULL = kind-agnostic (no ancestor declares a filter).
const char *
cmd_kind_of(const cmd_def_t *def)
{
  for(const cmd_def_t *p = def; p != NULL; p = p->parent)
    if(p->kind_filter != NULL && p->kind_filter[0] != NULL)
      return(p->kind_filter[0]);

  return(NULL);
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
          strncpy(bufs[i], p, CMD_ARG_SZ - 1);
          bufs[i][CMD_ARG_SZ - 1] = '\0';
        }
      }

      else
      {
        strncpy(bufs[i], p, CMD_ARG_SZ - 1);
        bufs[i][CMD_ARG_SZ - 1] = '\0';
      }

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

// Task callback for async command execution. Parses args if the command
// has an arg spec, then invokes the command callback.
static void
cmd_task_cb(task_t *t)
{
  cmd_task_data_t *d = (cmd_task_data_t *)t->data;
  const char      *uname;
  bool             admin;

  cmd_ctx_t ctx = {
    .bot      = d->bot,
    .msg      = &d->msg,
    .args     = d->args,
    .username = d->username[0] != '\0' ? d->username : NULL,
    .parsed   = NULL,
    .data     = d->cb_data,
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

  uname = (d->username[0] != '\0') ? d->username : NULL;
  admin = cmd_caller_is_admin(d->bot, uname);

  if(admin)
    kv_admin_context_set(true);

  d->cb(&ctx);

  if(admin)
    kv_admin_context_set(false);

  mem_free(d);

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
  username = bot_session_find(inst, msg->inst, msg->sender);
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
      char pfx_key[KV_KEY_SZ];
      const char *kv_pfx;

      snprintf(pfx_key, sizeof(pfx_key),
          "bot.%s.%s.prefix", bname, kind);

      kv_pfx = kv_get_str(pfx_key);

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
  username = bot_session_find(inst, msg->inst, msg->sender);

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
  strncpy(td->args, args, METHOD_TEXT_SZ - 1);

  if(username != NULL)
    strncpy(td->username, username, USERNS_USER_SZ - 1);

  td->arg_desc  = arg_desc;
  td->arg_count = arg_count;
  td->usage     = usage;

  // Submit task.
  snprintf(task_name, sizeof(task_name), "cmd:%s", cmd_name);

  t = task_add(task_name, TASK_THREAD, 128, cmd_task_cb, td);

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

// Reply helper

bool
cmd_reply(const cmd_ctx_t *ctx, const char *text)
{
  if(ctx == NULL || text == NULL)
    return(FAIL);

  if(ctx->msg == NULL || ctx->msg->inst == NULL)
    return(FAIL);

  {
    const char *target = ctx->msg->channel[0] != '\0'
        ? ctx->msg->channel
        : ctx->msg->sender;

    return(method_send(ctx->msg->inst, target, text));
  }
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
  if(def == NULL || def->cb == NULL || ctx == NULL)
    return;

  if(def->arg_desc != NULL && def->arg_count > 0 && ctx->parsed == NULL)
  {
    cmd_args_t parsed;
    char       arg_bufs[CMD_MAX_ARGS][CMD_ARG_SZ];
    cmd_ctx_t  sub;

    if(!cmd_parse_args(ctx->args, def->arg_desc, def->arg_count,
        arg_bufs, &parsed, ctx, def->usage))
      return;

    sub = *ctx;
    sub.parsed = &parsed;
    (def->cb)(&sub);
    return;
  }

  (def->cb)(ctx);
}

// Command iteration

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

// Asserted-identity command dispatch

// Evaluate the group+level check. Returns true if allowed. When ns is
// NULL (no userns available — e.g., early-boot botmanctl), only the
// literal owner principal is accepted; anyone else is denied. This
// mirrors how bot-path dispatch treats a missing namespace.
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

  if(ns == NULL)
    // No namespace to resolve against. Only the literal owner (which
    // carries membership in all default groups at max level) passes.
    return(strcmp(username, USERNS_OWNER_USER) == 0);

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
    method_inst_t *inst, userns_t *ns, const char *username)
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

  pthread_mutex_unlock(&cmd_mutex);

  // Synthetic message (drives reply routing + logs).
  memset(&msg, 0, sizeof(msg));
  msg.inst = inst;
  snprintf(msg.sender, METHOD_SENDER_SZ, "%s",
      username != NULL ? username : "(anon)");
  msg.timestamp = time(NULL);

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
    method_send(inst, msg.sender, "Permission denied.");

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

  if(ad != NULL && ac > 0)
  {
    memset(arg_bufs, 0, sizeof(arg_bufs));
    if(!cmd_parse_args(args, ad, ac, arg_bufs, &parsed, &ctx, usage))
      return(SUCCESS);    // validation failed, error already sent

    ctx.parsed = &parsed;
  }

  {
    bool admin = (ns != NULL && username != NULL && username[0] != '\0' &&
        userns_member_check(ns, username, USERNS_GROUP_ADMIN));

    if(admin)
      kv_admin_context_set(true);

    cb(&ctx);

    if(admin)
      kv_admin_context_set(false);
  }

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

  if(inst == NULL || msg == NULL || def == NULL || def->cb == NULL)
    return(FAIL);

  username = bot_session_find(inst, msg->inst, msg->sender);

  td = mem_alloc("cmd", "task_data", sizeof(*td));
  memset(td, 0, sizeof(*td));
  td->cb        = def->cb;
  td->cb_data   = def->data;
  td->bot       = inst;
  memcpy(&td->msg, msg, sizeof(method_msg_t));

  if(args != NULL)
    strncpy(td->args, args, METHOD_TEXT_SZ - 1);

  if(username != NULL)
    strncpy(td->username, username, USERNS_USER_SZ - 1);

  td->arg_desc  = def->arg_desc;
  td->arg_count = def->arg_count;
  td->usage     = def->usage;

  snprintf(task_name, sizeof(task_name), "cmd:%s", def->name);

  t = task_add(task_name, TASK_THREAD, 128, cmd_task_cb, td);

  if(t == NULL)
  {
    clam(CLAM_WARN, "cmd_dispatch_resolved",
        "'%s': failed to submit task for '%s'",
        bot_inst_name(inst), def->name);
    mem_free(td);
    return(FAIL);
  }

  __atomic_add_fetch(&cmd_stat_dispatches, 1, __ATOMIC_RELAXED);

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

  pthread_mutex_destroy(&cmd_mutex);
}
