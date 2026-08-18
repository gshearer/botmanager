// botmanager — MIT
// Cases for the gemini WS slot table's belief about what the gateway
// holds (OBS-42).
//
// The table's only record of the upstream state is one bool per slot.
// Four ways it diverges from the gateway, and every one of them is
// silent: fan-out matches on the subscriber list and never reads slot
// state, so a wrong belief cannot stop a feed that is already flowing —
// it corrupts what the NEXT pass decides to emit. The endpoints are a
// gateway streaming a symbol to nobody, and a slot table whose
// occupancy has stopped tracking live subscriptions and started
// tracking ever-subscribed pairs until it fills and feeds silently
// never start.
//
// The cases drive the public surface only — subscribe, unsubscribe,
// on_open, and injected frames through gem_ws_channels_dispatch_md. The
// send stub is the interleaving control point: it can park exactly one
// designated emit, holding a thread inside the window with `mu`
// released, and that is the whole fixture. Assertions read the emitted
// frame set, because what the module emits is the only thing a consumer
// can observe.
//
// ⚠ Gemini is NOT JSON-RPC — there is no `method` field, and the op is
// `"type":"subscribe"` / `"type":"unsubscribe"`. "subscribe" is a
// substring of "unsubscribe", so the matcher below keys on the full
// `"type":"<op>"` token. A bare substring search reads every
// unsubscribe as a subscribe and every case here goes false-green.

#include "test.h"

#include "gemini_ws_channels.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "exchange_api.h"
#include "gemini_ws.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SUITE "gemini_ws_channels"

// Frames a case can hold. g2 drives 128 slots through subscribe and
// on_open, which is the only case that comes near this.
#define LEDGER_MAX 512

// ------------------------------------------------------------------ //
// The transport stub: a frame ledger with one parkable emit           //
// ------------------------------------------------------------------ //

static pthread_mutex_t    ledger_mutex = PTHREAD_MUTEX_INITIALIZER;
static char               frames[LEDGER_MAX][GEM_WS_TX_BUF_SZ];
static gem_ws_session_id_t frame_sid[LEDGER_MAX];
static size_t             n_frames;

// The armed park, matched as two independent substrings of the frame so
// a case can name (op, symbol) without knowing the wire layout. Cleared
// as it fires: exactly one emit is ever held.
static char             park_op[32];
static char             park_sym[32];

// The armed send-FAILURE, matched the same two ways and cleared as it
// fires.
static char             fail_op[32];
static char             fail_sym[32];

static pthread_mutex_t  gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   gate_cond  = PTHREAD_COND_INITIALIZER;
static bool             parked;
static bool             released;

// Both substrings must be present, and `op` is matched against the
// full type token — see the header note.
static bool
frame_is(const char *frame, const char *op, const char *sym)
{
  char needle[48];

  snprintf(needle, sizeof(needle), "\"type\":\"%s\"", op);

  return(strstr(frame, needle) != NULL && strstr(frame, sym) != NULL);
}

static size_t
count_frames(const char *op, const char *sym)
{
  size_t n = 0;

  for(size_t i = 0; i < n_frames; i++)
    if(frame_is(frames[i], op, sym))
      n++;

  return(n);
}

static void
ledger_reset(void)
{
  pthread_mutex_lock(&ledger_mutex);
  n_frames   = 0;
  park_op[0] = '\0';
  fail_op[0] = '\0';
  pthread_mutex_unlock(&ledger_mutex);

  pthread_mutex_lock(&gate_mutex);
  parked   = false;
  released = false;
  pthread_mutex_unlock(&gate_mutex);
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

bool
gem_ws_send_text(gem_ws_session_id_t sid, const char *buf, size_t len)
{
  bool   hold = false;
  bool   die  = false;
  size_t n;

  pthread_mutex_lock(&ledger_mutex);

  if(n_frames < LEDGER_MAX)
  {
    frame_sid[n_frames] = sid;

    // buf is a pointer and a length, so the copy takes the length —
    // the ledger's own NUL is what the substring searches rely on.
    n = (len < sizeof(frames[0]) - 1) ? len : sizeof(frames[0]) - 1;

    memcpy(frames[n_frames], buf, n);
    frames[n_frames][n] = '\0';

    if(park_op[0] != '\0'
        && frame_is(frames[n_frames], park_op, park_sym))
    {
      hold       = true;
      park_op[0] = '\0';
    }

    if(fail_op[0] != '\0'
        && frame_is(frames[n_frames], fail_op, fail_sym))
    {
      die        = true;
      fail_op[0] = '\0';
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

  if(die)
    return(FAIL);

  return(SUCCESS);
}

// The three remaining out-of-file references. No case here reaches the
// USER channel, so creds are always absent — which is also the live
// posture (there are no Gemini keys).
bool
gem_apikey_configured(void)
{
  return(false);
}

// The pair helpers: gemini's REST form is lowercase concatenated, and
// the module uppercases at slot-insert time. "BTC-USD" -> "btcusd".
void
gem_pair_to_native(const char *in, char *out, size_t cap)
{
  size_t o = 0;

  for(size_t i = 0; in != NULL && in[i] != '\0' && o + 1 < cap; i++)
  {
    if(in[i] == '-')
      continue;

    out[o++] = (char)((in[i] >= 'A' && in[i] <= 'Z')
        ? in[i] - 'A' + 'a' : in[i]);
  }

  if(cap > 0)
    out[o] = '\0';
}

// Mirrors the live heuristic split closely enough for these cases:
// every symbol here is a 3-letter base against USD, so "BTCUSD" comes
// back as "BTC-USD" — which is what a subscriber registered, and
// therefore what fan-out matches on.
void
gem_pair_to_abstr(const char *in, char *out, size_t cap)
{
  size_t len = (in != NULL) ? strlen(in) : 0;

  if(out == NULL || cap == 0) return;

  out[0] = '\0';

  if(len == 6)
    snprintf(out, cap, "%.3s-%.3s", in, in + 3);

  else
    strlcpy(out, (in != NULL) ? in : "", cap);
}

// ------------------------------------------------------------------ //
// Fixture helpers                                                     //
// ------------------------------------------------------------------ //

static void
noop_event_cb(const exchange_ws_event_t *ev, void *user)
{
  (void)ev;
  (void)user;
}

// One consumer on one symbol of the ticker channel.
static void *
sub_one(const char *product)
{
  const exchange_ws_channel_t ch = EXCH_WS_TICKER;
  void                       *h  = NULL;

  gem_ws_subscribe(&ch, 1, &product, 1, noop_event_cb, NULL, &h);

  return(h);
}

// The gateway's answer for the l2 channel over one symbol.
static void
feed_ack(const char *native)
{
  char buf[256];
  int  n;

  n = snprintf(buf, sizeof(buf),
      "{\"type\":\"subscription_ack\",\"subscriptions\":"
      "[{\"name\":\"l2\",\"symbols\":[\"%s\"]}]}", native);

  gem_ws_channels_dispatch_md(buf, (size_t)n);
}

typedef struct
{
  void *handle;
} unsub_arg_t;

static void *
unsub_thread(void *arg)
{
  unsub_arg_t *a = arg;

  gem_ws_unsubscribe(a->handle);

  return(NULL);
}

// ------------------------------------------------------------------ //
// Cases                                                               //
// ------------------------------------------------------------------ //

// g1 — the ack that outlived its consumer. The consumer leaves between
// the subscribe frame going out and the ack landing, so its own
// unsubscribe correctly emits nothing (the gateway does not hold it
// yet). When the ack then lands, the slot is ACTIVE at refcount 0 and
// nothing else in the module will ever look at it again.
static void
case_g1(void)
{
  void *h;

  gem_ws_channels_init();
  ledger_reset();

  h = sub_one("BTC-USD");
  test_check_sz(SUITE, "g1: the subscribe frame goes out",
      1, count_frames("subscribe", "BTCUSD"));

  gem_ws_unsubscribe(h);
  test_check_sz(SUITE,
      "g1: a consumer leaving before its ack emits no unsubscribe",
      0, count_frames("unsubscribe", "BTCUSD"));

  feed_ack("BTCUSD");
  test_check_sz(SUITE,
      "g1: the ack that lands on a departed consumer is unsubscribed",
      1, count_frames("unsubscribe", "BTCUSD"));

  gem_ws_channels_deinit();
}

// g2 — the slot the flap could not forget. Each iteration strands one
// slot at refcount 0; on_open must make it forgettable, or the table
// fills with pairs nobody is subscribed to and the next real subscribe
// silently never reaches the wire.
static void
case_g2(void)
{
  // Wide enough for the widest int the format can render, which is
  // what -Wformat-truncation measures — not for the 9 bytes the loop
  // below actually produces.
  char  product[32];
  void *h;

  gem_ws_channels_init();
  ledger_reset();

  for(int i = 0; i < 128; i++)
  {
    snprintf(product, sizeof(product), "A%02dB-USD", i);

    h = sub_one(product);
    gem_ws_unsubscribe(h);
    gem_ws_channels_on_open(GEM_WS_MD);
  }

  ledger_reset();
  h = sub_one("ZZZ-USD");

  test_check_sz(SUITE,
      "g2: a slot stranded before its ack does not survive a flap",
      1, count_frames("subscribe", "ZZZUSD"));

  gem_ws_unsubscribe(h);
  gem_ws_channels_deinit();
}

// g3 — two consumers leaving at once. One is parked inside the emit
// window with `mu` released; the other's reap walks the same table and
// must not emit a second frame for the slot already on the wire.
static void
case_g3(void)
{
  unsub_arg_t arg;
  pthread_t   th;
  void       *ha;
  void       *hb;

  gem_ws_channels_init();
  ledger_reset();

  ha = sub_one("BTC-USD");
  hb = sub_one("ETH-USD");

  feed_ack("BTCUSD");
  feed_ack("ETHUSD");

  ledger_reset();
  arm_park("unsubscribe", "BTCUSD");

  arg.handle = ha;
  pthread_create(&th, NULL, unsub_thread, &arg);
  wait_parked();

  // Thread 2 walks a table in which BTCUSD is still refcount 0 and
  // still held by the gateway.
  gem_ws_unsubscribe(hb);

  release_park();
  pthread_join(th, NULL);

  test_check_sz(SUITE,
      "g3: a slot with a frame already on the wire is not emitted twice",
      1, count_frames("unsubscribe", "BTCUSD"));
  test_check_sz(SUITE, "g3: and the other consumer's slot is emitted once",
      1, count_frames("unsubscribe", "ETHUSD"));

  gem_ws_channels_deinit();
}

// g4 — the write that landed on the neighbour. A thread parked in the
// emit window held an INDEX; a compaction from another thread swaps a
// different slot into that index, and the parked thread's completion
// wrote onto it — clearing a live feed's belief, after which that
// feed's own unsubscribe could never be emitted.
static void
case_g4(void)
{
  unsub_arg_t arg;
  pthread_t   th;
  void       *ha;
  void       *hb;
  void       *hc;

  gem_ws_channels_init();
  ledger_reset();

  ha = sub_one("BTC-USD");
  hb = sub_one("ETH-USD");
  hc = sub_one("SOL-USD");

  feed_ack("BTCUSD");
  feed_ack("ETHUSD");
  feed_ack("SOLUSD");

  ledger_reset();
  arm_park("unsubscribe", "BTCUSD");

  arg.handle = ha;
  pthread_create(&th, NULL, unsub_thread, &arg);
  wait_parked();

  // ETHUSD leaves and is compacted out while BTCUSD's frame is on the
  // wire — this is the swap that moves a live slot into the index the
  // parked thread was holding.
  gem_ws_unsubscribe(hb);

  release_park();
  pthread_join(th, NULL);

  // SOLUSD is the live feed. If the parked completion wrote onto it,
  // its gateway_holds is false and this emits nothing.
  ledger_reset();
  gem_ws_unsubscribe(hc);

  test_check_sz(SUITE,
      "g4: a live slot still draws its own unsubscribe after a compaction",
      1, count_frames("unsubscribe", "SOLUSD"));

  gem_ws_channels_deinit();
}

// g5 — regression, green at HEAD and after. The fix must invent no
// frames: an acked consumer leaving emits exactly one unsubscribe, and
// an unacked one emits none.
static void
case_g5(void)
{
  void *h;

  gem_ws_channels_init();
  ledger_reset();

  h = sub_one("BTC-USD");
  feed_ack("BTCUSD");
  gem_ws_unsubscribe(h);

  test_check_sz(SUITE, "g5: an acked consumer leaving emits one unsubscribe",
      1, count_frames("unsubscribe", "BTCUSD"));

  ledger_reset();
  h = sub_one("ETH-USD");
  gem_ws_unsubscribe(h);

  test_check_sz(SUITE, "g5: an unacked consumer leaving emits none",
      0, count_frames("unsubscribe", "ETHUSD"));

  gem_ws_channels_deinit();
}

// ------------------------------------------------------------------ //
// OBS-58 — what price a synthesized ticker publishes                  //
//                                                                      //
// Gemini has no ticker channel; this module builds one out of the l2   //
// diff stream. The price it published used to be the price of whatever //
// row sat last in `changes`, which is a book level chosen by frame     //
// layout — including a level being REMOVED, and including the top of   //
// the ask ladder on the opening full-book snapshot. Consumers assign   //
// that field into the mark a paper or real fill executes against, so   //
// the wrongness is silent and expensive.                               //
//                                                                      //
// Prices are asserted in hundredths so a failure prints the number.    //
// ------------------------------------------------------------------ //

#define PX_MAX 32

static double px_seen[PX_MAX];
static size_t n_px;

static void
px_event_cb(const exchange_ws_event_t *ev, void *user)
{
  (void)user;

  if(ev->channel != EXCH_WS_TICKER) return;
  if(n_px >= PX_MAX) return;

  px_seen[n_px++] = ev->payload.ticker.price;
}

static size_t
px_cents(double px)
{
  return((size_t)(px * 100.0 + 0.5));
}

static void *
sub_ticker(const char *product)
{
  const exchange_ws_channel_t ch = EXCH_WS_TICKER;
  void                       *h  = NULL;

  n_px = 0;

  gem_ws_subscribe(&ch, 1, &product, 1, px_event_cb, NULL, &h);

  return(h);
}

static void
feed_md(const char *json)
{
  gem_ws_channels_dispatch_md(json, strlen(json));
}

// g6 — the opening full-book snapshot. Its last row is the top of the
// ask ladder: a resting order at $12.3B, which is what `px=1.2345679e+10`
// was. No trade has been seen, so the mid is the answer.
static void
case_g6(void)
{
  void *h;

  gem_ws_channels_init();
  ledger_reset();

  h = sub_ticker("BTC-USD");

  feed_md("{\"type\":\"l2_updates\",\"symbol\":\"BTCUSD\",\"changes\":["
      "[\"buy\",\"59412.50\",\"1.0\"],"
      "[\"sell\",\"59413.50\",\"1.0\"],"
      "[\"sell\",\"12345679000.00\",\"1.0\"]]}");

  test_check_sz(SUITE, "g6: one ticker from one l2_updates", 1, n_px);
  test_check_sz(SUITE, "g6: price is the mid, not the last diff row",
      px_cents(59413.00), px_cents(px_seen[0]));

  gem_ws_unsubscribe(h);
  gem_ws_channels_deinit();
}

// g7 — a level being REMOVED sits last in the diff. The old sweep set
// the price before it tested qty, so a level the venue just deleted
// priced the tick.
static void
case_g7(void)
{
  void *h;

  gem_ws_channels_init();
  ledger_reset();

  h = sub_ticker("ETH-USD");

  feed_md("{\"type\":\"l2_updates\",\"symbol\":\"ETHUSD\",\"changes\":["
      "[\"buy\",\"2000.00\",\"1.0\"],"
      "[\"sell\",\"2002.00\",\"1.0\"],"
      "[\"sell\",\"2500.00\",\"0\"]]}");

  test_check_sz(SUITE, "g7: a removed level does not price the tick",
      px_cents(2001.00), px_cents(px_seen[0]));

  gem_ws_unsubscribe(h);
  gem_ws_channels_deinit();
}

// g8 — trades ride inside the same envelope as the diff. The frame's
// own trade must reach the mark before the ticker reads it, or every
// such ticker is one frame stale.
static void
case_g8(void)
{
  void *h;

  gem_ws_channels_init();
  ledger_reset();

  h = sub_ticker("SOL-USD");

  feed_md("{\"type\":\"l2_updates\",\"symbol\":\"SOLUSD\",\"changes\":["
      "[\"buy\",\"140.00\",\"1.0\"],"
      "[\"sell\",\"141.00\",\"1.0\"]],"
      "\"trades\":[{\"event_id\":1,\"timestamp\":1755400000000,"
      "\"price\":\"140.25\",\"quantity\":\"0.5\",\"side\":\"buy\"}]}");

  test_check_sz(SUITE, "g8: the frame's own trade prices its ticker",
      px_cents(140.25), px_cents(px_seen[0]));

  gem_ws_unsubscribe(h);
  gem_ws_channels_deinit();
}

// g9 — a standalone `trade` envelope, then a diff-only frame. The mark
// persists across frames, so the ticker keeps reporting a traded price
// rather than falling back to the mid.
static void
case_g9(void)
{
  void *h;

  gem_ws_channels_init();
  ledger_reset();

  h = sub_ticker("BTC-USD");

  feed_md("{\"type\":\"trade\",\"symbol\":\"BTCUSD\",\"event_id\":7,"
      "\"timestamp\":1755400000000,\"price\":\"59500.00\","
      "\"quantity\":\"0.1\",\"side\":\"sell\"}");

  feed_md("{\"type\":\"l2_updates\",\"symbol\":\"BTCUSD\",\"changes\":["
      "[\"buy\",\"59412.50\",\"1.0\"],"
      "[\"sell\",\"59413.50\",\"1.0\"]]}");

  test_check_sz(SUITE, "g9: exactly one ticker (the trade emits none)",
      1, n_px);
  test_check_sz(SUITE, "g9: the last trade outranks the mid",
      px_cents(59500.00), px_cents(px_seen[0]));

  gem_ws_unsubscribe(h);
  gem_ws_channels_deinit();
}

// g10 — a diff carrying nothing priceable emits NO ticker. Publishing
// 0.0 would be worse than publishing nothing: the consumer assigns it
// straight into the fill mark, so a zero erases a good price.
static void
case_g10(void)
{
  void *h;

  gem_ws_channels_init();
  ledger_reset();

  h = sub_ticker("BTC-USD");

  feed_md("{\"type\":\"l2_updates\",\"symbol\":\"BTCUSD\",\"changes\":["
      "[\"buy\",\"0\",\"1.0\"]]}");

  test_check_sz(SUITE, "g10: an unpriceable diff emits no ticker",
      0, n_px);

  // One side alone is still a price — better than none, and never
  // wrong by more than the spread.
  feed_md("{\"type\":\"l2_updates\",\"symbol\":\"BTCUSD\",\"changes\":["
      "[\"buy\",\"59412.50\",\"1.0\"]]}");

  test_check_sz(SUITE, "g10: one live side prices the tick",
      px_cents(59412.50), px_cents(px_seen[0]));

  gem_ws_unsubscribe(h);
  gem_ws_channels_deinit();
}

int
main(void)
{
  mem_init();

  case_g1();
  case_g2();
  case_g3();
  case_g4();
  case_g5();
  case_g6();
  case_g7();
  case_g8();
  case_g9();
  case_g10();

  return(test_report(SUITE));
}
