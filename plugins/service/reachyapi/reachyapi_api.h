#ifndef BM_REACHYAPI_API_H
#define BM_REACHYAPI_API_H

// Public mechanism API for the reachyapi service plugin — the botman
// side of the Reachy Mini daemon's REST surface (motion, the emotion
// move library, motors, volume, sound upload/playback, face tracking,
// head wobbling, direction-of-arrival). Consumers include this header
// and resolve the exported symbols at runtime via
// plugin_dlsym("reachyapi", …) — the plugin is loaded RTLD_LOCAL.
//
// Three include modes:
//   - default: types + static-inline dlsym shims (abort on plugin miss).
//     Use this in command-surface or driver plugins that consider a
//     missing service plugin a startup misconfiguration.
//   - REACHYAPI_INTERNAL: types + real prototypes, no shims. Used by the
//     plugin's own translation units, which link against the definitions
//     directly rather than dlsym-ing themselves.
//   - REACHYAPI_TYPES_ONLY: types only. For consumers that want to
//     handle "plugin not loaded" gracefully through their own dlsym path.
//
// Async contract, uniform across every entry point below:
//   SUCCESS ⇒ the request is queued and the callback fires exactly once,
//             on the curl worker thread.
//   FAIL    ⇒ the callback was NOT invoked and never will be.
// (`PLUGIN.md §Async failure semantics`. Remember SUCCESS == false and
// FAIL == true — compare with `== SUCCESS`, never truthiness.)
// Callbacks run on the curl worker thread: be fast, take no lock a slow
// path also takes, and offload real work via task_add(). A NULL `cb` is
// legal and means fire-and-forget — the request is still submitted, and
// SUCCESS still means it was queued.
//
// The one exception to "exactly once", and it is not one a caller can
// act on: if the CALLING plugin is unloaded while its request is still
// airborne, the callback is dropped rather than fired into a mapping
// that no longer exists. Whatever was handed in as `user_data` is then
// leaked — only the caller could have freed it, and the caller is what
// went away. A plugin that wants its contexts accounted for on unload
// must not have live requests at that point.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"  // SUCCESS/FAIL

// The plugin's name, and so both its dlsym handle and the root of its
// clam contexts. Public because the shims below resolve against it.
#define REACHYAPI_CTX "reachyapi"

// Outcome of a completed call. Anything other than REACHY_OK means the
// payload fields are unset.
typedef enum
{
  REACHY_OK = 0,
  REACHY_TRANSPORT,   // curl-level failure — robot unreachable, timeout
  REACHY_HTTP,        // the daemon answered, but not with 2xx
  REACHY_MALFORMED    // 2xx whose body was not the JSON we expect
} reachy_status_t;

static inline const char *
reachy_status_str(reachy_status_t s)
{
  switch(s)
  {
    case REACHY_OK:        return("ok");
    case REACHY_TRANSPORT: return("robot unreachable");
    case REACHY_HTTP:      return("robot refused the request");
    case REACHY_MALFORMED: return("malformed response");
  }

  return("unknown error");
}

// Completion payload for the fire-and-forget calls (play, upload,
// volume, tracking, wobbling, motors). `http` is the raw status code,
// 0 when the transfer never reached the daemon.
typedef struct
{
  reachy_status_t status;
  long            http;
  void           *user_data;
} reachy_result_t;

typedef void (*reachy_done_cb_t)(const reachy_result_t *r);

// GET /api/state/doa — the XVF3800 microphone array's direction of
// arrival, served by the daemon from a persistent USB handle. `speech`
// is the array's hardware voice-activity flag.
typedef struct
{
  reachy_status_t status;
  double          angle;   // radians; array frame: 0 = left, pi/2 = front
  bool            speech;
  void           *user_data;
} reachy_doa_t;

typedef void (*reachy_doa_cb_t)(const reachy_doa_t *r);

// Motor control mode, long enough for "gravity_compensation".
#define REACHY_MOTOR_MODE_SZ 24

// GET /api/daemon/status, flattened. `face_x` / `face_y` are normalized
// to [-1,1] and meaningful only while `face_detected` — the daemon sends
// nulls when it has no target, and those land here as zeroes.
//
// `motors` is "enabled", "disabled" or "gravity_compensation", and is
// empty only if the daemon stopped reporting it. It is lifted out of
// `backend_status.motor_control_mode`, which was measured character-for
// -character equal to what GET /api/motors/status serves across all
// three modes on 2026-08-04 — so the mode rides along in a response the
// caller was already making, and no caller needs a second round trip to
// find out whether the robot has any torque in it.
typedef struct
{
  reachy_status_t status;
  char            state[24];
  bool            media_released;
  bool            no_media;
  char            motors[REACHY_MOTOR_MODE_SZ];
  bool            face_detected;
  double          face_x;
  double          face_y;
  char            version[16];
  void           *user_data;
} reachy_robot_status_t;

typedef void (*reachy_status_cb_t)(const reachy_robot_status_t *r);

// The emotion library's move names are short, lowercase, and stable;
// the library shipped with daemon 1.9.0 holds 84 of them.
#define REACHY_MOVE_NAME_SZ 96
#define REACHY_MOVE_MAX     128

// `moves` is valid for the duration of the callback only — copy out
// anything you keep.
typedef struct
{
  reachy_status_t status;
  const char    (*moves)[REACHY_MOVE_NAME_SZ];
  size_t          n_moves;
  void           *user_data;
} reachy_moves_t;

typedef void (*reachy_moves_cb_t)(const reachy_moves_t *r);

// Real declarations — visible only inside the reachyapi plugin.
// External consumers go through the static-inline dlsym shims below.
#ifdef REACHYAPI_INTERNAL

bool reachy_get_status(reachy_status_cb_t cb, void *user_data);
bool reachy_get_doa(reachy_doa_cb_t cb, void *user_data);

// The emotion library, by bare move name. Names are validated against
// the library's alphabet ([a-z0-9_-]) before they are spliced into the
// URL; anything else is refused with FAIL.
bool reachy_list_moves(reachy_moves_cb_t cb, void *user_data);
bool reachy_play_move(const char *move, reachy_done_cb_t cb,
    void *user_data);

// ⚠ The daemon is ASYMMETRIC about torque, measured 2026-08-04:
// `goto_sleep` disables the motors itself once the move finishes, but
// `wake_up` never enables them. So waking is two calls — motors first,
// the move second — while sleeping is one. A wake_up sent to a limp
// robot is accepted, returns a uuid, "completes", and moves nothing;
// there is no error on any surface (`4ef0cef`).
//
// Both return as soon as the daemon has ACCEPTED the move (~13 ms). The
// motion itself takes ~2.5 s and is visible in GET /api/move/running
// until it ends — a completion here is not a robot that has arrived.
bool reachy_wake(reachy_done_cb_t cb, void *user_data);
bool reachy_sleep_move(reachy_done_cb_t cb, void *user_data);

// mode: "enabled", "disabled" or "gravity_compensation". Read the
// current one from reachy_get_status()'s `motors`.
bool reachy_motors_mode(const char *mode, reachy_done_cb_t cb,
    void *user_data);

// pct is clamped to [0,100] by the daemon's own schema; anything above
// 100 is refused here rather than round-tripped for a 422.
bool reachy_set_volume(uint8_t pct, reachy_done_cb_t cb, void *user_data);

// `wav` is copied into the multipart body before submit — the caller
// keeps ownership and may free it as soon as this returns. `filename`
// lands in /tmp/reachy_mini_sounds/ on the robot and overwrites any
// same-named file, which is what makes a fixed per-bot name the right
// choice for a speech path.
bool reachy_upload_sound(const char *filename, const void *wav,
    size_t wav_len, reachy_done_cb_t cb, void *user_data);

bool reachy_play_sound(const char *filename, reachy_done_cb_t cb,
    void *user_data);
bool reachy_stop_sound(reachy_done_cb_t cb, void *user_data);

// Daemon-side face tracking. `weight` is the tracking strength in
// [0,1] and is ignored when `on` is false.
bool reachy_tracking(bool on, double weight, reachy_done_cb_t cb,
    void *user_data);

// Head wobbling: with it enabled, ANY daemon-played audio drives
// speech-synced head motion.
bool reachy_wobbling(bool on, reachy_done_cb_t cb, void *user_data);

#endif // REACHYAPI_INTERNAL

#if !defined(REACHYAPI_INTERNAL) && !defined(REACHYAPI_TYPES_ONLY)

#include "clam.h"
#include "plugin.h"

#include <stdlib.h>  // abort

static inline bool
reachy_get_status(reachy_status_cb_t cb, void *user_data)
{
  typedef bool (*fn_t)(reachy_status_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(REACHYAPI_CTX, "reachy_get_status",
        (void **)&cached);

    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, REACHYAPI_CTX, "dlsym failed: reachy_get_status");
      abort();
    }

    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }

  return(fn(cb, user_data));
}

static inline bool
reachy_get_doa(reachy_doa_cb_t cb, void *user_data)
{
  typedef bool (*fn_t)(reachy_doa_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(REACHYAPI_CTX, "reachy_get_doa",
        (void **)&cached);

    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, REACHYAPI_CTX, "dlsym failed: reachy_get_doa");
      abort();
    }

    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }

  return(fn(cb, user_data));
}

static inline bool
reachy_list_moves(reachy_moves_cb_t cb, void *user_data)
{
  typedef bool (*fn_t)(reachy_moves_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(REACHYAPI_CTX, "reachy_list_moves",
        (void **)&cached);

    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, REACHYAPI_CTX, "dlsym failed: reachy_list_moves");
      abort();
    }

    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }

  return(fn(cb, user_data));
}

static inline bool
reachy_play_move(const char *move, reachy_done_cb_t cb, void *user_data)
{
  typedef bool (*fn_t)(const char *, reachy_done_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(REACHYAPI_CTX, "reachy_play_move",
        (void **)&cached);

    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, REACHYAPI_CTX, "dlsym failed: reachy_play_move");
      abort();
    }

    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }

  return(fn(move, cb, user_data));
}

static inline bool
reachy_wake(reachy_done_cb_t cb, void *user_data)
{
  typedef bool (*fn_t)(reachy_done_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(REACHYAPI_CTX, "reachy_wake",
        (void **)&cached);

    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, REACHYAPI_CTX, "dlsym failed: reachy_wake");
      abort();
    }

    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }

  return(fn(cb, user_data));
}

static inline bool
reachy_sleep_move(reachy_done_cb_t cb, void *user_data)
{
  typedef bool (*fn_t)(reachy_done_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(REACHYAPI_CTX, "reachy_sleep_move",
        (void **)&cached);

    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, REACHYAPI_CTX, "dlsym failed: reachy_sleep_move");
      abort();
    }

    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }

  return(fn(cb, user_data));
}

static inline bool
reachy_motors_mode(const char *mode, reachy_done_cb_t cb, void *user_data)
{
  typedef bool (*fn_t)(const char *, reachy_done_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(REACHYAPI_CTX, "reachy_motors_mode",
        (void **)&cached);

    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, REACHYAPI_CTX, "dlsym failed: reachy_motors_mode");
      abort();
    }

    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }

  return(fn(mode, cb, user_data));
}

static inline bool
reachy_set_volume(uint8_t pct, reachy_done_cb_t cb, void *user_data)
{
  typedef bool (*fn_t)(uint8_t, reachy_done_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(REACHYAPI_CTX, "reachy_set_volume",
        (void **)&cached);

    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, REACHYAPI_CTX, "dlsym failed: reachy_set_volume");
      abort();
    }

    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }

  return(fn(pct, cb, user_data));
}

static inline bool
reachy_upload_sound(const char *filename, const void *wav, size_t wav_len,
    reachy_done_cb_t cb, void *user_data)
{
  typedef bool (*fn_t)(const char *, const void *, size_t,
      reachy_done_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(REACHYAPI_CTX, "reachy_upload_sound",
        (void **)&cached);

    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, REACHYAPI_CTX, "dlsym failed: reachy_upload_sound");
      abort();
    }

    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }

  return(fn(filename, wav, wav_len, cb, user_data));
}

static inline bool
reachy_play_sound(const char *filename, reachy_done_cb_t cb, void *user_data)
{
  typedef bool (*fn_t)(const char *, reachy_done_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(REACHYAPI_CTX, "reachy_play_sound",
        (void **)&cached);

    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, REACHYAPI_CTX, "dlsym failed: reachy_play_sound");
      abort();
    }

    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }

  return(fn(filename, cb, user_data));
}

static inline bool
reachy_stop_sound(reachy_done_cb_t cb, void *user_data)
{
  typedef bool (*fn_t)(reachy_done_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(REACHYAPI_CTX, "reachy_stop_sound",
        (void **)&cached);

    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, REACHYAPI_CTX, "dlsym failed: reachy_stop_sound");
      abort();
    }

    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }

  return(fn(cb, user_data));
}

static inline bool
reachy_tracking(bool on, double weight, reachy_done_cb_t cb, void *user_data)
{
  typedef bool (*fn_t)(bool, double, reachy_done_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(REACHYAPI_CTX, "reachy_tracking",
        (void **)&cached);

    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, REACHYAPI_CTX, "dlsym failed: reachy_tracking");
      abort();
    }

    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }

  return(fn(on, weight, cb, user_data));
}

static inline bool
reachy_wobbling(bool on, reachy_done_cb_t cb, void *user_data)
{
  typedef bool (*fn_t)(bool, reachy_done_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(REACHYAPI_CTX, "reachy_wobbling",
        (void **)&cached);

    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, REACHYAPI_CTX, "dlsym failed: reachy_wobbling");
      abort();
    }

    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }

  return(fn(on, cb, user_data));
}

#endif // !REACHYAPI_INTERNAL && !REACHYAPI_TYPES_ONLY

#endif // BM_REACHYAPI_API_H
