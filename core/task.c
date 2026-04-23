// botmanager — MIT
// Task subsystem: worker pool, scheduling, deferred execution.
#define TASK_INTERNAL
#include "task.h"

#include "pool.h"
#include "util.h"

// Name helpers

const char *
task_state_name(task_state_t s)
{
  switch(s)
  {
    case TASK_WAITING:  return("waiting");
    case TASK_RUNNING:  return("running");
    case TASK_SLEEPING: return("sleeping");
    case TASK_ENDED:    return("ended");
    case TASK_FATAL:    return("fatal");
    default:            return("unknown");
  }
}

const char *
task_type_name(task_type_t t)
{
  switch(t)
  {
    case TASK_PARENT: return("parent");
    case TASK_THREAD: return("thread");
    case TASK_ANY:    return("any");
    default:          return("unknown");
  }
}

const char *
task_kind_name(task_kind_t k)
{
  switch(k)
  {
    case TASK_ONCE:     return("once");
    case TASK_PERSIST:  return("persist");
    case TASK_PERIODIC: return("periodic");
    case TASK_DEFERRED: return("deferred");
    default:            return("unknown");
  }
}

// Sorted insert into the ready queue (by priority ascending).
// Must be called with task_lock held.
static void
ready_insert(task_t *t)
{
  task_t **pp = &ready_head;

  while(*pp != NULL && (*pp)->priority <= t->priority)
    pp = &(*pp)->next;

  t->next = *pp;
  *pp = t;
}

// Sorted insert into the timer queue (by sleep_until ascending).
// Must be called with task_lock held.
static void
timer_insert(task_t *t)
{
  task_t **pp = &timer_head;

  while(*pp != NULL && (*pp)->sleep_until <= t->sleep_until)
    pp = &(*pp)->next;

  t->next = *pp;
  *pp = t;
}

// Promote expired timers from the timer queue to the ready queue.
// Must be called with task_lock held.
static void
timer_promote(void)
{
  time_t now = time(NULL);

  while(timer_head != NULL && timer_head->sleep_until <= now)
  {
    task_t *t = timer_head;

    timer_head = t->next;
    t->next = NULL;
    t->state = TASK_WAITING;

    ready_insert(t);

    stats.sleeping--;
    stats.waiting++;
  }
}

static void
free_chain(task_t *t)
{
  while(t != NULL)
  {
    task_t *next = t->link;

    mem_free(t);
    t = next;
  }
}

static bool
type_matches(task_type_t task_type, task_type_t request_type)
{
  return(task_type == request_type
      || task_type == TASK_ANY
      || request_type == TASK_ANY);
}

// Public API

task_t *
task_create(const char *name, task_type_t type, uint8_t priority,
    task_cb_t cb, void *data)
{
  task_t *t = mem_alloc("task", "task", sizeof(task_t));

  strncpy(t->name, name, TASK_NAME_SZ - 1);
  t->name[TASK_NAME_SZ - 1] = '\0';
  t->state       = TASK_WAITING;
  t->type        = type;
  t->kind        = TASK_ONCE;
  t->priority    = priority;
  t->cb          = cb;
  t->data        = data;
  t->interval_ms = 0;
  t->created     = time(NULL);
  t->last_run    = 0;
  t->sleep_until = 0;
  t->run_count   = 0;
  t->link        = NULL;
  t->next        = NULL;

  return(t);
}

// Submit a task to the ready queue. Linked descendants are counted
// in statistics but not queued until their parent completes.
// TASK_DEFERRED tasks are placed in the timer queue if sleep_until > now.
// t: task to submit (must be in TASK_WAITING state)
void
task_submit(task_t *t)
{
  // Periodic tasks cannot have linked children (they never truly end).
  if(t->kind == TASK_PERIODIC && t->link != NULL)
  {
    clam(CLAM_WARN, "task_submit",
        "'%s': periodic tasks cannot have linked children, ignoring link",
        t->name);
    t->link = NULL;
  }

  pthread_mutex_lock(&task_lock);

  // Deferred tasks start in the timer queue.
  if(t->kind == TASK_DEFERRED && t->sleep_until > time(NULL))
  {
    t->state = TASK_SLEEPING;
    timer_insert(t);
    stats.total++;
    stats.sleeping++;
  }

  else
  {
    t->state = TASK_WAITING;
    ready_insert(t);
    stats.total++;
    stats.waiting++;
  }

  // Track periodic tasks.
  if(t->kind == TASK_PERIODIC)
    stats.periodic++;

  // Count any linked descendants.
  for(task_t *l = t->link; l != NULL; l = l->link)
  {
    stats.total++;
    stats.linked++;
  }

  pthread_cond_signal(&task_cond);
  pthread_mutex_unlock(&task_lock);

  clam(CLAM_DEBUG, "task_submit", "'%s' (type: %s kind: %s prio: %u)",
      t->name, task_type_name(t->type), task_kind_name(t->kind),
      t->priority);
}

task_t *
task_add(const char *name, task_type_t type, uint8_t priority,
    task_cb_t cb, void *data)
{
  task_t *t = task_create(name, type, priority, cb, data);

  task_submit(t);
  return(t);
}

// Create and spawn a persistent task on a dedicated thread.
task_t *
task_add_persist(const char *name, uint8_t priority,
    task_cb_t cb, void *data)
{
  task_t *t = task_create(name, TASK_THREAD, priority, cb, data);

  t->kind = TASK_PERSIST;

  // Track in stats and running list before spawning.
  pthread_mutex_lock(&task_lock);
  stats.total++;
  stats.persist++;
  stats.running++;
  t->state = TASK_RUNNING;
  t->last_run = time(NULL);
  t->run_count = 1;
  t->next = running_head;
  running_head = t;
  pthread_mutex_unlock(&task_lock);

  if(!pool_spawn_persist(t))
  {
    clam(CLAM_FATAL, "task_add_persist",
        "failed to spawn persist thread for '%s'", name);

    pthread_mutex_lock(&task_lock);
    stats.total--;
    stats.persist--;
    stats.running--;

    // Remove from running list.
    for(task_t **pp = &running_head; *pp != NULL; pp = &(*pp)->next)
    {
      if(*pp == t)
      {
        *pp = t->next;
        t->next = NULL;
        break;
      }
    }

    pthread_mutex_unlock(&task_lock);

    mem_free(t);
    return(NULL);
  }

  clam(CLAM_DEBUG, "task_add_persist", "'%s' spawned on dedicated thread",
      name);

  return(t);
}

task_t *
task_add_periodic(const char *name, task_type_t type,
    uint8_t priority, uint32_t interval_ms, task_cb_t cb, void *data)
{
  task_t *t = task_create(name, type, priority, cb, data);

  t->kind        = TASK_PERIODIC;
  t->interval_ms = interval_ms;

  task_submit(t);
  return(t);
}

task_t *
task_add_deferred(const char *name, task_type_t type,
    uint8_t priority, uint32_t delay_ms, task_cb_t cb, void *data)
{
  task_t *t = task_create(name, type, priority, cb, data);

  t->kind        = TASK_DEFERRED;
  t->sleep_until = time(NULL) + (time_t)(delay_ms / 1000);

  // Ensure minimum 1 second delay if any delay was requested.
  if(delay_ms > 0 && t->sleep_until <= time(NULL))
    t->sleep_until = time(NULL) + 1;

  task_submit(t);
  return(t);
}

task_t *
task_assign(task_type_t type)
{
  task_t **pp;

  pthread_mutex_lock(&task_lock);

  // Promote any expired timers to the ready queue.
  timer_promote();

  // Walk the ready queue (sorted by priority) and find the first
  // task whose type matches.
  pp = &ready_head;

  while(*pp != NULL)
  {
    task_t *t = *pp;

    if(type_matches(t->type, type))
    {
      // Dequeue.
      *pp = t->next;
      t->next = NULL;
      t->state = TASK_RUNNING;
      t->last_run = time(NULL);
      t->run_count++;

      stats.waiting--;
      stats.running++;

      // Track in running list.
      t->next = running_head;
      running_head = t;

      pthread_mutex_unlock(&task_lock);
      return(t);
    }

    pp = &(*pp)->next;
  }

  pthread_mutex_unlock(&task_lock);
  return(NULL);
}

void
task_wait(uint32_t timeout_ms)
{
  struct timespec ts;
  uint32_t        wait_ms = timeout_ms;

  clock_gettime(CLOCK_REALTIME, &ts);

  pthread_mutex_lock(&task_lock);

  // If a timer has already expired, promote and return immediately.
  if(timer_head != NULL && timer_head->sleep_until <= time(NULL))
  {
    timer_promote();
    pthread_mutex_unlock(&task_lock);
    return;
  }

  // Wait until the earliest timer fires or the caller's timeout,
  // whichever comes first.
  if(timer_head != NULL)
  {
    time_t   now      = time(NULL);
    uint32_t timer_ms = (uint32_t)(timer_head->sleep_until - now) * 1000;

    if(timer_ms < wait_ms)
      wait_ms = timer_ms;
  }

  ts.tv_sec  += wait_ms / 1000;
  ts.tv_nsec += (wait_ms % 1000) * 1000000L;

  if(ts.tv_nsec >= 1000000000L)
  {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000L;
  }

  pthread_cond_timedwait(&task_cond, &task_lock, &ts);

  // Promote any timers that expired while we slept.
  timer_promote();

  pthread_mutex_unlock(&task_lock);
}

// Wake all threads waiting in task_wait(). Used during shutdown.
void
task_wake_all(void)
{
  pthread_mutex_lock(&task_lock);
  pthread_cond_broadcast(&task_cond);
  pthread_mutex_unlock(&task_lock);
}

task_state_t
task_finish(task_t *t)
{
  task_state_t result = t->state;
  bool do_free = false;

  pthread_mutex_lock(&task_lock);

  // Remove from running list.
  for(task_t **pp = &running_head; *pp != NULL; pp = &(*pp)->next)
  {
    if(*pp == t)
    {
      *pp = t->next;
      t->next = NULL;
      break;
    }
  }

  stats.running--;

  switch(result)
  {
    case TASK_ENDED:
      // Periodic tasks reschedule instead of ending, unless shutting down.
      if(t->kind == TASK_PERIODIC && !pool_shutting_down())
      {
        t->state = TASK_SLEEPING;
        t->sleep_until = time(NULL)
            + (time_t)(t->interval_ms / 1000);

        // Minimum 1 second interval.
        if(t->interval_ms > 0 && t->interval_ms < 1000)
          t->sleep_until = time(NULL) + 1;

        timer_insert(t);
        stats.sleeping++;
        break;
      }

      // Periodic task during shutdown — free it and decrement periodic.
      if(t->kind == TASK_PERIODIC)
        stats.periodic--;

      // Persist task ending — decrement persist count.
      if(t->kind == TASK_PERSIST)
        stats.persist--;

      // Promote linked child to the ready queue.
      if(t->link != NULL)
      {
        task_t *child = t->link;

        child->state = TASK_WAITING;
        ready_insert(child);
        stats.linked--;
        stats.waiting++;
      }
      stats.total--;
      do_free = true;
      break;

    case TASK_FATAL:
      if(t->kind == TASK_PERIODIC)
        stats.periodic--;

      if(t->kind == TASK_PERSIST)
        stats.persist--;

      stats.total--;
      do_free = true;
      break;

    case TASK_WAITING:
      ready_insert(t);
      stats.waiting++;
      break;

    case TASK_SLEEPING:
      timer_insert(t);
      stats.sleeping++;
      break;

    default:
      break;
  }

  // Signal if new work is available.
  if(result == TASK_WAITING || result == TASK_ENDED)
    pthread_cond_signal(&task_cond);

  pthread_mutex_unlock(&task_lock);

  if(do_free)
  {
    if(result == TASK_FATAL)
      clam(CLAM_FATAL, "task_finish", "task '%s' returned FATAL", t->name);

    mem_free(t);
  }

  return(result);
}

// Get current task statistics (thread-safe snapshot).
void
task_get_stats(task_stats_t *out)
{
  pthread_mutex_lock(&task_lock);
  memcpy(out, &stats, sizeof(task_stats_t));
  pthread_mutex_unlock(&task_lock);
}

void
task_iterate(task_iter_cb_t cb, void *data)
{
  if(cb == NULL)
    return;

  pthread_mutex_lock(&task_lock);

  // Running tasks (includes persist).
  for(task_t *t = running_head; t != NULL; t = t->next)
    cb(t->name, t->state, t->kind, t->type, t->priority,
        t->run_count, t->interval_ms, t->created,
        t->last_run, t->sleep_until, data);

  // Waiting tasks (ready queue).
  for(task_t *t = ready_head; t != NULL; t = t->next)
    cb(t->name, t->state, t->kind, t->type, t->priority,
        t->run_count, t->interval_ms, t->created,
        t->last_run, t->sleep_until, data);

  // Sleeping tasks (timer queue).
  for(task_t *t = timer_head; t != NULL; t = t->next)
    cb(t->name, t->state, t->kind, t->type, t->priority,
        t->run_count, t->interval_ms, t->created,
        t->last_run, t->sleep_until, data);

  pthread_mutex_unlock(&task_lock);
}

// "show tasks" command

// Colorize a task state name.
static const char *
task_state_color(task_state_t s)
{
  switch(s)
  {
    case TASK_RUNNING:  return(CLR_GREEN);
    case TASK_WAITING:  return(CLR_YELLOW);
    case TASK_SLEEPING: return(CLR_CYAN);
    case TASK_ENDED:    return(CLR_WHITE);
    case TASK_FATAL:    return(CLR_RED);
    default:            return(CLR_RESET);
  }
}

// Colorize a task kind name.
static const char *
task_kind_color(task_kind_t k)
{
  switch(k)
  {
    case TASK_PERSIST:  return(CLR_PURPLE);
    case TASK_PERIODIC: return(CLR_BLUE);
    case TASK_DEFERRED: return(CLR_CYAN);
    default:            return(CLR_RESET);
  }
}

static void
task_show_cb(const char *name, task_state_t state, task_kind_t kind,
    task_type_t type, uint8_t priority, uint32_t run_count,
    uint32_t interval_ms, time_t created, time_t last_run,
    time_t sleep_until, void *data)
{
  task_show_state_t *st = data;
  char               line[512];
  time_t             now = time(NULL);
  char               age[16];
  char               extra[32] = "";

  (void)last_run;

  // Age: time since creation.
  util_fmt_duration(now - created, age, sizeof(age));

  // Extra detail column: interval for periodic, sleep remaining for
  // sleeping, blank otherwise.
  if(kind == TASK_PERIODIC && interval_ms > 0)
    snprintf(extra, sizeof(extra), "every %us", interval_ms / 1000);
  else if(state == TASK_SLEEPING && sleep_until > now)
  {
    char rem[16];

    util_fmt_duration(sleep_until - now, rem, sizeof(rem));
    snprintf(extra, sizeof(extra), "in %s", rem);
  }

  snprintf(line, sizeof(line),
      "  %-24s %s%-8s" CLR_RESET "  %s%-8s" CLR_RESET
      "  %-6s  pri=%-3u  runs=%-6u  age=%-8s  %s",
      name,
      task_state_color(state), task_state_name(state),
      task_kind_color(kind), task_kind_name(kind),
      task_type_name(type), priority, run_count,
      age, extra);

  cmd_reply(st->ctx, line);
  st->count++;
}

static void
task_cmd_show(const cmd_ctx_t *ctx)
{
  task_stats_t      ts;
  char              hdr[256];
  task_show_state_t st = { .ctx = ctx, .count = 0 };

  task_get_stats(&ts);

  snprintf(hdr, sizeof(hdr),
      CLR_BOLD "tasks:" CLR_RESET
      " %u total — %s%u running" CLR_RESET
      ", %s%u waiting" CLR_RESET
      ", %s%u sleeping" CLR_RESET
      ", %u linked, %s%u persist" CLR_RESET
      ", %s%u periodic" CLR_RESET,
      ts.total,
      CLR_GREEN,  ts.running,
      CLR_YELLOW, ts.waiting,
      CLR_CYAN,   ts.sleeping,
      ts.linked,
      CLR_PURPLE, ts.persist,
      CLR_BLUE,   ts.periodic);

  cmd_reply(ctx, hdr);

  if(ts.total == 0)
  {
    cmd_reply(ctx, "  (none)");
    return;
  }

  task_iterate(task_show_cb, &st);

  if(st.count == 0)
    cmd_reply(ctx, "  (none)");
}

// Register the "show tasks" command with the command subsystem.
void
task_register_commands(void)
{
  cmd_register("task", "tasks",
      "show tasks",
      "List all active tasks",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      task_cmd_show, NULL, "show", "t", NULL, 0, NULL, NULL);
}

// Initialize the task subsystem.
// Sets up the mutex, condition variable, and clears all queues and stats.
void
task_init(void)
{
  pthread_mutex_init(&task_lock, NULL);
  pthread_cond_init(&task_cond, NULL);
  ready_head   = NULL;
  timer_head   = NULL;
  running_head = NULL;
  memset(&stats, 0, sizeof(stats));
  task_ready = true;

  clam(CLAM_INFO, "task_init", "task system initialized");
}

// Shut down the task subsystem.
// Frees all queued tasks and their linked descendants, destroys
// the mutex and condition variable.
void
task_exit(void)
{
  task_stats_t s;

  if(!task_ready)
    return;

  task_get_stats(&s);

  clam(CLAM_INFO, "task_exit",
      "task system shutting down "
      "(total: %u wait: %u run: %u sleep: %u link: %u persist: %u periodic: %u)",
      s.total, s.waiting, s.running, s.sleeping, s.linked,
      s.persist, s.periodic);

  // Free all queued tasks and their linked descendants.
  while(ready_head != NULL)
  {
    task_t *t = ready_head;

    ready_head = t->next;
    free_chain(t->link);
    mem_free(t);
  }

  while(timer_head != NULL)
  {
    task_t *t = timer_head;

    timer_head = t->next;
    free_chain(t->link);
    mem_free(t);
  }

  pthread_mutex_destroy(&task_lock);
  pthread_cond_destroy(&task_cond);
  task_ready = false;
}
