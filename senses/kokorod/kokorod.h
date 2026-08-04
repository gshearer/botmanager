#ifndef KOKOROD_H
#define KOKOROD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// kokorod is a standalone daemon, not a plugin: it links no botman core, uses
// plain malloc, and logs to stderr. Its only contract with the tree is the
// HTTP surface on :8003, which mirrors OpenAI's /v1/audio/speech so the
// inference engine's `tts` kind (RCH-4) can speak to it with no special case.

int main(int, char **);

#ifdef KOKOROD_INTERNAL

#include <sherpa-onnx/c-api/c-api.h>

#define KOKOROD_PORT_DEFAULT    8003
#define KOKOROD_THREADS_DEFAULT 4
#define KOKOROD_VOICE_DEFAULT   "af_heart"
#define KOKOROD_SPEED_DEFAULT   1.0

#define KOKOROD_TEXT_MAX        4096
#define KOKOROD_REQUEST_MAX     (64u * 1024u)
#define KOKOROD_BACKLOG         8
#define KOKOROD_CLIENT_TIMEOUT  10

// The output stage exists to satisfy two facts measured against the real
// robot on 2026-08-03 (charter §C3), and both are load-bearing:
//
//   Mono is SILENTLY unplayable. The robot's ALSA sink is a dmix bound to
//   exactly two channels; a mono upload is accepted, play_sound answers 200,
//   and no sound is produced and no error is logged anywhere. So every
//   response is interleaved to stereo even though Kokoro generates mono.
//
//   Kokoro is quiet. Raw output peaks near -12 dBFS, which the robot's own
//   motors drown out. Each utterance is peak-normalised to -3 dBFS, leaving
//   3 dB of margin so a transient cannot clip.
#define KOKOROD_CHANNELS        2
#define KOKOROD_PEAK_TARGET     0.708f
#define KOKOROD_PEAK_EPSILON    1e-6f
#define KOKOROD_WAV_HEADER_SZ   44

typedef struct
{
  uint16_t    port;
  const char *model;
  const char *voices;
  const char *tokens;
  const char *data_dir;
  const char *lexicon;
  const char *lang;
  int32_t     threads;
} kokorod_cfg_t;

// Speaker id IS the index into this table — that is the sherpa-onnx contract
// for Kokoro, not a convention of ours, so the order may never be permuted.
// Verified against the model at startup; a mismatch is fatal rather than a
// warning, because the failure mode is the robot speaking in the wrong voice
// with nothing logged anywhere.
extern const char *const kokorod_voice_tbl[];
extern const int32_t     kokorod_voice_count;

static void  kokorod_usage(const char *);
static bool  kokorod_parse_args(int, char **, kokorod_cfg_t *);
static void  kokorod_on_signal(int);
static bool  kokorod_install_signals(void);

static int32_t kokorod_voice_sid(const char *);

static bool  kokorod_json_string(const char *, const char *, char *, size_t);
static bool  kokorod_json_number(const char *, const char *, double *);
static size_t kokorod_utf8_encode(uint32_t, char *);

static uint8_t *kokorod_wav_build(const SherpaOnnxGeneratedAudio *, size_t *);
static void  kokorod_wav_put32(uint8_t *, uint32_t);
static void  kokorod_wav_put16(uint8_t *, uint16_t);

static int   kokorod_listen(uint16_t);
static bool  kokorod_send_all(int, const void *, size_t);
static void  kokorod_respond(int, int, const char *, const char *, const void *, size_t);
static void  kokorod_respond_text(int, int, const char *, const char *);
static bool  kokorod_read_request(int, char *, size_t, size_t *, size_t *);
static void  kokorod_serve(int, const SherpaOnnxOfflineTts *);
static void  kokorod_handle_speech(int, const SherpaOnnxOfflineTts *, const char *);
static void  kokorod_handle_health(int);

#endif // KOKOROD_INTERNAL
#endif // KOKOROD_H
