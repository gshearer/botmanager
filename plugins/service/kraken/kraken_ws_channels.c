// botmanager — MIT
// Kraken WebSocket v2 channel multiplexer.
//
// Layered on top of kraken_ws.c's single-session transport. Owns:
//   * the local subscriber list (one per `exchange_ws_subscribe` caller),
//   * a (channel, symbol) slot table that refcounts shared subscribers
//     so N consumers watching the same feed share one upstream slot,
//   * the JSON-RPC req_id correlator that pairs subscribe acks with the
//     slot they originated from,
//   * the per-channel parser that turns one inbound `update` frame into
//     fanned-out `exchange_ws_event_t` events.
//
// Wire format is Kraken WS v2 JSON-RPC:
//
//   subscribe (public):
//     {"method":"subscribe",
//      "params":{"channel":"trade","symbol":["BTC/USD"],"req_id":N}}
//
//   subscribe (private):
//     {"method":"subscribe",
//      "params":{"channel":"executions","token":"<t>","req_id":N}}
//
//   subscribe ack:
//     {"method":"subscribe",
//      "result":{"channel":"trade","symbol":"BTC/USD"},
//      "success":true,"req_id":N}
//
//   data:
//     {"channel":"<name>","type":"snapshot|update","data":[{...}]}
//
//   error frame:
//     {"error":"<message>","method":"subscribe","req_id":N}
//
// On reconnect the slot table drives a full resubscribe so consumer
// callbacks never miss a beat across a flap. Sequence-gap detection is
// not performed: Kraken v2 does not include a per-product sequence
// number, and the trade/ohlc channels recover lost frames via REST
// backfill (fetch_candles + TradesHistory) — same shape as coinbase's
// rip in CB-WS-SEQ-1.

#define KR_INTERNAL
#include "kraken.h"

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
// ------------------------------------------------------------------ //

#define KR_WS_CH_MAX_SUBS               64
#define KR_WS_CH_MAX_PRODUCTS_PER_SUB   16
#define KR_WS_CH_MAX_SLOTS             128
#define KR_WS_CH_REQ_RING_SIZE         256

// Internal channel enum. Maps to Kraken v2 channel strings via
// kr_ws_channel_name(). The PRIVATE flag distinguishes channels that
// require a token in the subscribe payload.
typedef enum
{
  KR_CH_TICKER     = 0,
  KR_CH_TRADE      = 1,
  KR_CH_OHLC       = 2,    // 1-minute bars; KR-5 only subscribes to interval=1
  KR_CH_EXECUTIONS = 3,
  KR_CH_BALANCES   = 4,
  KR_CH__COUNT
} kr_ws_channel_t;

static bool
kr_ws_channel_is_private(kr_ws_channel_t ch)
{
  return(ch == KR_CH_EXECUTIONS || ch == KR_CH_BALANCES);
}

// Which gateway a channel's frames belong on. This is the ONLY place
// that decision is made: Kraken serves public market data and private
// account data on two endpoints, each of which refuses the other's
// channels outright (OBS-18), so a frame sent to the wrong one is not
// degraded — it is refused, and refused silently as far as the consumer
// is concerned.
static kr_ws_session_id_t
kr_ws_session_for_channel(kr_ws_channel_t ch)
{
  return(kr_ws_channel_is_private(ch) ? KR_WS_PRIVATE : KR_WS_PUBLIC);
}

static bool
kr_ws_channel_is_per_symbol(kr_ws_channel_t ch)
{
  // Public channels carry per-symbol subscribe params; private channels
  // (executions, balances) are global to the authenticated account.
  return(!kr_ws_channel_is_private(ch));
}

static const char *
kr_ws_channel_name(kr_ws_channel_t ch)
{
  switch(ch)
  {
    case KR_CH_TICKER:     return("ticker");
    case KR_CH_TRADE:      return("trade");
    case KR_CH_OHLC:       return("ohlc");
    case KR_CH_EXECUTIONS: return("executions");
    case KR_CH_BALANCES:   return("balances");
    case KR_CH__COUNT:     break;
  }
  return(NULL);
}

// ------------------------------------------------------------------ //
// Subscription handle. One per kr_ws_subscribe() call; freed by        //
// kr_ws_unsubscribe(). `channel_mask` is a bitmask over kr_ws_channel_t //
// so membership tests in fanout are single-op. Holds the per-channel  //
// product set (private channels ignore it — global to the account).   //
// ------------------------------------------------------------------ //

typedef struct kr_ws_sub
{
  uint32_t                id;
  exchange_ws_event_cb_t  cb;
  void                   *user;

  uint32_t                channel_mask;
  uint32_t                n_products;
  char                    products[KR_WS_CH_MAX_PRODUCTS_PER_SUB]
                                  [EXCHANGE_PRODUCT_ID_SZ];

  struct kr_ws_sub       *next;
} kr_ws_sub_t;

// The two -ING states mean the same thing in opposite directions: a frame
// for this slot is on the wire, and no other reconcile pass may emit one
// for it until phase 3 or an ack resolves it (OBS-20, OBS-35). Every
// other value is a settled belief about what the gateway holds.
typedef enum
{
  KR_SUB_IDLE,
  KR_SUB_SUBSCRIBING,
  KR_SUB_UNSUBSCRIBING,
  KR_SUB_ACTIVE,
  KR_SUB_FAILED
} kr_sub_state_t;

// Per (channel, symbol) dedup slot. `symbol_ws` is the empty string for
// channels that aren't symbol-keyed (executions / balances).
// `state` advances IDLE → SUBSCRIBING (frame sent) → ACTIVE (ack
// received) or FAILED (ack with success=false / timeout); a departing
// slot goes ACTIVE → UNSUBSCRIBING (frame sent) → IDLE.
typedef struct
{
  kr_ws_channel_t  channel;
  char             symbol_ws[EXCHANGE_PRODUCT_ID_SZ];   // Kraken WS form: BTC/USD
  uint32_t         refcount;
  kr_sub_state_t   state;
  uint32_t         req_id;        // last subscribe req_id; 0 = none in flight
  bool             sent_upstream; // true once subscribe ack received
  char             last_err[128];
} kr_ws_slot_t;

// Req-id correlator. Each outbound frame carries a fresh req_id and
// parks the slot's IDENTITY — never its index: an index is only true
// for as long as the hold that read it, and the ack it answers arrives
// a round trip later, by which time a compact may have moved another
// slot into that position (OBS-5). Stale entries are overwritten as the
// ring wraps; 256 slots covers every realistic burst.
typedef struct
{
  uint32_t         req_id;                              // 0 = empty
  kr_ws_channel_t  channel;
  char             symbol_ws[EXCHANGE_PRODUCT_ID_SZ];
} kr_ws_corr_entry_t;

static struct
{
  pthread_mutex_t          mu;
  kr_ws_sub_t             *head;
  uint32_t                 next_id;
  uint32_t                 n_subs;

  kr_ws_slot_t             slots[KR_WS_CH_MAX_SLOTS];
  uint32_t                 n_slots;

  kr_ws_corr_entry_t       corr_ring[KR_WS_CH_REQ_RING_SIZE];
  _Atomic uint32_t         next_req_id;

  // SAN-10 (coinbase twin, OBS-5): consumer callbacks run with `mu`
  // RELEASED (see kr_ws_fanout), so the lock no longer answers "is a
  // callback running?". This counter does, and it is what keeps
  // kr_ws_unsubscribe's contract — it returns only once nothing is
  // inside a consumer's callback. Both are mu-guarded; `drain` is
  // broadcast as the count reaches zero.
  uint32_t                 in_dispatch;
  pthread_cond_t           drain;

  bool                     initialized;
} kr_ws_ch;

// ------------------------------------------------------------------ //
// Slot table — caller holds kr_ws_ch.mu                               //
// ------------------------------------------------------------------ //

static int32_t
kr_ws_slot_find_locked(kr_ws_channel_t ch, const char *symbol_ws)
{
  const char *sym = (symbol_ws != NULL) ? symbol_ws : "";

  for(uint32_t i = 0; i < kr_ws_ch.n_slots; i++)
  {
    kr_ws_slot_t *s = &kr_ws_ch.slots[i];

    if(s->channel == ch && strcmp(s->symbol_ws, sym) == 0)
      return((int32_t)i);
  }

  return(-1);
}

static kr_ws_slot_t *
kr_ws_slot_alloc_locked(kr_ws_channel_t ch, const char *symbol_ws)
{
  kr_ws_slot_t *s;

  if(kr_ws_ch.n_slots >= KR_WS_CH_MAX_SLOTS)
    return(NULL);

  s = &kr_ws_ch.slots[kr_ws_ch.n_slots++];

  memset(s, 0, sizeof(*s));
  s->channel = ch;
  s->state   = KR_SUB_IDLE;

  snprintf(s->symbol_ws, sizeof(s->symbol_ws), "%s",
      (symbol_ws != NULL) ? symbol_ws : "");

  return(s);
}

// Drop slots whose refcount hit zero after an unsubscribe emission.
// Swap-with-last reshuffles the table, which is safe because nothing
// outside this hold remembers a position: the correlator ring parks
// (channel, symbol_ws) and a req_id, and reconcile re-derives every
// slot by that identity before writing it (OBS-5). Both sides have to
// stay true together — store an index anywhere and this becomes a
// silent write to the wrong subscription.
static void
kr_ws_slots_compact_locked(void)
{
  for(uint32_t i = 0; i < kr_ws_ch.n_slots; )
  {
    if(kr_ws_ch.slots[i].refcount == 0
        && kr_ws_ch.slots[i].state != KR_SUB_SUBSCRIBING
        && kr_ws_ch.slots[i].state != KR_SUB_UNSUBSCRIBING)
    {
      kr_ws_ch.slots[i] = kr_ws_ch.slots[kr_ws_ch.n_slots - 1];
      kr_ws_ch.n_slots--;
    }
    else
      i++;
  }
}

// ------------------------------------------------------------------ //
// Req-id correlator                                                   //
// ------------------------------------------------------------------ //

// Allocate a new req_id and park (req_id → slot identity) in the ring.
// Older entries at the same ring index are evicted silently. Caller
// holds mu.
static uint32_t
kr_ws_corr_push_locked(kr_ws_channel_t ch, const char *symbol_ws)
{
  kr_ws_corr_entry_t *ent;
  uint32_t            rid;

  for(;;)
  {
    rid = atomic_fetch_add(&kr_ws_ch.next_req_id, 1u) + 1u;

    // Wrap; never hand out 0 (reserved as "no req_id" sentinel).
    if(rid != 0)
      break;
  }

  ent = &kr_ws_ch.corr_ring[rid % KR_WS_CH_REQ_RING_SIZE];

  ent->req_id  = rid;
  ent->channel = ch;

  strlcpy(ent->symbol_ws, (symbol_ws != NULL) ? symbol_ws : "",
      sizeof(ent->symbol_ws));

  return(rid);
}

// Returns the parked entry by value; a `.req_id` of 0 is a miss, which
// is the sentinel the ring already reserves for an empty entry.
static kr_ws_corr_entry_t
kr_ws_corr_pop_locked(uint32_t req_id)
{
  kr_ws_corr_entry_t ent = {0};
  uint32_t           idx;

  if(req_id == 0)
    return(ent);

  idx = req_id % KR_WS_CH_REQ_RING_SIZE;

  if(kr_ws_ch.corr_ring[idx].req_id != req_id)
    return(ent);

  ent = kr_ws_ch.corr_ring[idx];

  kr_ws_ch.corr_ring[idx].req_id = 0;

  return(ent);
}

// ------------------------------------------------------------------ //
// Frame rendering                                                     //
// ------------------------------------------------------------------ //

// Render one Kraken v2 subscribe / unsubscribe frame for a single
// (channel, slot). `op` is "subscribe" or "unsubscribe". Returns bytes
// written, or 0 on overflow / unsupported. Symbol + token are JSON-safe
// by construction (Kraken altname/wsname is alnum + slash; tokens are
// base64-ish).
static size_t
kr_ws_render_frame(char *out, size_t cap, const char *op,
    kr_ws_channel_t ch, const char *symbol_ws, uint32_t req_id,
    const char *token)
{
  const char *cname;
  size_t      pos = 0;
  int         n;

  if(out == NULL || cap == 0 || op == NULL)
    return(0);

  cname = kr_ws_channel_name(ch);
  if(cname == NULL)
    return(0);

  n = snprintf(out + pos, cap - pos,
      "{\"method\":\"%s\",\"params\":{\"channel\":\"%s\"", op, cname);
  if(n < 0 || (size_t)n >= cap - pos) return(0);
  pos += (size_t)n;

  if(kr_ws_channel_is_per_symbol(ch))
  {
    if(symbol_ws == NULL || symbol_ws[0] == '\0')
      return(0);

    n = snprintf(out + pos, cap - pos,
        ",\"symbol\":[\"%s\"]", symbol_ws);
    if(n < 0 || (size_t)n >= cap - pos) return(0);
    pos += (size_t)n;
  }

  if(ch == KR_CH_OHLC)
  {
    n = snprintf(out + pos, cap - pos, ",\"interval\":1");
    if(n < 0 || (size_t)n >= cap - pos) return(0);
    pos += (size_t)n;
  }

  if(ch == KR_CH_TRADE || ch == KR_CH_OHLC)
  {
    // Snapshot=true is the v2 default but make it explicit so a future
    // protocol bump does not silently change behaviour.
    n = snprintf(out + pos, cap - pos, ",\"snapshot\":true");
    if(n < 0 || (size_t)n >= cap - pos) return(0);
    pos += (size_t)n;
  }

  if(kr_ws_channel_is_private(ch))
  {
    if(token == NULL || token[0] == '\0')
      return(0);

    n = snprintf(out + pos, cap - pos,
        ",\"token\":\"%s\"", token);
    if(n < 0 || (size_t)n >= cap - pos) return(0);
    pos += (size_t)n;
  }

  // Kraken WS v2 requires `req_id` at the top level of the envelope,
  // not nested inside `params`. A nested req_id is rejected with
  // "Unsupported field: 'req_id' for subscription type: '<channel>'"
  // and the subscribe silently fails (no req_id echoed back so the
  // local ack correlator never gets a hit).
  n = snprintf(out + pos, cap - pos,
      "},\"req_id\":%u}", req_id);
  if(n < 0 || (size_t)n >= cap - pos) return(0);
  pos += (size_t)n;

  return(pos);
}

// Emit `op` for one (channel, symbol) pair. Takes the identity rather
// than a slot pointer: mu is released around this call, so a pointer
// into the slot table would not survive it.
static bool
kr_ws_emit_one(const char *op, kr_ws_channel_t ch, const char *symbol_ws,
    uint32_t req_id, const char *token)
{
  char    frame[KR_WS_TX_BUF_SZ];
  size_t  len;

  len = kr_ws_render_frame(frame, sizeof(frame), op, ch, symbol_ws,
      req_id, token);

  if(len == 0)
    return(FAIL);

  return(kr_ws_send_text(kr_ws_session_for_channel(ch), frame, len));
}

// ------------------------------------------------------------------ //
// Reconcile: emit subscribe / unsubscribe frames for pending deltas.  //
//                                                                      //
// Three phases, because the transport cannot be driven under mu and a  //
// slot index cannot survive letting go of it (OBS-5):                  //
//                                                                      //
//   1. one coherent scan under the caller's hold, collecting the       //
//      identity + req_id of every slot in the requested transition,    //
//   2. the whole batch emitted with mu released exactly once,          //
//   3. post-emit state applied by re-deriving each slot from its       //
//      identity, guarded by the req_id it was minted for.              //
//                                                                      //
// The scan being one hold is what makes the walk itself safe: a        //
// compaction can no longer move an unvisited slot below the cursor.    //
// A slot with a frame already in flight is not collected by phase 1 —  //
// that is what makes two overlapping passes emit one frame between     //
// them instead of two.                                                 //
// Mu must be held by caller; this function releases it once, in the    //
// middle, and returns holding it.                                      //
// ------------------------------------------------------------------ //

typedef enum
{
  KR_RECONCILE_SUB,    // emit subscribe for slots that need it
  KR_RECONCILE_UNSUB   // emit unsubscribe for slots refcount==0
} kr_reconcile_op_t;

// `only_sid` restricts a pass to the slots belonging to one gateway.
// KR_WS_SID_ANY is the whole table, and is right wherever the emit is
// opportunistic: a frame for a session that is not open simply fails to
// send and its slot stays IDLE for that session's own on-open pass. A
// reconnect pass must NOT use it — resetting the sibling session's live
// slots would unsubscribe a feed nothing has flapped.
#define KR_WS_SID_ANY (-1)

// One pending emission. Carries the slot's identity rather than its
// position, so phase 3 can find the slot again whatever the table did
// while the frame was in flight.
typedef struct
{
  kr_ws_channel_t  channel;
  char             symbol_ws[EXCHANGE_PRODUCT_ID_SZ];
  uint32_t         rid;
  bool             rc;                     // kr_ws_emit_one's verdict
} kr_ws_emit_item_t;

static void
kr_ws_reconcile_locked(kr_reconcile_op_t op, const char *token, int only_sid)
{
  // ~4.5 KB of frame. Reconcile runs on subscribe, unsubscribe and
  // reconnect — never per message — and the table it mirrors is a
  // fixed 128 slots.
  kr_ws_emit_item_t  items[KR_WS_CH_MAX_SLOTS];
  uint32_t           n_items = 0;
  uint32_t           i;
  uint32_t           k;
  bool               want;
  bool               orphaned = false;
  const char        *opname = (op == KR_RECONCILE_SUB ? "subscribe"
                                                      : "unsubscribe");

  // Phase 1 — collect under the caller's hold.
  for(i = 0; i < kr_ws_ch.n_slots; i++)
  {
    kr_ws_slot_t *s = &kr_ws_ch.slots[i];

    if(only_sid != KR_WS_SID_ANY
        && (int)kr_ws_session_for_channel(s->channel) != only_sid)
      continue;

    if(op == KR_RECONCILE_SUB)
    {
      // Only emit for slots that have at least one consumer and aren't
      // already known active. A slot in FAILED stays failed until the
      // caller removes it; we don't auto-retry here.
      //
      // The in-flight test is the other half, and it is what makes two
      // overlapping passes safe (OBS-35). `sent_upstream` does not go
      // true until the ACK lands, so for one whole round trip a slot
      // that already has a frame on the wire still answers yes here —
      // and the second pass mints a second req_id for it. On a lazily
      // opened private session that overlap is the normal path, not a
      // race: the session opens BECAUSE of the subscribe that kicked
      // the token fetch, so on-open and the token trampoline reconcile
      // the same slots microseconds apart.
      //
      // The trade: a subscribe whose ack never arrives at all now waits
      // for its session's next open rather than for the next unrelated
      // reconcile. Kraken answers every subscribe, and a session that
      // goes quiet is dropped by the idle watchdog, whose on-open resets
      // these slots to IDLE.
      want = (s->refcount > 0
              && s->state != KR_SUB_SUBSCRIBING
              && (s->state == KR_SUB_IDLE || !s->sent_upstream));
    }
    else
    {
      // Only emit unsubscribe for slots whose refcount has dropped to
      // zero and which the gateway currently has live — and never for
      // one whose unsubscribe is already on the wire. Two consumers
      // leaving at once is enough: `sent_upstream` is not cleared until
      // phase 3, so each pass sees the other's target still looking
      // live and unsubscribes it a second time (OBS-20).
      want = (s->refcount == 0
              && s->sent_upstream
              && s->state != KR_SUB_UNSUBSCRIBING);
    }

    if(!want)
      continue;

    // Private channel must have a token; skip silently — caller
    // guarantees we only enter here with a non-NULL token when private
    // channels exist in the table.
    if(kr_ws_channel_is_private(s->channel)
        && (token == NULL || token[0] == '\0'))
      continue;

    items[n_items].rid     = kr_ws_corr_push_locked(s->channel,
        s->symbol_ws);
    items[n_items].channel = s->channel;
    items[n_items].rc      = FAIL;

    strlcpy(items[n_items].symbol_ws, s->symbol_ws,
        sizeof(items[n_items].symbol_ws));

    if(op == KR_RECONCILE_SUB)
    {
      s->req_id = items[n_items].rid;
      s->state  = KR_SUB_SUBSCRIBING;
    }

    // No req_id on this side. `req_id` is what the ack handler matches
    // against, and an unsubscribe ack that matched would forge ACTIVE +
    // sent_upstream onto the slot — the exact write OBS-5's guard exists
    // to refuse. The state is the whole guard here.
    else
      s->state = KR_SUB_UNSUBSCRIBING;

    n_items++;
  }

  if(n_items == 0)
    return;

  // Phase 2 — emit with mu released. Frames render from the snapshots
  // above and touch no shared state, so the log lines belong here too.
  pthread_mutex_unlock(&kr_ws_ch.mu);

  for(k = 0; k < n_items; k++)
  {
    items[k].rc = kr_ws_emit_one(opname, items[k].channel,
        items[k].symbol_ws, items[k].rid, token);

    // SUCCESS=false / FAIL=true convention: a raw `!rc` test inverts
    // the meaning of the return value. Compare against SUCCESS so the
    // deferred-send branch only fires on actual transport failure.
    if(items[k].rc != SUCCESS)
      clam(CLAM_DEBUG, KR_CTX,
          "ws %s ch=%s sym=%s deferred (send failed; retry on next "
          "open)",
          opname, kr_ws_channel_name(items[k].channel),
          items[k].symbol_ws);
    else
      clam(CLAM_INFO, KR_CTX,
          "ws %s ch=%s sym=%s req_id=%u",
          opname, kr_ws_channel_name(items[k].channel),
          items[k].symbol_ws, items[k].rid);
  }

  pthread_mutex_lock(&kr_ws_ch.mu);

  // Phase 3 — apply by identity. A slot that went away owes nothing;
  // one that has since been re-emitted holds a newer req_id and must
  // not be stomped by this pass's verdict.
  for(k = 0; k < n_items; k++)
  {
    kr_ws_slot_t *s;
    int32_t       idx;

    idx = kr_ws_slot_find_locked(items[k].channel, items[k].symbol_ws);

    if(idx < 0)
      continue;

    s = &kr_ws_ch.slots[idx];

    if(items[k].rc != SUCCESS)
    {
      if(op == KR_RECONCILE_SUB && s->req_id == items[k].rid)
      {
        s->state  = KR_SUB_IDLE;
        s->req_id = 0;
      }

      // An unsubscribe that never left is not in flight. Hand the slot
      // back the belief it had: `sent_upstream` was never touched, and
      // the only thing that sets it is the same ack that sets ACTIVE, so
      // ACTIVE is exactly where this slot came from. Without this the
      // slot is stranded — no pass wants it and compaction skips it.
      if(op == KR_RECONCILE_UNSUB && s->state == KR_SUB_UNSUBSCRIBING)
        s->state = KR_SUB_ACTIVE;

      continue;
    }

    if(op == KR_RECONCILE_UNSUB)
    {
      // The frame is on the wire, so this records what was SENT rather
      // than what we would prefer: the gateway no longer holds this
      // slot. That is why it is unconditional.
      s->sent_upstream = false;
      s->state         = KR_SUB_IDLE;
      s->req_id        = 0;

      // ...but a consumer may have arrived for this very feed while the
      // frame was in flight, and its own reconcile could not emit — the
      // slot was UNSUBSCRIBING. Nothing else will trigger for it. The
      // primitive owes that guarantee itself rather than borrowing
      // whenmoon's full-rebuild flow (OBS-20).
      if(s->refcount > 0)
        orphaned = true;
    }
  }

  // One tail pass for whatever the loop above handed back to a live
  // consumer, and it is ordered: the unsubscribe reached the transport in
  // phase 2, so the subscribe that replaces it cannot overtake it on the
  // socket. KR_RECONCILE_SUB never sets `orphaned`, so this recurses
  // exactly once; `token` and `only_sid` carry through unchanged, and a
  // private slot cannot reach here without the token that got it past
  // phase 1 in the first place.
  if(orphaned)
    kr_ws_reconcile_locked(KR_RECONCILE_SUB, token, only_sid);
}

// Synchronous probe whether the slot table contains any private slot
// with refcount>0 — used to decide whether kr_ws_subscribe must block on
// a token before reconciling.
static bool
kr_ws_has_private_pending_locked(void)
{
  for(uint32_t i = 0; i < kr_ws_ch.n_slots; i++)
  {
    kr_ws_slot_t *s = &kr_ws_ch.slots[i];

    if(s->refcount > 0
        && kr_ws_channel_is_private(s->channel)
        && (s->state == KR_SUB_IDLE || !s->sent_upstream))
      return(true);
  }
  return(false);
}

// ------------------------------------------------------------------ //
// Token-acquire trampoline. Reconciles after the token resolves       //
// (success or failure). On failure we still call reconcile so public  //
// slots flush; private slots stay IDLE and surface KR_SUB_FAILED via  //
// the consumer-error callback path on the next open.                  //
// ------------------------------------------------------------------ //

static void
kr_ws_token_then_reconcile_cb(bool ok, void *user)
{
  char  token[KR_WS_TOKEN_SZ] = {0};
  bool  have_token            = SUCCESS;

  (void)user;
  (void)ok;   // logged inside kr_ws_token_done; success/failure both
              // drop into the reconcile below.

  have_token = kr_ws_token_snapshot(token, sizeof(token));

  pthread_mutex_lock(&kr_ws_ch.mu);

  // KR_WS_SID_ANY: the token is the private session's business, but a
  // public slot left IDLE by an earlier failed send costs nothing to
  // retry here and the private session may not even be open yet — its
  // own on-open pass is what actually flushes these.
  kr_ws_reconcile_locked(KR_RECONCILE_SUB,
      have_token == SUCCESS ? token : NULL, KR_WS_SID_ANY);

  pthread_mutex_unlock(&kr_ws_ch.mu);
}

// ------------------------------------------------------------------ //
// Dispatch: parse the payload and fan out to every matching subscriber //
// ------------------------------------------------------------------ //

static kr_ws_channel_t
kr_ws_channel_parse(const char *name)
{
  if(name == NULL) return(KR_CH__COUNT);
  if(strcmp(name, "ticker")     == 0) return(KR_CH_TICKER);
  if(strcmp(name, "trade")      == 0) return(KR_CH_TRADE);
  if(strcmp(name, "ohlc")       == 0) return(KR_CH_OHLC);
  if(strcmp(name, "executions") == 0) return(KR_CH_EXECUTIONS);
  if(strcmp(name, "balances")   == 0) return(KR_CH_BALANCES);
  return(KR_CH__COUNT);
}

// Kraken v2 timestamps are ISO-8601 UTC with sub-second precision:
// "2026-05-12T12:34:56.789012Z". Parse to ms-since-epoch. On failure
// return 0 — callers stamp against the wall clock for liveness.
static int64_t
kr_ws_parse_iso8601_ms(const char *s)
{
  struct tm    tm    = {0};
  int          y, M, d, h, m, sec;
  long         frac_ms = 0;
  const char  *p;
  time_t       t;

  if(s == NULL || s[0] == '\0') return(0);

  if(sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &M, &d, &h, &m, &sec) != 6)
    return(0);

  tm.tm_year = y - 1900;
  tm.tm_mon  = M - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min  = m;
  tm.tm_sec  = sec;

  t = timegm(&tm);
  if(t == (time_t)-1) return(0);

  p = strchr(s, '.');
  if(p != NULL)
  {
    long  ns     = 0;
    int   digits = 0;
    int   i;

    p++;
    while(*p >= '0' && *p <= '9' && digits < 9)
    {
      ns = ns * 10 + (*p - '0');
      p++;
      digits++;
    }
    for(i = digits; i < 9; i++)
      ns *= 10;
    frac_ms = ns / 1000000;
  }

  return((int64_t)t * 1000 + frac_ms);
}

// Lowercase an ASCII string in place; safe on the small EXCHANGE_SIDE_SZ
// buffers fanout consumers expect to be "buy"/"sell".
static void
kr_ws_lower_ascii(char *s)
{
  size_t i;

  if(s == NULL) return;

  for(i = 0; s[i] != '\0'; i++)
  {
    if(s[i] >= 'A' && s[i] <= 'Z')
      s[i] = (char)(s[i] + 32);
  }
}

// Kraken WS v2 emits symbols as `BASE/QUOTE` (e.g. `BTC/USD`), but the
// abstraction-side product_id used by every downstream consumer
// (whenmoon's wm_market_find, strategies, the fanout subscriber chain)
// is the hyphen-separated `BASE-QUOTE` form built by
// `wm_market_wire_symbol`. Translate in place so consumers can compare
// product_ids byte-for-byte. Idempotent for already-hyphen forms.
static void
kr_ws_symbol_to_abstraction(char *s)
{
  size_t i;

  if(s == NULL) return;

  for(i = 0; s[i] != '\0'; i++)
  {
    if(s[i] == '/')
      s[i] = '-';
  }
}

// Kraken v2 emits doubles as either bare JSON numbers OR strings
// (depending on field). Accept both transparently.
static double
kr_ws_get_decimal(struct json_object *obj, const char *key)
{
  struct json_object *v;
  const char         *s;

  if(obj == NULL || !json_object_object_get_ex(obj, key, &v))
    return(0.0);

  if(json_object_is_type(v, json_type_string))
  {
    s = json_object_get_string(v);
    return(s != NULL ? strtod(s, NULL) : 0.0);
  }

  return(json_object_get_double(v));
}

static int64_t
kr_ws_get_int64_loose(struct json_object *obj, const char *key)
{
  struct json_object *v;
  const char         *s;

  if(obj == NULL || !json_object_object_get_ex(obj, key, &v))
    return(0);

  if(json_object_is_type(v, json_type_string))
  {
    s = json_object_get_string(v);
    return(s != NULL ? (int64_t)strtoll(s, NULL, 10) : 0);
  }

  return((int64_t)json_object_get_int64(v));
}

// Deliver one event to every sub whose channel/product set covers it.
// Takes kr_ws_ch.mu itself; the caller must NOT hold it.
//
// SAN-10 (the coinbase twin flagged DEDUCED as OBS-5): the matching
// subscribers are collected under the lock and called with it released.
// A consumer callback is another plugin's code and takes that plugin's
// locks — whenmoon's takes the markets rwlock — while whenmoon's
// subscribe path takes them the other way round, market rwlock then
// this mutex. Fanning out under `mu` closes that cycle. It was measured
// on coinbase; kraken has never been exercised under TSan, and this is
// the same code with a different wire format.
//
// `in_dispatch` replaces the lock as unsubscribe's barrier: it is
// raised before the lock goes and dropped after the last callback
// returns, so an unsubscribe that has already unlinked its sub still
// waits for a delivery in flight. A consumer callback still must not
// call kr_ws_subscribe / kr_ws_unsubscribe — that now waits on itself
// rather than deadlocking on the mutex.
static void
kr_ws_fanout(const exchange_ws_event_t *ev)
{
  struct
  {
    exchange_ws_event_cb_t cb;
    void                  *user;
  }        targets[KR_WS_CH_MAX_SUBS];   // subscribe caps the list at this
  uint32_t n = 0;
  uint32_t i;

  pthread_mutex_lock(&kr_ws_ch.mu);

  for(kr_ws_sub_t *s = kr_ws_ch.head; s != NULL; s = s->next)
  {
    bool match;

    if(!(s->channel_mask & (1u << ev->channel)))
      continue;

    if(ev->product_id[0] == '\0')
    {
      // Account-global event (executions/balances). Fan out to every sub
      // that subscribed to the matching channel mask, regardless of
      // their per-product set.
      match = true;
    }
    else
    {
      match = false;
      for(i = 0; i < s->n_products; i++)
      {
        if(strcmp(s->products[i], ev->product_id) == 0)
        {
          match = true;
          break;
        }
      }
    }

    if(match && s->cb != NULL)
    {
      targets[n].cb   = s->cb;
      targets[n].user = s->user;
      n++;
    }
  }

  if(n == 0)
  {
    pthread_mutex_unlock(&kr_ws_ch.mu);
    return;
  }

  kr_ws_ch.in_dispatch++;

  pthread_mutex_unlock(&kr_ws_ch.mu);

  for(i = 0; i < n; i++)
    targets[i].cb(ev, targets[i].user);

  pthread_mutex_lock(&kr_ws_ch.mu);

  kr_ws_ch.in_dispatch--;

  if(kr_ws_ch.in_dispatch == 0)
    pthread_cond_broadcast(&kr_ws_ch.drain);

  pthread_mutex_unlock(&kr_ws_ch.mu);
}

// Wait out every consumer callback currently in flight. Caller holds
// kr_ws_ch.mu; it is released while waiting and held again on return.
static void
kr_ws_drain_dispatch_locked(void)
{
  while(kr_ws_ch.in_dispatch > 0)
    pthread_cond_wait(&kr_ws_ch.drain, &kr_ws_ch.mu);
}

// ------ per-channel parsers ------

// `data` is one entry out of the envelope's `data[]` array. The envelope
// contains per-frame metadata that's threaded in as `frame_time_ms`.

static void
kr_ws_dispatch_ticker(struct json_object *row, int64_t frame_time_ms)
{
  exchange_ws_event_t   ev = {0};
  exchange_ws_ticker_t *t  = &ev.payload.ticker;
  char                  symbol[EXCHANGE_PRODUCT_ID_SZ] = {0};

  if(!json_get_str(row, "symbol", symbol, sizeof(symbol)))
    return;

  kr_ws_symbol_to_abstraction(symbol);

  ev.channel = EXCH_WS_TICKER;
  snprintf(ev.product_id, sizeof(ev.product_id), "%s", symbol);
  snprintf(t->product_id, sizeof(t->product_id), "%s", symbol);

  t->price      = kr_ws_get_decimal(row, "last");
  t->best_bid   = kr_ws_get_decimal(row, "bid");
  t->best_ask   = kr_ws_get_decimal(row, "ask");
  t->volume_24h = kr_ws_get_decimal(row, "volume");
  t->low_24h    = kr_ws_get_decimal(row, "low");
  t->high_24h   = kr_ws_get_decimal(row, "high");
  t->time_ms    = frame_time_ms;

  kr_ws_fanout(&ev);
}

static void
kr_ws_dispatch_trade_row(struct json_object *row, int64_t frame_time_ms)
{
  exchange_ws_event_t  ev = {0};
  exchange_ws_match_t *m  = &ev.payload.match;
  char                 symbol[EXCHANGE_PRODUCT_ID_SZ] = {0};
  char                 side[EXCHANGE_SIDE_SZ]         = {0};
  char                 ts[64];

  if(!json_get_str(row, "symbol", symbol, sizeof(symbol)))
    return;

  kr_ws_symbol_to_abstraction(symbol);

  ev.channel = EXCH_WS_TRADES;
  snprintf(ev.product_id, sizeof(ev.product_id), "%s", symbol);
  snprintf(m->product_id, sizeof(m->product_id), "%s", symbol);

  if(json_get_str(row, "side", side, sizeof(side)))
  {
    snprintf(m->side, sizeof(m->side), "%s", side);
    kr_ws_lower_ascii(m->side);
  }

  m->price    = kr_ws_get_decimal(row, "price");
  m->size     = kr_ws_get_decimal(row, "qty");
  m->trade_id = kr_ws_get_int64_loose(row, "trade_id");

  if(json_get_str(row, "timestamp", ts, sizeof(ts)))
    m->time_ms = kr_ws_parse_iso8601_ms(ts);

  if(m->time_ms == 0)
    m->time_ms = frame_time_ms;

  kr_ws_fanout(&ev);
}

static void
kr_ws_dispatch_ohlc_row(struct json_object *row, int64_t frame_time_ms)
{
  exchange_ws_event_t  ev = {0};
  exchange_ws_ohlc_t  *o  = &ev.payload.ohlc;
  char                 symbol[EXCHANGE_PRODUCT_ID_SZ] = {0};
  char                 ts[64];
  int32_t              interval = 1;

  if(!json_get_str(row, "symbol", symbol, sizeof(symbol)))
    return;

  kr_ws_symbol_to_abstraction(symbol);

  ev.channel = EXCH_WS_OHLC_1M;
  snprintf(ev.product_id, sizeof(ev.product_id), "%s", symbol);
  snprintf(o->product_id, sizeof(o->product_id), "%s", symbol);

  o->open   = kr_ws_get_decimal(row, "open");
  o->high   = kr_ws_get_decimal(row, "high");
  o->low    = kr_ws_get_decimal(row, "low");
  o->close  = kr_ws_get_decimal(row, "close");
  o->volume = kr_ws_get_decimal(row, "volume");

  if(json_get_int(row, "interval", &interval))
    o->interval_min = (uint32_t)interval;
  else
    o->interval_min = 1;

  if(json_get_str(row, "interval_begin", ts, sizeof(ts)))
    o->ts_open_ms = kr_ws_parse_iso8601_ms(ts);

  if(json_get_str(row, "timestamp", ts, sizeof(ts)))
    o->time_ms = kr_ws_parse_iso8601_ms(ts);

  if(o->time_ms == 0)
    o->time_ms = frame_time_ms;

  if(o->ts_open_ms == 0)
    o->ts_open_ms = o->time_ms;

  kr_ws_fanout(&ev);
}

// Kraken v2 executions row carries the order lifecycle:
//   {"order_id":"...","symbol":"BTC/USD","side":"buy",
//    "order_status":"new|filled|partially_filled|canceled|expired",
//    "limit_price":"...","cum_qty":"...","leaves_qty":"...",
//    "avg_price":"...","fee_usd_equiv":"...",
//    "exec_type":"new|trade|canceled|expired|status",
//    "timestamp":"...","cl_ord_id":"..."}
//
// On exec_type=="trade" the row also carries a fill snapshot: trade_id,
// last_qty, last_price.
static void
kr_ws_dispatch_executions_row(struct json_object *row,
    int64_t frame_time_ms)
{
  char         exec_type[16] = {0};
  char         symbol[EXCHANGE_PRODUCT_ID_SZ] = {0};
  char         side[EXCHANGE_SIDE_SZ] = {0};
  char         ts[64];
  bool         is_fill;

  if(json_object_is_type(row, json_type_object) == 0)
    return;

  json_get_str(row, "exec_type", exec_type, sizeof(exec_type));
  json_get_str(row, "symbol",    symbol,    sizeof(symbol));

  kr_ws_symbol_to_abstraction(symbol);

  is_fill = (strcmp(exec_type, "trade") == 0);

  if(json_get_str(row, "side", side, sizeof(side)))
    kr_ws_lower_ascii(side);

  if(is_fill)
  {
    exchange_ws_event_t       ev  = {0};
    exchange_ws_user_event_t *up  = &ev.payload.user;
    exchange_ws_user_fill_t  *f   = &up->u.fill;

    ev.channel = EXCH_WS_USER;
    snprintf(ev.product_id, sizeof(ev.product_id), "%s", symbol);

    up->kind = EXCH_WS_USER_KIND_FILL;

    json_get_str(row, "order_id",  f->order_id,        sizeof(f->order_id));
    json_get_str(row, "cl_ord_id", f->client_order_id, sizeof(f->client_order_id));
    snprintf(f->product_id, sizeof(f->product_id), "%s", symbol);
    snprintf(f->side,       sizeof(f->side),       "%s", side);

    f->trade_id = kr_ws_get_int64_loose(row, "trade_id");
    f->price    = kr_ws_get_decimal(row, "last_price");
    if(f->price == 0.0)
      f->price  = kr_ws_get_decimal(row, "avg_price");

    f->size = kr_ws_get_decimal(row, "last_qty");
    if(f->size == 0.0)
      f->size = kr_ws_get_decimal(row, "exec_qty");

    f->fee  = kr_ws_get_decimal(row, "fee_usd_equiv");
    if(f->fee == 0.0)
      f->fee = kr_ws_get_decimal(row, "fees");

    if(json_get_str(row, "timestamp", ts, sizeof(ts)))
      f->time_ms = kr_ws_parse_iso8601_ms(ts);

    if(f->time_ms == 0)
      f->time_ms = frame_time_ms;

    kr_ws_fanout(&ev);
  }
  else
  {
    exchange_ws_event_t       ev  = {0};
    exchange_ws_user_event_t *up  = &ev.payload.user;
    exchange_ws_user_order_t *o   = &up->u.order;

    ev.channel = EXCH_WS_USER;
    snprintf(ev.product_id, sizeof(ev.product_id), "%s", symbol);

    up->kind = EXCH_WS_USER_KIND_ORDER;

    json_get_str(row, "order_id",     o->order_id,         sizeof(o->order_id));
    json_get_str(row, "cl_ord_id",    o->client_order_id,  sizeof(o->client_order_id));
    snprintf(o->product_id, sizeof(o->product_id), "%s", symbol);
    snprintf(o->side,       sizeof(o->side),       "%s", side);

    json_get_str(row, "order_status", o->status,           sizeof(o->status));
    kr_ws_lower_ascii(o->status);

    o->limit_price         = kr_ws_get_decimal(row, "limit_price");
    o->cumulative_quantity = kr_ws_get_decimal(row, "cum_qty");
    o->leaves_quantity     = kr_ws_get_decimal(row, "leaves_qty");
    o->avg_price           = kr_ws_get_decimal(row, "avg_price");
    o->total_fees          = kr_ws_get_decimal(row, "fee_usd_equiv");

    if(json_get_str(row, "timestamp", ts, sizeof(ts)))
      o->time_ms = kr_ws_parse_iso8601_ms(ts);

    if(o->time_ms == 0)
      o->time_ms = frame_time_ms;

    o->creation_time_ms = o->time_ms;

    kr_ws_fanout(&ev);
  }
}

// Balances events carry per-asset rolling totals. The neutral surface
// has no balances payload yet; surface as a USER ORDER event with empty
// status="balances_update" so consumers can ignore-or-handle. KR-5
// keeps the channel parsed for completeness but no consumer drives it.
static void
kr_ws_dispatch_balances_row(struct json_object *row,
    int64_t frame_time_ms)
{
  (void)row;
  (void)frame_time_ms;
  // No-op: account balances are reconciled from REST; surfaced here only
  // to log channel activity at DEBUG (already done by the dispatch
  // wrapper).
}

// Subscribe-ack handler. Body shape:
//   {"method":"subscribe","result":{"channel":"trade","symbol":"BTC/USD"},
//    "success":true,"req_id":N}
// or:
//   {"method":"subscribe","success":false,"error":"...","req_id":N}
//
// Subscribe and unsubscribe acks arrive on the same shape and route
// here alike, and the ack lands a round trip after the frame that
// earned it. The correlator names the slot by identity and the req_id
// guard below decides whether this ack is still the one that slot is
// waiting on — without it an unsubscribe ack forges ACTIVE onto
// whatever the table has since put in the emitter's place (OBS-5).
static void
kr_ws_handle_ack_locked(struct json_object *root)
{
  kr_ws_corr_entry_t ent;
  bool               success = false;
  int32_t            req_id  = 0;
  int32_t            slot_idx;
  char               err_str[128] = {0};

  json_get_bool (root, "success", &success);
  json_get_int  (root, "req_id",  &req_id);
  json_get_str  (root, "error",   err_str, sizeof(err_str));

  if(req_id == 0)
    return;

  ent = kr_ws_corr_pop_locked((uint32_t)req_id);

  if(ent.req_id == 0)
  {
    clam(CLAM_DEBUG, KR_CTX, "ws ack req_id=%d: no matching slot", req_id);
    return;
  }

  slot_idx = kr_ws_slot_find_locked(ent.channel, ent.symbol_ws);

  // The healthy outcome for every unsubscribe ack: kr_ws_unsubscribe
  // emits and compacts under one hold, so the slot is always gone by
  // the time its own ack arrives.
  if(slot_idx < 0)
  {
    clam(CLAM_DEBUG, KR_CTX, "ws ack req_id=%d: slot ch=%s sym=%s gone",
        req_id, kr_ws_channel_name(ent.channel), ent.symbol_ws);
    return;
  }

  kr_ws_slot_t *s = &kr_ws_ch.slots[slot_idx];

  // Only the ack for the slot's LAST subscribe emission may write it.
  // `req_id` is set on the subscribe side alone, so an unsubscribe ack
  // and a superseded subscribe ack both fall out here.
  if(s->req_id != (uint32_t)req_id)
  {
    clam(CLAM_DEBUG, KR_CTX,
        "ws ack req_id=%d: stale/foreign (slot ch=%s sym=%s holds %u)",
        req_id, kr_ws_channel_name(s->channel), s->symbol_ws, s->req_id);
    return;
  }

  if(success)
  {
    s->state         = KR_SUB_ACTIVE;
    s->sent_upstream = true;
    s->last_err[0]   = '\0';
    s->req_id        = 0;

    clam(CLAM_INFO, KR_CTX, "ws subscribe ack ch=%s sym=%s req_id=%d",
        kr_ws_channel_name(s->channel), s->symbol_ws, req_id);
  }
  else
  {
    s->state         = KR_SUB_FAILED;
    s->sent_upstream = false;
    s->req_id        = 0;
    snprintf(s->last_err, sizeof(s->last_err), "%s",
        err_str[0] ? err_str : "subscribe failed");

    clam(CLAM_WARN, KR_CTX,
        "ws subscribe FAIL ch=%s sym=%s req_id=%d err='%s'",
        kr_ws_channel_name(s->channel), s->symbol_ws, req_id, s->last_err);
  }
}

void
kr_ws_channels_dispatch(const char *buf, size_t len)
{
  struct json_object *root;
  struct json_object *data;
  char                channel_name[32] = {0};
  char                method[32]       = {0};
  char                ts[64];
  int64_t             frame_time_ms = 0;
  size_t              n;
  size_t              i;
  kr_ws_channel_t     ch;

  if(!kr_ws_ch.initialized || buf == NULL || len == 0) return;

  root = json_parse_buf(buf, len, KR_CTX ":ws_recv");
  if(root == NULL) return;

  // Heartbeat: {"channel":"heartbeat"} — silent liveness signal. The
  // transport layer already counts any frame as liveness proof.
  if(json_get_str(root, "channel", channel_name, sizeof(channel_name))
      && strcmp(channel_name, "heartbeat") == 0)
  {
    json_object_put(root);
    return;
  }

  // Status frames carry connection/system state. Log at DEBUG and move
  // on; no consumer surface yet.
  if(channel_name[0] != '\0' && strcmp(channel_name, "status") == 0)
  {
    clam(CLAM_DEBUG, KR_CTX, "ws status frame received");
    json_object_put(root);
    return;
  }

  // Method frames are subscribe / unsubscribe acks (or top-level errors
  // that carry a `method` key).
  if(json_get_str(root, "method", method, sizeof(method))
      && (strcmp(method, "subscribe") == 0
          || strcmp(method, "unsubscribe") == 0))
  {
    pthread_mutex_lock(&kr_ws_ch.mu);
    kr_ws_handle_ack_locked(root);
    pthread_mutex_unlock(&kr_ws_ch.mu);

    json_object_put(root);
    return;
  }

  // Top-level error frames not associated with an ack.
  {
    char err_str[256] = {0};

    if(json_get_str(root, "error", err_str, sizeof(err_str)))
    {
      clam(CLAM_WARN, KR_CTX, "ws server error: %s", err_str);
      json_object_put(root);
      return;
    }
  }

  if(channel_name[0] == '\0')
  {
    json_object_put(root);
    return;
  }

  ch = kr_ws_channel_parse(channel_name);

  if(ch == KR_CH__COUNT)
  {
    clam(CLAM_DEBUG3, KR_CTX, "ws ignoring channel=%s", channel_name);
    json_object_put(root);
    return;
  }

  if(json_get_str(root, "timestamp", ts, sizeof(ts)))
    frame_time_ms = kr_ws_parse_iso8601_ms(ts);

  data = json_get_array(root, "data");

  if(data == NULL)
  {
    json_object_put(root);
    return;
  }

  n = json_object_array_length(data);

  // No lock here: the parsers below read the frame and nothing else,
  // and kr_ws_fanout takes kr_ws_ch.mu for exactly as long as it needs
  // the subscriber list (SAN-10). The ack handler above keeps its own
  // hold — it is the one path that touches the slot table.
  for(i = 0; i < n; i++)
  {
    struct json_object *row = json_object_array_get_idx(data, i);

    if(row == NULL) continue;

    switch(ch)
    {
      case KR_CH_TICKER:
        kr_ws_dispatch_ticker(row, frame_time_ms);
        break;
      case KR_CH_TRADE:
        kr_ws_dispatch_trade_row(row, frame_time_ms);
        break;
      case KR_CH_OHLC:
        kr_ws_dispatch_ohlc_row(row, frame_time_ms);
        break;
      case KR_CH_EXECUTIONS:
        kr_ws_dispatch_executions_row(row, frame_time_ms);
        break;
      case KR_CH_BALANCES:
        kr_ws_dispatch_balances_row(row, frame_time_ms);
        break;
      case KR_CH__COUNT:
        break;
    }
  }

  json_object_put(root);
}

// ------------------------------------------------------------------ //
// On-open hook                                                        //
// ------------------------------------------------------------------ //

// One session flapped and came back. Everything here is scoped to THAT
// session: the sibling's gateway never forgot anything, and resetting
// its live slots would re-emit subscribes for feeds that are still
// streaming.
void
kr_ws_channels_on_open(kr_ws_session_id_t sid)
{
  bool need_token = false;

  if(!kr_ws_ch.initialized) return;

  pthread_mutex_lock(&kr_ws_ch.mu);

  if(kr_ws_ch.n_slots == 0)
  {
    pthread_mutex_unlock(&kr_ws_ch.mu);
    return;
  }

  // Reset state on this session's slots — its gateway forgot us across
  // the flap.
  for(uint32_t i = 0; i < kr_ws_ch.n_slots; i++)
  {
    kr_ws_slot_t *s = &kr_ws_ch.slots[i];

    if(kr_ws_session_for_channel(s->channel) != sid)
      continue;

    s->sent_upstream = false;
    s->req_id        = 0;

    if(s->refcount > 0)
      s->state = KR_SUB_IDLE;
  }

  // Discard this session's pending acks from the connection that just
  // died; the sibling's are still in flight and answerable. Clearing the
  // req_id empties the entry; the identity beside it is never read
  // without one, and it is that identity which says whose entry it is.
  for(uint32_t i = 0; i < KR_WS_CH_REQ_RING_SIZE; i++)
  {
    if(kr_ws_ch.corr_ring[i].req_id != 0
        && kr_ws_session_for_channel(kr_ws_ch.corr_ring[i].channel) == sid)
      kr_ws_ch.corr_ring[i].req_id = 0;
  }

  if(sid == KR_WS_PRIVATE)
    need_token = kr_ws_has_private_pending_locked();

  if(need_token)
  {
    char token[KR_WS_TOKEN_SZ] = {0};

    if(kr_ws_token_snapshot(token, sizeof(token)) == SUCCESS)
    {
      kr_ws_reconcile_locked(KR_RECONCILE_SUB, token, (int)sid);
      pthread_mutex_unlock(&kr_ws_ch.mu);
      return;
    }

    // No fresh token — kick a fetch and reconcile from the trampoline
    // when it lands. Nothing on this session can flush without one.
    pthread_mutex_unlock(&kr_ws_ch.mu);
    (void)kr_ws_token_acquire(kr_ws_token_then_reconcile_cb, NULL);
    return;
  }

  // Public session, or a private one with nothing pending: no token is
  // owed either way. Reconcile inline.
  kr_ws_reconcile_locked(KR_RECONCILE_SUB, NULL, (int)sid);
  pthread_mutex_unlock(&kr_ws_ch.mu);
}

// Whether the transport should hold this session open at all. Market
// data is the reason the subsystem exists, so the public session is
// wanted whenever WS is enabled; the private one is wanted only while
// credentials exist and something is actually subscribed to it — an
// authenticated session with nothing on it earns nothing.
//
// Deliberately NOT kr_ws_has_private_pending_locked: that answers "is
// there a subscribe still to send", which goes false the moment the ack
// lands. An ACTIVE private slot is exactly the case that must keep the
// session up.
bool
kr_ws_channels_session_wanted(kr_ws_session_id_t sid)
{
  bool wanted = false;

  if(sid == KR_WS_PUBLIC)
    return(true);

  if(!kr_ws_ch.initialized || !kr_apikey_configured())
    return(false);

  pthread_mutex_lock(&kr_ws_ch.mu);

  for(uint32_t i = 0; i < kr_ws_ch.n_slots; i++)
  {
    if(kr_ws_ch.slots[i].refcount > 0
        && kr_ws_session_for_channel(kr_ws_ch.slots[i].channel) == sid)
    {
      wanted = true;
      break;
    }
  }

  pthread_mutex_unlock(&kr_ws_ch.mu);

  return(wanted);
}

// ------------------------------------------------------------------ //
// Channel mapping helpers (public API translation)                    //
// ------------------------------------------------------------------ //

// Translate a public exchange channel into the Kraken-internal channel
// list. EXCH_WS_USER expands to BOTH executions + balances. Returns the
// number of internal channels written (0 if unsupported).
static uint32_t
kr_ws_channel_from_exch(exchange_ws_channel_t in,
    kr_ws_channel_t *out, uint32_t cap)
{
  if(out == NULL || cap == 0) return(0);

  switch(in)
  {
    case EXCH_WS_TICKER:
      out[0] = KR_CH_TICKER;
      return(1);
    case EXCH_WS_TRADES:
      out[0] = KR_CH_TRADE;
      return(1);
    case EXCH_WS_OHLC_1M:
      out[0] = KR_CH_OHLC;
      return(1);
    case EXCH_WS_USER:
      if(cap < 2) return(0);
      out[0] = KR_CH_EXECUTIONS;
      out[1] = KR_CH_BALANCES;
      return(2);
    case EXCH_WS_BOOK_L2:
    default:
      return(0);
  }
}

// ------------------------------------------------------------------ //
// Public API: kr_ws_subscribe / kr_ws_unsubscribe                     //
// ------------------------------------------------------------------ //

bool
kr_ws_subscribe(const exchange_ws_channel_t *channels, uint32_t n_channels,
    const char *const *product_ids, uint32_t n_products,
    exchange_ws_event_cb_t cb, void *user, void **out_handle)
{
  kr_ws_sub_t            *sub;
  uint32_t                channel_mask     = 0;
  bool                    has_private      = false;
  bool                    has_per_symbol   = false;
  uint32_t                i;

  if(out_handle == NULL)
    return(FAIL);

  *out_handle = NULL;

  if(!kr_ws_ch.initialized)
  {
    clam(CLAM_WARN, KR_CTX, "ws subscribe: multiplexer not initialized");
    return(FAIL);
  }

  if(cb == NULL || channels == NULL || n_channels == 0)
  {
    clam(CLAM_WARN, KR_CTX, "ws subscribe: invalid args");
    return(FAIL);
  }

  if(n_products > KR_WS_CH_MAX_PRODUCTS_PER_SUB)
  {
    clam(CLAM_WARN, KR_CTX,
        "ws subscribe: too many products (%u > %d)",
        n_products, KR_WS_CH_MAX_PRODUCTS_PER_SUB);
    return(FAIL);
  }

  // The channel_mask is in the ABSTRACT enum's bit-space (matches
  // ev->channel at fanout time). Each abstract channel separately
  // expands into one or more internal kr_ws_channel_t values for the
  // slot-table walk below — different enum, different mask. A USER
  // sub takes one bit (1<<EXCH_WS_USER) but creates two slots
  // (executions + balances).
  for(i = 0; i < n_channels; i++)
  {
    kr_ws_channel_t mapped[2];
    uint32_t        n_mapped;
    uint32_t        j;

    n_mapped = kr_ws_channel_from_exch(channels[i], mapped,
        sizeof(mapped) / sizeof(mapped[0]));

    if(n_mapped == 0)
    {
      clam(CLAM_WARN, KR_CTX,
          "ws subscribe: channel %d not supported on Kraken",
          (int)channels[i]);
      return(FAIL);
    }

    channel_mask |= (1u << channels[i]);

    for(j = 0; j < n_mapped; j++)
    {
      if(kr_ws_channel_is_private(mapped[j]))
        has_private = true;
      else
        has_per_symbol = true;
    }
  }

  if(has_per_symbol && n_products == 0)
  {
    clam(CLAM_WARN, KR_CTX,
        "ws subscribe: per-symbol channel without product_ids");
    return(FAIL);
  }

  if(has_private && !kr_apikey_configured())
  {
    clam(CLAM_WARN, KR_CTX,
        "ws subscribe: private channel requested but credentials not configured");
    return(FAIL);
  }

  pthread_mutex_lock(&kr_ws_ch.mu);

  if(kr_ws_ch.n_subs >= KR_WS_CH_MAX_SUBS)
  {
    pthread_mutex_unlock(&kr_ws_ch.mu);
    clam(CLAM_WARN, KR_CTX, "ws subscribe: sub table full (%u)",
        kr_ws_ch.n_subs);
    return(FAIL);
  }

  sub = mem_alloc(KR_CTX, "ws_sub", sizeof(*sub));

  memset(sub, 0, sizeof(*sub));
  sub->id           = ++kr_ws_ch.next_id;
  sub->cb           = cb;
  sub->user         = user;
  sub->channel_mask = channel_mask;
  sub->n_products   = n_products;

  for(i = 0; i < n_products; i++)
  {
    if(product_ids[i] == NULL || product_ids[i][0] == '\0')
    {
      pthread_mutex_unlock(&kr_ws_ch.mu);
      mem_free(sub);
      clam(CLAM_WARN, KR_CTX,
          "ws subscribe: empty product_id at idx %u", i);
      return(FAIL);
    }
    snprintf(sub->products[i], sizeof(sub->products[i]), "%s",
        product_ids[i]);
  }

  sub->next        = kr_ws_ch.head;
  kr_ws_ch.head    = sub;
  kr_ws_ch.n_subs++;

  // Refcount every (internal-channel, symbol) pair this sub covers.
  // Walk the input abstract channel list and expand each to its
  // internal kr_ws_channel_t equivalents — EXCH_WS_USER expands to
  // {executions, balances}. Public channels: one slot per (channel,
  // product). Private channels: one global slot per channel.
  for(i = 0; i < n_channels; i++)
  {
    kr_ws_channel_t mapped[2];
    uint32_t        n_mapped;
    uint32_t        j;

    n_mapped = kr_ws_channel_from_exch(channels[i], mapped,
        sizeof(mapped) / sizeof(mapped[0]));

    for(j = 0; j < n_mapped; j++)
    {
      kr_ws_channel_t kch = mapped[j];

      if(kr_ws_channel_is_per_symbol(kch))
      {
        uint32_t p;

        for(p = 0; p < n_products; p++)
        {
          char    sym_ws[EXCHANGE_PRODUCT_ID_SZ];
          int32_t idx;

          kr_pair_lookup_ws(sub->products[p], sym_ws, sizeof(sym_ws));

          idx = kr_ws_slot_find_locked(kch, sym_ws);

          if(idx < 0)
          {
            kr_ws_slot_t *sl = kr_ws_slot_alloc_locked(kch, sym_ws);

            if(sl == NULL)
            {
              clam(CLAM_WARN, KR_CTX,
                  "ws subscribe: slot table full, skipping %s/%s",
                  kr_ws_channel_name(kch), sym_ws);
              continue;
            }
            idx = (int32_t)(sl - kr_ws_ch.slots);
          }

          kr_ws_ch.slots[idx].refcount++;
        }
      }
      else
      {
        int32_t idx = kr_ws_slot_find_locked(kch, "");

        if(idx < 0)
        {
          kr_ws_slot_t *sl = kr_ws_slot_alloc_locked(kch, "");

          if(sl == NULL)
          {
            clam(CLAM_WARN, KR_CTX,
                "ws subscribe: slot table full, skipping ch=%s",
                kr_ws_channel_name(kch));
            continue;
          }
          idx = (int32_t)(sl - kr_ws_ch.slots);
        }

        kr_ws_ch.slots[idx].refcount++;
      }
    }
  }

  // Reconcile. If private slots are pending and we don't have a fresh
  // token, kick the token fetch + reconcile from the trampoline.
  if(has_private)
  {
    char token[KR_WS_TOKEN_SZ] = {0};

    if(kr_ws_token_snapshot(token, sizeof(token)) == SUCCESS
        && !kr_ws_token_needs_refresh())
    {
      // KR_WS_SID_ANY: this sub may cover both classes at once. An
      // emit for a session that is not open fails and its slot waits
      // for that session's own on-open pass.
      kr_ws_reconcile_locked(KR_RECONCILE_SUB, token, KR_WS_SID_ANY);
      pthread_mutex_unlock(&kr_ws_ch.mu);
    }
    else
    {
      pthread_mutex_unlock(&kr_ws_ch.mu);
      (void)kr_ws_token_acquire(kr_ws_token_then_reconcile_cb, NULL);
    }
  }
  else
  {
    kr_ws_reconcile_locked(KR_RECONCILE_SUB, NULL, KR_WS_SID_ANY);
    pthread_mutex_unlock(&kr_ws_ch.mu);
  }

  *out_handle = sub;

  clam(CLAM_INFO, KR_CTX,
      "ws subscribe id=%u channels=0x%04x products=%u",
      sub->id, channel_mask, n_products);

  return(SUCCESS);
}

void
kr_ws_unsubscribe(void *driver_sub)
{
  kr_ws_sub_t  *handle = driver_sub;
  kr_ws_sub_t **pp;
  uint32_t      sub_id;
  uint32_t      i;

  if(handle == NULL || !kr_ws_ch.initialized) return;

  pthread_mutex_lock(&kr_ws_ch.mu);

  // Unlink from the global list.
  for(pp = &kr_ws_ch.head; *pp != NULL; pp = &(*pp)->next)
  {
    if(*pp == handle)
    {
      *pp = handle->next;
      kr_ws_ch.n_subs--;
      break;
    }
  }

  sub_id = handle->id;

  // Unlinked above, so no further fan-out can pick this sub up; wait
  // out the deliveries already running before the handle — and the
  // consumer's `user` behind it — goes away. SAN-10: the fan-out no
  // longer holds `mu`, so the unlink alone is not the barrier it used
  // to be.
  kr_ws_drain_dispatch_locked();

  // Decrement refcounts on every slot this sub held. Walk the abstract
  // channel mask, expand each abstract channel to its internal
  // kr_ws_channel_t list, and decrement the matching slot.
  for(int ach = EXCH_WS_TICKER; ach <= EXCH_WS_USER; ach++)
  {
    kr_ws_channel_t mapped[2];
    uint32_t        n_mapped;
    uint32_t        j;

    if(!(handle->channel_mask & (1u << ach))) continue;

    n_mapped = kr_ws_channel_from_exch((exchange_ws_channel_t)ach,
        mapped, sizeof(mapped) / sizeof(mapped[0]));

    for(j = 0; j < n_mapped; j++)
    {
      kr_ws_channel_t kch = mapped[j];

      if(kr_ws_channel_is_per_symbol(kch))
      {
        for(i = 0; i < handle->n_products; i++)
        {
          char    sym_ws[EXCHANGE_PRODUCT_ID_SZ];
          int32_t idx;

          kr_pair_lookup_ws(handle->products[i], sym_ws, sizeof(sym_ws));

          idx = kr_ws_slot_find_locked(kch, sym_ws);

          if(idx >= 0 && kr_ws_ch.slots[idx].refcount > 0)
            kr_ws_ch.slots[idx].refcount--;
        }
      }
      else
      {
        int32_t idx = kr_ws_slot_find_locked(kch, "");

        if(idx >= 0 && kr_ws_ch.slots[idx].refcount > 0)
          kr_ws_ch.slots[idx].refcount--;
      }
    }
  }

  // Unsubscribe walk — public channels only need a token-less frame;
  // private channels need the cached token. We bypass the strict needs-
  // refresh check (a stale token is still acceptable for unsubscribe;
  // worst case the gateway already forgot the slot).
  {
    char token[KR_WS_TOKEN_SZ] = {0};

    (void)kr_ws_token_snapshot(token, sizeof(token));
    kr_ws_reconcile_locked(KR_RECONCILE_UNSUB, token[0] ? token : NULL,
        KR_WS_SID_ANY);
  }

  // Reap empty slots.
  kr_ws_slots_compact_locked();

  mem_free(handle);

  pthread_mutex_unlock(&kr_ws_ch.mu);

  clam(CLAM_INFO, KR_CTX, "ws unsubscribe id=%u", sub_id);
}

// ------------------------------------------------------------------ //
// Lifecycle                                                           //
// ------------------------------------------------------------------ //

void
kr_ws_channels_init(void)
{
  if(kr_ws_ch.initialized) return;

  memset(&kr_ws_ch, 0, sizeof(kr_ws_ch));
  pthread_mutex_init(&kr_ws_ch.mu, NULL);
  pthread_cond_init(&kr_ws_ch.drain, NULL);
  atomic_store(&kr_ws_ch.next_req_id, 0u);

  // The memset above is the ring's reset: a zero req_id is the empty
  // entry, and nothing reads the identity beside it without one.

  kr_ws_ch.initialized = true;

  clam(CLAM_DEBUG, KR_CTX, "ws channel multiplexer initialized");
}

void
kr_ws_channels_deinit(void)
{
  kr_ws_sub_t *s;
  kr_ws_sub_t *n;

  if(!kr_ws_ch.initialized) return;

  pthread_mutex_lock(&kr_ws_ch.mu);

  // Drop the list first so nothing new fans out, then wait out what is
  // already inside a consumer callback — freeing a sub under a live
  // delivery is the same hazard kr_ws_unsubscribe drains for.
  s = kr_ws_ch.head;

  kr_ws_ch.head    = NULL;
  kr_ws_ch.n_subs  = 0;
  kr_ws_ch.n_slots = 0;

  kr_ws_drain_dispatch_locked();

  while(s != NULL)
  {
    n = s->next;
    mem_free(s);
    s = n;
  }

  pthread_mutex_unlock(&kr_ws_ch.mu);

  pthread_cond_destroy(&kr_ws_ch.drain);
  pthread_mutex_destroy(&kr_ws_ch.mu);
  kr_ws_ch.initialized = false;

  clam(CLAM_DEBUG, KR_CTX, "ws channel multiplexer torn down");
}
