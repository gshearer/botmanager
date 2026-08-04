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
// The inbound half only. `send()` is a stub here: giving the reply a
// voice is RCH-7's mouth loop, which extends this same file.
//
// Layering (PLUGIN.md §Layer Rules): PLUGIN_PROTOCOL over the
// reachyapi service, and nothing else. This plugin must never reach up
// into plugins/method/ — everything it knows about the conversation it
// learns through include/method.h.

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

// Live persist threads across every bound bot. RCH-6 spends one slot
// per bot (ears); RCH-7's mouth makes it two, against a tree-wide
// task_add_persist pool of 16 — so the array is generous, not tight.
#define REACHY_MAX_THREADS 32

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
} reachy_state_t;

// One bridge long-poll, shared between the ears thread and the curl
// worker that completes it. Refcounted for the same reason the state
// is: the waiter may walk away — on shutdown, or when a completion
// overruns REACHY_WAIT_MS — while the transfer is still airborne.
//
// The completion callback lives in THIS mapping and is handed to core's
// curl layer directly, which is what makes an unload safe: plugin_quiesce
// reads curl_iter_req_t.cb, sees a pointer into us, and delays the
// unmap until the transfer lands.
typedef struct
{
  pthread_mutex_t mutex;
  pthread_cond_t  cond;
  uint32_t        refs;      // atomic: the waiter + the completion
  bool            done;

  long            http;
  bool            transport;
  bool            have_seq;
  uint64_t        seq;
  double          doa;
  void           *wav;       // mem_alloc'd; the waiter takes ownership
  size_t          wav_len;
} reachy_poll_t;

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

static bool reachy_thread_track(task_handle_t);
static void reachy_thread_forget(task_handle_t);

static void reachy_state_ref(reachy_state_t *);
static void reachy_state_unref(reachy_state_t *);
static bool reachy_stopping(const reachy_state_t *);
static void reachy_deadline(struct timespec *, uint32_t);
static bool reachy_expired(const struct timespec *);
static bool reachy_nap(reachy_state_t *, uint32_t);

static reachy_poll_t *reachy_poll_create(void);
static void reachy_poll_unref(reachy_poll_t *);
static bool reachy_poll_wait(reachy_state_t *, reachy_poll_t *);

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

static bool reachy_word_in(const char *, const char *);
static char *reachy_trim(char *);
static bool reachy_addressed(reachy_state_t *, const char *);
static void reachy_deliver(reachy_state_t *, const reachy_dispatch_t *);
static void reachy_dispatch_task(task_t *);
static void reachy_stt_done(const llm_stt_response_t *);
static void reachy_transcribe(reachy_state_t *, void *, size_t, double);
static void reachy_ears(task_t *);

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

#endif // REACHY_INTERNAL

#endif // BM_REACHY_H
