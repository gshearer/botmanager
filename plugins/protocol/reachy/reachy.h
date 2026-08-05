#ifndef BM_REACHY_H
#define BM_REACHY_H

// The Reachy Mini as a method: bind this driver to a text bot and the
// bot grows ears. Utterances segmented on the robot by `earbridge` are
// long-polled off its HTTP bridge, transcribed by the whisper host
// through the inference engine, gated by a per-bot attention policy and
// delivered into the ordinary chat pipeline — persona, memory, dossiers
// and speak-policy all unchanged. The robot is the same mind as the
// text bots, embodied.
//
// Both halves. The reply pipeline's send() queues a line for the mouth
// thread, which synthesizes it through the same inference engine, hands
// the WAV to the robot and paces itself against the audio's own
// duration — so a multi-line answer comes out as speech rather than on
// top of itself.
//
// Layering (PLUGIN.md §Layer Rules): PLUGIN_METHOD over the reachyapi
// service, and nothing else. This plugin must never reach up into the
// bot plugin (plugins/method/text/) — everything it knows about the
// conversation it learns through include/method.h.

#ifdef REACHY_INTERNAL

#include "alloc.h"
#include "bot.h"
#include "clam.h"
#include "common.h"
#include "curl.h"
#include "kv.h"
#include "method.h"
#include "plugin.h"
#include "pool.h"
#include "task.h"
#include "util.h"

// Both included WITHOUT their _INTERNAL guards: this mapping is not
// theirs, so every call lands on the header's dlsym shim.
#include "inference.h"
#include "reachyapi_api.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define REACHY_CTX "reachy"

// The ear's address. Owned by reachyapi's plugin-level schema alongside
// the robot's own base_url — one robot, addressed plugin-wide — and
// read from here because this driver is the only thing that polls it.
#define REACHY_KV_BRIDGE_URL "plugin.reachyapi.bridge_url"

// "bot." + a BOT_NAME_SZ name + ".reachy." never reaches 80, and saying
// so is what lets the compiler prove every derived key fits KV_KEY_SZ.
#define REACHY_KV_PREFIX_SZ 80

// The bridge is a host and a port; the poll URL is that plus a query
// string. Two sizes rather than one, for the same reason.
#define REACHY_BASE_SZ 256
#define REACHY_URL_SZ  512

// The long poll's server-side wait and the transport bound around it.
// The pair is this thread's worst-case tail on unload: core's
// `core.plugin.unload_quiesce_ms` defaults to 5000, and a 3 s poll
// inside a 5 s timeout keeps us at that budget rather than past it.
#define REACHY_POLL_WAIT_MS 3000
#define REACHY_POLL_TIMEOUT 5

// How long the ears thread waits on one poll's completion before
// abandoning it. Past the transport bound, because a waiter that gives
// up first costs a wasted round trip and proves nothing.
#define REACHY_WAIT_MS 9000

// Bounded waits are served in slices so the shutdown flag is observed
// promptly without a second condvar to broadcast on.
#define REACHY_WAIT_SLICE_MS 250

// Pause after a failed poll, and how many consecutive failures demote
// the instance from AVAILABLE back to RUNNING. Three is long enough to
// ride out one WiFi hiccup and short enough that a dead bridge stops
// pretending it can carry a conversation.
#define REACHY_BACKOFF_MS  2000
#define REACHY_FAIL_DEMOTE 3

// The ceiling earbridge enforces on one utterance: 25 s of 16 kHz mono
// S16LE plus a RIFF header, rounded up. A body past this did not come
// from the ear.
#define REACHY_WAV_MAX (25u * 32000u + 4096u)

// Live persist threads across every bound bot. Two slots per bot — ears
// and mouth — against a tree-wide task_add_persist pool of 16, so the
// array is generous, not tight.
#define REACHY_MAX_THREADS 32

// The mouth's queue. A speaker lagging minutes behind the room is worse
// than one that misses a line, so an overflow drops the OLDEST.
#define REACHY_SPEECH_RING 16

// "reachy_say_" + a BOT_NAME_SZ name + ".wav", which is what lands in
// the robot's sound directory.
#define REACHY_FILE_SZ 80

// Bounds on the mouth's sync-waits. Synthesis is the long one, with the
// engine's own timeout sitting just inside the waiter's patience so a
// slow speech host is reported by the engine rather than abandoned here.
// The robot's two REST hops are LAN round trips; the barge-in probe is
// a poll the mouth must never stall on.
#define REACHY_TTS_TIMEOUT   20
#define REACHY_TTS_WAIT_MS   22000
#define REACHY_ROBOT_WAIT_MS 15000
#define REACHY_DOA_WAIT_MS   2000

// Playback pacing. The window comes from the WAV's own header; these are
// the fallback when it is not the canonical 44-byte RIFF kokorod emits
// (24 kHz stereo S16LE — 96,000 bytes per second).
#define REACHY_WAV_BYTE_RATE 96000u
#define REACHY_WAV_HDR_SZ    44u

// Ceiling on one line's playback window. A minute is already an
// unreasonable single reply; past that the arithmetic is wrong rather
// than the speech long, and waiting it out would wedge the queue.
#define REACHY_PLAY_MAX_MS 60000

// Grace past the computed window, so the tail of a phrase is not clipped
// by the next line landing on top of it.
#define REACHY_PLAY_GRACE_MS 250

// The slice playback is slept in: the barge-in probe cadence, and how
// promptly a teardown is felt mid-sentence.
#define REACHY_PLAY_SLICE_MS 250

// Consecutive positive direction-of-arrival probes before barge-in
// believes a human has taken the floor. The flag it reads is noisy —
// see the measurement above reachy_barged() — and this run length is a
// guard against that noise, not a cure for it.
#define REACHY_BARGE_RUN 5

// How long stop() waits on one straggler before saying so. The worst
// thread tail is a long poll plus its transport bound, so this is that
// with room to spare.
#define REACHY_JOIN_MS 10000

// The sender identity every utterance carries when no userns handle is
// configured. Anonymous, and deliberately not a nick anyone could hold.
#define REACHY_ANON_SPEAKER "voice"

// One bound bot's robot.
//
// Refcounted rather than owned by the registry alone: destroy() runs
// under the non-recursive method_mutex and therefore may not join the
// threads that read this, and those threads may not take that mutex to
// ask whether they still exist. One reference for the registry, one for
// each live persist thread, one for each dispatch worker in flight; the
// last drop frees.
typedef struct
{
  char           inst_name[METHOD_NAME_SZ];  // "<bot>_reachy"
  char           botname  [BOT_NAME_SZ];
  char           kv_prefix[REACHY_KV_PREFIX_SZ];  // "bot.<bot>.reachy."

  method_inst_t *inst;      // resolved in connect(); NULL before it

  uint32_t       refs;      // atomic
  bool           shutdown;  // atomic

  // Bounded sleeps wake on this pair instead of sleeping out their
  // slice, so a disconnect is felt in milliseconds.
  pthread_mutex_t wake_mutex;
  pthread_cond_t  wake_cond;

  // Ears-thread-private: the bridge cursor, the AVAILABLE latch, and
  // the consecutive-failure run that demotes us back to RUNNING.
  uint64_t       seq;
  bool           available;
  uint32_t       fails;

  // name-mode attention window. Dispatch workers run concurrently with
  // one another, so this one field is the only shared mutable state
  // outside the ears thread.
  pthread_mutex_t attn_mutex;
  time_t          attn_until;

  // The mouth's queue. send() is called from the curl worker (one call
  // per streamed reply line) and from task workers, so every field here
  // is written under the mutex; the mouth thread is the only reader.
  // Sized once with the state rather than per line, because a speaking
  // robot must not be allocating.
  pthread_mutex_t mouth_mutex;
  pthread_cond_t  mouth_cond;
  char            mouth_line[REACHY_SPEECH_RING][METHOD_TEXT_SZ];
  uint32_t        mouth_head;     // index of the next line to speak
  uint32_t        mouth_count;
  uint64_t        mouth_dropped;
} reachy_state_t;

// One request in flight, shared between the thread waiting on it and the
// curl worker that completes it. Refcounted for the same reason the
// state is: the waiter may walk away — on shutdown, or when a completion
// overruns its patience — while the transfer is still airborne.
//
// One context for four completions: the bridge long-poll and the
// bridge's health probe (ears), and synthesis, upload, playback and the
// direction-of-arrival probe (mouth). They fill different fields of it;
// none of them fills all.
//
// Every completion callback lives in THIS mapping and is handed to core
// directly, which is what makes an unload safe: plugin_quiesce reads
// curl_iter_req_t.cb, sees a pointer into us, and delays the unmap until
// the transfer lands.
typedef struct
{
  pthread_mutex_t mutex;
  pthread_cond_t  cond;
  uint32_t        refs;      // atomic: the waiter + the completion
  bool            done;
  bool            ok;        // the completion's own verdict

  long            http;
  bool            transport;
  bool            have_seq;
  uint64_t        seq;
  double          doa;
  bool            speech;
  void           *wav;       // mem_alloc'd; the waiter takes ownership
  size_t          wav_len;
} reachy_wait_t;

// A transcription in flight. Holds a reference on the state for the
// whole trip, so nothing can free it under a worker that is mid-deliver.
typedef struct
{
  reachy_state_t *st;
  double          doa;
} reachy_stt_ctx_t;

// The transcript's ride off the curl worker thread. The chatbot observe
// path is not fast and must not run on a completion callback, so the
// attention gate and method_deliver both happen here, on a task worker.
typedef struct
{
  reachy_state_t *st;
  double          doa;
  char            text[METHOD_TEXT_SZ];
} reachy_dispatch_t;

// Standing the robot up at connect, across three completions on the
// curl worker thread: ask what the motors are doing, raise torque if
// they are down, then play the wake move.
//
// It carries a COPY of the name and nothing else — deliberately. The
// chain outlives no state, holds no reference and touches no instance,
// so a bot destroyed while the robot is still rising raises no lifetime
// question at all; the worst case is three log lines about a creature
// that has already gone.
typedef struct
{
  char botname[BOT_NAME_SZ];
} reachy_posture_t;

static bool reachy_thread_track(task_handle_t);
static void reachy_thread_forget(task_handle_t);

static void reachy_state_ref(reachy_state_t *);
static void reachy_state_unref(reachy_state_t *);
static bool reachy_stopping(const reachy_state_t *);
static void reachy_deadline(struct timespec *, uint32_t);
static bool reachy_expired(const struct timespec *);
static bool reachy_nap(reachy_state_t *, uint32_t);

static reachy_wait_t *reachy_wait_create(void);
static void reachy_wait_unref(reachy_wait_t *);
static bool reachy_wait_for(reachy_state_t *, reachy_wait_t *, uint32_t);

static void reachy_kv_copy(const reachy_state_t *, const char *, char *,
    size_t);
static uint64_t reachy_kv_uint(const reachy_state_t *, const char *);
static bool reachy_kv_flag(const reachy_state_t *, const char *);
static double reachy_kv_double(const reachy_state_t *, const char *);

static bool reachy_bridge_url(char *, size_t);
static void reachy_utt_done(const curl_response_t *);
static void reachy_health_done(const curl_response_t *);
static bool reachy_prime_seq(reachy_state_t *, const char *);
static void reachy_bridge_up(reachy_state_t *);
static void reachy_bridge_down(reachy_state_t *);

static char *reachy_word_in(char *, const char *);
static char *reachy_trim(char *);
static void reachy_alias_rewrite(reachy_state_t *, char *, size_t, char *,
    size_t);
static bool reachy_addressed(reachy_state_t *, char *, size_t);
static void reachy_deliver(reachy_state_t *, const reachy_dispatch_t *);
static void reachy_dispatch_task(task_t *);
static void reachy_stt_done(const llm_stt_response_t *);
static void reachy_transcribe(reachy_state_t *, void *, size_t, double);
static void reachy_ears(task_t *);

static void reachy_mouth_push(reachy_state_t *, const char *);
static bool reachy_mouth_take(reachy_state_t *, char *, size_t);
static void reachy_mouth_flush(reachy_state_t *);
static void reachy_mouth_wake(reachy_state_t *);

static void reachy_tts_done(const llm_tts_response_t *);
static void reachy_robot_done(const reachy_result_t *);
static void reachy_doa_done(const reachy_doa_t *);

static void reachy_sound_file(const reachy_state_t *, char *, size_t);
static bool reachy_synthesize(reachy_state_t *, const char *, void **,
    size_t *);
static bool reachy_upload(reachy_state_t *, const char *, const void *,
    size_t);
static bool reachy_play(reachy_state_t *, const char *);
static uint32_t reachy_play_ms(const void *, size_t);
static uint32_t reachy_lap(struct timespec *);
static bool reachy_barged(reachy_state_t *);
static bool reachy_pace(reachy_state_t *, uint32_t, bool);
static void reachy_speak(reachy_state_t *, const char *);
static void reachy_mouth(task_t *);

static void reachy_posture_woke(const reachy_result_t *);
static void reachy_posture_torque(const reachy_result_t *);
static void reachy_posture_probe(const reachy_robot_status_t *);
static void reachy_posture_wake(const reachy_state_t *);

static void *reachy_create(const char *);
static void reachy_destroy(void *);
static bool reachy_connect(void *);
static void reachy_disconnect(void *);
static bool reachy_send(void *, const char *, const char *);
static bool reachy_get_context(void *, const char *, char *, size_t);
static bool reachy_get_self(void *, char *, size_t);
static void reachy_list_joined_channels(void *, method_joined_channel_cb_t,
    void *);

static bool reachy_start(void);
static void reachy_count_bound_cb(const char *, const char *, const void *,
    void *);
static bool reachy_stop(void);
static bool reachy_suspend(void);
static bool reachy_resume(void);

#endif // REACHY_INTERNAL

#endif // BM_REACHY_H
