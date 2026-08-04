// botmanager — MIT
// earbridge: the robot's ear. Shared microphone capture on the Reachy Mini's
// CM4, segmented into utterances by the XMOS hardware VAD and served to
// botman over HTTP long-poll.

#define EARBRIDGE_INTERNAL
#include "earbridge.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static eb_slot_t       eb_slots[EB_SLOTS];
static pthread_mutex_t eb_mtx;
static pthread_cond_t  eb_cv;
static uint32_t        eb_seq;

// Written by the VAD thread every poll, read by the capture thread every
// 20 ms period. The angle is only ever consumed at utterance open.
static atomic_bool  eb_vad_speech;
static _Atomic float eb_vad_angle;

static volatile sig_atomic_t eb_stop;
static eb_stats_t eb_stats;

// Sized from --max-s at startup and reused; see eb_slot_t on why nothing
// here is allocated per utterance.
static size_t   eb_wav_max;
static uint8_t *eb_send_buf;

static void
eb_usage(const char *argv0)
{
  fprintf(stderr,
    "usage: %s [options]\n"
    "\n"
    "  --daemon-url URL  reachy daemon, for the VAD   (default %s)\n"
    "  --port N          listen port                  (default %d)\n"
    "  --take MODE       ch0 | ch1 | mix              (default ch0)\n"
    "  --pre-ms N        pre-roll kept before speech  (default %d)\n"
    "  --hang-ms N       silence that ends utterance  (default %d)\n"
    "  --min-ms N        discard utterances shorter   (default %d)\n"
    "  --max-s N         force-close after            (default %d)\n"
    "  --poll-hz N       VAD polls per second         (default %d)\n"
    "\n"
    "Serves GET /utt?after=<seq>&wait=<ms> and GET /health.\n"
    "Captures via the shared dsnoop PCM: the robot daemon keeps the mic.\n",
    argv0, EB_DAEMON_DEFAULT, EB_PORT_DEFAULT, EB_PRE_MS_DEFAULT,
    EB_HANG_MS_DEFAULT, EB_MIN_MS_DEFAULT, EB_MAX_S_DEFAULT,
    EB_POLL_HZ_DEFAULT);
}

static bool
eb_parse_daemon_url(const char *url, eb_cfg_t *cfg)
{
  const char *host = url;
  const char *colon;
  size_t len;

  if(strncmp(host, "http://", 7) == 0)
    host += 7;

  cfg->daemon_port = 80;

  if((colon = strchr(host, ':')) != NULL)
  {
    cfg->daemon_port = (uint16_t)strtoul(colon + 1, NULL, 10);
    len = (size_t)(colon - host);
  }

  else
    len = strlen(host);

  if(len == 0 || len >= sizeof(cfg->daemon_host))
    return(false);

  memcpy(cfg->daemon_host, host, len);
  cfg->daemon_host[len] = '\0';

  if(cfg->daemon_port == 0)
    return(false);

  return(true);
}

static bool
eb_parse_args(int argc, char **argv, eb_cfg_t *cfg)
{
  int i = 1;

  memset(cfg, 0, sizeof(*cfg));
  cfg->port = EB_PORT_DEFAULT;
  cfg->pre_ms = EB_PRE_MS_DEFAULT;
  cfg->hang_ms = EB_HANG_MS_DEFAULT;
  cfg->min_ms = EB_MIN_MS_DEFAULT;
  cfg->max_s = EB_MAX_S_DEFAULT;
  cfg->poll_hz = EB_POLL_HZ_DEFAULT;
  cfg->take = EB_TAKE_CH0;

  if(!eb_parse_daemon_url(EB_DAEMON_DEFAULT, cfg))
    return(false);

  while(i < argc)
  {
    const char *arg = argv[i];
    const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;

    if(val == NULL)
    {
      fprintf(stderr, "earbridge: %s requires a value\n", arg);
      return(false);
    }

    if(strcmp(arg, "--daemon-url") == 0)
    {
      if(!eb_parse_daemon_url(val, cfg))
      {
        fprintf(stderr, "earbridge: cannot parse --daemon-url %s\n", val);
        return(false);
      }
    }

    else if(strcmp(arg, "--port") == 0)     cfg->port = (uint16_t)strtoul(val, NULL, 10);
    else if(strcmp(arg, "--pre-ms") == 0)   cfg->pre_ms = (uint32_t)strtoul(val, NULL, 10);
    else if(strcmp(arg, "--hang-ms") == 0)  cfg->hang_ms = (uint32_t)strtoul(val, NULL, 10);
    else if(strcmp(arg, "--min-ms") == 0)   cfg->min_ms = (uint32_t)strtoul(val, NULL, 10);
    else if(strcmp(arg, "--max-s") == 0)    cfg->max_s = (uint32_t)strtoul(val, NULL, 10);
    else if(strcmp(arg, "--poll-hz") == 0)  cfg->poll_hz = (uint32_t)strtoul(val, NULL, 10);

    else if(strcmp(arg, "--take") == 0)
    {
      if(strcmp(val, "ch0") == 0)      cfg->take = EB_TAKE_CH0;
      else if(strcmp(val, "ch1") == 0) cfg->take = EB_TAKE_CH1;
      else if(strcmp(val, "mix") == 0) cfg->take = EB_TAKE_MIX;

      else
      {
        fprintf(stderr, "earbridge: --take must be ch0, ch1 or mix\n");
        return(false);
      }
    }

    else
    {
      fprintf(stderr, "earbridge: unknown option %s\n", arg);
      return(false);
    }

    i += 2;
  }

  if(cfg->port == 0 || cfg->max_s == 0 || cfg->poll_hz == 0)
  {
    fprintf(stderr, "earbridge: --port, --max-s and --poll-hz must be non-zero\n");
    return(false);
  }

  // A pre-roll at or beyond the cap would leave no room for live audio.
  if(cfg->pre_ms >= cfg->max_s * 1000)
  {
    fprintf(stderr, "earbridge: --pre-ms must be less than --max-s\n");
    return(false);
  }

  return(true);
}

static void
eb_on_signal(int sig)
{
  (void)sig;

  eb_stop = 1;
}

static bool
eb_install_signals(void)
{
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = eb_on_signal;

  // No SA_RESTART: a blocked accept() must return EINTR so the loop can see
  // eb_stop rather than waiting for a client that will never arrive.
  if(sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0)
  {
    perror("earbridge: sigaction");
    return(false);
  }

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;

  if(sigaction(SIGPIPE, &sa, NULL) != 0)
  {
    perror("earbridge: sigaction(SIGPIPE)");
    return(false);
  }

  return(true);
}

static void
eb_wav_put32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void
eb_wav_put16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

// Mono 16 kHz S16LE — whisper's native form, so nothing downstream resamples.
static void
eb_wav_header(uint8_t *buf, size_t data_len)
{
  memcpy(buf + 0, "RIFF", 4);
  eb_wav_put32(buf + 4, (uint32_t)(EB_WAV_HEADER_SZ + data_len - 8));
  memcpy(buf + 8, "WAVEfmt ", 8);
  eb_wav_put32(buf + 16, 16);
  eb_wav_put16(buf + 20, 1);
  eb_wav_put16(buf + 22, 1);
  eb_wav_put32(buf + 24, EB_RATE);
  eb_wav_put32(buf + 28, EB_RATE * 2);
  eb_wav_put16(buf + 32, 2);
  eb_wav_put16(buf + 34, 16);
  memcpy(buf + 36, "data", 4);
  eb_wav_put32(buf + 40, (uint32_t)data_len);
}

static snd_pcm_t *
eb_pcm_open(void)
{
  snd_pcm_t *pcm = NULL;
  snd_pcm_hw_params_t *hw = NULL;
  unsigned int rate = EB_RATE;
  snd_pcm_uframes_t period = EB_PERIOD_FRAMES;
  int err;

  // Never hw:0,0 — that is the exclusive default PCM and opening it steals
  // the dsnoop slave out from under the robot daemon.
  if((err = snd_pcm_open(&pcm, EB_PCM_DEVICE, SND_PCM_STREAM_CAPTURE, 0)) < 0)
  {
    fprintf(stderr, "earbridge: snd_pcm_open(%s): %s\n", EB_PCM_DEVICE, snd_strerror(err));
    return(NULL);
  }

  // snd_pcm_hw_params_alloca() expands to alloca(), which -std=c11 does not
  // declare; the malloc form is explicit about the lifetime regardless.
  if((err = snd_pcm_hw_params_malloc(&hw)) < 0)
  {
    fprintf(stderr, "earbridge: hw_params_malloc: %s\n", snd_strerror(err));
    snd_pcm_close(pcm);
    return(NULL);
  }

  if((err = snd_pcm_hw_params_any(pcm, hw)) < 0
     || (err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0
     || (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0
     || (err = snd_pcm_hw_params_set_channels(pcm, hw, EB_CHANNELS_IN)) < 0
     || (err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL)) < 0
     || (err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL)) < 0
     || (err = snd_pcm_hw_params(pcm, hw)) < 0)
  {
    fprintf(stderr, "earbridge: hw_params: %s\n", snd_strerror(err));
    snd_pcm_hw_params_free(hw);
    snd_pcm_close(pcm);
    return(NULL);
  }

  snd_pcm_hw_params_free(hw);

  if(rate != EB_RATE)
  {
    fprintf(stderr, "earbridge: device gave %u Hz, need %d — refusing to ship "
                    "audio whisper would mis-hear\n", rate, EB_RATE);
    snd_pcm_close(pcm);
    return(NULL);
  }

  return(pcm);
}

static void
eb_publish(const int16_t *pcm, size_t samples, float doa, bool clipped)
{
  const size_t data_len = samples * sizeof(int16_t);
  eb_slot_t *slot;

  pthread_mutex_lock(&eb_mtx);

  eb_seq++;
  slot = &eb_slots[eb_seq % EB_SLOTS];

  // Recycling a slot nobody ever read means a client fell behind; that is a
  // real signal (botman stalled) and worth a counter rather than silence.
  if(slot->seq != 0 && !slot->delivered)
    atomic_fetch_add(&eb_stats.dropped, 1);

  slot->seq = eb_seq;
  slot->doa = doa;
  slot->ms = (uint32_t)((samples * 1000) / EB_RATE);
  slot->clipped = clipped;
  slot->delivered = false;
  slot->wav_len = EB_WAV_HEADER_SZ + data_len;

  eb_wav_header(slot->wav, data_len);
  memcpy(slot->wav + EB_WAV_HEADER_SZ, pcm, data_len);

  atomic_fetch_add(&eb_stats.utterances, 1);

  if(clipped)
    atomic_fetch_add(&eb_stats.clipped, 1);

  pthread_cond_broadcast(&eb_cv);
  pthread_mutex_unlock(&eb_mtx);

  fprintf(stderr, "earbridge: utterance seq=%u %u ms doa=%.3f%s\n",
    slot->seq, slot->ms, (double)doa, clipped ? " CLIPPED" : "");
}

static void *
eb_capture_thread(void *arg)
{
  const eb_cfg_t *cfg = arg;
  const size_t pre_cap = ((size_t)EB_RATE * cfg->pre_ms) / 1000;
  const size_t max_samples = (size_t)EB_RATE * cfg->max_s;
  const size_t min_samples = ((size_t)EB_RATE * cfg->min_ms) / 1000;
  const size_t hang_samples = ((size_t)EB_RATE * cfg->hang_ms) / 1000;
  int16_t *pre = NULL;
  int16_t *utt = NULL;
  int16_t *raw = NULL;
  snd_pcm_t *pcm = NULL;
  size_t pre_head = 0;
  size_t pre_fill = 0;
  size_t utt_len = 0;
  size_t silence = 0;
  float open_angle = 0.0f;
  bool open = false;

  if(pre_cap > 0)
    pre = calloc(pre_cap, sizeof(int16_t));

  utt = malloc(max_samples * sizeof(int16_t));
  raw = malloc((size_t)EB_PERIOD_FRAMES * EB_CHANNELS_IN * sizeof(int16_t));

  if((pre_cap > 0 && pre == NULL) || utt == NULL || raw == NULL)
  {
    fprintf(stderr, "earbridge: out of memory sizing capture buffers\n");
    free(pre);
    free(utt);
    free(raw);
    return(NULL);
  }

  while(eb_stop == 0)
  {
    snd_pcm_sframes_t got;
    bool speech;
    size_t i;

    if(pcm == NULL)
    {
      if((pcm = eb_pcm_open()) == NULL)
      {
        sleep(1);
        continue;
      }

      // A reopen means we lost an unknown amount of audio; anything half
      // captured is no longer a trustworthy utterance.
      open = false;
      utt_len = 0;
      pre_fill = 0;
      pre_head = 0;
    }

    if((got = snd_pcm_readi(pcm, raw, EB_PERIOD_FRAMES)) < 0)
    {
      if(snd_pcm_recover(pcm, (int)got, 1) == 0)
      {
        atomic_fetch_add(&eb_stats.alsa_recovered, 1);
        continue;
      }

      fprintf(stderr, "earbridge: capture failed (%s), reopening\n",
        snd_strerror((int)got));
      snd_pcm_close(pcm);
      pcm = NULL;
      atomic_fetch_add(&eb_stats.alsa_reopened, 1);
      sleep(1);
      continue;
    }

    speech = atomic_load(&eb_vad_speech);

    for(i = 0; i < (size_t)got; i++)
    {
      const int16_t l = raw[(i * EB_CHANNELS_IN) + 0];
      const int16_t r = raw[(i * EB_CHANNELS_IN) + 1];
      int16_t s;

      if(cfg->take == EB_TAKE_CH0)      s = l;
      else if(cfg->take == EB_TAKE_CH1) s = r;
      else                              s = (int16_t)(((int32_t)l + (int32_t)r) / 2);

      if(!open)
      {
        if(pre_cap > 0)
        {
          pre[pre_head] = s;
          pre_head = (pre_head + 1) % pre_cap;

          if(pre_fill < pre_cap)
            pre_fill++;
        }

        continue;
      }

      if(utt_len < max_samples)
        utt[utt_len++] = s;
    }

    if(!open && speech)
    {
      size_t start;
      size_t n;

      // Drain the pre-roll oldest-first so the utterance carries the moment
      // before the VAD noticed — speech onset is always clipped otherwise.
      open_angle = atomic_load(&eb_vad_angle);
      start = (pre_head + pre_cap - pre_fill) % (pre_cap > 0 ? pre_cap : 1);
      utt_len = 0;

      for(n = 0; n < pre_fill && utt_len < max_samples; n++)
        utt[utt_len++] = pre[(start + n) % pre_cap];

      pre_fill = 0;
      pre_head = 0;
      silence = 0;
      open = true;
      continue;
    }

    if(!open)
      continue;

    if(speech)
      silence = 0;

    else
      silence += (size_t)got;

    if(silence >= hang_samples || utt_len >= max_samples)
    {
      const bool clipped = (utt_len >= max_samples);

      // The trailing silence is hang-time, not speech; keep it out of the
      // length test so --min-ms means "this much voice".
      if(utt_len > silence && (utt_len - silence) >= min_samples)
        eb_publish(utt, utt_len, open_angle, clipped);

      else
        atomic_fetch_add(&eb_stats.short_discards, 1);

      open = false;
      utt_len = 0;
      silence = 0;
    }
  }

  if(pcm != NULL)
    snd_pcm_close(pcm);

  free(pre);
  free(utt);
  free(raw);
  return(NULL);
}

// One blocking HTTP/1.0 GET, no libcurl: the only endpoint is on loopback and
// the response is a two-field JSON object.
static int
eb_vad_fetch(const eb_cfg_t *cfg, float *angle, bool *speech)
{
  struct sockaddr_in addr;
  struct timeval tv;
  char req[256];
  char buf[1024];
  const char *p;
  size_t used = 0;
  int fd;
  int n;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(cfg->daemon_port);

  if(inet_pton(AF_INET, cfg->daemon_host, &addr.sin_addr) != 1)
    return(-1);

  if((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    return(-1);

  tv.tv_sec = EB_VAD_TIMEOUT;
  tv.tv_usec = 0;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  if(connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0)
  {
    close(fd);
    return(-1);
  }

  n = snprintf(req, sizeof(req),
    "GET /api/state/doa HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
    cfg->daemon_host);

  if(n < 0 || (size_t)n >= sizeof(req) || !eb_send_all(fd, req, (size_t)n))
  {
    close(fd);
    return(-1);
  }

  while(used < sizeof(buf) - 1)
  {
    const ssize_t got = read(fd, buf + used, sizeof(buf) - 1 - used);

    if(got < 0)
    {
      if(errno == EINTR)
        continue;

      break;
    }

    if(got == 0)
      break;

    used += (size_t)got;
  }

  close(fd);
  buf[used] = '\0';

  if(strstr(buf, "\"angle\"") == NULL)
    return(-1);

  *speech = (strstr(buf, "\"speech_detected\":true") != NULL);
  *angle = 0.0f;

  if((p = strstr(buf, "\"angle\":")) != NULL)
    (void)sscanf(p + 8, "%f", angle);

  return(0);
}

static void *
eb_vad_thread(void *arg)
{
  const eb_cfg_t *cfg = arg;
  const long interval_ns = 1000000000L / (long)cfg->poll_hz;

  while(eb_stop == 0)
  {
    struct timespec ts;
    float angle = 0.0f;
    bool speech = false;

    if(eb_vad_fetch(cfg, &angle, &speech) == 0)
    {
      atomic_store(&eb_vad_angle, angle);
      atomic_store(&eb_vad_speech, speech);
    }

    else
    {
      // The robot daemon outlives us but also boots slower; an unreachable
      // daemon means "no speech", never a crash and never a stale true.
      atomic_store(&eb_vad_speech, false);
      atomic_fetch_add(&eb_stats.vad_failures, 1);
    }

    atomic_fetch_add(&eb_stats.vad_polls, 1);

    ts.tv_sec = interval_ns / 1000000000L;
    ts.tv_nsec = interval_ns % 1000000000L;
    (void)nanosleep(&ts, NULL);
  }

  return(NULL);
}

static int
eb_listen(uint16_t port)
{
  struct sockaddr_in addr;
  int fd;
  int on = 1;

  if((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
  {
    perror("earbridge: socket");
    return(-1);
  }

  if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0)
  {
    perror("earbridge: SO_REUSEADDR");
    close(fd);
    return(-1);
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if(bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0)
  {
    perror("earbridge: bind");
    close(fd);
    return(-1);
  }

  if(listen(fd, EB_BACKLOG) != 0)
  {
    perror("earbridge: listen");
    close(fd);
    return(-1);
  }

  return(fd);
}

static bool
eb_send_all(int fd, const void *buf, size_t len)
{
  const uint8_t *p = buf;
  size_t sent = 0;

  while(sent < len)
  {
    const ssize_t n = write(fd, p + sent, len - sent);

    if(n < 0)
    {
      if(errno == EINTR)
        continue;

      return(false);
    }

    if(n == 0)
      return(false);

    sent += (size_t)n;
  }

  return(true);
}

static bool
eb_query_uint(const char *query, const char *key, uint32_t *out)
{
  char pattern[32];
  const char *p;

  if((size_t)snprintf(pattern, sizeof(pattern), "%s=", key) >= sizeof(pattern))
    return(false);

  if((p = strstr(query, pattern)) == NULL)
    return(false);

  *out = (uint32_t)strtoul(p + strlen(pattern), NULL, 10);
  return(true);
}

static void
eb_handle_utt(int fd, uint32_t after, uint32_t wait_ms)
{
  struct timespec deadline;
  char head[512];
  size_t len = 0;
  uint32_t seq = 0;
  uint32_t ms = 0;
  float doa = 0.0f;
  bool clipped = false;
  bool found = false;
  int n;

  if(wait_ms > EB_WAIT_MS_MAX)
    wait_ms = EB_WAIT_MS_MAX;

  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_sec += (time_t)(wait_ms / 1000);
  deadline.tv_nsec += (long)(wait_ms % 1000) * 1000000L;

  if(deadline.tv_nsec >= 1000000000L)
  {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }

  pthread_mutex_lock(&eb_mtx);

  while(eb_stop == 0)
  {
    struct timespec now;
    struct timespec slice;
    eb_slot_t *best = NULL;
    size_t i;

    // Oldest slot newer than `after`, so a client walks the ring forward in
    // order rather than skipping to the newest and losing the middle.
    for(i = 0; i < EB_SLOTS; i++)
    {
      if(eb_slots[i].seq > after && (best == NULL || eb_slots[i].seq < best->seq))
        best = &eb_slots[i];
    }

    if(best != NULL)
    {
      memcpy(eb_send_buf, best->wav, best->wav_len);
      len = best->wav_len;
      seq = best->seq;
      ms = best->ms;
      doa = best->doa;
      clipped = best->clipped;
      best->delivered = true;
      found = true;
      break;
    }

    clock_gettime(CLOCK_MONOTONIC, &now);

    if(now.tv_sec > deadline.tv_sec
       || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
      break;

    // Wake at least every 250 ms even with a 30 s deadline, so SIGTERM is
    // observed promptly instead of after the client's full wait.
    slice = now;
    slice.tv_nsec += 250000000L;

    if(slice.tv_nsec >= 1000000000L)
    {
      slice.tv_sec++;
      slice.tv_nsec -= 1000000000L;
    }

    if(slice.tv_sec > deadline.tv_sec
       || (slice.tv_sec == deadline.tv_sec && slice.tv_nsec > deadline.tv_nsec))
      slice = deadline;

    (void)pthread_cond_timedwait(&eb_cv, &eb_mtx, &slice);
  }

  pthread_mutex_unlock(&eb_mtx);

  if(!found)
  {
    const char *empty =
      "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";

    (void)eb_send_all(fd, empty, strlen(empty));
    return;
  }

  n = snprintf(head, sizeof(head),
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: audio/wav\r\n"
    "Content-Length: %zu\r\n"
    "X-Utt-Seq: %u\r\n"
    "X-Utt-DoA: %.3f\r\n"
    "X-Utt-Ms: %u\r\n"
    "X-Utt-Clipped: %d\r\n"
    "Connection: close\r\n"
    "\r\n",
    len, seq, (double)doa, ms, clipped ? 1 : 0);

  if(n < 0 || (size_t)n >= sizeof(head))
    return;

  if(eb_send_all(fd, head, (size_t)n))
    (void)eb_send_all(fd, eb_send_buf, len);
}

static void
eb_handle_health(int fd, const eb_cfg_t *cfg)
{
  char body[768];
  char head[256];
  uint32_t seq;
  int n;
  int h;

  pthread_mutex_lock(&eb_mtx);
  seq = eb_seq;
  pthread_mutex_unlock(&eb_mtx);

  n = snprintf(body, sizeof(body),
    "{\"ok\":true,\"seq\":%u,\"speech\":%s,\"doa\":%.3f,"
    "\"utterances\":%llu,\"dropped\":%llu,\"short_discards\":%llu,"
    "\"clipped\":%llu,\"alsa_recovered\":%llu,\"alsa_reopened\":%llu,"
    "\"vad_polls\":%llu,\"vad_failures\":%llu,"
    "\"take\":\"%s\",\"pre_ms\":%u,\"hang_ms\":%u,\"min_ms\":%u,\"max_s\":%u}\n",
    seq,
    atomic_load(&eb_vad_speech) ? "true" : "false",
    (double)atomic_load(&eb_vad_angle),
    (unsigned long long)atomic_load(&eb_stats.utterances),
    (unsigned long long)atomic_load(&eb_stats.dropped),
    (unsigned long long)atomic_load(&eb_stats.short_discards),
    (unsigned long long)atomic_load(&eb_stats.clipped),
    (unsigned long long)atomic_load(&eb_stats.alsa_recovered),
    (unsigned long long)atomic_load(&eb_stats.alsa_reopened),
    (unsigned long long)atomic_load(&eb_stats.vad_polls),
    (unsigned long long)atomic_load(&eb_stats.vad_failures),
    (cfg->take == EB_TAKE_CH0) ? "ch0" : ((cfg->take == EB_TAKE_CH1) ? "ch1" : "mix"),
    cfg->pre_ms, cfg->hang_ms, cfg->min_ms, cfg->max_s);

  if(n < 0 || (size_t)n >= sizeof(body))
    return;

  h = snprintf(head, sizeof(head),
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
    "Content-Length: %d\r\nConnection: close\r\n\r\n", n);

  if(h < 0 || (size_t)h >= sizeof(head))
    return;

  if(eb_send_all(fd, head, (size_t)h))
    (void)eb_send_all(fd, body, (size_t)n);
}

static void
eb_serve(int fd, const eb_cfg_t *cfg)
{
  char buf[EB_REQUEST_MAX];
  size_t used = 0;
  uint32_t after = 0;
  uint32_t wait_ms = 0;

  while(used < sizeof(buf) - 1)
  {
    const ssize_t n = read(fd, buf + used, sizeof(buf) - 1 - used);

    if(n < 0)
    {
      if(errno == EINTR)
        continue;

      return;
    }

    if(n == 0)
      break;

    used += (size_t)n;
    buf[used] = '\0';

    if(strstr(buf, "\r\n") != NULL)
      break;
  }

  buf[used] = '\0';

  if(strncmp(buf, "GET /utt", 8) == 0)
  {
    (void)eb_query_uint(buf, "after", &after);
    (void)eb_query_uint(buf, "wait", &wait_ms);
    eb_handle_utt(fd, after, wait_ms);
    return;
  }

  if(strncmp(buf, "GET /health", 11) == 0)
  {
    eb_handle_health(fd, cfg);
    return;
  }

  {
    const char *nf =
      "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\n"
      "Content-Length: 52\r\nConnection: close\r\n\r\n"
      "{\"error\":\"try GET /utt?after=N&wait=MS or /health\"}\n";

    (void)eb_send_all(fd, nf, strlen(nf));
  }
}

int
main(int argc, char **argv)
{
  eb_cfg_t cfg;
  pthread_condattr_t cattr;
  pthread_t capture_tid;
  pthread_t vad_tid;
  int listen_fd;
  size_t i;

  if(!eb_parse_args(argc, argv, &cfg))
  {
    eb_usage(argv[0]);
    return(EXIT_FAILURE);
  }

  if(!eb_install_signals())
    return(EXIT_FAILURE);

  eb_wav_max = EB_WAV_HEADER_SZ + ((size_t)EB_RATE * cfg.max_s * sizeof(int16_t));

  for(i = 0; i < EB_SLOTS; i++)
  {
    if((eb_slots[i].wav = malloc(eb_wav_max)) == NULL)
    {
      fprintf(stderr, "earbridge: out of memory allocating slot buffers\n");
      return(EXIT_FAILURE);
    }
  }

  if((eb_send_buf = malloc(eb_wav_max)) == NULL)
  {
    fprintf(stderr, "earbridge: out of memory allocating the send buffer\n");
    return(EXIT_FAILURE);
  }

  if(pthread_mutex_init(&eb_mtx, NULL) != 0)
  {
    perror("earbridge: pthread_mutex_init");
    return(EXIT_FAILURE);
  }

  // CLOCK_MONOTONIC so a wall-clock step (this box runs NTP) cannot turn a
  // 3 s long-poll into a 3 hour one.
  if(pthread_condattr_init(&cattr) != 0
     || pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC) != 0
     || pthread_cond_init(&eb_cv, &cattr) != 0)
  {
    perror("earbridge: pthread_cond_init");
    return(EXIT_FAILURE);
  }

  pthread_condattr_destroy(&cattr);

  if((listen_fd = eb_listen(cfg.port)) < 0)
    return(EXIT_FAILURE);

  if(pthread_create(&vad_tid, NULL, eb_vad_thread, &cfg) != 0)
  {
    perror("earbridge: pthread_create(vad)");
    close(listen_fd);
    return(EXIT_FAILURE);
  }

  if(pthread_create(&capture_tid, NULL, eb_capture_thread, &cfg) != 0)
  {
    perror("earbridge: pthread_create(capture)");
    eb_stop = 1;
    pthread_join(vad_tid, NULL);
    close(listen_fd);
    return(EXIT_FAILURE);
  }

  fprintf(stderr, "earbridge: listening on 0.0.0.0:%u — take=%s, vad via %s:%u, "
                  "pre=%ums hang=%ums min=%ums max=%us\n",
    cfg.port,
    (cfg.take == EB_TAKE_CH0) ? "ch0" : ((cfg.take == EB_TAKE_CH1) ? "ch1" : "mix"),
    cfg.daemon_host, cfg.daemon_port,
    cfg.pre_ms, cfg.hang_ms, cfg.min_ms, cfg.max_s);

  while(eb_stop == 0)
  {
    struct timeval tv;
    int fd = accept(listen_fd, NULL, NULL);

    if(fd < 0)
    {
      if(errno == EINTR)
        continue;

      perror("earbridge: accept");
      break;
    }

    // Generous: a long-poll legitimately holds the connection open for up to
    // EB_WAIT_MS_MAX before answering 204.
    tv.tv_sec = EB_CLIENT_TIMEOUT;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    eb_serve(fd, &cfg);
    close(fd);
  }

  fprintf(stderr, "earbridge: shutting down\n");
  eb_stop = 1;
  close(listen_fd);

  pthread_mutex_lock(&eb_mtx);
  pthread_cond_broadcast(&eb_cv);
  pthread_mutex_unlock(&eb_mtx);

  pthread_join(capture_tid, NULL);
  pthread_join(vad_tid, NULL);

  for(i = 0; i < EB_SLOTS; i++)
    free(eb_slots[i].wav);

  free(eb_send_buf);
  pthread_cond_destroy(&eb_cv);
  pthread_mutex_destroy(&eb_mtx);

  return(EXIT_SUCCESS);
}
