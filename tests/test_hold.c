// botmanager — MIT
// Cases for the chat plugin's live-record drain (plugins/bot/chat/hold.h).
//
// core's quiescence barrier range-tests tasks and curl requests, and an
// LLM request is neither: its curl record names the inference plugin's
// callback and the inference plugin's heap, so nothing core owns can
// tell that a chat callback is still airborne inside one. The drain
// here is the only thing that can, which makes its two silent failures
// worth rows: a shutdown that returns while a callback is still running
// is a dlclose over live code, and one that disowns the wrong owner
// takes another bot's work down with it. Neither says anything at the
// time — the crash arrives months later, on a reload.
//
// The waiting row is stated as elapsed milliseconds, so a drain that
// skips its wait is red under a plain -O2 build with no sanitizer.

#include "test.h"

#include "hold.h"

#include <pthread.h>
#include <time.h>

#define SUITE "hold"

// Long enough that a scheduler hiccup cannot fake an early return,
// short enough to sit through.
#define UNLINK_MS  300
#define FLOOR_MS   250
#define FAST_MS    50

// Every row leaves llm_user NULL and curl_id 0, so no engine call is
// reached: this suite touches no network and loads no plugin.
static void
hold_add(chatbot_hold_t *h, const void *owner)
{
  chatbot_hold_link(h, owner, NULL);
}

static uint64_t
ms_since(const struct timespec *from)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);

  return((uint64_t)(now.tv_sec - from->tv_sec) * 1000U
      + (uint64_t)((now.tv_nsec - from->tv_nsec) / 1000000L));
}

static uint64_t
shutdown_timed(const void *owner)
{
  struct timespec start;

  clock_gettime(CLOCK_MONOTONIC, &start);
  chatbot_hold_shutdown(owner);

  return(ms_since(&start));
}

// Stands in for the callback a disowned record is waiting on: it comes
// back late, and the drain must still be there when it does.
static void *
late_unlink(void *arg)
{
  struct timespec delay = { .tv_sec = 0,
                            .tv_nsec = UNLINK_MS * 1000L * 1000L };

  nanosleep(&delay, NULL);
  chatbot_hold_unlink(arg);

  return(NULL);
}

int
main(void)
{
  // Two owners that are never dereferenced — the drain matches on the
  // pointer alone, which is the property that lets a translation unit
  // below chatbot.h carry a hold.
  const char      owner_a = 'a';
  const char      owner_b = 'b';
  chatbot_hold_t  h_a     = { 0 };
  chatbot_hold_t  h_b     = { 0 };
  chatbot_hold_t  h_plug  = { 0 };
  chatbot_hold_t  h_late  = { 0 };
  pthread_t       th;
  uint64_t        took;

  // 1 — nothing outstanding is nothing to wait for.
  hold_add(&h_a, &owner_a);
  chatbot_hold_unlink(&h_a);
  took = shutdown_timed(&owner_a);
  test_check_bool(SUITE, "a drain with nothing listed returns at once",
      true, took < FAST_MS);

  // 2 — one bot's teardown is not another's.
  hold_add(&h_a, &owner_a);
  took = shutdown_timed(&owner_b);
  test_check_bool(SUITE, "draining one owner does not disown another's",
      false, chatbot_hold_disowned(&h_a));
  test_check_bool(SUITE, "and does not wait on it either",
      true, took < FAST_MS);
  chatbot_hold_unlink(&h_a);

  // 3 — NULL owner means the PLUGIN's work, not every bot's. The
  // record left listed is what proves it: an owner-blind drain would
  // disown h_b too, and a NULL-guarded one would disown neither.
  hold_add(&h_plug, NULL);
  hold_add(&h_b, &owner_b);
  pthread_create(&th, NULL, late_unlink, &h_plug);
  chatbot_hold_shutdown(NULL);
  pthread_join(th, NULL);
  test_check_bool(SUITE, "the plugin's own record is disowned by a NULL drain",
      true, chatbot_hold_disowned(&h_plug));
  test_check_bool(SUITE, "and a bot's record is left alone by it",
      false, chatbot_hold_disowned(&h_b));
  chatbot_hold_unlink(&h_b);

  // 4 — the drain waits for the callback, which is the whole point.
  hold_add(&h_late, &owner_a);
  pthread_create(&th, NULL, late_unlink, &h_late);
  took = shutdown_timed(&owner_a);
  pthread_join(th, NULL);
  test_check_bool(SUITE, "a drain waits for an airborne record to come back",
      true, took >= FLOOR_MS);

  return(test_report(SUITE));
}
