// botmanager — MIT
// Per-plugin in-flight set: cancel-and-drain from stop() so a completion
// callback cannot outlive the deinit() it would read.

#include "curl_flight.h"

#include "alloc.h"
#include "common.h"

#include <errno.h>
#include <time.h>

#define CURL_FLIGHT_CAP0 8

// Caller holds f->mu. Every live slot carries a distinct value — a curl
// id or a minted token — so this finds at most one, and the order of the
// array is never meaningful. A handle of 0 matches nothing by
// construction.
static uint32_t
curl_flight_find_locked(const curl_flight_t *f, uint64_t id)
{
  for(uint32_t i = 0; i < f->n; i++)
    if(f->slots[i] == id)
      return(i);

  return(f->n);
}

// Caller holds f->mu.
static uint64_t
curl_flight_open_locked(curl_flight_t *f)
{
  if(f->n == f->cap)
  {
    f->cap = f->cap != 0 ? f->cap * 2 : CURL_FLIGHT_CAP0;
    f->slots = f->slots != NULL
        ? mem_realloc(f->slots, f->cap * sizeof(*f->slots))
        : mem_alloc("curl", "flight", f->cap * sizeof(*f->slots));
  }

  f->slots[f->n] = CURL_FLIGHT_IDLE | ++f->seq;

  return(f->slots[f->n++]);
}

void
curl_flight_init(curl_flight_t *f)
{
  if(f == NULL)
    return;

  pthread_mutex_init(&f->mu, NULL);
  pthread_cond_init(&f->idle, NULL);

  f->slots    = NULL;
  f->n        = 0;
  f->cap      = 0;
  f->seq      = 0;
  f->grounded = false;
}

bool
curl_flight_open(curl_flight_t *f, uint64_t *slot)
{
  if(f == NULL || slot == NULL)
    return(FAIL);

  *slot = 0;

  pthread_mutex_lock(&f->mu);

  if(f->grounded)
  {
    pthread_mutex_unlock(&f->mu);
    return(FAIL);
  }

  *slot = curl_flight_open_locked(f);

  pthread_mutex_unlock(&f->mu);

  return(SUCCESS);
}

bool
curl_flight_relay(curl_flight_t *f, curl_request_t *req, uint64_t *slot)
{
  uint64_t id;
  uint32_t i;

  if(f == NULL || req == NULL || slot == NULL)
    return(FAIL);

  id = curl_request_id(req);

  pthread_mutex_lock(&f->mu);

  if(f->grounded)
  {
    pthread_mutex_unlock(&f->mu);
    curl_request_abandon(req);
    return(FAIL);
  }

  i = curl_flight_find_locked(f, *slot);

  if(i < f->n)
    f->slots[i] = id;

  *slot = id;

  pthread_mutex_unlock(&f->mu);

  if(curl_request_submit(req) != SUCCESS)
  {
    pthread_mutex_lock(&f->mu);

    i = curl_flight_find_locked(f, id);

    if(i < f->n)
    {
      f->slots[i] = CURL_FLIGHT_IDLE | ++f->seq;
      *slot       = f->slots[i];
    }

    pthread_mutex_unlock(&f->mu);

    return(FAIL);
  }

  return(SUCCESS);
}

void
curl_flight_close(curl_flight_t *f, uint64_t slot)
{
  uint32_t i;

  if(f == NULL)
    return;

  pthread_mutex_lock(&f->mu);

  i = curl_flight_find_locked(f, slot);

  if(i < f->n)
  {
    // The departing entry takes the last one's id rather than shifting
    // the tail down — nothing here is ordered.
    f->slots[i] = f->slots[--f->n];

    if(f->n == 0)
      pthread_cond_broadcast(&f->idle);
  }

  pthread_mutex_unlock(&f->mu);
}

uint32_t
curl_flight_drain(curl_flight_t *f, uint32_t ms)
{
  struct timespec deadline;
  uint32_t        left;

  if(f == NULL)
    return(0);

  clock_gettime(CLOCK_REALTIME, &deadline);

  deadline.tv_sec  += (time_t)(ms / 1000U);
  deadline.tv_nsec += (long)(ms % 1000U) * 1000000L;

  if(deadline.tv_nsec >= 1000000000L)
  {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }

  pthread_mutex_lock(&f->mu);

  f->grounded = true;

  while(f->n > 0)
  {
    // curl_request_cancel walks curl's own two lists and writes an
    // eventfd. It cannot re-enter this flight, and curl holds no lock of
    // its own across a completion callback, so f->mu -> curl's locks is
    // the only order that exists. A slot still at 0 has nothing on the
    // wire to cancel; it closes when its own path reaches the end.
    for(uint32_t i = 0; i < f->n; i++)
      if((f->slots[i] & CURL_FLIGHT_IDLE) == 0)
        curl_request_cancel(f->slots[i]);

    if(pthread_cond_timedwait(&f->idle, &f->mu, &deadline) == ETIMEDOUT)
      break;
  }

  left = f->n;

  // A flight that did not empty stays open. The caller's stop() is about
  // to refuse the unload over exactly this count, and a refused plugin
  // is left running and intact — which it would not be if it spent the
  // rest of its life turning its own callers away.
  if(left > 0)
    f->grounded = false;

  pthread_mutex_unlock(&f->mu);

  return(left);
}

void
curl_flight_destroy(curl_flight_t *f)
{
  if(f == NULL)
    return;

  pthread_mutex_lock(&f->mu);

  if(f->slots != NULL)
    mem_free(f->slots);

  f->slots = NULL;
  f->n     = 0;
  f->cap   = 0;

  pthread_mutex_unlock(&f->mu);

  pthread_mutex_destroy(&f->mu);
  pthread_cond_destroy(&f->idle);
}
