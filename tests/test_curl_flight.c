// botmanager — MIT
// Cases for the curl_flight cancel-and-drain promises (include/curl_flight.h).
//
// The flight exists so a plugin's stop() can guarantee one thing: when
// the drain returns zero, no completion callback of this plugin's is
// running or ever will be, and deinit() may free what they read. Get it
// wrong in the obvious direction — count a callback as landed when it
// *starts* rather than when it ends — and everything still looks right:
// the drain returns zero, the unload proceeds, and the callback carries
// on reading a mutex deinit() has since destroyed. Nothing crashes and
// no sanitizer attributes it back (OBS-34).
//
// That is the silent half, and it is why case_drain_waits_for_the_body
// exists: its callback sleeps before landing, so a drain that returns
// early returns *fast*, and the row is stated in elapsed milliseconds.
//
// No request here reaches the network. core.curl.max_active of 0 means
// the multi loop promotes nothing out of the submit queue, so every
// transfer completes exactly one way: the drain cancels it, the loop's
// cancel sweep delivers it, and the callback runs on the multi thread —
// which is the path a real stop() takes too.

#include "test.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "curl.h"
#include "curl_flight.h"
#include "kv.h"
#include "pool.h"
#include "task.h"

#include <time.h>

#define SUITE "curl_flight"

// A request that is never dispatched needs somewhere to claim to be
// going; nothing in this suite reaches the network.
#define NOWHERE "http://127.0.0.1:1/"

// How long the slow callback holds the flight open. Long enough that a
// drain returning early cannot be mistaken for a slow one.
#define BODY_MS 400

static curl_flight_t flight;
static uint32_t      landed = 0;

// A plugin's per-request slot, in the smallest form that still tells the
// truth: the suite runs one request at a time per callback shape, so a
// single module-level slot is the whole of its bookkeeping.
static uint64_t      slot = 0;

static uint64_t
ms_since(const struct timespec *from)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);

  return((uint64_t)(now.tv_sec - from->tv_sec) * 1000U
      + (uint64_t)((now.tv_nsec - from->tv_nsec) / 1000000L));
}

static void
sleep_ms(uint32_t ms)
{
  struct timespec ts;

  ts.tv_sec  = (time_t)(ms / 1000U);
  ts.tv_nsec = (long)(ms % 1000U) * 1000000L;

  nanosleep(&ts, NULL);
}

static void
fast_done(const curl_response_t *resp)
{
  (void)resp;
  landed++;
  curl_flight_close(&flight, curl_request_id(resp->request));
}

// The callback a plugin actually has: it reads the state deinit() is
// about to tear down, and it does so for a while. Landing is its last
// statement, which is the whole contract.
static void
slow_done(const curl_response_t *resp)
{
  sleep_ms(BODY_MS);
  landed++;
  curl_flight_close(&flight, curl_request_id(resp->request));
}

// One piece of work, one leg: open a slot, relay the request into it,
// and give the slot back if the submit is refused.
static bool
submit(curl_done_cb_t cb)
{
  curl_request_t *req;

  if(curl_flight_open(&flight, &slot) != SUCCESS)
    return(false);

  req = curl_request_create(CURL_METHOD_GET, NOWHERE, cb, NULL);

  if(curl_flight_relay(&flight, req, &slot) != SUCCESS)
  {
    curl_flight_close(&flight, slot);
    return(false);
  }

  return(true);
}

// An empty flight is the ordinary case — every plugin here is idle most
// of the time, and stop() must not spend its budget finding that out.
static void
case_empty_drain_is_immediate(void)
{
  struct timespec start;
  uint32_t        left;

  curl_flight_init(&flight);

  clock_gettime(CLOCK_MONOTONIC, &start);
  left = curl_flight_drain(&flight, 5000);

  test_check_sz(SUITE, "an empty flight drains to nothing", 0, left);

  test_check_bool(SUITE, "without waiting for its bound",
      true, ms_since(&start) < 1000);

  curl_flight_destroy(&flight);
}

// The filed request is queued and nothing promotes it, so the only way
// its callback ever runs is the drain's own cancel.
static void
case_drain_cancels_what_it_filed(void)
{
  uint32_t left;

  curl_flight_init(&flight);
  landed = 0;

  test_check_bool(SUITE, "a request is filed and submitted",
      true, submit(fast_done));

  left = curl_flight_drain(&flight, 5000);

  test_check_sz(SUITE, "the drain leaves nothing airborne", 0, left);
  test_check_sz(SUITE, "because the callback ran", 1, landed);

  curl_flight_destroy(&flight);
}

// The row that fails on the obvious mistake. The callback holds for
// BODY_MS before landing; a drain that waits for the *end* of it cannot
// return sooner.
static void
case_drain_waits_for_the_body(void)
{
  struct timespec start;
  uint32_t        left;
  uint64_t        took;

  curl_flight_init(&flight);
  landed = 0;

  test_check_bool(SUITE, "a slow request is filed", true, submit(slow_done));

  clock_gettime(CLOCK_MONOTONIC, &start);
  left = curl_flight_drain(&flight, 5000);
  took = ms_since(&start);

  test_check_sz(SUITE, "the slow flight drains to nothing", 0, left);

  test_check_bool(SUITE, "and the drain outlasts the callback body",
      true, took >= BODY_MS);

  curl_flight_destroy(&flight);
}

// Three at once: the drain re-issues its cancel every pass, so a set
// that empties one callback at a time still empties.
static void
case_drain_empties_a_set(void)
{
  uint32_t left;

  curl_flight_init(&flight);
  landed = 0;

  test_check_bool(SUITE, "three requests are filed",
      true, submit(fast_done) && submit(fast_done) && submit(fast_done));

  left = curl_flight_drain(&flight, 5000);

  test_check_sz(SUITE, "the set drains to nothing", 0, left);
  test_check_sz(SUITE, "and every callback ran", 3, landed);

  curl_flight_destroy(&flight);
}

// After the drain the plugin is being torn down, so work racing stop()
// must be refused at the door rather than left to deliver into
// deinit().
static void
case_grounded_refuses(void)
{
  curl_flight_init(&flight);
  landed = 0;

  curl_flight_drain(&flight, 5000);

  test_check_bool(SUITE, "a grounded flight refuses a submit",
      false, submit(fast_done));

  test_check_sz(SUITE, "and nothing was delivered", 0, landed);

  curl_flight_destroy(&flight);
}

// Two pieces of work open at once, neither on the wire. A close names
// one slot and takes exactly that one — the reason an idle slot carries
// a minted token instead of a zero every other idle slot also carries.
// The same row covers the two blind closes a failure path makes: a
// handle that was never opened, and one closed twice.
static void
case_a_blind_close_takes_nothing(void)
{
  uint64_t a = 0;
  uint64_t b = 0;
  uint32_t left;

  curl_flight_init(&flight);

  test_check_bool(SUITE, "two slots open", true,
      curl_flight_open(&flight, &a) == SUCCESS
      && curl_flight_open(&flight, &b) == SUCCESS);

  test_check_bool(SUITE, "and they are distinguishable", true, a != b);

  curl_flight_close(&flight, 0);   // never opened
  curl_flight_close(&flight, a);
  curl_flight_close(&flight, a);   // already closed

  // Nothing here is on the wire, so the drain has nothing to cancel and
  // waits its whole (short) bound out. That is the point: b is still
  // open, and only b.
  left = curl_flight_drain(&flight, 200);

  test_check_sz(SUITE, "only the slot that was named is gone", 1, left);

  curl_flight_close(&flight, b);
  curl_flight_destroy(&flight);
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

  // Nothing is promoted out of the submit queue, so no request in this
  // suite reaches libcurl or the network. The cancel sweep still runs
  // on every pass of the multi loop, which is what delivers them.
  kv_set("core.curl.max_active", "0");

  case_empty_drain_is_immediate();
  case_drain_cancels_what_it_filed();
  case_drain_waits_for_the_body();
  case_drain_empties_a_set();
  case_grounded_refuses();
  case_a_blind_close_takes_nothing();

  return(test_report(SUITE));
}
