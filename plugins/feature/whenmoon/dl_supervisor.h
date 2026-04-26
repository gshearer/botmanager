// dl_supervisor.h — DL-1 stall-watchdog for whenmoon download jobs.
// Internal; WHENMOON_INTERNAL-gated.
//
// The supervisor is a dynamic periodic task that materialises when the
// in-memory job table grows from zero and dies once the table empties.
// Each tick walks the jobtable looking for in-flight RUNNING jobs whose
// `last_progress_ms` is older than WM_DL_STALL_THRESHOLD_MS. Stalled
// jobs have their in-flight slot released so wm_dl_kick can re-dispatch
// them; the original request's completion callback (if it ever fires)
// is harmless — the job's state machine is idempotent on a duplicate
// page.
//
// The supervisor exists to recover from genuinely stuck states; the
// exchange-layer curl timeouts + EX-1 retry chain (250 .. 4000 ms, 5
// attempts) cover ordinary network/server flakes without it ever
// getting involved.

#ifndef BM_WHENMOON_DL_SUPERVISOR_H
#define BM_WHENMOON_DL_SUPERVISOR_H

#ifdef WHENMOON_INTERNAL

#include "dl_jobtable.h"

// Periodic tick interval. The minimum effective value enforced by
// task_add_periodic is 1000 ms; 5000 keeps wakeup churn negligible
// while still giving stuck jobs a bounded recovery latency.
#define WM_DL_SUPERVISOR_TICK_MS  5000

// A job whose last_progress_ms is older than this is considered stalled.
// 60 s is generous: the longest legitimate per-page wait on coinbase is
// the EX-1 retry budget (250+500+1000+2000+4000 = 7.75 s) plus per-call
// curl timeout, comfortably under the threshold.
#define WM_DL_STALL_THRESHOLD_MS  60000

// Spawn the supervisor task if it isn't already running. Idempotent;
// safe to call from any path that grows the job list (enqueue, restore).
// The supervisor's reference to the jobtable is captured here.
void wm_dl_supervisor_kick(dl_jobtable_t *t);

// If the jobtable is now empty, cancel the supervisor task. Called
// after a job is removed from the in-memory list. Returns immediately;
// task_cancel does not wait for an in-progress callback.
void wm_dl_supervisor_check(dl_jobtable_t *t);

// Forced teardown: cancel the supervisor and block until any in-flight
// tick finishes touching the jobtable. Called from
// wm_dl_jobtable_destroy *before* the jobtable is freed so the tick
// cannot dereference torn-down memory.
void wm_dl_supervisor_drain(void);

#endif // WHENMOON_INTERNAL
#endif // BM_WHENMOON_DL_SUPERVISOR_H
