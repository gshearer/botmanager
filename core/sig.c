// botmanager — MIT
// Signal-handler installation and async-safe dispatch to the main loop.
#define SIG_INTERNAL
#include "sig.h"

// Async-signal-safe handler for SIGTERM/SIGINT/SIGHUP: record signal.
// signum: the signal number received
static void
sig_shutdown_handler(int signum)
{
  if(shutdown_signal == 0)
    shutdown_signal = signum;
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
  write(STDERR_FILENO, "[FATAL] caught signal: ", 23);
  write(STDERR_FILENO, name, strlen(name));
  write(STDERR_FILENO, "\n", 1);

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
  return(shutdown_signal != 0);
}

int
sig_caught(void)
{
  return((int)shutdown_signal);
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

  sig_active = true;
  shutdown_signal = 0;

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
