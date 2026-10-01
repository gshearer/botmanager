// botmanager — MIT
// Acquisition engine: skip result pages by operator rule and learned host.

#include "acquire_priv.h"

#include "db.h"

#include <errno.h>
#include <inttypes.h>
#include <regex.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// The operator's rule. Compiled in place under the write lock; regexec
// on a compiled pattern is MT-Safe, so lookups share the read lock.
static pthread_rwlock_t acq_skip_rx_lock = PTHREAD_RWLOCK_INITIALIZER;
static regex_t          acq_skip_rx;
static bool             acq_skip_rx_set  = false;
static char             acq_skip_rx_src[KV_STR_SZ];

// Hosts currently skipped. A slot whose `until` has passed is free.
typedef struct
{
  char   host[ACQUIRE_HOST_SZ];
  time_t until;
} acq_skip_host_t;

static pthread_mutex_t  acq_skip_hosts_mutex = PTHREAD_MUTEX_INITIALIZER;
static acq_skip_host_t  acq_skip_hosts[ACQUIRE_SKIP_HOSTS_MAX];

// Skip writes queued and not yet called back. A queued write runs core's
// async_cb and lands in this plugin's callback, which the unload barrier
// cannot attribute to this mapping — so acq_skip_stop() latches new ones
// off and waits these out before deinit. Static and never destroyed.
static pthread_mutex_t  acq_skip_flight_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   acq_skip_flight_cond  = PTHREAD_COND_INITIALIZER;
static uint32_t         acq_skip_inflight     = 0;
static bool             acq_skip_stopping     = false;

// One failure. A URL already in fail_urls is a repeat of the same page
// and adds nothing, so the count is of DISTINCT failing URLs, held to
// the threshold so the array never grows past it. Once that many are
// held the host is skipped for host_skip_days, and an expired skip re-arms on
// the next failure because the array is still full — only a success
// empties it. An unexpired skip is left alone so in-flight fetches
// that land after it cannot extend it.
static const char acq_skip_fail_sql[] =
    "INSERT INTO acquire_host_skip AS t"
    " (host, fail_urls, last_fail_url, last_status, last_fail_at,"
    "  skip_until)"
    " VALUES ($1::text, ARRAY[$2::text], $2::text, $3::int, NOW(),"
    "  CASE WHEN $4::int <= 1"
    "   THEN NOW() + make_interval(days => $5::int) END)"
    " ON CONFLICT (host) DO UPDATE SET"
    "  fail_urls = CASE"
    "   WHEN $2::text = ANY(t.fail_urls)"
    "     OR cardinality(t.fail_urls) >= $4::int THEN t.fail_urls"
    "   ELSE t.fail_urls || $2::text END,"
    "  last_fail_url = $2::text,"
    "  last_status   = $3::int,"
    "  last_fail_at  = NOW(),"
    "  skip_until = CASE"
    "   WHEN t.skip_until > NOW() THEN t.skip_until"
    "   WHEN cardinality(t.fail_urls)"
    "     + CASE WHEN $2::text = ANY(t.fail_urls) THEN 0 ELSE 1 END"
    "     >= $4::int"
    "   THEN NOW() + make_interval(days => $5::int)"
    "   ELSE t.skip_until END"
    " RETURNING host,"
    "  COALESCE(EXTRACT(EPOCH FROM skip_until)::bigint, 0),"
    "  cardinality(fail_urls)";

// One success clears the record. A healthy host has no row to clear, so
// this costs one primary-key lookup per fetched page and writes nothing.
static const char acq_skip_ok_sql[] =
    "UPDATE acquire_host_skip"
    " SET fail_urls = '{}', skip_until = NULL, last_ok_at = NOW()"
    " WHERE host = $1::text"
    "  AND (cardinality(fail_urls) > 0 OR skip_until IS NOT NULL)"
    " RETURNING host";

// Returns true when `host` was not already skipped, so the caller logs
// the transition once rather than on every late failure.
static bool
acq_skip_arm(const char *host, time_t until)
{
  time_t           now   = time(NULL);
  acq_skip_host_t *same  = NULL;
  acq_skip_host_t *spare = NULL;
  bool             fresh = false;
  bool             full  = false;

  pthread_mutex_lock(&acq_skip_hosts_mutex);

  for(size_t i = 0; i < ACQUIRE_SKIP_HOSTS_MAX && same == NULL; i++)
  {
    acq_skip_host_t *h = &acq_skip_hosts[i];

    if(h->host[0] != '\0' && strcmp(h->host, host) == 0)
      same = h;

    else if(spare == NULL && h->until <= now)
      spare = h;
  }

  if(same != NULL)
  {
    fresh       = same->until <= now;
    same->until = until;
  }

  else if(spare != NULL)
  {
    strlcpy(spare->host, host, sizeof(spare->host));
    spare->until = until;
    fresh        = true;
  }

  else
    full = true;

  pthread_mutex_unlock(&acq_skip_hosts_mutex);

  if(full)
    clam(CLAM_WARN, ACQUIRE_CTX,
        "host skip: in-memory set full (%d), host=%s not skipped until"
        " a slot expires", ACQUIRE_SKIP_HOSTS_MAX, host);

  return(fresh);
}

static void
acq_skip_disarm(const char *host)
{
  pthread_mutex_lock(&acq_skip_hosts_mutex);

  for(size_t i = 0; i < ACQUIRE_SKIP_HOSTS_MAX; i++)
  {
    acq_skip_host_t *h = &acq_skip_hosts[i];

    if(h->host[0] != '\0' && strcmp(h->host, host) == 0)
    {
      h->host[0] = '\0';
      h->until   = 0;
    }
  }

  pthread_mutex_unlock(&acq_skip_hosts_mutex);
}

void
acq_skip_set_regex(const char *pattern)
{
  char err[160];
  int  rc = 0;

  if(pattern == NULL)
    pattern = "";

  pthread_rwlock_wrlock(&acq_skip_rx_lock);

  if(strcmp(pattern, acq_skip_rx_src) == 0)
  {
    pthread_rwlock_unlock(&acq_skip_rx_lock);
    return;
  }

  if(acq_skip_rx_set)
    regfree(&acq_skip_rx);

  acq_skip_rx_set = false;
  strlcpy(acq_skip_rx_src, pattern, sizeof(acq_skip_rx_src));

  // A pattern regcomp refuses means no rule, not a stale one and not
  // "skip everything": the WARN below names it, and the only cost of a
  // typo is that paywalled pages are fetched and fail as before.
  if(pattern[0] != '\0')
  {
    rc = regcomp(&acq_skip_rx, pattern, ACQUIRE_SKIP_REGEX_FLAGS);

    if(rc == 0)
      acq_skip_rx_set = true;

    else
      regerror(rc, &acq_skip_rx, err, sizeof(err));
  }

  pthread_rwlock_unlock(&acq_skip_rx_lock);

  if(rc != 0)
    clam(CLAM_WARN, ACQUIRE_CTX,
        "acquire.page_url_skip_regex refused (%s) — no page-skip rule"
        " until it is fixed", err);
}

void
acq_skip_start(void)
{
  db_result_t *res = db_result_alloc();
  size_t       n   = 0;
  char         sql[192];

  pthread_mutex_lock(&acq_skip_flight_mutex);
  acq_skip_stopping = false;
  pthread_mutex_unlock(&acq_skip_flight_mutex);

  snprintf(sql, sizeof(sql),
      "SELECT host, EXTRACT(EPOCH FROM skip_until)::bigint"
      " FROM acquire_host_skip WHERE skip_until > NOW()"
      " ORDER BY skip_until DESC LIMIT %d", ACQUIRE_SKIP_HOSTS_MAX);

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "host skip: load failed (%s) — learned skips start empty",
        res->error);
    db_result_free(res);
    return;
  }

  pthread_mutex_lock(&acq_skip_hosts_mutex);

  for(uint32_t r = 0; r < res->rows && n < ACQUIRE_SKIP_HOSTS_MAX; r++)
  {
    acq_skip_host_t *h = &acq_skip_hosts[n];

    db_result_copy(h->host, sizeof(h->host), res, r, 0);
    h->until = (time_t)db_result_get_i64(res, r, 1, 0);

    if(h->host[0] != '\0')
      n++;
  }

  pthread_mutex_unlock(&acq_skip_hosts_mutex);
  db_result_free(res);

  if(n > 0)
    clam(CLAM_INFO, ACQUIRE_CTX,
        "host skip: %zu host(s) still skipped from earlier failures", n);
}

// Take a flight slot for one write, or refuse it once stop has begun.
static bool
acq_skip_flight_take(void)
{
  bool ok;

  pthread_mutex_lock(&acq_skip_flight_mutex);
  ok = !acq_skip_stopping;

  if(ok)
    acq_skip_inflight++;

  pthread_mutex_unlock(&acq_skip_flight_mutex);
  return(ok);
}

// The last thing a skip-write callback does: everything after the unlock
// is the return out of this plugin's code.
static void
acq_skip_flight_done(void)
{
  pthread_mutex_lock(&acq_skip_flight_mutex);
  acq_skip_inflight--;
  pthread_cond_broadcast(&acq_skip_flight_cond);
  pthread_mutex_unlock(&acq_skip_flight_mutex);
}

size_t
acq_skip_stop(void)
{
  struct timespec deadline;
  uint32_t        left;

  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += ACQUIRE_SKIP_STOP_DRAIN_SECS;

  pthread_mutex_lock(&acq_skip_flight_mutex);
  acq_skip_stopping = true;

  while(acq_skip_inflight > 0)
    if(pthread_cond_timedwait(&acq_skip_flight_cond, &acq_skip_flight_mutex,
        &deadline) == ETIMEDOUT)
      break;

  left = acq_skip_inflight;
  pthread_mutex_unlock(&acq_skip_flight_mutex);

  if(left > 0)
    clam(CLAM_WARN, ACQUIRE_CTX,
        "host skip: %u write(s) still queued after %d s — an unload now"
        " can run their callback after the plugin is gone", left,
        ACQUIRE_SKIP_STOP_DRAIN_SECS);

  return(left);
}

void
acq_skip_exit(void)
{
  pthread_rwlock_wrlock(&acq_skip_rx_lock);

  if(acq_skip_rx_set)
    regfree(&acq_skip_rx);

  acq_skip_rx_set    = false;
  acq_skip_rx_src[0] = '\0';
  pthread_rwlock_unlock(&acq_skip_rx_lock);

  pthread_mutex_lock(&acq_skip_hosts_mutex);
  memset(acq_skip_hosts, 0, sizeof(acq_skip_hosts));
  pthread_mutex_unlock(&acq_skip_hosts_mutex);
}

acq_skip_t
acq_skip_check(const char *url)
{
  char   host[ACQUIRE_HOST_SZ];
  bool   hit = false;
  time_t now;

  // A match skips, and so does a regexec that could not finish: an
  // unanswered check is not a pass.
  pthread_rwlock_rdlock(&acq_skip_rx_lock);

  if(acq_skip_rx_set)
    hit = regexec(&acq_skip_rx, url, 0, NULL, 0) != REG_NOMATCH;

  pthread_rwlock_unlock(&acq_skip_rx_lock);

  if(hit)
    return(ACQ_SKIP_RULE);

  if(!acq_url_host(url, host, sizeof(host)))
    return(ACQ_SKIP_BADURL);

  now = time(NULL);
  pthread_mutex_lock(&acq_skip_hosts_mutex);

  for(size_t i = 0; i < ACQUIRE_SKIP_HOSTS_MAX && !hit; i++)
    hit = acq_skip_hosts[i].until > now
        && strcmp(acq_skip_hosts[i].host, host) == 0;

  pthread_mutex_unlock(&acq_skip_hosts_mutex);

  return(hit ? ACQ_SKIP_LEARNED : ACQ_SKIP_NONE);
}

static void
acq_skip_fail_done(db_result_t *res, void *data)
{
  char      host[ACQUIRE_HOST_SZ];
  char      when[32];
  int64_t   until;
  int64_t   n;
  time_t    t;
  struct tm tm;

  (void)data;

  if(!res->ok || res->rows == 0)
  {
    clam(CLAM_WARN, ACQUIRE_CTX, "host skip: failure not recorded: %s",
        res->error);
    db_result_free(res);
    acq_skip_flight_done();
    return;
  }

  db_result_copy(host, sizeof(host), res, 0, 0);
  until = db_result_get_i64(res, 0, 1, 0);
  n     = db_result_get_i64(res, 0, 2, 0);
  db_result_free(res);

  if(until <= (int64_t)time(NULL))
    clam(CLAM_DEBUG, ACQUIRE_CTX,
        "host skip: host=%s unreadable_pages=%" PRId64, host, n);

  else if(acq_skip_arm(host, (time_t)until))
  {
    t = (time_t)until;

    if(localtime_r(&t, &tm) == NULL
        || strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm) == 0)
      snprintf(when, sizeof(when), "epoch %" PRId64, until);

    clam(CLAM_INFO, ACQUIRE_CTX,
        "host skip: learned host=%s after %" PRId64 " unreadable pages —"
        " skipped until %s", host, n, when);
  }

  acq_skip_flight_done();
}

static void
acq_skip_ok_done(db_result_t *res, void *data)
{
  char host[ACQUIRE_HOST_SZ];

  (void)data;

  if(!res->ok)
    clam(CLAM_WARN, ACQUIRE_CTX, "host skip: success not recorded: %s",
        res->error);

  else if(res->rows > 0)
  {
    db_result_copy(host, sizeof(host), res, 0, 0);
    acq_skip_disarm(host);
    clam(CLAM_DEBUG, ACQUIRE_CTX, "host skip: host=%s read again, cleared",
        host);
  }

  db_result_free(res);
  acq_skip_flight_done();
}

void
acq_skip_note(const char *url, acq_fetch_verdict_t v, long status)
{
  char        host[ACQUIRE_HOST_SZ];
  char        st[24];
  char        thr[16];
  char        days[16];
  const char *params[5];
  uint32_t    n_fail;
  uint32_t    n_days;
  async_rc_t  rc;

  if(v == ACQ_FETCH_TRANSIENT || !acq_url_host(url, host, sizeof(host)))
    return;

  if(v == ACQ_FETCH_USABLE)
  {
    if(!acq_skip_flight_take())
      return;

    params[0] = host;
    rc = db_query_params_async(acq_skip_ok_sql, params, 1,
        acq_skip_ok_done, NULL);

    if(rc == ASYNC_FAILED_UNDELIVERED)
    {
      acq_skip_flight_done();
      clam(CLAM_DEBUG, ACQUIRE_CTX,
          "host skip: success for host=%s not queued", host);
    }

    return;
  }

  pthread_mutex_lock(&acquire_cfg_mutex);
  n_fail = acquire_cfg.host_skip_failures;
  n_days = acquire_cfg.host_skip_days;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  if(n_fail == 0 || !acq_skip_flight_take())
    return;

  snprintf(st,   sizeof(st),   "%ld", status);
  snprintf(thr,  sizeof(thr),  "%u",  n_fail);
  snprintf(days, sizeof(days), "%u",  n_days);

  params[0] = host;
  params[1] = url;
  params[2] = st;
  params[3] = thr;
  params[4] = days;

  rc = db_query_params_async(acq_skip_fail_sql, params, 5,
      acq_skip_fail_done, NULL);

  if(rc == ASYNC_FAILED_UNDELIVERED)
  {
    acq_skip_flight_done();
    clam(CLAM_WARN, ACQUIRE_CTX,
        "host skip: failure for host=%s not queued", host);
  }
}

bool
acq_skip_forget(const char *host, bool *existed)
{
  char         url[ACQUIRE_HOST_SZ + 16];
  char         key[ACQUIRE_HOST_SZ];
  const char  *params[1];
  db_result_t *res;

  *existed = false;

  // Through the boundary that wrote the key, so a typed `Example.COM.`
  // or `host:8080` finds the row `example.com` / `host` it was stored as.
  snprintf(url, sizeof(url), "https://%s/", host);

  if(!acq_url_host(url, key, sizeof(key)))
    return(true);

  params[0] = key;
  res = db_result_alloc();

  if(db_query_params("DELETE FROM acquire_host_skip WHERE host = $1::text",
      params, 1, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, ACQUIRE_CTX, "host skip: forget host=%s failed: %s",
        key, res->error);
    db_result_free(res);
    return(false);
  }

  *existed = res->rows_affected > 0;
  db_result_free(res);

  // After the row, so a failed DELETE leaves memory agreeing with it.
  acq_skip_disarm(key);
  return(true);
}

void
acq_skip_prune(void)
{
  char sql[256];

  snprintf(sql, sizeof(sql),
      "DELETE FROM acquire_host_skip"
      " WHERE (skip_until IS NULL OR skip_until < NOW())"
      "  AND COALESCE(last_fail_at, last_ok_at, NOW())"
      "   < NOW() - make_interval(days => %d)",
      ACQUIRE_HOST_SKIP_PRUNE_DAYS);

  (void)db_exec(sql, ACQUIRE_CTX);
}

size_t
acq_skip_armed_count(void)
{
  time_t now = time(NULL);
  size_t n   = 0;

  pthread_mutex_lock(&acq_skip_hosts_mutex);

  for(size_t i = 0; i < ACQUIRE_SKIP_HOSTS_MAX; i++)
    if(acq_skip_hosts[i].until > now)
      n++;

  pthread_mutex_unlock(&acq_skip_hosts_mutex);
  return(n);
}
