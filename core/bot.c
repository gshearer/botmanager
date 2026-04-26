// botmanager — MIT
// Bot-instance registry, lifecycle (add/start/stop/restore), method binding.
#define BOT_INTERNAL
#include "bot.h"
#include "cmd.h"

#include <fnmatch.h>
#include <regex.h>

// Forward declaration — avoids circular include with cmd.h.
extern void cmd_get_dispatch_stats(uint64_t *dispatches, uint64_t *denials);

// Forward declaration — userns lifetime discovery counter.
extern uint64_t userns_stat_discoveries;

// Method binding freelist management.

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

static void
bm_put(bot_method_t *m)
{
  m->next = bot_method_freelist;
  bot_method_freelist = m;
  bot_method_free_count++;
}

// Session freelist management.

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

static void
sess_put(bot_session_t *s)
{
  s->next = bot_session_freelist;
  bot_session_freelist = s;
  bot_session_free_count++;
}

// Clear all sessions from a bot instance. Caller must hold bot_mutex.
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

// Internal message forwarding callback.
// This is the method_msg_cb_t registered with method_subscribe().
// data points to the bot_inst_t.
// Match sender against a comma-separated ignore list. Each token is a
// shell-style glob pattern (?, *, [set]), matched case-insensitively
// against the full sender nick.
static bool
bot_sender_ignored(const char *list, const char *sender)
{
  const char *p;

  if(list == NULL || list[0] == '\0' || sender == NULL || sender[0] == '\0')
    return(false);

  p = list;

  while(*p != '\0')
  {
    const char *start;
    const char *end;
    size_t      tlen;
    char        pat[METHOD_SENDER_SZ];

    while(*p == ' ' || *p == '\t' || *p == ',') p++;
    start = p;
    while(*p != '\0' && *p != ',') p++;
    end = p;
    while(end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;

    tlen = (size_t)(end - start);
    if(tlen == 0) continue;

    if(tlen >= sizeof(pat)) tlen = sizeof(pat) - 1;
    memcpy(pat, start, tlen);
    pat[tlen] = '\0';

    if(fnmatch(pat, sender, FNM_CASEFOLD) == 0)
      return(true);
  }
  return(false);
}

static bool
bot_payload_ignored(const char *pattern, const char *text)
{
  regex_t re;
  int     rc;

  if(pattern == NULL || pattern[0] == '\0'
      || text == NULL || text[0] == '\0')
    return(false);

  if(regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB) != 0)
    return(false);

  rc = regexec(&re, text, 0, NULL, 0);
  regfree(&re);
  return(rc == 0);
}

static void
bot_msg_handler(const method_msg_t *msg, void *data)
{
  bot_inst_t *bot = (bot_inst_t *)data;

  if(bot == NULL || bot->driver == NULL || bot->driver->on_message == NULL)
    return;

  // Bot-level ignore_nicks (glob list). Cheapest filter — check first.
  {
    char        key[KV_KEY_SZ];
    const char *list;

    snprintf(key, sizeof(key), "bot.%s.ignore_nicks", bot->name);
    list = kv_get_str(key);
    if(bot_sender_ignored(list, msg->sender))
    {
      clam(CLAM_DEBUG, "bot_msg",
          "%s: dropped %s (ignore_nicks)",
          bot->name, msg->sender);
      return;
    }
  }

  // Bot-level ignore_regex (POSIX ERE matched against payload).
  {
    char        key[KV_KEY_SZ];
    const char *pattern;

    snprintf(key, sizeof(key), "bot.%s.ignore_regex", bot->name);
    pattern = kv_get_str(key);
    if(bot_payload_ignored(pattern, msg->text))
    {
      clam(CLAM_DEBUG, "bot_msg",
          "%s: dropped payload (ignore_regex)", bot->name);
      return;
    }
  }

  bot->msg_count++;
  bot->last_activity = time(NULL);
  bot->driver->on_message(bot->handle, msg);
}

// Instance management

// KV change callback for bot.<name>.userns.
// Extracts the bot name from the key, looks up the instance,
// and updates the userns binding.
static void
bot_userns_kv_cb(const char *key, void *data)
{
  const char *p;
  const char *end;
  char        botname[BOT_NAME_SZ];
  size_t      len;
  bot_inst_t *inst;
  const char *ns_val;

  (void)data;

  // key format: "bot.<botname>.userns"
  // Extract bot name between first and second dot.
  p = key;

  if(strncmp(p, "bot.", 4) != 0)
    return;

  p += 4;
  end = strchr(p, '.');

  if(end == NULL)
    return;

  len = (size_t)(end - p);

  if(len >= BOT_NAME_SZ)
    len = BOT_NAME_SZ - 1;

  memcpy(botname, p, len);
  botname[len] = '\0';

  inst = bot_find(botname);

  if(inst == NULL)
    return;

  ns_val = kv_get_str(key);

  if(ns_val == NULL || ns_val[0] == '\0')
    bot_set_userns(inst, NULL);
  else
    bot_set_userns(inst, ns_val);
}

// Create a new bot instance.
// drv: bot driver interface (must not be NULL)
bot_inst_t *
bot_create(const bot_driver_t *drv, const char *name)
{
  bot_inst_t *inst;

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

  inst = mem_alloc("bot", "instance", sizeof(*inst));
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
    snprintf(key, sizeof(key), "bot.%s.autostart", name);
    kv_register(key, KV_BOOL, "false", NULL, NULL,
        "Auto-start this bot on launch (true/false)");

    snprintf(key, sizeof(key), "bot.%s.maxidleauth", name);
    kv_register(key, KV_UINT32, "3600", NULL, NULL,
        "Seconds before idle authenticated sessions expire (0=never)");

    snprintf(key, sizeof(key), "bot.%s.userdiscovery", name);
    kv_register(key, KV_BOOL, "false", NULL, NULL,
        "Allow anonymous users to discover registered usernames (true/false)");

    snprintf(key, sizeof(key), "bot.%s.userns", name);
    kv_register(key, KV_STR, "", bot_userns_kv_cb, NULL,
        "User namespace name for this bot (empty = default)");

    snprintf(key, sizeof(key), "bot.%s.ignore_nicks", name);
    kv_register(key, KV_STR, "", NULL, NULL,
        "Comma-separated glob list of sender nicks to drop before"
        " dispatching messages to the driver (case-insensitive)");

    snprintf(key, sizeof(key), "bot.%s.ignore_regex", name);
    kv_register(key, KV_STR, "", NULL, NULL,
        "POSIX ERE matched against message payload; matching messages"
        " are dropped before dispatching to the driver");
  }

  clam(CLAM_INFO, "bot_create",
      "created '%s' (driver: %s)", name, drv->name);
  return(inst);
}

// Destroy a bot instance.
bool
bot_destroy(const char *name)
{
  bot_inst_t *inst = NULL;
  bot_inst_t *prev = NULL;

  if(name == NULL || name[0] == '\0')
    return(FAIL);

  pthread_mutex_lock(&bot_mutex);

  for(inst = bot_list; inst != NULL; prev = inst, inst = inst->next)
    if(strncasecmp(inst->name, name, BOT_NAME_SZ) == 0)
      break;

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
  {
    bot_method_t *m = inst->methods;

    while(m != NULL)
    {
      bot_method_t *next = m->next;
      bm_put(m);
      m = next;
    }
  }

  inst->methods = NULL;
  inst->method_count = 0;

  // Unlink from list.
  if(prev != NULL)
    prev->next = inst->next;
  else
    bot_list = inst->next;

  bot_count--;

  {
    // Build KV prefix for namespace deletion.
    char prefix[BOT_NAME_SZ + 8];
    char saved_name[BOT_NAME_SZ];

    snprintf(prefix, sizeof(prefix), "bot.%s.", inst->name);
    strncpy(saved_name, inst->name, BOT_NAME_SZ - 1);
    saved_name[BOT_NAME_SZ - 1] = '\0';

    pthread_mutex_unlock(&bot_mutex);

    // Delete KV namespace outside lock (kv_delete_prefix has its own).
    // Skip during shutdown — the DB state must survive for bot_restore().
    if(bot_ready)
      kv_delete_prefix(prefix);

    clam(CLAM_INFO, "bot_destroy", "destroyed '%s'", saved_name);
  }
  mem_free(inst);
  return(SUCCESS);
}

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

const char *
bot_inst_name(const bot_inst_t *inst)
{
  if(inst == NULL)
    return("(null)");

  return(inst->name);
}

// Method binding

bool
bot_bind_method(bot_inst_t *inst, const char *method_name,
    const char *method_kind)
{
  bot_method_t *bm;

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

  bm = bm_get();
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

  // Register per-method identity timeout KV.
  if(method_kind != NULL && method_kind[0] != '\0')
  {
    char key[KV_KEY_SZ];
    snprintf(key, sizeof(key), "bot.%s.%s.identtimeout",
        inst->name, method_kind);
    kv_register(key, KV_UINT32, "3600", NULL, NULL,
        "Seconds before identity cache expires for this method "
        "(0=use maxidleauth)");
  }

  return(SUCCESS);
}

bool
bot_unbind_method(bot_inst_t *inst, const char *method_name)
{
  bot_method_t *m, *prev = NULL;

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

// Namespace binding

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

  {
    // Look up existing namespace (does not create).
    userns_t *ns = userns_find(ns_name);

    if(ns == NULL)
    {
      clam(CLAM_WARN, "bot_set_userns",
          "'%s': namespace '%s' not found", inst->name, ns_name);
      return(FAIL);
    }

    pthread_mutex_lock(&bot_mutex);
    inst->userns = ns;
    pthread_mutex_unlock(&bot_mutex);
  }

  clam(CLAM_DEBUG, "bot_set_userns",
      "'%s': bound to namespace '%s'", inst->name, ns_name);
  return(SUCCESS);
}

userns_t *
bot_get_userns(const bot_inst_t *inst)
{
  if(inst == NULL)
    return(NULL);

  return(inst->userns);
}

// Clear the userns pointer on all bots bound to the named namespace.
// Used when a namespace is deleted. Works regardless of bot state.
void
bot_clear_userns(const char *ns_name)
{
  if(ns_name == NULL || ns_name[0] == '\0')
    return;

  pthread_mutex_lock(&bot_mutex);

  for(bot_inst_t *b = bot_list; b != NULL; b = b->next)
  {
    if(b->userns != NULL
        && strncasecmp(b->userns->name, ns_name, USERNS_NAME_SZ) == 0)
    {
      clam(CLAM_INFO, "bot_clear_userns",
          "'%s': namespace '%s' removed, clearing binding",
          b->name, ns_name);
      {
        char key[KV_KEY_SZ];

        b->userns = NULL;

        // Clear the KV key so the binding doesn't persist.
        snprintf(key, sizeof(key), "bot.%s.userns", b->name);
        kv_set_str(key, "");
      }
    }
  }

  pthread_mutex_unlock(&bot_mutex);
}

// Session tracking

userns_auth_t
bot_session_auth(bot_inst_t *inst, method_inst_t *method,
    const char *sender, const char *username, const char *password)
{
  userns_t     *ns;
  char          ctx[USERNS_MCTX_SZ] = {0};
  userns_auth_t result;

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

  ns = inst->userns;
  pthread_mutex_unlock(&bot_mutex);

  // Get method context for logging (outside lock — method has own lock).
  method_get_context(method, sender, ctx, sizeof(ctx));

  // Authenticate against the user namespace.
  result = userns_auth(ns, username, password,
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
  {
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
  }

  pthread_mutex_unlock(&bot_mutex);

  clam(CLAM_INFO, "bot_session_auth",
      "'%s': session created for '%s' on %s (%s)",
      inst->name, username,
      method_inst_name(method), sender);
  return(USERNS_AUTH_OK);
}

bool
bot_session_create(bot_inst_t *inst, method_inst_t *method,
    const char *sender, const char *username)
{
  if(inst == NULL || method == NULL || sender == NULL ||
     sender[0] == '\0' || username == NULL)
    return(FAIL);

  pthread_mutex_lock(&bot_mutex);

  if(inst->state != BOT_RUNNING)
  {
    pthread_mutex_unlock(&bot_mutex);
    return(FAIL);
  }

  // Check for existing session on this method+sender.
  for(bot_session_t *s = inst->sessions; s != NULL; s = s->next)
  {
    if(s->method == method &&
       strncasecmp(s->sender, sender, METHOD_SENDER_SZ) == 0)
    {
      strncpy(s->username, username, USERNS_USER_SZ - 1);
      s->username[USERNS_USER_SZ - 1] = '\0';
      s->login_time = time(NULL);
      s->auth_time  = s->login_time;
      s->last_seen  = s->login_time;

      pthread_mutex_unlock(&bot_mutex);
      clam(CLAM_INFO, "bot_session_create",
          "'%s': autoidentify session updated for '%s' on %s (%s)",
          inst->name, username,
          method_inst_name(method), sender);
      return(SUCCESS);
    }
  }

  // No existing session — check limit.
  if(inst->session_count >= bot_cfg.max_sessions)
  {
    pthread_mutex_unlock(&bot_mutex);
    clam(CLAM_WARN, "bot_session_create",
        "'%s': session limit reached (%u)", inst->name, bot_cfg.max_sessions);
    return(FAIL);
  }

  // Create new session.
  {
    bot_session_t *s = sess_get();
    strncpy(s->username, username, USERNS_USER_SZ - 1);
    s->username[USERNS_USER_SZ - 1] = '\0';
    s->method = method;
    strncpy(s->sender, sender, METHOD_SENDER_SZ - 1);
    s->sender[METHOD_SENDER_SZ - 1] = '\0';
    s->login_time = time(NULL);
    s->auth_time  = s->login_time;
    s->last_seen  = s->login_time;

    s->next = inst->sessions;
    inst->sessions = s;
    inst->session_count++;
  }

  pthread_mutex_unlock(&bot_mutex);

  clam(CLAM_INFO, "bot_session_create",
      "'%s': autoidentify session created for '%s' on %s (%s)",
      inst->name, username,
      method_inst_name(method), sender);
  return(SUCCESS);
}

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
      const char *name;

      s->last_seen = time(NULL);
      name = s->username;
      pthread_mutex_unlock(&bot_mutex);
      return(name);
    }
  }

  pthread_mutex_unlock(&bot_mutex);
  return(NULL);
}

const char *
bot_session_get_userns_cd(const bot_inst_t *inst,
    const method_inst_t *method, const char *sender)
{
  if(inst == NULL || method == NULL || sender == NULL || sender[0] == '\0')
    return("");

  pthread_mutex_lock(&bot_mutex);

  for(bot_session_t *s = inst->sessions; s != NULL; s = s->next)
  {
    if(s->method == method &&
       strncasecmp(s->sender, sender, METHOD_SENDER_SZ) == 0)
    {
      const char *cd = s->userns_cd;
      pthread_mutex_unlock(&bot_mutex);
      return(cd);
    }
  }

  pthread_mutex_unlock(&bot_mutex);
  return("");
}

bool
bot_session_set_userns_cd(bot_inst_t *inst,
    const method_inst_t *method, const char *sender,
    const char *ns_name)
{
  if(inst == NULL || method == NULL || sender == NULL || sender[0] == '\0')
    return(FAIL);

  pthread_mutex_lock(&bot_mutex);

  for(bot_session_t *s = inst->sessions; s != NULL; s = s->next)
  {
    if(s->method == method &&
       strncasecmp(s->sender, sender, METHOD_SENDER_SZ) == 0)
    {
      if(ns_name == NULL || ns_name[0] == '\0')
        s->userns_cd[0] = '\0';
      else
      {
        strncpy(s->userns_cd, ns_name, USERNS_NAME_SZ - 1);
        s->userns_cd[USERNS_NAME_SZ - 1] = '\0';
      }

      pthread_mutex_unlock(&bot_mutex);
      return(SUCCESS);
    }
  }

  pthread_mutex_unlock(&bot_mutex);
  return(FAIL);
}

const char *
bot_session_find_ex(const bot_inst_t *inst,
    const method_inst_t *method, const char *sender,
    const char *mfa_string, bool *is_authed)
{
  const char *user;
  userns_t   *ns;
  const char *matched;

  // First try the normal authenticated session lookup.
  user = bot_session_find(inst, method, sender);

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

  ns = bot_get_userns(inst);

  if(ns == NULL)
    return(NULL);

  matched = userns_mfa_match(ns, mfa_string);

  if(matched != NULL)
  {
    if(is_authed != NULL)
      *is_authed = false;
    return(matched);
  }

  return(NULL);
}

bool
bot_session_refresh_mfa(bot_inst_t *inst, method_inst_t *method,
    const char *username)
{
  char user[USERNS_USER_SZ];

  if(inst == NULL || method == NULL || username == NULL || username[0] == '\0')
    return(FAIL);

  // Copy username immediately — the caller may have passed a pointer
  // from userns_mfa_match()'s static buffer which can be overwritten.
  strncpy(user, username, USERNS_USER_SZ - 1);
  user[USERNS_USER_SZ - 1] = '\0';

  pthread_mutex_lock(&bot_mutex);

  for(bot_session_t *s = inst->sessions; s != NULL; s = s->next)
  {
    if(s->method == method &&
       strncasecmp(s->username, user, USERNS_USER_SZ) == 0)
    {
      s->last_seen = time(NULL);
      pthread_mutex_unlock(&bot_mutex);
      return(SUCCESS);
    }
  }

  pthread_mutex_unlock(&bot_mutex);
  return(FAIL);
}

const char *
bot_discover_user(bot_inst_t *inst, const char *mfa_string)
{
  static _Thread_local char discovered[USERNS_USER_SZ];
  char        key[KV_KEY_SZ];
  uint8_t     enabled;
  userns_t   *ns;
  const char *bang;
  size_t      hlen;
  size_t      j = 0;
  char        candidate[USERNS_USER_SZ];

  if(inst == NULL || mfa_string == NULL || mfa_string[0] == '\0')
    return(NULL);

  // Check if discovery is enabled for this bot.
  snprintf(key, sizeof(key), "bot.%s.userdiscovery", inst->name);
  enabled = (uint8_t)kv_get_uint(key);

  if(enabled == 0)
    return(NULL);

  ns = bot_get_userns(inst);

  if(ns == NULL)
    return(NULL);

  // Check if MFA already matches an existing user.
  if(userns_mfa_match(ns, mfa_string) != NULL)
    return(NULL);

  // Parse handle from MFA string (portion before '!').
  bang = strchr(mfa_string, '!');

  if(bang == NULL || bang == mfa_string)
    return(NULL);

  hlen = (size_t)(bang - mfa_string);

  if(hlen >= USERNS_USER_SZ)
    hlen = USERNS_USER_SZ - 1;

  // Copy handle, keeping only alphanumeric characters.
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
  strncpy(candidate, discovered, USERNS_USER_SZ - 1);
  candidate[USERNS_USER_SZ - 1] = '\0';

  for(uint32_t suffix = 1;
      userns_user_exists(ns, candidate) && suffix < 1000;
      suffix++)
    snprintf(candidate, sizeof(candidate), "%.*s%u",
        (int)(USERNS_USER_SZ - 5), discovered, suffix);

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

bool
bot_session_remove(bot_inst_t *inst, const method_inst_t *method,
    const char *sender)
{
  bot_session_t *s, *prev = NULL;

  if(inst == NULL || method == NULL || sender == NULL || sender[0] == '\0')
    return(FAIL);

  pthread_mutex_lock(&bot_mutex);

  for(s = inst->sessions; s != NULL; prev = s, s = s->next)
  {
    if(s->method == method &&
       strncasecmp(s->sender, sender, METHOD_SENDER_SZ) == 0)
    {
      if(prev != NULL)
        prev->next = s->next;
      else
        inst->sessions = s->next;

      {
        char uname[USERNS_USER_SZ];

        inst->session_count--;
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
  }

  pthread_mutex_unlock(&bot_mutex);
  return(FAIL);
}

void
bot_session_clear(bot_inst_t *inst)
{
  uint32_t count;

  if(inst == NULL)
    return;

  pthread_mutex_lock(&bot_mutex);

  count = inst->session_count;
  sess_clear_locked(inst);

  pthread_mutex_unlock(&bot_mutex);

  if(count > 0)
    clam(CLAM_DEBUG, "bot_session_clear",
        "'%s': cleared %u sessions", inst->name, count);
}

uint32_t
bot_session_count(const bot_inst_t *inst)
{
  if(inst == NULL)
    return(0);

  return(inst->session_count);
}

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

// Lifecycle

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
          plugin_find_type(PLUGIN_PROTOCOL, m->method_kind);

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
    if(m->created_by_bot && m->inst != NULL)
      method_connect(m->inst);

  clam(CLAM_INFO, "bot_start", "'%s' started (%u methods)",
      inst->name, inst->method_count);
  return(SUCCESS);
}

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

// State and statistics

bot_state_t
bot_get_state(const bot_inst_t *inst)
{
  if(inst == NULL)
    return(BOT_CREATED);

  return(inst->state);
}

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

// Iteration

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

const char *
bot_driver_name(const bot_inst_t *inst)
{
  if(inst == NULL || inst->driver == NULL || inst->driver->name == NULL)
    return("(unknown)");

  return(inst->driver->name);
}

uint32_t
bot_method_count(const bot_inst_t *inst)
{
  if(inst == NULL)
    return(0);

  return(inst->method_count);
}

method_inst_t *
bot_first_method(const bot_inst_t *inst)
{
  if(inst == NULL || inst->methods == NULL)
    return(NULL);

  return(inst->methods->inst);
}

method_inst_t *
bot_resolve_method(const bot_inst_t *inst, const char *key)
{
  if(inst == NULL || key == NULL || key[0] == '\0')
    return(NULL);

  // Instance-name match wins: users writing a specific connection name
  // (e.g. "drow") should resolve unambiguously even when the kind share
  // would also match.
  for(bot_method_t *m = inst->methods; m != NULL; m = m->next)
    if(strncasecmp(m->method_name, key, METHOD_NAME_SZ) == 0)
      return(m->inst);

  // Kind match: first binding of the requested plugin kind.
  for(bot_method_t *m = inst->methods; m != NULL; m = m->next)
    if(strncasecmp(m->method_kind, key, PLUGIN_NAME_SZ) == 0)
      return(m->inst);

  return(NULL);
}

void
bot_inc_cmd_count(bot_inst_t *inst)
{
  if(inst != NULL)
    __atomic_add_fetch(&inst->cmd_count, 1, __ATOMIC_RELAXED);
}

uint64_t
bot_cmd_count(const bot_inst_t *inst)
{
  return(inst != NULL ? inst->cmd_count : 0);
}

time_t
bot_last_activity(const bot_inst_t *inst)
{
  return(inst != NULL ? inst->last_activity : 0);
}

// Subsystem lifecycle

// Initialize the bot subsystem. Sets up the mutex and marks the
// subsystem as ready. Must be called before any other bot_* functions.
void
bot_init(void)
{
  pthread_mutex_init(&bot_mutex, NULL);
  bot_ready = true;

  clam(CLAM_INFO, "bot_init", "bot subsystem initialized");
}

// KV configuration

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

static void
bot_kv_changed(const char *key, void *data)
{
  (void)key;
  (void)data;
  bot_load_config();
}

// Session idle expiry

// Periodic task callback: scan all running bot instances and expire
// sessions where (now - last_seen) exceeds the bot's maxidleauth.
static void
bot_session_reaper(task_t *t)
{
  time_t now = time(NULL);

  pthread_mutex_lock(&bot_mutex);

  for(bot_inst_t *inst = bot_list; inst != NULL; inst = inst->next)
  {
    char           key[KV_KEY_SZ];
    uint32_t       maxidle;
    bot_session_t *s;
    bot_session_t *prev = NULL;

    if(inst->state != BOT_RUNNING || inst->sessions == NULL)
      continue;

    // Read per-bot maxidleauth (0 = never expire).
    snprintf(key, sizeof(key), "bot.%s.maxidleauth", inst->name);
    maxidle = (uint32_t)kv_get_uint(key);

    s = inst->sessions;

    while(s != NULL)
    {
      bot_session_t *next = s->next;
      uint32_t       timeout = 0;
      const char    *kind = method_inst_kind(s->method);

      // Determine effective timeout: per-method identtimeout takes
      // precedence, falling back to per-bot maxidleauth.

      if(kind != NULL)
      {
        char tkey[KV_KEY_SZ];
        snprintf(tkey, sizeof(tkey), "bot.%s.%s.identtimeout",
            inst->name, kind);
        timeout = (uint32_t)kv_get_uint(tkey);
      }

      if(timeout == 0)
        timeout = maxidle;

      if(timeout == 0)
      {
        prev = s;
        s = next;
        continue;
      }

      if((now - s->last_seen) > (time_t)timeout)
      {
        clam(CLAM_INFO, "bot_session_reaper",
            "'%s': expired session for '%s' on %s (%s) "
            "(idle %ld sec, limit %u sec)",
            inst->name, s->username,
            method_inst_name(s->method), s->sender,
            (long)(now - s->last_seen), timeout);

        // Notify the user before removing the session.
        method_send(s->method, s->sender,
            "Your identity has expired. "
            "Use identify to re-authenticate.");

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
        prev = s;

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
  kv_register("core.bot.max_methods",  KV_UINT32, "16",  bot_kv_changed, NULL,
      "Maximum number of method bindings per bot");
  kv_register("core.bot.max_sessions", KV_UINT32, "256", bot_kv_changed, NULL,
      "Maximum concurrent authenticated sessions across all bots");
  bot_load_config();

  // Start periodic session reaper (every 60 seconds).
  task_add_periodic("bot_session_reaper", TASK_ANY, 200,
      60000, bot_session_reaper, NULL);
}

// Per-bot method KV registration

uint32_t
bot_register_method_kv(const char *botname, const char *method_kind)
{
  const plugin_desc_t *pd;
  char                 bot_prefix[KV_KEY_SZ];
  uint32_t             registered = 0;

  if(botname == NULL || botname[0] == '\0' ||
     method_kind == NULL || method_kind[0] == '\0')
    return(0);

  // Find the protocol plugin by kind.
  pd = plugin_find_type(PLUGIN_PROTOCOL, method_kind);

  if(pd == NULL || pd->kv_inst_schema == NULL ||
     pd->kv_inst_schema_count == 0)
    return(0);

  // Build the bot prefix: "bot.<botname>.<kind>."
  snprintf(bot_prefix, sizeof(bot_prefix),
      "bot.%s.%s.", botname, method_kind);

  for(uint32_t i = 0; i < pd->kv_inst_schema_count; i++)
  {
    const plugin_kv_entry_t *e = &pd->kv_inst_schema[i];
    char                     new_key[KV_KEY_SZ];

    if(e->key == NULL)
      continue;

    // Instance schema keys are bare suffixes (e.g., "nick", "network").
    // Build the per-bot key: "bot.<botname>.<kind>.<suffix>"
    snprintf(new_key, sizeof(new_key), "%s%s", bot_prefix, e->key);

    if(kv_register(new_key, e->type, e->default_val, e->cb, NULL,
        e->help) == SUCCESS)
    {
      registered++;

      if(e->nl != NULL)
        kv_register_nl(new_key, e->nl);
    }
  }

  if(registered > 0)
    clam(CLAM_DEBUG, "bot_register_method_kv",
        "'%s': registered %u KV key(s) for method '%s'",
        botname, registered, method_kind);

  return(registered);
}

uint32_t
bot_register_driver_kv(const char *botname, const char *bot_kind)
{
  const plugin_desc_t *pd;
  char                 bot_prefix[KV_KEY_SZ];
  uint32_t             registered = 0;

  if(botname == NULL || botname[0] == '\0' ||
     bot_kind == NULL || bot_kind[0] == '\0')
    return(0);

  pd = plugin_find_type(PLUGIN_METHOD, bot_kind);

  if(pd == NULL)
    pd = plugin_find_type(PLUGIN_FEATURE, bot_kind);

  if(pd == NULL || pd->kv_inst_schema == NULL ||
     pd->kv_inst_schema_count == 0)
    return(0);

  snprintf(bot_prefix, sizeof(bot_prefix), "bot.%s.", botname);

  for(uint32_t i = 0; i < pd->kv_inst_schema_count; i++)
  {
    const plugin_kv_entry_t *e = &pd->kv_inst_schema[i];
    char                     new_key[KV_KEY_SZ];

    if(e->key == NULL)
      continue;

    snprintf(new_key, sizeof(new_key), "%s%s", bot_prefix, e->key);

    if(kv_register(new_key, e->type, e->default_val, e->cb, NULL,
        e->help) == SUCCESS)
    {
      registered++;

      if(e->nl != NULL)
        kv_register_nl(new_key, e->nl);
    }
  }

  if(registered > 0)
    clam(CLAM_DEBUG, "bot_register_driver_kv",
        "'%s': registered %u KV key(s) for driver '%s'",
        botname, registered, bot_kind);

  return(registered);
}

// Database persistence

// Create bot persistence tables (bot_instances, bot_methods) in the
// database if they do not already exist. Must be called after db_init().
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

  if(db_query("SELECT name, kind "
               "FROM bot_instances ORDER BY created", r) != SUCCESS)
  {
    clam(CLAM_WARN, "bot_restore",
        "failed to query bot_instances: %s", r->error);
    db_result_free(r);
    return(FAIL);
  }

  for(uint32_t i = 0; i < r->rows; i++)
  {
    const char          *name = db_result_get(r, i, 0);
    const char          *kind = db_result_get(r, i, 1);
    const plugin_desc_t *pd;
    const bot_driver_t  *drv;
    bot_inst_t          *inst;

    if(name == NULL || kind == NULL)
      continue;

    // Find the method or feature plugin by kind.
    pd = plugin_find_type(PLUGIN_METHOD, kind);

    if(pd == NULL)
      pd = plugin_find_type(PLUGIN_FEATURE, kind);

    if(pd == NULL || pd->ext == NULL)
    {
      clam(CLAM_WARN, "bot_restore",
          "'%s': no bot plugin with kind '%s'", name, kind);
      continue;
    }

    // Register the driver's per-instance KV keys (e.g., llm.*)
    // BEFORE creating the instance, so the driver's create() can read
    // persisted values (e.g., active personality) during initialization.
    bot_register_driver_kv(name, kind);

    drv = (const bot_driver_t *)pd->ext;
    inst = bot_create(drv, name);

    if(inst == NULL)
    {
      clam(CLAM_WARN, "bot_restore",
          "failed to create '%s'", name);
      continue;
    }

    // Set user namespace from KV if configured.
    {
      char        ns_key[KV_KEY_SZ];
      const char *ns_val;

      snprintf(ns_key, sizeof(ns_key), "bot.%s.userns", name);
      ns_val = kv_get_str(ns_key);
      if(ns_val != NULL && ns_val[0] != '\0')
        bot_set_userns(inst, ns_val);
    }

    // Track auto-start candidates via KV.
    {
      char as_key[KV_KEY_SZ];
      snprintf(as_key, sizeof(as_key), "bot.%s.autostart", name);

      if(kv_get_uint(as_key) != 0 && *auto_count < 64)
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
    bot_inst_t *inst;
    char        inst_name[BOT_NAME_SZ + METHOD_NAME_SZ + 2];

    if(bname == NULL || mkind == NULL)
      continue;

    inst = bot_find(bname);

    if(inst == NULL)
      continue;

    // Reconstruct method instance name: "{botname}_{method_kind}"
    snprintf(inst_name, sizeof(inst_name), "%s_%s", bname, mkind);

    bot_bind_method(inst, inst_name, mkind);

    // Register per-bot method KV keys (bot.<bname>.<mkind>.*).
    bot_register_method_kv(bname, mkind);
  }

  db_result_free(r);
}

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
bool
bot_restore(void)
{
  uint32_t restored   = 0;
  uint32_t auto_count = 0;
  char     auto_names[64][BOT_NAME_SZ];
  uint32_t started;

  if(bot_restore_instances(auto_names, &auto_count, &restored) != SUCCESS)
    return(FAIL);

  bot_restore_methods();

  started = bot_restore_autostart(auto_names, auto_count);

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
  {
    bot_method_t *m = bot_method_freelist;

    while(m != NULL)
    {
      bot_method_t *next = m->next;
      mem_free(m);
      m = next;
    }
  }

  bot_method_freelist = NULL;
  bot_method_free_count = 0;

  // Free the session freelist.
  {
    bot_session_t *s = bot_session_freelist;

    while(s != NULL)
    {
      bot_session_t *next = s->next;
      mem_free(s);
      s = next;
    }
  }

  bot_session_freelist = NULL;
  bot_session_free_count = 0;

  pthread_mutex_destroy(&bot_mutex);
}

void *
bot_get_handle(const bot_inst_t *inst)
{
  return(inst != NULL ? inst->handle : NULL);
}

// The per-kind verb registries (bot_show_verb_* and bot_verb_*) have
// been removed; verbs are now children of "show/bot" and "bot" in the
// unified command tree, filtered by a per-command kind_filter. See
// cmd_register(, NULL)'s kind_filter parameter and core/bot_cmd.c's
// help_ext_* + cmd_show_bot / admin_cmd_bot dispatchers.
