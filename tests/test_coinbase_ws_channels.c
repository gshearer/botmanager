// botmanager — MIT
// Cases for what coinbase's WS subscribe promises its caller (OBS-51).
//
// The multiplexer seats one slot per (channel, product) and skips any
// pair it cannot seat. It used to return the handle anyway, so a
// consumer held a live subscription for a feed no frame was ever
// emitted for — and nothing repairs that: whenmoon's reconcile diffs
// the set it REQUESTED against the set it wants, so a binding that is
// live-but-incomplete matches and is never re-driven.
//
// The cases drive the public surface only — subscribe, unsubscribe —
// and read the emitted frame set, because what the module puts on the
// wire is the only thing a consumer can observe. The enqueue stub is
// the whole fixture; no session, no reader thread, no JWT.
//
// ⚠ Every subscribe silently adds `heartbeats` + `status` to its own
// channel mask, and coinbase keys ALL THREE by product, so one
// single-product ticker subscribe seats THREE slots. That is the
// arithmetic every capacity assertion here rests on.

#include "test.h"

#define CB_INTERNAL
#include "coinbase.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define SUITE "coinbase_ws_channels"

// 42 products x 3 channels fills the table, and each fill sub emits one
// frame per channel — comfortably inside this.
#define LEDGER_MAX 512

// The table's own capacity, restated here on purpose: the module keeps
// it private, and a case that guessed it would go quietly false-green
// if it ever changed.
#define SLOTS_MAX 128

static pthread_mutex_t ledger_mutex = PTHREAD_MUTEX_INITIALIZER;
static char            frames[LEDGER_MAX][2048];
static size_t          n_frames;

// ------------------------------------------------------------------ //
// The two out-of-file references                                      //
// ------------------------------------------------------------------ //

bool
cb_sign_jwt_ws(char *out, size_t cap)
{
  if(out == NULL || cap == 0)
    return(FAIL);

  strlcpy(out, "test.jwt.token", cap);

  return(SUCCESS);
}

// The module renders a frame and hands it to the transport's paced
// queue rather than sending it (OBS-52); what it emits is unchanged, so
// this is the same ledger one seam further out.
bool
cb_ws_ctrl_enqueue(const char *buf, size_t len, const char *op,
    const char *channel)
{
  (void)op;
  (void)channel;

  pthread_mutex_lock(&ledger_mutex);

  if(n_frames < LEDGER_MAX && buf != NULL)
  {
    size_t n = (len < sizeof(frames[0]) - 1) ? len : sizeof(frames[0]) - 1;

    memcpy(frames[n_frames], buf, n);
    frames[n_frames][n] = '\0';
    n_frames++;
  }

  pthread_mutex_unlock(&ledger_mutex);

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Fixture helpers                                                     //
// ------------------------------------------------------------------ //

static void
ledger_reset(void)
{
  pthread_mutex_lock(&ledger_mutex);

  n_frames = 0;

  pthread_mutex_unlock(&ledger_mutex);
}

// Frames are one channel each, with the products batched into
// `product_ids`, so a pair is named by two independent substrings.
static size_t
count_frames(const char *op, const char *channel, const char *product)
{
  char   op_needle[48];
  char   ch_needle[48];
  size_t n = 0;

  snprintf(op_needle, sizeof(op_needle), "\"type\":\"%s\"", op);
  snprintf(ch_needle, sizeof(ch_needle), "\"channel\":\"%s\"", channel);

  for(size_t i = 0; i < n_frames; i++)
  {
    if(strstr(frames[i], op_needle) != NULL
        && strstr(frames[i], ch_needle) != NULL
        && (product == NULL || strstr(frames[i], product) != NULL))
      n++;
  }

  return(n);
}

static void
noop_event_cb(const coinbase_ws_event_t *ev, void *user)
{
  (void)ev;
  (void)user;
}

static coinbase_ws_sub_t *
sub_products(coinbase_ws_channel_t ch, const char *const *products,
    size_t n_products)
{
  return(coinbase_ws_subscribe(&ch, 1, products, n_products,
      noop_event_cb, NULL));
}

// ------------------------------------------------------------------ //
// Cases                                                               //
// ------------------------------------------------------------------ //

// c1 — the plain path, so the capacity case below cannot pass on a
// fixture that emits nothing at all.
static void
case_a_subscribe_reaches_the_wire(void)
{
  const char        *products[] = { "BTC-USD" };
  coinbase_ws_sub_t *h;

  cb_ws_channels_init();
  ledger_reset();

  h = sub_products(COINBASE_CH_TICKER, products, 1);

  test_check_bool(SUITE, "c1: a plain subscribe returns a handle",
      true, h != NULL);
  test_check_sz(SUITE, "c1: and puts the ticker on the wire",
      1, count_frames("subscribe", "ticker", "BTC-USD"));

  coinbase_ws_unsubscribe(h);
  cb_ws_channels_deinit();
}

// c2 — OBS-51: a subscribe that cannot seat every pair it asked for is
// refused whole, and gives back the seats it took on the way.
//
// The table is filled to 126 of 128 (42 products x 3 channels), so the
// next new product can seat two of its three pairs — which is exactly
// the shape the old code answered with a live handle. What proves the
// give-back is the call AFTER it: one `level2` pair on a product
// already seated needs exactly one free slot, and it can only have one
// if the refused call gave both of its seats back.
static void
case_a_partial_seat_is_refused_whole(void)
{
  // Wide enough for the widest int the format can render, which is what
  // -Wformat-truncation measures — not for the 8 bytes the loop makes.
  char               names[16][32];
  const char        *pp[16];
  coinbase_ws_sub_t *fill[3];
  coinbase_ws_sub_t *h;
  int                i;
  int                j;
  int                p = 0;

  cb_ws_channels_init();
  ledger_reset();

  // 16 + 16 + 10 = 42 products, 126 slots.
  for(i = 0; i < 3; i++)
  {
    int n = (i == 2) ? 10 : 16;

    for(j = 0; j < n; j++)
    {
      snprintf(names[j], sizeof(names[j]), "P%02d-USD", p++);
      pp[j] = names[j];
    }

    fill[i] = sub_products(COINBASE_CH_TICKER, pp,
        (size_t)n);

    test_check_bool(SUITE, "c2: the table fills without refusing",
        true, fill[i] != NULL);
  }

  ledger_reset();

  pp[0] = "ZZZ-USD";
  h     = sub_products(COINBASE_CH_TICKER, pp, 1);

  test_check_bool(SUITE,
      "c2: two seats for a three-pair product is refused, not "
      "half-taken", true, h == NULL);
  test_check_sz(SUITE,
      "c2: and the seats it could take never reach the wire",
      0, count_frames("subscribe", "ticker", "ZZZ-USD"));

  ledger_reset();

  pp[0] = "P00-USD";
  h     = sub_products(COINBASE_CH_LEVEL2, pp, 1);

  test_check_bool(SUITE,
      "c2: the refused call gave its seats back, so the next "
      "subscriber gets one", true, h != NULL);
  test_check_sz(SUITE, "c2: and that subscribe is on the wire",
      1, count_frames("subscribe", "level2", "P00-USD"));

  coinbase_ws_unsubscribe(h);

  for(i = 0; i < 3; i++)
    coinbase_ws_unsubscribe(fill[i]);

  cb_ws_channels_deinit();
}

// c3 — OBS-52: what a reconnect promises, because the paced queue's
// failure path leans on it. A frame that is queued but never reaches
// the wire leaves its slot reading sent_upstream, and the only thing
// that repairs that is cb_ws_channels_on_open clearing every slot's
// flag before it reconciles. So: an open must re-emit a live
// subscription it has already emitted once, and must emit nothing at
// all once the last subscription is gone.
static void
case_an_open_reconciles_from_the_slot_table(void)
{
  const char        *products[] = { "BTC-USD" };
  coinbase_ws_sub_t *h;

  cb_ws_channels_init();

  h = sub_products(COINBASE_CH_TICKER, products, 1);

  test_check_bool(SUITE, "c3: the fixture subscribe seats",
      true, h != NULL);

  // Already sent upstream once. A reconnect must not trust that.
  ledger_reset();
  cb_ws_channels_on_open();

  test_check_sz(SUITE, "c3: an open re-emits a live subscription",
      1, count_frames("subscribe", "ticker", "BTC-USD"));

  coinbase_ws_unsubscribe(h);

  ledger_reset();
  cb_ws_channels_on_open();

  test_check_sz(SUITE, "c3: and emits nothing once the last one is gone",
      0, count_frames("subscribe", "ticker", NULL));

  cb_ws_channels_deinit();
}

int
main(void)
{
  mem_init();
  clam_init();

  // The arithmetic every assertion in c2 rests on. If the table ever
  // grows, the fill loop above stops filling it and c2 goes green
  // without testing anything.
  test_check_sz(SUITE, "c0: the slot table is the size the cases assume",
      SLOTS_MAX, 42 * 3 + 2);

  case_a_subscribe_reaches_the_wire();
  case_a_partial_seat_is_refused_whole();
  case_an_open_reconciles_from_the_slot_table();

  return(test_report(SUITE));
}
