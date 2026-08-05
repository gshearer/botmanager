// botmanager — MIT
// urlgrabber feature plugin (PLUGIN_FEATURE, kind: urlgrabber).
//
// No bot driver: urlgrabber attaches an observer to every running bot's
// method stream and re-attaches to any bot that starts later (via the
// `bot_start` event). Each observer reads per-channel config out of the
// bot's own KV namespace and, when a channel is enabled, fetches the title
// of any URL it sees. ug_fetch.c owns the network + parsing mechanism.
// Configuration is entirely `!set kv` / `!show kv`.

#define URLGRABBER_INTERNAL
#include "urlgrabber.h"

#include "alloc.h"
#include "kv.h"
#include "task.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// Method subscriber-name bound (UG_SUB_NAME_SZ is method-internal).
#define UG_SUB_NAME_SZ  64

// Forward decls for helpers defined after their first use.
static void ug_register_bot_channels(const char *botname, const char *kind,
    const char *prefix);

// ------------------------------------------------------------------ //
// KV schema — global knobs, registered once under plugin.urlgrabber.* //
// ------------------------------------------------------------------ //

static const plugin_kv_entry_t ug_kv_schema[] = {
  { UG_KV_TIMEOUT,    KV_UINT32, "15",
    "HTTP timeout in seconds for a title fetch" },
  { UG_KV_MAX_TITLE,  KV_UINT32, "300",
    "Maximum announced title length in characters" },
  { UG_KV_MAX_BYTES,  KV_UINT32, "131072",
    "Bytes fetched per URL (Range-capped — the <title> lives in the head)" },
  { UG_KV_USER_AGENT, KV_STR,
    // A browser-like string on purpose: declaring ourselves a bot draws a
    // 403 challenge from Cloudflare and similar WAFs, which silently starves
    // whole sites of titles. Every mainstream link-preview bot presents as a
    // browser for exactly this reason.
    "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0",
    "User-Agent sent when fetching a page title" },
};

// ------------------------------------------------------------------ //
// Per-channel keys                                                    //
// ------------------------------------------------------------------ //

// Compose bot.<bot>.<kind>.chan.<channel>.urlgrabber.<suffix> into `buf`,
// so the knob sits right beside the channel's other settings. Any leading
// channel sigil ('#', '&', …) is dropped to match the IRC channel keyspace
// (which stores "cabal", not "#cabal"), and every component is
// width-bounded so the composed key can never exceed KV_KEY_SZ
// (4+32+1+16+6+48+12+8 = 127 < 128).
static void
ug_chan_key(char *buf, size_t bufsz, const char *bot, const char *kind,
    const char *channel, const char *suffix)
{
  const char *name = channel;

  if(name[0] == '#' || name[0] == '&' || name[0] == '+' || name[0] == '!')
    name++;

  snprintf(buf, bufsz, "bot.%.32s.%.16s.chan.%.48s.urlgrabber.%s",
      bot, kind, name, suffix);
}

// Register a channel's urlgrabber knobs (both, together) so they are
// visible to `!show kv` and settable via `!set kv`. kv_register rehydrates
// any persisted value from the DB (we are well past kv_load), so an
// operator's earlier `enabled true` survives a reload; a re-register once
// the key exists is refused, so the cheap kv_exists gate keeps it a no-op.
static void
ug_chan_register(const char *bot, const char *kind, const char *channel)
{
  char key[KV_KEY_SZ];

  ug_chan_key(key, sizeof(key), bot, kind, channel, UG_SUFFIX_ENABLED);
  if(kv_exists(key))
    return;

  kv_register(key, KV_BOOL, "false", NULL, NULL,
      "urlgrabber: watch this channel for URLs and announce titles");

  ug_chan_key(key, sizeof(key), bot, kind, channel, UG_SUFFIX_HOLDDOWN);
  kv_register(key, KV_UINT32, "5", NULL, NULL,
      "urlgrabber: minimum seconds between title fetches in this channel");
}

// ------------------------------------------------------------------ //
// Hold-down ring — plugin-global, keyed by "<bot>/<channel>"          //
// ------------------------------------------------------------------ //

#define UG_HOLDDOWN_SLOTS  128
#define UG_HDKEY_SZ        (BOT_NAME_SZ + METHOD_CHANNEL_SZ)

typedef struct
{
  bool   used;
  char   key[UG_HDKEY_SZ];             // "<bot>/<channel>"
  time_t last_fire;                    // CLOCK_MONOTONIC seconds
} ug_hd_slot_t;

static ug_hd_slot_t    ug_hd[UG_HOLDDOWN_SLOTS];
static pthread_mutex_t ug_hd_lock = PTHREAD_MUTEX_INITIALIZER;

static time_t
ug_mono_secs(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return(ts.tv_sec);
}

// Decide whether a fetch may fire for this bot+channel right now, and — if
// so — stamp the moment so the next URL is held down. A holddown of 0
// disables rate-limiting entirely. Returns true when clear.
static bool
ug_holddown_ready(const char *bot, const char *channel, uint32_t holddown)
{
  char          hk[UG_HDKEY_SZ];
  time_t        now = ug_mono_secs();
  ug_hd_slot_t *victim;
  bool          ready = false;

  if(holddown == 0)
    return(true);

  snprintf(hk, sizeof(hk), "%s/%s", bot, channel);

  pthread_mutex_lock(&ug_hd_lock);

  // Reuse this key's slot if present, else claim a free one, else evict
  // whichever slot fired longest ago (coldest, cheapest to lose).
  victim = &ug_hd[0];

  for(uint32_t i = 0; i < UG_HOLDDOWN_SLOTS; i++)
  {
    ug_hd_slot_t *s = &ug_hd[i];

    if(s->used && strcmp(s->key, hk) == 0)
    {
      if((now - s->last_fire) >= (time_t)holddown)
      {
        s->last_fire = now;
        ready = true;
      }
      goto done;
    }

    if(!s->used)
      victim = s;
    else if(victim->used && s->last_fire < victim->last_fire)
      victim = s;
  }

  snprintf(victim->key, sizeof(victim->key), "%s", hk);
  victim->last_fire = now;
  victim->used      = true;
  ready             = true;

done:
  pthread_mutex_unlock(&ug_hd_lock);
  return(ready);
}

// ------------------------------------------------------------------ //
// Observer callback                                                   //
// ------------------------------------------------------------------ //

// Per-attachment context handed to the observer as its subscriber data:
// just the bot name, from which everything else is (re)resolved so nothing
// dangles if the bot is torn down.
typedef struct
{
  char bot[BOT_NAME_SZ];
} ug_actx_t;

static void
ug_observe(const method_msg_t *msg, void *data)
{
  ug_actx_t  *ctx = data;
  bot_inst_t *bot;
  const char *kind;
  const char *prefix;
  const char *p;
  const char *method_name;
  char        key[KV_KEY_SZ];
  char        url[UG_URL_SZ];
  uint32_t    holddown;

  if(ctx == NULL || msg == NULL)
    return;

  // Only conversational lines in a public channel are candidates.
  if(msg->kind != METHOD_MSG_MESSAGE || msg->channel[0] == '\0')
    return;

  bot = bot_find(ctx->bot);
  if(bot == NULL)                       // bot torn down; detached at deinit
    return;

  kind = method_inst_kind(msg->inst);
  if(kind == NULL)
    return;

  // Keep the channel's knobs present (covers channels joined after attach).
  ug_chan_register(ctx->bot, kind, msg->channel);

  // Opted in? Absent/false ⇒ take no action — the safe, silent default.
  ug_chan_key(key, sizeof(key), ctx->bot, kind, msg->channel, UG_SUFFIX_ENABLED);
  if(kv_get_uint(key) == 0)
    return;

  // Never chase a URL sitting in someone's command arguments.
  prefix = cmd_get_prefix(bot);
  p      = msg->text;

  while(*p == ' ' || *p == '\t')
    p++;

  if(prefix != NULL && prefix[0] != '\0' &&
      strncmp(p, prefix, strlen(prefix)) == 0)
    return;

  if(!ug_find_url(msg->text, url, sizeof(url)))
    return;

  // Rate-limit per channel before committing to any outbound request.
  ug_chan_key(key, sizeof(key), ctx->bot, kind, msg->channel, UG_SUFFIX_HOLDDOWN);
  holddown = kv_exists(key) ? (uint32_t)kv_get_uint(key) : UG_DEFAULT_HOLDDOWN;

  if(!ug_holddown_ready(ctx->bot, msg->channel, holddown))
  {
    clam(CLAM_DEBUG, UG_CTX, "%s/%s: held down, skipping %s",
        ctx->bot, msg->channel, url);
    return;
  }

  method_name = method_inst_name(msg->inst);
  if(method_name == NULL)
    return;

  clam(CLAM_DEBUG, UG_CTX, "%s/%s: fetching %s",
      ctx->bot, msg->channel, url);
  ug_fetch(method_name, msg->channel, url);
}

// ------------------------------------------------------------------ //
// Attachment registry                                                 //
// ------------------------------------------------------------------ //

// One live observer subscription. `method_name` (not the instance pointer)
// is stored so teardown can re-resolve safely even if the instance was
// recreated or destroyed underneath us.
typedef struct ug_attach
{
  char              method_name[METHOD_NAME_SZ];
  char              sub_name[UG_SUB_NAME_SZ];
  ug_actx_t        *ctx;
  struct ug_attach *next;
} ug_attach_t;

static ug_attach_t    *ug_attachments = NULL;
static pthread_mutex_t ug_attach_lock = PTHREAD_MUTEX_INITIALIZER;

// Attach an observer to a bot's (first) method stream and register the
// urlgrabber knobs for the channels it already knows about. Idempotent: a
// duplicate subscription is refused by the method layer and simply skipped.
static void
ug_attach_bot(const char *botname)
{
  bot_inst_t    *bot;
  method_inst_t *inst;
  const char    *kind;
  ug_actx_t     *ctx;
  ug_attach_t   *node;
  char           sub_name[UG_SUB_NAME_SZ];

  if(botname == NULL || botname[0] == '\0')
    return;

  bot = bot_find(botname);
  if(bot == NULL)
    return;

  inst = bot_first_method(bot);
  if(inst == NULL)                      // not started / no binding yet
    return;

  kind = method_inst_kind(inst);
  if(kind == NULL)
    return;

  // Register knobs for every channel this bot is configured for, so they
  // are settable the instant urlgrabber is up (no need to wait for chatter).
  {
    char prefix[KV_KEY_SZ];

    snprintf(prefix, sizeof(prefix), "bot.%.32s.%.16s.chan.", botname, kind);
    // Collect distinct channel names under the KV lock, register after.
    ug_register_bot_channels(botname, kind, prefix);
  }

  ctx = mem_alloc(UG_CTX, "actx", sizeof(*ctx));
  if(ctx == NULL)
    return;

  snprintf(ctx->bot, sizeof(ctx->bot), "%s", botname);
  snprintf(sub_name, sizeof(sub_name), "urlgrabber:%.48s", botname);

  if(method_subscribe(inst, sub_name, ug_observe, ctx) != SUCCESS)
  {
    mem_free(ctx);                      // already attached (dup) — no-op
    return;
  }

  node = mem_alloc(UG_CTX, "attach", sizeof(*node));
  if(node == NULL)
  {
    method_unsubscribe(inst, sub_name);
    mem_free(ctx);
    return;
  }

  snprintf(node->method_name, sizeof(node->method_name), "%s",
      method_inst_name(inst));
  snprintf(node->sub_name, sizeof(node->sub_name), "%s", sub_name);
  node->ctx = ctx;

  pthread_mutex_lock(&ug_attach_lock);
  node->next     = ug_attachments;
  ug_attachments = node;
  pthread_mutex_unlock(&ug_attach_lock);

  clam(CLAM_INFO, UG_CTX, "attached observer to bot '%s' (%s)",
      botname, node->method_name);
}

// Drop every observer subscription and free its context. Called at plugin
// teardown so no callback survives into an unloaded .so.
static void
ug_detach_all(void)
{
  ug_attach_t *node;

  pthread_mutex_lock(&ug_attach_lock);
  node           = ug_attachments;
  ug_attachments = NULL;
  pthread_mutex_unlock(&ug_attach_lock);

  while(node != NULL)
  {
    ug_attach_t   *next = node->next;
    method_inst_t *inst = method_find(node->method_name);

    if(inst != NULL)
      method_unsubscribe(inst, node->sub_name);

    mem_free(node->ctx);
    mem_free(node);
    node = next;
  }
}

// ------------------------------------------------------------------ //
// Bot-channel discovery (scan bot.<bot>.<kind>.chan.* under the lock)  //
// ------------------------------------------------------------------ //

#define UG_MAX_SCAN_CHANS  64

typedef struct
{
  char     names[UG_MAX_SCAN_CHANS][METHOD_CHANNEL_SZ];
  uint32_t count;
  size_t   prefix_len;
} ug_scan_t;

// kv_iterate_prefix callback — runs under the KV lock, so it only collects
// (no kv_* calls). Key shape: bot.<bot>.<kind>.chan.<channel>.<property>;
// the channel is the segment between the prefix and the next '.'.
static void
ug_scan_cb(const char *key, kv_type_t type, const char *value, void *data)
{
  ug_scan_t  *sc = data;
  const char *suffix;
  const char *dot;
  size_t      nlen;

  (void)type;
  (void)value;

  if(sc->count >= UG_MAX_SCAN_CHANS)
    return;

  suffix = key + sc->prefix_len;
  dot    = strchr(suffix, '.');
  if(dot == NULL)
    return;

  nlen = (size_t)(dot - suffix);
  if(nlen == 0 || nlen >= METHOD_CHANNEL_SZ)
    return;

  for(uint32_t i = 0; i < sc->count; i++)
    if(strncmp(sc->names[i], suffix, nlen) == 0 && sc->names[i][nlen] == '\0')
      return;                           // already collected

  memcpy(sc->names[sc->count], suffix, nlen);
  sc->names[sc->count][nlen] = '\0';
  sc->count++;
}

// Discover the bot's configured channels and register urlgrabber knobs for
// each. Split from the iterate callback because kv_register must not run
// under the KV lock.
static void
ug_register_bot_channels(const char *botname, const char *kind,
    const char *prefix)
{
  ug_scan_t sc;

  memset(&sc, 0, sizeof(sc));
  sc.prefix_len = strlen(prefix);

  kv_iterate_prefix(prefix, ug_scan_cb, &sc);

  for(uint32_t i = 0; i < sc.count; i++)
    ug_chan_register(botname, kind, sc.names[i]);
}

// ------------------------------------------------------------------ //
// Bot-start event → deferred attach                                   //
// ------------------------------------------------------------------ //

// Worker-thread task body: attach to the named bot. Runs off the clam
// dispatch thread so the (DB-touching) attach never blocks logging.
static void
ug_attach_task(task_t *t)
{
  char *botname = t->data;

  if(botname != NULL)
  {
    ug_attach_bot(botname);
    mem_free(botname);
  }
}

// clam subscriber for "bot_start" events. The message is "'<name>' started
// (<n> methods)"; lift the name and defer the real work to a task.
static void
ug_on_bot_start(const clam_msg_t *msg)
{
  char  name[BOT_NAME_SZ];
  char *copy;

  if(msg == NULL)
    return;

  if(sscanf(msg->msg, "'%63[^']'", name) != 1 || name[0] == '\0')
    return;

  copy = mem_strdup(UG_CTX, "botname", name);
  if(copy == NULL)
    return;

  if(task_add(UG_CTX, TASK_ANY, 200, ug_attach_task, copy) == NULL)
    mem_free(copy);
}

// bot_iterate callback — runs under bot_mutex, so it only records names.
typedef struct
{
  char     names[BOT_NAME_SZ][BOT_NAME_SZ];
  uint32_t count;
} ug_botlist_t;

static void
ug_bot_collect(const char *name, const char *driver_name, bot_state_t state,
    uint32_t method_count, const char *userns_name,
    uint64_t cmd_count, time_t last_activity, void *data)
{
  ug_botlist_t *bl = data;

  (void)driver_name;
  (void)method_count;
  (void)userns_name;
  (void)cmd_count;
  (void)last_activity;

  // Only running bots have resolved, subscribable method instances; bots
  // that start later arrive through the bot_start event.
  if(state != BOT_RUNNING || bl->count >= BOT_NAME_SZ)
    return;

  snprintf(bl->names[bl->count], BOT_NAME_SZ, "%s", name);
  bl->count++;
}

// ------------------------------------------------------------------ //
// Lifecycle                                                           //
// ------------------------------------------------------------------ //

static bool
ug_start(void)
{
  ug_botlist_t bl;

  // Catch every bot that starts from now on (cold-boot restore + runtime).
  clam_subscribe(UG_CTX, CLAM_INFO, "^bot_start ", ug_on_bot_start);

  // Attach to bots already running (the hot-load path).
  memset(&bl, 0, sizeof(bl));
  bot_iterate(ug_bot_collect, &bl);

  for(uint32_t i = 0; i < bl.count; i++)
    ug_attach_bot(bl.names[i]);

  clam(CLAM_INFO, UG_CTX, "urlgrabber started (%u bot(s) attached)",
      bl.count);
  return(SUCCESS);
}

static void
ug_deinit(void)
{
  clam_unsubscribe(UG_CTX);
  ug_detach_all();
  clam(CLAM_INFO, UG_CTX, "urlgrabber plugin deinitialized");
}

// ------------------------------------------------------------------ //
// Plugin descriptor                                                   //
// ------------------------------------------------------------------ //

const plugin_desc_t bm_plugin_desc = {
  .api_version          = PLUGIN_API_VERSION,
  .name                 = "urlgrabber",
  .version              = "1.0",
  .type                 = PLUGIN_FEATURE,
  .kind                 = "urlgrabber",
  .provides             = { { .name = "feature_urlgrabber" } },
  .provides_count       = 1,
  .requires             = { { .name = "bot_chat" } },
  .requires_count       = 1,
  .kv_schema            = ug_kv_schema,
  .kv_schema_count      = sizeof(ug_kv_schema) / sizeof(ug_kv_schema[0]),
  .kv_inst_schema       = NULL,
  .kv_inst_schema_count = 0,
  .init                 = NULL,
  .start                = ug_start,
  .stop                 = NULL,
  .deinit               = ug_deinit,
  .ext                  = NULL,
};
