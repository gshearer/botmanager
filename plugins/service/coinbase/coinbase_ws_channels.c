// botmanager — MIT
// Coinbase Advanced Trade: WebSocket channel multiplexer (CB5).
//
// Layered on top of CB4's single-session transport. Owns the list of
// local subscribers and a dedup table of (channel, product) slots so
// that N consumers watching the same feed share one upstream slot.
// When a slot's refcount reaches zero the slot is unsubscribed
// upstream. On reconnect the slot table drives a full resubscribe so
// consumer callbacks never miss a beat across a flap.
//
// Wire format is Advanced Trade: one channel per subscribe frame,
//   {"type":"subscribe","product_ids":["BTC-USD"],
//    "channel":"ticker","jwt":"<jwt>"}
// — every subscribe (public or private) requires a fresh CDP JWT.
// Inbound events are wrapped in an envelope:
//   {"channel":"<name>","timestamp":"...","sequence_num":N,
//    "events":[{...}]}
// where the per-event payload depends on the channel.
//
// Sequence gap detection is intentionally not performed. The
// envelope's `sequence_num` is per-channel-per-connection (advances on
// every frame the gateway sends us) — useful for connection-level
// integrity but not directly comparable across consumer subsets. The
// `coinbase_ws_event_t.gap` field is preserved as an ABI placeholder
// and remains false.

#define CB_INTERNAL
#include "coinbase.h"

#include "json.h"

#include <json-c/json.h>

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// Capacity limits. The bounds are intentionally generous relative to
// whenmoon's foreseeable needs (≤ a handful of products across a few
// channels) while keeping a single subscribe / unsubscribe JSON frame
// comfortably under CB_WS_CH_TX_BUF_SZ.
#define CB_WS_CH_MAX_SUBS               64
#define CB_WS_CH_MAX_PRODUCTS_PER_SUB   16
#define CB_WS_CH_MAX_SLOTS             128
#define CB_WS_CH_TX_BUF_SZ            8192

// Subscription handle. One per coinbase_ws_subscribe() call; freed by
// coinbase_ws_unsubscribe(). `channel_mask` is a bitmask over
// coinbase_ws_channel_t so membership tests in the fanout loop are
// single-op.
struct coinbase_ws_sub
{
  uint32_t                 id;
  coinbase_ws_event_cb_t   cb;
  void                    *user;

  uint32_t                 channel_mask;
  uint32_t                 n_products;
  char                     products[CB_WS_CH_MAX_PRODUCTS_PER_SUB]
                                   [COINBASE_PRODUCT_ID_SZ];

  struct coinbase_ws_sub  *next;
};

// Per (channel, product) dedup slot. `product_id` is the empty string
// for channels that aren't product-keyed (status). `sent_upstream`
// tracks whether the server currently has a live subscribe for this
// pair — it flips to false on reconnect so the next OPEN hook re-emits
// all live slots.
typedef struct
{
  coinbase_ws_channel_t channel;
  char                  product_id[COINBASE_PRODUCT_ID_SZ];
  uint32_t              refcount;
  bool                  sent_upstream;
} cb_ws_slot_t;

static struct
{
  pthread_mutex_t         mu;
  struct coinbase_ws_sub *head;
  uint32_t                next_id;
  uint32_t                n_subs;

  cb_ws_slot_t            slots[CB_WS_CH_MAX_SLOTS];
  uint32_t                n_slots;

  // Subscribe-ack watchdog stamps (mu-guarded, CLOCK_MONOTONIC). The
  // gateway can keep a socket ping-pong-alive while silently ignoring
  // subscribes (INCIDENTS.md 2026-07-23) — the transport's idle
  // watchdog never fires in that state, so it compares these instead:
  // a subscribe send that outwaits CB_WS_SUB_ACK_TIMEOUT_MS with no
  // "subscriptions" ack forces a reconnect.
  uint64_t                last_sub_sent_ms;
  uint64_t                last_ack_ms;

  // SAN-10: consumer callbacks run with `mu` RELEASED (see cb_ws_fanout),
  // so the lock no longer answers "is a callback running?". This counter
  // does, and it is what keeps coinbase_ws_unsubscribe's contract — it
  // returns only once nothing is inside a consumer's callback. Both are
  // mu-guarded; `drain` is broadcast as the count reaches zero.
  uint32_t                in_dispatch;
  pthread_cond_t          drain;

  bool                    initialized;
} cb_ws_ch;

// ----------------------------------------------------------------------
// Channel metadata
// ----------------------------------------------------------------------

// Channel-name mapping uses Advanced Trade names on the wire. The
// public ABI enum still carries CB_FULL / CB_LEVEL2_BATCH for legacy
// source compatibility, but Advanced Trade has no equivalent — those
// slots return NULL and are silently skipped at subscribe time.
static const char *
cb_ws_channel_name(coinbase_ws_channel_t ch)
{
  switch(ch)
  {
    case COINBASE_CH_HEARTBEAT:    return("heartbeats");
    case COINBASE_CH_STATUS:       return("status");
    case COINBASE_CH_TICKER:       return("ticker");
    case COINBASE_CH_TICKER_BATCH: return("ticker_batch");
    case COINBASE_CH_LEVEL2:       return("level2");
    case COINBASE_CH_MATCHES:      return("market_trades");
    case COINBASE_CH_USER:         return("user");
    case COINBASE_CH_LEVEL2_BATCH: return(NULL);
    case COINBASE_CH_FULL:         return(NULL);
    case COINBASE_CH__COUNT:       break;
  }
  return(NULL);
}

// The user channel is the only one Coinbase requires a JWT for.
static bool
cb_ws_channel_requires_auth(coinbase_ws_channel_t ch)
{
  return(ch == COINBASE_CH_USER);
}

static uint64_t
cb_ws_ch_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);

  return(((uint64_t)ts.tv_sec * 1000u) + ((uint64_t)ts.tv_nsec / 1000000u));
}

static bool
cb_ws_channel_is_per_product(coinbase_ws_channel_t ch)
{
  // Advanced Trade requires `product_ids` on every subscribe, including
  // the channels that *emit* global events (heartbeats fires whenever
  // any subscribed product has activity; status emits product metadata
  // for the requested products). Slot-keying still uses the product
  // id; heartbeats/status events arrive without a product_id field so
  // fanout sees ev->product_id == NULL and matches any subscriber.
  (void)ch;
  return(true);
}

// Coinbase timestamps are ISO 8601 with optional fractional seconds:
// "2017-09-02T17:05:49.250000Z". Parse to milliseconds-since-epoch. On
// parse failure return 0 — callers that need precise timing should
// stamp against their own monotonic clock anyway.
static int64_t
cb_ws_parse_iso8601_ms(const char *s)
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
    long   ns       = 0;
    int    digits   = 0;
    int    i;

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

// ----------------------------------------------------------------------
// Slot table — caller holds cb_ws_ch.mu
// ----------------------------------------------------------------------

static int32_t
cb_ws_slot_find_locked(coinbase_ws_channel_t ch, const char *product_id)
{
  const char *pid = (product_id != NULL) ? product_id : "";

  for(uint32_t i = 0; i < cb_ws_ch.n_slots; i++)
  {
    cb_ws_slot_t *s = &cb_ws_ch.slots[i];

    if(s->channel == ch && strcmp(s->product_id, pid) == 0)
      return((int32_t)i);
  }

  return(-1);
}

static cb_ws_slot_t *
cb_ws_slot_alloc_locked(coinbase_ws_channel_t ch, const char *product_id)
{
  cb_ws_slot_t *s;

  if(cb_ws_ch.n_slots >= CB_WS_CH_MAX_SLOTS)
    return(NULL);

  s = &cb_ws_ch.slots[cb_ws_ch.n_slots++];

  memset(s, 0, sizeof(*s));
  s->channel = ch;

  snprintf(s->product_id, sizeof(s->product_id), "%s",
      (product_id != NULL) ? product_id : "");

  return(s);
}

// Drop slots whose refcount hit zero after an unsubscribe emission.
// Swap-with-last is safe because the slot table has no stable index
// exposed externally — subscribers hold no slot indices.
static void
cb_ws_slots_compact_locked(void)
{
  for(uint32_t i = 0; i < cb_ws_ch.n_slots; )
  {
    if(cb_ws_ch.slots[i].refcount == 0)
    {
      cb_ws_ch.slots[i] = cb_ws_ch.slots[cb_ws_ch.n_slots - 1];
      cb_ws_ch.n_slots--;
    }
    else
      i++;
  }
}

// ----------------------------------------------------------------------
// Frame rendering
// ----------------------------------------------------------------------

// Render one Advanced Trade subscribe / unsubscribe frame for a single
// channel, listing every product whose slot passes `include_slot`.
// `jwt` may be NULL — public channels are valid unsigned and the jwt
// field is then omitted entirely (never sent empty: Coinbase rejects
// malformed auth). When non-NULL it is embedded verbatim and must
// already be minted. Returns bytes written, or 0 when no slot
// qualifies for `channel` under `pred` (caller skips the frame).
// Channel name, product ids, and jwt are JSON-safe by construction
// (allowlisted enums, base64url, hex).
typedef bool (*cb_ws_slot_pred_t)(const cb_ws_slot_t *);

static size_t
cb_ws_render_frame_locked(char *out, size_t cap, const char *type,
    coinbase_ws_channel_t channel, cb_ws_slot_pred_t include_slot,
    const char *jwt)
{
  const char *cname;
  size_t      pos       = 0;
  bool        per_product;
  bool        first_pid = true;
  bool        any       = false;
  int         n;

  if(out == NULL || cap == 0)
    return(0);

  cname = cb_ws_channel_name(channel);
  if(cname == NULL) return(0);

  per_product = cb_ws_channel_is_per_product(channel);

  n = snprintf(out + pos, cap - pos, "{\"type\":\"%s\"", type);
  if(n < 0 || (size_t)n >= cap - pos) return(0);
  pos += (size_t)n;

  if(per_product)
  {
    n = snprintf(out + pos, cap - pos, ",\"product_ids\":[");
    if(n < 0 || (size_t)n >= cap - pos) return(0);
    pos += (size_t)n;

    for(uint32_t i = 0; i < cb_ws_ch.n_slots; i++)
    {
      cb_ws_slot_t *s = &cb_ws_ch.slots[i];

      if(s->channel != channel) continue;
      if(!include_slot(s))      continue;

      n = snprintf(out + pos, cap - pos, "%s\"%s\"",
          first_pid ? "" : ",", s->product_id);
      if(n < 0 || (size_t)n >= cap - pos) return(0);
      pos += (size_t)n;

      first_pid = false;
      any       = true;
    }

    if(!any) return(0);

    n = snprintf(out + pos, cap - pos, "]");
    if(n < 0 || (size_t)n >= cap - pos) return(0);
    pos += (size_t)n;
  }
  else
  {
    for(uint32_t i = 0; i < cb_ws_ch.n_slots; i++)
    {
      cb_ws_slot_t *s = &cb_ws_ch.slots[i];

      if(s->channel != channel) continue;
      if(!include_slot(s))      continue;
      any = true;
      break;
    }

    if(!any) return(0);
  }

  if(jwt != NULL)
    n = snprintf(out + pos, cap - pos,
        ",\"channel\":\"%s\",\"jwt\":\"%s\"}", cname, jwt);
  else
    n = snprintf(out + pos, cap - pos, ",\"channel\":\"%s\"}", cname);

  if(n < 0 || (size_t)n >= cap - pos) return(0);
  pos += (size_t)n;

  return(pos);
}

// ----------------------------------------------------------------------
// Slot predicates
// ----------------------------------------------------------------------

static bool cb_ws_pred_needs_sub(const cb_ws_slot_t *s)
{
  return(s->refcount > 0 && !s->sent_upstream);
}

static bool cb_ws_pred_needs_unsub(const cb_ws_slot_t *s)
{
  return(s->refcount == 0 && s->sent_upstream);
}

// ----------------------------------------------------------------------
// Reconcile — emit subscribe / unsubscribe frames for pending deltas
// ----------------------------------------------------------------------

// True when any slot on `ch` has a pending delta under `pred`.
static bool
cb_ws_delta_pending_locked(coinbase_ws_channel_t ch, cb_ws_slot_pred_t pred)
{
  for(uint32_t i = 0; i < cb_ws_ch.n_slots; i++)
  {
    const cb_ws_slot_t *s = &cb_ws_ch.slots[i];

    if(s->channel == ch && pred(s))
      return(true);
  }

  return(false);
}

// One frame per channel. Mints a single JWT covering the whole batch
// (Advanced Trade accepts the same JWT on every subscribe within its
// 120 s lifetime; minting once amortises ECDSA signing overhead in the
// resubscribe-on-reconnect case). A failed mint no longer aborts the
// batch: public channels go out unsigned (Coinbase requires auth only
// on the user channel) and auth channels are held — their slots keep
// needs_sub and retry on the next delta / reconnect once creds exist.
// A creds loss therefore degrades to "paper feed alive, auth channels
// held" instead of darkening everything (CB-WS-PUB-1).
static void
cb_ws_send_delta_locked(const char *op, cb_ws_slot_pred_t pred,
    bool new_sent_state)
{
  char        frame[CB_WS_CH_TX_BUF_SZ];
  char        jwt[CB_JWT_SZ];
  char        held[128]  = {0};
  size_t      held_len   = 0;
  const char *jwt_p      = jwt;
  bool        subscribing;
  size_t      len;
  bool        ok;

  subscribing = (strcmp(op, "subscribe") == 0);

  if(cb_sign_jwt_ws(jwt, sizeof(jwt)) != SUCCESS)
    jwt_p = NULL;

  for(int ch = 0; ch < COINBASE_CH__COUNT; ch++)
  {
    coinbase_ws_channel_t cch = (coinbase_ws_channel_t)ch;

    if(jwt_p == NULL && cb_ws_channel_requires_auth(cch))
    {
      const char *cname = cb_ws_channel_name(cch);
      int         n;

      if(cname == NULL)                          continue;
      if(!cb_ws_delta_pending_locked(cch, pred)) continue;

      // Held: slot untouched, stays pending, retries once creds exist.
      n = snprintf(held + held_len, sizeof(held) - held_len, "%s%s",
          held_len > 0 ? "," : "", cname);

      if(n > 0 && (size_t)n < sizeof(held) - held_len)
        held_len += (size_t)n;

      continue;
    }

    len = cb_ws_render_frame_locked(frame, sizeof(frame), op, cch, pred,
        jwt_p);
    if(len == 0) continue;

    ok = (cb_ws_send_json(frame, len) == SUCCESS);

    if(ok)
    {
      for(uint32_t i = 0; i < cb_ws_ch.n_slots; i++)
      {
        cb_ws_slot_t *s = &cb_ws_ch.slots[i];

        if(s->channel != cch) continue;
        if(!pred(s))          continue;

        s->sent_upstream = new_sent_state;
      }

      if(subscribing)
        cb_ws_ch.last_sub_sent_ms = cb_ws_ch_now_ms();

      clam(CLAM_INFO, CB_CTX, "ws %s ch=%s (%zu bytes)", op,
          cb_ws_channel_name(cch), len);
    }
    else
    {
      clam(CLAM_DEBUG, CB_CTX,
          "ws %s ch=%s held: session not open (will retry on open)",
          op, cb_ws_channel_name(cch));
    }
  }

  if(held_len > 0)
    clam(CLAM_WARN, CB_CTX,
        "ws %s: jwt mint failed (creds missing?) — public channels sent"
        " unsigned, auth channel(s) held: %s", op, held);

  else if(jwt_p == NULL)
    clam(CLAM_DEBUG, CB_CTX,
        "ws %s: jwt mint failed; all-public batch sent unsigned", op);
}

// ----------------------------------------------------------------------
// Dispatch: parse the payload and fan out to every matching subscriber
// ----------------------------------------------------------------------

// Deliver one event to every sub whose channel/product set covers the
// event. Takes cb_ws_ch.mu itself; the caller must NOT hold it.
//
// SAN-10: the matching subscribers are collected under the lock and
// called with it released. A consumer callback is another plugin's
// code and takes that plugin's locks — whenmoon's takes the markets
// rwlock — while whenmoon's subscribe path takes them the other way
// round, market rwlock then this mutex. Fanning out under `mu` closed
// that cycle; TSan measured it on the resub the operator re-applies by
// hand.
//
// `in_dispatch` replaces the lock as unsubscribe's barrier: it is
// raised before the lock goes and dropped after the last callback
// returns, so an unsubscribe that has already unlinked its sub still
// waits for a delivery in flight. The consumer contract is unchanged
// in the direction that matters — a callback still must not call
// coinbase_ws_subscribe / _unsubscribe, which would now wait on itself
// rather than deadlock on the mutex.
static void
cb_ws_fanout(const coinbase_ws_event_t *ev)
{
  struct
  {
    coinbase_ws_event_cb_t cb;
    void                  *user;
  }        targets[CB_WS_CH_MAX_SUBS];   // subscribe caps the list at this
  uint32_t n = 0;
  uint32_t i;

  pthread_mutex_lock(&cb_ws_ch.mu);

  for(struct coinbase_ws_sub *s = cb_ws_ch.head; s != NULL; s = s->next)
  {
    bool match;

    if(!(s->channel_mask & (1u << ev->channel)))
      continue;

    if(ev->product_id == NULL
        || !cb_ws_channel_is_per_product(ev->channel))
    {
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

    if(match)
    {
      targets[n].cb   = s->cb;
      targets[n].user = s->user;
      n++;
    }
  }

  if(n == 0)
  {
    pthread_mutex_unlock(&cb_ws_ch.mu);
    return;
  }

  cb_ws_ch.in_dispatch++;

  pthread_mutex_unlock(&cb_ws_ch.mu);

  for(i = 0; i < n; i++)
    targets[i].cb(ev, targets[i].user);

  pthread_mutex_lock(&cb_ws_ch.mu);

  cb_ws_ch.in_dispatch--;

  if(cb_ws_ch.in_dispatch == 0)
    pthread_cond_broadcast(&cb_ws_ch.drain);

  pthread_mutex_unlock(&cb_ws_ch.mu);
}

// Wait out every consumer callback currently in flight. Caller holds
// cb_ws_ch.mu; it is released while waiting and held again on return.
static void
cb_ws_drain_dispatch_locked(void)
{
  while(cb_ws_ch.in_dispatch > 0)
    pthread_cond_wait(&cb_ws_ch.drain, &cb_ws_ch.mu);
}

// --- per-channel parsers ---
//
// Each parser handles a single Advanced Trade event object (one entry
// out of the envelope's `events[]` array). The envelope's per-frame
// timestamp is threaded in as `frame_time_ms` so per-event payloads
// without their own `time` field still surface a timestamp.

// Lowercase an ASCII string in place; safe on the small COINBASE_SIDE_SZ
// buffers fanout consumers expect to be "buy"/"sell".
static void
cb_ws_lower_ascii(char *s)
{
  size_t i;

  if(s == NULL) return;

  for(i = 0; s[i] != '\0'; i++)
  {
    if(s[i] >= 'A' && s[i] <= 'Z')
      s[i] = (char)(s[i] + 32);
  }
}

static void
cb_ws_dispatch_heartbeats(struct json_object *event,
    int64_t frame_time_ms)
{
  coinbase_ws_heartbeat_t hb = {0};
  coinbase_ws_event_t     ev = {0};
  char                    buf[64];

  // Advanced Trade heartbeats carry `current_time` (non-ISO format,
  // not parsed) and a `heartbeat_counter` string. Surface the counter
  // in `last_trade_id` so liveness consumers can detect stalls;
  // fall back to the envelope timestamp for `time_ms`.
  if(json_get_str(event, "heartbeat_counter", buf, sizeof(buf)))
    hb.last_trade_id = (int64_t)strtoll(buf, NULL, 10);

  hb.time_ms = frame_time_ms;

  ev.channel    = COINBASE_CH_HEARTBEAT;
  ev.product_id = NULL;
  ev.sequence   = 0;
  ev.gap        = false;
  ev.payload    = &hb;

  cb_ws_fanout(&ev);
}

static void
cb_ws_dispatch_ticker(struct json_object *event,
    coinbase_ws_channel_t ch, int64_t frame_time_ms)
{
  struct json_object *tickers;
  size_t              n;
  size_t              i;

  tickers = json_get_array(event, "tickers");
  if(tickers == NULL) return;

  n = json_object_array_length(tickers);

  for(i = 0; i < n; i++)
  {
    struct json_object   *t   = json_object_array_get_idx(tickers, i);
    coinbase_ws_ticker_t  out = {0};
    coinbase_ws_event_t   pe  = {0};
    char                  num[32];

    if(t == NULL) continue;

    json_get_str(t, "product_id", out.product_id, sizeof(out.product_id));

    if(json_get_str(t, "price",       num, sizeof(num)))
      out.price      = strtod(num, NULL);
    if(json_get_str(t, "best_bid",    num, sizeof(num)))
      out.best_bid   = strtod(num, NULL);
    if(json_get_str(t, "best_ask",    num, sizeof(num)))
      out.best_ask   = strtod(num, NULL);
    if(json_get_str(t, "volume_24_h", num, sizeof(num)))
      out.volume_24h = strtod(num, NULL);
    if(json_get_str(t, "low_24_h",    num, sizeof(num)))
      out.low_24h    = strtod(num, NULL);
    if(json_get_str(t, "high_24_h",   num, sizeof(num)))
      out.high_24h   = strtod(num, NULL);

    out.time_ms = frame_time_ms;

    pe.channel    = ch;
    pe.product_id = out.product_id[0] ? out.product_id : NULL;
    pe.sequence   = 0;
    pe.gap        = false;
    pe.payload    = &out;

    cb_ws_fanout(&pe);
  }
}

static void
cb_ws_dispatch_market_trades(struct json_object *event,
    int64_t frame_time_ms)
{
  struct json_object *trades;
  size_t              n;
  size_t              i;

  trades = json_get_array(event, "trades");
  if(trades == NULL) return;

  n = json_object_array_length(trades);

  for(i = 0; i < n; i++)
  {
    struct json_object  *tr = json_object_array_get_idx(trades, i);
    coinbase_ws_match_t  m  = {0};
    coinbase_ws_event_t  pe = {0};
    char                 num[64];

    if(tr == NULL) continue;

    json_get_str(tr, "product_id", m.product_id, sizeof(m.product_id));
    json_get_str(tr, "side",       m.side,       sizeof(m.side));

    cb_ws_lower_ascii(m.side);

    if(json_get_str(tr, "trade_id", num, sizeof(num)))
      m.trade_id = (int64_t)strtoll(num, NULL, 10);

    if(json_get_str(tr, "price", num, sizeof(num)))
      m.price = strtod(num, NULL);
    if(json_get_str(tr, "size",  num, sizeof(num)))
      m.size  = strtod(num, NULL);

    if(json_get_str(tr, "time", num, sizeof(num)))
      m.time_ms = cb_ws_parse_iso8601_ms(num);

    if(m.time_ms == 0) m.time_ms = frame_time_ms;

    pe.channel    = COINBASE_CH_MATCHES;
    pe.product_id = m.product_id[0] ? m.product_id : NULL;
    pe.sequence   = 0;
    pe.gap        = false;
    pe.payload    = &m;

    cb_ws_fanout(&pe);
  }
}

static void
cb_ws_dispatch_l2(struct json_object *event, int64_t frame_time_ms)
{
  coinbase_ws_l2update_t  u   = {0};
  coinbase_ws_event_t     ev  = {0};
  struct json_object     *updates;
  size_t                  total;
  size_t                  i;

  json_get_str(event, "product_id", u.product_id, sizeof(u.product_id));
  u.time_ms = frame_time_ms;

  updates = json_get_array(event, "updates");

  if(updates != NULL)
  {
    total = json_object_array_length(updates);

    for(i = 0; i < total; i++)
    {
      struct json_object *up = json_object_array_get_idx(updates, i);
      char                side[16] = {0};
      char                price_s[32] = {0};
      char                qty_s[32] = {0};

      if(up == NULL) continue;

      if(u.n_changes >= COINBASE_WS_L2_MAX_CHANGES)
      {
        u.n_changes_dropped++;
        continue;
      }

      json_get_str(up, "side",         side,    sizeof(side));
      json_get_str(up, "price_level",  price_s, sizeof(price_s));
      json_get_str(up, "new_quantity", qty_s,   sizeof(qty_s));

      // Map Advanced Trade "bid" / "offer" onto legacy "buy" / "sell"
      // so existing consumers that switch on side strings keep working.
      if(strcmp(side, "bid") == 0)
        snprintf(u.changes[u.n_changes].side,
            sizeof(u.changes[u.n_changes].side), "buy");
      else if(strcmp(side, "offer") == 0 || strcmp(side, "ask") == 0)
        snprintf(u.changes[u.n_changes].side,
            sizeof(u.changes[u.n_changes].side), "sell");
      else
        snprintf(u.changes[u.n_changes].side,
            sizeof(u.changes[u.n_changes].side), "%s", side);

      u.changes[u.n_changes].price = strtod(price_s, NULL);
      u.changes[u.n_changes].size  = strtod(qty_s,   NULL);
      u.n_changes++;
    }

    if(u.n_changes_dropped > 0)
      clam(CLAM_WARN, CB_CTX,
          "ws l2 product=%s: %u deltas dropped (cap %d)",
          u.product_id, u.n_changes_dropped,
          COINBASE_WS_L2_MAX_CHANGES);
  }

  ev.channel    = COINBASE_CH_LEVEL2;
  ev.product_id = u.product_id[0] ? u.product_id : NULL;
  ev.sequence   = 0;
  ev.gap        = false;
  ev.payload    = &u;

  cb_ws_fanout(&ev);
}

// Pull a JSON field that the gateway emits as either a quoted decimal
// ("50000.00") or a bare JSON number (50000.00). Advanced Trade is
// inconsistent across endpoints; the user channel in particular has
// historically flipped representations between minor SDK versions.
static double
cb_ws_get_decimal_loose(struct json_object *obj, const char *key)
{
  char    s[64];
  double  d = 0.0;

  if(json_get_str(obj, key, s, sizeof(s)))
    return(strtod(s, NULL));

  if(json_get_double(obj, key, &d))
    return(d);

  return(0.0);
}

// Parse one entry from `events[].orders[]`. The AT user channel order
// shape carries the full order lifecycle: status transitions
// (OPEN → FILLED / CANCELLED / EXPIRED / FAILED) plus running totals
// (cumulative_quantity, leaves_quantity, avg_price, total_fees). Live-
// trading consumers derive fill-deltas from successive cumulative_qty
// observations on the same order_id when no separate `fills[]` array
// is present.
static void
cb_ws_dispatch_user_order(struct json_object *order_obj,
    int64_t frame_time_ms)
{
  coinbase_ws_user_event_t  evp = {0};
  coinbase_ws_user_order_t *o   = &evp.u.order;
  coinbase_ws_event_t       pe  = {0};
  char                      buf[64];

  evp.kind = COINBASE_WS_USER_KIND_ORDER;

  json_get_str(order_obj, "order_id",        o->order_id,
      sizeof(o->order_id));
  json_get_str(order_obj, "client_order_id", o->client_order_id,
      sizeof(o->client_order_id));
  json_get_str(order_obj, "product_id",      o->product_id,
      sizeof(o->product_id));
  json_get_str(order_obj, "status",          o->status,
      sizeof(o->status));

  if(json_get_str(order_obj, "order_side", o->side, sizeof(o->side)))
    cb_ws_lower_ascii(o->side);

  o->limit_price         = cb_ws_get_decimal_loose(order_obj, "limit_price");
  o->cumulative_quantity = cb_ws_get_decimal_loose(order_obj,
      "cumulative_quantity");
  o->leaves_quantity     = cb_ws_get_decimal_loose(order_obj,
      "leaves_quantity");
  o->avg_price           = cb_ws_get_decimal_loose(order_obj, "avg_price");
  o->total_fees          = cb_ws_get_decimal_loose(order_obj, "total_fees");

  if(json_get_str(order_obj, "creation_time", buf, sizeof(buf)))
    o->creation_time_ms = cb_ws_parse_iso8601_ms(buf);

  o->time_ms = frame_time_ms;

  pe.channel    = COINBASE_CH_USER;
  pe.product_id = o->product_id[0] ? o->product_id : NULL;
  pe.sequence   = 0;
  pe.gap        = false;
  pe.payload    = &evp;

  cb_ws_fanout(&pe);
}

// Parse one entry from `events[].fills[]`. Coinbase's AT docs describe
// this array as forthcoming / SDK-version-dependent; whether and when it
// fires is gateway-controlled. The parser lands ahead of consumers so
// the live engine sees fills the moment the gateway starts emitting
// them. Until then, fill detail is derived from order-update deltas.
static void
cb_ws_dispatch_user_fill(struct json_object *fill_obj,
    int64_t frame_time_ms)
{
  coinbase_ws_user_event_t  evp = {0};
  coinbase_ws_user_fill_t  *f   = &evp.u.fill;
  coinbase_ws_event_t       pe  = {0};
  char                      buf[64];

  evp.kind = COINBASE_WS_USER_KIND_FILL;

  json_get_str(fill_obj, "order_id",        f->order_id,
      sizeof(f->order_id));
  json_get_str(fill_obj, "client_order_id", f->client_order_id,
      sizeof(f->client_order_id));
  json_get_str(fill_obj, "product_id",      f->product_id,
      sizeof(f->product_id));

  if(json_get_str(fill_obj, "side", f->side, sizeof(f->side)))
    cb_ws_lower_ascii(f->side);

  if(json_get_str(fill_obj, "trade_id", buf, sizeof(buf)))
    f->trade_id = (int64_t)strtoll(buf, NULL, 10);

  f->price = cb_ws_get_decimal_loose(fill_obj, "price");
  f->size  = cb_ws_get_decimal_loose(fill_obj, "size");
  f->fee   = cb_ws_get_decimal_loose(fill_obj, "fee");

  if(json_get_str(fill_obj, "time", buf, sizeof(buf)))
    f->time_ms = cb_ws_parse_iso8601_ms(buf);

  if(f->time_ms == 0) f->time_ms = frame_time_ms;

  pe.channel    = COINBASE_CH_USER;
  pe.product_id = f->product_id[0] ? f->product_id : NULL;
  pe.sequence   = 0;
  pe.gap        = false;
  pe.payload    = &evp;

  cb_ws_fanout(&pe);
}

static void
cb_ws_dispatch_user(struct json_object *event,
    int64_t frame_time_ms)
{
  struct json_object *orders;
  struct json_object *fills;
  size_t              n;
  size_t              i;

  orders = json_get_array(event, "orders");

  if(orders != NULL)
  {
    n = json_object_array_length(orders);

    for(i = 0; i < n; i++)
    {
      struct json_object *o = json_object_array_get_idx(orders, i);

      if(o == NULL) continue;

      cb_ws_dispatch_user_order(o, frame_time_ms);
    }
  }

  fills = json_get_array(event, "fills");

  if(fills != NULL)
  {
    n = json_object_array_length(fills);

    for(i = 0; i < n; i++)
    {
      struct json_object *f = json_object_array_get_idx(fills, i);

      if(f == NULL) continue;

      cb_ws_dispatch_user_fill(f, frame_time_ms);
    }
  }
}

static void
cb_ws_dispatch_status(struct json_object *event,
    int64_t frame_time_ms)
{
  coinbase_ws_status_t st = {0};
  coinbase_ws_event_t  ev = {0};

  (void)event;
  st.time_ms = frame_time_ms;

  ev.channel    = COINBASE_CH_STATUS;
  ev.product_id = NULL;
  ev.sequence   = 0;
  ev.gap        = false;
  ev.payload    = &st;

  cb_ws_fanout(&ev);
}

// ----------------------------------------------------------------------
// Public internal API (called from coinbase_ws.c / coinbase.c)
// ----------------------------------------------------------------------

void
cb_ws_channels_init(void)
{
  if(cb_ws_ch.initialized) return;

  memset(&cb_ws_ch, 0, sizeof(cb_ws_ch));
  pthread_mutex_init(&cb_ws_ch.mu, NULL);
  pthread_cond_init(&cb_ws_ch.drain, NULL);
  cb_ws_ch.initialized = true;

  clam(CLAM_DEBUG, CB_CTX, "ws channel multiplexer initialized");
}

void
cb_ws_channels_deinit(void)
{
  struct coinbase_ws_sub *s;
  struct coinbase_ws_sub *n;

  if(!cb_ws_ch.initialized) return;

  pthread_mutex_lock(&cb_ws_ch.mu);

  // Drop the list first so nothing new fans out, then wait out what is
  // already inside a consumer callback — freeing a sub under a live
  // delivery is the same hazard coinbase_ws_unsubscribe drains for.
  s = cb_ws_ch.head;

  cb_ws_ch.head    = NULL;
  cb_ws_ch.n_subs  = 0;
  cb_ws_ch.n_slots = 0;

  cb_ws_drain_dispatch_locked();

  while(s != NULL)
  {
    n = s->next;
    mem_free(s);
    s = n;
  }

  pthread_mutex_unlock(&cb_ws_ch.mu);

  pthread_cond_destroy(&cb_ws_ch.drain);
  pthread_mutex_destroy(&cb_ws_ch.mu);
  cb_ws_ch.initialized = false;

  clam(CLAM_DEBUG, CB_CTX, "ws channel multiplexer torn down");
}

void
cb_ws_channels_on_open(void)
{
  if(!cb_ws_ch.initialized) return;

  pthread_mutex_lock(&cb_ws_ch.mu);

  if(cb_ws_ch.n_slots == 0)
  {
    pthread_mutex_unlock(&cb_ws_ch.mu);
    return;
  }

  // Clear sent_upstream on every slot so the "needs sub" predicate
  // picks them all up, then emit a single batched subscribe covering
  // everything.
  for(uint32_t i = 0; i < cb_ws_ch.n_slots; i++)
    cb_ws_ch.slots[i].sent_upstream = false;

  cb_ws_send_delta_locked("subscribe", cb_ws_pred_needs_sub,
      /* new_sent_state */ true);

  pthread_mutex_unlock(&cb_ws_ch.mu);
}

bool
cb_ws_channels_sub_ack_overdue(void)
{
  uint64_t sent;
  uint64_t acked;

  if(!cb_ws_ch.initialized) return(false);

  pthread_mutex_lock(&cb_ws_ch.mu);
  sent  = cb_ws_ch.last_sub_sent_ms;
  acked = cb_ws_ch.last_ack_ms;
  pthread_mutex_unlock(&cb_ws_ch.mu);

  if(sent == 0 || acked >= sent)
    return(false);

  return(cb_ws_ch_now_ms() - sent > CB_WS_SUB_ACK_TIMEOUT_MS);
}

void
cb_ws_channels_dispatch(const char *buf, size_t len)
{
  struct json_object *root;
  struct json_object *events;
  char                type[64]    = {0};
  char                channel[64] = {0};
  char                ts[64];
  int64_t             frame_time_ms = 0;
  size_t              ev_n;
  size_t              i;

  if(!cb_ws_ch.initialized || buf == NULL || len == 0) return;

  root = json_parse_buf(buf, len, "coinbase:ws_recv");
  if(root == NULL) return;

  // Gateway error frames keep the legacy {type:"error",message:"..."}
  // shape — surface them before looking for an Advanced Trade envelope.
  // Dump the full frame at WARN so the operator can see which subscribe
  // failed (Advanced Trade typically embeds the offending channel name
  // in the error body).
  if(json_get_str(root, "type", type, sizeof(type))
      && strcmp(type, "error") == 0)
  {
    char        msg[256]  = "?";
    const char *raw_json;

    json_get_str(root, "message", msg, sizeof(msg));

    raw_json = json_object_to_json_string_ext(root,
        JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);

    clam(CLAM_WARN, CB_CTX, "ws server error: %s | frame=%s", msg,
        raw_json != NULL ? raw_json : "?");

    json_object_put(root);
    return;
  }

  // Advanced Trade data frames are wrapped in
  //   {channel:"<name>", timestamp:"...", sequence_num:N, events:[...]}
  // and the "subscriptions" ack uses the same envelope (channel set to
  // "subscriptions"). Discriminate on the top-level `channel` field.
  if(!json_get_str(root, "channel", channel, sizeof(channel)))
  {
    json_object_put(root);
    return;
  }

  if(strcmp(channel, "subscriptions") == 0)
  {
    // Surface the channel list inside the ack so we can correlate
    // subscribes-out with acks-in and spot the missing channel.
    const char *raw_json;

    pthread_mutex_lock(&cb_ws_ch.mu);
    cb_ws_ch.last_ack_ms = cb_ws_ch_now_ms();
    pthread_mutex_unlock(&cb_ws_ch.mu);

    raw_json = json_object_to_json_string_ext(root,
        JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);

    clam(CLAM_DEBUG, CB_CTX, "ws subscriptions ack: %s",
        raw_json != NULL ? raw_json : "?");

    json_object_put(root);
    return;
  }

  if(json_get_str(root, "timestamp", ts, sizeof(ts)))
    frame_time_ms = cb_ws_parse_iso8601_ms(ts);

  events = json_get_array(root, "events");

  if(events == NULL)
  {
    json_object_put(root);
    return;
  }

  ev_n = json_object_array_length(events);

  // No lock here: the parsers below read the frame and nothing else,
  // and cb_ws_fanout takes cb_ws_ch.mu for exactly as long as it needs
  // the subscriber list (SAN-10).
  for(i = 0; i < ev_n; i++)
  {
    struct json_object *event = json_object_array_get_idx(events, i);

    if(event == NULL) continue;

    if(strcmp(channel, "heartbeats") == 0)
      cb_ws_dispatch_heartbeats(event, frame_time_ms);
    else if(strcmp(channel, "ticker") == 0)
      cb_ws_dispatch_ticker(event, COINBASE_CH_TICKER,
          frame_time_ms);
    else if(strcmp(channel, "ticker_batch") == 0)
      cb_ws_dispatch_ticker(event, COINBASE_CH_TICKER_BATCH,
          frame_time_ms);
    else if(strcmp(channel, "market_trades") == 0)
      cb_ws_dispatch_market_trades(event, frame_time_ms);
    else if(strcmp(channel, "l2_data") == 0)
      cb_ws_dispatch_l2(event, frame_time_ms);
    else if(strcmp(channel, "status") == 0)
      cb_ws_dispatch_status(event, frame_time_ms);
    else if(strcmp(channel, "user") == 0)
      cb_ws_dispatch_user(event, frame_time_ms);
    else
      clam(CLAM_DEBUG3, CB_CTX, "ws ignoring channel=%s", channel);
  }

  json_object_put(root);
}

// OBS-51: give back an incomplete arm. Every refcount this call took
// comes off, so a slot it allocated falls to zero and compacts away —
// nothing has been emitted for it yet, so no slot the gateway holds can
// be reaped by this path. Order matters: the deltas are indexed by slot
// position, so they must be applied BEFORE the compaction that moves
// slots. Caller holds `mu` and frees `sub` after unlocking.
static void
cb_ws_sub_rollback_locked(struct coinbase_ws_sub *sub,
    const uint32_t *rc_delta)
{
  struct coinbase_ws_sub **pp;
  uint32_t                 i;

  for(i = 0; i < cb_ws_ch.n_slots; i++)
    cb_ws_ch.slots[i].refcount -= rc_delta[i];

  cb_ws_slots_compact_locked();

  for(pp = &cb_ws_ch.head; *pp != NULL; pp = &(*pp)->next)
  {
    if(*pp == sub)
    {
      *pp = sub->next;
      cb_ws_ch.n_subs--;
      break;
    }
  }
}

// ----------------------------------------------------------------------
// Public API — exported via plugin_dlsym
// ----------------------------------------------------------------------

coinbase_ws_sub_t *
coinbase_ws_subscribe(const coinbase_ws_channel_t *channels,
    size_t n_channels, const char *const *product_ids, size_t n_products,
    coinbase_ws_event_cb_t cb, void *user)
{
  struct coinbase_ws_sub *sub;
  uint32_t                channel_mask     = 0;
  uint32_t                rc_delta[CB_WS_CH_MAX_SLOTS] = {0};
  uint32_t                n_wanted         = 0;
  uint32_t                n_armed          = 0;
  uint32_t                n_slots_seen     = 0;
  size_t                  i;
  bool                    has_product_chan = false;

  if(!cb_ws_ch.initialized)
  {
    clam(CLAM_WARN, CB_CTX, "ws subscribe: multiplexer not initialized");
    return(NULL);
  }

  if(cb == NULL || channels == NULL || n_channels == 0)
  {
    clam(CLAM_WARN, CB_CTX, "ws subscribe: invalid args");
    return(NULL);
  }

  if(n_products > CB_WS_CH_MAX_PRODUCTS_PER_SUB)
  {
    clam(CLAM_WARN, CB_CTX,
        "ws subscribe: too many products (%zu > %d)",
        n_products, CB_WS_CH_MAX_PRODUCTS_PER_SUB);
    return(NULL);
  }

  for(i = 0; i < n_channels; i++)
  {
    if((unsigned)channels[i] >= COINBASE_CH__COUNT
        || cb_ws_channel_name(channels[i]) == NULL)
    {
      clam(CLAM_WARN, CB_CTX,
          "ws subscribe: unsupported channel %d", (int)channels[i]);
      return(NULL);
    }

    if(cb_ws_channel_is_per_product(channels[i]))
      has_product_chan = true;

    channel_mask |= (1u << channels[i]);
  }

  if(has_product_chan && n_products == 0)
  {
    clam(CLAM_WARN, CB_CTX,
        "ws subscribe: product-keyed channel without product_ids");
    return(NULL);
  }

  // Advanced Trade requires a JWT on every subscribe (public channels
  // included). Creds may not yet be in the KV at subscribe time
  // (freshstart writes credentials post-launch); the slot still goes
  // into the table with sent_upstream=false, and cb_ws_send_delta_locked
  // retries on every reconcile. The creds-changed hook in coinbase_ws.c
  // schedules a reconnect that lands here via cb_ws_channels_on_open
  // once credentials arrive.

  // Heartbeats + status are always added to the upstream set for
  // plugin-local liveness — callers get them in their fanout mask so
  // their own callback can inspect the events if desired.
  channel_mask |= (1u << COINBASE_CH_HEARTBEAT)
               |  (1u << COINBASE_CH_STATUS);

  pthread_mutex_lock(&cb_ws_ch.mu);

  if(cb_ws_ch.n_subs >= CB_WS_CH_MAX_SUBS)
  {
    pthread_mutex_unlock(&cb_ws_ch.mu);
    clam(CLAM_WARN, CB_CTX, "ws subscribe: sub table full (%u)",
        cb_ws_ch.n_subs);
    return(NULL);
  }

  sub = mem_alloc(CB_CTX, "ws_sub", sizeof(*sub));
  sub->id           = ++cb_ws_ch.next_id;
  sub->cb           = cb;
  sub->user         = user;
  sub->channel_mask = channel_mask;
  sub->n_products   = (uint32_t)n_products;

  for(i = 0; i < n_products; i++)
  {
    if(product_ids[i] == NULL || product_ids[i][0] == '\0')
    {
      pthread_mutex_unlock(&cb_ws_ch.mu);
      mem_free(sub);
      clam(CLAM_WARN, CB_CTX,
          "ws subscribe: empty product_id at idx %zu", i);
      return(NULL);
    }
    snprintf(sub->products[i], sizeof(sub->products[i]), "%s",
        product_ids[i]);
  }

  sub->next = cb_ws_ch.head;
  cb_ws_ch.head = sub;
  cb_ws_ch.n_subs++;

  // Refcount every (channel, product) pair this sub covers.
  for(int ch = 0; ch < COINBASE_CH__COUNT; ch++)
  {
    coinbase_ws_channel_t cch = (coinbase_ws_channel_t)ch;

    if(!(channel_mask & (1u << ch))) continue;

    if(!cb_ws_channel_is_per_product(cch))
    {
      int32_t idx = cb_ws_slot_find_locked(cch, "");

      n_wanted++;

      if(idx < 0)
      {
        cb_ws_slot_t *sl = cb_ws_slot_alloc_locked(cch, "");

        if(sl == NULL)
        {
          clam(CLAM_WARN, CB_CTX,
              "ws subscribe: slot table full, skipping ch=%s",
              cb_ws_channel_name(cch));
          continue;
        }
        idx = (int32_t)(sl - cb_ws_ch.slots);
      }

      cb_ws_ch.slots[idx].refcount++;
      rc_delta[idx]++;
      n_armed++;
    }
    else
    {
      for(i = 0; i < n_products; i++)
      {
        const char *pid = sub->products[i];
        int32_t     idx = cb_ws_slot_find_locked(cch, pid);

        n_wanted++;

        if(idx < 0)
        {
          cb_ws_slot_t *sl = cb_ws_slot_alloc_locked(cch, pid);

          if(sl == NULL)
          {
            clam(CLAM_WARN, CB_CTX,
                "ws subscribe: slot table full, skipping %s/%s",
                cb_ws_channel_name(cch), pid);
            continue;
          }
          idx = (int32_t)(sl - cb_ws_ch.slots);
        }

        cb_ws_ch.slots[idx].refcount++;
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
    n_slots_seen = cb_ws_ch.n_slots;

    cb_ws_sub_rollback_locked(sub, rc_delta);
    pthread_mutex_unlock(&cb_ws_ch.mu);
    mem_free(sub);

    clam(CLAM_WARN, CB_CTX,
        "ws subscribe: armed %u of %u (channel,product) pairs — refusing "
        "the whole call (slots %u/%u)",
        n_armed, n_wanted, n_slots_seen, CB_WS_CH_MAX_SLOTS);

    return(NULL);
  }

  // Emit subscribe for every slot that's live but not yet upstream.
  cb_ws_send_delta_locked("subscribe", cb_ws_pred_needs_sub,
      /* new_sent_state */ true);

  pthread_mutex_unlock(&cb_ws_ch.mu);

  clam(CLAM_INFO, CB_CTX,
      "ws subscribe id=%u channels=0x%04x products=%u",
      sub->id, channel_mask, (uint32_t)n_products);

  return(sub);
}

void
coinbase_ws_unsubscribe(coinbase_ws_sub_t *sub)
{
  struct coinbase_ws_sub **pp;
  size_t                   i;
  uint32_t                 sub_id;

  if(sub == NULL || !cb_ws_ch.initialized) return;

  pthread_mutex_lock(&cb_ws_ch.mu);

  // Unlink from the global list.
  for(pp = &cb_ws_ch.head; *pp != NULL; pp = &(*pp)->next)
  {
    if(*pp == sub)
    {
      *pp = sub->next;
      cb_ws_ch.n_subs--;
      break;
    }
  }

  sub_id = sub->id;

  // Unlinked above, so no further fan-out can pick this sub up; wait
  // out the deliveries already running before the handle — and the
  // consumer's `user` behind it — goes away. SAN-10: the fan-out no
  // longer holds `mu`, so the unlink alone is not the barrier it used
  // to be.
  cb_ws_drain_dispatch_locked();

  // Decrement refcounts on every slot this sub held.
  for(int ch = 0; ch < COINBASE_CH__COUNT; ch++)
  {
    if(!(sub->channel_mask & (1u << ch))) continue;

    if(!cb_ws_channel_is_per_product((coinbase_ws_channel_t)ch))
    {
      int32_t idx = cb_ws_slot_find_locked(
          (coinbase_ws_channel_t)ch, "");

      if(idx >= 0 && cb_ws_ch.slots[idx].refcount > 0)
        cb_ws_ch.slots[idx].refcount--;
    }
    else
    {
      for(i = 0; i < sub->n_products; i++)
      {
        int32_t idx = cb_ws_slot_find_locked(
            (coinbase_ws_channel_t)ch, sub->products[i]);

        if(idx >= 0 && cb_ws_ch.slots[idx].refcount > 0)
          cb_ws_ch.slots[idx].refcount--;
      }
    }
  }

  // Emit unsubscribe for every slot that just hit refcount==0.
  cb_ws_send_delta_locked("unsubscribe", cb_ws_pred_needs_unsub,
      /* new_sent_state */ false);

  // Reap empty slots.
  cb_ws_slots_compact_locked();

  mem_free(sub);

  pthread_mutex_unlock(&cb_ws_ch.mu);

  clam(CLAM_INFO, CB_CTX, "ws unsubscribe id=%u", sub_id);
}
