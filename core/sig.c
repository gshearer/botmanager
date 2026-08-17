// botmanager — MIT
// Signal-handler installation and async-safe dispatch to the main loop.
#define SIG_INTERNAL
#include "sig.h"
#include "task.h"

// Async-signal-safe handler for SIGTERM/SIGINT/SIGHUP: record signal.
// signum: the signal number received
static void
sig_shutdown_handler(int signum)
{
  if(shutdown_signal == 0)
    shutdown_signal = signum;
}

// Write a whole diagnostic to stderr from inside a signal handler. clam()
// is not async-signal-safe and the process re-raises the moment this
// returns, so there is no recovery to attempt and none is attempted: the
// loop is here to survive a short write, not to report one.
// s:   bytes to write
// len: how many
static void
sig_write_stderr(const char *s, size_t len)
{
  ssize_t n;

  while(len > 0)
  {
    n = write(STDERR_FILENO, s, len);

    if(n <= 0)
      return;

    s   += (size_t)n;
    len -= (size_t)n;
  }
}

// Async-signal-safe handler for fatal signals: write to stderr, re-raise.
// Uses only write() which is async-signal-safe.
// signum: the signal number received
static void
sig_fatal_handler(int signum)
{
  const char *name = "UNKNOWN";
  struct sigaction sa;

  for(int i = 0; sig_table[i].name != NULL; i++)
  {
    if(sig_table[i].signum == signum)
    {
      name = sig_table[i].name;
      break;
    }
  }

  // Async-signal-safe output.
  sig_write_stderr("[FATAL] caught signal: ", 23);
  sig_write_stderr(name, strlen(name));
  sig_write_stderr("\n", 1);

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_DFL;
  sigaction(signum, &sa, NULL);
  raise(signum);
}

static void
install_handler(int signum, void (*handler)(int))
{
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;

  sigaction(signum, &sa, NULL);
}

// Public API

const char *
sig_name(int signum)
{
  for(int i = 0; sig_table[i].name != NULL; i++)
    if(sig_table[i].signum == signum)
      return(sig_table[i].name);

  return("UNKNOWN");
}

bool
sig_shutdown_requested(void)
{
  return(shutdown_signal != 0 || internal_shutdown);
}

int
sig_caught(void)
{
  return((int)shutdown_signal);
}

void
sig_reason_sanitize(char *dst, size_t sz, const char *src)
{
  size_t i = 0;

  if(sz == 0)
    return;

  while(src != NULL && i < sz - 1 && src[i] != '\0')
  {
    unsigned char c = (unsigned char)src[i];

    // Cast first: on a signed char every byte of a multi-byte UTF-8
    // sequence is negative, so an unqualified `< 0x20` would eat the
    // operator's accents and emoji along with the newlines.
    dst[i] = (c < 0x20 || c == 0x7f) ? ' ' : src[i];
    i++;
  }

  dst[i] = '\0';
}

void
sig_shutdown_reason(char *dst, size_t sz)
{
  if(sz == 0)
    return;

  pthread_mutex_lock(&reason_mutex);
  strlcpy(dst, shutdown_reason, sz);
  pthread_mutex_unlock(&reason_mutex);
}

void
sig_request_shutdown(const char *reason)
{
  // Sanitize on the way in, once. Every reader past this point — core's
  // shutdown log, method_unregister, whatever a driver puts on a wire —
  // takes the stored bytes as trusted, which is only true because this
  // is the single door they came through.
  pthread_mutex_lock(&reason_mutex);
  sig_reason_sanitize(shutdown_reason, sizeof(shutdown_reason), reason);
  pthread_mutex_unlock(&reason_mutex);

  internal_shutdown = true;

  // Wake the parent loop (which is sleeping in task_wait) so it
  // notices the flag immediately instead of waiting up to wait_ms.
  // Mirrors what pool_shutdown() does for its own flag.
  task_wake_all();
}

void
sig_init(void)
{
  // Shutdown signals: set flag and return.
  for(int i = 0; shutdown_signals[i] != 0; i++)
    install_handler(shutdown_signals[i], sig_shutdown_handler);

  // Fatal signals: log and re-raise for core dump.
  for(int i = 0; fatal_signals[i] != 0; i++)
    install_handler(fatal_signals[i], sig_fatal_handler);

  // SIGPIPE is ignored — essential for socket I/O.
  install_handler(SIGPIPE, SIG_IGN);

  sig_active        = true;
  shutdown_signal   = 0;
  internal_shutdown = false;

  clam(CLAM_INFO, "sig_init", "signal handling initialized");
}

void
sig_exit(void)
{
  if(!sig_active)
    return;

  clam(CLAM_INFO, "sig_exit", "restoring default signal dispositions");

  // Restore defaults for all handled signals.
  for(int i = 0; shutdown_signals[i] != 0; i++)
    install_handler(shutdown_signals[i], SIG_DFL);

  for(int i = 0; fatal_signals[i] != 0; i++)
    install_handler(fatal_signals[i], SIG_DFL);

  install_handler(SIGPIPE, SIG_DFL);

  sig_active = false;
}
