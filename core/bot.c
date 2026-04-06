#define BOT_INTERNAL
#include "bot.h"

// Forward declaration — avoids circular include with cmd.h.
extern void cmd_get_dispatch_stats(uint64_t *dispatches, uint64_t *denials);

// Forward declaration — userns lifetime discovery counter.
extern uint64_t userns_stat_discoveries;

// -----------------------------------------------------------------------
// Method binding freelist management.
// -----------------------------------------------------------------------

// Get a method binding struct from the freelist or allocate a new one.
// returns: zeroed struct
static bot_method_t *
bm_get(void)
{
  bot_method_t *m;

  if(bot_method_freelist != NULL)
  {
    m = bot_method_freelist;
    bot_method_freelist = m->next;
    bot_method_free_count--;
    memset(m, 0, sizeof(*m));
    return(m);
  }

  m = mem_alloc("bot", "method", sizeof(*m));
  memset(m, 0, sizeof(*m));
  return(m);
}

// Return a method binding struct to the freelist.
// m: struct to recycle
static void
bm_put(bot_method_t *m)
{
  m->next = bot_method_freelist;
  bot_method_freelist = m;
  bot_method_free_count++;
}

// -----------------------------------------------------------------------
// Session freelist management.
// -----------------------------------------------------------------------

// Get a session struct from the freelist or allocate a new one.
// returns: zeroed struct
static bot_session_t *
sess_get(void)
{
  bot_session_t *s;

  if(bot_session_freelist != NULL)
  {
    s = bot_session_freelist;
    bot_session_freelist = s->next;
    bot_session_free_count--;
    memset(s, 0, sizeof(*s));
    return(s);
  }

  s = mem_alloc("bot", "session", sizeof(*s));
  memset(s, 0, sizeof(*s));
  return(s);
}

// Return a session struct to the freelist.
// s: struct to recycle
static void
sess_put(bot_session_t *s)
{
  s->next = bot_session_freelist;
  bot_session_freelist = s;
  bot_session_free_count++;
}

// Clear all sessions from a bot instance. Caller must hold bot_mutex.
// inst: bot instance
static void
sess_clear_locked(bot_inst_t *inst)
{
  bot_session_t *s = inst->sessions;

  while(s != NULL)
  {
    bot_session_t *next = s->next;
    sess_put(s);
    s = next;
  }

  inst->sessions = NULL;
  inst->session_count = 0;
}

// -----------------------------------------------------------------------
// Internal message forwarding callback.
// This is the method_msg_cb_t registered with method_subscribe().
// data points to the bot_inst_t.
// -----------------------------------------------------------------------
static void
bot_msg_handler(const method_msg_t *msg, void *data)
{
  bot_inst_t *bot = (bot_inst_t *)data;

  if(bot == NULL || bot->driver == NULL || bot->driver->on_message == NULL)
    return;

  bot->msg_count++;
  bot->last_activity = time(NULL);
  bot->driver->on_message(bot->handle, msg);
}

// -----------------------------------------------------------------------
// Instance management
// -----------------------------------------------------------------------

// Create a new bot instance.
// drv: bot driver interface (must not be NULL)
// name: unique instance name
// returns: instance pointer, or NULL on failure
bot_inst_t *
bot_create(const bot_driver_t *drv, const char *name)
{
  if(drv == NULL || name == NULL || name[0] == '\0')
  {
    clam(CLAM_WARN, "bot_create", "invalid arguments");
    return(NULL);
  }

  pthread_mutex_lock(&bot_mutex);

  // Check for duplicate name.
  for(bot_inst_t *b = bot_list; b != NULL; b = b->next)
  {
    if(strncasecmp(b->name, name, BOT_NAME_SZ) == 0)
    {
      pthread_mutex_unlock(&bot_mutex);
      clam(CLAM_WARN, "bot_create",
          "duplicate instance name: '%s'", name);
      return(NULL);
    }
  }

  bot_inst_t *inst = mem_alloc("bot", "instance", sizeof(*inst));
  memset(inst, 0, sizeof(*inst));
  strncpy(inst->name, name, BOT_NAME_SZ - 1);
  inst->driver = drv;
  inst->state = BOT_CREATED;

  // Call driver create() if provided.
  if(drv->create != NULL)
  {
    inst->handle = drv->create(inst);

    if(inst->handle == NULL)
    {
      pthread_mutex_unlock(&bot_mutex);
      clam(CLAM_WARN, "bot_create",
          "driver create() failed for '%s'", name);
      mem_free(inst);
      return(NULL);
    }
  }

  // Prepend to list.
  inst->next = bot_list;
  bot_list = inst;
  bot_count++;

  pthread_mutex_unlock(&bot_mutex);

  // Register per-bot KV keys.
  {
    char key[KV_KEY_SZ];
    snprintf(key, sizeof(key), "bot.%s.maxidleauth", name);
    kv_register(key, KV_UINT32, "3600", NULL, NULL);

    snprintf(key, sizeof(key), "bot.%s.userdiscovery", name);
    kv_register(key, KV_UINT8, "0", NULL, NULL);
  }

  clam(CLAM_INFO, "bot_create",
      "created '%s' (driver: %s)", name, drv->name);
  return(inst);
}

// Destroy a bot instance.
// name: instance name
// returns: SUCCESS or FAIL
bool
bot_destroy(const char *name)
{
  if(name == NULL || name[0] == '\0')
    return(FAIL);

  pthread_mutex_lock(&bot_mutex);

  bot_inst_t *inst = NULL;
  bot_inst_t *prev = NULL;

  for(inst = bot_list; inst != NULL; prev = inst, inst = inst->next)
  {
    if(strncasecmp(inst->name, name, BOT_NAME_SZ) == 0)
      break;
  }

  if(inst == NULL)
  {
    pthread_mutex_unlock(&bot_mutex);
    clam(CLAM_WARN, "bot_destroy", "not found: '%s'", name);
    return(FAIL);
  }

  // Stop if running.
  if(inst->state == BOT_RUNNING)
  {
    // Inline stop logic to avoid recursive lock.
    inst->state = BOT_STOPPING;

    // Clear all active sessions.
    sess_clear_locked(inst);

    if(inst->driver->stop != NULL)
      inst->driver->stop(inst->handle);

    // Unsubscribe from all methods and destroy bot-created instances.
    for(bot_method_t *m = inst->methods; m != NULL; m = m->next)
    {
      if(m->subscribed && m->inst != NULL)
      {
        method_unsubscribe(m->inst, inst->name);
        m->subscribed = false;
      }

      if(m->created_by_bot && m->inst != NULL)
      {
        method_unregister(m->method_name);
        m->created_by_bot = false;
      }

      m->inst = NULL;
    }

    inst->state = BOT_CREATED;
  }

  // Call driver destroy().
  if(inst->driver->destroy != NULL && inst->handle != NULL)
    inst->driver->destroy(inst->handle);

  // Free all method bindings.
  bot_method_t *m = inst->methods;

  while(m != NULL)
  {
    bot_method_t *next = m->next;
    bm_put(m);
    m = next;
  }

  inst->methods = NULL;
  inst->method_count = 0;

  // Unlink from list.
  if(prev != NULL)
    prev->next = inst->next;
  else
    bot_list = inst->next;

  bot_count--;

  // Build KV prefix for namespace deletion.
  char prefix[BOT_NAME_SZ + 8];
  snprintf(prefix, sizeof(prefix), "bot.%s.", inst->name);

  char saved_name[BOT_NAME_SZ];
  strncpy(saved_name, inst->name, BOT_NAME_SZ - 1);
  saved_name[BOT_NAME_SZ - 1] = '\0';

  pthread_mutex_unlock(&bot_mutex);

  // Delete KV namespace outside lock (kv_delete_prefix has its own).
  // Skip during shutdown — the DB state must survive for bot_restore().
  if(bot_ready)
    kv_delete_prefix(prefix);

  clam(CLAM_INFO, "bot_destroy", "destroyed '%s'", saved_name);
  mem_free(inst);
  return(SUCCESS);
}

// Find an instance by name.
// name: instance name
// returns: instance pointer, or NULL
bot_inst_t *
bot_find(const char *name)
{
  if(name == NULL || name[0] == '\0')
    return(NULL);

  pthread_mutex_lock(&bot_mutex);

  for(bot_inst_t *b = bot_list; b != NULL; b = b->next)
  {
    if(strncasecmp(b->name, name, BOT_NAME_SZ) == 0)
    {
      pthread_mutex_unlock(&bot_mutex);
      return(b);
    }
  }

  pthread_mutex_unlock(&bot_mutex);
  return(NULL);
}

// Get the name of an instance.
// inst: bot instance
// returns: instance name string
const char *
bot_inst_name(const bot_inst_t *inst)
{
  if(inst == NULL)
    return("(null)");

  return(inst->name);
}

// -----------------------------------------------------------------------
// Method binding
// -----------------------------------------------------------------------

// Bind a method instance to this bot.
// inst: bot instance
// method_name: name of a method instance
// returns: SUCCESS or FAIL
bool
bot_bind_method(bot_inst_t *inst, const char *method_name,
    const char *method_kind)
{
  if(inst == NULL || method_name == NULL || method_name[0] == '\0')
  {
    clam(CLAM_WARN, "bot_bind_method", "invalid arguments");
    return(FAIL);
  }

  pthread_mutex_lock(&bot_mutex);

  if(inst->state != BOT_CREATED)
  {
    pthread_mutex_unlock(&bot_mutex);
    clam(CLAM_WARN, "bot_bind_method",
        "'%s': cannot bind while %s",
        inst->name, bot_state_name(inst->state));
    return(FAIL);
  }

  if(inst->method_count >= bot_cfg.max_methods)
  {
    pthread_mutex_unlock(&bot_mutex);
    clam(CLAM_WARN, "bot_bind_method",
        "'%s': method limit reached (%u)", inst->name, bot_cfg.max_methods);
    return(FAIL);
  }

  // Check for duplicate.
  for(bot_method_t *m = inst->methods; m != NULL; m = m->next)
  {
    if(strncasecmp(m->method_name, method_name, METHOD_NAME_SZ) == 0)
    {
      pthread_mutex_unlock(&bot_mutex);
      clam(CLAM_WARN, "bot_bind_method",
          "'%s': method '%s' already bound",
          inst->name, method_name);
      return(FAIL);
    }
  }

  bot_method_t *bm = bm_get();
  strncpy(bm->method_name, method_name, METHOD_NAME_SZ - 1);

  if(method_kind != NULL)
    strncpy(bm->method_kind, method_kind, PLUGIN_NAME_SZ - 1);

  // Prepend to list.
  bm->next = inst->methods;
  inst->methods = bm;
  inst->method_count++;

  pthread_mutex_unlock(&bot_mutex);

  clam(CLAM_DEBUG, "bot_bind_method",
      "'%s': bound method '%s'", inst->name, method_name);
  return(SUCCESS);
}

// Unbind a method from this bot.
// inst: bot instance
// method_name: method to unbind
// returns: SUCCESS or FAIL
bool
bot_unbind_method(bot_inst_t *inst, const char *method_name)
{
  if(inst == NULL || method_name == NULL || method_name[0] == '\0')
    return(FAIL);

  pthread_mutex_lock(&bot_mutex);

  if(inst->state != BOT_CREATED)
  {
    pthread_mutex_unlock(&bot_mutex);
    clam(CLAM_WARN, "bot_unbind_method",
        "'%s': cannot unbind while %s",
        inst->name, bot_state_name(inst->state));
    return(FAIL);
  }

  bot_method_t *m, *prev = NULL;

  for(m = inst->methods; m != NULL; prev = m, m = m->next)
  {
    if(strncasecmp(m->method_name, method_name, METHOD_NAME_SZ) == 0)
    {
      if(prev != NULL)
        prev->next = m->next;
      else
        inst->methods = m->next;

      inst->method_count--;
      bm_put(m);
      pthread_mutex_unlock(&bot_mutex);

      clam(CLAM_DEBUG, "bot_unbind_method",
          "'%s': unbound method '%s'", inst->name, method_name);
      return(SUCCESS);
    }
  }

  pthread_mutex_unlock(&bot_mutex);
  clam(CLAM_DEBUG, "bot_unbind_method",
      "'%s': method '%s' not bound", inst->name, method_name);
  return(FAIL);
}

// -----------------------------------------------------------------------
// Namespace binding
// -----------------------------------------------------------------------

// Set the user namespace for this bot instance.
// inst: bot instance
// ns_name: user namespace name (NULL to clear)
// returns: SUCCESS or FAIL
bool
bot_set_userns(bot_inst_t *inst, const char *ns_name)
{
  if(inst == NULL)
    return(FAIL);

  pthread_mutex_lock(&bot_mutex);

  if(inst->state != BOT_CREATED)
  {
    pthread_mutex_unlock(&bot_mutex);
    clam(CLAM_WARN, "bot_set_userns",
        "'%s': cannot set userns while %s",
        inst->name, bot_state_name(inst->state));
    return(FAIL);
  }

  if(ns_name == NULL)
  {
    inst->userns = NULL;
    pthread_mutex_unlock(&bot_mutex);
    clam(CLAM_DEBUG, "bot_set_userns", "'%s': cleared userns", inst->name);
    return(SUCCESS);
  }

  pthread_mutex_unlock(&bot_mutex);

  // userns_get has its own locking and may create the namespace.
  userns_t *ns = userns_get(ns_name);

  if(ns == NULL)
  {
    clam(CLAM_WARN, "bot_set_userns",
        "'%s': failed to get namespace '%s'", inst->name, ns_name);
    return(FAIL);
  }

  pthread_mutex_lock(&bot_mutex);
  inst->userns = ns;
  pthread_mutex_unlock(&bot_mutex);

  clam(CLAM_DEBUG, "bot_set_userns",
      "'%s': bound to namespace '%s'", inst->name, ns_name);
  return(SUCCESS);
}

// Get the user namespace bound to this bot instance.
// inst: bot instance
// returns: userns pointer, or NULL
userns_t *
bot_get_userns(const bot_inst_t *inst)
{
  if(inst == NULL)
    return(NULL);

  return(inst->userns);
}

// -----------------------------------------------------------------------
// Session tracking
// -----------------------------------------------------------------------

// Authenticate a user and create an active session.
// inst: bot instance
// method: method instance the user is on
// sender: protocol-level sender identity
// username: credential username
// password: credential password
// returns: auth result code
userns_auth_t
bot_session_auth(bot_inst_t *inst, method_inst_t *method,
    const char *sender, const char *username, const char *password)
{
  if(inst == NULL || method == NULL || sender == NULL ||
     sender[0] == '\0' || username == NULL || password == NULL)
    return(USERNS_AUTH_ERR);

  pthread_mutex_lock(&bot_mutex);

  if(inst->state != BOT_RUNNING)
  {
    pthread_mutex_unlock(&bot_mutex);
    clam(CLAM_WARN, "bot_session_auth",
        "'%s': not running", inst->name);
    return(USERNS_AUTH_ERR);
  }

  if(inst->userns == NULL)
  {
    pthread_mutex_unlock(&bot_mutex);
    clam(CLAM_WARN, "bot_session_auth",
        "'%s': no user namespace bound", inst->name);
    return(USERNS_AUTH_ERR);
  }

  userns_t *ns = inst->userns;
  pthread_mutex_unlock(&bot_mutex);

  // Get method context for logging (outside lock — method has own lock).
  char ctx[USERNS_MCTX_SZ] = {0};
  method_get_context(method, sender, ctx, sizeof(ctx));

  // Authenticate against the user namespace.
  userns_auth_t result = userns_auth(ns, username, password,
      ctx[0] != '\0' ? ctx : NULL);

  if(result != USERNS_AUTH_OK)
    return(result);

  // Auth succeeded — create or update session.
  pthread_mutex_lock(&bot_mutex);

  // Check for existing session on this method+sender.
  for(bot_session_t *s = inst->sessions; s != NULL; s = s->next)
  {
    if(s->method == method &&
       strncasecmp(s->sender, sender, METHOD_SENDER_SZ) == 0)
    {
      // Replace existing session (re-auth or different user).
      strncpy(s->username, username, USERNS_USER_SZ - 1);
      s->username[USERNS_USER_SZ - 1] = '\0';
      s->login_time = time(NULL);
      s->auth_time  = s->login_time;
      s->last_seen  = s->login_time;

      pthread_mutex_unlock(&bot_mutex);
      clam(CLAM_INFO, "bot_session_auth",
          "'%s': session updated for '%s' on %s (%s)",
          inst->name, username,
          method_inst_name(method), sender);
      return(USERNS_AUTH_OK);
    }
  }

  // No existing session — check limit.
  if(inst->session_count >= bot_cfg.max_sessions)
  {
    pthread_mutex_unlock(&bot_mutex);
    clam(CLAM_WARN, "bot_session_auth",
        "'%s': session limit reached (%u)", inst->name, bot_cfg.max_sessions);
    return(USERNS_AUTH_ERR);
  }

  // Create new session.
  bot_session_t *s = sess_get();
  strncpy(s->username, username, USERNS_USER_SZ - 1);
  s->username[USERNS_USER_SZ - 1] = '\0';
  s->method = method;
  strncpy(s->sender, sender, METHOD_SENDER_SZ - 1);
  s->sender[METHOD_SENDER_SZ - 1] = '\0';
  s->login_time = time(NULL);
  s->auth_time  = s->login_time;
  s->last_seen  = s->login_time;

  // Prepend to list.
  s->next = inst->sessions;
  inst->sessions = s;
  inst->session_count++;

  pthread_mutex_unlock(&bot_mutex);

  clam(CLAM_INFO, "bot_session_auth",
      "'%s': session created for '%s' on %s (%s)",
      inst->name, username,
      method_inst_name(method), sender);
  return(USERNS_AUTH_OK);
}

// Find an active session by method and sender.
// returns: authenticated username, or NULL if anonymous
const char *
bot_session_find(const bot_inst_t *inst,
    const method_inst_t *method, const char *sender)
{
  if(inst == NULL || method == NULL || sender == NULL || sender[0] == '\0')
    return(NULL);

  pthread_mutex_lock(&bot_mutex);

  for(bot_session_t *s = inst->sessions; s != NULL; s = s->next)
  {
    if(s->method == method &&
       strncasecmp(s->sender, sender, METHOD_SENDER_SZ) == 0)
    {
      s->last_seen = time(NULL);
      const char *name = s->username;
      pthread_mutex_unlock(&bot_mutex);
      return(name);
    }
  }

  pthread_mutex_unlock(&bot_mutex);
  return(NULL);
}

// Extended session lookup with MFA fallback.
// returns: username, or NULL if anonymous and no MFA match
const char *
bot_session_find_ex(const bot_inst_t *inst,
    const method_inst_t *method, const char *sender,
    const char *mfa_string, bool *is_authed)
{
  // First try the normal authenticated session lookup.
  const char *user = bot_session_find(inst, method, sender);

  if(user != NULL)
  {
    if(is_authed != NULL)
      *is_authed = true;
    return(user);
  }

  // No authenticated session. Try MFA pattern matching if we have
  // a namespace and an MFA string to match against.
  if(mfa_string == NULL || mfa_string[0] == '\0' || inst == NULL)
    return(NULL);

  userns_t *ns = bot_get_userns(inst);

  if(ns == NULL)
    return(NULL);

  const char *matched = userns_mfa_match(ns, mfa_string);

  if(matched != NULL)
  {
    if(is_authed != NULL)
      *is_authed = false;
    return(matched);
  }

  return(NULL);
}

// Attempt user discovery from an MFA string.
// returns: discovered username (static buffer), or NULL
const char *
bot_discover_user(bot_inst_t *inst, const char *mfa_string)
{
  if(inst == NULL || mfa_string == NULL || mfa_string[0] == '\0')
    return(NULL);

  // Check if discovery is enabled for this bot.
  char key[KV_KEY_SZ];
  snprintf(key, sizeof(key), "bot.%s.userdiscovery", inst->name);
  uint8_t enabled = (uint8_t)kv_get_uint(key);

  if(enabled == 0)
    return(NULL);

  userns_t *ns = bot_get_userns(inst);

  if(ns == NULL)
    return(NULL);

  // Check if MFA already matches an existing user.
  if(userns_mfa_match(ns, mfa_string) != NULL)
    return(NULL);

  // Parse handle from MFA string (portion before '!').
  const char *bang = strchr(mfa_string, '!');

  if(bang == NULL || bang == mfa_string)
    return(NULL);

  static _Thread_local char discovered[USERNS_USER_SZ];
  size_t hlen = (size_t)(bang - mfa_string);

  if(hlen >= USERNS_USER_SZ)
    hlen = USERNS_USER_SZ - 1;

  // Copy handle, keeping only alphanumeric characters.
  size_t j = 0;

  for(size_t i = 0; i < hlen && j < USERNS_USER_SZ - 1; i++)
  {
    char c = mfa_string[i];

    if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
       (c >= '0' && c <= '9'))
      discovered[j++] = c;
  }

  if(j == 0)
    return(NULL);

  discovered[j] = '\0';

  // Resolve collisions with numeric suffix.
  char candidate[USERNS_USER_SZ];
  strncpy(candidate, discovered, USERNS_USER_SZ - 1);
  candidate[USERNS_USER_SZ - 1] = '\0';

  for(uint32_t suffix = 1;
      userns_user_exists(ns, candidate) && suffix < 1000;
      suffix++)
  {
    snprintf(candidate, sizeof(candidate), "%.*s%u",
        (int)(USERNS_USER_SZ - 5), discovered, suffix);
  }

  if(userns_user_exists(ns, candidate))
    return(NULL);   // all candidates exhausted

  // Create the password-less user.
  if(userns_user_create_nopass(ns, candidate) != SUCCESS)
    return(NULL);

  // Add the triggering MFA pattern.
  userns_user_add_mfa(ns, candidate, mfa_string);

  bot_stat_discoveries++;
  __atomic_add_fetch(&userns_stat_discoveries, 1, __ATOMIC_RELAXED);

  clam(CLAM_INFO, "bot_discover_user",
      "'%s': discovered user '%s' from '%s'",
      inst->name, candidate, mfa_string);

  strncpy(discovered, candidate, USERNS_USER_SZ - 1);
  discovered[USERNS_USER_SZ - 1] = '\0';
  return(discovered);
}

// Remove an active session (logout).
// returns: SUCCESS or FAIL
bool
bot_session_remove(bot_inst_t *inst, const method_inst_t *method,
    const char *sender)
{
  if(inst == NULL || method == NULL || sender == NULL || sender[0] == '\0')
    return(FAIL);

  pthread_mutex_lock(&bot_mutex);

  bot_session_t *s, *prev = NULL;

  for(s = inst->sessions; s != NULL; prev = s, s = s->next)
  {
    if(s->method == method &&
       strncasecmp(s->sender, sender, METHOD_SENDER_SZ) == 0)
    {
      if(prev != NULL)
        prev->next = s->next;
      else
        inst->sessions = s->next;

      inst->session_count--;

      char uname[USERNS_USER_SZ];
      strncpy(uname, s->username, USERNS_USER_SZ);

      sess_put(s);

      pthread_mutex_unlock(&bot_mutex);

      clam(CLAM_INFO, "bot_session_remove",
          "'%s': removed session for '%s' on %s (%s)",
          inst->name, uname,
          method_inst_name(method), sender);
      return(SUCCESS);
    }
  }

  pthread_mutex_unlock(&bot_mutex);
  return(FAIL);
}

// Remove all active sessions for a bot instance.
// inst: bot instance
void
bot_session_clear(bot_inst_t *inst)
{
  if(inst == NULL)
    return;

  pthread_mutex_lock(&bot_mutex);

  uint32_t count = inst->session_count;
  sess_clear_locked(inst);

  pthread_mutex_unlock(&bot_mutex);

  if(count > 0)
    clam(CLAM_DEBUG, "bot_session_clear",
        "'%s': cleared %u sessions", inst->name, count);
}

// Get active session count for a bot instance.
// returns: session count
// inst: bot instance
uint32_t
bot_session_count(const bot_inst_t *inst)
{
  if(inst == NULL)
    return(0);

  return(inst->session_count);
}

// Iterate active sessions for a bot instance.
// inst: bot instance
// cb: callback invoked for each session
// data: opaque user data
void
bot_session_iterate(const bot_inst_t *inst,
    bot_session_iter_cb_t cb, void *data)
{
  if(inst == NULL || cb == NULL)
    return;

  pthread_mutex_lock(&bot_mutex);

  for(bot_session_t *s = inst->sessions; s != NULL; s = s->next)
  {
    const char *mname = (s->method != NULL)
        ? method_inst_name(s->method) : "(unknown)";

    cb(s->username, mname, s->auth_time, s->last_seen, data);
  }

  pthread_mutex_unlock(&bot_mutex);
}

// -----------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------

// Rollback helper: unsubscribe and destroy bot-created method instances.
// Must be called with bot_mutex held. Rolls back methods from
// inst->methods up to (but not including) stop_at.
static void
bot_start_rollback(bot_inst_t *inst, bot_method_t *stop_at)
{
  for(bot_method_t *r = inst->methods; r != stop_at; r = r->next)
  {
    if(r->subscribed)
    {
      method_unsubscribe(r->inst, inst->name);
      r->subscribed = false;
    }

    if(r->created_by_bot)
    {
      method_unregister(r->method_name);
      r->created_by_bot = false;
    }

    r->inst = NULL;
  }
}

// Start a bot instance.
// inst: bot instance
// returns: SUCCESS or FAIL
bool
bot_start(bot_inst_t *inst)
{
  if(inst == NULL)
    return(FAIL);

  pthread_mutex_lock(&bot_mutex);

  if(inst->state != BOT_CREATED)
  {
    pthread_mutex_unlock(&bot_mutex);
    clam(CLAM_WARN, "bot_start",
        "'%s': cannot start from state %s",
        inst->name, bot_state_name(inst->state));
    return(FAIL);
  }

  // Resolve and subscribe to all bound methods.
  for(bot_method_t *m = inst->methods; m != NULL; m = m->next)
  {
    m->inst = method_find(m->method_name);

    // If no existing instance, create one on demand from the plugin.
    if(m->inst == NULL && m->method_kind[0] != '\0')
    {
      const plugin_desc_t *pd =
          plugin_find_type(PLUGIN_METHOD, m->method_kind);

      if(pd != NULL && pd->ext != NULL)
      {
        const method_driver_t *mdrv = (const method_driver_t *)pd->ext;
        m->inst = method_register(mdrv, m->method_name);

        if(m->inst != NULL)
          m->created_by_bot = true;
      }
    }

    if(m->inst == NULL)
    {
      bot_start_rollback(inst, m);
      pthread_mutex_unlock(&bot_mutex);
      clam(CLAM_WARN, "bot_start",
          "'%s': method '%s' not found (kind: %s)",
          inst->name, m->method_name, m->method_kind);
      return(FAIL);
    }

    if(method_subscribe(m->inst, inst->name, bot_msg_handler, inst) != SUCCESS)
    {
      bot_start_rollback(inst, m);
      m->inst = NULL;
      pthread_mutex_unlock(&bot_mutex);
      clam(CLAM_WARN, "bot_start",
          "'%s': failed to subscribe to '%s'",
          inst->name, m->method_name);
      return(FAIL);
    }

    m->subscribed = true;
  }

  // Call driver start().
  if(inst->driver->start != NULL)
  {
    if(inst->driver->start(inst->handle) != SUCCESS)
    {
      bot_start_rollback(inst, NULL);
      pthread_mutex_unlock(&bot_mutex);
      clam(CLAM_WARN, "bot_start",
          "'%s': driver start() failed", inst->name);
      return(FAIL);
    }
  }

  inst->state = BOT_RUNNING;

  pthread_mutex_unlock(&bot_mutex);

  // Connect bot-created method instances (outside lock — connect may
  // initiate async I/O with its own locking).
  for(bot_method_t *m = inst->methods; m != NULL; m = m->next)
  {
    if(m->created_by_bot && m->inst != NULL)
      method_connect(m->inst);
  }

  clam(CLAM_INFO, "bot_start", "'%s' started (%u methods)",
      inst->name, inst->method_count);
  return(SUCCESS);
}

// Stop a bot instance.
// inst: bot instance
// returns: SUCCESS or FAIL
bool
bot_stop(bot_inst_t *inst)
{
  if(inst == NULL)
    return(FAIL);

  pthread_mutex_lock(&bot_mutex);

  if(inst->state != BOT_RUNNING)
  {
    pthread_mutex_unlock(&bot_mutex);
    clam(CLAM_WARN, "bot_stop",
        "'%s': cannot stop from state %s",
        inst->name, bot_state_name(inst->state));
    return(FAIL);
  }

  inst->state = BOT_STOPPING;

  // Clear all active sessions.
  sess_clear_locked(inst);

  // Call driver stop().
  if(inst->driver->stop != NULL)
    inst->driver->stop(inst->handle);

  // Unsubscribe from all methods and destroy bot-created instances.
  for(bot_method_t *m = inst->methods; m != NULL; m = m->next)
  {
    if(m->subscribed && m->inst != NULL)
    {
      method_unsubscribe(m->inst, inst->name);
      m->subscribed = false;
    }

    if(m->created_by_bot && m->inst != NULL)
    {
      method_unregister(m->method_name);
      m->created_by_bot = false;
    }

    m->inst = NULL;
  }

  inst->state = BOT_CREATED;

  pthread_mutex_unlock(&bot_mutex);

  clam(CLAM_INFO, "bot_stop", "'%s' stopped", inst->name);
  return(SUCCESS);
}

// -----------------------------------------------------------------------
// State and statistics
// -----------------------------------------------------------------------

// Get current state.
// inst: bot instance
// returns: current state
bot_state_t
bot_get_state(const bot_inst_t *inst)
{
  if(inst == NULL)
    return(BOT_CREATED);

  return(inst->state);
}

// returns: human-readable name of a bot state
const char *
bot_state_name(bot_state_t s)
{
  switch(s)
  {
    case BOT_CREATED:  return("CREATED");
    case BOT_RUNNING:  return("RUNNING");
    case BOT_STOPPING: return("STOPPING");
    default:           return("UNKNOWN");
  }
}

// Get bot subsystem statistics.
// out: destination for the snapshot
void
bot_get_stats(bot_stats_t *out)
{
  if(out == NULL)
    return;

  pthread_mutex_lock(&bot_mutex);

  out->instances        = bot_count;
  out->running          = 0;
  out->sessions         = 0;
  out->methods          = 0;
  out->discovered_users = bot_stat_discoveries;

  for(bot_inst_t *b = bot_list; b != NULL; b = b->next)
  {
    if(b->state == BOT_RUNNING)
      out->running++;
    out->sessions += b->session_count;
    out->methods  += b->method_count;
  }

  pthread_mutex_unlock(&bot_mutex);

  cmd_get_dispatch_stats(&out->cmd_dispatches, &out->cmd_denials);
}

// -----------------------------------------------------------------------
// Iteration
// -----------------------------------------------------------------------

// Iterate all bot instances.
// cb: callback for each instance
// data: opaque user data
void
bot_iterate(bot_iter_cb_t cb, void *data)
{
  if(cb == NULL)
    return;

  pthread_mutex_lock(&bot_mutex);

  for(bot_inst_t *b = bot_list; b != NULL; b = b->next)
  {
    const char *drv_name = (b->driver && b->driver->name)
        ? b->driver->name : "(unknown)";
    const char *ns_name = (b->userns != NULL)
        ? b->userns->name : NULL;

    cb(b->name, drv_name, b->state, b->method_count,
        b->session_count, ns_name, b->cmd_count,
        b->last_activity, data);
  }

  pthread_mutex_unlock(&bot_mutex);
}

// Get the driver name for a bot instance.
// returns: driver name string, or "(unknown)" if unavailable
// inst: bot instance
const char *
bot_driver_name(const bot_inst_t *inst)
{
  if(inst == NULL || inst->driver == NULL || inst->driver->name == NULL)
    return("(unknown)");

  return(inst->driver->name);
}

// Get the number of bound methods for a bot instance.
// returns: method count
// inst: bot instance
uint32_t
bot_method_count(const bot_inst_t *inst)
{
  if(inst == NULL)
    return(0);

  return(inst->method_count);
}

// Increment the command dispatch counter for a bot instance.
// inst: bot instance
void
bot_inc_cmd_count(bot_inst_t *inst)
{
  if(inst != NULL)
    __atomic_add_fetch(&inst->cmd_count, 1, __ATOMIC_RELAXED);
}

// -----------------------------------------------------------------------
// Subsystem lifecycle
// -----------------------------------------------------------------------

// Initialize the bot subsystem. Sets up the mutex and marks the
// subsystem as ready. Must be called before any other bot_* functions.
void
bot_init(void)
{
  pthread_mutex_init(&bot_mutex, NULL);
  bot_ready = true;

  clam(CLAM_INFO, "bot_init", "bot subsystem initialized");
}

// -----------------------------------------------------------------------
// KV configuration
// -----------------------------------------------------------------------

// Load bot configuration values from KV into bot_cfg. Clamps
// max_methods to [1, 64] and max_sessions to >= 1.
static void
bot_load_config(void)
{
  bot_cfg.max_methods  = (uint32_t)kv_get_uint("core.bot.max_methods");
  bot_cfg.max_sessions = (uint32_t)kv_get_uint("core.bot.max_sessions");

  if(bot_cfg.max_methods < 1)   bot_cfg.max_methods = 1;
  if(bot_cfg.max_methods > 64)  bot_cfg.max_methods = 64;
  if(bot_cfg.max_sessions < 1)  bot_cfg.max_sessions = 1;
}

// KV change callback for bot configuration keys. Reloads config
// whenever a watched key is modified.
// key: the KV key that changed (unused)
// data: opaque callback data (unused)
static void
bot_kv_changed(const char *key, void *data)
{
  (void)key;
  (void)data;
  bot_load_config();
}

// -----------------------------------------------------------------------
// Session idle expiry
// -----------------------------------------------------------------------

// Periodic task callback: scan all running bot instances and expire
// sessions where (now - last_seen) exceeds the bot's maxidleauth.
static void
bot_session_reaper(task_t *t)
{
  time_t now = time(NULL);

  pthread_mutex_lock(&bot_mutex);

  for(bot_inst_t *inst = bot_list; inst != NULL; inst = inst->next)
  {
    if(inst->state != BOT_RUNNING || inst->sessions == NULL)
      continue;

    // Read per-bot maxidleauth (0 = never expire).
    char key[KV_KEY_SZ];
    snprintf(key, sizeof(key), "bot.%s.maxidleauth", inst->name);
    uint32_t maxidle = (uint32_t)kv_get_uint(key);

    if(maxidle == 0)
      continue;

    bot_session_t *s = inst->sessions;
    bot_session_t *prev = NULL;

    while(s != NULL)
    {
      bot_session_t *next = s->next;

      if((now - s->last_seen) > (time_t)maxidle)
      {
        clam(CLAM_INFO, "bot_session_reaper",
            "'%s': expired session for '%s' on %s (%s) "
            "(idle %ld sec, limit %u sec)",
            inst->name, s->username,
            method_inst_name(s->method), s->sender,
            (long)(now - s->last_seen), maxidle);

        // Unlink.
        if(prev != NULL)
          prev->next = next;
        else
          inst->sessions = next;

        inst->session_count--;

        // Return to freelist.
        sess_put(s);
      }
      else
      {
        prev = s;
      }

      s = next;
    }
  }

  pthread_mutex_unlock(&bot_mutex);

  t->state = TASK_ENDED;
}

// Register bot subsystem KV keys and load initial values. Also
// starts the periodic session reaper task. Must be called after
// kv_init() and kv_load().
void
bot_register_config(void)
{
  kv_register("core.bot.max_methods",  KV_UINT32, "16",  bot_kv_changed, NULL);
  kv_register("core.bot.max_sessions", KV_UINT32, "256", bot_kv_changed, NULL);
  bot_load_config();

  // Start periodic session reaper (every 60 seconds).
  task_add_periodic("bot_session_reaper", TASK_ANY, 200,
      60000, bot_session_reaper, NULL);
}

// -----------------------------------------------------------------------
// Per-bot method KV registration
// -----------------------------------------------------------------------

// Register per-bot method KV keys by copying the method plugin's
// instance KV schema into the bot's namespace. Transforms bare
// suffix keys to bot.<botname>.<kind>.* with the same types and defaults.
// returns: number of keys registered (0 if plugin not found or no schema)
// botname: bot instance name
// method_kind: method plugin kind (e.g., "irc")
uint32_t
bot_register_method_kv(const char *botname, const char *method_kind)
{
  if(botname == NULL || botname[0] == '\0' ||
     method_kind == NULL || method_kind[0] == '\0')
    return(0);

  // Find the method plugin by kind.
  const plugin_desc_t *pd = plugin_find_type(PLUGIN_METHOD, method_kind);

  if(pd == NULL || pd->kv_inst_schema == NULL ||
     pd->kv_inst_schema_count == 0)
    return(0);

  // Build the bot prefix: "bot.<botname>.<kind>."
  char bot_prefix[KV_KEY_SZ];
  snprintf(bot_prefix, sizeof(bot_prefix),
      "bot.%s.%s.", botname, method_kind);

  uint32_t registered = 0;

  for(uint32_t i = 0; i < pd->kv_inst_schema_count; i++)
  {
    const plugin_kv_entry_t *e = &pd->kv_inst_schema[i];

    if(e->key == NULL)
      continue;

    // Instance schema keys are bare suffixes (e.g., "nick", "network").
    // Build the per-bot key: "bot.<botname>.<kind>.<suffix>"
    char new_key[KV_KEY_SZ];
    snprintf(new_key, sizeof(new_key), "%s%s", bot_prefix, e->key);

    if(kv_register(new_key, e->type, e->default_val, NULL, NULL) == SUCCESS)
      registered++;
  }

  if(registered > 0)
    clam(CLAM_DEBUG, "bot_register_method_kv",
        "'%s': registered %u KV key(s) for method '%s'",
        botname, registered, method_kind);

  return(registered);
}

// -----------------------------------------------------------------------
// Database persistence
// -----------------------------------------------------------------------

// Create bot persistence tables (bot_instances, bot_methods) in the
// database if they do not already exist. Must be called after db_init().
// returns: SUCCESS or FAIL
bool
bot_ensure_tables(void)
{
  static const char *ddl[] =
  {
    "CREATE TABLE IF NOT EXISTS bot_instances ("
      "name VARCHAR(64) PRIMARY KEY, "
      "kind VARCHAR(64) NOT NULL, "
      "userns_name VARCHAR(64), "
      "auto_start BOOLEAN NOT NULL DEFAULT FALSE, "
      "created TIMESTAMPTZ NOT NULL DEFAULT NOW())",

    "CREATE TABLE IF NOT EXISTS bot_methods ("
      "bot_name VARCHAR(64) NOT NULL "
        "REFERENCES bot_instances(name) ON DELETE CASCADE, "
      "method_kind VARCHAR(64) NOT NULL, "
      "PRIMARY KEY(bot_name, method_kind))",

    NULL
  };

  for(const char **sql = ddl; *sql != NULL; sql++)
  {
    db_result_t *r = db_result_alloc();

    if(db_query(*sql, r) != SUCCESS)
    {
      clam(CLAM_WARN, "bot", "DDL failed: %s", r->error);
      db_result_free(r);
      return(FAIL);
    }

    db_result_free(r);
  }

  clam(CLAM_DEBUG, "bot", "persistence tables ready");
  return(SUCCESS);
}

// Phase 1 helper: query bot_instances and recreate each one.
// Populates auto_names/auto_count with bots that need auto-start.
// restored: incremented for each successfully created instance.
// returns: SUCCESS or FAIL (FAIL only if the DB query itself fails)
static bool
bot_restore_instances(char auto_names[][BOT_NAME_SZ],
                      uint32_t *auto_count, uint32_t *restored)
{
  db_result_t *r = db_result_alloc();

  if(db_query("SELECT name, kind, userns_name, auto_start "
               "FROM bot_instances ORDER BY created", r) != SUCCESS)
  {
    clam(CLAM_WARN, "bot_restore",
        "failed to query bot_instances: %s", r->error);
    db_result_free(r);
    return(FAIL);
  }

  for(uint32_t i = 0; i < r->rows; i++)
  {
    const char *name       = db_result_get(r, i, 0);
    const char *kind       = db_result_get(r, i, 1);
    const char *userns     = db_result_get(r, i, 2);
    const char *auto_start = db_result_get(r, i, 3);

    if(name == NULL || kind == NULL)
      continue;

    // Find the bot plugin by kind.
    const plugin_desc_t *pd = plugin_find_type(PLUGIN_BOT, kind);

    if(pd == NULL || pd->ext == NULL)
    {
      clam(CLAM_WARN, "bot_restore",
          "'%s': no bot plugin with kind '%s'", name, kind);
      continue;
    }

    const bot_driver_t *drv = (const bot_driver_t *)pd->ext;
    bot_inst_t *inst = bot_create(drv, name);

    if(inst == NULL)
    {
      clam(CLAM_WARN, "bot_restore",
          "failed to create '%s'", name);
      continue;
    }

    // Set user namespace if configured.
    if(userns != NULL && userns[0] != '\0')
      bot_set_userns(inst, userns);

    // Track auto-start candidates.
    if(auto_start != NULL &&
        (auto_start[0] == 't' || auto_start[0] == 'T' ||
         auto_start[0] == '1'))
    {
      if(*auto_count < 64)
      {
        strncpy(auto_names[*auto_count], name, BOT_NAME_SZ - 1);
        auto_names[*auto_count][BOT_NAME_SZ - 1] = '\0';
        (*auto_count)++;
      }
    }

    (*restored)++;
  }

  db_result_free(r);
  return(SUCCESS);
}

// Phase 2 helper: query bot_methods and bind each method to its bot.
static void
bot_restore_methods(void)
{
  db_result_t *r = db_result_alloc();

  if(db_query("SELECT bot_name, method_kind FROM bot_methods", r) != SUCCESS)
  {
    clam(CLAM_WARN, "bot_restore",
        "failed to query bot_methods: %s", r->error);
    db_result_free(r);
    return;
  }

  for(uint32_t i = 0; i < r->rows; i++)
  {
    const char *bname = db_result_get(r, i, 0);
    const char *mkind = db_result_get(r, i, 1);

    if(bname == NULL || mkind == NULL)
      continue;

    bot_inst_t *inst = bot_find(bname);

    if(inst == NULL)
      continue;

    // Reconstruct method instance name: "{botname}_{method_kind}"
    char inst_name[BOT_NAME_SZ + METHOD_NAME_SZ + 2];
    snprintf(inst_name, sizeof(inst_name), "%s_%s", bname, mkind);

    bot_bind_method(inst, inst_name, mkind);

    // Register per-bot method KV keys (bot.<bname>.<mkind>.*).
    bot_register_method_kv(bname, mkind);
  }

  db_result_free(r);
}

// Phase 3 helper: auto-start bots that were previously running.
// returns: number of successfully started instances
static uint32_t
bot_restore_autostart(char auto_names[][BOT_NAME_SZ],
                      uint32_t auto_count)
{
  uint32_t started = 0;

  for(uint32_t i = 0; i < auto_count; i++)
  {
    bot_inst_t *inst = bot_find(auto_names[i]);

    if(inst == NULL)
      continue;

    if(bot_start(inst) == SUCCESS)
      started++;
    else
      clam(CLAM_WARN, "bot_restore",
          "failed to auto-start '%s'", auto_names[i]);
  }

  return(started);
}

// Restore bot instances from the database. Runs in three phases:
// (1) create instances from bot_instances, (2) bind methods from
// bot_methods, (3) auto-start previously running bots. Must be
// called after plugins are started and KV is loaded.
// returns: SUCCESS or FAIL
bool
bot_restore(void)
{
  uint32_t restored   = 0;
  uint32_t auto_count = 0;
  char     auto_names[64][BOT_NAME_SZ];

  if(bot_restore_instances(auto_names, &auto_count, &restored) != SUCCESS)
    return(FAIL);

  bot_restore_methods();

  uint32_t started = bot_restore_autostart(auto_names, auto_count);

  if(restored > 0)
    clam(CLAM_INFO, "bot_restore",
        "restored %u instance(s), %u auto-started", restored, started);

  return(SUCCESS);
}

// Shut down the bot subsystem. Stops and destroys all instances,
// frees the method binding and session freelists, and destroys
// the mutex.
void
bot_exit(void)
{
  if(!bot_ready)
    return;

  clam(CLAM_INFO, "bot_exit",
      "shutting down (%u instances, %u freelisted bindings, "
      "%u freelisted sessions)",
      bot_count, bot_method_free_count, bot_session_free_count);

  bot_ready = false;

  // Destroy all instances. Always remove the head.
  while(bot_list != NULL)
  {
    char name[BOT_NAME_SZ];
    strncpy(name, bot_list->name, BOT_NAME_SZ - 1);
    name[BOT_NAME_SZ - 1] = '\0';
    bot_destroy(name);
  }

  // Free the method binding freelist.
  bot_method_t *m = bot_method_freelist;

  while(m != NULL)
  {
    bot_method_t *next = m->next;
    mem_free(m);
    m = next;
  }

  bot_method_freelist = NULL;
  bot_method_free_count = 0;

  // Free the session freelist.
  bot_session_t *s = bot_session_freelist;

  while(s != NULL)
  {
    bot_session_t *next = s->next;
    mem_free(s);
    s = next;
  }

  bot_session_freelist = NULL;
  bot_session_free_count = 0;

  pthread_mutex_destroy(&bot_mutex);
}
