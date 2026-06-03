// botmanager — MIT
// wm_exch_query — synchronous-fetch bridge + display helpers for the
// operator exchange observability commands. See wm_exch_query.h.

#include "wm_exch_query.h"
#include "alloc.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct wm_sync_fetch
{
  pthread_mutex_t mu;
  pthread_cond_t  cv;
  int             refs;     // waiter + callback
  bool            done;
  size_t          buf_sz;
  unsigned char   buf[];    // result_sz bytes, owned by the handle
};

static void
wm_sync_fetch_release(wm_sync_fetch_t *w)
{
  bool last;

  pthread_mutex_lock(&w->mu);
  last = (--w->refs == 0);
  pthread_mutex_unlock(&w->mu);

  if(!last)
    return;

  pthread_cond_destroy(&w->cv);
  pthread_mutex_destroy(&w->mu);
  mem_free(w);
}

wm_sync_fetch_t *
wm_sync_fetch_begin(size_t result_sz)
{
  wm_sync_fetch_t *w;

  w = mem_alloc("whenmoon", "sync_fetch", sizeof(*w) + result_sz);

  if(w == NULL)
    return(NULL);

  memset(w, 0, sizeof(*w) + result_sz);
  pthread_mutex_init(&w->mu, NULL);
  pthread_cond_init(&w->cv, NULL);
  w->refs   = 2;
  w->done   = false;
  w->buf_sz = result_sz;

  return(w);
}

void
wm_sync_fetch_complete(wm_sync_fetch_t *w, const void *src, size_t src_sz)
{
  if(w == NULL)
    return;

  pthread_mutex_lock(&w->mu);

  if(src != NULL)
  {
    size_t n = src_sz < w->buf_sz ? src_sz : w->buf_sz;
    memcpy(w->buf, src, n);
  }

  w->done = true;
  pthread_cond_signal(&w->cv);
  pthread_mutex_unlock(&w->mu);

  wm_sync_fetch_release(w);
}

bool
wm_sync_fetch_wait(wm_sync_fetch_t *w, void *out, size_t out_sz,
    uint32_t timeout_ms)
{
  struct timespec deadline;
  int             rc = 0;
  bool            done;

  if(w == NULL)
    return(false);

  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec  += timeout_ms / 1000;
  deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;

  if(deadline.tv_nsec >= 1000000000L)
  {
    deadline.tv_sec  += 1;
    deadline.tv_nsec -= 1000000000L;
  }

  pthread_mutex_lock(&w->mu);

  while(!w->done && rc != ETIMEDOUT)
    rc = pthread_cond_timedwait(&w->cv, &w->mu, &deadline);

  done = w->done;

  if(done && out != NULL)
  {
    size_t n = out_sz < w->buf_sz ? out_sz : w->buf_sz;
    memcpy(out, w->buf, n);
  }

  pthread_mutex_unlock(&w->mu);

  wm_sync_fetch_release(w);
  return(done);
}

const char *
wm_fmt_amount(double v, char *buf, size_t cap)
{
  char *end;
  int   n;

  n = snprintf(buf, cap, "%.10f", v);

  if(n < 0 || (size_t)n >= cap)
    return(buf);

  // Trim trailing zeros, then a bare trailing dot ("1000.0000000000" ->
  // "1000", "0.0000000300" -> "0.00000003").
  end = buf + n - 1;

  while(end > buf && *end == '0')
    *end-- = '\0';

  if(end > buf && *end == '.')
    *end = '\0';

  return(buf);
}
