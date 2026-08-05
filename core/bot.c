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

// Record a public (channel) line as the bot's latest witnessed line for
// its (method, channel). DMs, empty lines, and command invocations are
// skipped — the last of these so a `!quote add` never quotes itself.
static void
bot_witness_record(bot_inst_t *bot, const method_msg_t *msg)
{
  const char    *method;
  const char    *prefix;
  bot_witness_t *w = NULL;

  if(msg->channel[0] == '\0' || msg->text[0] == '\0' || msg->inst == NULL)
    return;

  prefix = cmd_get_prefix(bot);

  if(prefix != NULL && prefix[0] != '\0' &&
     strncmp(msg->text, prefix, strlen(prefix)) == 0)
    return;

  method = method_inst_name(msg->inst);

  if(method == NULL)
    return;

  pthread_mutex_lock(&bot_witness_lock);

  // Reuse the slot for this (method, channel) if present.
  for(uint32_t i = 0; i < BOT_WITNESS_MAX; i++)
  {
    bot_witness_t *e = &bot->witness[i];

    if(e->valid && strcmp(e->method, method) == 0 &&
       strcmp(e->channel, msg->channel) == 0)
    {
      w = e;
      break;
    }
  }

  if(w == NULL)
  {
    w = &bot->witness[bot->witness_next];
    bot->witness_next = (bot->witness_next + 1) % BOT_WITNESS_MAX;
    memset(w, 0, sizeof(*w));
    snprintf(w->method,  sizeof(w->method),  "%s", method);
    snprintf(w->channel, sizeof(w->channel), "%s", msg->channel);
  }

  snprintf(w->nickname, sizeof(w->nickname), "%.*s",
      (int)(sizeof(w->nickname) - 1),
      (msg->nickname[0] != '\0') ? msg->nickname : msg->sender);
  snprintf(w->sender, sizeof(w->sender), "%s", msg->sender);
  snprintf(w->text,   sizeof(w->text),   "%s", msg->text);
  w->is_action = msg->is_action;
  w->valid     = true;

  pthread_mutex_unlock(&bot_witness_lock);
}

bool
bot_last_public_line(const bot_inst_t *inst, const method_inst_t *method,
    const char *channel, bot_public_line_t *out)
{
  const char *mname;
  bool        hit = false;

  if(inst == NULL || method == NULL || channel == NULL || out == NULL)
    return(false);

  mname = method_inst_name(method);

  if(mname == NULL)
    return(false);

  pthread_mutex_lock(&bot_witness_lock);

  for(uint32_t i = 0; i < BOT_WITNESS_MAX; i++)
  {
    const bot_witness_t *e = &inst->witness[i];

    if(e->valid && strcmp(e->method, mname) == 0 &&
       strcmp(e->channel, channel) == 0)
    {
      snprintf(out->nickname, sizeof(out->nickname), "%s", e->nickname);
      snprintf(out->sender,   sizeof(out->sender),   "%s", e->sender);
      snprintf(out->text,     sizeof(out->text),     "%s", e->text);
      out->is_action = e->is_action;
      hit = true;
      break;
    }
  }

  pthread_mutex_unlock(&bot_witness_lock);
  return(hit);
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
  bot_witness_record(bot, msg);
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

// ---------------------------------------------------------------------------
// Per-bot / per-(bot,method) KV contributors
// ---------------------------------------------------------------------------
//
// A tiny fixed registry of plugins that want to decorate every bot with
// their own KV keys (see bot.h). The array is guarded by its own mutex,
// never nested under bot_mutex during fan-out: we snapshot under the
// contributor lock, release it, then invoke the callbacks. Back-fill of a
// newly registered contributor (below) walks the bot list under bot_mutex
// and calls only that one contributor, so no re-registration storms.

#define BOT_KV_CONTRIB_MAX  8

typedef struct
{
  bot_kv_bot_cb_t    bot_cb;
  bot_kv_method_cb_t method_cb;
  void              *user;
  const void        *owner_pc;  // register call site; identifies the owning object
} bot_kv_contrib_t;

static bot_kv_contrib_t bot_kv_contribs[BOT_KV_CONTRIB_MAX];
static uint32_t         bot_kv_contrib_count = 0;
static pthread_mutex_t  bot_kv_contrib_mutex = PTHREAD_MUTEX_INITIALIZER;

// Snapshot the contributor table under its lock, so callbacks run unlocked.
static uint32_t
bot_kv_snapshot(bot_kv_contrib_t out[BOT_KV_CONTRIB_MAX])
{
  uint32_t n;

  pthread_mutex_lock(&bot_kv_contrib_mutex);
  n = bot_kv_contrib_count;
  memcpy(out, bot_kv_contribs, n * sizeof(out[0]));
  pthread_mutex_unlock(&bot_kv_contrib_mutex);

  return(n);
}

// Fan a fresh bot out to every contributor's bot_cb.
static void
bot_kv_fanout_bot(const char *name)
{
  bot_kv_contrib_t snap[BOT_KV_CONTRIB_MAX];
  uint32_t         n = bot_kv_snapshot(snap);

  for(uint32_t i = 0; i < n; i++)
    if(snap[i].bot_cb != NULL)
      snap[i].bot_cb(name, snap[i].user);
}

// Fan a freshly-bound (bot, method) pair out to every method_cb.
static void
bot_kv_fanout_method(const char *name, const char *method_kind)
{
  bot_kv_contrib_t snap[BOT_KV_CONTRIB_MAX];
  uint32_t         n = bot_kv_snapshot(snap);

  for(uint32_t i = 0; i < n; i++)
    if(snap[i].method_cb != NULL)
      snap[i].method_cb(name, method_kind, snap[i].user);
}

void
bot_kv_contributor_register(bot_kv_bot_cb_t bot_cb,
    bot_kv_method_cb_t method_cb, void *user)
{
  const void *owner_pc = __builtin_return_address(0);

  pthread_mutex_lock(&bot_kv_contrib_mutex);

  if(bot_kv_contrib_count >= BOT_KV_CONTRIB_MAX)
  {
    pthread_mutex_unlock(&bot_kv_contrib_mutex);
    clam(CLAM_WARN, "bot",
        "KV contributor table full (%d); dropping registration",
        BOT_KV_CONTRIB_MAX);
    return;
  }

  bot_kv_contribs[bot_kv_contrib_count].bot_cb    = bot_cb;
  bot_kv_contribs[bot_kv_contrib_count].method_cb = method_cb;
  bot_kv_contribs[bot_kv_contrib_count].user      = user;
  bot_kv_contribs[bot_kv_contrib_count].owner_pc  = owner_pc;
  bot_kv_contrib_count++;

  pthread_mutex_unlock(&bot_kv_contrib_mutex);

  // Back-fill: apply only this contributor to every existing bot and its
  // already-bound methods, so a late/hot-reloaded plugin catches up.
  // kv_register is idempotent-quiet only for new keys, so we invoke the
  // single new contributor rather than the full fan-out.
  pthread_mutex_lock(&bot_mutex);

  for(bot_inst_t *b = bot_list; b != NULL; b = b->next)
  {
    if(bot_cb != NULL)
      bot_cb(b->name, user);

    if(method_cb != NULL)
      for(bot_method_t *m = b->methods; m != NULL; m = m->next)
        method_cb(b->name, m->method_kind, user);
  }

  pthread_mutex_unlock(&bot_mutex);
}

void
bot_kv_contributor_unregister(void *user)
{
  pthread_mutex_lock(&bot_kv_contrib_mutex);

  for(uint32_t i = 0; i < bot_kv_contrib_count; i++)
    if(bot_kv_contribs[i].user == user)
    {
      // Compact the tail down over the removed slot.
      for(uint32_t j = i + 1; j < bot_kv_contrib_count; j++)
        bot_kv_contribs[j - 1] = bot_kv_contribs[j];

      bot_kv_contrib_count--;
      break;
    }

  pthread_mutex_unlock(&bot_kv_contrib_mutex);
}

uint32_t
bot_reclaim_contributors_owned(uintptr_t lo, uintptr_t hi)
{
  uint32_t removed = 0;
  uint32_t i       = 0;

  if(lo >= hi)
    return(0);

  pthread_mutex_lock(&bot_kv_contrib_mutex);

  while(i < bot_kv_contrib_count)
  {
    uintptr_t pc = (uintptr_t)bot_kv_contribs[i].owner_pc;

    if(pc >= lo && pc < hi)
    {
      // Compact the tail down over the removed slot; `i` stays put so
      // the entry shifted into it is tested on the next iteration.
      for(uint32_t j = i + 1; j < bot_kv_contrib_count; j++)
        bot_kv_contribs[j - 1] = bot_kv_contribs[j];

      bot_kv_contrib_count--;
      removed++;
    }

    else
      i++;
  }

  pthread_mutex_unlock(&bot_kv_contrib_mutex);

  if(removed > 0)
    clam(CLAM_DEBUG, "bot_reclaim",
        "reclaimed %u KV contributor(s)", removed);

  return(removed);
}

void
bot_audit_iterate_contributors(bot_audit_cb_t cb, void *data)
{
  char subject[32];

  if(cb == NULL)
    return;

  pthread_mutex_lock(&bot_kv_contrib_mutex);

  for(uint32_t i = 0; i < bot_kv_contrib_count; i++)
  {
    const bot_kv_contrib_t *c = &bot_kv_contribs[i];

    snprintf(subject, sizeof(subject), "contributor[%u]", i);
    cb(subject, "bot_cb",    fn_addr(&c->bot_cb),    data);
    cb(subject, "method_cb", fn_addr(&c->method_cb), data);
    cb(subject, "user",      c->user,                data);
  }

  pthread_mutex_unlock(&bot_kv_contrib_mutex);
}

void
bot_audit_iterate_bindings(bot_audit_cb_t cb, void *data)
{
  if(cb == NULL)
    return;

  pthread_mutex_lock(&bot_mutex);

  for(bot_inst_t *b = bot_list; b != NULL; b = b->next)
    cb(b->name, "driver", b->driver, data);

  pthread_mutex_unlock(&bot_mutex);
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
        "Seconds a temporary identity survives idle before it "
        "lazily expires (0=never)");

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

  // Invite registered plugins (e.g. `ask`) to layer their own
  // bot.<name>.* keys onto the fresh instance. Runs unlocked — bot_mutex
  // was released above and contributor callbacks only touch KV.
  bot_kv_fanout_bot(name);

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


    if(inst->driver != NULL && inst->driver->stop != NULL)
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
  if(inst->driver != NULL && inst->driver->destroy != NULL
      && inst->handle != NULL)
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
  #define MAX_CLEARED_BOTS 128

  char     cleared[MAX_CLEARED_BOTS][BOT_NAME_SZ];
  uint32_t n = 0;
  uint32_t i;

  if(ns_name == NULL || ns_name[0] == '\0')
    return;

  // Snapshot matched names + null the binding under the lock; emit
  // clam() + kv_set_str() (both can log -> re-enter bot_mutex via
  // clam_cmd_shared_cb -> bot_find) only AFTER releasing bot_mutex.
  pthread_mutex_lock(&bot_mutex);

  for(bot_inst_t *b = bot_list; b != NULL && n < MAX_CLEARED_BOTS;
      b = b->next)
  {
    if(b->userns != NULL
        && strncasecmp(b->userns->name, ns_name, USERNS_NAME_SZ) == 0)
    {
      snprintf(cleared[n], sizeof(cleared[n]), "%s", b->name);
      n++;
      b->userns = NULL;          // pointer null only — safe under lock
    }
  }

  pthread_mutex_unlock(&bot_mutex);

  for(i = 0; i < n; i++)
  {
    char key[KV_KEY_SZ];

    clam(CLAM_INFO, "bot_clear_userns",
        "'%s': namespace '%s' removed, clearing binding", cleared[i], ns_name);

    // Clear the KV key so the binding doesn't persist. Precision bound
    // keeps -Wformat-truncation quiet across the 2-D snapshot array.
    snprintf(key, sizeof(key), "bot.%.*s.userns",
        (int)(sizeof(cleared[i]) - 1), cleared[i]);
    kv_set_str(key, "");
  }

  #undef MAX_CLEARED_BOTS
}

// Identity

// Stateless per-message identity resolution. Two checks against the
// message's own metadata, in order: an exact temporary-MFA match
// (minted by !identify; lazy expiry — the first resolve past the idle
// window deletes the entry, and the one-time "identity expired" notice
// rides this call), then a permanent MFA pattern match gated on the
// user's autoidentify flag. No cache of the result exists anywhere:
// every message answers for itself.
//
// metadata may be NULL/empty; the method's context map is consulted
// for the sender in that case (third-party lookups by nick).
bool
bot_identity_resolve(bot_inst_t *inst, method_inst_t *method,
    const char *sender, const char *metadata,
    char *user_out, size_t user_sz)
{
  userns_t   *ns;
  char        meta[METHOD_META_SZ];
  char        user[USERNS_USER_SZ];
  const char *matched;
  uint32_t    timeout = 0;

  if(user_out != NULL && user_sz > 0)
    user_out[0] = '\0';

  if(inst == NULL || user_out == NULL || user_sz == 0)
    return(false);

  ns = bot_get_userns(inst);

  if(ns == NULL)
    return(false);

  meta[0] = '\0';

  if(metadata != NULL && metadata[0] != '\0')
    snprintf(meta, sizeof(meta), "%s", metadata);

  else if(method != NULL && sender != NULL && sender[0] != '\0')
  {
    char host[METHOD_META_SZ] = {0};

    if(method_get_context(method, sender, host, sizeof(host)) == SUCCESS
        && host[0] != '\0')
      snprintf(meta, sizeof(meta), "%s!%s", sender, host);
  }

  if(meta[0] == '\0')
    return(false);

  // Effective idle window: per-method identtimeout wins, then the
  // bot-level maxidleauth; 0 = temporary identities never expire.
  {
    const char *kind = (method != NULL) ? method_inst_kind(method) : NULL;
    char        key[KV_KEY_SZ];

    if(kind != NULL)
    {
      snprintf(key, sizeof(key), "bot.%s.%s.identtimeout",
          inst->name, kind);
      timeout = (uint32_t)kv_get_uint(key);
    }

    if(timeout == 0)
    {
      snprintf(key, sizeof(key), "bot.%s.maxidleauth", inst->name);
      timeout = (uint32_t)kv_get_uint(key);
    }
  }

  switch(userns_tmfa_resolve(ns, meta, timeout, user, sizeof(user)))
  {
    case TMFA_OK:
      snprintf(user_out, user_sz, "%s", user);
      return(true);

    case TMFA_EXPIRED:
      if(method != NULL && sender != NULL && sender[0] != '\0')
        method_send(method, sender,
            "Your identity has expired. "
            "Use identify to re-authenticate.");
      break;

    case TMFA_NONE:
      break;
  }

  matched = userns_mfa_match(ns, meta);

  if(matched != NULL && userns_user_get_autoidentify(ns, matched))
  {
    snprintf(user_out, user_sz, "%s", matched);
    return(true);
  }

  return(false);
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

  // A reload holding the driver is transient, and starting into a NULL
  // vtable is not. Refuse for the few seconds it takes to come back.
  if(inst->driver == NULL)
  {
    pthread_mutex_unlock(&bot_mutex);
    clam(CLAM_WARN, "bot_start",
        "'%s': its driver is detached by a plugin reload in progress",
        inst->name);
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

  // Call driver stop(). NULL while a reload holds the driver — the
  // suspend already stopped it.
  if(inst->driver != NULL && inst->driver->stop != NULL)
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
  out->methods          = 0;
  out->discovered_users = bot_stat_discoveries;

  for(bot_inst_t *b = bot_list; b != NULL; b = b->next)
  {
    if(b->state == BOT_RUNNING)
      out->running++;
    out->methods  += b->method_count;
  }

  pthread_mutex_unlock(&bot_mutex);

  cmd_get_dispatch_stats(&out->cmd_dispatches, &out->cmd_denials);
}

// Iteration

// Per-bot snapshot row for bot_iterate. Lets the iteration invoke
// callbacks with bot_mutex released — a callback may emit (cmd_reply ->
// method_send -> clam() -> clam_cmd_shared_cb -> bot_find), which
// re-locks bot_mutex on the same thread; calling the callback under the
// lock self-deadlocks. Mirrors method_iterate_instances.
typedef struct
{
  char        name[BOT_NAME_SZ];
  char        method_kinds[BOT_METHOD_KINDS_SZ];
  bot_state_t state;
  uint32_t    method_count;
  char        userns_name[USERNS_NAME_SZ];
  bool        has_userns;
  uint64_t    cmd_count;
  time_t      last_activity;
} bot_snap_t;

void
bot_iterate(bot_iter_cb_t cb, void *data)
{
  // Cap is generous for a dev instance; excess dropped silently (matches
  // method_iterate_instances / method_iterate_drivers).
  #define MAX_ITER_BOTS 128

  bot_snap_t snap[MAX_ITER_BOTS];
  uint32_t   count = 0;
  uint32_t   i;

  if(cb == NULL)
    return;

  pthread_mutex_lock(&bot_mutex);

  for(bot_inst_t *b = bot_list; b != NULL && count < MAX_ITER_BOTS;
      b = b->next)
  {
    bot_snap_t *s = &snap[count];

    snprintf(s->name, sizeof(s->name), "%s", b->name);
    bot_method_kinds(b, s->method_kinds, sizeof(s->method_kinds));
    s->state         = b->state;
    s->method_count  = b->method_count;
    s->has_userns    = (b->userns != NULL);

    if(s->has_userns)
      snprintf(s->userns_name, sizeof(s->userns_name), "%s",
          b->userns->name);
    else
      s->userns_name[0] = '\0';

    s->cmd_count     = b->cmd_count;
    s->last_activity = b->last_activity;
    count++;
  }

  pthread_mutex_unlock(&bot_mutex);

  for(i = 0; i < count; i++)
    cb(snap[i].name, snap[i].method_kinds, snap[i].state,
        snap[i].method_count,
        snap[i].has_userns ? snap[i].userns_name : NULL,
        snap[i].cmd_count, snap[i].last_activity, data);

  #undef MAX_ITER_BOTS
}

bool
bot_find_bound_to_driver(const char *driver_name, char *out_name,
    size_t out_cap, bot_state_t *out_state)
{
  bool found = false;

  if(driver_name == NULL)
    return(false);

  pthread_mutex_lock(&bot_mutex);

  for(bot_inst_t *b = bot_list; b != NULL; b = b->next)
  {
    if(b->driver == NULL || b->driver->name == NULL)
      continue;

    if(strcmp(b->driver->name, driver_name) != 0)
      continue;

    if(out_name != NULL && out_cap > 0)
      snprintf(out_name, out_cap, "%s", b->name);

    if(out_state != NULL)
      *out_state = b->state;

    found = true;
    break;
  }

  pthread_mutex_unlock(&bot_mutex);
  return(found);
}

// Suspend / resume across a plugin reload

// One bot's driver, lifted out of the instance and not yet torn down.
// The lift happens under bot_mutex and the teardown after it: once the
// vtable is out of the instance no delivery thread can reach the driver,
// so its stop()/destroy() are free to call back into the bot API.
typedef struct
{
  const bot_driver_t *drv;
  void               *handle;
  bool                running;
} bot_detach_t;

uint32_t
bot_suspend_driver(const char *driver_name)
{
  bot_detach_t det[BOT_SUSPEND_MAX];
  uint32_t     n = 0;

  if(driver_name == NULL || driver_name[0] == '\0')
    return(0);

  pthread_mutex_lock(&bot_mutex);

  for(bot_inst_t *b = bot_list; b != NULL && n < BOT_SUSPEND_MAX; b = b->next)
  {
    if(b->driver == NULL || b->driver->name == NULL)
      continue;

    if(strcmp(b->driver->name, driver_name) != 0)
      continue;

    det[n].drv     = b->driver;
    det[n].handle  = b->handle;
    det[n].running = (b->state == BOT_RUNNING);
    n++;

    memset(&b->susp, 0, sizeof(b->susp));
    b->susp.driver      = true;
    b->susp.was_running = (b->state == BOT_RUNNING);
    snprintf(b->susp.driver_name, sizeof(b->susp.driver_name), "%s",
        driver_name);

    // The state is left alone deliberately. The bot is still subscribed
    // to its methods and still the owner of its sessions; it has only
    // lost the ability to answer, which bot_msg_handler already treats
    // as "drop the message".
    b->driver = NULL;
    b->handle = NULL;
  }

  pthread_mutex_unlock(&bot_mutex);

  for(uint32_t i = 0; i < n; i++)
  {
    if(det[i].running && det[i].drv->stop != NULL)
      det[i].drv->stop(det[i].handle);

    if(det[i].drv->destroy != NULL && det[i].handle != NULL)
      det[i].drv->destroy(det[i].handle);
  }

  if(n > 0)
    clam(CLAM_INFO, "bot_suspend",
        "detached %u bot(s) from driver '%s'", n, driver_name);

  return(n);
}

uint32_t
bot_resume_driver(const bot_driver_t *drv, const char *kind)
{
  char     names[BOT_SUSPEND_MAX][BOT_NAME_SZ];
  uint32_t n       = 0;
  uint32_t resumed = 0;

  if(drv == NULL || drv->name == NULL)
    return(0);

  pthread_mutex_lock(&bot_mutex);

  for(bot_inst_t *b = bot_list; b != NULL && n < BOT_SUSPEND_MAX; b = b->next)
  {
    if(!b->susp.driver || strcmp(b->susp.driver_name, drv->name) != 0)
      continue;

    snprintf(names[n], BOT_NAME_SZ, "%s", b->name);
    n++;
  }

  pthread_mutex_unlock(&bot_mutex);

  for(uint32_t i = 0; i < n; i++)
  {
    bot_inst_t *b      = bot_find(names[i]);
    void       *handle = NULL;
    bool        start;

    if(b == NULL)
      continue;

    // The rows outlived the reload; the bindings did not. Per-bot
    // instance KV is registered by core on the plugin's behalf and
    // attributed to it, so the Class-A sweep reclaimed it at unload and
    // nothing re-registers it until a bot binds again — which is here.
    if(kind != NULL && kind[0] != '\0')
      bot_register_driver_kv(names[i], kind);

    if(drv->create != NULL)
    {
      handle = drv->create(b);

      if(handle == NULL)
      {
        clam(CLAM_WARN, "bot_resume",
            "'%s': driver '%s' create() failed; the bot stays detached",
            names[i], drv->name);
        continue;
      }
    }

    pthread_mutex_lock(&bot_mutex);
    b->driver = drv;
    b->handle = handle;
    start     = b->susp.was_running;
    memset(&b->susp, 0, sizeof(b->susp));
    pthread_mutex_unlock(&bot_mutex);

    if(start && drv->start != NULL && drv->start(handle) != SUCCESS)
      clam(CLAM_WARN, "bot_resume",
          "'%s': driver '%s' start() failed after the reload",
          names[i], drv->name);

    resumed++;
  }

  if(resumed > 0)
    clam(CLAM_INFO, "bot_resume",
        "re-attached %u bot(s) to driver '%s'", resumed, drv->name);

  return(resumed);
}

uint32_t
bot_suspend_method(const char *method_kind)
{
  char     names[BOT_SUSPEND_MAX][BOT_NAME_SZ];
  uint32_t n       = 0;
  uint32_t stopped = 0;

  if(method_kind == NULL || method_kind[0] == '\0')
    return(0);

  pthread_mutex_lock(&bot_mutex);

  for(bot_inst_t *b = bot_list; b != NULL && n < BOT_SUSPEND_MAX; b = b->next)
  {
    // Only a running bot holds a method instance — the binding is a name
    // until bot_start() resolves it.
    if(b->state != BOT_RUNNING)
      continue;

    for(bot_method_t *m = b->methods; m != NULL; m = m->next)
    {
      if(strncasecmp(m->method_kind, method_kind, PLUGIN_NAME_SZ) != 0)
        continue;

      memset(&b->susp, 0, sizeof(b->susp));
      b->susp.method      = true;
      b->susp.was_running = true;
      snprintf(b->susp.method_kind, sizeof(b->susp.method_kind), "%s",
          method_kind);

      snprintf(names[n], BOT_NAME_SZ, "%s", b->name);
      n++;
      break;
    }
  }

  pthread_mutex_unlock(&bot_mutex);

  // A method's connection lives in the mapping about to go away, so
  // there is nothing to preserve and no half-measure: stop the bot,
  // which unsubscribes it and unregisters the method instance it owns.
  // Sessions go with it — the users will have to identify again.
  for(uint32_t i = 0; i < n; i++)
  {
    bot_inst_t *b = bot_find(names[i]);

    if(b == NULL)
      continue;

    if(bot_stop(b) == SUCCESS)
    {
      stopped++;
      continue;
    }

    clam(CLAM_WARN, "bot_suspend",
        "'%s': would not stop for the '%s' reload", names[i], method_kind);

    pthread_mutex_lock(&bot_mutex);
    memset(&b->susp, 0, sizeof(b->susp));
    pthread_mutex_unlock(&bot_mutex);
  }

  if(stopped > 0)
    clam(CLAM_INFO, "bot_suspend",
        "stopped %u bot(s) bound to method '%s'", stopped, method_kind);

  return(stopped);
}

uint32_t
bot_resume_method(const char *method_kind)
{
  char     names[BOT_SUSPEND_MAX][BOT_NAME_SZ];
  uint32_t n       = 0;
  uint32_t resumed = 0;

  if(method_kind == NULL || method_kind[0] == '\0')
    return(0);

  pthread_mutex_lock(&bot_mutex);

  for(bot_inst_t *b = bot_list; b != NULL && n < BOT_SUSPEND_MAX; b = b->next)
  {
    if(!b->susp.method
        || strncasecmp(b->susp.method_kind, method_kind, PLUGIN_NAME_SZ) != 0)
      continue;

    snprintf(names[n], BOT_NAME_SZ, "%s", b->name);
    n++;
  }

  pthread_mutex_unlock(&bot_mutex);

  for(uint32_t i = 0; i < n; i++)
  {
    bot_inst_t *b = bot_find(names[i]);
    bool        start;

    if(b == NULL)
      continue;

    // Same rebind as a driver resume, one tier down: these are the
    // bot.<bot>.<method>.* keys the method plugin declared.
    bot_register_method_kv(names[i], method_kind);

    pthread_mutex_lock(&bot_mutex);
    start = b->susp.was_running;
    memset(&b->susp, 0, sizeof(b->susp));
    pthread_mutex_unlock(&bot_mutex);

    // bot_start() re-resolves the method plugin by kind, so the
    // instance it creates comes from the mapping that just arrived.
    if(start && bot_start(b) != SUCCESS)
    {
      clam(CLAM_WARN, "bot_resume",
          "'%s': would not start after the '%s' reload",
          names[i], method_kind);
      continue;
    }

    resumed++;
  }

  if(resumed > 0)
    clam(CLAM_INFO, "bot_resume",
        "restarted %u bot(s) on method '%s'", resumed, method_kind);

  return(resumed);
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

// Both of the following walk inst->methods without taking bot_mutex, as
// bot_resolve_method() above does: the list is built and torn down while
// the bot is CREATED, and bot_iterate() calls bot_method_kinds() with the
// lock already held.
bool
bot_has_method_kind(const bot_inst_t *inst, const char *kind)
{
  if(inst == NULL || kind == NULL || kind[0] == '\0')
    return(false);

  for(bot_method_t *m = inst->methods; m != NULL; m = m->next)
    if(strncasecmp(m->method_kind, kind, PLUGIN_NAME_SZ) == 0)
      return(true);

  return(false);
}

size_t
bot_method_kinds(const bot_inst_t *inst, char *out, size_t out_sz)
{
  size_t n = 0;

  if(out == NULL || out_sz == 0)
    return(0);

  out[0] = '\0';

  if(inst == NULL)
    return(0);

  for(bot_method_t *m = inst->methods; m != NULL; m = m->next)
  {
    bool dup = false;
    int  w;

    // Two IRC networks on one bot are two bindings of one kind; the
    // reader wants "irc", not "irc, irc".
    for(bot_method_t *e = inst->methods; e != m; e = e->next)
      if(strncasecmp(e->method_kind, m->method_kind, PLUGIN_NAME_SZ) == 0)
      {
        dup = true;
        break;
      }

    if(dup)
      continue;

    w = snprintf(out + n, out_sz - n, "%s%s",
        n > 0 ? ", " : "", m->method_kind);

    if(w < 0)
      break;

    // Truncated: keep the prefix that fits and stop.
    if((size_t)w >= out_sz - n)
    {
      n = out_sz - 1;
      break;
    }

    n += (size_t)w;
  }

  return(n);
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
// max_methods to [1, 64].
static void
bot_load_config(void)
{
  bot_cfg.max_methods = (uint32_t)kv_get_uint("core.bot.max_methods");

  if(bot_cfg.max_methods < 1)   bot_cfg.max_methods = 1;
  if(bot_cfg.max_methods > 64)  bot_cfg.max_methods = 64;
}

static void
bot_kv_changed(const char *key, void *data)
{
  (void)key;
  (void)data;
  bot_load_config();
}


// Register bot subsystem KV keys and load initial values. Must be
// called after kv_init() and kv_load().
void
bot_register_config(void)
{
  kv_register("core.bot.max_methods",  KV_UINT32, "16",  bot_kv_changed, NULL,
      "Maximum number of method bindings per bot");
  bot_load_config();

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

  // Invite contributors (e.g. `ask`) to layer their own
  // bot.<botname>.<method>.* keys onto this binding. Done first so it
  // fires even when the method plugin itself exposes no instance schema.
  bot_kv_fanout_method(botname, method_kind);

  // Find the method plugin by kind.
  pd = plugin_find_type(PLUGIN_METHOD, method_kind);

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

    // The instance schema belongs to the plugin that declared it, not to
    // this loop; attribute the key there so its unload reclaims it.
    if(kv_register_owned(new_key, e->type, e->default_val, e->cb, NULL,
        e->help, e) == SUCCESS)
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

  pd = plugin_find_type(PLUGIN_BOT, bot_kind);

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

    // The instance schema belongs to the plugin that declared it, not to
    // this loop; attribute the key there so its unload reclaims it.
    if(kv_register_owned(new_key, e->type, e->default_val, e->cb, NULL,
        e->help, e) == SUCCESS)
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

    // TAXO-2 (2026-08-05), one-time and DELETE ME. The bot plugin was
    // renamed 'text' -> 'chat'; bot_restore_instances() resolves this
    // column with plugin_find_type(), so a stored 'text' matches
    // nothing and every bot is silently left unrestored with only a
    // WARN. Idempotent, and a no-op on any tree that has been
    // fresh-started since -- freshstart.sh drops both tables. Drop this
    // line the first time that happens. See root TODO.md §TAXO-2.
    "UPDATE bot_instances SET kind = 'chat' WHERE kind = 'text'",

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

    // Find the bot plugin by kind.
    pd = plugin_find_type(PLUGIN_BOT, kind);

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
// frees the method binding freelist, and destroys the mutex.
void
bot_exit(void)
{
  if(!bot_ready)
    return;

  clam(CLAM_INFO, "bot_exit",
      "shutting down (%u instances, %u freelisted bindings)",
      bot_count, bot_method_free_count);

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
