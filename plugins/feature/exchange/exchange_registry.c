// botmanager — MIT
// Per-exchange registry: name → exchange_t, with the protocol vtable
// stamped at registration time. Mutex-protected; the public surface is
// thread-safe.

#define EXCHANGE_INTERNAL
#include "exchange.h"

#include <stdio.h>
#include <string.h>

// Registry list head + lock.
static exchange_t       *exchange_registry      = NULL;
static pthread_mutex_t   exchange_registry_lock;
static bool              exchange_registry_ready = false;

void
exchange_registry_init(void)
{
  pthread_mutex_init(&exchange_registry_lock, NULL);
  exchange_registry      = NULL;
  exchange_registry_ready = true;
}

void
exchange_registry_destroy(void)
{
  exchange_t *e;

  if(!exchange_registry_ready)
    return;

  pthread_mutex_lock(&exchange_registry_lock);

  e = exchange_registry;
  exchange_registry = NULL;

  pthread_mutex_unlock(&exchange_registry_lock);

  while(e != NULL)
  {
    exchange_t *next = e->next;

    pthread_mutex_destroy(&e->lock);
    mem_free(e);
    e = next;
  }

  pthread_mutex_destroy(&exchange_registry_lock);
  exchange_registry_ready = false;
}

exchange_t *
exchange_find(const char *name)
{
  exchange_t *e;
  exchange_t *found = NULL;

  if(!exchange_registry_ready || name == NULL || name[0] == '\0')
    return(NULL);

  pthread_mutex_lock(&exchange_registry_lock);

  for(e = exchange_registry; e != NULL; e = e->next)
  {
    if(strncmp(e->name, name, sizeof(e->name)) == 0)
    {
      found = e;
      break;
    }
  }

  pthread_mutex_unlock(&exchange_registry_lock);
  return(found);
}

bool
exchange_registry_add(const char *name,
    const exchange_protocol_vtable_t *vt)
{
  exchange_t *e;
  size_t      nlen;

  if(!exchange_registry_ready || name == NULL || vt == NULL)
    return(FAIL);

  nlen = strnlen(name, sizeof(((exchange_t *)0)->name));

  if(nlen == 0 || nlen >= sizeof(((exchange_t *)0)->name))
    return(FAIL);

  // Reject duplicates.
  if(exchange_find(name) != NULL)
  {
    clam(CLAM_WARN, EXCHANGE_CTX,
        "register '%s' rejected: already registered", name);
    return(FAIL);
  }

  e = mem_alloc("exchange.reg", "exch", sizeof(*e));

  if(e == NULL)
    return(FAIL);

  memset(e, 0, sizeof(*e));
  memcpy(e->name, name, nlen);
  e->name[nlen] = '\0';
  e->vt         = vt;

  if(pthread_mutex_init(&e->lock, NULL) != 0)
  {
    mem_free(e);
    return(FAIL);
  }

  exchange_limiter_init(&e->limiter, vt->advertised_rps,
      vt->advertised_burst);

  pthread_mutex_lock(&exchange_registry_lock);
  e->next = exchange_registry;
  exchange_registry = e;
  pthread_mutex_unlock(&exchange_registry_lock);

  clam(CLAM_INFO, EXCHANGE_CTX,
      "registered '%s' rps=%u burst=%u",
      e->name, (unsigned)vt->advertised_rps,
      (unsigned)vt->advertised_burst);

  return(SUCCESS);
}

void
exchange_registry_iterate(exchange_registry_iter_cb_t cb, void *user)
{
  exchange_t *e;

  if(!exchange_registry_ready || cb == NULL)
    return;

  pthread_mutex_lock(&exchange_registry_lock);

  for(e = exchange_registry; e != NULL; e = e->next)
    cb(e, user);

  pthread_mutex_unlock(&exchange_registry_lock);
}

// ------------------------------------------------------------------ //
// Public API: registration                                            //
// ------------------------------------------------------------------ //

bool
exchange_register(const char *name, const exchange_protocol_vtable_t *vt)
{
  return(exchange_registry_add(name, vt));
}

// Mark dead + drain the queue: every pending request is surfaced as
// failure. Caller is the protocol plugin's `deinit`, which by contract
// runs after all consumers have stopped — no new requests should arrive.
void
exchange_unregister(const char *name)
{
  exchange_t     *e;
  exchange_req_t *q_head;

  if(name == NULL)
    return;

  e = exchange_find(name);

  if(e == NULL)
    return;

  pthread_mutex_lock(&e->lock);

  e->dead   = true;
  q_head    = e->q_head;
  e->q_head = NULL;
  e->q_count = 0;

  {
    task_handle_t h = e->pump_handle;

    e->pump_handle = TASK_HANDLE_NONE;

    if(h != TASK_HANDLE_NONE)
      task_cancel(h);
  }

  pthread_mutex_unlock(&e->lock);

  // Fail every queued request off-lock so user callbacks can take other
  // locks without colliding with e->lock.
  while(q_head != NULL)
  {
    exchange_req_t *next = q_head->next;

    q_head->next = NULL;
    exchange_req_fail(q_head, 0, "exchange unregistered");
    q_head = next;
  }

  clam(CLAM_INFO, EXCHANGE_CTX, "unregistered '%s'", name);
}
