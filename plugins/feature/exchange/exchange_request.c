// botmanager — MIT
// Public exchange_request(): enqueue + event-driven dispatch + retry.
//
// Shape mirrors whenmoon's dl_scheduler.c dispatch loop — retired in
// EX-1 (aaea176), so no path is given: the file is gone, and naming a
// live-looking one would be fiction. Proven in WM-S* live verification:
// no periodic tick;
// dispatch is kicked by enqueue and by completion. Token-bucket math
// lives in exchange_limiter.c, retry policy in exchange_retry.c.

#define EXCHANGE_INTERNAL
#include "exchange.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

// Monotonic ms used for the circuit-breaker deadline. CLOCK_MONOTONIC
// matches the limiter and is immune to wall-clock skew.
static int64_t
exchange_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return((int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000);
}

// ------------------------------------------------------------------ //
// Request lifecycle                                                   //
// ------------------------------------------------------------------ //

static char *
exchange_dup(const char *module, const char *name, const char *s,
    size_t *out_len)
{
  size_t len;
  char  *out;

  if(s == NULL)
  {
    if(out_len != NULL) *out_len = 0;
    return(NULL);
  }

  len = strlen(s);
  out = mem_alloc(module, name, len + 1);

  memcpy(out, s, len);
  out[len] = '\0';

  if(out_len != NULL) *out_len = len;

  return(out);
}

exchange_req_t *
exchange_req_new(exchange_t *exch, uint8_t prio,
    exchange_op_kind_t kind, const char *path, const char *body,
    exchange_response_cb_t cb, void *user)
{
  exchange_req_t *r;

  if(exch == NULL || cb == NULL || path == NULL || path[0] == '\0')
    return(NULL);

  r = mem_alloc("exchange.req", "req", sizeof(*r));

  memset(r, 0, sizeof(*r));
  r->exch    = exch;
  r->prio    = prio;
  r->kind    = kind;
  r->user_cb = cb;
  r->user    = user;

  r->path = exchange_dup("exchange.req", "path", path, NULL);

  if(r->path == NULL)
  {
    mem_free(r);
    return(NULL);
  }

  if(body != NULL && body[0] != '\0')
  {
    r->body = exchange_dup("exchange.req", "body", body, &r->body_len);

    if(r->body == NULL)
    {
      mem_free(r->path);
      mem_free(r);
      return(NULL);
    }
  }

  return(r);
}

void
exchange_req_free(exchange_req_t *r)
{
  const exchange_protocol_vtable_t *vt;

  if(r == NULL)
    return;

  // The protocol handle's lifetime is tied to the abstraction; it is
  // the protocol plugin's job to keep it alive across submit() until
  // free_request() runs. Releasing it here is the abstraction's last
  // step before freeing the request itself. A tombstoned registration
  // has no free_request to call — the driver's own deinit took the
  // handle with it.
  if(r->proto_handle != NULL)
  {
    vt = exchange_vt_snapshot(r->exch);

    if(vt != NULL && vt->free_request != NULL)
      vt->free_request(r->proto_handle);

    r->proto_handle = NULL;
  }

  if(r->path != NULL)
    mem_free(r->path);

  if(r->body != NULL)
    mem_free(r->body);

  mem_free(r);
}

// ------------------------------------------------------------------ //
// Delivery (success + failure)                                        //
// ------------------------------------------------------------------ //

void
exchange_req_succeed(exchange_req_t *r, int http_status,
    const char *body, size_t body_len)
{
  exchange_response_cb_t cb;
  void                  *user;

  if(r == NULL)
    return;

  cb     = r->user_cb;
  user   = r->user;
  r->user_cb = NULL;

  if(cb != NULL)
    cb(http_status, body, body_len, NULL, user);

  exchange_req_free(r);
}

void
exchange_req_fail(exchange_req_t *r, int http_status, const char *err)
{
  exchange_response_cb_t cb;
  void                  *user;

  if(r == NULL)
    return;

  cb     = r->user_cb;
  user   = r->user;
  r->user_cb = NULL;

  if(cb != NULL)
    cb(http_status, NULL, 0,
        err != NULL ? err : "exchange request failed", user);

  exchange_req_free(r);
}

// ------------------------------------------------------------------ //
// Priority queue                                                      //
// ------------------------------------------------------------------ //

// Insert ascending by priority (0 = highest = head). Stable wrt prio.
static void
exchange_q_push_locked(exchange_t *exch, exchange_req_t *r)
{
  exchange_req_t **pp = &exch->q_head;

  while(*pp != NULL && (*pp)->prio <= r->prio)
    pp = &(*pp)->next;

  r->next = *pp;
  *pp = r;
  exch->q_count++;
}

// Pop the highest-priority request whose tier is allowed to consume a
// token. Returns NULL when the queue is empty or no eligible request can
// take a token under the reserved-slot policy. On success the limiter
// has already consumed one token.
//
// Circuit-breaker gate runs first: if a recent burst of failures
// tripped the breaker, refuse every pop until the cooldown deadline
// passes. The stall pump re-enters dispatch at that moment so the
// first queued request becomes the implicit recovery probe.
static exchange_req_t *
exchange_q_pop_eligible_locked(exchange_t *exch)
{
  exchange_req_t **pp;
  exchange_req_t  *r;

  if(exch->breaker_tripped_until_ms > 0
      && exch->breaker_tripped_until_ms > exchange_now_ms())
    return(NULL);

  for(pp = &exch->q_head; *pp != NULL; pp = &(*pp)->next)
  {
    r = *pp;

    if(exchange_limiter_take_locked(&exch->limiter, r->prio))
    {
      *pp = r->next;
      r->next = NULL;
      exch->q_count--;
      exch->limiter.in_flight++;
      return(r);
    }
  }

  return(NULL);
}

// ------------------------------------------------------------------ //
// Stall pump                                                          //
// ------------------------------------------------------------------ //
//
// When exchange_dispatch finishes with a non-empty queue because no
// request could take a token, no future event is guaranteed to wake the
// queue — completions only fire while requests are at the protocol
// transport, and all of those just completed. Without a pump, the
// queue parks until a new submit (or an unrelated completion) calls
// exchange_dispatch again. Symptom: the whenmoon downloader saturates
// the burst on the supervisor stall release, then the next page only
// fires ~60 s later when the supervisor stalls again. (See WM-CD-2.)
//
// Fix: schedule a deferred dispatch ~1 s out whenever pop_eligible
// returns NULL with q_count > 0. `pump_handle` is the bookkeeping that
// keeps us from piling up duplicate timers — at most one outstanding
// per exchange. The cb clears the handle then re-enters dispatch, which
// is safe even if something else (a fresh enqueue or a completion)
// already drained the queue in the meantime.

static void
exchange_pump_cb(task_t *t)
{
  exchange_t *exch = t->data;

  t->state = TASK_ENDED;

  if(exch == NULL)
    return;

  pthread_mutex_lock(&exch->lock);
  exch->pump_handle = TASK_HANDLE_NONE;
  pthread_mutex_unlock(&exch->lock);

  exchange_dispatch(exch);
}

// Arm the stall-pump if not already armed. Caller must hold exch->lock.
//
// When the breaker is tripped, stretch the delay out to the cooldown
// deadline so we don't burn a wakeup every second only to refuse the
// pop again. The bucket-dry path keeps the default 1 s — its dry
// state can change on any monotonic-tick refill.
static void
exchange_arm_pump_locked(exchange_t *exch)
{
  uint32_t delay_ms = EXCHANGE_PUMP_DELAY_MS;

  if(exch == NULL || exch->dead)
    return;

  if(exch->pump_handle != TASK_HANDLE_NONE)
    return;

  if(exch->breaker_tripped_until_ms > 0)
  {
    int64_t remaining = exch->breaker_tripped_until_ms - exchange_now_ms();

    if(remaining > (int64_t)delay_ms)
      delay_ms = (uint32_t)remaining;
  }

  exch->pump_handle = task_add_deferred("exchange_pump", TASK_ANY, 120,
      delay_ms, exchange_pump_cb, exch);
}

// ------------------------------------------------------------------ //
// Retry path                                                          //
// ------------------------------------------------------------------ //

static void
exchange_retry_task_cb(task_t *t)
{
  exchange_req_t *r      = t->data;
  bool            queued = false;

  t->state = TASK_ENDED;

  if(r == NULL)
    return;

  // Re-enqueue at the same priority. The scheduled retry already paid
  // its delay; on re-dispatch we'll fetch a fresh token. The liveness
  // test shares the push's hold: exchange_unregister drains the queue
  // under this lock, so a request pushed after that drain would sit on
  // a tombstone and its caller would never hear back.
  if(r->exch != NULL)
  {
    pthread_mutex_lock(&r->exch->lock);

    queued = (r->exch->vt != NULL);

    if(queued)
      exchange_q_push_locked(r->exch, r);

    pthread_mutex_unlock(&r->exch->lock);
  }

  if(!queued)
  {
    exchange_req_fail(r, 0, "exchange unregistered during retry");
    return;
  }

  exchange_dispatch(r->exch);
}

static void
exchange_arm_retry(exchange_req_t *r)
{
  const exchange_protocol_vtable_t *vt;
  uint32_t                          delay_ms;
  task_handle_t                     h;

  r->attempts++;

  if(r->attempts >= EXCHANGE_RETRY_MAX_ATTEMPTS)
  {
    exchange_req_fail(r, 0, "exchange retry attempts exhausted");
    return;
  }

  delay_ms = exchange_backoff_ms(r->attempts);

  // Free the protocol handle from the prior attempt; the next attempt
  // will rebuild it via vtable.build_request. This is the only spot
  // where we keep the request alive and drop the handle.
  if(r->proto_handle != NULL)
  {
    vt = exchange_vt_snapshot(r->exch);

    if(vt != NULL && vt->free_request != NULL)
      vt->free_request(r->proto_handle);

    r->proto_handle = NULL;
  }

  h = task_add_deferred("exchange_retry", TASK_ANY, 120,
      delay_ms, exchange_retry_task_cb, r);

  if(h == TASK_HANDLE_NONE)
  {
    // Couldn't schedule a retry — surface the failure rather than spin.
    exchange_req_fail(r, 0, "exchange retry: task_add_deferred failed");
    return;
  }

  clam(CLAM_DEBUG2, EXCHANGE_CTX,
      "retry %s prio=%u attempt=%u delay=%ums path='%s'",
      r->exch->name, (unsigned)r->prio,
      (unsigned)r->attempts, (unsigned)delay_ms, r->path);
}

// ------------------------------------------------------------------ //
// Internal completion (hands off vtable.submit's response)            //
// ------------------------------------------------------------------ //

static void
exchange_internal_response_cb(int http_status, const char *body,
    size_t body_len, const char *err, void *user)
{
  exchange_req_t     *r = user;
  exchange_t         *exch;
  exchange_outcome_t  outcome;
  bool                transport_err = (http_status == 0);
  char                errbuf[160];
  const char         *errmsg;

  if(r == NULL)
    return;

  exch = r->exch;

  if(exch == NULL)
  {
    exchange_req_fail(r, 0, "exchange request lost its exchange handle");
    return;
  }

  outcome = exchange_classify_status(http_status, transport_err);

  // One locked section for in-flight bookkeeping + breaker state.
  // RETRY and FAIL both bump the consecutive-fails counter; only OK
  // resets it. Crossing the threshold trips the breaker; a failure
  // after an already-tripped state re-extends the cooldown so the
  // probe loss doesn't immediately re-permit a burst.
  {
    bool tripped_now    = false;
    bool recovered_now  = false;
    uint32_t fails_seen = 0;

    pthread_mutex_lock(&exch->lock);

    if(exch->limiter.in_flight > 0)
      exch->limiter.in_flight--;

    if(http_status == 429)
      exchange_limiter_penalty_locked(&exch->limiter, 2.0);

    if(outcome == EXCHANGE_OUTCOME_OK)
    {
      if(exch->breaker_tripped_until_ms != 0
          || exch->breaker_consec_fails >= EXCHANGE_BREAKER_THRESHOLD)
        recovered_now = true;

      fails_seen                     = exch->breaker_consec_fails;
      exch->breaker_consec_fails     = 0;
      exch->breaker_tripped_until_ms = 0;
    }

    else
    {
      exch->breaker_consec_fails++;
      fails_seen = exch->breaker_consec_fails;

      if(fails_seen >= EXCHANGE_BREAKER_THRESHOLD)
      {
        if(exch->breaker_tripped_until_ms == 0)
          tripped_now = true;

        exch->breaker_tripped_until_ms = exchange_now_ms()
            + EXCHANGE_BREAKER_COOLDOWN_MS;
      }
    }

    pthread_mutex_unlock(&exch->lock);

    if(tripped_now)
      clam(CLAM_WARN, EXCHANGE_CTX,
          "'%s' circuit breaker tripped (%u consecutive failures); "
          "dispatch paused for %u ms",
          exch->name, (unsigned)fails_seen,
          (unsigned)EXCHANGE_BREAKER_COOLDOWN_MS);

    else if(recovered_now)
      clam(CLAM_INFO, EXCHANGE_CTX,
          "'%s' circuit breaker recovered after %u failure(s)",
          exch->name, (unsigned)fails_seen);
  }

  switch(outcome)
  {
    case EXCHANGE_OUTCOME_OK:
      exchange_req_succeed(r, http_status, body, body_len);
      break;

    case EXCHANGE_OUTCOME_RETRY:
      exchange_arm_retry(r);
      break;

    case EXCHANGE_OUTCOME_FAIL:
    default:
      errmsg = exchange_describe_status(http_status, transport_err,
          err, errbuf, sizeof(errbuf));
      exchange_req_fail(r, http_status,
          errmsg != NULL ? errmsg : "exchange request failed");
      break;
  }

  // Keep the chain moving regardless of branch — completion frees a
  // limiter slot which may unblock a waiting request.
  exchange_dispatch(exch);
}

// ------------------------------------------------------------------ //
// Dispatch loop                                                       //
// ------------------------------------------------------------------ //

void
exchange_dispatch(exchange_t *exch)
{
  const exchange_protocol_vtable_t *vt;
  exchange_req_t                   *r;
  bool                              built;
  bool                              submitted;

  if(exch == NULL)
    return;

  // One hold for the vtable snapshot and the pop it will serve, so the
  // request and the driver it is about to reach come from the same
  // registration. A tombstone (vt == NULL) has already had its queue
  // drained by exchange_unregister: nothing to pop, nothing to pump.
  pthread_mutex_lock(&exch->lock);

  vt = exch->vt;
  r  = (vt != NULL) ? exchange_q_pop_eligible_locked(exch) : NULL;

  // Pop returned NULL with a non-empty queue: tokens are dry, no
  // request could be promoted. Arm the stall pump so we re-enter
  // dispatch once the bucket has had time to refill — see the long
  // comment above exchange_pump_cb. Cheap when armed; idempotent.
  if(r == NULL && vt != NULL && exch->q_count > 0)
    exchange_arm_pump_locked(exch);

  pthread_mutex_unlock(&exch->lock);

  if(r == NULL)
    return;

  // Off-lock vtable calls, through the snapshot — protocol plugin owns
  // its own threading.
  if(vt->build_request == NULL || vt->submit == NULL)
  {
    exchange_req_fail(r, 0, "exchange protocol vtable invalid");

    // Refund the token so the queue keeps moving.
    pthread_mutex_lock(&exch->lock);

    if(exch->limiter.in_flight > 0)
      exch->limiter.in_flight--;

    exchange_limiter_refund_locked(&exch->limiter);
    pthread_mutex_unlock(&exch->lock);
    return;
  }

  built = vt->build_request(r->kind, r->path, r->body,
      &r->proto_handle);

  if(built != SUCCESS || r->proto_handle == NULL)
  {
    exchange_req_fail(r, 0, "exchange build_request failed");

    pthread_mutex_lock(&exch->lock);

    if(exch->limiter.in_flight > 0)
      exch->limiter.in_flight--;

    exchange_limiter_refund_locked(&exch->limiter);
    pthread_mutex_unlock(&exch->lock);
    return;
  }

  submitted = vt->submit(r->proto_handle, r->prio,
      exchange_internal_response_cb, r);

  if(submitted != SUCCESS)
  {
    // submit failed synchronously; the abstraction owns the protocol
    // handle and frees it via exchange_req_free. The internal callback
    // will not fire — we must surface the failure ourselves.
    exchange_req_fail(r, 0, "exchange submit failed");

    pthread_mutex_lock(&exch->lock);

    if(exch->limiter.in_flight > 0)
      exch->limiter.in_flight--;

    exchange_limiter_refund_locked(&exch->limiter);
    pthread_mutex_unlock(&exch->lock);

    // The next eligible request may proceed.
    exchange_dispatch(exch);
    return;
  }

  // submit() accepted the handle. The protocol plugin's curl completion
  // will route through exchange_internal_response_cb. Tail-call into
  // dispatch in case more queued requests can take additional tokens
  // (depends on burst depth).
  exchange_dispatch(exch);
}

// ------------------------------------------------------------------ //
// Public entry point                                                  //
// ------------------------------------------------------------------ //

bool
exchange_request(const char *exchange, uint8_t prio,
    exchange_op_kind_t kind, const char *path, const char *body_json,
    exchange_response_cb_t cb, void *user)
{
  exchange_t     *exch;
  exchange_req_t *r;
  bool            queued;

  if(cb == NULL || path == NULL || path[0] == '\0')
    return(FAIL);

  exch = exchange_find(exchange);

  if(exch == NULL)
  {
    clam(CLAM_WARN, EXCHANGE_CTX,
        "request to unknown exchange '%s'",
        exchange != NULL ? exchange : "(null)");
    return(FAIL);
  }

  r = exchange_req_new(exch, prio, kind, path, body_json, cb, user);

  if(r == NULL)
    return(FAIL);

  // The tombstone test shares the push's hold, for the reason
  // exchange_retry_task_cb states: exchange_unregister drains this queue
  // under this lock, and a request pushed after the drain is never
  // completed. FAIL here means the callback did not fire and never will,
  // which is this entry point's documented contract.
  pthread_mutex_lock(&exch->lock);

  queued = (exch->vt != NULL);

  if(queued)
    exchange_q_push_locked(exch, r);

  pthread_mutex_unlock(&exch->lock);

  if(!queued)
  {
    exchange_req_free(r);
    return(FAIL);
  }

  exchange_dispatch(exch);
  return(SUCCESS);
}
