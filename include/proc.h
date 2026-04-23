#ifndef BM_PROC_H
#define BM_PROC_H

#include <stddef.h>

// Generic subprocess primitive.
//
// Spawns an external program, merges its stdout + stderr into a single
// growable byte buffer (capped to avoid unbounded captures), and fires
// a one-shot completion callback from a dedicated waiter thread once
// the child has exited and the reader has drained its stdout.
//
// Intended for plugin-kind-agnostic use (claude bot kind, ninja/git
// helpers, acquire-engine shells). Not safe to call from early daemon
// init — the timeout watchdog requires the task subsystem to be up.

#define PROC_STDOUT_CAP_DEFAULT ((size_t)65536)

typedef struct proc_handle proc_handle_t;

// Completion callback fired exactly once from the waiter thread after
// the child has exited AND the reader thread has fully drained stdout.
// status    : WEXITSTATUS when !signalled (0..255); signal number when signalled.
// signalled : non-zero if the child was terminated by a signal.
// stdout_buf: owned by the handle, valid until proc_free() is called.
// stdout_len: bytes captured (may be less than total written if cap hit).
// user      : opaque pointer copied from spec->user.
typedef void (*proc_exit_cb_t)(int status, int signalled,
    const char *stdout_buf, size_t stdout_len, void *user);

typedef struct
{
  const char *const *argv;        // NULL-terminated
  const char        *cwd;         // NULL = inherit
  const char *const *envp;        // NULL = inherit parent env
  size_t             stdout_cap;  // 0 = PROC_STDOUT_CAP_DEFAULT
  unsigned           timeout_sec; // 0 = no watchdog
  proc_exit_cb_t     on_exit;     // required
  void              *user;
} proc_spec_t;

// Spawn a subprocess. Returns a handle on success (child is running,
// reader and waiter threads spawned); NULL on immediate failure
// (pipe/fork/thread-create) with errno preserved where possible.
// The on_exit callback fires exactly once, from the waiter thread,
// after both the reader has finished draining stdout and waitpid has
// returned. The stdout_buf is owned by the handle and remains valid
// until proc_free() is called.
proc_handle_t *proc_spawn(const proc_spec_t *spec);

// Send a signal to the child. Returns 0 on success, -1 on failure
// (errno set). No-op after on_exit has fired.
int proc_kill(proc_handle_t *h, int sig);

// Joins reader + waiter threads (safe to call before on_exit
// completes — will block). Closes fds. Frees buffer. Do not double-free.
void proc_free(proc_handle_t *h);

#ifdef PROC_INTERNAL

#include "common.h"
#include "alloc.h"
#include "clam.h"
#include "task.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#endif // PROC_INTERNAL

#endif
