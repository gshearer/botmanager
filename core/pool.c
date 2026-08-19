// botmanager — MIT
// Elastic worker pool plus dedicated threads for persistent tasks.
#define _GNU_SOURCE
#define POOL_INTERNAL
#include "pool.h"

// Reap any retired worker threads (join and free slot).
// Must be called with pool_mutex held.
static void
reap_retired_locked(void)
{
  for(uint16_t i = 1; i <= pool_cfg.max_threads; i++)
  {
    if(workers[i].wstate == WORKER_RETIRING)
    {
      // pthread_join may briefly block but the thread has already exited.
      pthread_mutex_unlock(&pool_mutex);
      pthread_join(workers[i].thread, NULL);
      pthread_mutex_lock(&pool_mutex);
      workers[i].wstate = WORKER_UNUSED;
    }
  }
}

// Create a worker thread in an unused slot.
// Must be called with pool_mutex held.
static bool
spawn_worker_locked(void)
{
  reap_retired_locked();

  for(uint16_t i = 1; i <= pool_cfg.max_threads; i++)
  {
    if(workers[i].wstate == WORKER_UNUSED)
    {
      workers[i].id          = (uint8_t)i;
      workers[i].jobs        = 0;
      workers[i].created     = time(NULL);
      workers[i].last_active = workers[i].created;
      workers[i].wstate      = WORKER_RUNNING;
      workers[i].idle        = false;

      if(pthread_create(&workers[i].thread, NULL,
              worker_entry, &workers[i]) != 0)
      {
        workers[i].wstate = WORKER_UNUSED;
        return(false);
      }

      pool_size++;

      if(pool_size > pool_peak)
        pool_peak = pool_size;

      return(true);
    }
  }

  return(false);
}

static bool
should_retire(worker_t *w)
{
  bool can_retire;

  if(w->id == 0)
    return(false);

  pthread_mutex_lock(&pool_mutex);

  can_retire = (pool_size > pool_cfg.min_threads)
      && (pool_idle >= pool_cfg.min_spare);

  pthread_mutex_unlock(&pool_mutex);

  if(!can_retire)
    return(false);

  return((time(NULL) - w->last_active) > (time_t)pool_cfg.max_idle_secs);
}

// Check if spare workers are below threshold and spawn if needed.
// Called after a worker picks up work.
static void
check_spare(void)
{
  pthread_mutex_lock(&pool_mutex);

  if(pool_idle < pool_cfg.min_spare
      && pool_size < pool_cfg.max_threads)
  {
    if(spawn_worker_locked())
    {
      // Both counters are snapshotted here: reading pool_idle after the
      // unlock raced every worker's guarded pool_idle++ for one log line.
      uint16_t n     = pool_size;
      uint16_t spare = pool_idle;

      pthread_mutex_unlock(&pool_mutex);
      clam(CLAM_DEBUG, "pool", "scaled to %u workers (spare: %u)",
          n, spare);
      return;
    }
  }

  pthread_mutex_unlock(&pool_mutex);
}

// Claim one of the background slots. A worker holds a slot for exactly
// as long as it runs a background task, so the count is the number of
// workers a flood of background work can occupy — every worker above it
// stays reachable by a command.
//
// Claimed BEFORE the queue walk rather than after it: sixty-four
// workers waking together would each see room and each take background
// work, which is the flood this exists to bound.
static bool
pool_bg_claim(void)
{
  bool got;

  pthread_mutex_lock(&pool_mutex);
  got = (pool_bg < pool_cfg.bg_budget);

  if(got)
    pool_bg++;

  pthread_mutex_unlock(&pool_mutex);
  return(got);
}

static void
pool_bg_release(void)
{
  pthread_mutex_lock(&pool_mutex);
  pool_bg--;
  pthread_mutex_unlock(&pool_mutex);
}

static void *
worker_entry(void *arg)
{
  worker_t *w = (worker_t *)arg;
  uint8_t id = w->id;
  task_type_t type = (id == 0) ? TASK_PARENT : TASK_THREAD;
  uint8_t  exit_id;
  uint64_t exit_jobs;

  clam(CLAM_DEBUG, "pool", "worker %u started", id);

  while(!pool_stopping)
  {
    task_t       *t;
    task_state_t  result;
    bool          claimed;
    bool          background;

    claimed    = pool_bg_claim();
    t          = task_assign(type, claimed);
    background = (t != NULL && t->lane == TASK_BACKGROUND);

    // A claim is permission to take background work, not a hold on the
    // work taken: a worker that took none, or took an interactive task,
    // gives its slot back before it can shut another worker out.
    if(claimed && !background)
      pool_bg_release();

    if(t == NULL)
    {
      if(should_retire(w))
        break;

      // Mark idle, wait for work, mark not idle.
      pthread_mutex_lock(&pool_mutex);
      w->idle = true;
      pool_idle++;
      pthread_mutex_unlock(&pool_mutex);

      task_wait(pool_cfg.wait_ms);

      pthread_mutex_lock(&pool_mutex);
      w->idle = false;
      pool_idle--;
      pthread_mutex_unlock(&pool_mutex);

      continue;
    }

    // Got work. Check if we need more workers.
    check_spare();

    // Execute the callback.
    t->cb(t);
    result = task_finish(t);

    if(background)
      pool_bg_release();

    w->jobs++;
    w->last_active = time(NULL);

    if(result == TASK_FATAL)
    {
      pool_shutdown();
      break;
    }
  }

  // Worker is exiting.
  exit_id   = w->id;
  exit_jobs = w->jobs;

  pthread_mutex_lock(&pool_mutex);
  w->wstate = WORKER_RETIRING;
  pool_size--;

  if(w->idle)
  {
    pool_idle--;
    w->idle = false;
  }

  pthread_mutex_unlock(&pool_mutex);

  clam(CLAM_DEBUG, "pool", "worker %u exiting (jobs: %lu)", exit_id, exit_jobs);
  return(NULL);
}

// Hand this thread's persist slot back as joinable. Called as the very
// last act of persist_entry so no lock is wanted after it.
static void
persist_retire_self(void)
{
  pthread_mutex_lock(&pool_mutex);

  for(uint16_t i = 0; i < POOL_MAX_PERSIST; i++)
  {
    if(persist_workers[i].wstate == WORKER_RUNNING
        && pthread_equal(persist_workers[i].thread, pthread_self()))
    {
      persist_workers[i].wstate = WORKER_RETIRING;
      break;
    }
  }

  pthread_mutex_unlock(&pool_mutex);
}

// Join and free every slot whose thread has already exited. Mirrors
// reap_retired_locked() for the elastic pool: without it a plugin that
// unloads and reloads burns a persist slot per cycle.
// Must be called with pool_mutex held; releases it around each join.
static void
reap_persist_locked(void)
{
  for(uint16_t i = 0; i < POOL_MAX_PERSIST; i++)
  {
    if(persist_workers[i].wstate == WORKER_RETIRING)
    {
      persist_workers[i].wstate = WORKER_JOINING;

      pthread_mutex_unlock(&pool_mutex);
      pthread_join(persist_workers[i].thread, NULL);
      pthread_mutex_lock(&pool_mutex);

      persist_workers[i].wstate  = WORKER_UNUSED;
      persist_workers[i].task_id = TASK_HANDLE_NONE;
    }
  }
}

static void *
persist_entry(void *arg)
{
  task_t       *t = (task_t *)arg;
  task_state_t  result;

  t->state    = TASK_RUNNING;
  t->last_run = time(NULL);
  t->run_count++;

  clam(CLAM_DEBUG, "pool", "persist thread started for '%s'", t->name);

  // Execute the callback (blocks until the task exits its loop).
  t->cb(t);

  result = t->state;

  clam(CLAM_DEBUG, "pool", "persist thread '%s' exiting (state: %s)",
      t->name, task_state_name(result));

  // Use task_finish to handle stats cleanup and free the task.
  task_finish(t);

  // Decrement persist thread count in the pool.
  pthread_mutex_lock(&pool_mutex);
  persist_count--;
  pthread_mutex_unlock(&pool_mutex);

  if(result == TASK_FATAL)
    pool_shutdown();

  // Retiring is this thread's last act, so a joiner holding pool_mutex
  // can never be waiting on a thread that still wants the lock.
  persist_retire_self();

  return(NULL);
}

// Public API

// Recompute how many workers may run background tasks at once. The
// reserve is what is left for commands, so it cannot take the last
// worker: a budget of zero is not a reservation, it is a pool that
// never resolves a name again.
static void
pool_bg_budget_apply(void)
{
  uint16_t max     = pool_cfg.max_threads;
  uint16_t reserve = pool_cfg.reserve;

  if(reserve >= max)
  {
    clam(CLAM_WARN, "pool",
        "reserve_interactive %u leaves no worker for background work "
        "with max_threads %u — reserving %u",
        reserve, max, (unsigned)(max - 1));
    reserve = (uint16_t)(max - 1);
  }

  pool_cfg.reserve   = reserve;
  pool_cfg.bg_budget = (uint16_t)(max - reserve);
}

// Override pool limits. Must be called before pool_init().
// min_spare: minimum idle workers before scaling up
// max_idle_secs: seconds before an idle worker retires
void
pool_set_limits(uint16_t max_threads, uint16_t min_threads,
    uint16_t min_spare, uint32_t max_idle_secs)
{
  if(max_threads < 1) max_threads = 1;
  if(min_threads < 1) min_threads = 1;
  if(min_threads > max_threads) min_threads = max_threads;
  if(min_spare > min_threads)   min_spare = min_threads;

  pool_cfg.max_threads   = max_threads;
  pool_cfg.min_threads   = min_threads;
  pool_cfg.min_spare     = min_spare;
  pool_cfg.max_idle_secs = max_idle_secs;

  pool_bg_budget_apply();
}

// Initialize the thread pool. Allocates worker slots and spawns
// the minimum number of worker threads.
void
pool_init(void)
{
  pthread_mutex_init(&pool_mutex, NULL);
  pool_stopping = false;
  pool_size = 0;
  pool_idle = 0;
  persist_count = 0;

  // Initialize persist worker slots.
  for(uint16_t i = 0; i < POOL_MAX_PERSIST; i++)
    persist_workers[i].wstate = WORKER_UNUSED;

  // Allocate worker array: slot 0 = parent, 1..max = workers.
  workers = mem_alloc("pool", "workers",
      sizeof(worker_t) * (pool_cfg.max_threads + 1));

  // Slot 0 reserved for parent.
  workers[0].id     = 0;
  workers[0].wstate = WORKER_UNUSED;
  workers[0].jobs   = 0;
  workers[0].idle   = false;

  // Mark all worker slots as unused.
  for(uint16_t i = 1; i <= pool_cfg.max_threads; i++)
    workers[i].wstate = WORKER_UNUSED;

  // Create the minimum number of worker threads.
  pthread_mutex_lock(&pool_mutex);

  for(uint16_t i = 0; i < pool_cfg.min_threads; i++)
    if(!spawn_worker_locked())
      break;

  pthread_mutex_unlock(&pool_mutex);

  pool_ready = true;

  clam(CLAM_INFO, "pool_init",
      "thread pool initialized (workers: %u, min: %u, max: %u, spare: %u)",
      pool_size, pool_cfg.min_threads,
      pool_cfg.max_threads, pool_cfg.min_spare);
}

// Enter the parent worker loop on the calling (main) thread.
// Processes TASK_PARENT tasks until shutdown or signal. Blocks until done.
void
pool_run_parent(void)
{
  worker_t *parent = &workers[0];
  task_t   *t;

  parent->wstate      = WORKER_RUNNING;
  parent->thread      = pthread_self();
  parent->created     = time(NULL);
  parent->last_active = parent->created;

  clam(CLAM_DEBUG, "pool", "parent entering worker loop");

  while(!pool_stopping && !sig_shutdown_requested())
  {
    task_state_t result;

    // The parent is one dedicated thread, not part of the capacity the
    // reserve divides up, so it never asks for a background slot.
    t = task_assign(TASK_PARENT, true);

    if(t != NULL)
    {
      t->cb(t);
      result = task_finish(t);

      parent->jobs++;
      parent->last_active = time(NULL);

      if(result == TASK_FATAL)
      {
        pool_shutdown();
        break;
      }
    }

    else
      task_wait(pool_cfg.wait_ms);
  }

  clam(CLAM_DEBUG, "pool", "parent exiting worker loop (jobs: %lu)",
      parent->jobs);
}

// Request pool shutdown. Sets the stop flag and wakes all workers.
// Safe to call multiple times; subsequent calls are no-ops.
void
pool_shutdown(void)
{
  if(pool_stopping)
    return;

  pool_stopping = true;
  task_wake_all();

  clam(CLAM_INFO, "pool_shutdown", "shutdown requested, waking workers");
}

bool
pool_shutting_down(void)
{
  return(pool_stopping);
}

// Spawn a dedicated thread for a persistent task. The task runs on
// its own thread outside the elastic pool until it exits or shutdown.
// returns: true on success, false if no persist slot or thread creation fails
// t: task to run (must be TASK_PERSIST kind)
bool
pool_spawn_persist(task_t *t)
{
  pthread_mutex_lock(&pool_mutex);

  // Slots of readers that exited on their own — nobody joined them, so
  // collect them here rather than leaking one per reload cycle.
  reap_persist_locked();

  // Find an unused persist slot.
  for(uint16_t i = 0; i < POOL_MAX_PERSIST; i++)
  {
    if(persist_workers[i].wstate == WORKER_UNUSED)
    {
      persist_workers[i].id          = (uint8_t)i;
      persist_workers[i].jobs        = 1;
      persist_workers[i].created     = time(NULL);
      persist_workers[i].last_active = persist_workers[i].created;
      persist_workers[i].wstate      = WORKER_RUNNING;
      persist_workers[i].idle        = false;
      persist_workers[i].task_id     = t->id;

      if(pthread_create(&persist_workers[i].thread, NULL,
              persist_entry, t) != 0)
      {
        persist_workers[i].wstate  = WORKER_UNUSED;
        persist_workers[i].task_id = TASK_HANDLE_NONE;
        pthread_mutex_unlock(&pool_mutex);
        return(false);
      }

      persist_count++;
      pthread_mutex_unlock(&pool_mutex);

      clam(CLAM_DEBUG, "pool", "persist thread %u spawned for '%s'",
          i, t->name);
      return(true);
    }
  }

  pthread_mutex_unlock(&pool_mutex);
  clam(CLAM_WARN, "pool", "no persist slots available (max: %u)",
      POOL_MAX_PERSIST);
  return(false);
}

// The join a plugin needs before its .so is unmapped. `id` that no slot
// holds is already gone, which is success. The slot is parked in
// WORKER_JOINING for the duration so pool_exit and the spawn-time reap
// leave the thread to us.
bool
pool_join_persist(task_handle_t id, uint32_t timeout_ms)
{
  struct timespec deadline;
  pthread_t       thread;
  worker_state_t  prev;
  uint16_t        slot = POOL_MAX_PERSIST;
  int             rc;

  if(id == TASK_HANDLE_NONE)
    return(true);

  pthread_mutex_lock(&pool_mutex);

  for(uint16_t i = 0; i < POOL_MAX_PERSIST; i++)
  {
    if(persist_workers[i].task_id == id
        && (persist_workers[i].wstate == WORKER_RUNNING
            || persist_workers[i].wstate == WORKER_RETIRING))
    {
      slot = i;
      break;
    }
  }

  if(slot == POOL_MAX_PERSIST)
  {
    pthread_mutex_unlock(&pool_mutex);
    return(true);
  }

  thread = persist_workers[slot].thread;
  prev   = persist_workers[slot].wstate;

  persist_workers[slot].wstate = WORKER_JOINING;

  pthread_mutex_unlock(&pool_mutex);

  clock_gettime(CLOCK_REALTIME, &deadline);

  deadline.tv_sec  += (time_t)(timeout_ms / 1000U);
  deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000L * 1000L;

  if(deadline.tv_nsec >= 1000L * 1000L * 1000L)
  {
    deadline.tv_nsec -= 1000L * 1000L * 1000L;
    deadline.tv_sec  += 1;
  }

  rc = pthread_timedjoin_np(thread, NULL, &deadline);

  pthread_mutex_lock(&pool_mutex);

  // A thread that exits in the instant the deadline lapses is joined,
  // not reported stuck — and this is also what keeps the slot from
  // being restored to RUNNING after its thread is already gone.
  if(rc != 0 && pthread_tryjoin_np(thread, NULL) == 0)
    rc = 0;

  if(rc != 0)
  {
    // Still running. Hand the slot back exactly as we found it — the
    // caller decides whether a stuck reader is fatal to the unload.
    persist_workers[slot].wstate = prev;
    pthread_mutex_unlock(&pool_mutex);

    clam(CLAM_WARN, "pool", "persist join timed out after %u ms (slot %u)",
        timeout_ms, slot);
    return(false);
  }

  persist_workers[slot].wstate  = WORKER_UNUSED;
  persist_workers[slot].task_id = TASK_HANDLE_NONE;

  pthread_mutex_unlock(&pool_mutex);

  clam(CLAM_DEBUG, "pool", "persist thread joined (slot %u)", slot);
  return(true);
}

// Shut down and clean up the thread pool. Joins all elastic and
// persist worker threads, frees resources, and destroys the mutex.
void
pool_exit(void)
{
  if(!pool_ready)
    return;

  if(!pool_stopping)
    pool_shutdown();

  // Join all elastic worker threads.
  for(uint16_t i = 1; i <= pool_cfg.max_threads; i++)
  {
    if(workers[i].wstate == WORKER_RUNNING
        || workers[i].wstate == WORKER_RETIRING)
    {
      pthread_join(workers[i].thread, NULL);
      workers[i].wstate = WORKER_UNUSED;
    }
  }

  // Join all persist threads.
  for(uint16_t i = 0; i < POOL_MAX_PERSIST; i++)
  {
    if(persist_workers[i].wstate == WORKER_RUNNING
        || persist_workers[i].wstate == WORKER_RETIRING)
    {
      pthread_join(persist_workers[i].thread, NULL);
      persist_workers[i].wstate = WORKER_UNUSED;
    }
  }

  clam(CLAM_INFO, "pool_exit",
      "thread pool shut down (parent jobs: %lu)", workers[0].jobs);

  mem_free(workers);
  workers = NULL;

  pthread_mutex_destroy(&pool_mutex);
  pool_ready = false;
}

// Get pool statistics (thread-safe snapshot).
void
pool_get_stats(pool_stats_t *out)
{
  pthread_mutex_lock(&pool_mutex);
  out->total        = pool_size;
  out->idle         = pool_idle;
  out->persist      = persist_count;
  out->peak_workers = pool_peak;
  out->bg_active    = pool_bg;
  out->bg_budget    = pool_cfg.bg_budget;

  // Sum jobs across all worker slots.
  out->jobs_completed = 0;

  if(workers != NULL)
  {
    for(uint16_t i = 0; i <= pool_cfg.max_threads; i++)
      out->jobs_completed += workers[i].jobs;
  }

  // Include persist worker jobs.
  for(uint16_t i = 0; i < persist_count; i++)
    out->jobs_completed += persist_workers[i].jobs;

  pthread_mutex_unlock(&pool_mutex);
}

// KV configuration

// Load pool configuration from the KV store and apply sanity clamps.
// Updates pool_cfg with clamped values.
static void
pool_load_config(void)
{
  uint16_t max_threads   = (uint16_t)kv_get_uint("core.pool.max_threads");
  uint16_t min_threads   = (uint16_t)kv_get_uint("core.pool.min_threads");
  uint16_t min_spare     = (uint16_t)kv_get_uint("core.pool.min_spare");
  uint32_t max_idle_secs = (uint32_t)kv_get_uint("core.pool.max_idle_secs");
  uint32_t wait_ms       = (uint32_t)kv_get_uint("core.pool.wait_ms");
  uint16_t reserve       = (uint16_t)kv_get_uint("core.pool.reserve_interactive");

  // Sanity clamps.
  if(max_threads < 1)   max_threads = 1;
  if(max_threads > 256)  max_threads = 256;
  if(min_threads < 1)   min_threads = 1;
  if(min_threads > max_threads) min_threads = max_threads;
  if(min_spare > min_threads)   min_spare = min_threads;
  if(wait_ms < 100)     wait_ms = 100;
  if(wait_ms > 10000)   wait_ms = 10000;

  pool_cfg.max_threads   = max_threads;
  pool_cfg.min_threads   = min_threads;
  pool_cfg.min_spare     = min_spare;
  pool_cfg.max_idle_secs = max_idle_secs;
  pool_cfg.wait_ms       = wait_ms;
  pool_cfg.reserve       = reserve;

  pool_bg_budget_apply();
}

static void
pool_kv_changed(const char *key, void *data)
{
  (void)key;
  (void)data;
  pool_load_config();
}

// Register all pool configuration keys with the KV subsystem.
// Sets defaults and attaches the change callback for live reloading.
static void
pool_register_kv(void)
{
  kv_register("core.pool.max_threads",   KV_UINT16, "64",
      pool_kv_changed, NULL, "Maximum worker threads in the pool");
  kv_register("core.pool.min_threads",   KV_UINT16, "1",
      pool_kv_changed, NULL, "Minimum worker threads kept alive");
  kv_register("core.pool.min_spare",     KV_UINT16, "1",
      pool_kv_changed, NULL, "Minimum idle spare threads to maintain");
  kv_register("core.pool.max_idle_secs", KV_UINT32, "300",
      pool_kv_changed, NULL, "Seconds before excess idle threads exit");
  kv_register("core.pool.wait_ms",       KV_UINT32, "1000",
      pool_kv_changed, NULL, "Thread work-queue wait timeout in milliseconds");
  kv_register("core.pool.reserve_interactive", KV_UINT16, "8",
      pool_kv_changed, NULL,
      "Workers background work may never occupy, kept for command bodies "
      "(0 lets background work fill the pool)");
}

// Register KV keys and load initial pool configuration from the store.
// Must be called after kv_init() and kv_load().
void
pool_register_config(void)
{
  pool_register_kv();
  pool_load_config();

  clam(CLAM_DEBUG, "pool", "config loaded from KV (max: %u, min: %u, "
      "spare: %u, idle: %us, wait: %ums, reserve: %u)",
      pool_cfg.max_threads, pool_cfg.min_threads,
      pool_cfg.min_spare, pool_cfg.max_idle_secs, pool_cfg.wait_ms,
      pool_cfg.reserve);
}
