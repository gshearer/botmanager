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
// room, or a photograph of it, to whoever asks.
//
// It never opens hw:0,0. Capture goes through the dsnoop PCM the robot daemon
// also uses, so both read the microphone concurrently and /api/media/release
// is never called — face tracking and DoA keep working throughout. Recording
// from the default PCM instead would steal the dsnoop slave and break them.
//
// It is also the robot's eye, on the same terms: frames come from the robot
// daemon's camera IPC socket, the unixfd branch it shares with its own local
// consumers, so face tracking keeps running and nothing is released. The
// JPEG is made by gst-launch-1.0 as a CHILD process — GStreamer never enters
// this address space, so a camera fault costs one 503, never the ear.

int main(int, char **);

#ifdef EARBRIDGE_INTERNAL

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/types.h>

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

// The energy gate. The XVF3800's speech flag is the only thing telling us an
// utterance happened, and it is wrong far more often than it is right: over
// ten minutes of a silent room it produced 23 utterances, all of them the
// CM4's cooling fan, which whisper then transcribed into confident English
// ("Thank you." being the favourite). Level alone is what separates the two,
// so we measure it here rather than shipping the fan to a GPU.
//
// Measured on the robot 2026-08-04 (scripts/reachy_mic_probe.sh):
//
//   fan, worst 250 ms window          -42.0 dBFS
//   noisiest false utterance          -40.3 dBFS   (loudest 250 ms)
//   quietest real speech              -13.7 dBFS   (loudest 250 ms)
//
// -35 dBFS therefore clears the noisiest artefact by ~5 dB and sits ~21 dB
// under real speech. It is a starting point, not a law — the fan is NOT
// stationary (it drifted 22 dB inside eight minutes), so watch
// quiet_discards against utterances in /health and move it.
//
// A floor at or below -90 dBFS passes everything: no 16-bit sample can be
// quieter than one LSB, so that is how you turn the gate off.
#define EB_FLOOR_DB_DEFAULT (-35.0f)
#define EB_FLOOR_WIN_MS     250

#define EB_WAIT_MS_MAX      30000
#define EB_REQUEST_MAX      2048
#define EB_BACKLOG          8
#define EB_CLIENT_TIMEOUT   35
#define EB_VAD_TIMEOUT      2

// The eye. A frame takes ~0.2 s end to end (measured 2026-09-28, 1280x720),
// so the timeout is a hung pipeline, not a slow one. The largest mode the
// camera offers, 3840x2592, encodes to ~2 MB.
#define EB_CAMERA_SOCKET_DEFAULT "/tmp/reachymini_camera_socket"
#define EB_GST_LAUNCH            "/usr/bin/gst-launch-1.0"
#define EB_JPEG_QUALITY          85
#define EB_FRAME_TIMEOUT_MS      3000
#define EB_FRAME_MAX             (3u * 1024u * 1024u)

// Accepted connections a worker lane holds before refusing with a 503: a
// caller would rather hear "busy" now than an answer seconds stale.
#define EB_LANE_DEPTH            4

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
  float      floor_db;
  eb_take_t  take;
  char       camera_socket[108];   // sizeof sun_path
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
  atomic_ullong quiet_discards;   // never rose above --floor-db
  atomic_ullong clipped;
  atomic_ullong alsa_recovered;
  atomic_ullong alsa_reopened;
  atomic_ullong vad_polls;
  atomic_ullong vad_failures;
  atomic_ullong frames;
  atomic_ullong frame_failures;   // camera or pipeline, not a busy queue
  atomic_ullong lane_busy;        // refused: a worker lane already full
} eb_stats_t;

// One accepted connection a worker lane will answer. `after` and `wait_ms`
// are the /utt query; the frame lane ignores them.
typedef struct
{
  int      fd;
  uint32_t after;
  uint32_t wait_ms;
} eb_job_t;

// A lane is one worker thread answering its route's connections in order.
// The accept loop only reads a request line and files it, because the two
// slow routes are slow on purpose: a long-poll parks for seconds waiting
// for speech, and a frame asked for meanwhile must not wait behind it.
// Each route keeps one worker, so the send buffer /utt fills stays
// single-user and one capture never races another for the camera.
typedef struct
{
  eb_job_t        jobs[EB_LANE_DEPTH];
  uint32_t        head;
  uint32_t        count;
  pthread_mutex_t mtx;
  pthread_cond_t  cv;
} eb_lane_t;

extern char **environ;

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

// Threshold is mean-square per sample, not dB: the conversion happens once at
// startup so the hot path never calls log10 on a CM4.
static uint64_t eb_floor_thresh(float);
static bool     eb_loud_enough(const int16_t *, size_t, uint64_t);

static int   eb_vad_fetch(const eb_cfg_t *, float *, bool *);
static void *eb_vad_thread(void *);

static int   eb_listen(uint16_t);
static bool  eb_send_all(int, const void *, size_t);
static bool  eb_serve(int, const eb_cfg_t *);
static void  eb_handle_utt(int, uint32_t, uint32_t);
static void  eb_handle_health(int, const eb_cfg_t *);
static bool  eb_query_uint(const char *, const char *, uint32_t *);

static void  eb_refuse_busy(int);
static bool  eb_lane_push(eb_lane_t *, const eb_job_t *);
static bool  eb_lane_pop(eb_lane_t *, eb_job_t *);
static void  eb_lane_wake(eb_lane_t *);
static void *eb_utt_thread(void *);
static void *eb_frame_thread(void *);
static bool  eb_frame_capture(const eb_cfg_t *, uint8_t *, size_t *);
static void  eb_handle_frame(int, const eb_cfg_t *);

#endif // EARBRIDGE_INTERNAL
#endif // EARBRIDGE_H
