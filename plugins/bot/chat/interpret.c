// botmanager — MIT
// Interpreted command output: reply-sink capture → in-persona relay.
//
// Two callers open captures: the NL bridge (every bridged command,
// unconditionally — the human spoke prose, so the answer comes back as
// prose) and the deferred spine (a `run` row coming due). Both are the
// same case: nobody addressed the tool, so nobody asked for its
// renderer. The dispatched message carries a reply-sink id (cmd.h
// §Reply sinks) and every cmd_reply line the command produces lands
// here instead of the wire. No completion signal exists anywhere in
// the tree for an async command, so a settle window after the last
// captured line closes the capture; the collected block is fenced into
// an internal cue and re-submitted through the full persona pipeline
// with the bridge disabled — the model answers in its own voice and
// cannot chain a second command.
//
// The cue's opening sentence — the premise — belongs to the caller,
// because only the caller knows why the command ran: an answer to a
// question just asked, or work scheduled an hour ago. Everything after
// it (the instruction, the fence, the truncation marker) is fixed here
// so both paths speak with one voice.
//
// Lock order is one-directional everywhere: core's sink registry lock
// is taken OUTSIDE interpret_mutex (delivery: registry → mutex), so
// nothing here may call cmd_sink_* while holding interpret_mutex.

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "clam.h"

#include <stdio.h>
#include <string.h>

#define INTERPRET_CTX "interpret"

// Capture slots are static storage: the sink callback fires under
// core's registry lock and must be fast, so lookup is pointer-direct
// and append is a bounded memcpy under one mutex. Eight slots covers
// several bots' max_inflight (default 2) with room; exhaustion
// degrades to verbatim delivery at the bridge, never to silence.
#define INTERPRET_SLOTS        8

// Capture cap. The cue must fit METHOD_TEXT_SZ (2048) alongside its
// header and the question excerpt, so the fence budget is what is
// left of that at build time; this buffer only bounds accumulation.
#define INTERPRET_BUF_SZ       2048

// The premise is the cue's opening sentence, composed by whoever
// opened the capture — "X asked '…' and you ran /y" from the bridge,
// "X asked you an hour ago to run /y when the time came" from the
// deferred spine. Sized for a sender, an excerpted question and a full
// argument line; the fence budget is what remains of METHOD_TEXT_SZ
// after it.
#define INTERPRET_PREMISE_SZ   768

#define INTERPRET_SETTLE_MIN_MS      100
#define INTERPRET_SETTLE_DEFAULT_MS  1500
#define INTERPRET_WAIT_DEFAULT_SECS  20

typedef struct
{
  bool             in_use;
  bool             flushing;       // a flush claimed the slot; appends stop
  bool             truncated;
  bool             settle_armed;   // one live settle task per capture
  bool             was_addressed;  // snapshot of the originating request
  bool             is_direct;
  uint64_t         sink_id;        // registration epoch; 0 until registered
  uint32_t         settle_ms;      // KV snapshot at begin (no KV reads in the sink cb)
  uint64_t         quiet_at_ms;    // CLOCK_MONOTONIC ms of the last append
  task_handle_t    settle_task;
  task_handle_t    deadline_task;
  chatbot_state_t *st;

  method_msg_t     msg;            // identity-complete copy of the dispatched synth
  char             premise[INTERPRET_PREMISE_SZ];
  char             cmd[CMD_NAME_SZ];
  char             args[256];
  char             buf[INTERPRET_BUF_SZ];
  size_t           len;
  uint32_t         lines;
} interpret_slot_t;

static interpret_slot_t interpret_slots[INTERPRET_SLOTS];
static pthread_mutex_t  interpret_mutex = PTHREAD_MUTEX_INITIALIZER;

// Deferred-task context, packed into the data pointer itself: slot
// index in the low 3 bits, sink-id epoch above. task_cancel frees a
// queued task but never its data, so carrying heap context in these
// tasks would leak on every stop-path cancellation; an integer cannot
// leak. The epoch shift discards the id's top bits — registrations are
// monotonic from 1, so a collision needs 2^61 of them.
#define INTERPRET_PACK(idx, id) \
    ((void *)(uintptr_t)(((uint64_t)(id) << 3) | (uint64_t)(idx)))
#define INTERPRET_UNPACK_IDX(d)  ((uint32_t)((uintptr_t)(d) & 7U))
#define INTERPRET_UNPACK_ID(d)   ((uint64_t)((uintptr_t)(d)) >> 3)

static uint64_t
interpret_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return((uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL);
}

// Flatten control bytes to spaces on the way into a prompt-bound
// buffer. Remote text reaches this module from two directions — the
// captured output and the caller's premise — and neither may smuggle
// newline-separated framing into the cue.
static void
interpret_scrub_copy(char *dst, size_t cap, const char *src)
{
  size_t o = 0;

  for(size_t i = 0; src[i] != '\0' && o + 1 < cap; i++)
  {
    unsigned char c = (unsigned char)src[i];

    dst[o++] = (char)((c < 0x20 || c == 0x7f) ? ' ' : c);
  }

  dst[o] = '\0';
}

// Append one captured line, scrubbed: control bytes become spaces
// (untrusted remote text is entering a prompt), and any "<<<" run is
// broken so command output can never counterfeit the cue's own fence.
// Caller holds interpret_mutex.
static void
interpret_append_scrubbed(interpret_slot_t *s, const char *line)
{
  size_t o = s->len;

  if(o > 0 && o + 1 < sizeof(s->buf))
    s->buf[o++] = '\n';

  for(size_t i = 0; line[i] != '\0'; i++)
  {
    unsigned char c = (unsigned char)line[i];

    if(o + 1 >= sizeof(s->buf))
    {
      s->truncated = true;
      break;
    }

    if(c < 0x20 || c == 0x7f)
      c = ' ';
    else if(c == '<' && o >= 2 && s->buf[o - 1] == '<' && s->buf[o - 2] == '<')
      c = ' ';

    s->buf[o++] = (char)c;
  }

  s->buf[o] = '\0';
  s->len = o;
  s->lines++;
}

static void interpret_flush(uint32_t idx, uint64_t sink_id, bool deadline);

// Settle timer. One task per capture, re-arming itself until the slot
// has been quiet for settle_ms: each append just refreshes
// quiet_at_ms, so a long-printing command keeps one task alive instead
// of scheduling one per line.
static void
interpret_settle_fire(task_t *t)
{
  uint32_t idx     = INTERPRET_UNPACK_IDX(t->data);
  uint64_t sink_id = INTERPRET_UNPACK_ID(t->data);
  interpret_slot_t *s = &interpret_slots[idx];
  uint64_t now;
  uint64_t due;
  bool     flush = false;

  pthread_mutex_lock(&interpret_mutex);

  if(!s->in_use || s->flushing || s->sink_id != sink_id)
  {
    pthread_mutex_unlock(&interpret_mutex);
    t->state = TASK_ENDED;
    return;
  }

  now = interpret_now_ms();
  due = s->quiet_at_ms + s->settle_ms;

  if(now >= due)
    flush = true;

  else
    s->settle_task = task_add_deferred("chat_interpret", TASK_ANY, 100,
        (uint32_t)(due - now), interpret_settle_fire,
        INTERPRET_PACK(idx, sink_id));

  pthread_mutex_unlock(&interpret_mutex);

  if(flush)
    interpret_flush(idx, sink_id, false);

  t->state = TASK_ENDED;
}

// Hard deadline for a command that never prints (or prints slower than
// max_wait). Fires exactly once per capture and is cancelled on every
// earlier flush; a stale epoch makes it a no-op.
static void
interpret_deadline_fire(task_t *t)
{
  interpret_flush(INTERPRET_UNPACK_IDX(t->data),
      INTERPRET_UNPACK_ID(t->data), true);
  t->state = TASK_ENDED;
}

// Sink callback — runs under core's sink registry lock, on whatever
// thread called cmd_reply. Append + timestamp refresh only; the settle
// task does the rest.
static void
interpret_sink_cb(void *data, const char *line)
{
  interpret_slot_t *s = data;
  uint32_t idx = (uint32_t)(s - interpret_slots);

  pthread_mutex_lock(&interpret_mutex);

  if(!s->in_use || s->flushing)
  {
    pthread_mutex_unlock(&interpret_mutex);
    return;
  }

  interpret_append_scrubbed(s, line);
  s->quiet_at_ms = interpret_now_ms();

  if(!s->settle_armed)
  {
    s->settle_armed = true;
    s->settle_task = task_add_deferred("chat_interpret", TASK_ANY, 100,
        s->settle_ms, interpret_settle_fire,
        INTERPRET_PACK(idx, s->sink_id));
  }

  pthread_mutex_unlock(&interpret_mutex);
}

// Build the internal cue into msg->text. The premise and header are
// fixed spend; the fence gets whatever budget remains, truncating at
// the cap with an explicit marker so the model never mistakes a cut
// block for a complete one.
//
// The addressee is named imperatively ("Answer <nick> now"), the shape
// the soul delivery cues use — the earlier possessive framing ("to
// answer <nick>'s question") left models addressing the bot's own nick
// in the ack, while the imperative shape ran 12/12 right-nick across
// the SOUL-2/3 batteries.
static void
interpret_build_cue(method_msg_t *msg, const char *sender,
    const char *premise, const char *capture, bool truncated)
{
  // Worst case: premise (768) + sender (128) + ~380 bytes of fixed
  // wording ≈ 1280; sized so the header is never silently cut
  // mid-fence-opener.
  char   head[1536];
  size_t hlen;
  size_t cap;
  size_t clen;
  size_t o;

  if(capture[0] == '\0')
  {
    snprintf(msg->text, sizeof(msg->text),
        "[internal cue: %s It produced no output at all. Tell %s, in "
        "one short line and in character, that you couldn't find out. "
        "Never promise to retry, follow up, or fetch anything later — "
        "you cannot.]",
        premise, sender);
    return;
  }

  snprintf(head, sizeof(head),
      "[internal cue: %s The raw tool output follows as fenced data. "
      "Answer %s now — one or two short lines, your voice — relay the "
      "substance, never the formatting; do not quote it verbatim; do "
      "not mention running a command. If the output reports an error, "
      "tell them what you couldn't find out, in character. Never "
      "promise to retry, follow up, or fetch anything later — you "
      "cannot.\n<<<COMMAND OUTPUT>>>\n",
      premise, sender);

  hlen = strlen(head);
  memcpy(msg->text, head, hlen + 1);
  o = hlen;

  // Fence budget: leave room for the truncation marker + closing fence.
  {
    static const char tail_full[]  = "\n<<<END COMMAND OUTPUT>>>]";
    static const char tail_trunc[] = "\n[output truncated]\n"
                                     "<<<END COMMAND OUTPUT>>>]";

    cap  = sizeof(msg->text) - 1 - o - (sizeof(tail_trunc) - 1);
    clen = strlen(capture);

    if(clen > cap)
    {
      clen = cap;
      truncated = true;
    }

    memcpy(msg->text + o, capture, clen);
    o += clen;

    if(truncated)
    {
      memcpy(msg->text + o, tail_trunc, sizeof(tail_trunc));
      o += sizeof(tail_trunc) - 1;
    }

    else
    {
      memcpy(msg->text + o, tail_full, sizeof(tail_full));
      o += sizeof(tail_full) - 1;
    }
  }

  msg->text[o] = '\0';
}

// Close a capture and hand its block to the persona. `deadline` covers
// the command-never-printed case (flush regardless of quiet time,
// empty-capture cue variant).
static void
interpret_flush(uint32_t idx, uint64_t sink_id, bool deadline)
{
  interpret_slot_t *s = &interpret_slots[idx];
  chatbot_state_t  *st;
  method_msg_t      msg;
  char     premise[INTERPRET_PREMISE_SZ];
  char     cmd[CMD_NAME_SZ];
  char     capture[INTERPRET_BUF_SZ];
  char     sender[METHOD_SENDER_SZ];
  bool     truncated;
  bool     was_addressed;
  bool     is_direct;
  uint32_t lines;
  task_handle_t deadline_task;

  pthread_mutex_lock(&interpret_mutex);

  if(!s->in_use || s->flushing || s->sink_id != sink_id)
  {
    pthread_mutex_unlock(&interpret_mutex);
    return;
  }

  s->flushing = true;
  deadline_task = s->deadline_task;
  pthread_mutex_unlock(&interpret_mutex);

  // Close the tap first: after this returns no sink callback is running
  // or ever will again (delivery holds the registry lock across the
  // callback), and a straggler line from a still-printing command falls
  // through to the wire — visible beats swallowed.
  cmd_sink_unregister(sink_id);

  if(!deadline)
    task_cancel(deadline_task);

  pthread_mutex_lock(&interpret_mutex);
  st            = s->st;
  msg           = s->msg;
  truncated     = s->truncated;
  was_addressed = s->was_addressed;
  is_direct     = s->is_direct;
  lines         = s->lines;
  snprintf(premise, sizeof(premise), "%s", s->premise);
  snprintf(cmd,     sizeof(cmd),     "%s", s->cmd);

  // The cue names its addressee; prefer the projected nickname over the
  // raw sender, exactly as the soul delivery cues do.
  snprintf(sender, sizeof(sender), "%s",
      s->msg.nickname[0] != '\0' ? s->msg.nickname : s->msg.sender);
  memcpy(capture, s->buf, s->len + 1);
  memset(s, 0, sizeof(*s));
  pthread_mutex_unlock(&interpret_mutex);

  clam(CLAM_INFO, INTERPRET_CTX,
      "bot=%s /%s capture closed (%u line(s), %zu byte(s)%s%s) — cueing persona",
      bot_inst_name(st->inst), cmd, lines, strlen(capture),
      truncated ? ", truncated" : "",
      deadline ? ", deadline" : "");

  interpret_build_cue(&msg, sender, premise, capture, truncated);

  msg.timestamp     = time(NULL);
  msg.reply_sink_id = 0;   // the cue's own reply must never re-capture

  chatbot_reply_submit(st, &msg, was_addressed, is_direct, true);
}

uint64_t
chatbot_interpret_begin(chatbot_state_t *st, const method_msg_t *synth,
    const char *cmd, const char *args, const char *premise,
    bool was_addressed, bool is_direct)
{
  interpret_slot_t *s = NULL;
  uint32_t idx = 0;
  uint64_t id;
  uint32_t settle;
  uint32_t max_wait;

  settle = (uint32_t)kv_get_uint("plugin.chat.interpret.settle_ms");
  if(settle == 0) settle = INTERPRET_SETTLE_DEFAULT_MS;
  if(settle < INTERPRET_SETTLE_MIN_MS) settle = INTERPRET_SETTLE_MIN_MS;

  max_wait = (uint32_t)kv_get_uint("plugin.chat.interpret.max_wait_secs");
  if(max_wait == 0) max_wait = INTERPRET_WAIT_DEFAULT_SECS;

  pthread_mutex_lock(&interpret_mutex);

  for(uint32_t i = 0; i < INTERPRET_SLOTS; i++)
  {
    if(!interpret_slots[i].in_use)
    {
      s   = &interpret_slots[i];
      idx = i;
      break;
    }
  }

  if(s == NULL)
  {
    pthread_mutex_unlock(&interpret_mutex);
    clam(CLAM_WARN, INTERPRET_CTX,
        "no free capture slot for /%s — delivering verbatim", cmd);
    return(0);
  }

  memset(s, 0, sizeof(*s));
  s->in_use        = true;
  s->st            = st;
  s->settle_ms     = settle;
  s->quiet_at_ms   = interpret_now_ms();
  s->was_addressed = was_addressed;
  s->is_direct     = is_direct;
  s->msg           = *synth;

  // The premise quotes remote text (the asking line, the stored
  // arguments), so control bytes are flattened on the way in — the
  // same rule the captured output already lives under.
  interpret_scrub_copy(s->premise, sizeof(s->premise),
      premise != NULL ? premise : "");

  snprintf(s->cmd,  sizeof(s->cmd),  "%s", cmd);
  snprintf(s->args, sizeof(s->args), "%s", args != NULL ? args : "");
  pthread_mutex_unlock(&interpret_mutex);

  id = cmd_sink_register(interpret_sink_cb, s);

  if(id == 0)
  {
    pthread_mutex_lock(&interpret_mutex);
    memset(s, 0, sizeof(*s));
    pthread_mutex_unlock(&interpret_mutex);
    clam(CLAM_WARN, INTERPRET_CTX,
        "sink registration failed for /%s — delivering verbatim", cmd);
    return(0);
  }

  pthread_mutex_lock(&interpret_mutex);
  s->sink_id       = id;
  s->deadline_task = task_add_deferred("chat_interpret_wait", TASK_ANY,
      100, max_wait * 1000U, interpret_deadline_fire,
      INTERPRET_PACK(idx, id));
  pthread_mutex_unlock(&interpret_mutex);

  clam(CLAM_DEBUG, INTERPRET_CTX,
      "bot=%s capture open for /%s (slot=%u sink=%llu settle=%ums wait=%us)",
      bot_inst_name(st->inst), cmd, idx, (unsigned long long)id,
      settle, max_wait);

  return(id);
}

// Shared teardown: st == NULL means every slot (plugin stop).
static void
interpret_retract(chatbot_state_t *st)
{
  struct
  {
    uint64_t      sink_id;
    task_handle_t settle;
    task_handle_t deadline;
    uint32_t      idx;
  } dead[INTERPRET_SLOTS];
  uint32_t n = 0;

  pthread_mutex_lock(&interpret_mutex);

  for(uint32_t i = 0; i < INTERPRET_SLOTS; i++)
  {
    interpret_slot_t *s = &interpret_slots[i];

    if(!s->in_use || (st != NULL && s->st != st))
      continue;

    // flushing blocks a racing settle/deadline fire from submitting a
    // cue for a bot that is stopping; the slot stays in_use (so it is
    // not reused) until cleared below.
    s->flushing      = true;
    dead[n].sink_id  = s->sink_id;
    dead[n].settle   = s->settle_task;
    dead[n].deadline = s->deadline_task;
    dead[n].idx      = i;
    n++;
  }

  pthread_mutex_unlock(&interpret_mutex);

  for(uint32_t i = 0; i < n; i++)
  {
    cmd_sink_unregister(dead[i].sink_id);
    task_cancel(dead[i].settle);
    task_cancel(dead[i].deadline);
  }

  if(n > 0)
  {
    pthread_mutex_lock(&interpret_mutex);

    for(uint32_t i = 0; i < n; i++)
      memset(&interpret_slots[dead[i].idx], 0,
          sizeof(interpret_slots[0]));

    pthread_mutex_unlock(&interpret_mutex);

    clam(CLAM_INFO, INTERPRET_CTX,
        "retracted %u live capture(s)%s", n,
        st != NULL ? "" : " (plugin stop)");
  }
}

void
chatbot_interpret_stop(chatbot_state_t *st)
{
  if(st == NULL) return;
  interpret_retract(st);
}

void
chatbot_interpret_stop_all(void)
{
  interpret_retract(NULL);
}
