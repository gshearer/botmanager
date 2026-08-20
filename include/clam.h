#ifndef BM_CLAM_H
#define BM_CLAM_H

#include <stdbool.h>
#include <stdint.h>

// Severity levels: 0 = highest (fatal), 7 = lowest (debug5).
#define CLAM_FATAL   0
#define CLAM_WARN    1
#define CLAM_INFO    2
#define CLAM_DEBUG   3
#define CLAM_DEBUG2  4
#define CLAM_DEBUG3  5
#define CLAM_DEBUG4  6
#define CLAM_DEBUG5  7

#define CLAM_MSG_SZ       1000
#define CLAM_CTX_SZ       60
#define CLAM_SUB_NAME_SZ  40
#define CLAM_REGEX_SZ     100

typedef struct
{
  uint8_t sev;
  char    context[CLAM_CTX_SZ];
  char    msg[CLAM_MSG_SZ];
} clam_msg_t;

typedef void (*clam_cb_t)(const clam_msg_t *msg);

void clam(uint8_t sev, const char *context, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// sev is the maximum severity to receive (e.g., CLAM_INFO receives
// FATAL, WARN, INFO).
// regex: optional POSIX ERE filter run against "<context> <msg>" so
// subscribers can select by subsystem label as well as body (e.g.,
// "^curl " or "curl|acquire"). NULL = match all. FATAL messages
// bypass the filter.
void clam_subscribe(const char *name, uint8_t sev, const char *regex,
    clam_cb_t cb);

bool clam_unsubscribe(const char *name);

// Audit hook: yields each subscriber's retained callback pointer.
// `subject` is the subscriber name — the same handle
// clam_unsubscribe() takes.
//
// Invoked UNDER clam_mutex: the callback must be fast and must not
// call clam() or any other clam_* API.
typedef void (*clam_audit_cb_t)(const char *subject, const char *field,
    const void *ptr, void *data);

void clam_audit_iterate(clam_audit_cb_t cb, void *data);

// Drop every subscriber registered by code inside the address range
// [lo,hi) — one loaded object's mapping. Subscribers are Class A (see
// PLIFE-3): the retained cb is dispatched from any thread
// that logs, so leaving one behind past dlclose crashes the next
// clam() call, and dropping it only costs the plugin its own log feed.
// Returns the number of subscribers removed.
uint32_t clam_reclaim_owned(uintptr_t lo, uintptr_t hi);

// Must be called before any other clam function.
void clam_init(void);

// Open a log file and register a CLAM subscriber that writes to it.
// Returns FAIL if path is NULL or empty.
bool clam_open_log(const char *path);

// No-op if not open.
void clam_close_log(void);

// Frees all subscribers and freelisted entries.
void clam_exit(void);

#ifdef CLAM_INTERNAL

#include "common.h"
#include "colors.h"

#include <stdarg.h>
#include <regex.h>

typedef struct clam_sub
{
  char             name[CLAM_SUB_NAME_SZ];
  char             regex_str[CLAM_REGEX_SZ];
  regex_t          regex_compiled;
  bool             has_regex;
  uint8_t          sev;
  uint64_t         count;
  time_t           last;
  clam_cb_t        cb;
  const void      *owner_pc;  // clam_subscribe() call site; identifies the owning object
  struct clam_sub *next;
} clam_sub_t;

static pthread_mutex_t clam_mutex;
static clam_sub_t     *clam_subs     = NULL;
static clam_sub_t     *clam_freelist = NULL;
static uint32_t        clam_sub_count  = 0;
static uint32_t        clam_free_count = 0;
static bool            clam_ready = false;

// Indexed by clam severity level.
static const char *sev_label[] = {
  [CLAM_FATAL]  = "FATAL",
  [CLAM_WARN]   = " WARN",
  [CLAM_INFO]   = " INFO",
  [CLAM_DEBUG]  = "  DBG",
  [CLAM_DEBUG2] = " DBG2",
  [CLAM_DEBUG3] = " DBG3",
  [CLAM_DEBUG4] = " DBG4",
  [CLAM_DEBUG5] = " DBG5",
};

#endif // CLAM_INTERNAL

#endif // BM_CLAM_H
