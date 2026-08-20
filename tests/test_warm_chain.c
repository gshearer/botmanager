// botmanager — MIT
// Cases for whenmoon's warmup-chain drain
// (plugins/extension/whenmoon/warm_chain.h).
//
// A deferred task that re-arms itself keeps no handle anybody else can
// cancel: the handle you last saw is superseded by the hop you are
// racing. whenmoon had two of them, so nothing ended the warmup at
// unload and deinit() destroyed the markets rwlock under a replay that
// was still holding it (OBS-43).
//
// The two failures worth rows are both silent. A drain that returns
// while a body is still inside the plugin is a dlclose over live code
// and says nothing at the time — it arrives months later as a SIGSEGV
// in pthread_rwlock_unlock. And a drain that REFUSES destructively is
// worse than not refusing: stop() returning FAIL leaves the plugin
// running, so a refusal that left the cancelled warmups dead would
// strand every market in WARMING with nothing scheduled to promote it.
//
// The waiting row is stated as elapsed milliseconds, so a drain that
// skips its wait is red under a plain -O2 build with no sanitizer.
//
// No worker pool is started here, which is the determinism the suite
// wants: a deferred sits in the real timer queue and nothing runs it,
// so every transition below is the one the test asked for.

#include "test.h"

#include "alloc.h"
#include "task.h"
#include "warm_chain.h"

#include <pthread.h>
#include <string.h>
#include <time.h>

#define SUITE "warm_chain"

// Long enough that a scheduler hiccup cannot fake an early return,
// short enough to sit through.
#define REFUSE_MS  400
#define FLOOR_MS   350
#define FAST_MS    100

// Well beyond the suite's runtime: every armed hop below is meant to
// stay in the timer queue, never to fire.
#define HOP_MS     600000u

static int g_released;
static int g_ran;

static void
count_release(void *ctx)
{
  (void)ctx;
  g_released++;
}

// No worker pool runs in this suite, so a body firing at all would mean
// the timer queue is being drained by something the test did not start.
static void
never_runs(task_t *t)
{
  g_ran++;

  if(t != NULL)
    t->state = TASK_ENDED;
}

static uint64_t
ms_since(const struct timespec *from)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);

  return((uint64_t)(now.tv_sec - from->tv_sec) * 1000U
      + (uint64_t)((now.tv_nsec - from->tv_nsec) / 1000000L));
}

static void
nap(uint32_t ms)
{
  struct timespec d = { .tv_sec  = (time_t)(ms / 1000U),
                        .tv_nsec = (long)(ms % 1000U) * 1000000L };

  nanosleep(&d, NULL);
}

typedef struct
{
  uint32_t timeout_ms;
  char     offender[TASK_NAME_SZ];
  bool     drained;
  uint64_t took_ms;
} drain_arg_t;

static void *
drain_thread(void *arg)
{
  drain_arg_t     *a = arg;
  struct timespec  start;

  clock_gettime(CLOCK_MONOTONIC, &start);

  a->drained = wm_warm_chain_drain(a->timeout_ms, a->offender,
      sizeof(a->offender));
  a->took_ms = ms_since(&start);

  return(NULL);
}

// A chain that is armed and waiting in the timer queue, as every hop of
// a live warmup is between ticks.
static wm_warm_chain_t *
armed(void *ctx, const char *name)
{
  wm_warm_chain_t *c = wm_warm_chain_open(ctx, count_release);

  if(c != NULL && !wm_warm_chain_arm(c, name, HOP_MS, never_runs))
  {
    wm_warm_chain_close(c);
    return(NULL);
  }

  return(c);
}

int
main(void)
{
  // Contexts the chains carry. They are never dereferenced — the
  // release callback only counts — which is what lets this suite hold
  // one without whenmoon's headers.
  static int       ctx_a;
  static int       ctx_b;
  static int       ctx_c;
  static int       ctx_d;
  wm_warm_chain_t *a;
  wm_warm_chain_t *b;
  wm_warm_chain_t *c;
  wm_warm_chain_t *d;
  drain_arg_t      arg;
  pthread_t        th;
  task_stats_t     before;
  task_stats_t     after;
  struct timespec  start;

  mem_init();
  task_init();

  // 1 — one armed chain is one release, and no body ever ran.
  g_released = 0;
  a          = armed(&ctx_a, "wm_warm_recheck");
  test_check_bool(SUITE, "a chain arms while nothing is draining",
      true, a != NULL);
  test_check_bool(SUITE, "a drain over one armed chain succeeds",
      true, wm_warm_chain_drain(1000, NULL, 0));
  test_check_sz(SUITE, "and releases its ctx exactly once",
      1, (size_t)g_released);

  // 2 — three chains, three releases, no double free.
  wm_warm_chain_reset();
  g_released = 0;
  a          = armed(&ctx_a, "wm_warm_recheck");
  b          = armed(&ctx_b, "wm_warm_recheck");
  c          = armed(&ctx_c, "wm_warmup");
  test_check_bool(SUITE, "three chains all arm",
      true, a != NULL && b != NULL && c != NULL);
  test_check_bool(SUITE, "a drain over three armed chains succeeds",
      true, wm_warm_chain_drain(1000, NULL, 0));
  test_check_sz(SUITE, "and releases each ctx exactly once",
      3, (size_t)g_released);

  // 3 — nothing listed is nothing to wait for.
  wm_warm_chain_reset();
  clock_gettime(CLOCK_MONOTONIC, &start);
  test_check_bool(SUITE, "a drain over an empty list succeeds",
      true, wm_warm_chain_drain(REFUSE_MS, NULL, 0));
  test_check_bool(SUITE, "and returns at once",
      true, ms_since(&start) < FAST_MS);

  // 4 — a successful drain leaves arming off, so the plugin on its way
  // to deinit() cannot schedule work into the mapping being unmapped.
  test_check_bool(SUITE, "open after a successful drain is refused",
      true, wm_warm_chain_open(&ctx_a, count_release) == NULL);

  // 5 — a fresh plugin life re-enables it.
  wm_warm_chain_reset();
  a = wm_warm_chain_open(&ctx_a, count_release);
  test_check_bool(SUITE, "open after a reset succeeds again",
      true, a != NULL);
  wm_warm_chain_close(a);

  // 6 — ⭐ the row this suite exists for. One chain is armed and one has
  // a body inside it; the drain must outlast its whole budget rather
  // than reporting a quiet plugin, and it must hand back everything it
  // took off the queue on the way out.
  wm_warm_chain_reset();
  g_released = 0;
  a          = armed(&ctx_a, "wm_warmup");
  b          = armed(&ctx_b, "wm_warm_recheck");
  test_check_bool(SUITE, "the body's chain enters cleanly",
      true, wm_warm_chain_enter(b));

  task_get_stats(&before);

  arg.timeout_ms = REFUSE_MS;
  arg.drained    = true;
  pthread_create(&th, NULL, drain_thread, &arg);
  pthread_join(th, NULL);

  task_get_stats(&after);

  test_check_bool(SUITE, "a drain over an executing body refuses",
      false, arg.drained);
  test_check_bool(SUITE, "and waits out its whole budget first",
      true, arg.took_ms >= FLOOR_MS);
  test_check_str(SUITE, "and names the task still running",
      "wm_warm_recheck", arg.offender);
  test_check_sz(SUITE, "and releases nothing at all",
      0, (size_t)g_released);
  test_check_sz(SUITE, "and re-arms every chain it cancelled",
      (size_t)before.sleeping, (size_t)after.sleeping);

  // 7 — the refusal was non-destructive, so the retry that follows it
  // works: once the body leaves, the same drain succeeds.
  wm_warm_chain_close(b);
  test_check_sz(SUITE, "closing the body's chain releases only its ctx",
      1, (size_t)g_released);
  test_check_bool(SUITE, "the drain retried after the body left succeeds",
      true, wm_warm_chain_drain(1000, NULL, 0));
  test_check_sz(SUITE, "and the re-armed chain is released by it",
      2, (size_t)g_released);

  // 8 — a body dispatched before the drain started still finds the door
  // shut: enter() refuses, and the body's kill path is the only thing
  // it may reach.
  wm_warm_chain_reset();
  g_released = 0;
  c          = armed(&ctx_c, "wm_warmup");
  d          = armed(&ctx_d, "wm_warm_recheck");
  test_check_bool(SUITE, "the blocking body enters before the drain",
      true, wm_warm_chain_enter(d));

  arg.timeout_ms = REFUSE_MS;
  pthread_create(&th, NULL, drain_thread, &arg);
  nap(FAST_MS);

  test_check_bool(SUITE, "enter refuses once a drain has started",
      false, wm_warm_chain_enter(c));

  wm_warm_chain_close(c);
  pthread_join(th, NULL);

  test_check_bool(SUITE, "the drain that refused it also refused",
      false, arg.drained);

  wm_warm_chain_close(d);
  test_check_bool(SUITE, "and the list drains clean afterwards",
      true, wm_warm_chain_drain(1000, NULL, 0));
  test_check_sz(SUITE, "with both ctxs released exactly once",
      2, (size_t)g_released);

  test_check_sz(SUITE, "no chain body ever ran (no pool is started)",
      0, (size_t)g_ran);

  return(test_report(SUITE));
}
