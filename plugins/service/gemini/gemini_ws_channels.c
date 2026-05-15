// botmanager — MIT
// Gemini WebSocket channel multiplexer.
//
// Layered on top of gemini_ws.c's two-session transport. Owns:
//   * the local subscriber list (one per `exchange_ws_subscribe` call),
//   * a per-(channel, native_sym) slot table that refcounts shared
//     subscribers so N consumers watching the same feed share one
//     upstream subscription,
//   * a monotonic req_id counter (informational — Gemini Market Data
//     v2 does not echo req_ids on the wire; correlation against
//     `subscription_ack` envelopes goes through (channel, symbol)
//     matching instead),
//   * per-channel parsers turning one inbound MD/OE frame into one or
//     more fanned-out exchange_ws_event_t events.
//
// Wire format is Gemini's Market Data v2 + Order Events stream:
//
//   MD subscribe (caller-driven; sent on GEM_WS_MD at OPEN):
//     {"type":"subscribe",
//      "subscriptions":[
//        {"name":"l2",         "symbols":["BTCUSD","ETHUSD"]},
//        {"name":"candles_1m", "symbols":["BTCUSD"]},
//        {"name":"trades",     "symbols":["BTCUSD"]}
//      ]}
//
//   MD frames:
//     heartbeat        — {"type":"heartbeat",...}                      no-op
//     subscription_ack — confirms subscribe; correlated by (chan,sym)
//     l2_updates       — initial snapshot + ongoing diff updates;
//                         carries inline `trades` arrays
//     trade            — individual trade prints
//     candles_1m_updates — initial/streaming 1-minute bars
//
//   OE subscribe — none. Account-implicit. HMAC handshake auths.
//
//   OE frames:
//     subscription_ack — log only
//     heartbeat        — no-op
//     [array]          — initial snapshot list
//     initial / accepted / booked / fill / cancelled / rejected /
//       cancel_rejected / closed — order lifecycle envelopes
//
// On reconnect the slot table drives a full resubscribe so consumer
// callbacks never miss a beat across an MD flap. OE reconnect re-signs
// the handshake (handled in gemini_ws.c); no further multiplexer work
// is needed beyond logging the OPEN transition.
//
// Per `finding_cb_ws_seq_gap_false_positive`, we leave
// exchange_ws_event_t.gap ABI-only / always-false. No per-product gap
// detection.

#define GEM_INTERNAL
#include "gemini.h"

#include "json.h"

#include <json-c/json.h>

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// ------------------------------------------------------------------ //
// Capacity limits.                                                    //
// (Defined here too as compile-time asserts that the sizing macros    //
// in gemini.h stayed consistent with the slot/sub structs below.)    //
// ------------------------------------------------------------------ //

#define GEM_WS_CH_MAX_SUBS              GEM_SUBS_MAX
#define GEM_WS_CH_MAX_PRODUCTS_PER_SUB  GEM_PRODS_PER_SUB_MAX
#define GEM_WS_CH_MAX_SLOTS             GEM_SLOTS_MAX
#define GEM_WS_CH_REQ_RING_SIZE         GEM_REQ_ID_RING

// Internal channel enum. EXCH_WS_BOOK_L2 is not exposed; the four
// remaining abstract channels each map to exactly one Gemini channel.
typedef enum
{
  GEM_CH_TICKER      = 0,    // derived from l2_updates best-bid/ask
  GEM_CH_TRADES      = 1,    // `trades` channel
  GEM_CH_OHLC_1M     = 2,    // `candles_1m` channel
  GEM_CH_USER        = 3,    // Order Events stream (account-implicit)
  GEM_CH__COUNT
} gem_ws_channel_t;

static bool
gem_ws_channel_is_per_symbol(gem_ws_channel_t ch)
{
  // GEM_CH_USER is account-wide; everything else is symbol-keyed.
  return(ch != GEM_CH_USER);
}

// MD subscribe-frame channel name. GEM_CH_USER is NULL because it's
// bound to the OE session implicitly.
static const char *
gem_ws_md_channel_name(gem_ws_channel_t ch)
{
  switch(ch)
  {
    case GEM_CH_TICKER:  return("l2");
    case GEM_CH_TRADES:  return("trades");
    case GEM_CH_OHLC_1M: return("candles_1m");
    case GEM_CH_USER:    return(NULL);
    case GEM_CH__COUNT:  break;
  }
  return(NULL);
}

// ------------------------------------------------------------------ //
// Subscription handle. One per gem_ws_subscribe() call; freed by      //
// gem_ws_unsubscribe(). `channel_mask` is a bitmask over the abstract //
// exchange_ws_channel_t enum so fanout membership tests are single-op.//
// ------------------------------------------------------------------ //

struct exchange_ws_sub
{
  uint32_t                id;
  exchange_ws_event_cb_t  cb;
  void                   *user;

  uint32_t                channel_mask;  // 1u << exchange_ws_channel_t
  uint32_t                n_products;
  // products[] are stored in abstraction form (e.g. "BTC-USD") so
  // fanout matches byte-for-byte against ev->product_id.
  char                    products[GEM_WS_CH_MAX_PRODUCTS_PER_SUB]
                                  [EXCHANGE_PRODUCT_ID_SZ];

  struct exchange_ws_sub *next;
};

typedef enum
{
  GEM_SUB_IDLE,
  GEM_SUB_SUBSCRIBING,
  GEM_SUB_ACTIVE,
  GEM_SUB_FAILED
} gem_sub_state_t;

// Per-(channel, native_sym) dedup slot. `symbol_native` is the empty
// string for non-symbol-keyed channels (USER). `state` advances
// IDLE → SUBSCRIBING (frame sent) → ACTIVE (ack received) or FAILED
// (server-side rejection — surfaces in the log only).
//
// `symbol_native` is the WS form Gemini emits, which is the uppercase
// concatenated `BTCUSD` (gem_pair_to_native returns lowercase `btcusd`
// for REST; we uppercase at slot-insert time so subscription_ack
// matching can compare byte-for-byte against incoming envelopes).
typedef struct
{
  gem_ws_channel_t channel;
  char             symbol_native[16];   // Gemini WS form: BTCUSD
  uint32_t         refcount;
  gem_sub_state_t  state;
  uint32_t         req_id;        // monotonic per slot; informational
  bool             sent_upstream; // true once subscribe ack received
  char             last_err[128];
} gem_ws_slot_t;

static struct
{
  pthread_mutex_t          mu;
  struct exchange_ws_sub  *head;
  uint32_t                 next_id;
  uint32_t                 n_subs;

  gem_ws_slot_t            slots[GEM_WS_CH_MAX_SLOTS];
  uint32_t                 n_slots;

  _Atomic uint32_t         next_req_id;

  bool                     initialized;
} gem_ws_ch;

// ------------------------------------------------------------------ //
// Symbol case helpers                                                 //
// ------------------------------------------------------------------ //

// Uppercase an ASCII string in place.
static void
gem_str_upper(char *s)
{
  size_t i;

  if(s == NULL) return;

  for(i = 0; s[i] != '\0'; i++)
  {
    if(s[i] >= 'a' && s[i] <= 'z')
      s[i] = (char)(s[i] - 32);
  }
}

// Lowercase an ASCII string in place; safe on the small EXCHANGE_SIDE_SZ
// buffers fanout consumers expect to be "buy"/"sell".
static void
gem_str_lower(char *s)
{
  size_t i;

  if(s == NULL) return;

  for(i = 0; s[i] != '\0'; i++)
  {
    if(s[i] >= 'A' && s[i] <= 'Z')
      s[i] = (char)(s[i] + 32);
  }
}

// Translate any-form symbol → Gemini WS native form (uppercase
// concatenated, e.g. "BTCUSD"). Cache lookup first (gem_pair_to_native
// gives lowercase), then uppercase in place. Falls back to the
// caller's input if the cache lookup is empty.
static void
gem_ws_symbol_to_native(const char *in, char *out, size_t cap)
{
  if(out == NULL || cap == 0) return;

  out[0] = '\0';

  if(in == NULL || in[0] == '\0') return;

  gem_pair_to_native(in, out, cap);

  if(out[0] == '\0')
  {
    // Cache miss — passthrough; uppercase below.
    size_t n = strnlen(in, cap - 1);
    memcpy(out, in, n);
    out[n] = '\0';
  }

  gem_str_upper(out);
}

// Translate any-form symbol → abstraction form (uppercase hyphenated,
// e.g. "BTC-USD"). Cache lookup first; fallback empty output so
// callers can detect a cache miss and skip the event (per docs the
// abstraction surface refuses unknown products).
static void
gem_ws_symbol_to_abstr(const char *in, char *out, size_t cap)
{
  if(out == NULL || cap == 0) return;

  out[0] = '\0';

  if(in == NULL || in[0] == '\0') return;

  gem_pair_to_abstr(in, out, cap);
}

// ------------------------------------------------------------------ //
// Slot table — caller holds gem_ws_ch.mu                              //
// ------------------------------------------------------------------ //

static int32_t
gem_ws_slot_find_locked(gem_ws_channel_t ch, const char *symbol_native)
{
  const char *sym = (symbol_native != NULL) ? symbol_native : "";

  for(uint32_t i = 0; i < gem_ws_ch.n_slots; i++)
  {
    gem_ws_slot_t *s = &gem_ws_ch.slots[i];

    if(s->channel == ch && strcmp(s->symbol_native, sym) == 0)
      return((int32_t)i);
  }

  return(-1);
}

static gem_ws_slot_t *
gem_ws_slot_alloc_locked(gem_ws_channel_t ch, const char *symbol_native)
{
  gem_ws_slot_t *s;
  uint32_t       rid;

  if(gem_ws_ch.n_slots >= GEM_WS_CH_MAX_SLOTS)
    return(NULL);

  s = &gem_ws_ch.slots[gem_ws_ch.n_slots++];

  memset(s, 0, sizeof(*s));
  s->channel = ch;
  s->state   = GEM_SUB_IDLE;

  // Allocate a monotonic req_id for tracing — Gemini doesn't echo it
  // back but operators can correlate against multiplexer logs.
  for(;;)
  {
    rid = atomic_fetch_add(&gem_ws_ch.next_req_id, 1u) + 1u;

    if(rid != 0)
      break;
  }
  s->req_id = rid;

  snprintf(s->symbol_native, sizeof(s->symbol_native), "%s",
      (symbol_native != NULL) ? symbol_native : "");

  return(s);
}

// Drop slots whose refcount hit zero. Swap-with-last is safe because
// no external index escapes this module.
static void
gem_ws_slots_compact_locked(void)
{
  for(uint32_t i = 0; i < gem_ws_ch.n_slots; )
  {
    if(gem_ws_ch.slots[i].refcount == 0
        && gem_ws_ch.slots[i].state != GEM_SUB_SUBSCRIBING)
    {
      gem_ws_ch.slots[i] = gem_ws_ch.slots[gem_ws_ch.n_slots - 1];
      gem_ws_ch.n_slots--;
    }
    else
      i++;
  }
}

// ------------------------------------------------------------------ //
// MD subscribe / unsubscribe frame rendering                          //
//                                                                      //
// Gemini's subscribe frame groups symbols per channel. We render one  //
// frame per channel covering every live native symbol, so a single    //
// subscribe-on-open burst comprises at most GEM_CH__COUNT-1 frames    //
// (USER is not an MD channel).                                        //
// ------------------------------------------------------------------ //

// Append `,"SYM"` (or `"SYM"` for the first) to a JSON array body.
// Returns updated `pos`, or 0 on overflow.
static size_t
gem_ws_json_append_sym(char *out, size_t cap, size_t pos,
    bool first, const char *sym)
{
  int n;

  if(out == NULL || cap == 0 || pos >= cap) return(0);

  n = snprintf(out + pos, cap - pos, "%s\"%s\"",
      first ? "" : ",", sym);

  if(n < 0 || (size_t)n >= cap - pos)
    return(0);

  return(pos + (size_t)n);
}

// Build a `subscribe` frame for one Gemini channel covering every
// live native symbol with refcount>0. Returns bytes written, 0 on
// no-op (channel has no live slots) or overflow.
static size_t
gem_ws_render_subscribe_for_channel_locked(gem_ws_channel_t ch,
    char *out, size_t cap)
{
  const char *chan_name;
  size_t      pos    = 0;
  bool        first  = true;
  uint32_t    nsyms  = 0;
  int         n;
  uint32_t    i;

  chan_name = gem_ws_md_channel_name(ch);
  if(chan_name == NULL) return(0);

  n = snprintf(out, cap,
      "{\"type\":\"subscribe\",\"subscriptions\":[{"
      "\"name\":\"%s\",\"symbols\":[",
      chan_name);
  if(n < 0 || (size_t)n >= cap) return(0);
  pos = (size_t)n;

  for(i = 0; i < gem_ws_ch.n_slots; i++)
  {
    gem_ws_slot_t *s = &gem_ws_ch.slots[i];

    if(s->channel != ch) continue;
    if(s->refcount == 0) continue;
    if(s->symbol_native[0] == '\0') continue;

    pos = gem_ws_json_append_sym(out, cap, pos, first,
        s->symbol_native);
    if(pos == 0) return(0);
    first = false;
    nsyms++;
  }

  if(nsyms == 0) return(0);

  n = snprintf(out + pos, cap - pos, "]}]}");
  if(n < 0 || (size_t)n >= cap - pos) return(0);
  pos += (size_t)n;

  return(pos);
}

// Same shape, but for unsubscribe. `chan_syms` is a flat array of
// (slot_idx, symbol) we emit for. We build off the slot-table walk
// inline from the caller; this helper just renders a single symbol.
static size_t
gem_ws_render_unsubscribe_one(gem_ws_channel_t ch, const char *sym,
    char *out, size_t cap)
{
  const char *chan_name;
  int         n;

  chan_name = gem_ws_md_channel_name(ch);
  if(chan_name == NULL) return(0);
  if(sym == NULL || sym[0] == '\0') return(0);

  n = snprintf(out, cap,
      "{\"type\":\"unsubscribe\",\"subscriptions\":[{"
      "\"name\":\"%s\",\"symbols\":[\"%s\"]}]}",
      chan_name, sym);

  if(n < 0 || (size_t)n >= cap) return(0);

  return((size_t)n);
}

// Emit subscribe frames for every channel with live slots. Mu held
// by caller; this function temporarily releases the lock when calling
// gem_ws_send_text so the transport lock can be taken without
// inverting order.
static void
gem_ws_emit_resubscribe_locked(void)
{
  for(int c = 0; c < GEM_CH__COUNT; c++)
  {
    gem_ws_channel_t  ch    = (gem_ws_channel_t)c;
    char              frame[GEM_WS_TX_BUF_SZ];
    size_t            flen;
    bool              ok;
    uint32_t          i;

    if(!gem_ws_channel_is_per_symbol(ch)) continue;

    flen = gem_ws_render_subscribe_for_channel_locked(ch, frame,
        sizeof(frame));

    if(flen == 0) continue;

    // Mark slots SUBSCRIBING before releasing the lock — a concurrent
    // unsubscribe must not free the slot during the send.
    for(i = 0; i < gem_ws_ch.n_slots; i++)
    {
      gem_ws_slot_t *s = &gem_ws_ch.slots[i];

      if(s->channel == ch && s->refcount > 0)
      {
        s->state         = GEM_SUB_SUBSCRIBING;
        s->sent_upstream = false;
      }
    }

    pthread_mutex_unlock(&gem_ws_ch.mu);
    ok = gem_ws_send_text(GEM_WS_MD, frame, flen);
    pthread_mutex_lock(&gem_ws_ch.mu);

    if(ok != SUCCESS)
    {
      clam(CLAM_DEBUG, GEM_CTX ".ws.md",
          "%s subscribe deferred (send failed; retry on next open)",
          gem_ws_md_channel_name(ch));

      // Roll slots back to IDLE so the next open retries.
      for(i = 0; i < gem_ws_ch.n_slots; i++)
      {
        gem_ws_slot_t *s = &gem_ws_ch.slots[i];

        if(s->channel == ch && s->state == GEM_SUB_SUBSCRIBING)
          s->state = GEM_SUB_IDLE;
      }
      continue;
    }

    clam(CLAM_INFO, GEM_CTX ".ws.md", "%s subscribe sent (%zu bytes)",
        gem_ws_md_channel_name(ch), flen);
  }
}

// Emit unsubscribe frames for slots whose refcount has dropped to
// zero AND were previously acked. Mu held by caller; temporarily
// released around the send.
static void
gem_ws_emit_unsubscribe_locked(void)
{
  uint32_t i;

  for(i = 0; i < gem_ws_ch.n_slots; i++)
  {
    gem_ws_slot_t  snap;
    gem_ws_slot_t *s   = &gem_ws_ch.slots[i];
    char           frame[GEM_WS_TX_BUF_SZ];
    size_t         flen;
    bool           ok;

    if(s->refcount != 0) continue;
    if(!s->sent_upstream) continue;
    if(!gem_ws_channel_is_per_symbol(s->channel)) continue;

    snap = *s;

    flen = gem_ws_render_unsubscribe_one(snap.channel, snap.symbol_native,
        frame, sizeof(frame));

    if(flen == 0) continue;

    pthread_mutex_unlock(&gem_ws_ch.mu);
    ok = gem_ws_send_text(GEM_WS_MD, frame, flen);
    pthread_mutex_lock(&gem_ws_ch.mu);

    if(i >= gem_ws_ch.n_slots) { i = (uint32_t)-1; continue; }

    s = &gem_ws_ch.slots[i];

    if(ok != SUCCESS)
    {
      clam(CLAM_DEBUG, GEM_CTX ".ws.md",
          "%s unsubscribe deferred (send failed)",
          gem_ws_md_channel_name(snap.channel));
      continue;
    }

    clam(CLAM_INFO, GEM_CTX ".ws.md",
        "%s unsubscribe sent sym=%s",
        gem_ws_md_channel_name(snap.channel), snap.symbol_native);

    s->sent_upstream = false;
    s->state         = GEM_SUB_IDLE;
  }
}

// ------------------------------------------------------------------ //
// Fan-out                                                             //
// ------------------------------------------------------------------ //

// Deliver one event to every sub whose channel/product set covers it.
// Caller holds gem_ws_ch.mu; callbacks fire with the lock taken.
//
// For account-global events (USER with empty product_id), every sub
// that subscribes to the matching channel matches regardless of its
// per-product set.
static void
gem_ws_fanout_locked(const exchange_ws_event_t *ev)
{
  for(struct exchange_ws_sub *s = gem_ws_ch.head; s != NULL; s = s->next)
  {
    bool match;

    if(!(s->channel_mask & (1u << ev->channel)))
      continue;

    if(ev->product_id[0] == '\0' || s->n_products == 0)
    {
      // Account-global event OR sub didn't constrain by product.
      match = true;
    }
    else
    {
      match = false;
      for(uint32_t i = 0; i < s->n_products; i++)
      {
        if(strcmp(s->products[i], ev->product_id) == 0)
        {
          match = true;
          break;
        }
      }
    }

    if(match && s->cb != NULL)
      s->cb(ev, s->user);
  }
}

// ------------------------------------------------------------------ //
// MD: l2_updates parser                                               //
//                                                                      //
// Frame shape:                                                         //
//   {"type":"l2_updates",                                              //
//    "symbol":"BTCUSD",                                                //
//    "changes":[                                                       //
//      ["buy",  "59412.50","1.234"],   (price, remaining_qty)         //
//      ["sell", "59413.10","0.500"], ...                               //
//    ],                                                                //
//    "trades":[                                                        //
//      {"event_id":..,"timestamp":..,"price":"...","quantity":"...",  //
//       "side":"buy"}, ...                                             //
//    ],                                                                //
//    "auction_events":[...]   (ignored)                                //
//   }                                                                  //
//                                                                      //
// We derive a ticker (best-bid, best-ask, last) from the snapshot or  //
// the latest update, and emit any inline `trades` as TRADES events.   //
// 24h aggregates (volume_24h / low_24h / high_24h) come from the      //
// derived ticker REST poller in a follow-up (out of scope here — the  //
// fields are left zero in the event payload; consumers tolerant of    //
// missing data, e.g. whenmoon, ignore them).                          //
// ------------------------------------------------------------------ //

static int64_t
gem_ws_now_envelope_ms(struct json_object *root)
{
  int64_t ts = 0;

  // Gemini's MD v2 carries "timestamp" as a number of ms.
  if(!json_get_int64(root, "timestamp", &ts))
  {
    struct timespec now;

    clock_gettime(CLOCK_REALTIME, &now);
    ts = (int64_t)now.tv_sec * 1000 + (int64_t)(now.tv_nsec / 1000000);
  }

  return(ts);
}

// Read a numeric-or-string field as double — same pattern as
// gem_json_num in gemini_orders.c, redeclared here to avoid a cross-TU
// helper dependency.
static double
gem_ws_json_num(struct json_object *obj, const char *key)
{
  struct json_object *v;
  const char         *str;

  if(obj == NULL || !json_object_object_get_ex(obj, key, &v))
    return(0.0);

  if(json_object_is_type(v, json_type_string))
  {
    str = json_object_get_string(v);
    return(str != NULL ? strtod(str, NULL) : 0.0);
  }

  return(json_object_get_double(v));
}

static int64_t
gem_ws_json_int64(struct json_object *obj, const char *key)
{
  struct json_object *v;
  const char         *str;

  if(obj == NULL || !json_object_object_get_ex(obj, key, &v))
    return(0);

  if(json_object_is_type(v, json_type_string))
  {
    str = json_object_get_string(v);
    return(str != NULL ? (int64_t)strtoll(str, NULL, 10) : 0);
  }

  return((int64_t)json_object_get_int64(v));
}

// Emit one TICKER event derived from the latest top-of-book in the
// l2_updates payload. `changes` is the diff array — we sweep it to
// compute the best bid + best ask + last-known price.
static void
gem_ws_md_emit_ticker_from_l2(const char *product_abstr,
    struct json_object *changes, int64_t time_ms)
{
  exchange_ws_event_t   ev = {0};
  exchange_ws_ticker_t *t  = &ev.payload.ticker;
  size_t                i;
  size_t                n;
  double                best_bid = 0.0;
  double                best_ask = 0.0;
  double                last     = 0.0;

  if(changes == NULL) return;

  n = (size_t)json_object_array_length(changes);

  for(i = 0; i < n; i++)
  {
    struct json_object *row = json_object_array_get_idx(changes, (int)i);
    const char         *side;
    double              price;
    double              qty;

    if(row == NULL) continue;
    if(json_object_get_type(row) != json_type_array) continue;
    if(json_object_array_length(row) < 3) continue;

    {
      const char *p_str;
      const char *q_str;

      side  = json_object_get_string(json_object_array_get_idx(row, 0));
      p_str = json_object_get_string(json_object_array_get_idx(row, 1));
      q_str = json_object_get_string(json_object_array_get_idx(row, 2));

      price = (p_str != NULL) ? strtod(p_str, NULL) : 0.0;
      qty   = (q_str != NULL) ? strtod(q_str, NULL) : 0.0;
    }

    if(side == NULL || price <= 0.0) continue;

    last = price;

    // qty=0 means the level was removed; only count populated levels
    // toward best bid/ask.
    if(qty <= 0.0) continue;

    if(strcmp(side, "buy") == 0 || strcmp(side, "bid") == 0)
    {
      if(best_bid == 0.0 || price > best_bid)
        best_bid = price;
    }
    else if(strcmp(side, "sell") == 0 || strcmp(side, "ask") == 0)
    {
      if(best_ask == 0.0 || price < best_ask)
        best_ask = price;
    }
  }

  if(best_bid == 0.0 && best_ask == 0.0 && last == 0.0)
    return;

  ev.channel = EXCH_WS_TICKER;
  snprintf(ev.product_id, sizeof(ev.product_id), "%s", product_abstr);
  snprintf(t->product_id, sizeof(t->product_id), "%s", product_abstr);

  t->best_bid = best_bid;
  t->best_ask = best_ask;
  t->price    = last;
  // 24h aggregates left at 0.0 — Gemini's MD v2 doesn't surface them.
  t->volume_24h = 0.0;
  t->low_24h    = 0.0;
  t->high_24h   = 0.0;
  t->time_ms    = time_ms;

  gem_ws_fanout_locked(&ev);
}

// Emit TRADES events from the inline `trades` array carried inside an
// l2_updates envelope. Each trade entry is an object:
//   {"event_id":<int>,"timestamp":<ms>,"price":"...","quantity":"...",
//    "side":"buy"}
static void
gem_ws_md_emit_trades_array(const char *product_abstr,
    struct json_object *trades, int64_t fallback_time_ms)
{
  size_t i;
  size_t n;

  if(trades == NULL) return;

  n = (size_t)json_object_array_length(trades);

  for(i = 0; i < n; i++)
  {
    exchange_ws_event_t  ev  = {0};
    exchange_ws_match_t *m   = &ev.payload.match;
    struct json_object  *row = json_object_array_get_idx(trades, (int)i);
    char                 side[EXCHANGE_SIDE_SZ] = {0};

    if(row == NULL) continue;
    if(json_object_get_type(row) != json_type_object) continue;

    ev.channel = EXCH_WS_TRADES;
    snprintf(ev.product_id, sizeof(ev.product_id), "%s", product_abstr);
    snprintf(m->product_id, sizeof(m->product_id), "%s", product_abstr);

    if(json_get_str(row, "side", side, sizeof(side)))
    {
      gem_str_lower(side);
      snprintf(m->side, sizeof(m->side), "%s", side);
    }

    m->price    = gem_ws_json_num(row, "price");
    m->size     = gem_ws_json_num(row, "quantity");
    m->trade_id = gem_ws_json_int64(row, "event_id");
    m->time_ms  = gem_ws_json_int64(row, "timestamp");

    if(m->time_ms == 0) m->time_ms = fallback_time_ms;

    gem_ws_fanout_locked(&ev);
  }
}

static void
gem_ws_md_handle_l2_updates_locked(struct json_object *root)
{
  char                 symbol_native[16] = {0};
  char                 symbol_abstr[EXCHANGE_PRODUCT_ID_SZ] = {0};
  struct json_object  *changes;
  struct json_object  *trades;
  int64_t              time_ms;

  if(!json_get_str(root, "symbol", symbol_native, sizeof(symbol_native)))
    return;

  gem_ws_symbol_to_abstr(symbol_native, symbol_abstr,
      sizeof(symbol_abstr));

  if(symbol_abstr[0] == '\0')
  {
    clam(CLAM_DEBUG, GEM_CTX ".ws.md",
        "l2_updates: cache miss for native='%s'", symbol_native);
    return;
  }

  time_ms = gem_ws_now_envelope_ms(root);

  changes = json_get_array(root, "changes");
  trades  = json_get_array(root, "trades");

  if(changes != NULL)
    gem_ws_md_emit_ticker_from_l2(symbol_abstr, changes, time_ms);

  if(trades != NULL)
    gem_ws_md_emit_trades_array(symbol_abstr, trades, time_ms);
}

// Standalone `trade` envelope:
//   {"type":"trade","symbol":"BTCUSD","event_id":..,"timestamp":..,
//    "price":"...","quantity":"...","side":"buy"}
static void
gem_ws_md_handle_trade_locked(struct json_object *root)
{
  exchange_ws_event_t  ev  = {0};
  exchange_ws_match_t *m   = &ev.payload.match;
  char                 symbol_native[16] = {0};
  char                 symbol_abstr[EXCHANGE_PRODUCT_ID_SZ] = {0};
  char                 side[EXCHANGE_SIDE_SZ] = {0};
  int64_t              time_ms;

  if(!json_get_str(root, "symbol", symbol_native, sizeof(symbol_native)))
    return;

  gem_ws_symbol_to_abstr(symbol_native, symbol_abstr,
      sizeof(symbol_abstr));

  if(symbol_abstr[0] == '\0')
  {
    clam(CLAM_DEBUG, GEM_CTX ".ws.md",
        "trade: cache miss for native='%s'", symbol_native);
    return;
  }

  time_ms = gem_ws_now_envelope_ms(root);

  ev.channel = EXCH_WS_TRADES;
  snprintf(ev.product_id, sizeof(ev.product_id), "%s", symbol_abstr);
  snprintf(m->product_id, sizeof(m->product_id), "%s", symbol_abstr);

  if(json_get_str(root, "side", side, sizeof(side)))
  {
    gem_str_lower(side);
    snprintf(m->side, sizeof(m->side), "%s", side);
  }

  m->price    = gem_ws_json_num(root, "price");
  m->size     = gem_ws_json_num(root, "quantity");
  m->trade_id = gem_ws_json_int64(root, "event_id");
  m->time_ms  = gem_ws_json_int64(root, "timestamp");

  if(m->time_ms == 0) m->time_ms = time_ms;

  gem_ws_fanout_locked(&ev);
}

// `candles_1m_updates` envelope:
//   {"type":"candles_1m_updates","symbol":"BTCUSD",
//    "changes":[[ts_ms,open,high,low,close,volume], ...]}
//
// Each change row is a 6-element JSON array. We emit one OHLC event
// per row — the aggregator dedups by ts_open_ms so streaming updates
// of the active bar don't double-count.
static void
gem_ws_md_handle_candles_1m_locked(struct json_object *root)
{
  char                 symbol_native[16] = {0};
  char                 symbol_abstr[EXCHANGE_PRODUCT_ID_SZ] = {0};
  struct json_object  *changes;
  int64_t              time_ms;
  size_t               i;
  size_t               n;

  if(!json_get_str(root, "symbol", symbol_native, sizeof(symbol_native)))
    return;

  gem_ws_symbol_to_abstr(symbol_native, symbol_abstr,
      sizeof(symbol_abstr));

  if(symbol_abstr[0] == '\0')
  {
    clam(CLAM_DEBUG, GEM_CTX ".ws.md",
        "candles_1m: cache miss for native='%s'", symbol_native);
    return;
  }

  changes = json_get_array(root, "changes");
  if(changes == NULL) return;

  time_ms = gem_ws_now_envelope_ms(root);

  n = (size_t)json_object_array_length(changes);

  for(i = 0; i < n; i++)
  {
    exchange_ws_event_t  ev  = {0};
    exchange_ws_ohlc_t  *o   = &ev.payload.ohlc;
    struct json_object  *row = json_object_array_get_idx(changes, (int)i);
    struct json_object  *v;

    if(row == NULL) continue;
    if(json_object_get_type(row) != json_type_array) continue;
    if(json_object_array_length(row) < 6) continue;

    ev.channel = EXCH_WS_OHLC_1M;
    snprintf(ev.product_id, sizeof(ev.product_id), "%s", symbol_abstr);
    snprintf(o->product_id, sizeof(o->product_id), "%s", symbol_abstr);

    v = json_object_array_get_idx(row, 0);
    o->ts_open_ms = (v != NULL) ? (int64_t)json_object_get_int64(v) : 0;

    v = json_object_array_get_idx(row, 1);
    o->open = (v != NULL) ? json_object_get_double(v) : 0.0;

    v = json_object_array_get_idx(row, 2);
    o->high = (v != NULL) ? json_object_get_double(v) : 0.0;

    v = json_object_array_get_idx(row, 3);
    o->low = (v != NULL) ? json_object_get_double(v) : 0.0;

    v = json_object_array_get_idx(row, 4);
    o->close = (v != NULL) ? json_object_get_double(v) : 0.0;

    v = json_object_array_get_idx(row, 5);
    o->volume = (v != NULL) ? json_object_get_double(v) : 0.0;

    o->interval_min = 1;
    o->time_ms      = time_ms;

    gem_ws_fanout_locked(&ev);
  }
}

// MD subscription_ack envelope. Body shape:
//   {"type":"subscription_ack",
//    "subscriptions":[{"name":"l2","symbols":["BTCUSD","ETHUSD"]}, ...]}
//
// Correlate by (channel, symbol) since Gemini's MD v2 does not echo
// JSON-RPC ids. Mark every matching slot ACTIVE + sent_upstream=true.
static void
gem_ws_md_handle_sub_ack_locked(struct json_object *root)
{
  struct json_object *subs;
  size_t              n;
  size_t              i;

  subs = json_get_array(root, "subscriptions");
  if(subs == NULL) return;

  n = (size_t)json_object_array_length(subs);

  for(i = 0; i < n; i++)
  {
    struct json_object  *sub = json_object_array_get_idx(subs, (int)i);
    struct json_object  *syms_arr;
    char                 name[24] = {0};
    gem_ws_channel_t     ch;
    size_t               j;
    size_t               m;

    if(sub == NULL) continue;
    if(!json_get_str(sub, "name", name, sizeof(name))) continue;

    if(strcmp(name, "l2") == 0)               ch = GEM_CH_TICKER;
    else if(strcmp(name, "trades") == 0)      ch = GEM_CH_TRADES;
    else if(strcmp(name, "candles_1m") == 0)  ch = GEM_CH_OHLC_1M;
    else
    {
      clam(CLAM_DEBUG, GEM_CTX ".ws.md",
          "subscription_ack: unknown channel name='%s'", name);
      continue;
    }

    syms_arr = json_get_array(sub, "symbols");
    if(syms_arr == NULL) continue;

    m = (size_t)json_object_array_length(syms_arr);

    for(j = 0; j < m; j++)
    {
      struct json_object *v = json_object_array_get_idx(syms_arr, (int)j);
      const char         *sym;
      int32_t             idx;

      if(v == NULL) continue;

      sym = json_object_get_string(v);
      if(sym == NULL || sym[0] == '\0') continue;

      idx = gem_ws_slot_find_locked(ch, sym);
      if(idx < 0) continue;

      gem_ws_ch.slots[idx].state         = GEM_SUB_ACTIVE;
      gem_ws_ch.slots[idx].sent_upstream = true;
      gem_ws_ch.slots[idx].last_err[0]   = '\0';

      clam(CLAM_INFO, GEM_CTX ".ws.md",
          "subscription_ack ch=%s sym=%s", name, sym);
    }
  }
}

// ------------------------------------------------------------------ //
// OE: Order Events parser                                             //
// ------------------------------------------------------------------ //

// Helpers for surfacing the type/status fields. Gemini uses
// "exchange limit"/"exchange market" on the order envelope; the
// abstraction expects compact tokens. The collapse helper is hoisted
// to gemini.h so REST + WS share it.
static void
gem_oe_emit_order(struct json_object *frame, const char *status,
    int64_t envelope_time_ms)
{
  exchange_ws_event_t       ev   = {0};
  exchange_ws_user_event_t *up   = &ev.payload.user;
  exchange_ws_user_order_t *o    = &up->u.order;
  char                      symbol_native[16] = {0};
  char                      symbol_abstr[EXCHANGE_PRODUCT_ID_SZ] = {0};
  char                      side[EXCHANGE_SIDE_SZ] = {0};
  int64_t                   ts;

  if(frame == NULL) return;

  json_get_str(frame, "symbol", symbol_native, sizeof(symbol_native));

  if(symbol_native[0] != '\0')
  {
    gem_ws_symbol_to_abstr(symbol_native, symbol_abstr,
        sizeof(symbol_abstr));
    if(symbol_abstr[0] == '\0')
    {
      // Cache miss — fall back to native form so consumers at least
      // see the order, even though the product won't match a
      // canonical id. Mark as absent so fanout takes the global path.
      snprintf(symbol_abstr, sizeof(symbol_abstr), "%s", symbol_native);
    }
  }

  ev.channel = EXCH_WS_USER;
  snprintf(ev.product_id, sizeof(ev.product_id), "%s", symbol_abstr);

  up->kind = EXCH_WS_USER_KIND_ORDER;

  json_get_str(frame, "order_id",        o->order_id,        sizeof(o->order_id));
  json_get_str(frame, "client_order_id", o->client_order_id, sizeof(o->client_order_id));
  snprintf(o->product_id, sizeof(o->product_id), "%s", symbol_abstr);

  if(json_get_str(frame, "side", side, sizeof(side)))
  {
    gem_str_lower(side);
    snprintf(o->side, sizeof(o->side), "%s", side);
  }

  // The caller-supplied `status` is the abstraction-level token
  // ("OPEN"/"FILLED"/"CANCELLED"/"FAILED"). Trust it.
  if(status != NULL && status[0] != '\0')
    snprintf(o->status, sizeof(o->status), "%s", status);

  // exchange_ws_user_order_t carries no `type` field today; Gemini's
  // verbose "exchange limit"/"exchange market" wire form would need
  // `gem_type_to_generic` (hoisted in gemini.h) when an ABI bump
  // surfaces the type at the abstraction seam. REST adapter callers
  // already exercise that collapse on the order_t path.

  o->limit_price         = gem_ws_json_num(frame, "price");
  o->cumulative_quantity = gem_ws_json_num(frame, "executed_amount");
  o->leaves_quantity     = gem_ws_json_num(frame, "remaining_amount");
  o->avg_price           = gem_ws_json_num(frame, "avg_execution_price");
  o->total_fees          = 0.0;   // not present on order envelopes

  ts = gem_ws_json_int64(frame, "timestampms");
  if(ts == 0) ts = gem_ws_json_int64(frame, "timestamp_ms");
  if(ts == 0) ts = envelope_time_ms;

  o->time_ms          = ts;
  o->creation_time_ms = ts;

  gem_ws_fanout_locked(&ev);
}

static void
gem_oe_emit_fill(struct json_object *frame, int64_t envelope_time_ms)
{
  exchange_ws_event_t       ev   = {0};
  exchange_ws_user_event_t *up   = &ev.payload.user;
  exchange_ws_user_fill_t  *f    = &up->u.fill;
  struct json_object       *fill_obj;
  char                      symbol_native[16] = {0};
  char                      symbol_abstr[EXCHANGE_PRODUCT_ID_SZ] = {0};
  char                      side[EXCHANGE_SIDE_SZ] = {0};
  int64_t                   ts;
  bool                      is_live   = true;
  double                    remaining = 0.0;
  bool                      have_remaining;

  if(frame == NULL) return;

  json_get_str(frame, "symbol", symbol_native, sizeof(symbol_native));

  if(symbol_native[0] != '\0')
  {
    gem_ws_symbol_to_abstr(symbol_native, symbol_abstr,
        sizeof(symbol_abstr));
    if(symbol_abstr[0] == '\0')
      snprintf(symbol_abstr, sizeof(symbol_abstr), "%s", symbol_native);
  }

  ev.channel = EXCH_WS_USER;
  snprintf(ev.product_id, sizeof(ev.product_id), "%s", symbol_abstr);

  up->kind = EXCH_WS_USER_KIND_FILL;

  json_get_str(frame, "order_id",        f->order_id,        sizeof(f->order_id));
  json_get_str(frame, "client_order_id", f->client_order_id, sizeof(f->client_order_id));
  snprintf(f->product_id, sizeof(f->product_id), "%s", symbol_abstr);

  if(json_get_str(frame, "side", side, sizeof(side)))
  {
    gem_str_lower(side);
    snprintf(f->side, sizeof(f->side), "%s", side);
  }

  // Gemini's fill envelope nests trade details under "fill":
  //   {"fill":{"price":"...","amount":"...","fee":"...","trade_id":...}}
  // Fall back to the envelope root if "fill" isn't present (older
  // gateway shape).
  fill_obj = json_get_obj(frame, "fill");

  if(fill_obj != NULL)
  {
    f->trade_id = gem_ws_json_int64(fill_obj, "trade_id");
    if(f->trade_id == 0)
      f->trade_id = gem_ws_json_int64(fill_obj, "tid");

    f->price = gem_ws_json_num(fill_obj, "price");
    f->size  = gem_ws_json_num(fill_obj, "amount");
    f->fee   = gem_ws_json_num(fill_obj, "fee");
  }
  else
  {
    f->trade_id = gem_ws_json_int64(frame, "trade_id");
    if(f->trade_id == 0)
      f->trade_id = gem_ws_json_int64(frame, "tid");

    f->price = gem_ws_json_num(frame, "price");
    f->size  = gem_ws_json_num(frame, "amount");
    f->fee   = gem_ws_json_num(frame, "fee_amount");
  }

  ts = gem_ws_json_int64(frame, "timestampms");
  if(ts == 0) ts = gem_ws_json_int64(frame, "timestamp_ms");
  if(ts == 0) ts = envelope_time_ms;

  f->time_ms = ts;

  gem_ws_fanout_locked(&ev);

  // Surface a follow-up ORDER state transition when the fill envelope
  // carries `is_live` + `remaining_amount`, so consumers see the
  // resulting order state. Gemini's docs document both fields on the
  // fill envelope; treat absence as "no transition surfaced".
  have_remaining = json_object_object_get_ex(frame, "remaining_amount", NULL);
  remaining      = gem_ws_json_num(frame, "remaining_amount");
  (void)json_get_bool(frame, "is_live", &is_live);

  if(have_remaining)
  {
    const char *follow_status = is_live ? "OPEN" : "FILLED";

    // If remaining_amount > 0, prefer OPEN; otherwise FILLED. Trust
    // is_live for the truly-closed-without-remaining edge case
    // (e.g. immediate-or-cancel partial).
    if(remaining > 0.0)
      follow_status = "OPEN";
    else if(!is_live)
      follow_status = "FILLED";

    gem_oe_emit_order(frame, follow_status, envelope_time_ms);
  }
}

// Walk one envelope object (already known to have a "type" key).
static void
gem_oe_parse_frame_locked(struct json_object *frame, int64_t envelope_time_ms)
{
  char type[32] = {0};

  if(frame == NULL) return;
  if(!json_get_str(frame, "type", type, sizeof(type))) return;

  if(strcmp(type, "heartbeat") == 0)
    return;   // keep-alive; transport layer counts as liveness

  if(strcmp(type, "subscription_ack") == 0)
  {
    clam(CLAM_DEBUG, GEM_CTX ".ws.oe", "subscription_ack");
    return;
  }

  if(strcmp(type, "initial")   == 0
      || strcmp(type, "accepted") == 0
      || strcmp(type, "booked")   == 0)
  {
    gem_oe_emit_order(frame, "OPEN", envelope_time_ms);
    return;
  }

  if(strcmp(type, "fill") == 0)
  {
    gem_oe_emit_fill(frame, envelope_time_ms);
    return;
  }

  if(strcmp(type, "cancelled") == 0)
  {
    gem_oe_emit_order(frame, "CANCELLED", envelope_time_ms);
    return;
  }

  if(strcmp(type, "rejected") == 0)
  {
    gem_oe_emit_order(frame, "FAILED", envelope_time_ms);
    return;
  }

  if(strcmp(type, "closed") == 0)
  {
    gem_oe_emit_order(frame, "FILLED", envelope_time_ms);
    return;
  }

  if(strcmp(type, "cancel_rejected") == 0)
  {
    char order_id[EXCHANGE_ORDER_ID_SZ] = {0};
    json_get_str(frame, "order_id", order_id, sizeof(order_id));
    clam(CLAM_INFO, GEM_CTX ".ws.oe",
        "cancel_rejected order_id=%s", order_id);
    return;
  }

  clam(CLAM_DEBUG3, GEM_CTX ".ws.oe",
      "ignoring envelope type='%s'", type);
}

// ------------------------------------------------------------------ //
// Public dispatch                                                     //
// ------------------------------------------------------------------ //

void
gem_ws_channels_dispatch_md(const char *buf, size_t len)
{
  struct json_object *root;
  char                type[24] = {0};

  if(!gem_ws_ch.initialized || buf == NULL || len == 0) return;

  root = json_parse_buf(buf, len, GEM_CTX ".ws.md");
  if(root == NULL) return;

  if(!json_get_str(root, "type", type, sizeof(type)))
  {
    // Some MD envelopes do not carry a "type" key (rare; e.g. server-
    // side error frames). Log at debug and drop.
    clam(CLAM_DEBUG3, GEM_CTX ".ws.md",
        "frame without type (%zu bytes)", len);
    json_object_put(root);
    return;
  }

  pthread_mutex_lock(&gem_ws_ch.mu);

  if(strcmp(type, "heartbeat") == 0)
  {
    // No-op; transport layer counts as liveness.
  }
  else if(strcmp(type, "subscription_ack") == 0)
  {
    gem_ws_md_handle_sub_ack_locked(root);
  }
  else if(strcmp(type, "l2_updates") == 0)
  {
    gem_ws_md_handle_l2_updates_locked(root);
  }
  else if(strcmp(type, "trade") == 0)
  {
    gem_ws_md_handle_trade_locked(root);
  }
  else if(strcmp(type, "candles_1m_updates") == 0)
  {
    gem_ws_md_handle_candles_1m_locked(root);
  }
  else
  {
    clam(CLAM_DEBUG3, GEM_CTX ".ws.md", "ignoring type='%s'", type);
  }

  pthread_mutex_unlock(&gem_ws_ch.mu);

  json_object_put(root);
}

void
gem_ws_channels_dispatch_oe(const char *buf, size_t len)
{
  struct json_object *root;
  int64_t             envelope_time_ms;

  if(!gem_ws_ch.initialized || buf == NULL || len == 0) return;

  root = json_parse_buf(buf, len, GEM_CTX ".ws.oe");
  if(root == NULL) return;

  envelope_time_ms = gem_ws_now_envelope_ms(root);

  pthread_mutex_lock(&gem_ws_ch.mu);

  if(json_object_get_type(root) == json_type_array)
  {
    // Array root: walk each envelope. Gemini sends the initial
    // snapshot of all open orders as an array of `initial` entries.
    size_t n = (size_t)json_object_array_length(root);
    size_t i;

    for(i = 0; i < n; i++)
    {
      struct json_object *frame = json_object_array_get_idx(root, (int)i);

      if(frame == NULL) continue;
      if(json_object_get_type(frame) != json_type_object) continue;

      gem_oe_parse_frame_locked(frame, envelope_time_ms);
    }
  }
  else if(json_object_get_type(root) == json_type_object)
  {
    gem_oe_parse_frame_locked(root, envelope_time_ms);
  }

  pthread_mutex_unlock(&gem_ws_ch.mu);

  json_object_put(root);
}

// ------------------------------------------------------------------ //
// On-open hook                                                        //
// ------------------------------------------------------------------ //

void
gem_ws_channels_on_open(gem_ws_session_id_t sid)
{
  if(!gem_ws_ch.initialized) return;

  switch(sid)
  {
    case GEM_WS_OE:
      clam(CLAM_INFO, GEM_CTX ".ws.oe",
          "open (order-events stream)");
      return;

    case GEM_WS_MD:
      break;
  }

  pthread_mutex_lock(&gem_ws_ch.mu);

  if(gem_ws_ch.n_slots == 0)
  {
    pthread_mutex_unlock(&gem_ws_ch.mu);
    return;
  }

  // Reset state on every slot — the gateway forgot us across the flap.
  for(uint32_t i = 0; i < gem_ws_ch.n_slots; i++)
  {
    gem_ws_ch.slots[i].sent_upstream = false;

    if(gem_ws_ch.slots[i].refcount > 0)
      gem_ws_ch.slots[i].state = GEM_SUB_IDLE;
  }

  gem_ws_emit_resubscribe_locked();

  pthread_mutex_unlock(&gem_ws_ch.mu);
}

// ------------------------------------------------------------------ //
// Channel mapping helpers (public API translation)                    //
// ------------------------------------------------------------------ //

static gem_ws_channel_t
gem_ws_channel_from_exch(exchange_ws_channel_t in)
{
  switch(in)
  {
    case EXCH_WS_TICKER:  return(GEM_CH_TICKER);
    case EXCH_WS_TRADES:  return(GEM_CH_TRADES);
    case EXCH_WS_OHLC_1M: return(GEM_CH_OHLC_1M);
    case EXCH_WS_USER:    return(GEM_CH_USER);
    case EXCH_WS_BOOK_L2: return(GEM_CH__COUNT);
  }
  return(GEM_CH__COUNT);
}

// ------------------------------------------------------------------ //
// Public API: gem_ws_subscribe / gem_ws_unsubscribe                   //
// ------------------------------------------------------------------ //

bool
gem_ws_subscribe(const exchange_ws_channel_t *channels, uint32_t n_channels,
    const char *const *product_ids, uint32_t n_products,
    exchange_ws_event_cb_t cb, void *user, exchange_ws_sub_t **out_handle)
{
  struct exchange_ws_sub *sub;
  uint32_t                channel_mask     = 0;
  bool                    has_user_channel = false;
  bool                    has_per_symbol   = false;
  uint32_t                i;

  if(out_handle == NULL)
    return(FAIL);

  *out_handle = NULL;

  if(!gem_ws_ch.initialized)
  {
    clam(CLAM_WARN, GEM_CTX ".ws", "subscribe: multiplexer not initialized");
    return(FAIL);
  }

  if(cb == NULL || channels == NULL || n_channels == 0)
  {
    clam(CLAM_WARN, GEM_CTX ".ws", "subscribe: invalid args");
    return(FAIL);
  }

  if(n_products > GEM_WS_CH_MAX_PRODUCTS_PER_SUB)
  {
    clam(CLAM_WARN, GEM_CTX ".ws",
        "subscribe: too many products (%u > %d)",
        n_products, GEM_WS_CH_MAX_PRODUCTS_PER_SUB);
    return(FAIL);
  }

  // Validate channels + classify.
  for(i = 0; i < n_channels; i++)
  {
    gem_ws_channel_t mapped = gem_ws_channel_from_exch(channels[i]);

    if(mapped == GEM_CH__COUNT)
    {
      clam(CLAM_WARN, GEM_CTX ".ws",
          "subscribe: channel %d not supported on Gemini",
          (int)channels[i]);
      return(FAIL);
    }

    channel_mask |= (1u << channels[i]);

    if(mapped == GEM_CH_USER)
      has_user_channel = true;
    else
      has_per_symbol = true;
  }

  if(has_per_symbol && n_products == 0)
  {
    clam(CLAM_WARN, GEM_CTX ".ws",
        "subscribe: per-symbol channel without product_ids");
    return(FAIL);
  }

  if(has_user_channel && !gem_apikey_configured())
  {
    clam(CLAM_WARN, GEM_CTX ".ws",
        "subscribe: USER channel requested but credentials not configured");
    return(FAIL);
  }

  pthread_mutex_lock(&gem_ws_ch.mu);

  if(gem_ws_ch.n_subs >= GEM_WS_CH_MAX_SUBS)
  {
    pthread_mutex_unlock(&gem_ws_ch.mu);
    clam(CLAM_WARN, GEM_CTX ".ws", "subscribe: sub table full (%u)",
        gem_ws_ch.n_subs);
    return(FAIL);
  }

  sub = mem_alloc(GEM_CTX ".ws", "sub", sizeof(*sub));

  if(sub == NULL)
  {
    pthread_mutex_unlock(&gem_ws_ch.mu);
    return(FAIL);
  }

  memset(sub, 0, sizeof(*sub));
  sub->id           = ++gem_ws_ch.next_id;
  sub->cb           = cb;
  sub->user         = user;
  sub->channel_mask = channel_mask;
  sub->n_products   = n_products;

  // Store products in abstraction form so fanout matches byte-for-byte
  // against ev->product_id (which the parsers populate via
  // gem_pair_to_abstr).
  for(i = 0; i < n_products; i++)
  {
    char abstr[EXCHANGE_PRODUCT_ID_SZ] = {0};

    if(product_ids[i] == NULL || product_ids[i][0] == '\0')
    {
      pthread_mutex_unlock(&gem_ws_ch.mu);
      mem_free(sub);
      clam(CLAM_WARN, GEM_CTX ".ws",
          "subscribe: empty product_id at idx %u", i);
      return(FAIL);
    }

    gem_ws_symbol_to_abstr(product_ids[i], abstr, sizeof(abstr));

    if(abstr[0] == '\0')
    {
      // Cache miss — fall back to the caller's input. Fanout still
      // works as long as the producer side ends up with the same
      // string (which happens when the symbols cache primes and
      // serves both ends consistently).
      snprintf(sub->products[i], sizeof(sub->products[i]), "%s",
          product_ids[i]);
    }
    else
      snprintf(sub->products[i], sizeof(sub->products[i]), "%s", abstr);
  }

  sub->next      = gem_ws_ch.head;
  gem_ws_ch.head = sub;
  gem_ws_ch.n_subs++;

  // Refcount every (channel, native_symbol) pair this sub covers.
  // USER channels do NOT take a slot in the MD slot table — they
  // ride the OE session implicitly; the sub is recorded so fanout
  // can deliver order/fill events to it. We still touch n_products
  // for USER subs (per-product filtering applies on the fanout
  // side; an empty products[] list = account-wide match).
  for(i = 0; i < n_channels; i++)
  {
    gem_ws_channel_t mapped = gem_ws_channel_from_exch(channels[i]);

    if(mapped == GEM_CH_USER) continue;   // OE-implicit; no slot

    {
      uint32_t p;

      for(p = 0; p < n_products; p++)
      {
        char    sym_native[16] = {0};
        int32_t idx;

        gem_ws_symbol_to_native(sub->products[p], sym_native,
            sizeof(sym_native));

        if(sym_native[0] == '\0')
        {
          clam(CLAM_WARN, GEM_CTX ".ws",
              "subscribe: cannot resolve native symbol for '%s'",
              sub->products[p]);
          continue;
        }

        idx = gem_ws_slot_find_locked(mapped, sym_native);

        if(idx < 0)
        {
          gem_ws_slot_t *sl = gem_ws_slot_alloc_locked(mapped, sym_native);

          if(sl == NULL)
          {
            clam(CLAM_WARN, GEM_CTX ".ws",
                "subscribe: slot table full, skipping %s/%s",
                gem_ws_md_channel_name(mapped), sym_native);
            continue;
          }
          idx = (int32_t)(sl - gem_ws_ch.slots);
        }

        gem_ws_ch.slots[idx].refcount++;
      }
    }
  }

  // Reconcile the upstream MD subscription set. The MD session may
  // not be OPEN — gem_ws_send_text returns FAIL silently and the
  // on-open hook re-emits at the next OPEN transition.
  gem_ws_emit_resubscribe_locked();

  pthread_mutex_unlock(&gem_ws_ch.mu);

  *out_handle = sub;

  clam(CLAM_INFO, GEM_CTX ".ws",
      "subscribe id=%u channels=0x%04x products=%u",
      sub->id, channel_mask, n_products);

  return(SUCCESS);
}

void
gem_ws_unsubscribe(exchange_ws_sub_t *handle)
{
  struct exchange_ws_sub **pp;
  uint32_t                  sub_id;
  uint32_t                  i;

  if(handle == NULL || !gem_ws_ch.initialized) return;

  pthread_mutex_lock(&gem_ws_ch.mu);

  // Unlink from the global list.
  for(pp = &gem_ws_ch.head; *pp != NULL; pp = &(*pp)->next)
  {
    if(*pp == handle)
    {
      *pp = handle->next;
      gem_ws_ch.n_subs--;
      break;
    }
  }

  sub_id = handle->id;

  // Decrement refcounts on every slot this sub held. USER channels
  // never took a slot, so they're skipped here too.
  for(int ach = EXCH_WS_TICKER; ach <= EXCH_WS_USER; ach++)
  {
    gem_ws_channel_t mapped;

    if(!(handle->channel_mask & (1u << ach))) continue;

    mapped = gem_ws_channel_from_exch((exchange_ws_channel_t)ach);

    if(mapped == GEM_CH__COUNT)   continue;
    if(mapped == GEM_CH_USER)     continue;

    for(i = 0; i < handle->n_products; i++)
    {
      char    sym_native[16] = {0};
      int32_t idx;

      gem_ws_symbol_to_native(handle->products[i], sym_native,
          sizeof(sym_native));

      if(sym_native[0] == '\0') continue;

      idx = gem_ws_slot_find_locked(mapped, sym_native);

      if(idx >= 0 && gem_ws_ch.slots[idx].refcount > 0)
        gem_ws_ch.slots[idx].refcount--;
    }
  }

  // Emit unsubscribe frames for slots that hit zero. The MD session
  // may not be OPEN — silent FAIL, which is fine: the gateway forgets
  // the subscription when the slot drops out of the table.
  gem_ws_emit_unsubscribe_locked();

  // Reap empty slots.
  gem_ws_slots_compact_locked();

  mem_free(handle);

  pthread_mutex_unlock(&gem_ws_ch.mu);

  clam(CLAM_INFO, GEM_CTX ".ws", "unsubscribe id=%u", sub_id);
}

// ------------------------------------------------------------------ //
// Lifecycle                                                           //
// ------------------------------------------------------------------ //

void
gem_ws_channels_init(void)
{
  if(gem_ws_ch.initialized) return;

  memset(&gem_ws_ch, 0, sizeof(gem_ws_ch));
  pthread_mutex_init(&gem_ws_ch.mu, NULL);
  atomic_store(&gem_ws_ch.next_req_id, 0u);

  gem_ws_ch.initialized = true;

  clam(CLAM_DEBUG, GEM_CTX, "ws channel multiplexer initialized");
}

void
gem_ws_channels_deinit(void)
{
  struct exchange_ws_sub *s;
  struct exchange_ws_sub *n;

  if(!gem_ws_ch.initialized) return;

  pthread_mutex_lock(&gem_ws_ch.mu);

  s = gem_ws_ch.head;
  while(s != NULL)
  {
    n = s->next;
    mem_free(s);
    s = n;
  }
  gem_ws_ch.head    = NULL;
  gem_ws_ch.n_subs  = 0;
  gem_ws_ch.n_slots = 0;

  pthread_mutex_unlock(&gem_ws_ch.mu);

  pthread_mutex_destroy(&gem_ws_ch.mu);
  gem_ws_ch.initialized = false;

  clam(CLAM_DEBUG, GEM_CTX, "ws channel multiplexer torn down");
}
