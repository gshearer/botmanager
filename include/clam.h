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

#define CLAM_MSG_SZ  1000
#define CLAM_CTX_SZ  40

// A clam message delivered to subscribers.
typedef struct
{
  uint8_t sev;
  char    context[CLAM_CTX_SZ];
  char    msg[CLAM_MSG_SZ];
} clam_msg_t;

// Subscriber callback type.
typedef void (*clam_cb_t)(const clam_msg_t *msg);

// Publish a message. Variadic printf-style.
// sev: severity level
// context: subsystem name
// fmt: printf-style format string
void clam(uint8_t sev, const char *context, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// Subscribe to CLAM messages.
// name: subscriber identifier (used for unsubscribe)
// sev: maximum severity to receive (e.g., CLAM_INFO receives FATAL, WARN, INFO)
// regex: optional regex filter on message text (NULL = match all)
// cb: callback invoked for each matching message
void clam_subscribe(const char *name, uint8_t sev, const char *regex,
    clam_cb_t cb);

// returns: SUCCESS or FAIL
// name: subscriber identifier to remove
bool clam_unsubscribe(const char *name);

// Initialize CLAM. Must be called before any other clam function.
// Registers the built-in stdinout subscriber for color-coded stdout output.
void clam_init(void);

// Register KV settings for the stdinout subscriber.
// Must be called after kv_init()/kv_load().
// Keys: core.clam.stdinout.severity (UINT8, default 7),
//        core.clam.stdinout.regex (STR, default ".*").
void clam_register_config(void);

// Shut down CLAM. Frees all subscribers and freelisted entries.
void clam_exit(void);

#ifdef CLAM_INTERNAL

#include "common.h"
#include "colors.h"

#include <stdarg.h>
#include <regex.h>

#define CLAM_SUB_NAME_SZ  40
#define CLAM_REGEX_SZ     100

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
  struct clam_sub *next;
} clam_sub_t;

static pthread_mutex_t clam_mutex;
static clam_sub_t     *clam_subs     = NULL;
static clam_sub_t     *clam_freelist = NULL;
static uint32_t        clam_sub_count  = 0;
static uint32_t        clam_free_count = 0;
static bool            clam_ready = false;

// Human-readable severity labels, indexed by clam severity level.
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

// Color per severity level.
static const char *sev_color[] = {
  [CLAM_FATAL]  = CON_RED,
  [CLAM_WARN]   = CON_YELLOW,
  [CLAM_INFO]   = CON_GREEN,
  [CLAM_DEBUG]  = CON_CYAN,
  [CLAM_DEBUG2] = CON_PURPLE,
  [CLAM_DEBUG3] = CON_BLUE,
  [CLAM_DEBUG4] = CON_WHITE,
  [CLAM_DEBUG5] = CON_WHITE,
};

// Direct pointer to the stdinout subscriber for KV callback updates.
static clam_sub_t *stdinout_sub = NULL;

#endif // CLAM_INTERNAL

#endif // BM_CLAM_H
