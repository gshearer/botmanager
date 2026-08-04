#ifndef EARBRIDGE_H
#define EARBRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// earbridge runs ON the robot's CM4, not on the botman host. It is the one
// piece of this initiative that lives outside the botman tree at runtime:
// raw microphone PCM never leaves the robot over REST, so something local has
// to segment speech and hand it up. It links only libasound and pthreads.
//
// SECURITY POSTURE: there is no authentication. The robot sits on an isolated
// VLAN and the only client is botman. Anything that changes either of those
// facts must add auth here first — this daemon will hand a recording of the
// room to whoever asks.
//
// It never opens hw:0,0. Capture goes through the dsnoop PCM the robot daemon
// also uses, so both read the microphone concurrently and /api/media/release
// is never called — face tracking and DoA keep working throughout. Recording
// from the default PCM instead would steal the dsnoop slave and break them.

int main(int, char **);

#ifdef EARBRIDGE_INTERNAL

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdatomic.h>

#define EB_PCM_DEVICE       "reachymini_audio_src"
#define EB_RATE             16000
#define EB_CHANNELS_IN      2
#define EB_PERIOD_FRAMES    320
#define EB_WAV_HEADER_SZ    44
#define EB_SLOTS            8

#define EB_PORT_DEFAULT     8090
#define EB_DAEMON_DEFAULT   "http://127.0.0.1:8000"
#define EB_PRE_MS_DEFAULT   400
#define EB_HANG_MS_DEFAULT  700
#define EB_MIN_MS_DEFAULT   250
#define EB_MAX_S_DEFAULT    25
#define EB_POLL_HZ_DEFAULT  10

#define EB_WAIT_MS_MAX      30000
#define EB_REQUEST_MAX      2048
#define EB_BACKLOG          8
#define EB_CLIENT_TIMEOUT   35
#define EB_VAD_TIMEOUT      2

typedef enum
{
  EB_TAKE_CH0 = 0,
  EB_TAKE_CH1,
  EB_TAKE_MIX
} eb_take_t;

typedef struct
{
  uint16_t   port;
  char       daemon_host[128];
  uint16_t   daemon_port;
  uint32_t   pre_ms;
  uint32_t   hang_ms;
  uint32_t   min_ms;
  uint32_t   max_s;
  uint32_t   poll_hz;
  eb_take_t  take;
} eb_cfg_t;

// One finalized utterance. wav is allocated once at startup to the maximum
// utterance size and reused forever — the ring drops oldest rather than
// allocating, so a talkative room cannot grow this process without bound.
typedef struct
{
  uint32_t seq;                   // 0 means "never filled"
  float    doa;                   // radians, latched when the utterance OPENED
  uint32_t ms;
  bool     clipped;               // hit --max-s and was cut
  bool     delivered;             // served at least once; distinguishes a
                                  // recycled slot from one nobody ever read
  uint8_t *wav;
  size_t   wav_len;
} eb_slot_t;

typedef struct
{
  atomic_ullong utterances;
  atomic_ullong dropped;          // ring overwrote one before a client read it
  atomic_ullong short_discards;   // shorter than --min-ms
  atomic_ullong clipped;
  atomic_ullong alsa_recovered;
  atomic_ullong alsa_reopened;
  atomic_ullong vad_polls;
  atomic_ullong vad_failures;
} eb_stats_t;

static void  eb_usage(const char *);
static bool  eb_parse_args(int, char **, eb_cfg_t *);
static bool  eb_parse_daemon_url(const char *, eb_cfg_t *);
static void  eb_on_signal(int);
static bool  eb_install_signals(void);

static void  eb_wav_put32(uint8_t *, uint32_t);
static void  eb_wav_put16(uint8_t *, uint16_t);
static void  eb_wav_header(uint8_t *, size_t);

static snd_pcm_t *eb_pcm_open(void);
static void *eb_capture_thread(void *);
static void  eb_publish(const int16_t *, size_t, float, bool);

static int   eb_vad_fetch(const eb_cfg_t *, float *, bool *);
static void *eb_vad_thread(void *);

static int   eb_listen(uint16_t);
static bool  eb_send_all(int, const void *, size_t);
static void  eb_serve(int, const eb_cfg_t *);
static void  eb_handle_utt(int, uint32_t, uint32_t);
static void  eb_handle_health(int, const eb_cfg_t *);
static bool  eb_query_uint(const char *, const char *, uint32_t *);

#endif // EARBRIDGE_INTERNAL
#endif // EARBRIDGE_H
