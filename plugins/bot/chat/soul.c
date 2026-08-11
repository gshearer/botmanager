// botmanager — MIT
// The soul: a per-bot heartbeat that lets a persona act on its own.
// Each tick runs due chores (reminders today, the weather watch next)
// and every chore speaks through the same cue → reply pipeline that
// volunteer speech uses, so persona, contract and markup all inherit.
//
// Durable chore state lives in the DB, never in this mapping: a
// reminder survives restart and reload because delivery is a claimed
// UPDATE (the note plugin's exactly-once shape), not an in-memory
// queue. The scheduler side mirrors extract.c: one periodic task per
// bot, a mutex-guarded sched list whose entries outlive bot stops, and
// a soft latch instead of task churn.

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "clam.h"
#include "db.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define SOUL_CTX "soul"

#define SOUL_INTERVAL_DEFAULT_SECS 60
#define SOUL_INTERVAL_MIN_SECS     5

// One claim batch per tick per bot; a backlog drains at this rate.
#define SOUL_REMIND_CLAIM_MAX      5

// Reminder horizon. Beyond this the ack would be a promise nobody
// remembers making.
#define SOUL_REMIND_MAX_SECS       (30ULL * 86400ULL)

typedef struct soul_sched soul_sched_t;

// A chore is one autonomous duty, run from the tick when its gates
// pass. kv_suffix names a per-chore knob family under
// bot.<n>.behavior.soul.<suffix>.* (NULL = no knobs of its own);
// min_interval_secs floors the per-bot cadence for chores that poll
// paid externals (0 = every tick). fn runs on the tick's worker thread
// and may block on sync db_query / geocoders — the task is TASK_THREAD
// for exactly this reason.
typedef void (*soul_chore_fn_t)(soul_sched_t *s, uint32_t chore,
    chatbot_state_t *st, bot_inst_t *bot);

typedef struct
{
  const char      *name;
  const char      *kv_suffix;
  uint32_t         min_interval_secs;
  soul_chore_fn_t  fn;
} soul_chore_t;

static void soul_chore_reminders(soul_sched_t *, uint32_t,
    chatbot_state_t *, bot_inst_t *);

static const soul_chore_t soul_chores[] = {
  { "reminders", NULL, 0, soul_chore_reminders },
};

#define SOUL_CHORE_COUNT (sizeof(soul_chores) / sizeof(soul_chores[0]))

// Per-bot schedule entry. Entries persist across bot stop/start (the
// extract.c latch pattern): stop flips active off and the parked task
// no-ops until re-scheduled; only plugin stop cancels tasks and only
// plugin deinit frees entries, in that order — each entry is a live
// task's data pointer until the cancel lands.
struct soul_sched
{
  char           bot_name[BOT_NAME_SZ];
  uint32_t       ns_id;
  bool           active;
  task_handle_t  task;
  time_t         last_ran [SOUL_CHORE_COUNT];
  bool           in_flight[SOUL_CHORE_COUNT];
  soul_sched_t  *next;
};

static soul_sched_t   *soul_sched_head = NULL;
static pthread_mutex_t soul_mutex      = PTHREAD_MUTEX_INITIALIZER;

// Statics vanish with the mapping, so an initializer is the whole init
// story; soul_exit() flips it for the teardown window.
static bool            soul_ready      = true;

// Caller holds soul_mutex.
static soul_sched_t *
soul_sched_find_locked(const char *bot_name)
{
  for(soul_sched_t *s = soul_sched_head; s != NULL; s = s->next)
    if(strcmp(s->bot_name, bot_name) == 0)
      return(s);

  return(NULL);
}

// Run one statement, logging failure. SUCCESS/FAIL so the un-claim
// path can notice a miss.
static bool
soul_db_exec(const char *sql)
{
  db_result_t *res = db_result_alloc();
  bool         ok;

  if(res == NULL)
    return(FAIL);

  ok = (db_query(sql, res) == SUCCESS && res->ok) ? SUCCESS : FAIL;

  if(ok != SUCCESS)
    clam(CLAM_WARN, SOUL_CTX, "sql failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");

  db_result_free(res);
  return(ok);
}

static void
soul_copy_col(char *dst, size_t cap, const db_result_t *res,
    uint32_t row, uint32_t col)
{
  const char *s = db_result_get(res, row, col);

  snprintf(dst, cap, "%s", s != NULL ? s : "");
}

// Copy stored user text into a prompt-bound buffer: control bytes
// become spaces so a line typed months ago can't smuggle framing into
// today's cue.
static void
soul_scrub_copy(char *dst, size_t cap, const char *src)
{
  size_t o = 0;

  for(size_t i = 0; src[i] != '\0' && o + 1 < cap; i++)
  {
    unsigned char c = (unsigned char)src[i];

    dst[o++] = (char)((c < 0x20 || c == 0x7f) ? ' ' : c);
  }

  dst[o] = '\0';
}

// ---------- chore: reminders (D6/D7) ----------

static void
soul_chore_reminders(soul_sched_t *s, uint32_t chore,
    chatbot_state_t *st, bot_inst_t *bot)
{
  db_result_t *res;
  char         sql[640];
  uint32_t     rows;
  time_t       now;

  (void)chore;
  (void)bot;

  // Claim-then-read in one statement (note_db_claim's shape): two
  // racing witnesses cannot both deliver a row, and the claim is what
  // makes delivery restart- and reload-safe — the guard is in the DB,
  // not in this mapping.
  snprintf(sql, sizeof(sql),
      "WITH claimed AS ("
      "UPDATE chat_reminders SET delivered_at = NOW() WHERE id IN ("
      "SELECT id FROM chat_reminders WHERE ns_id = %" PRIu32
      " AND delivered_at IS NULL AND due_at <= NOW()"
      " ORDER BY due_at ASC LIMIT %d)"
      " RETURNING id, sender, nickname, username, hostname, verified_id,"
      " method_name, channel, body,"
      " EXTRACT(EPOCH FROM created_at)::BIGINT AS created_epoch)"
      " SELECT * FROM claimed ORDER BY id ASC",
      s->ns_id, SOUL_REMIND_CLAIM_MAX);

  res = db_result_alloc();

  if(res == NULL || db_query(sql, res) != SUCCESS || !res->ok)
  {
    if(res != NULL)
      clam(CLAM_WARN, SOUL_CTX, "reminder claim failed: %s",
          res->error[0] != '\0' ? res->error : "(no driver error)");

    db_result_free(res);
    return;
  }

  now  = time(NULL);
  rows = res->rows;

  for(uint32_t i = 0; i < rows; i++)
  {
    method_msg_t   msg;
    method_inst_t *inst;
    const char    *cell;
    char           mname[METHOD_NAME_SZ];
    char           body[CMD_ARG_SZ];
    char           ago[32];
    int64_t        id;
    time_t         created;

    cell = db_result_get(res, i, 0);
    id   = cell != NULL ? (int64_t)strtoll(cell, NULL, 10) : 0;

    // The method is re-resolved by NAME on every delivery so nothing
    // dangles across a reload (the note plugin's rule).
    soul_copy_col(mname, sizeof(mname), res, i, 6);
    inst = method_find(mname);

    if(inst == NULL)
    {
      // Claim-then-fail must un-claim: with the method gone (unbound,
      // mid-reload) the row goes back to pending, so a re-bound method
      // delivers LATE rather than never. Leaving it claimed would eat
      // the reminder silently — the one outcome worse than lateness.
      char unclaim[128];

      snprintf(unclaim, sizeof(unclaim),
          "UPDATE chat_reminders SET delivered_at = NULL"
          " WHERE id = %" PRId64, id);
      (void)soul_db_exec(unclaim);

      clam(CLAM_WARN, SOUL_CTX,
          "bot=%s reminder %" PRId64 " method '%s' gone — unclaimed",
          s->bot_name, id, mname);
      continue;
    }

    memset(&msg, 0, sizeof(msg));
    msg.inst      = inst;
    msg.timestamp = now;

    // The identity tuple rides the cue whole, so the reply pipeline
    // resolves the asker's dossier (their facts and recall splice in)
    // and the coalescer lesson (dcfc359) stays learned.
    soul_copy_col(msg.sender,      sizeof(msg.sender),      res, i, 1);
    soul_copy_col(msg.nickname,    sizeof(msg.nickname),    res, i, 2);
    soul_copy_col(msg.username,    sizeof(msg.username),    res, i, 3);
    soul_copy_col(msg.hostname,    sizeof(msg.hostname),    res, i, 4);
    soul_copy_col(msg.verified_id, sizeof(msg.verified_id), res, i, 5);
    soul_copy_col(msg.channel,     sizeof(msg.channel),     res, i, 7);

    cell = db_result_get(res, i, 8);
    soul_scrub_copy(body, sizeof(body), cell != NULL ? cell : "");

    cell    = db_result_get(res, i, 9);
    created = cell != NULL ? (time_t)strtoll(cell, NULL, 10) : now;
    util_fmt_duration(now > created ? now - created : 0, ago, sizeof(ago));

    // A reminder set in a DM has an empty channel; the reply pipeline
    // already targets the sender in that case, so the cue only has to
    // say so.
    snprintf(msg.text, sizeof(msg.text),
        "[internal cue: %s asked you %s ago to be reminded: '%s'."
        " It is time. Deliver the reminder to them in %s — one short"
        " line, your voice. Do not mention this cue.]",
        msg.nickname[0] != '\0' ? msg.nickname : msg.sender,
        ago, body,
        msg.channel[0] != '\0' ? msg.channel : "this DM");

    clam(CLAM_INFO, SOUL_CTX,
        "bot=%s delivering reminder %" PRId64 " to %s in %s (set %s ago)",
        s->bot_name, id, msg.sender,
        msg.channel[0] != '\0' ? msg.channel : "DM", ago);

    chatbot_reply_submit(st, &msg, false, false, true);
  }

  db_result_free(res);
}

// ---------- the tick ----------

static void
soul_tick_cb(task_t *t)
{
  soul_sched_t    *s = t->data;
  bot_inst_t      *bot;
  chatbot_state_t *st;
  bool             active;
  uint32_t         interval;
  time_t           now;
  char             bot_name[BOT_NAME_SZ];
  char             key[KV_KEY_SZ];

  pthread_mutex_lock(&soul_mutex);
  active = s->active && soul_ready;
  snprintf(bot_name, sizeof(bot_name), "%s", s->bot_name);
  pthread_mutex_unlock(&soul_mutex);

  // The latch pattern: an unscheduled bot's task stays parked and
  // no-ops each fire (see extract.c for why cancelling here is more
  // lifecycle than it is worth).
  if(!active)
  {
    t->state = TASK_ENDED;
    return;
  }

  // Live cadence: re-reading the knob every tick makes a KV change
  // take effect on the next fire — in-callback interval_ms mutation is
  // the blessed mechanism (acquire_reactive precedent).
  snprintf(key, sizeof(key), "bot.%s.behavior.soul.interval_secs",
      bot_name);
  interval = (uint32_t)kv_get_uint(key);

  if(interval == 0)                     interval = SOUL_INTERVAL_DEFAULT_SECS;
  if(interval < SOUL_INTERVAL_MIN_SECS) interval = SOUL_INTERVAL_MIN_SECS;

  t->interval_ms = interval * 1000U;

  // Gates, cheapest first. A failed gate ends the tick, not the task:
  // everything is re-examined fresh on the next fire.
  bot = bot_find(bot_name);

  if(bot == NULL || bot_get_state(bot) != BOT_RUNNING
      || (st = bot_get_handle(bot)) == NULL)
  {
    t->state = TASK_ENDED;
    return;
  }

  snprintf(key, sizeof(key), "bot.%s.behavior.chat.enabled", bot_name);

  if(kv_get_uint(key) == 0)
  {
    t->state = TASK_ENDED;
    return;
  }

  // D9 — the shared hush gate. Chores skipped under mute are not
  // lost: durable work (a due reminder) stays pending in the DB and
  // delivers on the first tick after the mute lapses.
  if(chatbot_mute_active(bot_name))
  {
    clam(CLAM_DEBUG, SOUL_CTX, "bot=%s tick skipped (muted)", bot_name);
    t->state = TASK_ENDED;
    return;
  }

  now = time(NULL);

  for(uint32_t i = 0; i < SOUL_CHORE_COUNT; i++)
  {
    const soul_chore_t *c   = &soul_chores[i];
    bool                run = false;

    pthread_mutex_lock(&soul_mutex);

    if(s->in_flight[i])
      clam(CLAM_DEBUG, SOUL_CTX,
          "bot=%s chore=%s still in flight — skipped", bot_name, c->name);

    else if(c->min_interval_secs > 0)
    {
      time_t last = s->last_ran[i];

      // Reload zeroes this stamp; floor it to handle creation so a
      // fresh mapping waits one window instead of reading "never ran"
      // as "due now" (the volunteer floor idiom — durable dedup state
      // stays in the DB, never here).
      if(last < st->created_at)
        last = st->created_at;

      if(now - last >= (time_t)c->min_interval_secs)
        run = true;
    }

    else
      run = true;

    if(run)
    {
      s->in_flight[i] = true;
      s->last_ran[i]  = now;
    }

    pthread_mutex_unlock(&soul_mutex);

    if(!run)
      continue;

    c->fn(s, i, st, bot);

    // Every chore today is synchronous, so the flag drops here. A
    // chore whose work outlives its tick (an async fetch fan-out)
    // must clear it from its completion path instead.
    pthread_mutex_lock(&soul_mutex);
    s->in_flight[i] = false;
    pthread_mutex_unlock(&soul_mutex);
  }

  t->state = TASK_ENDED;
}

// ---------- schedule / lifecycle ----------

void
soul_schedule(const char *bot_name, uint32_t ns_id, uint32_t interval_secs)
{
  bool          need_task;
  soul_sched_t *s;

  if(!soul_ready || bot_name == NULL || bot_name[0] == '\0')
    return;

  if(interval_secs == 0)
    interval_secs = SOUL_INTERVAL_DEFAULT_SECS;

  if(interval_secs < SOUL_INTERVAL_MIN_SECS)
    interval_secs = SOUL_INTERVAL_MIN_SECS;

  pthread_mutex_lock(&soul_mutex);
  s = soul_sched_find_locked(bot_name);

  if(s == NULL)
  {
    s = mem_alloc("chat", "soul_sched", sizeof(*s));

    if(s == NULL)
    {
      pthread_mutex_unlock(&soul_mutex);
      return;
    }

    memset(s, 0, sizeof(*s));
    snprintf(s->bot_name, sizeof(s->bot_name), "%s", bot_name);
    s->next         = soul_sched_head;
    soul_sched_head = s;
  }

  s->ns_id  = ns_id;
  s->active = true;

  // If a task already exists for this bot, it re-reads everything on
  // its next fire; otherwise spawn one. First fire is immediate by
  // task_add_periodic contract.
  need_task = (s->task == TASK_HANDLE_NONE);
  pthread_mutex_unlock(&soul_mutex);

  if(need_task)
  {
    char          tname[64];
    task_handle_t t;

    snprintf(tname, sizeof(tname), "soul.%.32s", bot_name);

    t = task_add_periodic(tname, TASK_THREAD, 200,
        interval_secs * 1000U, soul_tick_cb, s);

    pthread_mutex_lock(&soul_mutex);
    s->task = t;
    pthread_mutex_unlock(&soul_mutex);

    clam(CLAM_INFO, SOUL_CTX, "bot '%s' heartbeat every %u s",
        bot_name, interval_secs);
  }
}

void
soul_unschedule(const char *bot_name)
{
  soul_sched_t *s;

  if(bot_name == NULL)
    return;

  pthread_mutex_lock(&soul_mutex);
  s = soul_sched_find_locked(bot_name);

  if(s != NULL)
    s->active = false;

  pthread_mutex_unlock(&soul_mutex);

  if(s != NULL)
    clam(CLAM_INFO, SOUL_CTX, "bot '%s' heartbeat latched off", bot_name);
}

void
soul_stop(void)
{
  uint32_t cancelled = 0;

  pthread_mutex_lock(&soul_mutex);

  for(soul_sched_t *s = soul_sched_head; s != NULL; s = s->next)
  {
    s->active = false;

    if(s->task == TASK_HANDLE_NONE)
      continue;

    task_cancel(s->task);
    s->task = TASK_HANDLE_NONE;
    cancelled++;
  }

  pthread_mutex_unlock(&soul_mutex);

  if(cancelled > 0)
    clam(CLAM_DEBUG, SOUL_CTX, "cancelled %u heartbeat(s)", cancelled);
}

void
soul_exit(void)
{
  soul_sched_t *s;

  if(!soul_ready)
    return;

  soul_ready = false;

  // Free the sched list. Each entry is a live task's `data`, so
  // soul_stop() must already have cancelled them.
  pthread_mutex_lock(&soul_mutex);
  s               = soul_sched_head;
  soul_sched_head = NULL;

  while(s != NULL)
  {
    soul_sched_t *next = s->next;

    mem_free(s);
    s = next;
  }

  pthread_mutex_unlock(&soul_mutex);
}

// The chat DDL discipline (memory_ensure_tables): owner-run idempotent
// batches at plugin start(). Called after dossier_register_config so
// the dossier(id) FK target exists.
void
soul_ensure_schema(void)
{
  (void)soul_db_exec(
      "CREATE TABLE IF NOT EXISTS chat_reminders ("
      " id           BIGSERIAL    PRIMARY KEY,"
      " ns_id        INTEGER      NOT NULL REFERENCES userns(id) ON DELETE CASCADE,"
      " dossier_id   BIGINT       REFERENCES dossier(id) ON DELETE SET NULL,"
      " sender       VARCHAR(128) NOT NULL,"
      " nickname     VARCHAR(64)  NOT NULL DEFAULT '',"
      " username     VARCHAR(64)  NOT NULL DEFAULT '',"
      " hostname     VARCHAR(128) NOT NULL DEFAULT '',"
      " verified_id  VARCHAR(128) NOT NULL DEFAULT '',"
      " method_name  VARCHAR(64)  NOT NULL,"
      " channel      VARCHAR(128) NOT NULL DEFAULT '',"
      " body         TEXT         NOT NULL,"
      " due_at       TIMESTAMPTZ  NOT NULL,"
      " created_at   TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
      " delivered_at TIMESTAMPTZ"
      ")");

  (void)soul_db_exec(
      "CREATE INDEX IF NOT EXISTS idx_chat_reminders_due"
      " ON chat_reminders(ns_id, due_at) WHERE delivered_at IS NULL");
}

// ---------- !remind (D7) ----------

static bool
soul_remind_insert(uint32_t ns_id, int64_t dossier,
    const method_msg_t *msg, const char *method_name,
    const char *body, uint64_t secs)
{
  char *e_sender = db_escape(msg->sender);
  char *e_nick   = db_escape(msg->nickname);
  char *e_user   = db_escape(msg->username);
  char *e_host   = db_escape(msg->hostname);
  char *e_vid    = db_escape(msg->verified_id);
  char *e_meth   = db_escape(method_name);
  char *e_chan   = db_escape(msg->channel);
  char *e_body   = db_escape(body);
  bool  ok       = FAIL;

  if(e_sender != NULL && e_nick != NULL && e_user != NULL
      && e_host != NULL && e_vid != NULL && e_meth != NULL
      && e_chan != NULL && e_body != NULL)
  {
    char dossier_cell[32];
    char sql[3072];

    if(dossier > 0)
      snprintf(dossier_cell, sizeof(dossier_cell), "%" PRId64, dossier);
    else
      snprintf(dossier_cell, sizeof(dossier_cell), "NULL");

    // due_at is computed DB-side so the daemon's clock and the DB's
    // never argue about when "in 5 minutes" is.
    snprintf(sql, sizeof(sql),
        "INSERT INTO chat_reminders"
        " (ns_id, dossier_id, sender, nickname, username, hostname,"
        "  verified_id, method_name, channel, body, due_at)"
        " VALUES (%" PRIu32 ", %s, '%s', '%s', '%s', '%s', '%s', '%s',"
        " '%s', '%s', NOW() + %llu * INTERVAL '1 second')",
        ns_id, dossier_cell, e_sender, e_nick, e_user, e_host, e_vid,
        e_meth, e_chan, e_body, (unsigned long long)secs);

    ok = soul_db_exec(sql);
  }

  mem_free(e_sender);
  mem_free(e_nick);
  mem_free(e_user);
  mem_free(e_host);
  mem_free(e_vid);
  mem_free(e_meth);
  mem_free(e_chan);
  mem_free(e_body);

  return(ok);
}

static const cmd_arg_desc_t ad_remind[] = {
  { "duration", CMD_ARG_NONE, CMD_ARG_REQUIRED, 32, NULL },
  { "message",  CMD_ARG_NONE, CMD_ARG_REQUIRED | CMD_ARG_REST, 0, NULL },
};

static void
cmd_remind(const cmd_ctx_t *ctx)
{
  chatbot_state_t *st;
  userns_t        *ns;
  const char      *method_name;
  const char      *body;
  uint64_t         secs;
  int64_t          dossier;
  char             key[KV_KEY_SZ];
  char             ack[96];

  secs = chatbot_parse_duration_secs(ctx->parsed->argv[0]);

  if(secs == 0)
  {
    cmd_reply(ctx, "bad duration (use e.g. 30s, 5m, 2h, 1d)");
    return;
  }

  if(secs > SOUL_REMIND_MAX_SECS)
  {
    cmd_reply(ctx, "that's too far out — keep it under 30 days");
    return;
  }

  st = bot_get_handle(ctx->bot);
  ns = bot_get_userns(ctx->bot);

  if(st == NULL || ns == NULL)
  {
    cmd_reply(ctx, "this bot has no namespace to keep reminders in");
    return;
  }

  // A reminder is delivered as persona speech, so a command-only bot
  // (chat disabled) would accept it and never say it. Refuse the dead
  // letter up front.
  snprintf(key, sizeof(key), "bot.%s.behavior.chat.enabled",
      bot_inst_name(ctx->bot));

  if(kv_get_uint(key) == 0)
  {
    cmd_reply(ctx, "this bot doesn't speak (chat is disabled) — it"
        " would take your reminder and never deliver it");
    return;
  }

  method_name = method_inst_name(ctx->msg->inst);

  if(method_name == NULL || method_name[0] == '\0')
  {
    cmd_reply(ctx, "cannot tell which method to deliver on");
    return;
  }

  // 0 (unmatched sender, anonymous dossiers off) stores NULL —
  // delivery keys on the stored tuple, so a dossier is attribution,
  // never a requirement.
  dossier = chatbot_resolve_dossier(st, ctx->msg);
  body    = ctx->parsed->argv[1];

  if(soul_remind_insert(ns->id, dossier, ctx->msg, method_name,
      body, secs) != SUCCESS)
  {
    cmd_reply(ctx, "failed to store the reminder");
    return;
  }

  clam(CLAM_INFO, SOUL_CTX,
      "bot=%s reminder set by %s for %s in '%s' (%llu s)",
      bot_inst_name(ctx->bot), ctx->msg->sender,
      ctx->parsed->argv[0], ctx->msg->channel,
      (unsigned long long)secs);

  // Persona-neutral on purpose: a persona with `interpret: remind`
  // captures this line and voices it in character instead.
  snprintf(ack, sizeof(ack), "noted — I'll remind you in %s.",
      ctx->parsed->argv[0]);
  cmd_reply(ctx, ack);
}

static const cmd_nl_slot_t remind_slots[] = {
  { .name  = "duration",
    .type  = CMD_NL_ARG_DURATION,
    .flags = CMD_NL_SLOT_REQUIRED },
  { .name  = "message",
    .type  = CMD_NL_ARG_FREE,
    .flags = CMD_NL_SLOT_REQUIRED | CMD_NL_SLOT_REMAINDER },
};

static const cmd_nl_example_t remind_examples[] = {
  { .utterance  = "remind me in 20 minutes to flip the steaks",
    .invocation = "/remind 20m flip the steaks" },
  { .utterance  = "poke me about the laundry in two hours",
    .invocation = "/remind 2h the laundry" },
};

static const cmd_nl_t remind_nl = {
  .when          = "Someone asks to be reminded of something after a"
                   " delay, or asks you to poke them later.",
  .syntax        = "/remind <duration> <message>",
  .slots         = remind_slots,
  .slot_count    = (uint8_t)(sizeof(remind_slots)
                             / sizeof(remind_slots[0])),
  .examples      = remind_examples,
  .example_count = (uint8_t)(sizeof(remind_examples)
                             / sizeof(remind_examples[0])),
  .dispatch_text = NULL,
};

bool
soul_remind_register(void)
{
  return(cmd_register("chat", "remind",
      "remind <duration> <message>",
      "Set a reminder the bot delivers when it comes due",
      "Stores a reminder in the database and delivers it in the bot's\n"
      "own voice once due (checked every behavior.soul.interval_secs,\n"
      "default 60 s). Durations read like 30s, 5m, 2h or 1d, up to 30\n"
      "days; the message is kept to ~250 characters. Set in a channel\n"
      "it is delivered there; set in a DM it comes back as a DM.\n"
      "Reminders survive restarts and reloads.",
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_remind, NULL, NULL, NULL,
      ad_remind, (uint8_t)(sizeof(ad_remind) / sizeof(ad_remind[0])),
      NULL, &remind_nl));
}
