// botmanager — MIT
// warm_chain.c — OBS-43: ending a deferred task that re-arms itself.

#include "warm_chain.h"

#include "alloc.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

// One self-rescheduling deferred task. Every field is read and written
// under wm_warm_lock except `ctx`, which is set once at open and never
// rewritten.
struct wm_warm_chain
{
  wm_warm_chain_t *next;

  void            *ctx;
  void           (*release)(void *);

  // The armed deferred, or TASK_HANDLE_NONE while a thread is inside
  // the chain. `name`/`delay_ms`/`cb` are what a refused drain re-arms
  // from.
  task_handle_t    handle;
  char             name[TASK_NAME_SZ];
  uint32_t         delay_ms;
  task_cb_t        cb;

  // A thread holds this chain and will reach arm() or close(). True
  // from open() until the first successful arm, and again from the
  // moment a body is dispatched until its next arm or close. This is
  // the whole question a drain waits on: freeing a busy chain is the
  // use-after-free this file exists to end.
  bool             running;

  // The drain dequeued it before it ever ran, so its ctx is retained
  // and a refused drain re-arms it (D3: a stop() that returns FAIL
  // leaves the plugin exactly as it found it).
  bool             parked;
};

static pthread_mutex_t  wm_warm_lock     = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   wm_warm_idle     = PTHREAD_COND_INITIALIZER;
static wm_warm_chain_t *wm_warm_head     = NULL;
static bool             wm_warm_draining = false;

// Caller holds wm_warm_lock.
static wm_warm_chain_t *
wm_warm_busy(void)
{
  wm_warm_chain_t *c;

  for(c = wm_warm_head; c != NULL; c = c->next)
  {
    if(c->running)
      return(c);
  }

  return(NULL);
}

// Caller holds wm_warm_lock.
static void
wm_warm_unlink(wm_warm_chain_t *c)
{
  wm_warm_chain_t **pp;

  for(pp = &wm_warm_head; *pp != NULL; pp = &(*pp)->next)
  {
    if(*pp == c)
    {
      *pp = c->next;
      return;
    }
  }
}

// Runs with no lock held: `release` is the caller's code and this file
// will not hold a lock across something it does not own.
static void
wm_warm_reap(wm_warm_chain_t *c)
{
  if(c->release != NULL)
    c->release(c->ctx);

  mem_free(c);
}

wm_warm_chain_t *
wm_warm_chain_open(void *ctx, void (*release)(void *))
{
  wm_warm_chain_t *c;

  pthread_mutex_lock(&wm_warm_lock);

  if(wm_warm_draining)
  {
    pthread_mutex_unlock(&wm_warm_lock);
    return(NULL);
  }

  c = mem_alloc("whenmoon", "warm_chain", sizeof(*c));

  memset(c, 0, sizeof(*c));

  c->ctx     = ctx;
  c->release = release;
  c->handle  = TASK_HANDLE_NONE;

  // Busy until it is armed — the opener still holds the pointer.
  c->running = true;

  c->next      = wm_warm_head;
  wm_warm_head = c;

  pthread_mutex_unlock(&wm_warm_lock);

  return(c);
}

void *
wm_warm_chain_ctx(const wm_warm_chain_t *c)
{
  return(c->ctx);
}

bool
wm_warm_chain_arm(wm_warm_chain_t *c, const char *name, uint32_t delay_ms,
    task_cb_t cb)
{
  task_handle_t h;

  pthread_mutex_lock(&wm_warm_lock);

  if(wm_warm_draining)
  {
    pthread_mutex_unlock(&wm_warm_lock);
    return(false);
  }

  strlcpy(c->name, name, sizeof(c->name));
  c->delay_ms = delay_ms;
  c->cb       = cb;

  // task_add_deferred is called UNDER wm_warm_lock, and must stay that
  // way: it takes core's task_lock and nothing in core takes ours, so
  // the order is one-way. Releasing the lock around it would reopen the
  // race this type exists to close — the handle published here is the
  // one a concurrent drain must see.
  h = task_add_deferred(c->name, TASK_ANY, 200, delay_ms, cb, c);

  if(h == TASK_HANDLE_NONE)
  {
    pthread_mutex_unlock(&wm_warm_lock);
    return(false);
  }

  c->handle  = h;
  c->parked  = false;
  c->running = false;

  pthread_mutex_unlock(&wm_warm_lock);

  return(true);
}

bool
wm_warm_chain_enter(wm_warm_chain_t *c)
{
  bool ok;

  pthread_mutex_lock(&wm_warm_lock);

  // The deferred has fired; the handle is spent.
  c->handle  = TASK_HANDLE_NONE;
  c->running = true;
  ok         = !wm_warm_draining;

  pthread_mutex_unlock(&wm_warm_lock);

  return(ok);
}

void
wm_warm_chain_close(wm_warm_chain_t *c)
{
  pthread_mutex_lock(&wm_warm_lock);

  wm_warm_unlink(c);
  pthread_cond_broadcast(&wm_warm_idle);

  pthread_mutex_unlock(&wm_warm_lock);

  wm_warm_reap(c);
}

bool
wm_warm_chain_drain(uint32_t timeout_ms, char *offender, size_t offender_cap)
{
  struct timespec  deadline;
  wm_warm_chain_t *c;
  wm_warm_chain_t *dead;

  if(offender != NULL && offender_cap > 0)
    offender[0] = '\0';

  pthread_mutex_lock(&wm_warm_lock);

  wm_warm_draining = true;

  // Take the queued half off the timer queue. task_cancel returns true
  // only when the task was dequeued WITHOUT ever running (task.h:158),
  // so a false here means a worker is already inside the body and has
  // not reached enter() yet — that chain is busy, and freeing it below
  // would be the use-after-free.
  for(c = wm_warm_head; c != NULL; c = c->next)
  {
    task_handle_t h = c->handle;

    if(h == TASK_HANDLE_NONE)
      continue;

    c->handle = TASK_HANDLE_NONE;

    if(task_cancel(h))
      c->parked = true;

    else
      c->running = true;
  }

  // CLOCK_REALTIME because pthread_cond_timedwait uses the condvar's
  // clock and this file sets no pthread_condattr_t.
  clock_gettime(CLOCK_REALTIME, &deadline);

  deadline.tv_sec  += (time_t)(timeout_ms / 1000u);
  deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;

  if(deadline.tv_nsec >= 1000000000L)
  {
    deadline.tv_sec  += 1;
    deadline.tv_nsec -= 1000000000L;
  }

  // Once wm_warm_draining is set, every body either fails enter() and
  // closes, or completes and fails arm() and closes. No path re-arms,
  // so the busy set only shrinks and this terminates.
  while(wm_warm_busy() != NULL)
  {
    if(pthread_cond_timedwait(&wm_warm_idle, &wm_warm_lock, &deadline)
        == ETIMEDOUT)
      break;
  }

  c = wm_warm_busy();

  if(c != NULL)
  {
    if(offender != NULL && offender_cap > 0)
      strlcpy(offender, c->name, offender_cap);

    // D3: refuse without having destroyed anything. stop() returning
    // FAIL leaves the plugin running, so every chain this drain took
    // off the queue goes back on it exactly as it was found.
    wm_warm_draining = false;

    for(c = wm_warm_head; c != NULL; c = c->next)
    {
      if(!c->parked)
        continue;

      c->handle = task_add_deferred(c->name, TASK_ANY, 200, c->delay_ms,
          c->cb, c);
      c->parked = false;
    }

    pthread_mutex_unlock(&wm_warm_lock);

    return(false);
  }

  // Nothing is busy and nothing is queued: what is left was cancelled
  // before it ran. Detach the list under the lock and reap it outside.
  // wm_warm_draining stays set — the plugin is on its way to deinit(),
  // and only wm_warm_chain_reset re-enables arming.
  dead         = wm_warm_head;
  wm_warm_head = NULL;

  pthread_mutex_unlock(&wm_warm_lock);

  while(dead != NULL)
  {
    wm_warm_chain_t *next = dead->next;

    wm_warm_reap(dead);
    dead = next;
  }

  return(true);
}

void
wm_warm_chain_reset(void)
{
  pthread_mutex_lock(&wm_warm_lock);
  wm_warm_draining = false;
  pthread_mutex_unlock(&wm_warm_lock);
}
