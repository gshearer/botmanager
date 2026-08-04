// botmanager — MIT
// Reachy Mini protocol driver: the robot's ear, wired into the brain.
#define REACHY_INTERNAL
#include "reachy.h"

// ----------------------------------------------------------------------
// Instance KV schema — "bot.<name>.reachy.*"
// ----------------------------------------------------------------------

// Per-bot policy. The robot's own address is plugin-level over in
// reachyapi (one robot, addressed plugin-wide); everything here is a
// question about this bot's manners, which is exactly what changes
// between two creatures sharing one body.
static const plugin_kv_entry_t reachy_inst_kv_schema[] = {
  { "speaker", KV_STR, "",
    "User namespace handle every utterance is attributed to; empty "
    "leaves the voice anonymous" },
  { "channel", KV_STR, "voice",
    "Room name utterances arrive on. Empty would make them direct "
    "messages, which costs the bot its unprompted speech" },

  { "stt_model", KV_STR, "whisper1",
    "Registered speech-to-text model utterances are transcribed by" },
  { "tts_model", KV_STR, "kokoro1",
    "Registered text-to-speech model replies are spoken through" },
  { "tts_voice", KV_STR, "af_heart",
    "Voice id the speech host synthesizes with" },
  { "tts_speed", KV_STR, "1.0",
    "Synthesis rate multiplier; 1.0 is the voice's natural pace" },

  { "volume", KV_UINT8, "60",
    "Speaker volume applied to the robot at connect (0-100)" },

  { "attention.mode", KV_STR, "open",
    "`open` hears everything in the room; `name` hears only what is "
    "addressed to the bot" },
  { "attention.names", KV_STR, "",
    "Extra comma-separated names the bot answers to in `name` mode; "
    "empty means its own name only" },
  { "attention.window_s", KV_UINT32, "90",
    "Seconds attention stays open after the bot is addressed by name" },

  { "barge_in", KV_UINT8, "0",
    "Stop speaking when a human starts. DEFAULT OFF: the microphone "
    "array reports speech in half of all samples of a silent room, so "
    "this interrupts the bot rather than for it — see reachy_barged()" },
  { "wobble", KV_UINT8, "1",
    "Drive speech-synced head motion from played audio at connect" },
  { "tracking_weight", KV_STR, "0.6",
    "Face-tracking strength in [0,1] applied at connect; 0 leaves "
    "tracking alone" },
  { "sleep_on_disconnect", KV_UINT8, "1",
    "Settle the robot into its shell when the bot stops; the daemon "
    "drops torque at the end of that move, so it ends up limp. A "
    "plugin reload is exempt — that is a blip, not a goodbye" },

  { "prefix", KV_STR, "!",
    "Command prefix this bot answers to when spoken aloud" },
};

// ----------------------------------------------------------------------
// Plugin-global live-thread registry
// ----------------------------------------------------------------------

// stop() must join every thread whose body lives in this mapping, and
// the only place a join is legal is stop() itself: disconnect() and
// destroy() both run under the non-recursive method_mutex, and a
// delivering thread may be blocked acquiring that very mutex. So each
// thread files its handle here on the way in and withdraws it on the
// way out, and stop() joins whatever is left.
static pthread_mutex_t reachy_threads_mutex = PTHREAD_MUTEX_INITIALIZER;
static task_handle_t   reachy_threads[REACHY_MAX_THREADS];
static uint32_t        reachy_threads_count = 0;

static bool
reachy_thread_track(task_handle_t h)
{
  bool ok = false;

  pthread_mutex_lock(&reachy_threads_mutex);

  if(reachy_threads_count < REACHY_MAX_THREADS)
  {
    reachy_threads[reachy_threads_count++] = h;
    ok = true;
  }

  pthread_mutex_unlock(&reachy_threads_mutex);

  if(!ok)
    clam(CLAM_WARN, REACHY_CTX,
        "thread registry full at %u — this thread cannot be joined on "
        "unload", (uint32_t)REACHY_MAX_THREADS);

  return(ok ? SUCCESS : FAIL);
}

static void
reachy_thread_forget(task_handle_t h)
{
  pthread_mutex_lock(&reachy_threads_mutex);

  for(uint32_t i = 0; i < reachy_threads_count; i++)
  {
    if(reachy_threads[i] != h)
      continue;

    reachy_threads[i] = reachy_threads[--reachy_threads_count];
    break;
  }

  pthread_mutex_unlock(&reachy_threads_mutex);
}

// ----------------------------------------------------------------------
// Telling a reload apart from a goodbye
// ----------------------------------------------------------------------

// disconnect() is called for both, by the same core path, and the two
// want opposite things from the body: a bot being stopped should be put
// to bed, while a bot whose code is being swapped underneath it should
// not notice at all. suspend() is the one signal that separates them —
// core calls it only on the reload path, immediately before it detaches
// our bots, and detaching is what runs disconnect().
//
// Set on the way into a reload, cleared on the way out. Cleared in
// resume() rather than in connect(), because a reload that is refused
// downstream is put straight back without anything having disconnected
// at all, and resume() is the only call both paths share.
static bool reachy_reloading = false;  // atomic

// ----------------------------------------------------------------------
// State lifetime and bounded waiting
// ----------------------------------------------------------------------

static void
reachy_state_ref(reachy_state_t *st)
{
  __atomic_fetch_add(&st->refs, 1, __ATOMIC_RELAXED);
}

static void
reachy_state_unref(reachy_state_t *st)
{
  if(__atomic_sub_fetch(&st->refs, 1, __ATOMIC_ACQ_REL) != 0)
    return;

  pthread_cond_destroy(&st->wake_cond);
  pthread_cond_destroy(&st->mouth_cond);
  pthread_mutex_destroy(&st->wake_mutex);
  pthread_mutex_destroy(&st->mouth_mutex);
  pthread_mutex_destroy(&st->attn_mutex);
  mem_free(st);
}

static bool
reachy_stopping(const reachy_state_t *st)
{
  return(__atomic_load_n(&st->shutdown, __ATOMIC_ACQUIRE)
      || pool_shutting_down());
}

// Absolute CLOCK_REALTIME deadline `ms` from now — the form
// pthread_cond_timedwait wants.
static void
reachy_deadline(struct timespec *ts, uint32_t ms)
{
  clock_gettime(CLOCK_REALTIME, ts);

  ts->tv_sec  += (time_t)(ms / 1000u);
  ts->tv_nsec += (long)(ms % 1000u) * 1000000L;

  if(ts->tv_nsec >= 1000000000L)
  {
    ts->tv_sec++;
    ts->tv_nsec -= 1000000000L;
  }
}

static bool
reachy_expired(const struct timespec *deadline)
{
  struct timespec now;

  clock_gettime(CLOCK_REALTIME, &now);

  return(now.tv_sec > deadline->tv_sec
      || (now.tv_sec == deadline->tv_sec
          && now.tv_nsec >= deadline->tv_nsec));
}

// Sleep up to `ms`, waking early when the instance is torn down.
// returns: true when the caller should carry on, false to unwind.
static bool
reachy_nap(reachy_state_t *st, uint32_t ms)
{
  struct timespec deadline;

  reachy_deadline(&deadline, ms);

  pthread_mutex_lock(&st->wake_mutex);

  while(!reachy_stopping(st) && !reachy_expired(&deadline))
    pthread_cond_timedwait(&st->wake_cond, &st->wake_mutex, &deadline);

  pthread_mutex_unlock(&st->wake_mutex);

  return(!reachy_stopping(st));
}

// ----------------------------------------------------------------------
// The bounded wait context — one shape for every request this driver
// blocks on, whether it is the ear's long poll or the mouth's synthesis
// ----------------------------------------------------------------------

static reachy_wait_t *
reachy_wait_create(void)
{
  reachy_wait_t *p = mem_alloc(REACHY_CTX, "wait", sizeof(*p));

  memset(p, 0, sizeof(*p));

  pthread_mutex_init(&p->mutex, NULL);
  pthread_cond_init(&p->cond, NULL);

  // The waiter and the completion each hold one.
  p->refs = 2;

  return(p);
}

static void
reachy_wait_unref(reachy_wait_t *p)
{
  if(__atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL) != 0)
    return;

  // Only reached with an untaken payload when the waiter walked away
  // before the transfer landed.
  if(p->wav != NULL)
    mem_free(p->wav);

  pthread_cond_destroy(&p->cond);
  pthread_mutex_destroy(&p->mutex);
  mem_free(p);
}

// Wait up to `ms` for the completion to run, in slices so a teardown is
// felt without a second condvar to broadcast on. Each caller brings its
// own patience: a bridge poll, a synthesis and a direction probe are
// three quite different lengths of reasonable.
// returns: true when the completion actually ran.
static bool
reachy_wait_for(reachy_state_t *st, reachy_wait_t *p, uint32_t ms)
{
  struct timespec deadline;
  bool            done;

  reachy_deadline(&deadline, ms);

  pthread_mutex_lock(&p->mutex);

  while(!p->done && !reachy_stopping(st) && !reachy_expired(&deadline))
  {
    struct timespec slice;

    reachy_deadline(&slice, REACHY_WAIT_SLICE_MS);
    pthread_cond_timedwait(&p->cond, &p->mutex, &slice);
  }

  done = p->done;
  pthread_mutex_unlock(&p->mutex);

  return(done);
}

// ----------------------------------------------------------------------
// Instance KV readers
// ----------------------------------------------------------------------

// kv_get_str hands back internal storage that is only valid until the
// value changes, so every read copies out immediately — which is also
// why there is no reachy_kv_str returning a borrowed pointer.
static void
reachy_kv_copy(const reachy_state_t *st, const char *suffix, char *out,
    size_t cap)
{
  char        key[KV_KEY_SZ];
  const char *val;

  snprintf(key, sizeof(key), "%s%s", st->kv_prefix, suffix);
  val = kv_get_str(key);

  snprintf(out, cap, "%s", val != NULL ? val : "");
}

static uint64_t
reachy_kv_uint(const reachy_state_t *st, const char *suffix)
{
  char key[KV_KEY_SZ];

  snprintf(key, sizeof(key), "%s%s", st->kv_prefix, suffix);

  return(kv_get_uint(key));
}

// The 0/1 knobs are KV_UINT8 rather than KV_BOOL, matching the charter's
// pinned schema — anything non-zero is on.
static bool
reachy_kv_flag(const reachy_state_t *st, const char *suffix)
{
  return(reachy_kv_uint(st, suffix) != 0);
}

// tts_speed and tracking_weight are KV_STR so an operator can type
// "0.6" without the schema rounding it to an integer.
static double
reachy_kv_double(const reachy_state_t *st, const char *suffix)
{
  char buf[32];

  reachy_kv_copy(st, suffix, buf, sizeof(buf));

  return(strtod(buf, NULL));
}

// ----------------------------------------------------------------------
// The ear: bridge long-poll → whisper → attention → method_deliver
// ----------------------------------------------------------------------

static bool
reachy_bridge_url(char *out, size_t cap)
{
  const char *url = kv_get_str(REACHY_KV_BRIDGE_URL);
  size_t      n;

  if(url == NULL || url[0] == '\0')
  {
    clam(CLAM_WARN, REACHY_CTX, REACHY_KV_BRIDGE_URL " is unset");
    return(FAIL);
  }

  n = (size_t)snprintf(out, cap, "%s", url);

  if(n >= cap)
    return(FAIL);

  while(n > 0 && out[n - 1] == '/')
    out[--n] = '\0';

  return(n > 0 ? SUCCESS : FAIL);
}

// Curl worker thread. Copies out everything the ears thread will need,
// because none of it survives this callback.
static void
reachy_utt_done(const curl_response_t *resp)
{
  reachy_wait_t      *p = resp->user_data;
  const char         *hdr;
  unsigned long long  seq;

  pthread_mutex_lock(&p->mutex);

  p->http      = resp->status;
  p->transport = (resp->status == 0);

  if(resp->status == 200)
  {
    if(resp->body_len > 0 && resp->body_len <= REACHY_WAV_MAX)
    {
      p->wav = mem_alloc(REACHY_CTX, "utt", resp->body_len);
      memcpy(p->wav, resp->body, resp->body_len);
      p->wav_len = resp->body_len;
    }

    else
      clam(CLAM_WARN, REACHY_CTX,
          "the ear answered 200 with a %zu-byte body — not an utterance",
          resp->body_len);
  }

  hdr = curl_response_header(resp, "X-Utt-Seq");

  if(hdr != NULL && sscanf(hdr, "%llu", &seq) == 1)
  {
    p->seq      = (uint64_t)seq;
    p->have_seq = true;
  }

  hdr = curl_response_header(resp, "X-Utt-DoA");

  if(hdr != NULL)
    p->doa = strtod(hdr, NULL);

  p->done = true;
  pthread_cond_signal(&p->cond);
  pthread_mutex_unlock(&p->mutex);

  reachy_wait_unref(p);
}

// Curl worker thread. The bridge's counters, of which exactly one
// matters: where "now" is.
static void
reachy_health_done(const curl_response_t *resp)
{
  reachy_wait_t      *p = resp->user_data;
  const char         *seq;
  unsigned long long  v;

  pthread_mutex_lock(&p->mutex);

  p->http      = resp->status;
  p->transport = (resp->status == 0);

  // {"ok":true,"seq":707,…} — read without a JSON parser because this
  // is the only response body this driver will ever look inside.
  if(resp->status == 200 && resp->body != NULL
      && (seq = strstr(resp->body, "\"seq\"")) != NULL
      && sscanf(seq, "\"seq\"%*[: ]%llu", &v) == 1)
  {
    p->seq      = (uint64_t)v;
    p->have_seq = true;
  }

  p->done = true;
  pthread_cond_signal(&p->cond);
  pthread_mutex_unlock(&p->mutex);

  reachy_wait_unref(p);
}

// Find out where the ear currently is before listening to it.
//
// earbridge keeps a ring of eight utterances and drops the oldest, so a
// cursor starting at zero is handed whatever is still in that ring —
// speech from before this bot existed, which in a quiet room can be
// hours old. A creature that wakes up answering a conversation it never
// attended is not charming, it is broken.
static bool
reachy_prime_seq(reachy_state_t *st, const char *base)
{
  reachy_wait_t  *p;
  curl_request_t *cr;
  char            url[REACHY_URL_SZ];
  bool            ok = false;

  snprintf(url, sizeof(url), "%s/health", base);

  p  = reachy_wait_create();
  cr = curl_request_create(CURL_METHOD_GET, url, reachy_health_done, p);

  curl_request_set_timeout(cr, REACHY_POLL_TIMEOUT);

  if(curl_request_submit(cr) != SUCCESS)
  {
    reachy_wait_unref(p);
    reachy_wait_unref(p);
    return(FAIL);
  }

  if(reachy_wait_for(st, p, REACHY_WAIT_MS))
  {
    pthread_mutex_lock(&p->mutex);

    if(p->http == 200 && p->have_seq)
    {
      st->seq = p->seq;
      ok      = true;
    }

    pthread_mutex_unlock(&p->mutex);
  }

  reachy_wait_unref(p);

  if(ok)
    clam(CLAM_INFO, REACHY_CTX,
        "%s: the ear is at utterance %llu — listening from there",
        st->botname, (unsigned long long)st->seq);

  return(ok ? SUCCESS : FAIL);
}

// The state ladder. connect() leaves the instance RUNNING and the ear
// promotes it on first contact, because method_send refuses anything
// short of AVAILABLE — so a bot whose bridge is down cannot half-hold a
// conversation it has no way to finish.
static void
reachy_bridge_up(reachy_state_t *st)
{
  st->fails = 0;

  if(st->available || st->inst == NULL || reachy_stopping(st))
    return;

  method_set_state(st->inst, METHOD_AVAILABLE);
  st->available = true;

  clam(CLAM_INFO, REACHY_CTX, "%s: the ear answers — available",
      st->botname);
}

static void
reachy_bridge_down(reachy_state_t *st)
{
  st->fails++;

  if(st->fails < REACHY_FAIL_DEMOTE || !st->available)
    return;

  if(st->inst != NULL && !reachy_stopping(st))
    method_set_state(st->inst, METHOD_RUNNING);

  st->available = false;

  clam(CLAM_WARN, REACHY_CTX,
      "%s: the ear has gone quiet after %u tries — no longer available",
      st->botname, st->fails);
}

// Case-insensitive whole-word search, so a bot called "mini" is not
// addressed by the word "minimum".
// returns: the match inside `hay`, or NULL. The position is what lets an
// alias be rewritten in place; a bool would not be enough.
static char *
reachy_word_in(char *hay, const char *needle)
{
  size_t nlen = strlen(needle);

  if(nlen == 0)
    return(NULL);

  for(char *p = hay; *p != '\0'; p++)
  {
    if(strncasecmp(p, needle, nlen) != 0)
      continue;

    if(p != hay && (isalnum((unsigned char)p[-1]) || p[-1] == '_'))
      continue;

    if(p[nlen] != '\0' && (isalnum((unsigned char)p[nlen]) || p[nlen] == '_'))
      continue;

    return(p);
  }

  return(NULL);
}

// Put the bot's own name where the alias was heard.
//
// Without this, `attention.names` is only half a feature. It decides
// what the DRIVER listens to, but the chat plugin runs its own address
// classifier against `method_get_self()` — which knows one name — so an
// utterance admitted by an alias arrives looking like somebody else's
// conversation, is filed as WITNESS, and (at the default
// `interject_prob` of 0) is answered with silence. Measured live
// 2026-08-04: "Minnie, tell me a story…" passed this gate and the brain
// ignored it.
//
// Rewriting is honest here in a way it would not be elsewhere: the alias
// list exists precisely to say "these sounds are also my name", and
// whisper's spelling of a spoken name is a guess, not testimony.
static void
reachy_alias_rewrite(reachy_state_t *st, char *text, size_t cap, char *at,
    size_t alias_len)
{
  size_t name_len = strlen(st->botname);
  size_t tail_len = strlen(at + alias_len);

  // Refuse rather than truncate: an unanswered request beats a request
  // answered with its last words missing.
  if((size_t)(at - text) + name_len + tail_len + 1 > cap)
    return;

  clam(CLAM_DEBUG, REACHY_CTX, "%s: heard its name as \"%.*s\"",
      st->botname, (int)alias_len, at);

  memmove(at + name_len, at + alias_len, tail_len + 1);
  memcpy(at, st->botname, name_len);
}

static char *
reachy_trim(char *s)
{
  size_t n;

  while(isspace((unsigned char)*s))
    s++;

  n = strlen(s);

  while(n > 0 && isspace((unsigned char)s[n - 1]))
    s[--n] = '\0';

  return(s);
}

// The attention gate. `open` hears the whole room; `name` hears only
// what carries the bot's name, and then keeps listening for a window
// afterwards so a conversation does not need the name in every line.
static bool
reachy_addressed(reachy_state_t *st, char *text, size_t cap)
{
  char    mode [16];
  char    names[KV_STR_SZ];
  char   *save = NULL;
  bool    hit  = false;
  time_t  now  = time(NULL);

  reachy_kv_copy(st, "attention.mode", mode, sizeof(mode));

  if(strcasecmp(mode, "name") != 0)
    return(true);

  hit = (reachy_word_in(text, st->botname) != NULL);

  if(!hit)
  {
    reachy_kv_copy(st, "attention.names", names, sizeof(names));

    for(char *tok = strtok_r(names, ",", &save); tok != NULL && !hit;
        tok = strtok_r(NULL, ",", &save))
    {
      const char *alias = reachy_trim(tok);
      char       *at    = reachy_word_in(text, alias);

      if(at == NULL)
        continue;

      reachy_alias_rewrite(st, text, cap, at, strlen(alias));
      hit = true;
    }
  }

  pthread_mutex_lock(&st->attn_mutex);

  if(hit)
    st->attn_until = now + (time_t)reachy_kv_uint(st, "attention.window_s");
  else
    hit = (now < st->attn_until);

  pthread_mutex_unlock(&st->attn_mutex);

  return(hit);
}

static void
reachy_deliver(reachy_state_t *st, const reachy_dispatch_t *d)
{
  method_msg_t msg;
  char         channel[METHOD_CHANNEL_SZ];

  // Sized to the NARROWER of the two fields it fills: sender takes 128,
  // nickname only 64, and the routing key and the display label must
  // not be allowed to disagree by truncation.
  char         speaker[METHOD_NICKNAME_SZ];

  // The window between this check and method_deliver is the residual
  // race the driver design documents and accepts: a `/bot stop` landing
  // inside it walks a freed instance. It is a few instructions against
  // an operator-timescale action, and the only way to close it is a
  // join under method_mutex — which is the deadlock.
  if(reachy_stopping(st) || st->inst == NULL)
    return;

  reachy_kv_copy(st, "speaker", speaker, sizeof(speaker));
  reachy_kv_copy(st, "channel", channel, sizeof(channel));

  if(speaker[0] == '\0')
    snprintf(speaker, sizeof(speaker), "%s", REACHY_ANON_SPEAKER);

  // Zeroed first: that is what makes this a METHOD_MSG_MESSAGE, which
  // the chat plugin's observe path requires.
  memset(&msg, 0, sizeof(msg));

  snprintf(msg.sender,   sizeof(msg.sender),   "%s", speaker);
  snprintf(msg.nickname, sizeof(msg.nickname), "%s", speaker);
  snprintf(msg.channel,  sizeof(msg.channel),  "%s", channel);
  snprintf(msg.text,     sizeof(msg.text),     "%s", d->text);
  snprintf(msg.metadata, sizeof(msg.metadata), "doa=%.2f", d->doa);

  clam(CLAM_INFO, REACHY_CTX, "%s: heard \"%.200s\"", st->botname, d->text);

  method_deliver(st->inst, &msg);
}

// Task worker. The whole chat observe path runs synchronously off
// method_deliver, which is far too much work for a curl completion —
// so the transcript changes threads here.
static void
reachy_dispatch_task(task_t *t)
{
  reachy_dispatch_t *d = t->data;

  t->state = TASK_ENDED;

  if(reachy_addressed(d->st, d->text, sizeof(d->text)))
    reachy_deliver(d->st, d);
  else
    clam(CLAM_DEBUG, REACHY_CTX, "%s: unaddressed, dropped \"%.120s\"",
        d->st->botname, d->text);

  reachy_state_unref(d->st);
  mem_free(d);
}

// Curl worker thread, through the inference engine. The transcript
// arrives already whitespace-collapsed and trimmed, so what lands in
// method_msg_t.text is one line no matter how whisper punctuated it.
static void
reachy_stt_done(const llm_stt_response_t *resp)
{
  reachy_stt_ctx_t  *s  = resp->user_data;
  reachy_state_t    *st = s->st;
  reachy_dispatch_t *d;

  if(!resp->ok || resp->text == NULL || resp->text[0] == '\0')
  {
    if(!resp->ok)
      clam(CLAM_WARN, REACHY_CTX, "%s: transcription failed — %.200s",
          st->botname, resp->error != NULL && resp->error[0] != '\0'
              ? resp->error : "the speech host answered with no transcript");

    else
      clam(CLAM_DEBUG, REACHY_CTX, "%s: silence transcribed to nothing",
          st->botname);

    mem_free(s);
    reachy_state_unref(st);
    return;
  }

  d = mem_alloc(REACHY_CTX, "dispatch", sizeof(*d));

  memset(d, 0, sizeof(*d));

  // The reference moves across rather than being taken again: this
  // closure is done with it the moment the task owns the transcript.
  d->st  = st;
  d->doa = s->doa;
  snprintf(d->text, sizeof(d->text), "%s", resp->text);

  mem_free(s);

  if(task_add("reachy_dispatch", TASK_THREAD, 50, reachy_dispatch_task, d)
      == NULL)
  {
    clam(CLAM_WARN, REACHY_CTX, "%s: no worker took the transcript",
        st->botname);
    mem_free(d);
    reachy_state_unref(st);
  }
}

// Hand the utterance to whisper and return to listening immediately —
// transcription overlaps the next long poll on purpose, because a
// second speaker does not wait for the first to be understood.
static void
reachy_transcribe(reachy_state_t *st, void *wav, size_t wav_len, double doa)
{
  reachy_stt_ctx_t *s;
  char              model[64];

  reachy_kv_copy(st, "stt_model", model, sizeof(model));

  if(model[0] == '\0')
  {
    clam(CLAM_WARN, REACHY_CTX, "%s: no stt_model configured", st->botname);
    return;
  }

  s = mem_alloc(REACHY_CTX, "stt", sizeof(*s));

  s->st  = st;
  s->doa = doa;
  reachy_state_ref(st);

  // The engine copies the WAV into its request body before returning,
  // so the caller's buffer is never shared with the transfer.
  if(llm_stt_submit(model, wav, wav_len, reachy_stt_done, s) != SUCCESS)
  {
    clam(CLAM_WARN, REACHY_CTX,
        "%s: the speech host would not take the utterance", st->botname);
    mem_free(s);
    reachy_state_unref(st);
  }
}

// The ears thread. One long poll at a time — earbridge serves a single
// request at a time by design, so a second concurrent poll would only
// queue behind this one.
static void
reachy_ears(task_t *t)
{
  reachy_state_t *st = t->data;
  char            base[REACHY_BASE_SZ];
  char            safe[REACHY_URL_SZ];
  bool            primed = false;

  reachy_thread_track(t->id);

  clam(CLAM_INFO, REACHY_CTX, "%s: listening", st->botname);

  while(!reachy_stopping(st))
  {
    reachy_wait_t  *p;
    curl_request_t *cr;
    char            url[REACHY_URL_SZ];
    void           *wav     = NULL;
    size_t          wav_len = 0;
    double          doa     = 0.0;
    long            http;

    if(reachy_bridge_url(base, sizeof(base)) != SUCCESS)
    {
      if(!reachy_nap(st, REACHY_BACKOFF_MS))
        break;

      continue;
    }

    // Until the cursor is anchored there is nothing sensible to ask
    // for, so a bridge that will not answer /health simply keeps us
    // here — which is where a bot with no working ear belongs.
    if(!primed)
    {
      if(reachy_prime_seq(st, base) != SUCCESS)
      {
        reachy_bridge_down(st);

        if(!reachy_nap(st, REACHY_BACKOFF_MS))
          break;

        continue;
      }

      primed = true;
    }

    snprintf(url, sizeof(url), "%s/utt?after=%llu&wait=%u", base,
        (unsigned long long)st->seq, (unsigned)REACHY_POLL_WAIT_MS);

    p  = reachy_wait_create();
    cr = curl_request_create(CURL_METHOD_GET, url, reachy_utt_done, p);

    // The seq is the poll cursor and cannot be inferred: earbridge's
    // slot ring drops the oldest, so counting utterances ourselves
    // would desync the moment one is missed.
    curl_request_capture_header(cr, "X-Utt-Seq");
    curl_request_capture_header(cr, "X-Utt-DoA");
    curl_request_set_timeout(cr, REACHY_POLL_TIMEOUT);

    if(curl_request_submit(cr) != SUCCESS)
    {
      // A refused submit releases the request itself and the completion
      // will never fire, so both references are ours to drop.
      reachy_wait_unref(p);
      reachy_wait_unref(p);
      reachy_bridge_down(st);

      if(!reachy_nap(st, REACHY_BACKOFF_MS))
        break;

      continue;
    }

    if(!reachy_wait_for(st, p, REACHY_WAIT_MS))
    {
      // Abandoned rather than answered: the transfer still holds our
      // completion, and the last reference goes with it.
      reachy_wait_unref(p);

      if(reachy_stopping(st))
        break;

      clam(CLAM_WARN, REACHY_CTX, "%s: the ear did not answer in %u ms",
          st->botname, (unsigned)REACHY_WAIT_MS);
      reachy_bridge_down(st);

      if(!reachy_nap(st, REACHY_BACKOFF_MS))
        break;

      continue;
    }

    pthread_mutex_lock(&p->mutex);

    http = p->http;

    if(p->have_seq)
      st->seq = p->seq;

    wav     = p->wav;
    wav_len = p->wav_len;
    doa     = p->doa;
    p->wav  = NULL;   // taken

    pthread_mutex_unlock(&p->mutex);
    reachy_wait_unref(p);

    // 204 is the quiet room, and it is by far the most common answer.
    if(http == 200 || http == 204)
      reachy_bridge_up(st);

    else
    {
      util_redact_url(base, safe, sizeof(safe));
      clam(CLAM_WARN, REACHY_CTX, "%s: %s answered http %ld", st->botname,
          safe, http);
      reachy_bridge_down(st);

      if(!reachy_nap(st, REACHY_BACKOFF_MS))
        break;

      continue;
    }

    if(wav == NULL)
      continue;

    if(!reachy_stopping(st))
      reachy_transcribe(st, wav, wav_len, doa);

    mem_free(wav);
  }

  clam(CLAM_INFO, REACHY_CTX, "%s: stopped listening", st->botname);

  reachy_thread_forget(t->id);
  reachy_state_unref(st);

  t->state = TASK_ENDED;
}

// ----------------------------------------------------------------------
// The mouth: a queued line → synthesis → the robot → paced by its own
// duration
// ----------------------------------------------------------------------

// Called from the curl worker (one call per streamed reply line) and
// from task workers, so it does nothing slow: a copy under the mutex and
// a signal.
//
// Overflow drops the OLDEST rather than refusing the newest. A speaker
// that is a minute behind the room is not participating in the
// conversation, it is reciting at it.
static void
reachy_mouth_push(reachy_state_t *st, const char *text)
{
  uint64_t dropped = 0;
  uint32_t slot;

  pthread_mutex_lock(&st->mouth_mutex);

  if(st->mouth_count == REACHY_SPEECH_RING)
  {
    st->mouth_head = (st->mouth_head + 1u) % REACHY_SPEECH_RING;
    st->mouth_count--;
    dropped = ++st->mouth_dropped;
  }

  slot = (st->mouth_head + st->mouth_count) % REACHY_SPEECH_RING;
  snprintf(st->mouth_line[slot], METHOD_TEXT_SZ, "%s", text);
  st->mouth_count++;

  pthread_cond_signal(&st->mouth_cond);
  pthread_mutex_unlock(&st->mouth_mutex);

  if(dropped != 0)
    clam(CLAM_WARN, REACHY_CTX,
        "%s: the mouth is %u lines behind — dropped the oldest (%llu so "
        "far)", st->botname, (uint32_t)REACHY_SPEECH_RING,
        (unsigned long long)dropped);
}

// Block until there is a line to speak or the instance is torn down.
// returns: true when `out` holds a line.
static bool
reachy_mouth_take(reachy_state_t *st, char *out, size_t cap)
{
  bool got = false;

  pthread_mutex_lock(&st->mouth_mutex);

  while(st->mouth_count == 0 && !reachy_stopping(st))
  {
    struct timespec slice;

    reachy_deadline(&slice, REACHY_WAIT_SLICE_MS);
    pthread_cond_timedwait(&st->mouth_cond, &st->mouth_mutex, &slice);
  }

  if(st->mouth_count > 0)
  {
    snprintf(out, cap, "%s", st->mouth_line[st->mouth_head]);
    st->mouth_head = (st->mouth_head + 1u) % REACHY_SPEECH_RING;
    st->mouth_count--;
    got = true;
  }

  pthread_mutex_unlock(&st->mouth_mutex);

  return(got);
}

// Everything still queued is abandoned. Only barge-in calls this: once a
// human has the floor, the rest of an answer they interrupted is stale.
static void
reachy_mouth_flush(reachy_state_t *st)
{
  uint32_t n;

  pthread_mutex_lock(&st->mouth_mutex);

  n              = st->mouth_count;
  st->mouth_count = 0;

  pthread_mutex_unlock(&st->mouth_mutex);

  if(n > 0)
    clam(CLAM_INFO, REACHY_CTX,
        "%s: interrupted — abandoned %u queued line(s)", st->botname, n);
}

static void
reachy_mouth_wake(reachy_state_t *st)
{
  pthread_mutex_lock(&st->mouth_mutex);
  pthread_cond_broadcast(&st->mouth_cond);
  pthread_mutex_unlock(&st->mouth_mutex);
}

// Curl worker thread, through the inference engine. The audio is valid
// for this callback only, so it is copied out before the waiter is told
// anything landed.
static void
reachy_tts_done(const llm_tts_response_t *resp)
{
  reachy_wait_t *w = resp->user_data;

  pthread_mutex_lock(&w->mutex);

  w->http = resp->http_status;
  w->ok   = (resp->ok && resp->bytes != NULL && resp->bytes_len > 0);

  if(w->ok)
  {
    w->wav = mem_alloc(REACHY_CTX, "speech", resp->bytes_len);
    memcpy(w->wav, resp->bytes, resp->bytes_len);
    w->wav_len = resp->bytes_len;
  }

  w->done = true;
  pthread_cond_signal(&w->cond);
  pthread_mutex_unlock(&w->mutex);

  reachy_wait_unref(w);
}

// Curl worker thread. The robot's fire-and-forget verbs all answer in
// this one shape, so upload and playback share a completion.
static void
reachy_robot_done(const reachy_result_t *r)
{
  reachy_wait_t *w = r->user_data;

  pthread_mutex_lock(&w->mutex);

  w->http = r->http;
  w->ok   = (r->status == REACHY_OK);
  w->done = true;

  pthread_cond_signal(&w->cond);
  pthread_mutex_unlock(&w->mutex);

  reachy_wait_unref(w);
}

// Curl worker thread.
static void
reachy_doa_done(const reachy_doa_t *r)
{
  reachy_wait_t *w = r->user_data;

  pthread_mutex_lock(&w->mutex);

  w->ok     = (r->status == REACHY_OK);
  w->doa    = r->angle;
  w->speech = r->speech;
  w->done   = true;

  pthread_cond_signal(&w->cond);
  pthread_mutex_unlock(&w->mutex);

  reachy_wait_unref(w);
}

// One fixed name per bot, overwritten every line. The robot's sound
// directory is a tmpfs on a 4-core CM4 with a few gigabytes to its name;
// a unique filename per utterance would fill it in an afternoon of
// conversation and nothing would ever delete them.
static void
reachy_sound_file(const reachy_state_t *st, char *out, size_t cap)
{
  size_t n;

  n = (size_t)snprintf(out, cap, "reachy_say_%s.wav", st->botname);

  if(n >= cap)
    n = cap - 1;

  // The daemon's upload endpoint refuses anything outside this alphabet,
  // and it refuses it as a flat FAIL with no clue which character did it.
  for(size_t i = 0; i < n; i++)
  {
    char c = out[i];

    if(!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
      out[i] = '_';
  }
}

// returns: SUCCESS with `*wav` owned by the caller, FAIL with nothing
// allocated.
static bool
reachy_synthesize(reachy_state_t *st, const char *text, void **wav,
    size_t *wav_len)
{
  llm_tts_params_t  params;
  reachy_wait_t    *w;
  char              model[64];
  char              voice[64];
  bool              ok = false;

  *wav     = NULL;
  *wav_len = 0;

  reachy_kv_copy(st, "tts_model", model, sizeof(model));
  reachy_kv_copy(st, "tts_voice", voice, sizeof(voice));

  if(model[0] == '\0')
  {
    clam(CLAM_WARN, REACHY_CTX, "%s: no tts_model configured", st->botname);
    return(FAIL);
  }

  memset(&params, 0, sizeof(params));

  params.voice        = voice[0] != '\0' ? voice : NULL;
  params.speed        = reachy_kv_double(st, "tts_speed");
  params.timeout_secs = REACHY_TTS_TIMEOUT;

  w = reachy_wait_create();

  if(llm_tts_submit(model, &params, text, reachy_tts_done, w) != SUCCESS)
  {
    // Refused before it left: the completion will never fire, so both
    // references are ours to drop.
    reachy_wait_unref(w);
    reachy_wait_unref(w);
    clam(CLAM_WARN, REACHY_CTX,
        "%s: the speech host would not take the line", st->botname);
    return(FAIL);
  }

  if(!reachy_wait_for(st, w, REACHY_TTS_WAIT_MS))
  {
    // Abandoned rather than answered: the transfer still holds our
    // completion, and the last reference goes with it.
    reachy_wait_unref(w);

    if(!reachy_stopping(st))
      clam(CLAM_WARN, REACHY_CTX, "%s: synthesis did not answer in %u ms",
          st->botname, (unsigned)REACHY_TTS_WAIT_MS);

    return(FAIL);
  }

  pthread_mutex_lock(&w->mutex);

  if(w->ok)
  {
    *wav     = w->wav;
    *wav_len = w->wav_len;
    w->wav   = NULL;   // taken
    ok       = true;
  }

  pthread_mutex_unlock(&w->mutex);
  reachy_wait_unref(w);

  if(!ok)
    clam(CLAM_WARN, REACHY_CTX, "%s: the speech host found no voice",
        st->botname);

  return(ok ? SUCCESS : FAIL);
}

// The WAV is copied into the multipart body before the submit returns,
// so the caller keeps ownership of `wav` throughout.
static bool
reachy_upload(reachy_state_t *st, const char *file, const void *wav,
    size_t wav_len)
{
  reachy_wait_t *w = reachy_wait_create();
  bool           ok;

  if(reachy_upload_sound(file, wav, wav_len, reachy_robot_done, w)
      != SUCCESS)
  {
    reachy_wait_unref(w);
    reachy_wait_unref(w);
    clam(CLAM_WARN, REACHY_CTX,
        "%s: the robot was not even asked to take the audio", st->botname);
    return(FAIL);
  }

  if(!reachy_wait_for(st, w, REACHY_ROBOT_WAIT_MS))
  {
    reachy_wait_unref(w);

    if(!reachy_stopping(st))
      clam(CLAM_WARN, REACHY_CTX, "%s: the robot did not take the audio "
          "within %u ms", st->botname, (unsigned)REACHY_ROBOT_WAIT_MS);

    return(FAIL);
  }

  pthread_mutex_lock(&w->mutex);
  ok = w->ok;
  pthread_mutex_unlock(&w->mutex);

  reachy_wait_unref(w);

  if(!ok)
    clam(CLAM_WARN, REACHY_CTX, "%s: the robot refused the audio",
        st->botname);

  return(ok ? SUCCESS : FAIL);
}

static bool
reachy_play(reachy_state_t *st, const char *file)
{
  reachy_wait_t *w = reachy_wait_create();
  bool           ok;

  if(reachy_play_sound(file, reachy_robot_done, w) != SUCCESS)
  {
    reachy_wait_unref(w);
    reachy_wait_unref(w);
    clam(CLAM_WARN, REACHY_CTX,
        "%s: the audio is on the robot but playing it was refused before "
        "it left", st->botname);
    return(FAIL);
  }

  if(!reachy_wait_for(st, w, REACHY_ROBOT_WAIT_MS))
  {
    reachy_wait_unref(w);

    if(!reachy_stopping(st))
      clam(CLAM_WARN, REACHY_CTX,
          "%s: the robot did not start playing within %u ms", st->botname,
          (unsigned)REACHY_ROBOT_WAIT_MS);

    return(FAIL);
  }

  pthread_mutex_lock(&w->mutex);
  ok = w->ok;
  pthread_mutex_unlock(&w->mutex);

  reachy_wait_unref(w);

  if(!ok)
    clam(CLAM_WARN, REACHY_CTX, "%s: the robot would not play the audio",
        st->botname);

  return(ok ? SUCCESS : FAIL);
}

// How long this audio takes to play, in milliseconds.
//
// The daemon's play_sound returns the moment playback STARTS, so the
// only thing that keeps two lines from landing on top of each other is
// this arithmetic. It is read from the file's own header rather than
// assumed: kokorod emits 24 kHz stereo S16LE today (96,000 bytes per
// second), and a server that one day emits something else should slow
// the robot down, not desynchronize it.
static uint32_t
reachy_play_ms(const void *wav, size_t wav_len)
{
  const uint8_t *b    = wav;
  uint32_t       rate = 0;
  uint64_t       ms;

  if(wav_len <= REACHY_WAV_HDR_SZ)
    return(0);

  // Canonical 44-byte RIFF: byte rate is the little-endian word at 28.
  if(memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WAVE", 4) == 0)
    rate = (uint32_t)b[28] | ((uint32_t)b[29] << 8)
         | ((uint32_t)b[30] << 16) | ((uint32_t)b[31] << 24);

  if(rate == 0)
    rate = REACHY_WAV_BYTE_RATE;

  ms = ((uint64_t)(wav_len - REACHY_WAV_HDR_SZ) * 1000u) / rate;

  return(ms > REACHY_PLAY_MAX_MS ? (uint32_t)REACHY_PLAY_MAX_MS
                                 : (uint32_t)ms);
}

// Has a human taken the floor?
//
// ⚠ The flag this reads is not trustworthy on this hardware, which is
// why `barge_in` ships OFF. Measured 2026-08-04 over 451 probes of a
// SILENT room: `speech_detected` was true in 49% of them, in 93 separate
// runs, 28 of which were 3 or more probes long and 11 of which were 5 or
// more. The cause is the one §HALLUCINATION found — the CM4's cooling
// fan sits above the XVF3800's speech threshold — and no run length
// separates it from a person. The array's echo canceller is meanwhile
// doing its job: during 19 s of the robot's OWN speech the flag was true
// LESS often than in silence, so self-interruption was never the risk.
//
// REACHY_BARGE_RUN is therefore a guard for the day the fan is quieter,
// not a fix for today. Leave the knob off until `reachy_mic_probe.sh`
// says the floor has moved.
static bool
reachy_barged(reachy_state_t *st)
{
  reachy_wait_t *w = reachy_wait_create();
  bool           speech = false;

  if(reachy_get_doa(reachy_doa_done, w) != SUCCESS)
  {
    reachy_wait_unref(w);
    reachy_wait_unref(w);
    return(false);
  }

  if(!reachy_wait_for(st, w, REACHY_DOA_WAIT_MS))
  {
    reachy_wait_unref(w);
    return(false);
  }

  pthread_mutex_lock(&w->mutex);
  speech = (w->ok && w->speech);
  pthread_mutex_unlock(&w->mutex);

  reachy_wait_unref(w);

  return(speech);
}

// Hold the mouth for the length of the audio, watching for an
// interruption if the operator has asked for one.
// returns: true when playback ran its course, false when it was cut
// short by a human or a teardown.
static bool
reachy_pace(reachy_state_t *st, uint32_t ms, bool barge_in)
{
  struct timespec deadline;
  uint32_t        run = 0;

  reachy_deadline(&deadline, ms + REACHY_PLAY_GRACE_MS);

  while(!reachy_expired(&deadline))
  {
    if(!reachy_nap(st, REACHY_PLAY_SLICE_MS))
      return(false);

    if(!barge_in)
      continue;

    if(!reachy_barged(st))
    {
      run = 0;
      continue;
    }

    if(++run < REACHY_BARGE_RUN)
      continue;

    clam(CLAM_INFO, REACHY_CTX, "%s: a human has the floor — stopping",
        st->botname);

    reachy_stop_sound(NULL, NULL);
    reachy_mouth_flush(st);

    return(false);
  }

  return(true);
}

// Milliseconds since `since`, which is then advanced to now — so three
// hops in a row are each measured without three pairs of timestamps.
static uint32_t
reachy_lap(struct timespec *since)
{
  struct timespec now;
  int64_t         ms;

  clock_gettime(CLOCK_MONOTONIC, &now);

  // Signed throughout: the nanosecond term is negative whenever the lap
  // crossed a second boundary, and the seconds term is what pays for it.
  ms = (int64_t)(now.tv_sec - since->tv_sec) * 1000
     + (int64_t)(now.tv_nsec - since->tv_nsec) / 1000000;

  *since = now;

  return(ms > 0 ? (uint32_t)ms : 0u);
}

// One line, end to end. Every failure arm drops this line and returns:
// the mouth must never wedge on a phrase it cannot say.
//
// The three hop timings are logged together because that is how they are
// read — "the robot was slow to answer" is a question about which of
// synthesis, the WiFi upload or the daemon took the time, and one line
// carrying all three answers it without a stopwatch.
static void
reachy_speak(reachy_state_t *st, const char *text)
{
  struct timespec lap;
  char            file[REACHY_FILE_SZ];
  void           *wav     = NULL;
  size_t          wav_len = 0;
  uint32_t        tts_ms;
  uint32_t        up_ms;
  uint32_t        play_ms;
  uint32_t        ms;

  clock_gettime(CLOCK_MONOTONIC, &lap);

  if(reachy_synthesize(st, text, &wav, &wav_len) != SUCCESS)
    return;

  tts_ms = reachy_lap(&lap);

  reachy_sound_file(st, file, sizeof(file));

  ms = reachy_play_ms(wav, wav_len);

  if(reachy_upload(st, file, wav, wav_len) != SUCCESS)
  {
    mem_free(wav);
    return;
  }

  up_ms = reachy_lap(&lap);

  // The robot has its own copy now, and the pacing length is already
  // measured — nothing below needs these bytes.
  mem_free(wav);

  if(reachy_play(st, file) != SUCCESS)
    return;

  play_ms = reachy_lap(&lap);

  clam(CLAM_INFO, REACHY_CTX,
      "%s: says \"%.160s\" (%u.%03u s of audio; tts %u ms, upload %u ms, "
      "play %u ms)", st->botname, text, ms / 1000u, ms % 1000u, tts_ms,
      up_ms, play_ms);

  reachy_pace(st, ms, reachy_kv_flag(st, "barge_in"));
}

// The mouth thread. Strictly one line at a time: the robot has one
// speaker, and the whole point of pacing is that the next line waits for
// this one to finish being heard.
static void
reachy_mouth(task_t *t)
{
  reachy_state_t *st = t->data;
  char            line[METHOD_TEXT_SZ];

  reachy_thread_track(t->id);

  clam(CLAM_INFO, REACHY_CTX, "%s: the mouth is open", st->botname);

  while(!reachy_stopping(st))
    if(reachy_mouth_take(st, line, sizeof(line)))
      reachy_speak(st, line);

  clam(CLAM_INFO, REACHY_CTX, "%s: the mouth is closed", st->botname);

  reachy_thread_forget(t->id);
  reachy_state_unref(st);

  t->state = TASK_ENDED;
}

// ----------------------------------------------------------------------
// Method driver vtable
// ----------------------------------------------------------------------

// Runs UNDER the non-recursive method_mutex, before the instance is even
// in the registry — so it is pure allocation and string derivation.
// Any method_* call from here deadlocks.
static void *
reachy_create(const char *inst_name)
{
  reachy_state_t *st     = mem_alloc(REACHY_CTX, "state", sizeof(*st));
  const char     *suffix = "_reachy";
  size_t          nlen   = strlen(inst_name);
  size_t          slen   = strlen(suffix);

  memset(st, 0, sizeof(*st));

  snprintf(st->inst_name, sizeof(st->inst_name), "%s", inst_name);

  // The instance name is always recomputed as "<bot>_<kind>", so the
  // suffix strip is how the bot name comes back — and the KV prefix
  // core cloned the instance schema under depends on getting it right.
  if(nlen > slen && strcmp(inst_name + nlen - slen, suffix) == 0)
    snprintf(st->botname, sizeof(st->botname), "%.*s", (int)(nlen - slen),
        inst_name);

  else
    snprintf(st->botname, sizeof(st->botname), "%s", inst_name);

  snprintf(st->kv_prefix, sizeof(st->kv_prefix), "bot.%s.reachy.",
      st->botname);

  // One reference for the registry; threads take their own.
  st->refs = 1;

  pthread_mutex_init(&st->wake_mutex, NULL);
  pthread_mutex_init(&st->attn_mutex, NULL);
  pthread_mutex_init(&st->mouth_mutex, NULL);
  pthread_cond_init(&st->wake_cond, NULL);
  pthread_cond_init(&st->mouth_cond, NULL);

  return(st);
}

static void
reachy_destroy(void *handle)
{
  reachy_state_unref(handle);
}

// ----------------------------------------------------------------------
// Posture at connect — torque, then the move, and only if it is needed
// ----------------------------------------------------------------------

// The robot's daemon is asymmetric about torque, measured 2026-08-04:
// goto_sleep drops the motors when its move ends, but wake_up never
// raises them. A limp robot accepts wake_up, returns a uuid, "completes"
// and stirs not at all, with no error on any surface — which is exactly
// how a reload used to leave it lying in its shell.
//
// So the driver asks before it lifts. A robot already on its feet is
// left standing exactly as the room last saw it: a reload is an event in
// the daemon, not in the living room, and a creature that hauls itself
// upright every time its code is rebuilt is a poltergeist.
static void
reachy_posture_woke(const reachy_result_t *r)
{
  reachy_posture_t *p = r->user_data;

  if(r->status == REACHY_OK)
    clam(CLAM_INFO, REACHY_CTX, "%s: robot woken — motors on, rising",
        p->botname);

  else
    clam(CLAM_WARN, REACHY_CTX,
        "%s: motors are on but the wake move failed — %s", p->botname,
        reachy_status_str(r->status));

  mem_free(p);
}

static void
reachy_posture_torque(const reachy_result_t *r)
{
  reachy_posture_t *p = r->user_data;

  if(r->status != REACHY_OK)
  {
    clam(CLAM_WARN, REACHY_CTX,
        "%s: could not enable the robot's motors — %s; it will hear the "
        "room but cannot move in it", p->botname,
        reachy_status_str(r->status));
    mem_free(p);
    return;
  }

  if(reachy_wake(reachy_posture_woke, p) != SUCCESS)
  {
    clam(CLAM_WARN, REACHY_CTX,
        "%s: the wake move was refused before it left", p->botname);
    mem_free(p);
  }
}

static void
reachy_posture_probe(const reachy_robot_status_t *r)
{
  reachy_posture_t *p = r->user_data;

  // An unreadable robot is left alone rather than heaved to its feet on
  // a guess — the same posture the volume and tracking calls take.
  if(r->status != REACHY_OK)
  {
    clam(CLAM_WARN, REACHY_CTX,
        "%s: could not read the robot's motor state — %s; leaving its "
        "posture alone", p->botname, reachy_status_str(r->status));
    mem_free(p);
    return;
  }

  if(strcmp(r->motors, "enabled") == 0)
  {
    clam(CLAM_INFO, REACHY_CTX,
        "%s: robot is already up — leaving it as it stands", p->botname);
    mem_free(p);
    return;
  }

  if(reachy_motors_mode("enabled", reachy_posture_torque, p) != SUCCESS)
  {
    clam(CLAM_WARN, REACHY_CTX,
        "%s: the motor enable was refused before it left", p->botname);
    mem_free(p);
  }
}

static void
reachy_posture_wake(const reachy_state_t *st)
{
  reachy_posture_t *p = mem_alloc(REACHY_CTX, "posture", sizeof(*p));

  snprintf(p->botname, sizeof(p->botname), "%s", st->botname);

  if(reachy_get_status(reachy_posture_probe, p) != SUCCESS)
  {
    clam(CLAM_WARN, REACHY_CTX,
        "%s: could not ask the robot whether it is awake — is "
        "plugin.reachyapi.base_url set?", st->botname);
    mem_free(p);
  }
}

// Runs outside every core lock, which is what makes the instance
// lookup and the thread spawn legal here and nowhere else.
static bool
reachy_connect(void *handle)
{
  reachy_state_t *st = handle;
  char            task_name[TASK_NAME_SZ];
  double          weight;
  task_handle_t   h;

  st->inst = method_find(st->inst_name);

  if(st->inst == NULL)
  {
    clam(CLAM_WARN, REACHY_CTX, "connect: method instance '%s' not found",
        st->inst_name);
    return(FAIL);
  }

  __atomic_store_n(&st->shutdown, false, __ATOMIC_RELEASE);

  st->seq       = 0;
  st->fails     = 0;
  st->available = false;

  // Posture, fire-and-forget. Every one of these is idempotent on the
  // daemon's side and none of them is worth failing a connect over: a
  // robot that will not turn its head can still hold a conversation.
  reachy_set_volume((uint8_t)reachy_kv_uint(st, "volume"), NULL, NULL);
  reachy_wobbling(reachy_kv_flag(st, "wobble"), NULL, NULL);

  weight = reachy_kv_double(st, "tracking_weight");

  if(weight > 0.0)
    reachy_tracking(true, weight, NULL, NULL);

  // Standing up is the one thing that is NOT fire-and-forget: it needs
  // torque before the move, and it needs to know whether the robot is
  // already on its feet. See the chain above.
  reachy_posture_wake(st);

  st->mouth_head    = 0;
  st->mouth_count   = 0;
  st->mouth_dropped = 0;

  // Each reference is taken BEFORE its spawn: a thread that starts
  // instantly must never find a state that has already been freed
  // underneath it.
  reachy_state_ref(st);

  // TASK_NAME_SZ is 40 and a bot name may be 63, so both names are
  // clipped here rather than by snprintf — the task name is a label in
  // `show tasks`, and a deliberate clip reads better than a silent one.
  snprintf(task_name, sizeof(task_name), "reachy_ears_%.27s", st->botname);
  h = task_add_persist(task_name, 50, reachy_ears, st);

  if(h == TASK_HANDLE_NONE)
  {
    clam(CLAM_WARN, REACHY_CTX, "%s: could not spawn the ears thread",
        st->botname);
    reachy_state_unref(st);
    return(FAIL);
  }

  reachy_state_ref(st);

  snprintf(task_name, sizeof(task_name), "reachy_mouth_%.26s", st->botname);
  h = task_add_persist(task_name, 50, reachy_mouth, st);

  if(h == TASK_HANDLE_NONE)
  {
    // The ear is already listening. A creature that hears the room and
    // cannot answer it is worse than one that does neither, so the whole
    // connect unwinds — the flag is how the ears thread is told.
    clam(CLAM_WARN, REACHY_CTX, "%s: could not spawn the mouth thread",
        st->botname);
    reachy_state_unref(st);

    __atomic_store_n(&st->shutdown, true, __ATOMIC_RELEASE);

    pthread_mutex_lock(&st->wake_mutex);
    pthread_cond_broadcast(&st->wake_cond);
    pthread_mutex_unlock(&st->wake_mutex);

    return(FAIL);
  }

  // RUNNING, not AVAILABLE: the ear promotes us when the bridge answers.
  method_set_state(st->inst, METHOD_RUNNING);

  return(SUCCESS);
}

// Runs UNDER method_mutex, back to back with destroy(). No joins — the
// ears thread may be blocked taking this very mutex inside
// method_deliver — and no method_* calls. Both threads are told the same
// way, on the two condvars they sleep on.
static void
reachy_disconnect(void *handle)
{
  reachy_state_t *st = handle;

  __atomic_store_n(&st->shutdown, true, __ATOMIC_RELEASE);

  pthread_mutex_lock(&st->wake_mutex);
  pthread_cond_broadcast(&st->wake_cond);
  pthread_mutex_unlock(&st->wake_mutex);

  reachy_mouth_wake(st);

  // Plain curl submits, which is legal under the mutex. Silence first:
  // a robot that tucks itself away mid-sentence and keeps talking from
  // inside its shell is a haunting, not a goodbye.
  reachy_stop_sound(NULL, NULL);

  // A reload is not a goodbye, and putting the robot to bed for one is
  // worse than pointless: goto_sleep takes ~2.5 s and ends by dropping
  // torque, so it lands AFTER the connect that is already racing it and
  // leaves the robot limp in its shell with every call having returned
  // 200. Observed twice on 2026-08-04. reachy_suspend() is how we know.
  if(__atomic_load_n(&reachy_reloading, __ATOMIC_ACQUIRE))
    return;

  if(reachy_kv_flag(st, "sleep_on_disconnect"))
    reachy_sleep_move(NULL, NULL);
}

// Called from the curl worker thread — one call per streamed reply line
// — and from task workers via cmd_reply, so it does the least it can:
// one copy into the ring, one signal. Everything expensive belongs to
// the mouth thread.
//
// `target` is ignored on purpose. It is the channel when there is one
// and the sender otherwise, and here both name the same room, the same
// microphone and the same speaker.
//
// SUCCESS even when the line is dropped: the reply pipeline treats a
// send failure as something to report to a user and retry, and neither
// makes sense for speech that has already been overtaken by the
// conversation.
static bool
reachy_send(void *handle, const char *target, const char *text)
{
  reachy_state_t *st = handle;
  char            spoken[METHOD_TEXT_SZ];

  (void)target;

  if(text == NULL || text[0] == '\0')
    return(SUCCESS);

  // An action ("/me waves") read out verbatim is a bot reading a stage
  // direction. Spoken aloud it wants its subject back.
  if(strncmp(text, "/me ", 4) == 0)
    snprintf(spoken, sizeof(spoken), "%s %s", st->botname, text + 4);

  else
    snprintf(spoken, sizeof(spoken), "%s", text);

  reachy_mouth_push(st, spoken);

  return(SUCCESS);
}

// Every voice in the room arrives through one microphone, so there is
// no per-sender context to distinguish — the constant IS the second
// factor, and it says exactly which body the words came out of.
static bool
reachy_get_context(void *handle, const char *sender, char *ctx,
    size_t ctx_sz)
{
  (void)handle;
  (void)sender;

  snprintf(ctx, ctx_sz, "voice@reachy");

  return(SUCCESS);
}

// Without this the chat plugin's address classifier gets NULL and every
// utterance lands as WITNESS — no natural addressing, no name-mode.
static bool
reachy_get_self(void *handle, char *buf, size_t buf_sz)
{
  reachy_state_t *st = handle;

  if(st->botname[0] == '\0')
    return(FAIL);

  snprintf(buf, buf_sz, "%s", st->botname);

  return(SUCCESS);
}

// The robot is in exactly one room. Yielding it is what gives the
// creature unprompted speech — the volunteer path only considers
// channels a bot reports being in.
static void
reachy_list_joined_channels(void *handle, method_joined_channel_cb_t cb,
    void *data)
{
  reachy_state_t *st = handle;
  char            channel[METHOD_CHANNEL_SZ];

  reachy_kv_copy(st, "channel", channel, sizeof(channel));

  if(channel[0] != '\0')
    cb(channel, data);
}

// Emote, member listing and ejection have no meaning in a room the
// robot perceives as one microphone: all NULL, all NULL-safe in core.
// .colors NULL makes core strip abstract markers for us — a spoken
// reply has no use for colour codes.
static const method_driver_t reachy_driver = {
  .name                 = "reachy",
  .caps                 = 0,
  .colors               = NULL,
  .create               = reachy_create,
  .destroy              = reachy_destroy,
  .connect              = reachy_connect,
  .disconnect           = reachy_disconnect,
  .send                 = reachy_send,
  .send_emote           = NULL,
  .get_context          = reachy_get_context,
  .list_channel         = NULL,
  .list_joined_channels = reachy_list_joined_channels,
  .get_self             = reachy_get_self,
  .eject_probe          = NULL,
  .eject                = NULL,
};

// ----------------------------------------------------------------------
// Plugin lifecycle
// ----------------------------------------------------------------------

static bool
reachy_start(void)
{
  clam(CLAM_INFO, REACHY_CTX,
      "method plugin started (driver available)");

  return(SUCCESS);
}

// Runs under method_mutex — count and nothing else.
static void
reachy_count_bound_cb(const char *subject, const char *field, const void *ptr,
    void *data)
{
  uint32_t *n = data;

  (void)subject;

  if(strcmp(field, "driver") == 0 && ptr == (const void *)&reachy_driver)
    (*n)++;
}

static bool
reachy_stop(void)
{
  task_handle_t pending[REACHY_MAX_THREADS];
  uint32_t      bound = 0;
  uint32_t      n     = 0;

  // A surviving instance means a vtable pointer into our .text. Core's
  // reload path suspends bound bots first, so reaching this refusal
  // means someone unloaded us out from under a live creature.
  method_audit_iterate(reachy_count_bound_cb, &bound);

  if(bound > 0)
  {
    clam(CLAM_WARN, REACHY_CTX,
        "stop refused: %u method instance(s) still bound to the reachy "
        "driver; stop and destroy the bot first", bound);
    return(FAIL);
  }

  // Snapshot, then join outside the registry lock: a thread on its way
  // out takes that lock to withdraw itself, so joining while holding it
  // would deadlock against the very thread being waited on.
  pthread_mutex_lock(&reachy_threads_mutex);

  for(uint32_t i = 0; i < reachy_threads_count; i++)
    pending[n++] = reachy_threads[i];

  pthread_mutex_unlock(&reachy_threads_mutex);

  for(uint32_t i = 0; i < n; i++)
    if(!task_persist_join(pending[i], REACHY_JOIN_MS))
      clam(CLAM_WARN, REACHY_CTX,
          "thread %llu did not exit within %u ms — unloading now would "
          "unmap code it is still running",
          (unsigned long long)pending[i], (unsigned)REACHY_JOIN_MS);

  return(SUCCESS);
}

// Neither hook has any state to put down — the driver holds nothing
// across an unload that core does not already restore. They exist for
// one reason: to mark the window in which a disconnect means "back in a
// moment" rather than "goodbye". See reachy_reloading.
static bool
reachy_suspend(void)
{
  __atomic_store_n(&reachy_reloading, true, __ATOMIC_RELEASE);

  return(SUCCESS);
}

static bool
reachy_resume(void)
{
  __atomic_store_n(&reachy_reloading, false, __ATOMIC_RELEASE);

  return(SUCCESS);
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "reachy",
  .version         = "1.0",
  .type            = PLUGIN_PROTOCOL,
  .kind            = "reachy",

  .provides        = { { .name = "protocol_reachy" } },
  .provides_count  = 1,
  .requires        = { { .name = "service_reachyapi" } },
  .requires_count  = 1,

  .kv_schema            = NULL,
  .kv_schema_count      = 0,
  .kv_inst_schema       = reachy_inst_kv_schema,
  .kv_inst_schema_count = sizeof(reachy_inst_kv_schema)
                          / sizeof(reachy_inst_kv_schema[0]),

  .start           = reachy_start,
  .stop            = reachy_stop,
  .suspend         = reachy_suspend,
  .resume          = reachy_resume,
  .ext             = &reachy_driver,
};
