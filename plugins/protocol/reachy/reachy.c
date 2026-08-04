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

  { "barge_in", KV_UINT8, "1",
    "Stop speaking the moment a human starts (1) or talk over them (0)" },
  { "wobble", KV_UINT8, "1",
    "Drive speech-synced head motion from played audio at connect" },
  { "tracking_weight", KV_STR, "0.6",
    "Face-tracking strength in [0,1] applied at connect; 0 leaves "
    "tracking alone" },
  { "sleep_on_disconnect", KV_UINT8, "1",
    "Play the goto_sleep move when the bot stops" },

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
  pthread_mutex_destroy(&st->wake_mutex);
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
// The long-poll wait context
// ----------------------------------------------------------------------

static reachy_poll_t *
reachy_poll_create(void)
{
  reachy_poll_t *p = mem_alloc(REACHY_CTX, "poll", sizeof(*p));

  memset(p, 0, sizeof(*p));

  pthread_mutex_init(&p->mutex, NULL);
  pthread_cond_init(&p->cond, NULL);

  // The waiter and the completion each hold one.
  p->refs = 2;

  return(p);
}

static void
reachy_poll_unref(reachy_poll_t *p)
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

// Wait for the transfer to complete, in slices so a teardown is felt
// without a second condvar to broadcast on.
// returns: true when the completion actually ran.
static bool
reachy_poll_wait(reachy_state_t *st, reachy_poll_t *p)
{
  struct timespec deadline;
  bool            done;

  reachy_deadline(&deadline, REACHY_WAIT_MS);

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
  reachy_poll_t      *p = resp->user_data;
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

  reachy_poll_unref(p);
}

// Curl worker thread. The bridge's counters, of which exactly one
// matters: where "now" is.
static void
reachy_health_done(const curl_response_t *resp)
{
  reachy_poll_t      *p = resp->user_data;
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

  reachy_poll_unref(p);
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
  reachy_poll_t  *p;
  curl_request_t *cr;
  char            url[REACHY_URL_SZ];
  bool            ok = false;

  snprintf(url, sizeof(url), "%s/health", base);

  p  = reachy_poll_create();
  cr = curl_request_create(CURL_METHOD_GET, url, reachy_health_done, p);

  curl_request_set_timeout(cr, REACHY_POLL_TIMEOUT);

  if(curl_request_submit(cr) != SUCCESS)
  {
    reachy_poll_unref(p);
    reachy_poll_unref(p);
    return(FAIL);
  }

  if(reachy_poll_wait(st, p))
  {
    pthread_mutex_lock(&p->mutex);

    if(p->http == 200 && p->have_seq)
    {
      st->seq = p->seq;
      ok      = true;
    }

    pthread_mutex_unlock(&p->mutex);
  }

  reachy_poll_unref(p);

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
static bool
reachy_word_in(const char *hay, const char *needle)
{
  size_t nlen = strlen(needle);

  if(nlen == 0)
    return(false);

  for(const char *p = hay; *p != '\0'; p++)
  {
    if(strncasecmp(p, needle, nlen) != 0)
      continue;

    if(p != hay && (isalnum((unsigned char)p[-1]) || p[-1] == '_'))
      continue;

    if(p[nlen] != '\0' && (isalnum((unsigned char)p[nlen]) || p[nlen] == '_'))
      continue;

    return(true);
  }

  return(false);
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
reachy_addressed(reachy_state_t *st, const char *text)
{
  char    mode [16];
  char    names[KV_STR_SZ];
  char   *save = NULL;
  bool    hit  = false;
  time_t  now  = time(NULL);

  reachy_kv_copy(st, "attention.mode", mode, sizeof(mode));

  if(strcasecmp(mode, "name") != 0)
    return(true);

  hit = reachy_word_in(text, st->botname);

  if(!hit)
  {
    reachy_kv_copy(st, "attention.names", names, sizeof(names));

    for(char *tok = strtok_r(names, ",", &save); tok != NULL && !hit;
        tok = strtok_r(NULL, ",", &save))
      hit = reachy_word_in(text, reachy_trim(tok));
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

  if(reachy_addressed(d->st, d->text))
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
    reachy_poll_t  *p;
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

    p  = reachy_poll_create();
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
      reachy_poll_unref(p);
      reachy_poll_unref(p);
      reachy_bridge_down(st);

      if(!reachy_nap(st, REACHY_BACKOFF_MS))
        break;

      continue;
    }

    if(!reachy_poll_wait(st, p))
    {
      // Abandoned rather than answered: the transfer still holds our
      // completion, and the last reference goes with it.
      reachy_poll_unref(p);

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
    reachy_poll_unref(p);

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
  pthread_cond_init(&st->wake_cond, NULL);

  return(st);
}

static void
reachy_destroy(void *handle)
{
  reachy_state_unref(handle);
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

  reachy_motors_mode("enabled", NULL, NULL);
  reachy_wake(NULL, NULL);

  // Taken BEFORE the spawn: a thread that starts instantly must never
  // find a state that has already been freed underneath it.
  reachy_state_ref(st);

  // TASK_NAME_SZ is 40 and a bot name may be 63, so the name is clipped
  // here rather than by snprintf — the task name is a label in
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

  // RUNNING, not AVAILABLE: the ear promotes us when the bridge answers.
  method_set_state(st->inst, METHOD_RUNNING);

  return(SUCCESS);
}

// Runs UNDER method_mutex, back to back with destroy(). No joins — the
// ears thread may be blocked taking this very mutex inside
// method_deliver — and no method_* calls.
static void
reachy_disconnect(void *handle)
{
  reachy_state_t *st = handle;

  __atomic_store_n(&st->shutdown, true, __ATOMIC_RELEASE);

  pthread_mutex_lock(&st->wake_mutex);
  pthread_cond_broadcast(&st->wake_cond);
  pthread_mutex_unlock(&st->wake_mutex);

  // Plain curl submits, which is legal under the mutex.
  reachy_stop_sound(NULL, NULL);

  if(reachy_kv_flag(st, "sleep_on_disconnect"))
    reachy_sleep_move(NULL, NULL);
}

// RCH-7 replaces this whole body with the mouth loop. SUCCESS rather
// than FAIL on purpose: the reply pipeline treats a send failure as
// something to report and retry, and there is nothing here to retry.
static bool
reachy_send(void *handle, const char *target, const char *text)
{
  reachy_state_t *st = handle;

  (void)target;

  clam(CLAM_WARN, REACHY_CTX,
      "%s: the mouth is not built yet — swallowed \"%.120s\"",
      st->botname, text);

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
          "ears thread %llu did not exit within %u ms — unloading now "
          "would unmap code it is still running",
          (unsigned long long)pending[i], (unsigned)REACHY_JOIN_MS);

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
  .ext             = &reachy_driver,
};
