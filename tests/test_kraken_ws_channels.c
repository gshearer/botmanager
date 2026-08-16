// botmanager — MIT
// Cases for the kraken WS slot table's index-across-time holes (OBS-5).
//
// The multiplexer names a slot by its position in `kr_ws_ch.slots[]` in
// two places that outlive the lock hold which made the position true:
// across the reconcile emit window, where `mu` is dropped per slot so
// the frame can reach the transport, and across the whole ack round
// trip, where the index is parked in the correlator ring. Compaction is
// swap-with-last, so either one can come back to a different slot.
//
// Every consequence is silent. Fanout matches on the subscriber list and
// never reads slot state, so a wrong write cannot stop a feed that is
// already flowing — it corrupts what the NEXT reconcile decides to emit.
// The endpoints are a subscribe that is never sent (a feed that quietly
// never starts) and an unsubscribe that is never sent (a gateway
// streaming to nobody), and both persist until the next WS flap.
//
// The cases drive the public surface only — subscribe, unsubscribe, and
// injected frames through kr_ws_channels_dispatch. The send stub is the
// interleaving control point: it can park exactly one designated emit,
// which holds a thread inside the window with `mu` released, and that
// is the whole fixture. Assertions read the emitted frame set, because
// what the module emits is the only thing a consumer can observe.

#include "test.h"

#include "kraken_ws_channels.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "exchange_api.h"
#include "kraken_ws.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SUITE "kraken_ws_channels"

// Frames a case can hold. Two products per sub across six cases sits an
// order of magnitude under this.
#define LEDGER_MAX 64

// ------------------------------------------------------------------ //
// The transport stub: a frame ledger with one parkable emit           //
// ------------------------------------------------------------------ //

static pthread_mutex_t  ledger_mutex = PTHREAD_MUTEX_INITIALIZER;
static char             frames[LEDGER_MAX][KR_WS_TX_BUF_SZ];
static size_t           n_frames;

// The armed park, matched as two independent substrings of the frame so
// a case can name (op, symbol) without knowing the wire layout. Cleared
// as it fires: exactly one emit is ever held.
static char             park_op[32];
static char             park_sym[32];

static pthread_mutex_t  gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   gate_cond  = PTHREAD_COND_INITIALIZER;
static bool             parked;
static bool             released;

// Both substrings must be present, and `op` is matched against the
// method field rather than the raw frame: "subscribe" is a substring of
// "unsubscribe", so a bare search reads every unsubscribe as a subscribe.
static bool
frame_is(const char *frame, const char *op, const char *sym)
{
  char needle[48];

  snprintf(needle, sizeof(needle), "\"method\":\"%s\"", op);

  return(strstr(frame, needle) != NULL && strstr(frame, sym) != NULL);
}

bool
kr_ws_send_text(const char *buf, size_t len)
{
  bool   hold = false;
  size_t n;

  pthread_mutex_lock(&ledger_mutex);

  if(n_frames < LEDGER_MAX)
  {
    // buf is a pointer and a length, so the copy takes the length —
    // the ledger's own NUL is what the substring searches below rely on.
    n = (len < sizeof(frames[0]) - 1) ? len : sizeof(frames[0]) - 1;

    memcpy(frames[n_frames], buf, n);
    frames[n_frames][n] = '\0';

    if(park_op[0] != '\0'
        && frame_is(frames[n_frames], park_op, park_sym))
    {
      hold       = true;
      park_op[0] = '\0';
    }

    n_frames++;
  }

  pthread_mutex_unlock(&ledger_mutex);

  if(hold)
  {
    pthread_mutex_lock(&gate_mutex);

    parked = true;
    pthread_cond_broadcast(&gate_cond);

    while(!released)
      pthread_cond_wait(&gate_cond, &gate_mutex);

    pthread_mutex_unlock(&gate_mutex);
  }

  return(SUCCESS);
}

// The remaining five out-of-file references. Public channels reach none
// of the token path; a case that did would be a case bug, so acquire
// refuses rather than inventing a completion.

void
kr_pair_lookup_ws(const char *input, char *out, size_t cap)
{
  if(out == NULL || cap == 0)
    return;

  strlcpy(out, (input != NULL) ? input : "", cap);
}

bool
kr_apikey_configured(void)
{
  return(false);
}

bool
kr_ws_token_snapshot(char *out, size_t cap)
{
  if(out != NULL && cap > 0)
    out[0] = '\0';

  return(FAIL);
}

bool
kr_ws_token_needs_refresh(void)
{
  return(true);
}

bool
kr_ws_token_acquire(kr_ws_token_done_cb_t cb, void *user)
{
  (void)cb;
  (void)user;

  return(FAIL);
}

// ------------------------------------------------------------------ //
// Ledger readers + the fixture                                        //
// ------------------------------------------------------------------ //

static size_t
n_frames_with(const char *op, const char *sym)
{
  size_t count = 0;
  size_t i;

  pthread_mutex_lock(&ledger_mutex);

  for(i = 0; i < n_frames; i++)
  {
    if(frame_is(frames[i], op, sym))
      count++;
  }

  pthread_mutex_unlock(&ledger_mutex);

  return(count);
}

// The req_id of the first frame matching (op, sym); 0 when there is
// none, which is the same sentinel the correlator reserves.
static uint32_t
rid_of(const char *op, const char *sym)
{
  uint32_t rid = 0;
  size_t   i;

  pthread_mutex_lock(&ledger_mutex);

  for(i = 0; i < n_frames; i++)
  {
    const char *p;

    if(!frame_is(frames[i], op, sym))
      continue;

    p = strstr(frames[i], "\"req_id\":");

    if(p != NULL)
      rid = (uint32_t)strtoul(p + strlen("\"req_id\":"), NULL, 10);

    break;
  }

  pthread_mutex_unlock(&ledger_mutex);

  return(rid);
}

static void
arm_park(const char *op, const char *sym)
{
  pthread_mutex_lock(&ledger_mutex);

  strlcpy(park_op, op, sizeof(park_op));
  strlcpy(park_sym, sym, sizeof(park_sym));

  pthread_mutex_unlock(&ledger_mutex);
}

static void
wait_parked(void)
{
  pthread_mutex_lock(&gate_mutex);

  while(!parked)
    pthread_cond_wait(&gate_cond, &gate_mutex);

  pthread_mutex_unlock(&gate_mutex);
}

static void
release_park(void)
{
  pthread_mutex_lock(&gate_mutex);

  released = true;
  pthread_cond_broadcast(&gate_cond);

  pthread_mutex_unlock(&gate_mutex);
}

static void
fixture_reset(void)
{
  kr_ws_channels_deinit();
  kr_ws_channels_init();

  pthread_mutex_lock(&ledger_mutex);

  n_frames    = 0;
  park_op[0]  = '\0';
  park_sym[0] = '\0';

  pthread_mutex_unlock(&ledger_mutex);

  pthread_mutex_lock(&gate_mutex);

  parked   = false;
  released = false;

  pthread_mutex_unlock(&gate_mutex);
}

// ------------------------------------------------------------------ //
// Consumer + driver helpers                                           //
// ------------------------------------------------------------------ //

static size_t ev_count;
static char   ev_product[EXCHANGE_PRODUCT_ID_SZ];

static void
event_cb(const exchange_ws_event_t *ev, void *user)
{
  (void)user;

  ev_count++;
  strlcpy(ev_product, ev->product_id, sizeof(ev_product));
}

static void
subscribe_products(void **out, const char *const *products,
    uint32_t n_products)
{
  const exchange_ws_channel_t channels[] = { EXCH_WS_TICKER };

  *out = NULL;

  kr_ws_subscribe(channels, 1, products, n_products, event_cb, NULL, out);
}

static void
subscribe_one(void **out, const char *product)
{
  const char *const products[] = { product };

  subscribe_products(out, products, 1);
}

// Kraken answers acks and errors on the same frame shape; the parser
// reads only success / req_id / error, so these three fields are the
// whole wire surface a case needs.
static void
inject_ack(const char *method, uint32_t req_id, bool success,
    const char *err)
{
  char buf[256];
  int  n;

  if(err != NULL)
    n = snprintf(buf, sizeof(buf),
        "{\"method\":\"%s\",\"success\":%s,\"req_id\":%u,\"error\":\"%s\"}",
        method, success ? "true" : "false", req_id, err);
  else
    n = snprintf(buf, sizeof(buf),
        "{\"method\":\"%s\",\"success\":%s,\"req_id\":%u}",
        method, success ? "true" : "false", req_id);

  if(n > 0)
    kr_ws_channels_dispatch(buf, (size_t)n);
}

static void
subscribe_and_ack(void **out, const char *product,
    const char *sym)
{
  subscribe_one(out, product);
  inject_ack("subscribe", rid_of("subscribe", sym), true, NULL);
}

// ------------------------------------------------------------------ //
// c1 — the deterministic misroute. No second thread is involved.      //
// ------------------------------------------------------------------ //
//
// kr_ws_unsubscribe emits and compacts under one hold, so the table is
// always shorter by the time the unsubscribe ack arrives. When another
// slot has taken the compacted index, that ack is applied to it — and
// the parser cannot tell an unsubscribe ack from a subscribe one, so
// the write forges ACTIVE / sent_upstream onto a slot the gateway has
// never confirmed. The victim's own ack then finds its index past the
// end of the table and is dropped, leaving the forgery standing: the
// slot is believed live, every later reconcile skips it, and the feed
// never starts.

static void
case_unsub_ack_forges_a_stranger(void)
{
  void *aaa   = NULL;
  void *bbb   = NULL;
  void *probe = NULL;
  uint32_t           rid_bbb;
  uint32_t           rid_unsub;

  fixture_reset();

  subscribe_and_ack(&aaa, "AAA-USD", "AAA");

  // Deliberately un-acked: BBB is still SUBSCRIBING when the table moves.
  subscribe_one(&bbb, "BBB-USD");
  rid_bbb = rid_of("subscribe", "BBB");

  kr_ws_unsubscribe(aaa);
  rid_unsub = rid_of("unsubscribe", "AAA");

  // AAA's slot is gone and BBB now occupies the index its ack names.
  inject_ack("unsubscribe", rid_unsub, true, NULL);

  // BBB's real answer: the gateway rejected it.
  inject_ack("subscribe", rid_bbb, false, "test-reject");

  // A second consumer on BBB gives the next reconcile its chance to act
  // on that rejection.
  subscribe_one(&probe, "BBB-USD");

  test_check_sz(SUITE, "a rejected subscribe is retried by the next reconcile",
      2, n_frames_with("subscribe", "BBB"));
}

// ------------------------------------------------------------------ //
// c2 — the wrong-slot write across the release window                 //
// ------------------------------------------------------------------ //
//
// Two unsubscribes overlap. The first parks inside its emit with `mu`
// free; the second runs to completion, compacting two slots out and
// swapping two survivors down into their indexes. The first then
// resumes on a raw index that now names a live subscription and writes
// its post-emit bookkeeping onto it — clearing sent_upstream on a feed
// nobody unsubscribed. The damage shows on the next reconcile, which
// re-emits a subscribe for a slot the gateway already holds.

static void *
c2_unsubscribe_entry(void *arg)
{
  kr_ws_unsubscribe(arg);

  return(NULL);
}

static void
case_wrong_slot_across_the_release(void)
{
  void *ppp   = NULL;
  void *qqq   = NULL;
  void *rrr   = NULL;
  void *sss   = NULL;
  void *probe = NULL;
  pthread_t          t;

  fixture_reset();

  subscribe_and_ack(&ppp, "PPP-USD", "PPP");
  subscribe_and_ack(&qqq, "QQQ-USD", "QQQ");
  subscribe_and_ack(&rrr, "RRR-USD", "RRR");
  subscribe_and_ack(&sss, "SSS-USD", "SSS");

  arm_park("unsubscribe", "QQQ");

  pthread_create(&t, NULL, c2_unsubscribe_entry, qqq);
  wait_parked();

  // Runs to completion while the other thread is held: emits for PPP and
  // QQQ alike, then compacts both away — SSS and RRR swap down into the
  // indexes they vacated.
  kr_ws_unsubscribe(ppp);

  release_park();
  pthread_join(t, NULL);

  // Any reconcile will do; this one also proves the table still works.
  subscribe_one(&probe, "TTT-USD");

  test_check_sz(SUITE,
      "a live slot keeps its upstream state when a neighbour's "
      "unsubscribe resumes", 1, n_frames_with("subscribe", "RRR"));
}

// ------------------------------------------------------------------ //
// c4 — the emission a moved slot never gets                           //
// ------------------------------------------------------------------ //
//
// The walk cursor is an index too. A compaction during the emit window
// moves the table's last slot down into a vacated index — and when that
// index is below the cursor, the slot passes the walk without being
// seen. The guard on re-entry only tests whether the table shrank past
// the cursor, which this does not. The slot wants a subscribe, nothing
// emits one, and nothing tries again until the next reconcile trigger.

static void *c4_sub;

static void *
c4_subscribe_entry(void *arg)
{
  const char *const products[] = { "CCC-USD", "DDD-USD" };

  (void)arg;

  subscribe_products(&c4_sub, products, 2);

  return(NULL);
}

static void
case_a_moved_slot_loses_its_emission(void)
{
  void *aaa = NULL;
  void *bbb = NULL;
  pthread_t          t;

  fixture_reset();

  subscribe_and_ack(&aaa, "AAA-USD", "AAA");
  subscribe_and_ack(&bbb, "BBB-USD", "BBB");

  arm_park("subscribe", "CCC");

  // One sub over two products: CCC emits first and parks, leaving DDD
  // unvisited at the end of the table.
  pthread_create(&t, NULL, c4_subscribe_entry, NULL);
  wait_parked();

  // AAA's slot is compacted out and DDD swaps down into index 0 —
  // behind the parked walk's cursor.
  kr_ws_unsubscribe(aaa);

  release_park();
  pthread_join(t, NULL);

  test_check_sz(SUITE,
      "a slot moved below the cursor still gets its subscribe",
      1, n_frames_with("subscribe", "DDD"));
}

// ------------------------------------------------------------------ //
// c5 — the ordinary path, guarded against drift                       //
// ------------------------------------------------------------------ //

static void
case_fanout_and_teardown(void)
{
  void *aaa = NULL;
  void *bbb = NULL;
  const char        *tick =
      "{\"channel\":\"ticker\",\"data\":"
      "[{\"symbol\":\"AAA/USD\",\"last\":1.0}]}";

  fixture_reset();

  ev_count      = 0;
  ev_product[0] = '\0';

  subscribe_and_ack(&aaa, "AAA-USD", "AAA");
  subscribe_and_ack(&bbb, "BBB-USD", "BBB");

  kr_ws_channels_dispatch(tick, strlen(tick));

  test_check_sz(SUITE, "one ticker frame reaches one subscriber",
      1, ev_count);

  test_check_str(SUITE, "carrying the product id in abstraction form",
      "AAA-USD", ev_product);

  kr_ws_unsubscribe(aaa);
  kr_ws_unsubscribe(bbb);

  test_check_sz(SUITE, "each unsubscribe emits its own frame",
      1, n_frames_with("unsubscribe", "AAA"));

  test_check_sz(SUITE, "for the product it held",
      1, n_frames_with("unsubscribe", "BBB"));
}

// ------------------------------------------------------------------ //
// c6 — ack semantics that the fix must not change                     //
// ------------------------------------------------------------------ //
//
// A rejected subscribe stays retryable, and a req_id is consumed by the
// ack that answers it — a replay cannot resurrect it. Both hold before
// and after the fix; they are here so the rewrite cannot quietly trade
// one for the other.

static void
case_ack_semantics(void)
{
  void *first  = NULL;
  void *second = NULL;
  void *third  = NULL;
  uint32_t           rid;

  fixture_reset();

  subscribe_one(&first, "EEE-USD");
  rid = rid_of("subscribe", "EEE");

  inject_ack("subscribe", rid, false, "test-reject");

  subscribe_one(&second, "EEE-USD");

  test_check_sz(SUITE, "a rejected slot is retried by the next reconcile",
      2, n_frames_with("subscribe", "EEE"));

  // The rid above has already been answered; replaying it must not mark
  // the pending slot active, which a third consumer's reconcile shows.
  inject_ack("subscribe", rid, true, NULL);

  subscribe_one(&third, "EEE-USD");

  test_check_sz(SUITE, "a replayed ack cannot mark a pending slot active",
      3, n_frames_with("subscribe", "EEE"));
}

int
main(void)
{
  mem_init();
  clam_init();

  case_unsub_ack_forges_a_stranger();
  case_wrong_slot_across_the_release();
  case_a_moved_slot_loses_its_emission();
  case_fanout_and_teardown();
  case_ack_semantics();

  kr_ws_channels_deinit();

  return(test_report(SUITE));
}
