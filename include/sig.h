#ifndef BM_SIG_H
#define BM_SIG_H

#include <stdbool.h>
#include <stddef.h>

// A shutdown reason, NUL included. Sized so that the longest one still
// fits an IRC QUIT trailing parameter inside the protocol's 512-byte
// line, and so that it can hold everything a command line can carry
// (CMD_ARG_SZ) without the operator's wording being cut short.
#define SIG_REASON_SZ 256

// Installs handlers for POSIX signals. SIGTERM and SIGINT trigger
// graceful shutdown.
void sig_init(void);

void sig_exit(void);

bool sig_shutdown_requested(void);

// Request shutdown programmatically (e.g. /quit). Has the same effect
// on pool_run_parent as catching SIGTERM, so main.c's shutdown sequence
// runs in order — critically, curl_begin_shutdown() executes while the
// multi-loop thread is still alive and can run the drain. Calling
// pool_shutdown() directly from outside main.c races the curl drain
// (the multi loop exits on pool_shutting_down() before the drain
// trigger arrives) and wedges shutdown for ~11s.
//
// `reason` is the operator's own words for why, or NULL for none. It is
// sanitized and copied here — the caller's storage is its own business
// — and every method driver is handed it as its parting message.
void sig_request_shutdown(const char *reason);

// The reason the running shutdown was requested with, copied into dst
// and always NUL-terminated. Left EMPTY when the shutdown carried none
// — a signal, or a bare /quit — which is what a driver reads as "use
// your own wording". Safe from any thread, before or during shutdown.
void sig_shutdown_reason(char *dst, size_t sz);

// The trust boundary for a reason. Copies src into dst folding every
// control byte to a space, because the consumers are protocols where a
// line ending IS the message boundary and a CR or LF in an operator's
// words would forge a second one. Past this call the bytes are trusted
// and every driver may put them straight on the wire.
//
// Public because it is the boundary, and the boundary is what the
// cases in tests/test_shutdown_reason.c cover. src may be NULL.
void sig_reason_sanitize(char *dst, size_t sz, const char *src);

// The signal number that triggered shutdown, or 0 if none.
int sig_caught(void);

// Returns "UNKNOWN" for unrecognized signals.
const char *sig_name(int signum);

#ifdef SIG_INTERNAL

#include "common.h"
#include "clam.h"

#include <pthread.h>
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

// Set by sig_request_shutdown() — programmatic shutdown requests
// (e.g. /quit). Read by pool_run_parent via sig_shutdown_requested().
// _Atomic, not volatile: it is written on a command thread and read on
// the parent thread with no lock between them, and volatile promises
// nothing about that (measured as a data race, TSan 2026-08-15).
// task_wake_all() in sig_request_shutdown() is what makes the parent
// look promptly; it was never what made the flag safe to read.
static _Atomic bool internal_shutdown = false;

static bool sig_active = false;

// Written once by whoever requests the shutdown, read by every method
// driver on its way down. A mutex rather than a lean on the atomic
// flag's ordering: the readers run on driver threads during teardown,
// a second /quit may still arrive while they do, and last-writer-wins
// on a whole string is only meaningful if nobody can see half of one.
// Nothing logs while it is held (the standing clam-under-lock rule).
static pthread_mutex_t reason_mutex = PTHREAD_MUTEX_INITIALIZER;

static char shutdown_reason[SIG_REASON_SZ] = "";

#endif // SIG_INTERNAL

#endif // BM_SIG_H
