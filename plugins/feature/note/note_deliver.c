// botmanager — MIT
// note delivery: the pending set, the per-bot observers that consult it,
// and the worker task that claims and announces a user's mail.
//
// The design constraint is that this code runs on every witnessed line of
// every bot. So the hot path is a scan of a small in-memory array and
// nothing else — no DB, no identity resolution — until that array says
// this namespace actually has mail waiting.

#define NOTE_INTERNAL
#include "note.h"

#include "alloc.h"
#include "colors.h"
#include "task.h"
#include "util.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// Method subscriber-name bound (the real one is method-internal).
#define NOTE_SUB_NAME_SZ  64

// ------------------------------------------------------------------ //
// Pending set                                                         //
// ------------------------------------------------------------------ //

// "Who might have mail" — a bounded set of (namespace, recipient) pairs.
// It is an optimisation, never an authority: the DB claim is what decides
// whether a note exists. So a full table degrades safely by setting
// `note_pending_all`, after which every resolved user is checked against
// the DB until the backlog drains.

#define NOTE_PENDING_SLOTS  256

typedef struct
{
  bool     used;
  uint32_t ns_id;
  char     user[USERNS_USER_SZ];
} note_pending_t;

static note_pending_t  note_pending[NOTE_PENDING_SLOTS];
static bool            note_pending_all = false;
static pthread_mutex_t note_pending_lock = PTHREAD_MUTEX_INITIALIZER;

void
note_pending_add(uint32_t ns_id, const char *recipient)
{
  note_pending_t *slot = NULL;

  if(recipient == NULL || recipient[0] == '\0')
    return;

  pthread_mutex_lock(&note_pending_lock);

  for(uint32_t i = 0; i < NOTE_PENDING_SLOTS; i++)
  {
    note_pending_t *s = &note_pending[i];

    if(s->used)
    {
      if(s->ns_id == ns_id && strcasecmp(s->user, recipient) == 0)
      {
        pthread_mutex_unlock(&note_pending_lock);
        return;                         // already flagged
      }
    }

    else if(slot == NULL)
      slot = s;
  }

  if(slot == NULL)
  {
    // Out of slots. Fall back to asking the DB about everyone rather
    // than silently dropping someone's mail on the floor.
    note_pending_all = true;
    pthread_mutex_unlock(&note_pending_lock);

    clam(CLAM_WARN, NOTE_CTX,
        "pending set full (%d slots) — falling back to per-user DB checks",
        NOTE_PENDING_SLOTS);
    return;
  }

  snprintf(slot->user, sizeof(slot->user), "%s", recipient);
  slot->ns_id = ns_id;
  slot->used  = true;

  pthread_mutex_unlock(&note_pending_lock);
}

// Drop a recipient's flag once their mail is known to be drained. Also
// lifts the overflow fallback the moment the set is empty again.
static void
note_pending_clear(uint32_t ns_id, const char *recipient)
{
  bool empty = true;

  if(recipient == NULL || recipient[0] == '\0')
    return;

  pthread_mutex_lock(&note_pending_lock);

  for(uint32_t i = 0; i < NOTE_PENDING_SLOTS; i++)
  {
    note_pending_t *s = &note_pending[i];

    if(!s->used)
      continue;

    if(s->ns_id == ns_id && strcasecmp(s->user, recipient) == 0)
      s->used = false;

    else
      empty = false;
  }

  if(empty)
    note_pending_all = false;

  pthread_mutex_unlock(&note_pending_lock);
}

bool
note_pending_any(uint32_t ns_id)
{
  bool any = false;

  pthread_mutex_lock(&note_pending_lock);

  if(note_pending_all)
    any = true;

  else
    for(uint32_t i = 0; i < NOTE_PENDING_SLOTS && !any; i++)
      if(note_pending[i].used && note_pending[i].ns_id == ns_id)
        any = true;

  pthread_mutex_unlock(&note_pending_lock);
  return(any);
}

bool
note_pending_maybe(uint32_t ns_id, const char *recipient)
{
  bool maybe = false;

  if(recipient == NULL || recipient[0] == '\0')
    return(false);

  pthread_mutex_lock(&note_pending_lock);

  if(note_pending_all)
    maybe = true;

  else
    for(uint32_t i = 0; i < NOTE_PENDING_SLOTS && !maybe; i++)
    {
      const note_pending_t *s = &note_pending[i];

      if(s->used && s->ns_id == ns_id && strcasecmp(s->user, recipient) == 0)
        maybe = true;
    }

  pthread_mutex_unlock(&note_pending_lock);
  return(maybe);
}

// ------------------------------------------------------------------ //
// Delivery                                                            //
// ------------------------------------------------------------------ //

// Everything the worker task needs, resolved by name so nothing dangles
// if the method instance is recreated between queue and run.
typedef struct
{
  char     method_name[METHOD_NAME_SZ];
  char     target     [METHOD_CHANNEL_SZ];   // channel, or the sender for a DM
  char     display    [METHOD_NICKNAME_SZ];  // how to address the recipient
  char     user       [USERNS_USER_SZ];      // canonical recipient username
  uint32_t ns_id;
} note_deliver_t;

static void
note_deliver_task(task_t *t)
{
  note_deliver_t *d = t->data;
  note_row_t      rows[NOTE_DELIVER_MAX];
  method_inst_t  *inst;
  time_t          now = time(NULL);
  int             got;

  if(d == NULL)
    return;

  inst = method_find(d->method_name);

  if(inst == NULL)
    goto out;

  got = note_db_claim(d->ns_id, d->user, rows, NOTE_DELIVER_MAX);

  if(got < 0)
    goto out;                           // DB trouble; keep the flag set

  for(int i = 0; i < got; i++)
  {
    char age[32];
    char line[NOTE_BODY_SZ + 256];

    util_fmt_duration(now - rows[i].created, age, sizeof(age));

    snprintf(line, sizeof(line),
        CLR_CYAN "%s" CLR_RESET ": note from " CLR_CYAN "%s" CLR_RESET
        " (%s ago) — %s",
        d->display, rows[i].sender[0] != '\0' ? rows[i].sender : "someone",
        age, rows[i].body);

    method_send(inst, d->target, line);
  }

  // A short claim means the queue is drained; a full one means there may
  // be more, so leave the flag up for the recipient's next line.
  if(got < NOTE_DELIVER_MAX)
    note_pending_clear(d->ns_id, d->user);

  if(got > 0)
    clam(CLAM_INFO, NOTE_CTX, "delivered %d note(s) to %s on %s",
        got, d->user, d->target);

out:

  method_release(inst);
  mem_free(d);
}

// ------------------------------------------------------------------ //
// Observer                                                            //
// ------------------------------------------------------------------ //

// Per-attachment context: just the bot name, from which everything else
// is (re)resolved so nothing dangles if the bot is torn down.
typedef struct
{
  char bot[BOT_NAME_SZ];
} note_actx_t;

static void
note_observe(const method_msg_t *msg, void *data)
{
  note_actx_t    *ctx = data;
  bot_inst_t     *bot;
  userns_t       *ns;
  note_deliver_t *d;
  const char     *method_name;
  char            user[USERNS_USER_SZ];

  if(ctx == NULL || msg == NULL || msg->kind != METHOD_MSG_MESSAGE)
    return;

  bot = bot_find(ctx->bot);

  if(bot == NULL)                       // bot torn down; detached at deinit
    return;

  ns = bot_get_userns(bot);

  if(ns == NULL)
    return;

  // The gate that makes this free on an idle network: no mail anywhere in
  // this namespace means we never even resolve who spoke.
  if(!note_pending_any(ns->id))
    return;

  // Resolving here duplicates the text method's own per-line resolve.
  // That is deliberate and safe: the call is idempotent by design — a
  // temp-MFA refresh is a stamp, and the one-shot expiry notice fires
  // from whichever caller gets there first, exactly once either way.
  if(!bot_identity_resolve(bot, msg->inst, msg->sender, msg->metadata,
      user, sizeof(user)))
    return;                             // anonymous: no userns identity

  if(!note_pending_maybe(ns->id, user))
    return;

  method_name = method_inst_name(msg->inst);

  if(method_name == NULL)
    return;

  d = mem_alloc(NOTE_CTX, "deliver", sizeof(*d));

  memset(d, 0, sizeof(*d));
  snprintf(d->method_name, sizeof(d->method_name), "%s", method_name);
  snprintf(d->user, sizeof(d->user), "%s", user);
  snprintf(d->display, sizeof(d->display), "%s",
      msg->nickname[0] != '\0' ? msg->nickname : user);
  snprintf(d->target, sizeof(d->target), "%s",
      msg->channel[0] != '\0' ? msg->channel : msg->sender);
  d->ns_id = ns->id;

  // The claim is a DB round-trip; it must not happen on the delivery
  // thread that is still walking this message through its subscribers.
  if(task_add(NOTE_CTX, TASK_ANY, 200, note_deliver_task, d) == NULL)
    mem_free(d);
}

// ------------------------------------------------------------------ //
// Attachment registry                                                 //
// ------------------------------------------------------------------ //

// One live observer subscription. `method_name` (not the instance
// pointer) is stored so teardown can re-resolve safely even if the
// instance was recreated or destroyed underneath us.
typedef struct note_attach
{
  char                method_name[METHOD_NAME_SZ];
  char                sub_name   [NOTE_SUB_NAME_SZ];
  note_actx_t        *ctx;
  struct note_attach *next;
} note_attach_t;

static note_attach_t  *note_attachments = NULL;
static pthread_mutex_t note_attach_lock = PTHREAD_MUTEX_INITIALIZER;

// Idempotent: a duplicate subscription is refused by the method layer and
// simply skipped.
static void
note_attach_bot(const char *botname)
{
  bot_inst_t     *bot;
  method_inst_t  *inst;
  note_actx_t    *ctx;
  note_attach_t  *node;
  char            sub_name[NOTE_SUB_NAME_SZ];

  if(botname == NULL || botname[0] == '\0')
    return;

  bot = bot_find(botname);

  if(bot == NULL)
    return;

  inst = bot_first_method(bot);

  if(inst == NULL)                      // not started / no binding yet
    return;

  ctx = mem_alloc(NOTE_CTX, "actx", sizeof(*ctx));

  snprintf(ctx->bot, sizeof(ctx->bot), "%s", botname);
  snprintf(sub_name, sizeof(sub_name), "note:%.48s", botname);

  if(method_subscribe(inst, sub_name, note_observe, ctx) != SUCCESS)
  {
    method_release(inst);
    mem_free(ctx);                      // already attached (dup) — no-op
    return;
  }

  node = mem_alloc(NOTE_CTX, "attach", sizeof(*node));

  snprintf(node->method_name, sizeof(node->method_name), "%s",
      method_inst_name(inst));
  snprintf(node->sub_name, sizeof(node->sub_name), "%s", sub_name);
  node->ctx = ctx;

  pthread_mutex_lock(&note_attach_lock);
  node->next       = note_attachments;
  note_attachments = node;
  pthread_mutex_unlock(&note_attach_lock);

  method_release(inst);

  clam(CLAM_INFO, NOTE_CTX, "attached observer to bot '%s' (%s)",
      botname, node->method_name);
}

// Drop every observer subscription and free its context, so no callback
// survives into an unloaded .so.
static void
note_detach_all(void)
{
  note_attach_t *node;

  pthread_mutex_lock(&note_attach_lock);
  node             = note_attachments;
  note_attachments = NULL;
  pthread_mutex_unlock(&note_attach_lock);

  while(node != NULL)
  {
    note_attach_t *next = node->next;
    method_inst_t *inst = method_find(node->method_name);

    if(inst != NULL)
    {
      method_unsubscribe(inst, node->sub_name);
      method_release(inst);
    }

    mem_free(node->ctx);
    mem_free(node);
    node = next;
  }
}

// ------------------------------------------------------------------ //
// Bot-start event → deferred attach                                   //
// ------------------------------------------------------------------ //

static void
note_attach_task(task_t *t)
{
  char *botname = t->data;

  if(botname != NULL)
  {
    note_attach_bot(botname);
    mem_free(botname);
  }
}

// clam subscriber for "bot_start" events. The message is "'<name>'
// started (<n> methods)"; lift the name and defer the real work to a task
// so the clam dispatch thread never blocks.
static void
note_on_bot_start(const clam_msg_t *msg)
{
  char  name[BOT_NAME_SZ];
  char *copy;

  if(msg == NULL)
    return;

  if(sscanf(msg->msg, "'%63[^']'", name) != 1 || name[0] == '\0')
    return;

  copy = mem_strdup(NOTE_CTX, "botname", name);

  if(task_add(NOTE_CTX, TASK_ANY, 200, note_attach_task, copy) == NULL)
    mem_free(copy);
}

// bot_iterate callback — runs under bot_mutex, so it only records names.
typedef struct
{
  char     names[BOT_NAME_SZ][BOT_NAME_SZ];
  uint32_t count;
} note_botlist_t;

static void
note_bot_collect(const char *name, const char *method_kinds,
    bot_state_t state, uint32_t method_count, const char *userns_name,
    uint64_t cmd_count, time_t last_activity, void *data)
{
  note_botlist_t *bl = data;

  (void)method_kinds;
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

bool
note_deliver_start(void)
{
  note_botlist_t bl;
  int            loaded;

  // Notes left before a restart are still owed to their recipients.
  loaded = note_db_pending_reload();

  if(loaded < 0)
    clam(CLAM_WARN, NOTE_CTX, "could not load the pending set");

  // Catch every bot that starts from now on (cold-boot restore + runtime).
  clam_subscribe(NOTE_CTX, CLAM_INFO, "^bot_start ", note_on_bot_start);

  // Attach to bots already running (the hot-load path).
  memset(&bl, 0, sizeof(bl));
  bot_iterate(note_bot_collect, &bl);

  for(uint32_t i = 0; i < bl.count; i++)
    note_attach_bot(bl.names[i]);

  clam(CLAM_INFO, NOTE_CTX,
      "note delivery started (%u bot(s) attached, %d recipient(s) pending)",
      bl.count, loaded > 0 ? loaded : 0);
  return(SUCCESS);
}

void
note_deliver_stop(void)
{
  clam_unsubscribe(NOTE_CTX);
  note_detach_all();
}
