#define CLAM_INTERNAL
#include "clam.h"
#include "console.h"
#include "kv.h"

// -----------------------------------------------------------------------
// Built-in stdinout subscriber: color-coded stdout output.
// -----------------------------------------------------------------------

// Stdinout subscriber callback: writes color-coded output to stdout.
// Uses console_output_lock/unlock to avoid corrupting the readline
// input line when log messages arrive on other threads.
// m: incoming clam message
static void
stdinout_cb(const clam_msg_t *m)
{
  uint8_t sev = m->sev;

  if(sev > CLAM_DEBUG5)
    sev = CLAM_DEBUG5;

  struct tm tm;
  time_t    now = time(NULL);

  localtime_r(&now, &tm);

  console_output_lock();

  printf("%02d:%02d:%02d %s%s %-*s%s %s\n",
      tm.tm_hour, tm.tm_min, tm.tm_sec,
      sev_color[sev], sev_label[sev],
      CLAM_CTX_SZ, m->context,
      CON_RESET, m->msg);

  console_output_unlock();
}

// -----------------------------------------------------------------------
// Get a subscriber struct from the freelist or malloc a new one.
// CLAM is below the mem subsystem in the init order, so it uses raw malloc.
// returns: zeroed subscriber struct (never NULL — aborts on OOM)
// -----------------------------------------------------------------------
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

  s = malloc(sizeof(*s));

  if(s == NULL)
  {
    fprintf(stderr, "[FATAL] clam: OOM allocating subscriber struct\n");
    abort();
  }

  memset(s, 0, sizeof(*s));
  return(s);
}

// -----------------------------------------------------------------------
// Return a subscriber struct to the freelist.
// s: subscriber to recycle
// -----------------------------------------------------------------------
static void
sub_put(clam_sub_t *s)
{
  s->next = clam_freelist;
  clam_freelist = s;
  clam_free_count++;
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

// Publish a message to all matching subscribers.
// sev: severity level
// context: subsystem name
// fmt: printf-style format string
void
clam(uint8_t sev, const char *context, const char *fmt, ...)
{
  clam_msg_t m;
  va_list ap;

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

  time_t now = time(NULL);

  for(clam_sub_t *s = clam_subs; s != NULL; s = s->next)
  {
    // Skip if message severity is below subscriber threshold.
    if(sev > s->sev)
      continue;

    // FATAL always bypasses regex filtering.
    if(sev != CLAM_FATAL && s->has_regex)
    {
      if(regexec(&s->regex_compiled, m.msg, 0, NULL, 0) != 0)
        continue;
    }

    s->count++;
    s->last = now;
    s->cb(&m);
  }

  pthread_mutex_unlock(&clam_mutex);
}

// Subscribe to CLAM messages.
// name: subscriber identifier (used for unsubscribe)
// sev: maximum severity to receive
// regex: optional regex filter on message text (NULL = match all)
// cb: callback invoked for each matching message
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
    {
      s->has_regex = true;
    }

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

// returns: SUCCESS or FAIL
// name: subscriber identifier to remove
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

// Initialize CLAM subsystem and register the built-in stdinout subscriber.
// Must be called before any other clam function.
void
clam_init(void)
{
  pthread_mutex_init(&clam_mutex, NULL);
  clam_ready = true;

  // Register the built-in stdinout subscriber (all severities, no regex).
  stdinout_sub = sub_get();
  stdinout_sub->sev = CLAM_DEBUG5;
  stdinout_sub->cb = stdinout_cb;
  strncpy(stdinout_sub->name, "stdinout", CLAM_SUB_NAME_SZ - 1);
  stdinout_sub->next = clam_subs;
  clam_subs = stdinout_sub;
  clam_sub_count++;

  clam(CLAM_INFO, "clam_init",
      "central logging and messaging initialized (max_msg: %u)",
      CLAM_MSG_SZ);
}

// -----------------------------------------------------------------------
// KV callback: reload stdinout subscriber settings from KV store.
// key: changed key name
// data: unused
// -----------------------------------------------------------------------
static void
stdinout_kv_changed(const char *key, void *data)
{
  (void)key;
  (void)data;

  if(stdinout_sub == NULL)
    return;

  uint8_t sev = (uint8_t)kv_get_uint("core.clam.stdinout.severity");
  const char *regex = kv_get_str("core.clam.stdinout.regex");

  pthread_mutex_lock(&clam_mutex);

  stdinout_sub->sev = sev;

  // Update regex. Treat ".*" or empty as "match all" (no compiled regex).
  if(stdinout_sub->has_regex)
  {
    regfree(&stdinout_sub->regex_compiled);
    stdinout_sub->has_regex = false;
    stdinout_sub->regex_str[0] = '\0';
  }

  if(regex != NULL && regex[0] != '\0'
      && strcmp(regex, ".*") != 0)
  {
    strncpy(stdinout_sub->regex_str, regex, CLAM_REGEX_SZ - 1);

    if(regcomp(&stdinout_sub->regex_compiled, regex,
          REG_EXTENDED | REG_NOSUB) == 0)
    {
      stdinout_sub->has_regex = true;
    }

    else
    {
      stdinout_sub->has_regex = false;
      stdinout_sub->regex_str[0] = '\0';
    }
  }

  pthread_mutex_unlock(&clam_mutex);
}

// Register KV settings for the stdinout subscriber and apply initial values.
// Must be called after kv_init()/kv_load().
void
clam_register_config(void)
{
  kv_register("core.clam.stdinout.severity", KV_UINT8, "7",
      stdinout_kv_changed, NULL);
  kv_register("core.clam.stdinout.regex", KV_STR, ".*",
      stdinout_kv_changed, NULL);

  // Apply loaded values (KV may have DB overrides from kv_load).
  stdinout_kv_changed(NULL, NULL);

  clam(CLAM_DEBUG, "clam", "stdinout KV config registered");
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
  stdinout_sub = NULL;
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
