#ifndef BM_SIG_H
#define BM_SIG_H

#include <stdbool.h>

// Initialize signal handling. Installs handlers for POSIX signals.
// SIGTERM and SIGINT trigger graceful shutdown.
void sig_init(void);

// Restore default signal dispositions.
void sig_exit(void);

// returns: true if a graceful shutdown has been requested via signal
bool sig_shutdown_requested(void);

// returns: the signal number that triggered shutdown, or 0 if none
int sig_caught(void);

// returns: human-readable name of a signal number, or "UNKNOWN"
// signum: signal number to look up
const char *sig_name(int signum);

#ifdef SIG_INTERNAL

#include "common.h"
#include "clam.h"

#include <signal.h>
#include <unistd.h>

// Signal name entry for lookup table.
typedef struct
{
  int         signum;
  const char *name;
} sig_entry_t;

// Signal name table for human-readable logging.
static const sig_entry_t sig_table[] = {
  { SIGHUP,  "SIGHUP"  },
  { SIGINT,  "SIGINT"  },
  { SIGQUIT, "SIGQUIT" },
  { SIGILL,  "SIGILL"  },
  { SIGABRT, "SIGABRT" },
  { SIGBUS,  "SIGBUS"  },
  { SIGFPE,  "SIGFPE"  },
  { SIGUSR1, "SIGUSR1" },
  { SIGUSR2, "SIGUSR2" },
  { SIGSEGV, "SIGSEGV" },
  { SIGPIPE, "SIGPIPE" },
  { SIGALRM, "SIGALRM" },
  { SIGTERM, "SIGTERM" },
  { SIGCHLD, "SIGCHLD" },
  { SIGCONT, "SIGCONT" },
  { SIGTSTP, "SIGTSTP" },
  { SIGURG,  "SIGURG"  },
  { SIGTTIN, "SIGTTIN" },
  { SIGXCPU, "SIGXCPU" },
  { SIGXFSZ, "SIGXFSZ" },
  { 0,       NULL      },
};

// Signals that request graceful shutdown.
static const int shutdown_signals[] = { SIGTERM, SIGINT, SIGHUP, 0 };

// Fatal signals: log and re-raise with default handler for core dump.
static const int fatal_signals[] = { SIGQUIT, SIGILL, SIGBUS, SIGFPE, SIGSEGV, 0 };

static volatile sig_atomic_t shutdown_signal = 0;
static bool sig_active = false;

#endif // SIG_INTERNAL

#endif // BM_SIG_H
