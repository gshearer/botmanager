// botmanager — MIT
// Cases for the WS handle that outlives the registration that issued it.
//
// A protocol plugin's deinit frees every subscriber node it handed out,
// but the consumer holding the handle is not told. The registry entry
// survives the unload — it is tombstoned and revived by the reload — so
// after `/plugin reload kraken` the name resolves again and the freed
// handle sails through the abstraction into the *revived* driver, which
// frees it a second time. There is no error path: mem_free on a pointer
// it never issued is a [FATAL] on a stderr that is /dev/null, and the
// daemon simply stops. Where the address has been reused in between, it
// frees an innocent subsystem's live block instead and nothing stops.
//
// The seam that closes it is a registration generation: the abstraction
// owns the public handle, stamps it with the generation it was issued
// under, and forwards to the driver only while the stamp still matches.
//
// The cases drive the public surface only — register, subscribe,
// unregister, unsubscribe. The driver stub is the whole fixture: its
// `ws_subscribe` allocates a node and remembers it, its `ws_unsubscribe`
// records and frees the pointer it was handed, and `stub_teardown()`
// stands in for the deinit that frees whatever is still outstanding.
// That teardown, run right after `exchange_unregister` as the real
// drivers do, is what makes case one the defect rather than a leak.

#define EXCHANGE_INTERNAL
#include "exchange.h"

#include "test.h"

#include <stddef.h>
#include <stdint.h>

#define SUITE "exchange_ws_gen"

// ------------------------------------------------------------------ //
// The driver stub: subscriber nodes with a deinit that frees them     //
// ------------------------------------------------------------------ //

// Two live nodes at once is the deepest any case goes.
#define STUB_NODES_MAX 8

typedef struct
{
  uint32_t id;
} stub_node_t;

static stub_node_t *stub_live[STUB_NODES_MAX];
static uint32_t     n_stub_live;
static uint32_t     next_node_id;

// What the abstraction handed the driver, and how often.
static uint32_t     n_unsub;
static void        *last_unsub;

// Drop `node` from the outstanding set — it has been freed by whoever
// called this, and stub_teardown() must not free it a second time.
static void
stub_forget(const void *node)
{
  uint32_t i;

  for(i = 0; i < n_stub_live; i++)
  {
    if(stub_live[i] == node)
    {
      stub_live[i] = stub_live[--n_stub_live];
      return;
    }
  }
}

static bool
stub_ws_subscribe(const exchange_ws_channel_t *channels, uint32_t n_channels,
    const char *const *product_ids, uint32_t n_products,
    exchange_ws_event_cb_t cb, void *user, void **out_driver_sub)
{
  stub_node_t *node;

  (void)channels;
  (void)n_channels;
  (void)product_ids;
  (void)n_products;
  (void)cb;
  (void)user;

  if(n_stub_live >= STUB_NODES_MAX)
    return(FAIL);

  node     = mem_alloc("test.ws_gen", "node", sizeof(*node));
  node->id = ++next_node_id;

  stub_live[n_stub_live++] = node;
  *out_driver_sub          = node;

  return(SUCCESS);
}

static void
stub_ws_unsubscribe(void *driver_sub)
{
  n_unsub++;
  last_unsub = driver_sub;

  stub_forget(driver_sub);
  mem_free(driver_sub);
}

// The deinit analog: free every node still outstanding, exactly as
// kr_ws_channels_deinit walks its subscriber list. Runs after
// exchange_unregister, mirroring the drivers' own ordering.
static void
stub_teardown(void)
{
  while(n_stub_live > 0)
    mem_free(stub_live[--n_stub_live]);
}

static const exchange_protocol_vtable_t stub_vt =
{
  .advertised_rps   = 1,
  .advertised_burst = 1,
  .ws_subscribe     = stub_ws_subscribe,
  .ws_unsubscribe   = stub_ws_unsubscribe
};

// The one symbol the linked translation units reach for that no suite
// fixture can supply: exchange_request.c is not in this binary. It can
// never fire — the fixture queues no requests.
void
exchange_req_fail(exchange_req_t *r, int http_status, const char *err)
{
  (void)r;
  (void)http_status;
  (void)err;
}

// ------------------------------------------------------------------ //
// Fixture                                                             //
// ------------------------------------------------------------------ //

static void
ws_event_noop(const exchange_ws_event_t *ev, void *user)
{
  (void)ev;
  (void)user;
}

static void
fixture_reset(void)
{
  n_unsub    = 0;
  last_unsub = NULL;
}

static bool
subscribe_one(exchange_ws_sub_t **out_handle)
{
  const exchange_ws_channel_t channel    = EXCH_WS_TICKER;
  const char *const           product_id = "BTC-USD";

  return(exchange_ws_subscribe("stub", &channel, 1, &product_id, 1,
      ws_event_noop, NULL, out_handle));
}

// ------------------------------------------------------------------ //
// Cases                                                               //
// ------------------------------------------------------------------ //

// The defect. Pre-fix this case does not fail — it aborts, because the
// second free is the driver's and mem_free has no error path.
static void
case_handle_from_a_dead_registration(void)
{
  exchange_ws_sub_t *handle = NULL;

  fixture_reset();

  exchange_register("stub", &stub_vt);

  test_check_bool(SUITE, "subscribe on a live registration",
      SUCCESS, subscribe_one(&handle));

  exchange_unregister("stub");
  stub_teardown();
  exchange_register("stub", &stub_vt);

  exchange_ws_unsubscribe("stub", handle);

  test_check_sz(SUITE,
      "a handle issued before the reload never reaches the revived driver",
      0, (size_t)n_unsub);

  exchange_unregister("stub");
}

static void
case_same_epoch_unsubscribe_forwards(void)
{
  exchange_ws_sub_t *handle = NULL;
  const void        *node;

  fixture_reset();

  exchange_register("stub", &stub_vt);
  subscribe_one(&handle);

  node = stub_live[n_stub_live - 1];

  exchange_ws_unsubscribe("stub", handle);

  test_check_sz(SUITE, "a same-epoch unsubscribe reaches the driver once",
      1, (size_t)n_unsub);
  test_check_bool(SUITE, "and carries the node its subscribe returned",
      true, last_unsub == node);

  exchange_unregister("stub");
}

// The tombstone window — unregistered, never revived. This is what the
// old name-resolution guard covered; the generation covers it too.
static void
case_tombstone_window_drops(void)
{
  exchange_ws_sub_t *handle = NULL;

  fixture_reset();

  exchange_register("stub", &stub_vt);
  subscribe_one(&handle);

  exchange_unregister("stub");
  stub_teardown();

  exchange_ws_unsubscribe("stub", handle);

  test_check_sz(SUITE, "an unsubscribe against a tombstone reaches no driver",
      0, (size_t)n_unsub);
}

static void
case_subscribe_refused_while_tombstoned(void)
{
  // A poison value: the call must clear it before it can fail.
  exchange_ws_sub_t *handle = (exchange_ws_sub_t *)&stub_vt;

  fixture_reset();

  exchange_register("stub", &stub_vt);
  exchange_unregister("stub");

  test_check_bool(SUITE, "subscribe on a tombstoned exchange is refused",
      FAIL, subscribe_one(&handle));
  test_check_bool(SUITE, "and leaves no handle behind",
      true, handle == NULL);
}

static void
case_guards(void)
{
  exchange_ws_sub_t *handle = NULL;

  fixture_reset();

  exchange_register("stub", &stub_vt);

  exchange_ws_unsubscribe("stub", NULL);
  exchange_ws_unsubscribe(NULL, NULL);
  exchange_ws_unsubscribe("", NULL);

  test_check_sz(SUITE, "a NULL handle reaches no driver",
      0, (size_t)n_unsub);

  // The handle carries its own binding, so the name is a log field and
  // nothing more — a caller that gets it wrong used to leak the node.
  subscribe_one(&handle);
  exchange_ws_unsubscribe(NULL, handle);

  test_check_sz(SUITE,
      "the handle names its binding, so a missing name still unsubscribes",
      1, (size_t)n_unsub);

  exchange_unregister("stub");
}

int
main(void)
{
  mem_init();
  clam_init();
  exchange_registry_init();

  case_handle_from_a_dead_registration();
  case_same_epoch_unsubscribe_forwards();
  case_tombstone_window_drops();
  case_subscribe_refused_while_tombstoned();
  case_guards();

  exchange_registry_destroy();

  return(test_report(SUITE));
}
