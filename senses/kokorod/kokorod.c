// botmanager — MIT
// kokorod: the robot's voice. Kokoro speech synthesis behind an
// OpenAI-shaped /v1/audio/speech endpoint, served as stereo WAV.

#define KOKOROD_INTERNAL
#include "kokorod.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// Kokoro's speaker ids are positional: sid N is entry N here.
//
// This is NOT the upstream Hugging Face voice list, which has 54 entries —
// kokoro-multi-lang-v1_0 ships 53 and omits em_santa, so every voice after
// em_alex would be off by one. The authoritative mapping is embedded in the
// model itself; re-derive it for any other Kokoro build with:
//
//   strings -n 6 model.onnx | grep -A1 speaker_names
//
// which prints the comma-separated names in sid order (alongside id2speaker
// and speaker2id maps saying the same thing).
const char *const kokorod_voice_tbl[] =
{
  "af_alloy",   "af_aoede",     "af_bella",   "af_heart",    "af_jessica",
  "af_kore",    "af_nicole",    "af_nova",    "af_river",    "af_sarah",
  "af_sky",     "am_adam",      "am_echo",    "am_eric",     "am_fenrir",
  "am_liam",    "am_michael",   "am_onyx",    "am_puck",     "am_santa",
  "bf_alice",   "bf_emma",      "bf_isabella","bf_lily",     "bm_daniel",
  "bm_fable",   "bm_george",    "bm_lewis",   "ef_dora",     "em_alex",
  "ff_siwis",   "hf_alpha",     "hf_beta",    "hm_omega",    "hm_psi",
  "if_sara",    "im_nicola",    "jf_alpha",   "jf_gongitsune","jf_nezumi",
  "jf_tebukuro","jm_kumo",      "pf_dora",    "pm_alex",     "pm_santa",
  "zf_xiaobei", "zf_xiaoni",    "zf_xiaoxiao","zf_xiaoyi",   "zm_yunjian",
  "zm_yunxi",   "zm_yunxia",    "zm_yunyang",
};

const int32_t kokorod_voice_count =
  (int32_t)(sizeof(kokorod_voice_tbl) / sizeof(kokorod_voice_tbl[0]));

// Written only by the signal handler, read only by the accept loop.
static volatile sig_atomic_t kokorod_stop = 0;

static void
kokorod_usage(const char *argv0)
{
  fprintf(stderr,
    "usage: %s --model FILE --voices FILE --tokens FILE [options]\n"
    "\n"
    "  --model FILE      Kokoro model.onnx                        (required)\n"
    "  --voices FILE     voices.bin                               (required)\n"
    "  --tokens FILE     tokens.txt                               (required)\n"
    "  --data-dir DIR    espeak-ng-data directory\n"
    "  --lexicon FILE    lexicon file(s), comma separated\n"
    "  --lang LANG       language hint, e.g. en\n"
    "  --port N          listen port on 127.0.0.1        (default %d)\n"
    "  --threads N       synthesis threads               (default %d)\n"
    "\n"
    "Serves POST /v1/audio/speech and GET /health.\n",
    argv0, KOKOROD_PORT_DEFAULT, KOKOROD_THREADS_DEFAULT);
}

static bool
kokorod_parse_args(int argc, char **argv, kokorod_cfg_t *cfg)
{
  int i = 1;

  memset(cfg, 0, sizeof(*cfg));
  cfg->port = KOKOROD_PORT_DEFAULT;
  cfg->threads = KOKOROD_THREADS_DEFAULT;

  while(i < argc)
  {
    const char *arg = argv[i];
    const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;

    // Every option below takes a value; reject a trailing bare flag here
    // rather than letting NULL reach strtol or the config struct.
    if(val == NULL)
    {
      fprintf(stderr, "kokorod: %s requires a value\n", arg);
      return(false);
    }

    if(strcmp(arg, "--model") == 0)          cfg->model = val;
    else if(strcmp(arg, "--voices") == 0)    cfg->voices = val;
    else if(strcmp(arg, "--tokens") == 0)    cfg->tokens = val;
    else if(strcmp(arg, "--data-dir") == 0)  cfg->data_dir = val;
    else if(strcmp(arg, "--lexicon") == 0)   cfg->lexicon = val;
    else if(strcmp(arg, "--lang") == 0)      cfg->lang = val;
    else if(strcmp(arg, "--port") == 0)      cfg->port = (uint16_t)strtoul(val, NULL, 10);
    else if(strcmp(arg, "--threads") == 0)   cfg->threads = (int32_t)strtol(val, NULL, 10);

    else
    {
      fprintf(stderr, "kokorod: unknown option %s\n", arg);
      return(false);
    }

    i += 2;
  }

  if(cfg->model == NULL || cfg->voices == NULL || cfg->tokens == NULL)
  {
    fprintf(stderr, "kokorod: --model, --voices and --tokens are all required\n");
    return(false);
  }

  if(cfg->port == 0)
  {
    fprintf(stderr, "kokorod: --port must be 1-65535\n");
    return(false);
  }

  if(cfg->threads < 1)
    cfg->threads = 1;

  return(true);
}

static void
kokorod_on_signal(int sig)
{
  (void)sig;

  kokorod_stop = 1;
}

static bool
kokorod_install_signals(void)
{
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = kokorod_on_signal;

  // Deliberately no SA_RESTART: a blocked accept() must return EINTR so the
  // loop can observe kokorod_stop and exit instead of waiting for a client
  // that will never connect.
  if(sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0)
  {
    perror("kokorod: sigaction");
    return(false);
  }

  // A client that hangs up mid-response must cost us the write, not the
  // process.
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;

  if(sigaction(SIGPIPE, &sa, NULL) != 0)
  {
    perror("kokorod: sigaction(SIGPIPE)");
    return(false);
  }

  return(true);
}

static int32_t
kokorod_voice_sid(const char *name)
{
  int32_t i;

  for(i = 0; i < kokorod_voice_count; i++)
  {
    if(strcmp(name, kokorod_voice_tbl[i]) == 0)
      return(i);
  }

  return(-1);
}

static size_t
kokorod_utf8_encode(uint32_t cp, char *out)
{
  if(cp < 0x80)
  {
    out[0] = (char)cp;
    return(1);
  }

  if(cp < 0x800)
  {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return(2);
  }

  if(cp < 0x10000)
  {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return(3);
  }

  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return(4);
}

// A deliberately small tolerant scanner rather than a JSON dependency: the
// request shape is fixed and machine-generated. It finds "<key>" at any depth
// and decodes the string that follows, so it would be wrong for arbitrary
// JSON — a key of the same name nested elsewhere would win if it came first.
// Adequate and honest for a four-field body; do not grow it into a parser.
static bool
kokorod_json_string(const char *body, const char *key, char *out, size_t out_sz)
{
  char pattern[64];
  const char *p;
  size_t used = 0;

  if(out_sz == 0)
    return(false);

  out[0] = '\0';

  if((size_t)snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= sizeof(pattern))
    return(false);

  if((p = strstr(body, pattern)) == NULL)
    return(false);

  p += strlen(pattern);

  while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;

  if(*p != ':')
    return(false);

  p++;

  while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;

  if(*p != '"')
    return(false);

  p++;

  while(*p != '\0' && *p != '"')
  {
    char decoded[4];
    size_t len = 1;

    if(*p == '\\')
    {
      p++;

      switch(*p)
      {
        case '"':  decoded[0] = '"';  break;
        case '\\': decoded[0] = '\\'; break;
        case '/':  decoded[0] = '/';  break;
        case 'b':  decoded[0] = '\b'; break;
        case 'f':  decoded[0] = '\f'; break;
        case 'n':  decoded[0] = '\n'; break;
        case 'r':  decoded[0] = '\r'; break;
        case 't':  decoded[0] = '\t'; break;

        case 'u':
        {
          char hex[5] = {0};
          uint32_t cp;

          if(strnlen(p + 1, 4) < 4)
            return(false);

          memcpy(hex, p + 1, 4);
          cp = (uint32_t)strtoul(hex, NULL, 16);
          p += 4;

          // Surrogate pair: a high surrogate is meaningless alone, so pull
          // its partner in rather than emitting a replacement character.
          if(cp >= 0xD800 && cp <= 0xDBFF && p[1] == '\\' && p[2] == 'u')
          {
            char lo_hex[5] = {0};
            uint32_t lo;

            if(strnlen(p + 3, 4) < 4)
              return(false);

            memcpy(lo_hex, p + 3, 4);
            lo = (uint32_t)strtoul(lo_hex, NULL, 16);

            if(lo >= 0xDC00 && lo <= 0xDFFF)
            {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              p += 6;
            }
          }

          len = kokorod_utf8_encode(cp, decoded);
          break;
        }

        case '\0':
          return(false);

        default:
          decoded[0] = *p;
          break;
      }
    }

    else
      decoded[0] = *p;

    if(used + len >= out_sz)
      return(false);

    memcpy(out + used, decoded, len);
    used += len;
    p++;
  }

  if(*p != '"')
    return(false);

  out[used] = '\0';
  return(true);
}

static bool
kokorod_json_number(const char *body, const char *key, double *out)
{
  char pattern[64];
  const char *p;
  char *end = NULL;
  double v;

  if((size_t)snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= sizeof(pattern))
    return(false);

  if((p = strstr(body, pattern)) == NULL)
    return(false);

  p += strlen(pattern);

  while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;

  if(*p != ':')
    return(false);

  p++;
  v = strtod(p, &end);

  if(end == p)
    return(false);

  *out = v;
  return(true);
}

static void
kokorod_wav_put32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void
kokorod_wav_put16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

// Returns a malloc'd WAV the caller frees. Mono float -> peak-normalised,
// channel-duplicated S16 stereo; see the header for why both transforms are
// mandatory rather than cosmetic.
static uint8_t *
kokorod_wav_build(const SherpaOnnxGeneratedAudio *audio, size_t *out_len)
{
  const uint32_t rate = (uint32_t)audio->sample_rate;
  const uint32_t block_align = KOKOROD_CHANNELS * 2u;
  const size_t frames = (size_t)audio->n;
  const size_t data_len = frames * block_align;
  const size_t total = KOKOROD_WAV_HEADER_SZ + data_len;
  uint8_t *buf;
  uint8_t *pcm;
  float peak = 0.0f;
  float scale;
  size_t i;

  if(audio->n <= 0)
    return(NULL);

  if((buf = malloc(total)) == NULL)
    return(NULL);

  memcpy(buf + 0, "RIFF", 4);
  kokorod_wav_put32(buf + 4, (uint32_t)(total - 8));
  memcpy(buf + 8, "WAVEfmt ", 8);
  kokorod_wav_put32(buf + 16, 16);                        // fmt chunk size
  kokorod_wav_put16(buf + 20, 1);                         // PCM
  kokorod_wav_put16(buf + 22, KOKOROD_CHANNELS);
  kokorod_wav_put32(buf + 24, rate);
  kokorod_wav_put32(buf + 28, rate * block_align);        // byte rate
  kokorod_wav_put16(buf + 32, (uint16_t)block_align);
  kokorod_wav_put16(buf + 34, 16);                        // bits per sample
  memcpy(buf + 36, "data", 4);
  kokorod_wav_put32(buf + 40, (uint32_t)data_len);

  for(i = 0; i < frames; i++)
  {
    const float m = fabsf(audio->samples[i]);

    if(m > peak)
      peak = m;
  }

  // Digital silence would otherwise divide by zero and amplify nothing into
  // full-scale noise.
  scale = (peak > KOKOROD_PEAK_EPSILON) ? (KOKOROD_PEAK_TARGET / peak) : 1.0f;

  pcm = buf + KOKOROD_WAV_HEADER_SZ;

  for(i = 0; i < frames; i++)
  {
    float v = audio->samples[i] * scale;
    int16_t s;
    int32_t c;

    if(v > 1.0f)  v = 1.0f;
    if(v < -1.0f) v = -1.0f;

    s = (int16_t)rintf(v * 32767.0f);

    for(c = 0; c < KOKOROD_CHANNELS; c++)
      kokorod_wav_put16(pcm + (i * block_align) + ((size_t)c * 2u), (uint16_t)s);
  }

  *out_len = total;
  return(buf);
}

static int
kokorod_listen(uint16_t port)
{
  struct sockaddr_in addr;
  int fd;
  int on = 1;

  if((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
  {
    perror("kokorod: socket");
    return(-1);
  }

  if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0)
  {
    perror("kokorod: SO_REUSEADDR");
    close(fd);
    return(-1);
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if(bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0)
  {
    perror("kokorod: bind");
    close(fd);
    return(-1);
  }

  if(listen(fd, KOKOROD_BACKLOG) != 0)
  {
    perror("kokorod: listen");
    close(fd);
    return(-1);
  }

  return(fd);
}

static bool
kokorod_send_all(int fd, const void *buf, size_t len)
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

static void
kokorod_respond(int fd, int status, const char *reason, const char *ctype,
                const void *body, size_t len)
{
  char head[256];
  const int n = snprintf(head, sizeof(head),
    "HTTP/1.1 %d %s\r\n"
    "Content-Type: %s\r\n"
    "Content-Length: %zu\r\n"
    "Connection: close\r\n"
    "\r\n",
    status, reason, ctype, len);

  if(n < 0 || (size_t)n >= sizeof(head))
    return;

  if(!kokorod_send_all(fd, head, (size_t)n))
    return;

  if(len > 0)
    (void)kokorod_send_all(fd, body, len);
}

static void
kokorod_respond_text(int fd, int status, const char *reason, const char *body)
{
  kokorod_respond(fd, status, reason, "application/json", body, strlen(body));
}

// Reads headers, then exactly Content-Length bytes of body. buf is always
// NUL-terminated on success so the JSON scanners can treat it as a string.
static bool
kokorod_read_request(int fd, char *buf, size_t buf_sz, size_t *body_off,
                     size_t *body_len)
{
  size_t used = 0;
  size_t header_end = 0;
  size_t content_len = 0;
  const char *cl;
  size_t i;

  *body_off = 0;
  *body_len = 0;

  while(header_end == 0)
  {
    const ssize_t n = read(fd, buf + used, buf_sz - 1 - used);

    if(n < 0)
    {
      if(errno == EINTR)
        continue;

      return(false);
    }

    if(n == 0)
      return(false);

    used += (size_t)n;
    buf[used] = '\0';

    for(i = 3; i < used; i++)
    {
      if(buf[i - 3] == '\r' && buf[i - 2] == '\n' && buf[i - 1] == '\r' && buf[i] == '\n')
      {
        header_end = i + 1;
        break;
      }
    }

    if(header_end == 0 && used >= buf_sz - 1)
      return(false);
  }

  // Case-insensitive scan, bounded to the header block: a body that happens
  // to contain the literal "content-length:" must not be consulted.
  for(cl = NULL, i = 0; i + 15 <= header_end; i++)
  {
    if(strncasecmp(buf + i, "content-length:", 15) == 0)
    {
      cl = buf + i + 15;
      break;
    }
  }

  if(cl != NULL)
    content_len = (size_t)strtoul(cl, NULL, 10);

  if(content_len >= buf_sz - header_end)
    return(false);

  while(used < header_end + content_len)
  {
    const ssize_t n = read(fd, buf + used, buf_sz - 1 - used);

    if(n < 0)
    {
      if(errno == EINTR)
        continue;

      return(false);
    }

    if(n == 0)
      return(false);

    used += (size_t)n;
  }

  buf[header_end + content_len] = '\0';

  *body_off = header_end;
  *body_len = content_len;
  return(true);
}

static void
kokorod_handle_health(int fd)
{
  char body[128];
  const int n = snprintf(body, sizeof(body),
    "{\"ok\":true,\"voices\":%d}\n", kokorod_voice_count);

  if(n > 0 && (size_t)n < sizeof(body))
    kokorod_respond(fd, 200, "OK", "application/json", body, (size_t)n);
}

static void
kokorod_handle_speech(int fd, const SherpaOnnxOfflineTts *tts, const char *body)
{
  static char text[KOKOROD_TEXT_MAX];
  char voice[64];
  SherpaOnnxGenerationConfig gen;
  const SherpaOnnxGeneratedAudio *audio;
  struct timespec t0, t1;
  uint8_t *wav;
  size_t wav_len = 0;
  double speed = KOKOROD_SPEED_DEFAULT;
  double ms;
  int32_t sid;

  if(!kokorod_json_string(body, "input", text, sizeof(text)))
  {
    // Ambiguous by construction: the scanner fails the same way for a missing
    // key and for text longer than the cap, so say both.
    kokorod_respond_text(fd, 400, "Bad Request",
      "{\"error\":\"missing \\\"input\\\", or it exceeds 4096 bytes\"}\n");
    return;
  }

  if(text[0] == '\0')
  {
    kokorod_respond_text(fd, 400, "Bad Request",
      "{\"error\":\"\\\"input\\\" is empty\"}\n");
    return;
  }

  if(!kokorod_json_string(body, "voice", voice, sizeof(voice)))
    snprintf(voice, sizeof(voice), "%s", KOKOROD_VOICE_DEFAULT);

  if((sid = kokorod_voice_sid(voice)) < 0)
  {
    char err[512];
    int n = snprintf(err, sizeof(err),
      "{\"error\":\"unknown voice\",\"voice\":\"%.48s\",\"known\":\"%s, %s, %s, %s, … (%d total, see /health)\"}\n",
      voice, kokorod_voice_tbl[3], kokorod_voice_tbl[2], kokorod_voice_tbl[16],
      kokorod_voice_tbl[21], kokorod_voice_count);

    if(n > 0 && (size_t)n < sizeof(err))
      kokorod_respond(fd, 400, "Bad Request", "application/json", err, (size_t)n);

    return;
  }

  if(kokorod_json_number(body, "speed", &speed))
  {
    if(speed < 0.1 || speed > 5.0)
    {
      kokorod_respond_text(fd, 400, "Bad Request",
        "{\"error\":\"\\\"speed\\\" must be between 0.1 and 5.0\"}\n");
      return;
    }
  }

  memset(&gen, 0, sizeof(gen));
  gen.sid = sid;
  gen.speed = (float)speed;

  clock_gettime(CLOCK_MONOTONIC, &t0);
  audio = SherpaOnnxOfflineTtsGenerateWithConfig(tts, text, &gen, NULL, NULL);
  clock_gettime(CLOCK_MONOTONIC, &t1);

  ms = ((double)(t1.tv_sec - t0.tv_sec) * 1000.0) +
       ((double)(t1.tv_nsec - t0.tv_nsec) / 1000000.0);

  if(audio == NULL || audio->n <= 0)
  {
    if(audio != NULL)
      SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);

    kokorod_respond_text(fd, 500, "Internal Server Error",
      "{\"error\":\"synthesis produced no audio\"}\n");
    return;
  }

  wav = kokorod_wav_build(audio, &wav_len);

  fprintf(stderr, "kokorod: %s sid=%d %zu chars -> %.0f ms, %d samples @ %d Hz, %zu bytes\n",
    voice, sid, strlen(text), ms, audio->n, audio->sample_rate, wav_len);

  SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);

  if(wav == NULL)
  {
    kokorod_respond_text(fd, 500, "Internal Server Error",
      "{\"error\":\"out of memory building wav\"}\n");
    return;
  }

  kokorod_respond(fd, 200, "OK", "audio/wav", wav, wav_len);
  free(wav);
}

static void
kokorod_serve(int fd, const SherpaOnnxOfflineTts *tts)
{
  static char buf[KOKOROD_REQUEST_MAX];
  size_t body_off = 0;
  size_t body_len = 0;

  if(!kokorod_read_request(fd, buf, sizeof(buf), &body_off, &body_len))
  {
    kokorod_respond_text(fd, 400, "Bad Request",
      "{\"error\":\"malformed or oversized request\"}\n");
    return;
  }

  if(strncmp(buf, "POST /v1/audio/speech", 21) == 0)
  {
    kokorod_handle_speech(fd, tts, buf + body_off);
    return;
  }

  if(strncmp(buf, "GET /health", 11) == 0)
  {
    kokorod_handle_health(fd);
    return;
  }

  kokorod_respond_text(fd, 404, "Not Found",
    "{\"error\":\"try POST /v1/audio/speech or GET /health\"}\n");
}

int
main(int argc, char **argv)
{
  kokorod_cfg_t cfg;
  SherpaOnnxOfflineTtsConfig tts_cfg;
  const SherpaOnnxOfflineTts *tts;
  int listen_fd;
  int rc = EXIT_SUCCESS;
  int32_t speakers;

  if(!kokorod_parse_args(argc, argv, &cfg))
  {
    kokorod_usage(argv[0]);
    return(EXIT_FAILURE);
  }

  if(!kokorod_install_signals())
    return(EXIT_FAILURE);

  memset(&tts_cfg, 0, sizeof(tts_cfg));
  tts_cfg.model.kokoro.model = cfg.model;
  tts_cfg.model.kokoro.voices = cfg.voices;
  tts_cfg.model.kokoro.tokens = cfg.tokens;
  tts_cfg.model.kokoro.data_dir = cfg.data_dir;
  tts_cfg.model.kokoro.lexicon = cfg.lexicon;
  tts_cfg.model.kokoro.lang = cfg.lang;
  tts_cfg.model.kokoro.length_scale = 1.0f;
  tts_cfg.model.num_threads = cfg.threads;
  tts_cfg.model.provider = "cpu";
  tts_cfg.max_num_sentences = 1;

  if((tts = SherpaOnnxCreateOfflineTts(&tts_cfg)) == NULL)
  {
    fprintf(stderr, "kokorod: failed to load the Kokoro model — check --model, "
                    "--voices and --tokens\n");
    return(EXIT_FAILURE);
  }

  // A table that disagrees with the model means every sid past the divergence
  // selects the wrong voice, and nothing downstream can detect it. Refuse.
  speakers = SherpaOnnxOfflineTtsNumSpeakers(tts);

  if(speakers != kokorod_voice_count)
  {
    fprintf(stderr, "kokorod: voice table has %d entries but the model reports %d "
                    "speakers — refusing to serve the wrong voice\n",
            kokorod_voice_count, speakers);
    SherpaOnnxDestroyOfflineTts(tts);
    return(EXIT_FAILURE);
  }

  if((listen_fd = kokorod_listen(cfg.port)) < 0)
  {
    SherpaOnnxDestroyOfflineTts(tts);
    return(EXIT_FAILURE);
  }

  fprintf(stderr, "kokorod: listening on 127.0.0.1:%u — %d voices, %d threads, %d Hz\n",
    cfg.port, speakers, cfg.threads, SherpaOnnxOfflineTtsSampleRate(tts));

  while(kokorod_stop == 0)
  {
    struct timeval tv;
    int fd = accept(listen_fd, NULL, NULL);

    if(fd < 0)
    {
      if(errno == EINTR)
        continue;

      perror("kokorod: accept");
      rc = EXIT_FAILURE;
      break;
    }

    // The loop serves one request at a time, so a stalled client would
    // otherwise wedge every other caller behind it.
    tv.tv_sec = KOKOROD_CLIENT_TIMEOUT;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    kokorod_serve(fd, tts);
    close(fd);
  }

  fprintf(stderr, "kokorod: shutting down\n");
  close(listen_fd);
  SherpaOnnxDestroyOfflineTts(tts);

  return(rc);
}
