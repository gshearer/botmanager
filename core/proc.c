// botmanager — MIT
// Process spawn + capture helpers (pipe2 + execvpe + timeout-kill).
#define _GNU_SOURCE  // pipe2/execvpe are GNU; must precede any system header
                    // pulled in transitively via proc.h.

#define PROC_INTERNAL
#include "proc.h"

// Constants

// Seconds between SIGTERM and the SIGKILL escalation for a timed-out
// child. Matches the SIGTERM-before-SIGKILL convention in AGENTS.md.
#define PROC_TIMEOUT_KILL_DELAY_SEC  5

// Stack-local read buffer size for the reader thread.
#define PROC_READ_CHUNK              4096

// Initial heap size of the growable capture buffer. Doubles on demand
// up to h->buf_cap.
#define PROC_BUF_INITIAL             4096

// Log tag used for every clam() call in this module.
#define PROC_CTX                     "proc"

// Handle

struct proc_handle
{
  pid_t             pid;
  int               stdout_fd;     // parent's read end; -1 after close
  pthread_t         reader_tid;
  pthread_t         waiter_tid;

  // Recorded by the waiter thread just before invoking on_exit; lets
  // proc_free() detect the "called from inside on_exit on the waiter
  // thread" case and pthread_detach instead of pthread_join (which
  // would self-deadlock).
  pthread_t         waiter_self;
  bool              waiter_self_set;

  pthread_mutex_t   lock;          // guards buf/status/signalled/exited/fd
  char             *buf;           // mem_alloc'd stdout capture
  size_t            buf_cap;       // max capture size (bytes)
  size_t            buf_sz;        // current heap allocation
  size_t            buf_len;       // bytes actually captured
  int               status;        // exit status when !signalled; signal num otherwise
  bool              signalled;
  bool              exited;        // waitpid returned
  bool              reader_done;
  proc_exit_cb_t    on_exit;
  void             *user;
  unsigned          timeout_sec;
};

// Forward declarations (statics)

static void *proc_reader(void *arg);
static void *proc_waiter(void *arg);
static void  proc_timeout_cb(task_t *t);
static void  proc_kill_cb(task_t *t);

// Reader thread

static void *
proc_reader(void *arg)
{
  proc_handle_t *h = arg;
  char           chunk[PROC_READ_CHUNK];

  for(;;)
  {
    ssize_t n = read(h->stdout_fd, chunk, sizeof(chunk));

    if(n > 0)
    {
      pthread_mutex_lock(&h->lock);

      if(h->buf_len < h->buf_cap)
      {
        size_t space = h->buf_cap - h->buf_len;
        size_t take  = (size_t)n < space ? (size_t)n : space;

        // Grow the heap allocation as needed (double up to buf_cap).
        if(h->buf_len + take > h->buf_sz)
        {
          size_t new_sz = h->buf_sz;

          while(new_sz < h->buf_len + take)
            new_sz *= 2;

          if(new_sz > h->buf_cap)
            new_sz = h->buf_cap;

          h->buf    = mem_realloc(h->buf, new_sz);
          h->buf_sz = new_sz;
        }

        memcpy(h->buf + h->buf_len, chunk, take);
        h->buf_len += take;
      }

      // Past cap: drop silently; keep reading so the child doesn't block.
      pthread_mutex_unlock(&h->lock);
      continue;
    }

    if(n == 0)
      break;  // EOF

    if(errno == EINTR)
      continue;

    clam(CLAM_WARN, PROC_CTX, "read failed pid=%d errno=%d (%s)",
        (int)h->pid, errno, strerror(errno));
    break;
  }

  pthread_mutex_lock(&h->lock);
  h->reader_done = true;

  if(h->stdout_fd >= 0)
  {
    close(h->stdout_fd);
    h->stdout_fd = -1;
  }

  pthread_mutex_unlock(&h->lock);

  return(NULL);
}

// Waiter thread

static void *
proc_waiter(void *arg)
{
  proc_handle_t *h = arg;
  int            ws = 0;

  for(;;)
  {
    pid_t rc = waitpid(h->pid, &ws, 0);

    if(rc == h->pid)
      break;

    if(rc == -1 && errno == EINTR)
      continue;

    clam(CLAM_WARN, PROC_CTX, "waitpid failed pid=%d errno=%d (%s)",
        (int)h->pid, errno, strerror(errno));
    break;
  }

  pthread_mutex_lock(&h->lock);

  if(WIFEXITED(ws))
  {
    h->status    = WEXITSTATUS(ws);
    h->signalled = false;
  }

  else if(WIFSIGNALED(ws))
  {
    h->status    = WTERMSIG(ws);
    h->signalled = true;
  }

  else
  {
    // Shouldn't happen for a blocking waitpid with default options.
    h->status    = 0;
    h->signalled = false;
  }

  h->exited = true;
  pthread_mutex_unlock(&h->lock);

  // Reader must finish before on_exit sees the buffer. No lock held
  // during the join, and the reader owns the only write path to buf.
  pthread_join(h->reader_tid, NULL);

  clam(CLAM_INFO, PROC_CTX,
      "exited pid=%d %s=%d stdout=%zu bytes%s",
      (int)h->pid,
      h->signalled ? "signal" : "status",
      h->status,
      h->buf_len,
      h->buf_len >= h->buf_cap ? " (capped)" : "");

  // Record own tid under lock so proc_free() can spot the
  // called-from-on_exit case and detach instead of join.
  pthread_mutex_lock(&h->lock);
  h->waiter_self     = pthread_self();
  h->waiter_self_set = true;
  pthread_mutex_unlock(&h->lock);

  h->on_exit(h->status, h->signalled ? 1 : 0, h->buf, h->buf_len, h->user);

  return(NULL);
}

// Timeout watchdog

static void
proc_timeout_cb(task_t *t)
{
  proc_handle_t *h = t->data;
  bool           alive;
  pid_t          pid;

  pthread_mutex_lock(&h->lock);
  alive = !h->exited;
  pid   = h->pid;
  pthread_mutex_unlock(&h->lock);

  if(alive)
  {
    clam(CLAM_WARN, PROC_CTX,
        "timeout: SIGTERM pid=%d after %us", (int)pid, h->timeout_sec);

    if(kill(pid, SIGTERM) != 0)
      clam(CLAM_WARN, PROC_CTX,
          "SIGTERM failed pid=%d errno=%d (%s)",
          (int)pid, errno, strerror(errno));

    task_add_deferred("proc_kill", TASK_ANY, 200,
        (uint32_t)PROC_TIMEOUT_KILL_DELAY_SEC * 1000, proc_kill_cb, h);
  }

  t->state = TASK_ENDED;
}

static void
proc_kill_cb(task_t *t)
{
  proc_handle_t *h = t->data;
  bool           alive;
  pid_t          pid;

  pthread_mutex_lock(&h->lock);
  alive = !h->exited;
  pid   = h->pid;
  pthread_mutex_unlock(&h->lock);

  if(alive)
  {
    clam(CLAM_WARN, PROC_CTX,
        "timeout: SIGKILL pid=%d (SIGTERM ignored)", (int)pid);

    if(kill(pid, SIGKILL) != 0)
      clam(CLAM_WARN, PROC_CTX,
          "SIGKILL failed pid=%d errno=%d (%s)",
          (int)pid, errno, strerror(errno));
  }

  t->state = TASK_ENDED;
}

// Spawn

// Spawn a subprocess per the spec. See include/proc.h for semantics.
// spec: required; argv[0] must be a program to execvp / execvpe.
proc_handle_t *
proc_spawn(const proc_spec_t *spec)
{
  proc_handle_t *h;
  int            pfd[2] = { -1, -1 };
  int            saved_errno;
  pid_t          pid;

  if(spec == NULL || spec->argv == NULL || spec->argv[0] == NULL
      || spec->on_exit == NULL)
  {
    errno = EINVAL;
    return(NULL);
  }

  if(pipe2(pfd, O_CLOEXEC) != 0)
  {
    saved_errno = errno;
    clam(CLAM_WARN, PROC_CTX, "pipe2 failed errno=%d (%s)",
        saved_errno, strerror(saved_errno));
    errno = saved_errno;
    return(NULL);
  }

  h = mem_alloc(PROC_CTX, "handle", sizeof *h);
  // mem_alloc zeros and aborts on OOM; h is non-NULL and cleared.

  h->stdout_fd   = -1;
  h->buf_cap     = spec->stdout_cap ? spec->stdout_cap
                                    : PROC_STDOUT_CAP_DEFAULT;
  h->buf_sz      = PROC_BUF_INITIAL;

  if(h->buf_sz > h->buf_cap)
    h->buf_sz = h->buf_cap;

  h->buf         = mem_alloc(PROC_CTX, "stdout_buf", h->buf_sz);
  h->on_exit     = spec->on_exit;
  h->user        = spec->user;
  h->timeout_sec = spec->timeout_sec;

  if(pthread_mutex_init(&h->lock, NULL) != 0)
  {
    saved_errno = errno;
    clam(CLAM_WARN, PROC_CTX, "mutex_init failed errno=%d (%s)",
        saved_errno, strerror(saved_errno));
    mem_free(h->buf);
    mem_free(h);
    close(pfd[0]);
    close(pfd[1]);
    errno = saved_errno;
    return(NULL);
  }

  pid = fork();

  if(pid == -1)
  {
    saved_errno = errno;
    clam(CLAM_WARN, PROC_CTX, "fork failed errno=%d (%s)",
        saved_errno, strerror(saved_errno));
    pthread_mutex_destroy(&h->lock);
    mem_free(h->buf);
    mem_free(h);
    close(pfd[0]);
    close(pfd[1]);
    errno = saved_errno;
    return(NULL);
  }

  if(pid == 0)
  {
    // --- Child ---
    // Merge stdout+stderr onto the write end of the pipe, then close
    // both original descriptors. Do NOT run any atexit handlers; use
    // _exit on every failure path.
    if(dup2(pfd[1], STDOUT_FILENO) == -1)
      _exit(127);

    if(dup2(pfd[1], STDERR_FILENO) == -1)
      _exit(127);

    close(pfd[0]);
    close(pfd[1]);

    if(spec->cwd != NULL && chdir(spec->cwd) != 0)
      _exit(127);

    if(spec->envp != NULL)
      execvpe(spec->argv[0], (char *const *)spec->argv,
          (char *const *)spec->envp);
    else
      execvp(spec->argv[0], (char *const *)spec->argv);

    // execvp(e) only returns on failure.
    _exit(127);
  }

  // --- Parent ---
  close(pfd[1]);
  h->pid       = pid;
  h->stdout_fd = pfd[0];

  if(pthread_create(&h->reader_tid, NULL, proc_reader, h) != 0)
  {
    saved_errno = errno;
    clam(CLAM_WARN, PROC_CTX, "reader pthread_create failed errno=%d (%s)",
        saved_errno, strerror(saved_errno));
    // Child is already running; reap it after signalling so we don't
    // leak a zombie.
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    close(pfd[0]);
    pthread_mutex_destroy(&h->lock);
    mem_free(h->buf);
    mem_free(h);
    errno = saved_errno;
    return(NULL);
  }

  if(pthread_create(&h->waiter_tid, NULL, proc_waiter, h) != 0)
  {
    saved_errno = errno;
    clam(CLAM_WARN, PROC_CTX, "waiter pthread_create failed errno=%d (%s)",
        saved_errno, strerror(saved_errno));
    // Kill the child to force EOF on the read pipe so the reader
    // thread exits; then join it before tearing down the handle.
    kill(pid, SIGKILL);
    pthread_join(h->reader_tid, NULL);
    waitpid(pid, NULL, 0);
    pthread_mutex_destroy(&h->lock);
    mem_free(h->buf);
    mem_free(h);
    errno = saved_errno;
    return(NULL);
  }

  clam(CLAM_INFO, PROC_CTX, "spawned pid=%d argv0=%s cap=%zu timeout=%us",
      (int)pid, spec->argv[0], h->buf_cap, spec->timeout_sec);

  if(spec->timeout_sec > 0)
    task_add_deferred("proc_timeout", TASK_ANY, 200,
        (uint32_t)spec->timeout_sec * 1000, proc_timeout_cb, h);

  return(h);
}

// Kill

// Signal a running child. No-op (returns 0) once on_exit has fired.
// h  : handle from proc_spawn
// sig: signal number
int
proc_kill(proc_handle_t *h, int sig)
{
  int   rc = 0;
  pid_t pid;
  bool  alive;

  pthread_mutex_lock(&h->lock);
  alive = !h->exited;
  pid   = h->pid;
  pthread_mutex_unlock(&h->lock);

  if(!alive)
    return(0);

  if(kill(pid, sig) != 0)
    rc = -1;

  return(rc);
}

// Free

// Destroy the handle. Joins reader + waiter (or detaches waiter when
// called from inside on_exit on the waiter thread), closes fds, frees
// the capture buffer. Double-free is UB; see include/proc.h.
void
proc_free(proc_handle_t *h)
{
  bool      detach_waiter = false;
  pthread_t waiter_tid;

  pthread_join(h->reader_tid, NULL);

  pthread_mutex_lock(&h->lock);
  waiter_tid = h->waiter_tid;

  if(h->waiter_self_set
      && pthread_equal(pthread_self(), h->waiter_self))
    detach_waiter = true;

  pthread_mutex_unlock(&h->lock);

  if(detach_waiter)
    pthread_detach(waiter_tid);
  else
    pthread_join(waiter_tid, NULL);

  // stdout_fd is normally closed by the reader before it returns; this
  // covers the error paths in which the reader bailed before EOF.
  pthread_mutex_lock(&h->lock);

  if(h->stdout_fd >= 0)
  {
    close(h->stdout_fd);
    h->stdout_fd = -1;
  }

  pthread_mutex_unlock(&h->lock);

  pthread_mutex_destroy(&h->lock);
  mem_free(h->buf);
  mem_free(h);
}
