// botmanager — MIT
// Gemini WebSocket channel multiplexer.
//
// Layered on top of gemini_ws.c's two-session transport. Owns:
//   * the local subscriber list (one per `exchange_ws_subscribe` call),
//   * a per-(channel, native_sym) slot table that refcounts shared
//     subscribers so N consumers watching the same feed share one
//     upstream subscription,
//   * a monotonic req_id counter (informational only — Gemini Market
//     Data v2 echoes nothing at all: no req_ids, and no acknowledgement
//     of any kind. There is nothing on the wire to correlate against),
//   * per-channel parsers turning one inbound MD/OE frame into one or
//     more fanned-out exchange_ws_event_t events.
//
// Wire format is Gemini's Market Data v2 + Order Events stream:
//
//   MD subscribe (caller-driven; sent on GEM_WS_MD at OPEN):
//     {"type":"subscribe",
//      "subscriptions":[
//        {"name":"l2",         "symbols":["BTCUSD","ETHUSD"]},
//        {"name":"candles_1m", "symbols":["BTCUSD"]}
//      ]}
//
//   ⛔ Those are the ONLY two channels this gateway has (OBS-55). A
//   third line naming `trades` used to be rendered here and the whole
//   frame was answered {"reason":"InvalidJson","result":"error"};
//   `trade` singular is refused identically. Trade prints arrive
//   inside the `l2` stream.
//
//   MD frames:
//     heartbeat        — {"type":"heartbeat",...}                      no-op
//     l2_updates       — initial snapshot + ongoing diff updates;
//                         carries inline `trades` arrays
//     trade            — individual trade prints
//     candles_1m_updates — initial/streaming 1-minute bars
//     {"result":"error","reason":...} — the gateway REFUSING a frame we
//                         sent. Typeless, so it is matched before the
//                         type test and logged at WARN (OBS-55).
//     ⛔ NO subscription_ack: this gateway confirms nothing (OBS-53).
//
//   OE subscribe — none. Account-implicit. HMAC handshake auths.
//
//   OE frames:
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

// Gemini's WS symbol form: the uppercase concatenated pair, `BTCUSD`.
// Named because the reap copies one out from under `mu` and the copy
// must be able to hold whatever the slot holds (OBS-42).
#define GEM_WS_SYM_NATIVE_SZ            16
#define GEM_WS_CH_REQ_RING_SIZE         GEM_REQ_ID_RING

// Internal channel enum. EXCH_WS_BOOK_L2 is not exposed, and the
// mapping from the remaining abstract channels is NOT one-to-one:
// OBS-55 measured that this gateway has no `trades` channel, so
// EXCH_WS_TICKER and EXCH_WS_TRADES both land on `l2`, which carries
// book diffs and trade prints in the same stream.
typedef enum
{
  // ⚠ These values are DENSE and must stay dense: every per-channel
  // pass is `for(c = 0; c < GEM_CH__COUNT; c++)`, so a hole is a
  // channel that does not exist being rendered and marked per-symbol.
  // OBS-55 removed `GEM_CH_TRADES = 1` (this gateway has no trades
  // channel) and renumbered rather than leaving the gap behind.
  // Nothing persists or transmits these — the slot table is in-memory
  // and the subscription mask is keyed by the ABSTRACT
  // exchange_ws_channel_t — so renumbering is free.
  GEM_CH_TICKER      = 0,    // `l2`; also carries trade + book events
  GEM_CH_OHLC_1M     = 1,    // `candles_1m` channel
  GEM_CH_USER        = 2,    // Order Events stream (account-implicit)
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

typedef struct gem_ws_sub
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

  struct gem_ws_sub      *next;
} gem_ws_sub_t;

typedef enum
{
  GEM_SUB_IDLE,
  GEM_SUB_SUBSCRIBING,
  GEM_SUB_ACTIVE,
  GEM_SUB_FAILED,

  // A slot whose unsubscribe frame is already on the wire (OBS-42).
  // Both windows in which `mu` is dropped are guarded by this: a
  // concurrent reap skips it rather than emitting a second frame, and
  // compaction refuses it rather than swapping a slot out from under
  // its own completion.
  GEM_SUB_UNSUBSCRIBING
} gem_sub_state_t;

// Per-(channel, native_sym) dedup slot. `symbol_native` is the empty
// string for non-symbol-keyed channels (USER). `state` advances
// IDLE → SUBSCRIBING (frame sent) → ACTIVE (ack received) or FAILED
// (server-side rejection — surfaces in the log only).
//
// `symbol_native` is the WS form Gemini emits, which is the uppercase
// concatenated `BTCUSD` (gem_pair_to_native returns lowercase `btcusd`
// for REST; we uppercase at slot-insert time so it compares
// byte-for-byte against the symbols in incoming envelopes).
typedef struct
{
  gem_ws_channel_t channel;
  char             symbol_native[GEM_WS_SYM_NATIVE_SZ];
  uint32_t         refcount;
  gem_sub_state_t  state;
  uint32_t         req_id;        // monotonic per slot; informational

  // OBS-42 named this `wire_subscribed` and forbade it meaning "we sent
  // it", because the guard below needs "the gateway does not have it".
  // ⛔ OBS-53 measured that the distinction is UNAVAILABLE HERE:
  // wss://api.gemini.com/v2/marketdata acknowledges nothing — not a
  // subscribe, not an unsubscribe — so whether the gateway holds a
  // subscription is not observable at this venue, ever.
  //
  // So the field records what we can know and what we are on the hook
  // for: a subscribe for this slot went out on the wire, therefore an
  // unsubscribe is owed and the slot may not be forgotten. Set on a
  // successful subscribe send; cleared by a successful unsubscribe
  // send or a session flap.
  //
  // The asymmetry is what makes send-success the safe belief, and it
  // was measured, not assumed: believing the wire carries a
  // subscription it does not costs ONE redundant unsubscribe frame,
  // which this venue accepts in silence; believing it does not when it
  // does is OBS-53 itself — a feed streaming to nobody that no later
  // pass can name.
  bool             wire_subscribed;
  char             last_err[128];
} gem_ws_slot_t;

// OBS-58: one last-traded price per abstract product id. Survives a
// session flap deliberately — a reconnect does not make the last trade
// untrue, and the alternative is a resubscribe window in which every
// synthesized ticker falls back to the mid.
typedef struct
{
  char   product_abstr[EXCHANGE_PRODUCT_ID_SZ];
  double price;
} gem_ws_mark_t;

static struct
{
  pthread_mutex_t          mu;
  gem_ws_sub_t            *head;
  uint32_t                 next_id;
  uint32_t                 n_subs;

  gem_ws_slot_t            slots[GEM_WS_CH_MAX_SLOTS];
  uint32_t                 n_slots;

  // OBS-58: last traded price per abstract product. Gemini's MD v2 has
  // no ticker channel — gem_ws_md_channel_name maps EXCH_WS_TICKER onto
  // `l2` — so the ticker we publish is synthesized, and its `price` has
  // to come from somewhere. It comes from here: the trades this
  // multiplexer has already parsed, which is the same quantity coinbase
  // and kraken read straight off their venue's ticker row. Written and
  // read under `mu`, by the MD reader thread only.
  gem_ws_mark_t            marks[GEM_WS_CH_MAX_SLOTS];
  uint32_t                 n_marks;

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

// Drop slots whose refcount hit zero AND that nothing is still
// waiting on. Swap-with-last is safe because no external index
// escapes this module — but an index held across a dropped `mu` is
// not an escape the compiler can see, which is why the two states
// below are refused (OBS-42 D4).
//
// The `wire_subscribed` conjunct is the one that matters: forgetting a
// slot whose subscribe went out leaves that feed running to nobody,
// and no later pass will ever emit for it because the table no longer
// records it.
static void
gem_ws_slots_compact_locked(void)
{
  for(uint32_t i = 0; i < gem_ws_ch.n_slots; )
  {
    if(gem_ws_ch.slots[i].refcount == 0
        && !gem_ws_ch.slots[i].wire_subscribed
        && gem_ws_ch.slots[i].state != GEM_SUB_SUBSCRIBING
        && gem_ws_ch.slots[i].state != GEM_SUB_UNSUBSCRIBING)
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
        s->wire_subscribed = false;
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

    // OBS-53 — the send IS the confirmation, because nothing else ever
    // arrives. Match on (channel, SUBSCRIBING) rather than on an index
    // remembered from before the unlock: the table is compacted by
    // swap-with-last, so an index does not survive the gap (OBS-42 D4).
    // A slot added for this channel while the lock was down is IDLE,
    // not SUBSCRIBING, so this cannot claim a subscribe it never sent.
    for(i = 0; i < gem_ws_ch.n_slots; i++)
    {
      gem_ws_slot_t *s = &gem_ws_ch.slots[i];

      if(s->channel == ch && s->state == GEM_SUB_SUBSCRIBING)
      {
        s->state           = GEM_SUB_ACTIVE;
        s->wire_subscribed = true;
      }
    }

    clam(CLAM_INFO, GEM_CTX ".ws.md", "%s subscribe sent (%zu bytes)",
        gem_ws_md_channel_name(ch), flen);
  }
}

// One slot's identity, copied out from under `mu`. OBS-42 D4: the
// table is compacted by swap-with-last, so an INDEX taken before `mu`
// is dropped names a different slot afterwards — and the writes at the
// end of phase B landed on whatever had been swapped in, clearing a
// live feed's flag so its own unsubscribe could never be emitted.
// Identity is the only thing that survives a compaction.
typedef struct
{
  gem_ws_channel_t channel;
  char             symbol_native[GEM_WS_SYM_NATIVE_SZ];
} gem_ws_ident_t;

// Emit unsubscribe frames for slots whose refcount has dropped to zero
// and that the gateway still holds. Mu held by caller; released around
// each send.
//
// Two phases, because `mu` cannot be held across gem_ws_send_text:
//
//   A — one uninterrupted hold: choose the slots, copy their
//       IDENTITIES, and mark each UNSUBSCRIBING. The mark is what makes
//       a concurrent pass skip a slot whose frame is already on the
//       wire, and what stops compaction moving it.
//   B — per identity: render, drop mu, send, retake mu, then re-find
//       the slot BY IDENTITY and write only if it is still the slot we
//       marked.
//
// The old shape walked by index across the gap and restarted the walk
// from 0 whenever the table shrank, which both re-emitted for slots it
// had already handled and wrote onto the neighbour that a compaction
// had swapped into the index it was holding.
static void
gem_ws_emit_unsubscribe_locked(void)
{
  gem_ws_ident_t ident[GEM_WS_CH_MAX_SLOTS];
  uint32_t       n = 0;
  uint32_t       i;

  // Phase A — pick and mark, mu never dropped.
  for(i = 0; i < gem_ws_ch.n_slots; i++)
  {
    gem_ws_slot_t *s = &gem_ws_ch.slots[i];

    if(s->refcount != 0) continue;
    if(!s->wire_subscribed) continue;
    if(s->state == GEM_SUB_UNSUBSCRIBING) continue;

    // GEM_CH_USER has no wire name (gem_ws_md_channel_name returns
    // NULL for it), so a reap that took it would render nothing and
    // silently mark a slot it can never unmark.
    if(!gem_ws_channel_is_per_symbol(s->channel)) continue;

    s->state = GEM_SUB_UNSUBSCRIBING;

    ident[n].channel = s->channel;
    strlcpy(ident[n].symbol_native, s->symbol_native,
        sizeof(ident[n].symbol_native));
    n++;
  }

  // Phase B — one send per identity.
  for(i = 0; i < n; i++)
  {
    char           frame[GEM_WS_TX_BUF_SZ];
    size_t         flen;
    bool           ok;
    int32_t        idx;
    gem_ws_slot_t *s;

    flen = gem_ws_render_unsubscribe_one(ident[i].channel,
        ident[i].symbol_native, frame, sizeof(frame));

    if(flen == 0)
    {
      // Nothing went out, so the mark has to come off or the slot is
      // stranded: no pass wants it and compaction refuses it.
      idx = gem_ws_slot_find_locked(ident[i].channel,
          ident[i].symbol_native);

      if(idx >= 0 && gem_ws_ch.slots[idx].state == GEM_SUB_UNSUBSCRIBING)
        gem_ws_ch.slots[idx].state = GEM_SUB_ACTIVE;

      continue;
    }

    pthread_mutex_unlock(&gem_ws_ch.mu);
    ok = gem_ws_send_text(GEM_WS_MD, frame, flen);
    pthread_mutex_lock(&gem_ws_ch.mu);

    // Re-find by identity, never by index. A slot that is no longer
    // UNSUBSCRIBING is not the one this frame was for.
    idx = gem_ws_slot_find_locked(ident[i].channel,
        ident[i].symbol_native);

    if(idx < 0)
      continue;

    s = &gem_ws_ch.slots[idx];

    if(s->state != GEM_SUB_UNSUBSCRIBING)
      continue;

    if(ok != SUCCESS)
    {
      clam(CLAM_DEBUG, GEM_CTX ".ws.md",
          "%s unsubscribe deferred (send failed)",
          gem_ws_md_channel_name(ident[i].channel));

      // Back to the belief it came from: wire_subscribed was never
      // touched, and the only thing that sets it also sets ACTIVE.
      s->state = GEM_SUB_ACTIVE;
      continue;
    }

    clam(CLAM_INFO, GEM_CTX ".ws.md",
        "%s unsubscribe sent sym=%s",
        gem_ws_md_channel_name(ident[i].channel), ident[i].symbol_native);

    s->wire_subscribed = false;
    s->state         = GEM_SUB_IDLE;
  }
}

// The one place a slot nobody wants is given back. Emit first, then
// compact — compaction refuses a slot the gateway still holds, so a
// slot only becomes forgettable once its unsubscribe has actually
// gone out.
static void
gem_ws_reap_unwanted_locked(void)
{
  gem_ws_emit_unsubscribe_locked();
  gem_ws_slots_compact_locked();
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
  for(gem_ws_sub_t *s = gem_ws_ch.head; s != NULL; s = s->next)
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

// Record a traded price. Table is append-only over the process
// lifetime; GEM_WS_CH_MAX_SLOTS bounds the subscribable product set, so
// it cannot be outgrown by a product we are subscribed to.
static void
gem_ws_mark_note_locked(const char *product_abstr, double price)
{
  uint32_t i;

  if(product_abstr == NULL || product_abstr[0] == '\0' || price <= 0.0)
    return;

  for(i = 0; i < gem_ws_ch.n_marks; i++)
  {
    if(strcmp(gem_ws_ch.marks[i].product_abstr, product_abstr) == 0)
    {
      gem_ws_ch.marks[i].price = price;
      return;
    }
  }

  if(gem_ws_ch.n_marks >= GEM_WS_CH_MAX_SLOTS)
  {
    clam(CLAM_WARN, GEM_CTX ".ws.md",
        "mark table full (%u) — no last price for %s",
        gem_ws_ch.n_marks, product_abstr);
    return;
  }

  i = gem_ws_ch.n_marks++;

  strlcpy(gem_ws_ch.marks[i].product_abstr, product_abstr,
      sizeof(gem_ws_ch.marks[i].product_abstr));
  gem_ws_ch.marks[i].price = price;
}

// 0.0 when this product has not traded since the mapping loaded.
static double
gem_ws_mark_get_locked(const char *product_abstr)
{
  uint32_t i;

  if(product_abstr == NULL) return(0.0);

  for(i = 0; i < gem_ws_ch.n_marks; i++)
  {
    if(strcmp(gem_ws_ch.marks[i].product_abstr, product_abstr) == 0)
      return(gem_ws_ch.marks[i].price);
  }

  return(0.0);
}

// Emit one TICKER event derived from the latest top-of-book in the
// l2_updates payload. `changes` is the diff array — we sweep it for the
// best bid and best ask.
//
// OBS-58: `price` is NOT taken from this sweep. Until 2026-08-17 it was
// the price of whatever row happened to sit last in the diff, assigned
// before the qty==0 test below, so a *removed* level a long way off the
// book set the published price — and on a fresh socket, where the first
// l2_updates is the whole book, that was the top of the ask ladder
// (`px=1.2345679e+10`, a resting order at $12.3B). 348 of 6,876 logged
// ticks were off by more than 0.5%. It is the last traded price now,
// falling back to the mid and then to whichever side of the book we
// have. See exchange_api.h's exchange_ws_ticker_t for what the field
// means across venues.
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
  double                price    = 0.0;

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

  price = gem_ws_mark_get_locked(product_abstr);

  if(price <= 0.0)
  {
    if(best_bid > 0.0 && best_ask > 0.0)
      price = (best_bid + best_ask) / 2.0;

    else
      price = (best_bid > 0.0) ? best_bid : best_ask;
  }

  // Nothing knowable about this product yet. Publishing a zero would be
  // worse than publishing nothing: whenmoon assigns ticker price into
  // mk->last_px unconditionally, and last_px is the mark a paper or
  // real fill executes against (market_engine.c).
  if(price <= 0.0)
    return;

  ev.channel = EXCH_WS_TICKER;
  snprintf(ev.product_id, sizeof(ev.product_id), "%s", product_abstr);
  snprintf(t->product_id, sizeof(t->product_id), "%s", product_abstr);

  t->best_bid = best_bid;
  t->best_ask = best_ask;
  t->price    = price;
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

    gem_ws_mark_note_locked(product_abstr, m->price);
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

  // OBS-58: trades first. They carry the price the ticker synthesized
  // from `changes` publishes, so a frame that carries both must record
  // its own trades before the ticker reads the mark — otherwise every
  // such ticker is one frame stale.
  if(trades != NULL)
    gem_ws_md_emit_trades_array(symbol_abstr, trades, time_ms);

  if(changes != NULL)
    gem_ws_md_emit_ticker_from_l2(symbol_abstr, changes, time_ms);
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

  gem_ws_mark_note_locked(symbol_abstr, m->price);
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

// ⛔ The `subscription_ack` handler that stood here is DELETED (OBS-53).
// It correlated an ack envelope by (channel, symbol) and was the only
// writer of the slot's held flag — and this venue never sends one.
// Measured five independent ways: zero such lines across the daemon's
// entire logged history, zero across ~7 minutes of live l2, and three
// fresh probe runs (l2/BTCUSD, l2/PAXGUSD, candles_1m/PAXGUSD) in which
// the subscribe was answered by the snapshot and nothing else.
//
// Its OBS-42 D1 duty — reaping a slot whose consumer left inside the
// ack window — is not dropped, it is DESIGNED OUT: the subscribe send
// now marks the slot ACTIVE under the same lock it was rendered under,
// so the window it guarded no longer exists. If Gemini ever starts
// acking, nothing breaks: the slot is already ACTIVE and the envelope
// falls through to the unhandled-type debug line.

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

  // OBS-55 — the venue refuses with a typeless envelope, so this test
  // must come BEFORE the one below or a hard error the driver caused is
  // filed as an anonymous frame. Shape:
  //   {"reason":"InvalidJson","result":"error"}
  // WARN, not DEBUG: it is always a defect on our side — a channel this
  // gateway does not have, or a frame it will not parse — and the whole
  // reason OBS-55 needed a live gate to find is that the refusal used to
  // be indistinguishable from noise.
  {
    char result[16] = {0};

    if(json_get_str(root, "result", result, sizeof(result))
        && strcmp(result, "error") == 0)
    {
      char reason[64] = {0};

      if(!json_get_str(root, "reason", reason, sizeof(reason)))
        strlcpy(reason, "(no reason given)", sizeof(reason));

      clam(CLAM_WARN, GEM_CTX ".ws.md",
          "gateway REFUSED a frame we sent: reason='%s' (%zu bytes)",
          reason, len);

      json_object_put(root);
      return;
    }
  }

  if(!json_get_str(root, "type", type, sizeof(type)))
  {
    // Post-GEM-VERIFY-1: pre-fix this fired ~61×/connect because the
    // unfragmented dispatch trigger handed the parser truncated L2
    // snapshots that happened to parse as valid JSON (no `type` at
    // the partial top level). With the CURLWS_CONT-gated reassembly
    // in gem_ws_on_frame_locked, the path is reached only for
    // genuinely typeless server envelopes.
    //
    // ⚠ OBS-55: this comment used to say "(rare)". It was reached on
    // EVERY connection, by the `trades` subscribe this driver sent to
    // a gateway that has no such channel — the arm above now names
    // that, and the render that caused it is gone.
    clam(CLAM_DEBUG5, GEM_CTX ".ws.md",
        "frame without type (%zu bytes)", len);
    json_object_put(root);
    return;
  }

  pthread_mutex_lock(&gem_ws_ch.mu);

  if(strcmp(type, "heartbeat") == 0)
  {
    // No-op; transport layer counts as liveness.
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

  // Reset state on every slot — the gateway forgot us across the flap,
  // so nothing this session sent can still be answered.
  //
  // OBS-42 D3: the reset is UNCONDITIONAL. It used to skip refcount-0
  // slots, which is exactly the set that needs it: a slot stranded at
  // SUBSCRIBING by a consumer that left before its ack survived every
  // flap, because compaction refuses SUBSCRIBING and nothing else ever
  // looked at it. It held one of GEM_WS_CH_MAX_SLOTS for the life of
  // the mapping, and the table's occupancy stopped tracking live
  // subscriptions and started tracking ever-subscribed pairs.
  for(uint32_t i = 0; i < gem_ws_ch.n_slots; i++)
  {
    gem_ws_ch.slots[i].wire_subscribed = false;
    gem_ws_ch.slots[i].state         = GEM_SUB_IDLE;
  }

  // Now that nothing is held or in flight, the slots nobody wants are
  // forgettable — and must go before the resubscribe, or we re-ask the
  // gateway for feeds no consumer is left to read.
  gem_ws_slots_compact_locked();

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
    // OBS-55 — this gateway has NO trades channel, in either spelling
    // (`trades` and `trade` are both answered `InvalidJson`, measured
    // against the same pair in the same minute). Trades arrive as
    // `"type":"trade"` frames INSIDE the `l2` stream, so a consumer
    // asking for trades wants exactly the subscription a ticker
    // consumer wants, and says so here.
    //
    // ⭑ This is a mapping and not a refusal on purpose: a consumer
    // asking for trades ALONE still gets a wire subscription. Today
    // whenmoon is the only caller and it always asks for ticker and
    // trades together, so dropping the render would have worked — on
    // today's callers. The API permits trades-only, and that is what
    // this is priced on.
    //
    // Delivery is unaffected either way: gem_ws_fanout_locked gates on
    // the SUBSCRIPTION's abstract channel mask, never on the slot
    // table, so a trade event still reaches exactly the consumers who
    // asked for EXCH_WS_TRADES.
    case EXCH_WS_TRADES:  return(GEM_CH_TICKER);
    case EXCH_WS_OHLC_1M: return(GEM_CH_OHLC_1M);
    case EXCH_WS_USER:    return(GEM_CH_USER);
    case EXCH_WS_BOOK_L2: return(GEM_CH__COUNT);
  }
  return(GEM_CH__COUNT);
}

// OBS-51: give back an incomplete arm. Every refcount this call took
// comes off, so a slot it allocated falls to zero and compacts away —
// nothing has been emitted for it yet, so none is `wire_subscribed` and
// none can be stranded. Order matters: the deltas are indexed by slot
// position, so they must be applied BEFORE the compaction that moves
// slots. Caller holds `mu` and frees `sub` after unlocking.
static void
gem_ws_sub_rollback_locked(gem_ws_sub_t *sub, const uint32_t *rc_delta)
{
  gem_ws_sub_t **pp;
  uint32_t       i;

  for(i = 0; i < gem_ws_ch.n_slots; i++)
    gem_ws_ch.slots[i].refcount -= rc_delta[i];

  gem_ws_slots_compact_locked();

  for(pp = &gem_ws_ch.head; *pp != NULL; pp = &(*pp)->next)
  {
    if(*pp == sub)
    {
      *pp = sub->next;
      gem_ws_ch.n_subs--;
      break;
    }
  }
}

// ------------------------------------------------------------------ //
// Public API: gem_ws_subscribe / gem_ws_unsubscribe                   //
// ------------------------------------------------------------------ //

bool
gem_ws_subscribe(const exchange_ws_channel_t *channels, uint32_t n_channels,
    const char *const *product_ids, uint32_t n_products,
    exchange_ws_event_cb_t cb, void *user, void **out_handle)
{
  gem_ws_sub_t           *sub;
  uint32_t                channel_mask     = 0;
  bool                    has_user_channel = false;
  bool                    has_per_symbol   = false;
  uint32_t                rc_delta[GEM_WS_CH_MAX_SLOTS] = {0};
  uint32_t                n_wanted         = 0;
  uint32_t                n_armed          = 0;
  uint32_t                n_slots_seen     = 0;
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

        n_wanted++;

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
        rc_delta[idx]++;
        n_armed++;
      }
    }
  }

  // OBS-51: SUCCESS means the handle covers every pair it asked for.
  // A partial arm cannot be reported through this interface and cannot
  // be repaired by the consumer either — whenmoon's reconcile diffs the
  // set it REQUESTED against the set it wants, so a binding that is
  // live-but-incomplete matches and is never re-driven. Refuse the
  // whole call instead: a NULL handle is the one answer that reconcile
  // reads as "retry me".
  if(n_armed < n_wanted)
  {
    n_slots_seen = gem_ws_ch.n_slots;

    gem_ws_sub_rollback_locked(sub, rc_delta);
    pthread_mutex_unlock(&gem_ws_ch.mu);
    mem_free(sub);

    clam(CLAM_WARN, GEM_CTX ".ws",
        "subscribe: armed %u of %u (channel,symbol) pairs — refusing the "
        "whole call (slots %u/%u)",
        n_armed, n_wanted, n_slots_seen, GEM_WS_CH_MAX_SLOTS);

    return(FAIL);
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
gem_ws_unsubscribe(void *driver_sub)
{
  gem_ws_sub_t  *handle = driver_sub;
  gem_ws_sub_t **pp;
  uint32_t       sub_id;
  uint32_t       i;

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

  // Give back every slot that just hit zero. The MD session may not be
  // OPEN — a silent send failure is fine, and leaves the slot ACTIVE
  // and un-compacted so the next open resubscribes it rather than
  // forgetting a feed the gateway is still streaming.
  gem_ws_reap_unwanted_locked();

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
  gem_ws_sub_t *s;
  gem_ws_sub_t *n;

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
  gem_ws_ch.n_marks = 0;

  pthread_mutex_unlock(&gem_ws_ch.mu);

  pthread_mutex_destroy(&gem_ws_ch.mu);
  gem_ws_ch.initialized = false;

  clam(CLAM_DEBUG, GEM_CTX, "ws channel multiplexer torn down");
}
