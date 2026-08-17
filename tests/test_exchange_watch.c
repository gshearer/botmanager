// botmanager — MIT
// Cases for the registration watch: the event that says a provider's
// slot table is empty again.
//
// A provider reload tombstones the registry entry, drops the vtable and
// bumps the generation; the reload's new instance revives the same name
// with an EMPTY driver slot table. OBS-19 made the consumer's stale
// handles safe to drop, and deliberately stopped there — so a consumer
// whose state is keyed to that registration (whenmoon's WS
// subscriptions) had no way to learn that everything it held is gone.
// The market feed simply stopped until an unrelated market command
// happened to rebuild it.
//
// The seam that closes it is a watch: the registry fires every watcher
// after each SUCCESSFUL registration — fresh or revived — with no lock
// held, so the callback may re-enter the abstraction and subscribe
// again from inside the fire. These cases drive that contract over the
// public surface only: register a watch, register an exchange, and
// count.
//
// The fixture is test_exchange_ws_gen.c's driver stub, unchanged: the
// watch's whole point is that a callback can subscribe against the
// registration that fired it, and the stub is what makes that
// observable.

#define EXCHANGE_INTERNAL
#include "exchange.h"

#include "test.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SUITE "exchange_watch"

// ------------------------------------------------------------------ //
// The driver stub: subscriber nodes with a deinit that frees them     //
// ------------------------------------------------------------------ //

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

  node     = mem_alloc("test.watch", "node", sizeof(*node));
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
// kr_ws_channels_deinit walks its subscriber list.
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

static bool
subscribe_one(exchange_ws_sub_t **out_handle)
{
  const exchange_ws_channel_t channel    = EXCH_WS_TICKER;
  const char *const           product_id = "BTC-USD";

  return(exchange_ws_subscribe("stub", &channel, 1, &product_id, 1,
      ws_event_noop, NULL, out_handle));
}

// What the watchers saw.
static uint32_t watch_fires;
static char     watch_last_name[EXCHANGE_NAME_SZ];
static void    *watch_last_user;

static uint32_t second_fires;

// c5's observations: the subscribe issued from inside the fire.
static bool                reentrant_ran;
static bool                reentrant_rc;
static exchange_ws_sub_t  *reentrant_handle;

static void
watch_counting_cb(const char *name, void *user)
{
  watch_fires++;
  strlcpy(watch_last_name, name, sizeof(watch_last_name));
  watch_last_user = user;
}

static void
watch_second_cb(const char *name, void *user)
{
  (void)name;
  (void)user;

  second_fires++;
}

// Models whenmoon exactly: the consumer rebuilds its subscriptions from
// inside the callback, against the registration that just fired.
static void
watch_reentrant_cb(const char *name, void *user)
{
  (void)name;
  (void)user;

  reentrant_ran = true;
  reentrant_rc  = subscribe_one(&reentrant_handle);
}

static void
fixture_reset(void)
{
  n_unsub          = 0;
  last_unsub       = NULL;
  watch_fires      = 0;
  second_fires     = 0;
  watch_last_user  = NULL;
  reentrant_ran    = false;
  reentrant_rc     = FAIL;
  reentrant_handle = NULL;

  watch_last_name[0] = '\0';

  // Every case owns the whole table: unregistering a pair that was
  // never registered is a documented no-op.
  exchange_watch_unregister(watch_counting_cb, NULL);
  exchange_watch_unregister(watch_second_cb, NULL);
  exchange_watch_unregister(watch_reentrant_cb, NULL);
}

// ------------------------------------------------------------------ //
// Cases                                                               //
// ------------------------------------------------------------------ //

static void
case_fresh_registration_fires(void)
{
  int sentinel = 0;

  fixture_reset();

  test_check_bool(SUITE, "a watch registers",
      SUCCESS, exchange_watch_register(watch_counting_cb, &sentinel));

  exchange_register("stub", &stub_vt);

  test_check_sz(SUITE, "a fresh registration fires the watch once",
      1, (size_t)watch_fires);
  test_check_str(SUITE, "and names the exchange that registered",
      "stub", watch_last_name);
  test_check_bool(SUITE, "and carries the user pointer it registered with",
      true, watch_last_user == &sentinel);

  exchange_unregister("stub");
  exchange_watch_unregister(watch_counting_cb, &sentinel);
}

// The OBS-23 case in miniature: unload, reload, and the consumer is
// told — because a revived registration is exactly as empty as a fresh
// one.
static void
case_revived_registration_fires(void)
{
  fixture_reset();

  exchange_register("stub", &stub_vt);
  exchange_watch_register(watch_counting_cb, NULL);

  exchange_unregister("stub");
  stub_teardown();

  test_check_sz(SUITE, "unregistration alone fires nothing",
      0, (size_t)watch_fires);

  exchange_register("stub", &stub_vt);

  test_check_sz(SUITE, "a revived registration fires the watch",
      1, (size_t)watch_fires);
  test_check_str(SUITE, "under the same name",
      "stub", watch_last_name);

  exchange_unregister("stub");
  exchange_watch_unregister(watch_counting_cb, NULL);
}

static void
case_unregistered_watch_is_silent(void)
{
  fixture_reset();

  exchange_watch_register(watch_counting_cb, NULL);
  exchange_watch_unregister(watch_counting_cb, NULL);

  exchange_register("stub", &stub_vt);

  test_check_sz(SUITE, "an unregistered watch never fires",
      0, (size_t)watch_fires);

  exchange_unregister("stub");
}

// No exchange lock may be held across the fire, or this deadlocks; the
// entry must also be fully live, or the subscribe is refused.
static void
case_fire_is_reentrant(void)
{
  fixture_reset();

  exchange_watch_register(watch_reentrant_cb, NULL);
  exchange_register("stub", &stub_vt);

  test_check_bool(SUITE, "the callback ran", true, reentrant_ran);
  test_check_bool(SUITE, "a subscribe issued from inside the fire succeeds",
      SUCCESS, reentrant_rc);
  test_check_bool(SUITE, "and yields a handle",
      true, reentrant_handle != NULL);

  exchange_ws_unsubscribe("stub", reentrant_handle);

  test_check_sz(SUITE, "which reaches the driver like any other",
      1, (size_t)n_unsub);

  exchange_unregister("stub");
  exchange_watch_unregister(watch_reentrant_cb, NULL);
}

static void
case_two_watchers_both_fire(void)
{
  fixture_reset();

  exchange_watch_register(watch_counting_cb, NULL);
  exchange_watch_register(watch_second_cb, NULL);

  exchange_register("stub", &stub_vt);

  test_check_sz(SUITE, "the first of two watchers fires",
      1, (size_t)watch_fires);
  test_check_sz(SUITE, "and so does the second",
      1, (size_t)second_fires);

  exchange_unregister("stub");
  exchange_watch_unregister(watch_counting_cb, NULL);
  exchange_watch_unregister(watch_second_cb, NULL);
}

static void
case_refusals(void)
{
  int slot_user[5] = { 0, 1, 2, 3, 4 };

  fixture_reset();

  test_check_bool(SUITE, "a NULL callback is refused",
      FAIL, exchange_watch_register(NULL, NULL));

  test_check_bool(SUITE, "the first registration of a pair is accepted",
      SUCCESS, exchange_watch_register(watch_counting_cb, &slot_user[0]));
  test_check_bool(SUITE, "the same (cb, user) pair a second time is refused",
      FAIL, exchange_watch_register(watch_counting_cb, &slot_user[0]));

  // The same callback with a different user is a different watcher, so
  // three more fill the table of four.
  exchange_watch_register(watch_counting_cb, &slot_user[1]);
  exchange_watch_register(watch_counting_cb, &slot_user[2]);
  exchange_watch_register(watch_counting_cb, &slot_user[3]);

  test_check_bool(SUITE, "a fifth watcher does not fit the table",
      FAIL, exchange_watch_register(watch_counting_cb, &slot_user[4]));

  // A pair that was never registered: no crash, and nothing else is
  // evicted by the miss.
  exchange_watch_unregister(watch_second_cb, &slot_user[0]);

  exchange_register("stub", &stub_vt);

  test_check_sz(SUITE, "the four registered watchers all fire",
      4, (size_t)watch_fires);

  // A FAILED registration — the name is live — must fire nothing.
  watch_fires = 0;

  test_check_bool(SUITE, "registering a live name again is refused",
      FAIL, exchange_register("stub", &stub_vt));
  test_check_sz(SUITE, "and a refused registration fires no watcher",
      0, (size_t)watch_fires);

  exchange_unregister("stub");

  exchange_watch_unregister(watch_counting_cb, &slot_user[0]);
  exchange_watch_unregister(watch_counting_cb, &slot_user[1]);
  exchange_watch_unregister(watch_counting_cb, &slot_user[2]);
  exchange_watch_unregister(watch_counting_cb, &slot_user[3]);
}

int
main(void)
{
  mem_init();
  clam_init();
  exchange_registry_init();

  case_fresh_registration_fires();
  case_revived_registration_fires();
  case_unregistered_watch_is_silent();
  case_fire_is_reentrant();
  case_two_watchers_both_fire();
  case_refusals();

  exchange_registry_destroy();

  return(test_report(SUITE));
}
