// botmanager — MIT
// Cases for the vtable a dispatch reads while the unload thread clears it.
//
// Every capability shim resolves the exchange, is told the vtable is
// present, and then dereferences it a second time to reach the hook —
// with no lock held either time. `exchange_unregister` NULLs that same
// field under `e->lock` from the unloading plugin's thread, and its own
// comment says the drop is the point: every function pointer in the
// vtable lands in a mapping that is about to go away. The registry
// comment that makes the unlocked read look safe — unregister "by
// contract runs after all consumers have stopped" — holds for daemon
// shutdown ordering and is false for a targeted `/plugin reload kraken`,
// where no dependent is cycled and whenmoon keeps dispatching throughout
// (OBS-24).
//
// So the check and the use straddle the write. A dispatch that passes
// the check one instruction before the unload thread lands calls through
// a NULL vtable, and the daemon's stderr is /dev/null: the log simply
// stops. The same shape sits in the request path, where `exchange_-
// dispatch` reads `vt` three separate times across a build and a submit,
// and in the two enqueue paths, which test `dead` outside the lock they
// then take to push — a request pushed after the drain is never
// completed and its caller waits forever.
//
// The seam that closes it is a snapshot: read the vtable once under
// `e->lock` and dispatch through that pointer, exactly as
// `exchange_ws_unsubscribe` already did for the WS half. A snapshot
// cannot become NULL, so the call either goes to the registration that
// was live when it was read, or is refused; the residue — an unload
// completing while a driver call is executing — is what core's quiesce
// barrier and `plugin_unmap_notify` govern, and no lock here could.
//
// The threaded case is the defect. Pre-fix it does not fail, it
// SEGVs — and under `-fsanitize=thread` it reports the read/write pair
// whether or not the window is hit that run. The stub driver is the
// whole fixture: it counts what reached it, completes every request
// synchronously so the queue never stalls into the task pool, and
// advertises a bucket wide enough that no dispatch ever arms the pump.

#define EXCHANGE_INTERNAL
#include "exchange.h"

#include "test.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#define SUITE "exchange_vt_race"

// Dispatch attempts per racing thread, and registration cycles against
// them. Sized so a plain -O2 run is a fraction of a second: TSan needs
// only that the two accesses overlap once, and the tombstone window is
// wide (a full exchange_register, clam line included), so a refusal is
// certain long before either count runs out.
#define RACE_DISPATCHES  40000
#define RACE_CYCLES      20000

// ------------------------------------------------------------------ //
// The driver stub                                                     //
// ------------------------------------------------------------------ //

// What reached the driver. Written from the racing dispatch threads.
static _Atomic uint64_t n_hook_calls;

static bool
stub_is_authenticated(void)
{
  return(true);
}

static async_rc_t
stub_place_order_async(const exchange_place_order_req_t *req,
    exchange_done_order_cb_t cb, void *user)
{
  (void)req;
  (void)cb;
  (void)user;

  n_hook_calls++;
  return(ASYNC_AIRBORNE);
}

static async_rc_t
stub_fetch_candles_async(const char *product_id, exchange_granularity_t gran,
    int64_t since_ms, int64_t until_ms,
    exchange_done_candles_cb_t cb, void *user)
{
  (void)product_id;
  (void)gran;
  (void)since_ms;
  (void)until_ms;
  (void)cb;
  (void)user;

  n_hook_calls++;
  return(ASYNC_AIRBORNE);
}

// The REST path. `build_request` hands back a handle the abstraction
// owns, `submit` completes it synchronously with a 2xx so the request is
// delivered and freed inside the call — no curl, no task, no retry.
static bool
stub_build_request(exchange_op_kind_t kind, const char *path,
    const char *body_json, void **out_handle)
{
  (void)kind;
  (void)body_json;

  n_hook_calls++;
  *out_handle = mem_strdup("test.vt_race", "handle", path);

  return(SUCCESS);
}

static bool
stub_submit(void *handle, uint8_t prio, exchange_response_cb_t cb,
    void *user)
{
  (void)handle;
  (void)prio;

  n_hook_calls++;
  cb(200, "{}", 2, NULL, user);

  return(SUCCESS);
}

static void
stub_free_request(void *handle)
{
  mem_free(handle);
}

// A bucket nothing can drain: the pump and the retry ladder both reach
// for the task pool, which no test binary brings up.
static const exchange_protocol_vtable_t stub_vt =
{
  .build_request       = stub_build_request,
  .submit              = stub_submit,
  .free_request        = stub_free_request,
  .advertised_rps      = 1000000,
  .advertised_burst    = 1000000,
  .is_authenticated    = stub_is_authenticated,
  .place_order_async   = stub_place_order_async,
  .fetch_candles_async = stub_fetch_candles_async
};

// ------------------------------------------------------------------ //
// Fixture                                                             //
// ------------------------------------------------------------------ //

// Every outcome the racing threads saw, tallied by what the abstraction
// returned rather than by what the stub counted — the question is
// whether a caller can be left without an answer.
static _Atomic uint64_t n_airborne;
static _Atomic uint64_t n_refused;
static _Atomic uint64_t n_undelivered;
static _Atomic uint64_t n_responses;

static void
order_done_noop(const exchange_order_result_t *res, void *user)
{
  (void)res;
  (void)user;
}

static void
candles_done_noop(const exchange_candles_result_t *res, void *user)
{
  (void)res;
  (void)user;
}

static void
response_noop(int http_status, const char *body, size_t body_len,
    const char *err, void *user)
{
  (void)http_status;
  (void)body;
  (void)body_len;
  (void)err;
  (void)user;

  n_responses++;
}

static void
tally(async_rc_t rc)
{
  if(rc == ASYNC_AIRBORNE)
    n_airborne++;

  else if(rc == ASYNC_FAILED_DELIVERED)
    n_refused++;

  else
    n_undelivered++;
}

// The capability half: an authenticated verb and a public one, both
// reaching the driver through the resolver.
static void *
caps_thread(void *arg)
{
  exchange_place_order_req_t req;
  uint32_t                   i;

  (void)arg;

  memset(&req, 0, sizeof(req));
  strlcpy(req.product_id, "BTC-USD", sizeof(req.product_id));
  strlcpy(req.side, "buy", sizeof(req.side));
  strlcpy(req.type, "limit", sizeof(req.type));
  req.price = 1.0;
  req.size  = 1.0;

  for(i = 0; i < RACE_DISPATCHES; i++)
  {
    tally(exchange_place_order_async("stub", &req, order_done_noop, NULL));
    tally(exchange_fetch_candles_async("stub", "BTC-USD", EXCH_GRAN_1M,
        0, 0, candles_done_noop, NULL));
  }

  return(NULL);
}

// The request half: the dispatch loop's own build/submit reads.
static void *
request_thread(void *arg)
{
  uint32_t i;

  (void)arg;

  for(i = 0; i < RACE_DISPATCHES; i++)
    exchange_request("stub", EXCHANGE_PRIO_TRANSACTIONAL,
        EXCHANGE_OP_REST_GET, "/products", NULL, response_noop, NULL);

  return(NULL);
}

// The unload thread: `deinit` drops the vtable, the reload takes the
// tombstone back.
static void *
cycle_thread(void *arg)
{
  uint32_t i;

  (void)arg;

  for(i = 0; i < RACE_CYCLES; i++)
  {
    exchange_unregister("stub");
    exchange_register("stub", &stub_vt);
  }

  return(NULL);
}

// ------------------------------------------------------------------ //
// Cases                                                               //
// ------------------------------------------------------------------ //

// The defect. Pre-fix this case does not fail — it SEGVs on a NULL
// vtable, or reports a data race under TSan.
static void
case_dispatch_against_a_registration_being_dropped(void)
{
  pthread_t caps;
  pthread_t reqs;
  pthread_t cycler;

  n_hook_calls  = 0;
  n_airborne    = 0;
  n_refused     = 0;
  n_undelivered = 0;
  n_responses   = 0;

  exchange_register("stub", &stub_vt);

  pthread_create(&caps, NULL, caps_thread, NULL);
  pthread_create(&reqs, NULL, request_thread, NULL);
  pthread_create(&cycler, NULL, cycle_thread, NULL);

  pthread_join(caps, NULL);
  pthread_join(reqs, NULL);
  pthread_join(cycler, NULL);

  test_check_sz(SUITE, "every capability call ends in a stated outcome",
      (size_t)RACE_DISPATCHES * 2,
      (size_t)(n_airborne + n_refused + n_undelivered));
  test_check_sz(SUITE, "and none of them loses the caller's callback",
      0, (size_t)n_undelivered);
  test_check_bool(SUITE, "the cycling registration refused some of them",
      true, n_refused > 0);
  test_check_bool(SUITE, "and served the rest",
      true, n_airborne > 0);

  exchange_unregister("stub");
}

// The contract the racing case leans on, stated where it cannot flake:
// a tombstoned registration serves nothing and answers everything.
static void
case_tombstone_refuses_every_verb(void)
{
  exchange_place_order_req_t req;

  memset(&req, 0, sizeof(req));
  n_hook_calls = 0;

  exchange_register("stub", &stub_vt);
  exchange_unregister("stub");

  test_check_bool(SUITE, "place_order on a tombstone is refused, cb fired",
      true, exchange_place_order_async("stub", &req, order_done_noop, NULL)
          == ASYNC_FAILED_DELIVERED);
  test_check_bool(SUITE, "fetch_candles on a tombstone is refused, cb fired",
      true, exchange_fetch_candles_async("stub", "BTC-USD", EXCH_GRAN_1M,
          0, 0, candles_done_noop, NULL) == ASYNC_FAILED_DELIVERED);
  test_check_bool(SUITE, "a request against a tombstone is refused",
      FAIL, exchange_request("stub", EXCHANGE_PRIO_TRANSACTIONAL,
          EXCHANGE_OP_REST_GET, "/products", NULL, response_noop, NULL));
  test_check_sz(SUITE, "and the driver was never reached",
      0, (size_t)n_hook_calls);
}

// A live registration is the other half of that contract: the same three
// calls reach the driver and the REST one completes.
static void
case_live_registration_serves_every_verb(void)
{
  exchange_place_order_req_t req;

  memset(&req, 0, sizeof(req));
  n_hook_calls = 0;
  n_responses  = 0;

  exchange_register("stub", &stub_vt);

  test_check_bool(SUITE, "place_order on a live registration is airborne",
      true, exchange_place_order_async("stub", &req, order_done_noop, NULL)
          == ASYNC_AIRBORNE);
  test_check_bool(SUITE, "fetch_candles on a live registration is airborne",
      true, exchange_fetch_candles_async("stub", "BTC-USD", EXCH_GRAN_1M,
          0, 0, candles_done_noop, NULL) == ASYNC_AIRBORNE);
  test_check_bool(SUITE, "a request against a live registration is accepted",
      SUCCESS, exchange_request("stub", EXCHANGE_PRIO_TRANSACTIONAL,
          EXCHANGE_OP_REST_GET, "/products", NULL, response_noop, NULL));
  test_check_sz(SUITE, "the driver saw both hooks, the build and the submit",
      4, (size_t)n_hook_calls);
  test_check_sz(SUITE, "and the request was completed once",
      1, (size_t)n_responses);

  exchange_unregister("stub");
}

// The clam sink: a subscriber that keeps the register/unregister lines
// out of stdout. Without one, clam() prints every line it is handed and
// the racing case is 40,000 of them.
static void
clam_sink(const clam_msg_t *msg)
{
  (void)msg;
}

int
main(void)
{
  mem_init();
  clam_init();
  clam_subscribe("test.vt_race", CLAM_FATAL, NULL, clam_sink);
  exchange_registry_init();

  case_live_registration_serves_every_verb();
  case_tombstone_refuses_every_verb();
  case_dispatch_against_a_registration_being_dropped();

  exchange_registry_destroy();

  return(test_report(SUITE));
}
