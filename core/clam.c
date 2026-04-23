// botmanager — MIT
// CLAM event bus: in-process publish/subscribe for log + telemetry events.
#define CLAM_INTERNAL
#include "clam.h"

#include <errno.h>

// Callback-watchdog threshold. A subscriber callback that runs longer
// than this under clam_mutex stalls every other thread emitting clam
// events. Warn via stderr (NOT clam()) since the mutex is held here.
#define CLAM_CB_WATCHDOG_MS 200

// Per-thread recursion guard. clam_mutex is a non-recursive mutex held
// across the subscriber dispatch loop. If a subscriber callback's
// downstream path re-enters clam() on the same thread (e.g. a user
// subscription routed to an IRC destination, which triggers
// method_send → irc_send_raw → clam(DEBUG3, ...)), the re-entry would
// self-deadlock. This flag causes re-entrant clam() calls on the same
// thread to be dropped silently, breaking the recursion. Events emitted
// from inside a subscriber callback are inherently un-deliverable safely
// — dropping them is the correct behaviour.
static __thread int clam_in_cb = 0;

// File logger state.

static FILE *clam_log_fp = NULL;

static void
clam_file_cb(const clam_msg_t *m)
{
  uint8_t   sev;
  struct tm tm;
  time_t    now;

  if(clam_log_fp == NULL)
    return;

  sev = m->sev;

  if(sev > CLAM_DEBUG5)
    sev = CLAM_DEBUG5;

  now = time(NULL);
  localtime_r(&now, &tm);

  fprintf(clam_log_fp,
      "%04d-%02d-%02d %02d:%02d:%02d %s %-*s %s\n",
      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
      tm.tm_hour, tm.tm_min, tm.tm_sec,
      sev_label[sev], CLAM_CTX_SZ, m->context, m->msg);

  fflush(clam_log_fp);
}

static clam_sub_t *
sub_get(void)
{
  clam_sub_t *s;

  if(clam_freelist != NULL)
  {
    s = clam_freelist;
    clam_freelist = s->next;
    clam_free_count--;

    if(s->has_regex)
      regfree(&s->regex_compiled);

    memset(s, 0, sizeof(*s));
    return(s);
  }

  // intentional: mem_alloc depends on clam for logging; raw malloc breaks the cycle
  s = malloc(sizeof(*s));

  if(s == NULL)
  {
    fprintf(stderr, "[FATAL] clam: OOM allocating subscriber struct\n");
    abort();
  }

  memset(s, 0, sizeof(*s));
  return(s);
}

static void
sub_put(clam_sub_t *s)
{
  s->next = clam_freelist;
  clam_freelist = s;
  clam_free_count++;
}

// Public API

void
clam(uint8_t sev, const char *context, const char *fmt, ...)
{
  clam_msg_t m;
  va_list    ap;
  time_t     now;
  char       haystack[CLAM_CTX_SZ + CLAM_MSG_SZ + 2];
  bool       haystack_built = false;

  // Re-entry guard: if we're already inside a subscriber dispatch on
  // this thread, drop the event rather than self-deadlock on the
  // non-recursive clam_mutex. See clam_in_cb declaration for why.
  if(clam_in_cb > 0)
    return;

  memset(&m, 0, sizeof(m));
  m.sev = sev;
  strncpy(m.context, context, CLAM_CTX_SZ - 1);

  va_start(ap, fmt);
  vsnprintf(m.msg, CLAM_MSG_SZ, fmt, ap);
  va_end(ap);

  pthread_mutex_lock(&clam_mutex);

  if(clam_sub_count == 0)
  {
    // No subscribers — default to stdout.
    printf("[%s] %s\n", context, m.msg);
    pthread_mutex_unlock(&clam_mutex);
    return;
  }

  now = time(NULL);

  // Regex runs against "<context> <msg>" so subscribers can filter by
  // subsystem (e.g. "^curl ") as well as message body. Built lazily —
  // only when at least one subscriber has a regex — to keep the common
  // no-filter path cheap.
  for(clam_sub_t *s = clam_subs; s != NULL; s = s->next)
  {
    struct timespec ts_before, ts_after;
    long            elapsed_ms;

    // Skip if message severity is below subscriber threshold.
    if(sev > s->sev)
      continue;

    // FATAL always bypasses regex filtering.
    if(sev != CLAM_FATAL && s->has_regex)
    {
      if(!haystack_built)
      {
        snprintf(haystack, sizeof(haystack), "%s %s", m.context, m.msg);
        haystack_built = true;
      }
      if(regexec(&s->regex_compiled, haystack, 0, NULL, 0) != 0)
        continue;
    }

    s->count++;
    s->last = now;

    clock_gettime(CLOCK_MONOTONIC, &ts_before);
    clam_in_cb++;
    s->cb(&m);
    clam_in_cb--;
    clock_gettime(CLOCK_MONOTONIC, &ts_after);

    elapsed_ms =
        (ts_after.tv_sec  - ts_before.tv_sec)  * 1000L +
        (ts_after.tv_nsec - ts_before.tv_nsec) / 1000000L;

    if(elapsed_ms >= CLAM_CB_WATCHDOG_MS)
      fprintf(stderr,
          "[WARN clam] subscriber '%s' cb took %ld ms"
          " (ctx=%s sev=%u)\n",
          s->name, elapsed_ms, m.context, (unsigned)m.sev);
  }

  pthread_mutex_unlock(&clam_mutex);
}

// Subscribe to CLAM messages.
//        (NULL = match all). FATAL bypasses the filter.
void
clam_subscribe(const char *name, uint8_t sev, const char *regex,
    clam_cb_t cb)
{
  clam_sub_t *s;

  pthread_mutex_lock(&clam_mutex);

  s = sub_get();
  s->sev = sev;
  s->cb = cb;
  strncpy(s->name, name, CLAM_SUB_NAME_SZ - 1);

  if(regex != NULL && regex[0] != '\0')
  {
    strncpy(s->regex_str, regex, CLAM_REGEX_SZ - 1);

    if(regcomp(&s->regex_compiled, regex, REG_EXTENDED | REG_NOSUB) == 0)
      s->has_regex = true;

    else
    {
      // Bad regex — subscribe without filter, log a warning after unlock.
      s->has_regex = false;
      s->regex_str[0] = '\0';
    }
  }

  s->next = clam_subs;
  clam_subs = s;
  clam_sub_count++;

  pthread_mutex_unlock(&clam_mutex);

  clam(CLAM_DEBUG, "clam_subscribe", "added '%s' (sev: %u regex: %s)",
      name, sev, (regex != NULL && regex[0] != '\0') ? regex : "*");
}

bool
clam_unsubscribe(const char *name)
{
  clam_sub_t *s, *prev = NULL;

  pthread_mutex_lock(&clam_mutex);

  for(s = clam_subs; s != NULL; prev = s, s = s->next)
  {
    if(strncasecmp(s->name, name, CLAM_SUB_NAME_SZ) == 0)
    {
      if(prev != NULL)
        prev->next = s->next;

      else
        clam_subs = s->next;

      clam_sub_count--;
      sub_put(s);
      pthread_mutex_unlock(&clam_mutex);

      clam(CLAM_DEBUG, "clam_unsubscribe", "removed '%s'", name);
      return(SUCCESS);
    }
  }

  pthread_mutex_unlock(&clam_mutex);
  clam(CLAM_DEBUG, "clam_unsubscribe", "not found: '%s'", name);
  return(FAIL);
}

// Initialize CLAM subsystem.
// Must be called before any other clam function.
void
clam_init(void)
{
  pthread_mutex_init(&clam_mutex, NULL);
  clam_ready = true;

  clam(CLAM_INFO, "clam_init",
      "central logging and messaging initialized (max_msg: %u)",
      CLAM_MSG_SZ);
}

bool
clam_open_log(const char *path)
{
  if(path == NULL || path[0] == '\0')
    return(FAIL);

  clam_log_fp = fopen(path, "a");

  if(clam_log_fp == NULL)
  {
    fprintf(stderr, "clam: cannot open log file: %s: %s\n",
        path, strerror(errno));
    return(FAIL);
  }

  clam_subscribe("logfile", CLAM_DEBUG5, NULL, clam_file_cb);
  return(SUCCESS);
}

// Close the log file and unsubscribe. Called during shutdown.
void
clam_close_log(void)
{
  if(clam_log_fp == NULL)
    return;

  clam_unsubscribe("logfile");

  fclose(clam_log_fp);
  clam_log_fp = NULL;
}

// Shut down CLAM. Frees all active subscribers and freelist entries.
void
clam_exit(void)
{
  clam_sub_t *s, *next;

  if(!clam_ready)
    return;

  clam(CLAM_INFO, "clam_exit", "shutting down (%u subscribers, %u freelisted)",
      clam_sub_count, clam_free_count);

  clam_ready = false;

  // Close log file before freeing subscribers.
  clam_close_log();

  // Free active subscribers.
  s = clam_subs;

  while(s != NULL)
  {
    next = s->next;

    if(s->has_regex)
      regfree(&s->regex_compiled);

    free(s);
    s = next;
  }

  clam_subs = NULL;
  clam_sub_count = 0;

  // Free the freelist.
  s = clam_freelist;

  while(s != NULL)
  {
    next = s->next;

    if(s->has_regex)
      regfree(&s->regex_compiled);

    free(s);
    s = next;
  }

  clam_freelist = NULL;
  clam_free_count = 0;

  pthread_mutex_destroy(&clam_mutex);
}
