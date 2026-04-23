#ifndef BM_SIG_H
#define BM_SIG_H

#include <stdbool.h>

// Installs handlers for POSIX signals. SIGTERM and SIGINT trigger
// graceful shutdown.
void sig_init(void);

void sig_exit(void);

bool sig_shutdown_requested(void);

// The signal number that triggered shutdown, or 0 if none.
int sig_caught(void);

// Returns "UNKNOWN" for unrecognized signals.
const char *sig_name(int signum);

#ifdef SIG_INTERNAL

#include "common.h"
#include "clam.h"

#include <signal.h>
#include <unistd.h>

typedef struct
{
  int         signum;
  const char *name;
} sig_entry_t;

// For human-readable logging.
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

static const int shutdown_signals[] = { SIGTERM, SIGINT, SIGHUP, 0 };

// Fatal signals: log and re-raise with default handler for core dump.
static const int fatal_signals[] = { SIGQUIT, SIGILL, SIGBUS, SIGFPE, SIGSEGV, 0 };

static volatile sig_atomic_t shutdown_signal = 0;
static bool sig_active = false;

#endif // SIG_INTERNAL

#endif // BM_SIG_H
