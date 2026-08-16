// botmanager — MIT
// Cases for the bound curl_request_submit_wait promises (include/curl.h).
//
// The wait exists so a bulk pipeline gets backpressure instead of
// silent drops, and it used to have no end: the predicate is "the
// submit queue has room", and a queue only makes room as the endpoints
// ahead of it answer. One that accepts a connection and never replies
// holds its slot for the whole request timeout — llm's embed default is
// 300 s — so a caller could sit on a pool worker for as long as the
// sickest peer chose, unreachable by the operator and invisible in the
// log, because a thread parked in pthread_cond_wait writes nothing.
//
// That is the silent half, and it is why these rows exist. The fixture
// reproduces the stall with no network at all: core.curl.max_active of
// 0 means the drain thread never promotes anything, so the queue fills
// and stays full for as long as the suite likes. Against the unbounded
// version the second case never returns and meson reports a timeout.
//
// The rows are stated as elapsed milliseconds around the call, so the
// suite is red on a regression under a plain -O2 build and needs no
// sanitizer to notice.

#include "test.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "curl.h"
#include "kv.h"
#include "pool.h"
#include "task.h"

#include <time.h>

#define SUITE "curl_submit_wait"

// The bound every row waits out. Long enough that a scheduler hiccup
// cannot fake an early return, short enough to sit through.
#define WAIT_MS 300

// A request that is never dispatched needs somewhere to claim to be
// going; nothing in this suite reaches the network.
#define NOWHERE "http://127.0.0.1:1/"

static void
done_cb(const curl_response_t *resp)
{
  (void)resp;
}

static uint64_t
ms_since(const struct timespec *from)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);

  return((uint64_t)(now.tv_sec - from->tv_sec) * 1000U
      + (uint64_t)((now.tv_nsec - from->tv_nsec) / 1000000L));
}

// Submit one request through the bounded wait and report how long the
// call took and whether it was taken.
static uint64_t
submit_timed(uint32_t wait_ms, bool *out_ok)
{
  struct timespec  start;
  curl_request_t  *req;

  req = curl_request_create(CURL_METHOD_GET, NOWHERE, done_cb, NULL);

  clock_gettime(CLOCK_MONOTONIC, &start);
  *out_ok = (curl_request_submit_wait(req, wait_ms) == SUCCESS);

  return(ms_since(&start));
}

// With room in the queue the wait is not entered at all: the bound is
// a ceiling on the stall, never a delay on the ordinary path.
static void
case_room_is_immediate(void)
{
  bool     ok = false;
  uint64_t took;

  kv_set("core.curl.max_queued", "1");

  took = submit_timed(WAIT_MS, &ok);

  test_check_bool(SUITE, "a submit into a free queue is taken", true, ok);

  test_check_bool(SUITE, "and does not wait for its bound",
      true, took < WAIT_MS);
}

// The queue is now full and nothing drains it — max_active is 0, so the
// multi loop promotes nothing however long it runs. This is the case
// that never returned.
static void
case_full_queue_gives_up(void)
{
  bool     ok = true;
  uint64_t took;

  took = submit_timed(WAIT_MS, &ok);

  test_check_bool(SUITE, "a submit into a full queue is refused",
      false, ok);

  test_check_bool(SUITE, "after waiting its bound out",
      true, took >= WAIT_MS);

  // The ceiling is the row that would have failed before: any margin
  // here is scheduling, not a second wait.
  test_check_bool(SUITE, "and not appreciably longer",
      true, took < WAIT_MS + 2000);
}

// Zero is the documented "do not wait" — the fast-fail submit without
// the queue-full WARN, and the reading a caller passing an unset knob
// will get.
static void
case_zero_does_not_wait(void)
{
  bool     ok = true;
  uint64_t took;

  took = submit_timed(0, &ok);

  test_check_bool(SUITE, "a zero bound refuses a full queue", false, ok);

  test_check_bool(SUITE, "without waiting", true, took < WAIT_MS);
}

// A slot freed under a waiter is still what ends the wait early — the
// bound did not replace the condition, it only capped it.
static void
case_room_ends_the_wait(void)
{
  bool     ok = false;
  uint64_t took;

  kv_set("core.curl.max_queued", "64");

  took = submit_timed(WAIT_MS, &ok);

  test_check_bool(SUITE, "a raised cap is taken immediately", true, ok);

  test_check_bool(SUITE, "without waiting", true, took < WAIT_MS);
}

int
main(void)
{
  mem_init();
  clam_init();
  kv_init();
  task_init();
  pool_init();
  curl_init();
  curl_register_config();

  // Nothing is ever promoted out of the submit queue, so no request in
  // this suite reaches libcurl, the network, or a completion callback.
  kv_set("core.curl.max_active", "0");

  case_room_is_immediate();
  case_full_queue_gives_up();
  case_zero_does_not_wait();
  case_room_ends_the_wait();

  return(test_report(SUITE));
}
