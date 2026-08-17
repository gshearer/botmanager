// botmanager — MIT
// the chat plugin's live-record list: what a teardown drains because
// core's quiescence barrier cannot see it.

#include "hold.h"

#include "clam.h"
#include "common.h"
#include "curl.h"
#include "inference.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>

// Module-level rather than a field on the owner, which is the whole
// point: these records outlive the thing they name, and the list is
// what a teardown uses to make sure they no longer do. The mutex is
// leaf — nothing under it calls back into chat — and the one engine
// call made while holding it (llm_cancel_user) walks curl's own lists
// and writes an eventfd, so no delivery is ever waiting on us.

static pthread_mutex_t hold_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  hold_cond  = PTHREAD_COND_INITIALIZER;
static chatbot_hold_t *hold_head  = NULL;

void
chatbot_hold_link(chatbot_hold_t *h, const void *owner,
    const void *llm_user)
{
  pthread_mutex_lock(&hold_mutex);

  h->owner    = owner;
  h->llm_user = llm_user;
  h->curl_id  = 0;
  h->disowned = false;
  h->next     = hold_head;
  hold_head   = h;

  pthread_mutex_unlock(&hold_mutex);
}

void
chatbot_hold_set_curl(chatbot_hold_t *h, uint64_t curl_id)
{
  pthread_mutex_lock(&hold_mutex);
  h->curl_id = curl_id;
  pthread_mutex_unlock(&hold_mutex);
}

bool
chatbot_hold_disowned(const chatbot_hold_t *h)
{
  bool disowned;

  pthread_mutex_lock(&hold_mutex);
  disowned = h->disowned;
  pthread_mutex_unlock(&hold_mutex);

  return(disowned);
}

void
chatbot_hold_unlink(chatbot_hold_t *h)
{
  chatbot_hold_t **pp;

  pthread_mutex_lock(&hold_mutex);

  for(pp = &hold_head; *pp != NULL; pp = &(*pp)->next)
    if(*pp == h)
    {
      *pp = h->next;
      break;
    }

  h->next  = NULL;
  h->owner = NULL;

  pthread_cond_broadcast(&hold_cond);
  pthread_mutex_unlock(&hold_mutex);
}

// Nothing here frees a record: a record is only ever freed by the
// callback holding it, which is exactly why the wait exists. A callback
// that had already read `disowned` as false keeps its record on the list
// for the whole of its body, so finding none is proof that no thread is
// still inside one — and a callback that re-arms another async leg
// leaves the record listed for the next one to disown.
//
// A record linked *after* the disown pass is still waited for:
// uncancelled, but waited for, which is what safety needs. What makes
// the wait converge is the caller — memory_stop() raises the backfill's
// stopping flag before it drains, so nothing new can be submitted.
void
chatbot_hold_shutdown(const void *owner)
{
  struct timespec deadline;
  uint32_t        disowned  = 0;
  uint32_t        cancelled = 0;
  uint32_t        stranded  = 0;
  bool            waiting;

  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += CHATBOT_HOLD_DRAIN_SECS;

  pthread_mutex_lock(&hold_mutex);

  for(chatbot_hold_t *h = hold_head; h != NULL; h = h->next)
  {
    if(h->owner != owner)
      continue;

    h->disowned = true;
    disowned++;

    if(h->llm_user != NULL)
      cancelled += llm_cancel_user(h->llm_user);

    if(h->curl_id != 0 && curl_request_cancel(h->curl_id) == SUCCESS)
      cancelled++;
  }

  for(;;)
  {
    waiting = false;

    for(chatbot_hold_t *h = hold_head; h != NULL; h = h->next)
      if(h->owner == owner)
      {
        waiting = true;
        break;
      }

    if(!waiting)
      break;

    if(pthread_cond_timedwait(&hold_cond, &hold_mutex, &deadline)
        == ETIMEDOUT)
      break;
  }

  for(chatbot_hold_t *h = hold_head; h != NULL; h = h->next)
    if(h->owner == owner)
      stranded++;

  pthread_mutex_unlock(&hold_mutex);

  if(stranded > 0)
    clam(CLAM_WARN, "chatbot",
        "%u airborne record(s) did not come back within %u s; their "
        "owner is freed with them still in flight", stranded,
        CHATBOT_HOLD_DRAIN_SECS);

  else if(disowned > 0)
    clam(CLAM_DEBUG, "chatbot",
        "disowned %u airborne record(s) (%u cancelled at their "
        "engine); all came back", disowned, cancelled);
}
