// botmanager — MIT
// DL-1 stall watchdog for whenmoon download jobs.
//
// Lifecycle: a single periodic task that wm_dl_supervisor_kick() spawns
// when the jobtable grows from zero, and wm_dl_supervisor_check() /
// wm_dl_supervisor_drain() retire when the table empties or the plugin
// tears down. Static singleton state — only one whenmoon plugin
// instance, only one jobtable, only one supervisor.
//
// On each tick we walk the jobtable looking for in-flight RUNNING jobs
// whose last_progress_ms has aged past WM_DL_STALL_THRESHOLD_MS. For
// each, we log a warning, drop the in-flight slot (so wm_dl_kick can
// re-pick the job), refresh last_progress_ms (so we don't immediately
// re-fire on the next tick), and kick the dispatcher.
//
// Race window: if a stalled request later returns, its completion
// callback re-resolves the job under the table lock and updates state
// as usual. Re-dispatching can produce a duplicate page request; the
// per-table primary keys absorb duplicates and the cursor stays
// monotone. This is best-effort recovery — the alternative (a job stuck
// forever) is strictly worse.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "dl_jobtable.h"
#include "dl_schema.h"
#include "dl_supervisor.h"

#include "clam.h"
#include "pool.h"
#include "task.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

// ------------------------------------------------------------------ //
// Static singleton state                                              //
// ------------------------------------------------------------------ //

static pthread_mutex_t  s_lock      = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   s_tick_done = PTHREAD_COND_INITIALIZER;
static dl_jobtable_t   *s_jt        = NULL;
static task_handle_t    s_handle    = TASK_HANDLE_NONE;

// True between the start of a tick callback and its return — drain
// uses this to avoid freeing the jobtable out from under a running tick.
static bool             s_in_tick   = false;

// ------------------------------------------------------------------ //
// Periodic tick                                                       //
// ------------------------------------------------------------------ //

static void
wm_dl_supervisor_tick(task_t *t)
{
  dl_jobtable_t *jt;
  dl_job_t      *j;
  int64_t        now_ms;
  uint32_t       recovered        = 0;
  bool           unfilled_running = false;

  pthread_mutex_lock(&s_lock);
  s_in_tick = true;
  jt        = s_jt;
  pthread_mutex_unlock(&s_lock);

  if(jt == NULL || pool_shutting_down())
    goto done;

  now_ms = wm_dl_now_ms();

  pthread_mutex_lock(&jt->lock);

  if(jt->destroying)
  {
    pthread_mutex_unlock(&jt->lock);
    goto done;
  }

  for(j = jt->jobs_head; j != NULL; j = j->next)
  {
    int64_t age;

    // WM-DL-RACE-1: a job in RUNNING+!in_flight is the post-bail state
    // wm_dl_kick leaves behind on a synchronous dispatch_one failure.
    // Without a re-kick, a "lone" job with no siblings driving
    // completion callbacks would sit here forever (the existing stall
    // check below filters by in_flight, so it ignores zombies). Note
    // it now and fire one kick at the tail of the tick.
    if(!j->in_flight)
    {
      if(j->state == DL_JOB_RUNNING)
        unfilled_running = true;
      continue;
    }

    age = now_ms - j->last_progress_ms;

    if(age <= WM_DL_STALL_THRESHOLD_MS)
      continue;

    clam(CLAM_WARN, WM_DL_CTX,
        "stall: job %" PRId64 " (%s, state=%d) idle %" PRId64
        " ms; releasing slot",
        j->id, j->exchange_symbol, (int)j->state, age);

    j->in_flight        = false;
    j->last_progress_ms = now_ms;

    if(jt->in_flight_count > 0)
      jt->in_flight_count--;

    // Only RUNNING jobs need re-dispatch — DONE / FAILED jobs whose
    // in-flight request is being abandoned just need their slot freed
    // so wm_dl_remove_completed can reap them on the way out of the
    // tick.
    if(j->state == DL_JOB_RUNNING)
      recovered++;
  }

  pthread_mutex_unlock(&jt->lock);

  if(recovered > 0 || unfilled_running)
    wm_dl_kick(jt);

  // Reap any FAILED / DONE jobs whose in-flight slot we just freed.
  // wm_dl_remove_completed is a no-op when nothing reapable is on the
  // list and a no-op (returning early) when the list isn't empty after
  // the walk — cheap to call every tick.
  wm_dl_remove_completed(jt);

done:
  pthread_mutex_lock(&s_lock);
  s_in_tick = false;
  pthread_cond_broadcast(&s_tick_done);
  pthread_mutex_unlock(&s_lock);

  // Periodic: TASK_ENDED means "iteration done, reschedule". The task
  // engine honours s_handle's cancelled flag and frees on the next
  // finish if cancellation has been requested.
  t->state = TASK_ENDED;
}

// ------------------------------------------------------------------ //
// Public surface                                                      //
// ------------------------------------------------------------------ //

void
wm_dl_supervisor_kick(dl_jobtable_t *t)
{
  if(t == NULL)
    return;

  pthread_mutex_lock(&s_lock);

  if(s_handle == TASK_HANDLE_NONE)
  {
    s_jt     = t;
    s_handle = task_add_periodic("wm_dl_supervisor", TASK_ANY, 100,
                  WM_DL_SUPERVISOR_TICK_MS, wm_dl_supervisor_tick, NULL);

    if(s_handle == TASK_HANDLE_NONE)
    {
      clam(CLAM_WARN, WM_DL_CTX,
          "supervisor task spawn failed; stall recovery disabled");
      s_jt = NULL;
    }

    else
      clam(CLAM_INFO, WM_DL_CTX,
          "supervisor armed (tick=%u ms, stall_threshold=%u ms)",
          (unsigned)WM_DL_SUPERVISOR_TICK_MS,
          (unsigned)WM_DL_STALL_THRESHOLD_MS);
  }

  pthread_mutex_unlock(&s_lock);
}

void
wm_dl_supervisor_check(dl_jobtable_t *t)
{
  bool empty;

  if(t == NULL)
    return;

  pthread_mutex_lock(&t->lock);
  empty = (t->jobs_head == NULL);
  pthread_mutex_unlock(&t->lock);

  if(!empty)
    return;

  pthread_mutex_lock(&s_lock);

  if(s_handle != TASK_HANDLE_NONE)
  {
    task_cancel(s_handle);
    s_handle = TASK_HANDLE_NONE;
    s_jt     = NULL;

    clam(CLAM_INFO, WM_DL_CTX, "supervisor disarmed (job table empty)");
  }

  pthread_mutex_unlock(&s_lock);
}

void
wm_dl_supervisor_drain(void)
{
  pthread_mutex_lock(&s_lock);

  if(s_handle != TASK_HANDLE_NONE)
  {
    task_cancel(s_handle);
    s_handle = TASK_HANDLE_NONE;
  }

  // Wait for any in-progress tick to finish touching the jobtable
  // before the caller proceeds to free it. task_cancel does not block;
  // s_in_tick is the explicit barrier.
  while(s_in_tick)
    pthread_cond_wait(&s_tick_done, &s_lock);

  s_jt = NULL;

  pthread_mutex_unlock(&s_lock);
}
