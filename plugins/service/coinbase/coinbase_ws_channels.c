// botmanager — MIT
// Coinbase Exchange: WebSocket channel multiplexer (CB5).
//
// Layered on top of CB4's single-session transport. Owns the list of
// local subscribers and a dedup table of (channel, product) slots so
// that N consumers watching the same feed share one upstream slot.
// When a slot's refcount reaches zero the slot is unsubscribed
// upstream. On reconnect the slot table drives a full resubscribe so
// consumer callbacks never miss a beat across a flap.
//
// Parses each inbound text frame into a typed event and fans it out
// to every local subscriber whose channel + product set matches.
//
// Sequence gap detection is intentionally not performed. Coinbase's
// `sequence` field is per-product-global — every event for a product
// across every channel increments it — so any consumer subscribed to
// a strict subset (e.g. heartbeat + ticker without matches/level2)
// will observe huge "gaps" on every unsubscribed-channel event. Doing
// per-(channel, product) gap detection produces a flood of false
// positives at production volumes (hundreds/sec on a busy pair).
// True per-message integrity needs a `full` subscription and per-
// product (not per-channel) tracking; we don't subscribe to `full`
// for trading, so the check is skipped. The `coinbase_ws_event_t.gap`
// field is preserved as an ABI placeholder and remains false.

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

  bool                    initialized;
} cb_ws_ch;

// ----------------------------------------------------------------------
// Channel metadata
// ----------------------------------------------------------------------

static const char *
cb_ws_channel_name(coinbase_ws_channel_t ch)
{
  switch(ch)
  {
    case COINBASE_CH_HEARTBEAT:    return("heartbeat");
    case COINBASE_CH_STATUS:       return("status");
    case COINBASE_CH_TICKER:       return("ticker");
    case COINBASE_CH_TICKER_BATCH: return("ticker_batch");
    case COINBASE_CH_LEVEL2:       return("level2");
    case COINBASE_CH_LEVEL2_BATCH: return("level2_batch");
    case COINBASE_CH_MATCHES:      return("matches");
    case COINBASE_CH_FULL:         return("full");
    case COINBASE_CH_USER:         return("user");
    case COINBASE_CH__COUNT:       break;
  }
  return(NULL);
}

static bool
cb_ws_channel_is_per_product(coinbase_ws_channel_t ch)
{
  return(ch != COINBASE_CH_STATUS);
}

static bool
cb_ws_channel_needs_auth(coinbase_ws_channel_t ch)
{
  return(ch == COINBASE_CH_FULL || ch == COINBASE_CH_USER);
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

// Render a subscribe / unsubscribe frame covering every slot that
// matches `auth_only` and passes the `include_slot` predicate. Returns
// bytes written, or 0 when no slot qualifies (caller must not emit an
// empty frame). Auth frames include the signature block; public frames
// do not. All identifiers used (channel names, product ids, sig, key,
// passphrase, ts) are JSON-safe by construction (allowlisted enums or
// base64) so no escaping is performed.
typedef bool (*cb_ws_slot_pred_t)(const cb_ws_slot_t *);

static size_t
cb_ws_render_frame_locked(char *out, size_t cap, const char *type,
    bool auth_only, cb_ws_slot_pred_t include_slot,
    const char *sig, const char *apikey, const char *passphrase,
    const char *ts)
{
  size_t pos = 0;
  int    n;
  bool   first_channel = true;
  bool   any           = false;

  if(out == NULL || cap == 0) return(0);

  n = snprintf(out + pos, cap - pos, "{\"type\":\"%s\"", type);
  if(n < 0 || (size_t)n >= cap - pos) return(0);
  pos += (size_t)n;

  if(auth_only)
  {
    n = snprintf(out + pos, cap - pos,
        ",\"signature\":\"%s\",\"key\":\"%s\""
        ",\"passphrase\":\"%s\",\"timestamp\":\"%s\"",
        (sig != NULL) ? sig : "",
        (apikey != NULL) ? apikey : "",
        (passphrase != NULL) ? passphrase : "",
        (ts != NULL) ? ts : "");
    if(n < 0 || (size_t)n >= cap - pos) return(0);
    pos += (size_t)n;
  }

  n = snprintf(out + pos, cap - pos, ",\"channels\":[");
  if(n < 0 || (size_t)n >= cap - pos) return(0);
  pos += (size_t)n;

  for(int ch = 0; ch < COINBASE_CH__COUNT; ch++)
  {
    coinbase_ws_channel_t  cch = (coinbase_ws_channel_t)ch;
    const char            *cname;
    bool                   emitted = false;

    if(cb_ws_channel_needs_auth(cch) != auth_only) continue;

    cname = cb_ws_channel_name(cch);
    if(cname == NULL) continue;

    // Gather product_ids for this channel from the slot table.
    for(uint32_t i = 0; i < cb_ws_ch.n_slots; i++)
    {
      cb_ws_slot_t *s = &cb_ws_ch.slots[i];

      if(s->channel != cch) continue;
      if(!include_slot(s))  continue;

      if(!emitted)
      {
        if(!first_channel)
        {
          if(pos >= cap - 1) return(0);
          out[pos++] = ',';
        }
        first_channel = false;

        if(!cb_ws_channel_is_per_product(cch))
        {
          n = snprintf(out + pos, cap - pos, "\"%s\"", cname);
          if(n < 0 || (size_t)n >= cap - pos) return(0);
          pos += (size_t)n;
          emitted = true;
          any     = true;
          break;           // non-product channel has no product list
        }

        n = snprintf(out + pos, cap - pos,
            "{\"name\":\"%s\",\"product_ids\":[", cname);
        if(n < 0 || (size_t)n >= cap - pos) return(0);
        pos += (size_t)n;

        n = snprintf(out + pos, cap - pos, "\"%s\"", s->product_id);
        if(n < 0 || (size_t)n >= cap - pos) return(0);
        pos += (size_t)n;

        emitted = true;
        any     = true;
      }
      else
      {
        n = snprintf(out + pos, cap - pos, ",\"%s\"", s->product_id);
        if(n < 0 || (size_t)n >= cap - pos) return(0);
        pos += (size_t)n;
      }
    }

    if(emitted && cb_ws_channel_is_per_product(cch))
    {
      n = snprintf(out + pos, cap - pos, "]}");
      if(n < 0 || (size_t)n >= cap - pos) return(0);
      pos += (size_t)n;
    }
  }

  n = snprintf(out + pos, cap - pos, "]}");
  if(n < 0 || (size_t)n >= cap - pos) return(0);
  pos += (size_t)n;

  return(any ? pos : 0);
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
// Auth signing helper
// ----------------------------------------------------------------------

// Produce signature / timestamp for an authenticated subscribe. The
// prehash matches Coinbase's documented rule: ts + "GET" + "/users/self/verify".
// Returns SUCCESS when both values fit in the caller's buffers.
static bool
cb_ws_auth_sign(char *ts_out, size_t ts_cap,
    char *sig_out, size_t sig_cap)
{
  if(cb_timestamp_str(ts_out, ts_cap) == 0)
    return(FAIL);

  if(cb_sign_request("GET", "/users/self/verify", NULL, 0, ts_out,
        sig_out, sig_cap) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

// ----------------------------------------------------------------------
// Reconcile — emit subscribe / unsubscribe frames for pending deltas
// ----------------------------------------------------------------------

// After a successful send for a subscribe frame, mark every slot the
// frame covered as sent_upstream=true. After an unsubscribe frame,
// flip sent_upstream=false. The predicate selects the same set used at
// render time so the state transitions line up with the wire.
static void
cb_ws_mark_slots_locked(cb_ws_slot_pred_t pred, bool auth_only,
    bool sent_state)
{
  for(uint32_t i = 0; i < cb_ws_ch.n_slots; i++)
  {
    cb_ws_slot_t *s = &cb_ws_ch.slots[i];

    if(cb_ws_channel_needs_auth(s->channel) != auth_only) continue;
    if(!pred(s)) continue;

    s->sent_upstream = sent_state;
  }
}

static void
cb_ws_send_delta_locked(const char *op, cb_ws_slot_pred_t pred,
    bool new_sent_state)
{
  char         frame[CB_WS_CH_TX_BUF_SZ];
  size_t       len;
  bool         ok;

  // --- public half ---
  len = cb_ws_render_frame_locked(frame, sizeof(frame), op, /* auth_only */ false,
      pred, NULL, NULL, NULL, NULL);

  if(len > 0)
  {
    ok = (cb_ws_send_json(frame, len) == SUCCESS);

    if(ok)
    {
      cb_ws_mark_slots_locked(pred, /* auth_only */ false, new_sent_state);
      clam(CLAM_INFO, CB_CTX, "ws %s (public, %zu bytes)", op, len);
    }
    else
    {
      clam(CLAM_DEBUG, CB_CTX,
          "ws %s (public) held: session not open (will retry on open)", op);
    }
  }

  // --- auth half (only if credentials are configured) ---
  if(!cb_apikey_configured())
    return;

  char ts [CB_TS_SZ];
  char sig[CB_SIG_SZ];

  if(cb_ws_auth_sign(ts, sizeof(ts), sig, sizeof(sig)) != SUCCESS)
  {
    clam(CLAM_WARN, CB_CTX, "ws %s auth sign failed", op);
    return;
  }

  const char *apikey     = kv_get_str("plugin.coinbase.apikey");
  const char *passphrase = kv_get_str("plugin.coinbase.passphrase");

  len = cb_ws_render_frame_locked(frame, sizeof(frame), op, /* auth_only */ true,
      pred, sig, apikey, passphrase, ts);

  if(len == 0)
    return;

  ok = (cb_ws_send_json(frame, len) == SUCCESS);

  if(ok)
  {
    cb_ws_mark_slots_locked(pred, /* auth_only */ true, new_sent_state);
    clam(CLAM_INFO, CB_CTX, "ws %s (auth, %zu bytes)", op, len);
  }
  else
  {
    clam(CLAM_DEBUG, CB_CTX,
        "ws %s (auth) held: session not open", op);
  }
}

// ----------------------------------------------------------------------
// Dispatch: parse the payload and fan out to every matching subscriber
// ----------------------------------------------------------------------

// Deliver one event to every sub whose channel/product set covers the
// event. Called with cb_ws_ch.mu held — callbacks are invoked with the
// lock taken, which matches the documented no-reentry contract on the
// consumer callback.
static void
cb_ws_fanout_locked(const coinbase_ws_event_t *ev)
{
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
      for(uint32_t i = 0; i < s->n_products; i++)
      {
        if(strcmp(s->products[i], ev->product_id) == 0)
        {
          match = true;
          break;
        }
      }
    }

    if(match)
      s->cb(ev, s->user);
  }
}

// --- per-channel parsers ---

static void
cb_ws_dispatch_heartbeat_locked(struct json_object *root)
{
  coinbase_ws_heartbeat_t hb = {0};
  coinbase_ws_event_t     ev = {0};
  char                    time_str[40];

  json_get_str   (root, "product_id", hb.product_id, sizeof(hb.product_id));
  json_get_int64 (root, "sequence",     &hb.sequence);
  json_get_int64 (root, "last_trade_id", &hb.last_trade_id);

  if(json_get_str(root, "time", time_str, sizeof(time_str)))
    hb.time_ms = cb_ws_parse_iso8601_ms(time_str);

  ev.channel    = COINBASE_CH_HEARTBEAT;
  ev.product_id = hb.product_id[0] ? hb.product_id : NULL;
  ev.sequence   = hb.sequence;
  ev.gap        = false;
  ev.payload    = &hb;

  cb_ws_fanout_locked(&ev);
}

static void
cb_ws_dispatch_ticker_locked(struct json_object *root,
    coinbase_ws_channel_t ch)
{
  coinbase_ws_ticker_t  t  = {0};
  coinbase_ws_event_t   ev = {0};
  char                  time_str[40];
  char                  num[32];

  json_get_str(root, "product_id", t.product_id, sizeof(t.product_id));
  json_get_int64(root, "sequence",  &t.sequence);

  if(json_get_str(root, "price", num, sizeof(num)))      t.price      = strtod(num, NULL);
  if(json_get_str(root, "best_bid", num, sizeof(num)))   t.best_bid   = strtod(num, NULL);
  if(json_get_str(root, "best_ask", num, sizeof(num)))   t.best_ask   = strtod(num, NULL);
  if(json_get_str(root, "volume_24h", num, sizeof(num))) t.volume_24h = strtod(num, NULL);
  if(json_get_str(root, "low_24h", num, sizeof(num)))    t.low_24h    = strtod(num, NULL);
  if(json_get_str(root, "high_24h", num, sizeof(num)))   t.high_24h   = strtod(num, NULL);

  if(json_get_str(root, "time", time_str, sizeof(time_str)))
    t.time_ms = cb_ws_parse_iso8601_ms(time_str);

  ev.channel    = ch;
  ev.product_id = t.product_id[0] ? t.product_id : NULL;
  ev.sequence   = t.sequence;
  ev.gap        = false;
  ev.payload    = &t;

  cb_ws_fanout_locked(&ev);
}

static void
cb_ws_dispatch_match_locked(struct json_object *root)
{
  coinbase_ws_match_t  m  = {0};
  coinbase_ws_event_t  ev = {0};
  char                 time_str[40];
  char                 num[32];

  json_get_str  (root, "product_id", m.product_id, sizeof(m.product_id));
  json_get_str  (root, "side",       m.side,       sizeof(m.side));
  json_get_int64(root, "trade_id",   &m.trade_id);
  json_get_int64(root, "sequence",   &m.sequence);

  if(json_get_str(root, "price", num, sizeof(num))) m.price = strtod(num, NULL);
  if(json_get_str(root, "size",  num, sizeof(num))) m.size  = strtod(num, NULL);

  if(json_get_str(root, "time", time_str, sizeof(time_str)))
    m.time_ms = cb_ws_parse_iso8601_ms(time_str);

  ev.channel    = COINBASE_CH_MATCHES;
  ev.product_id = m.product_id[0] ? m.product_id : NULL;
  ev.sequence   = m.sequence;
  ev.gap        = false;
  ev.payload    = &m;

  cb_ws_fanout_locked(&ev);
}

static void
cb_ws_dispatch_l2update_locked(struct json_object *root)
{
  coinbase_ws_l2update_t  u  = {0};
  coinbase_ws_event_t     ev = {0};
  struct json_object     *changes;
  char                    time_str[40];

  json_get_str(root, "product_id", u.product_id, sizeof(u.product_id));

  if(json_get_str(root, "time", time_str, sizeof(time_str)))
    u.time_ms = cb_ws_parse_iso8601_ms(time_str);

  changes = json_get_array(root, "changes");

  if(changes != NULL)
  {
    size_t total = json_object_array_length(changes);

    for(size_t i = 0; i < total; i++)
    {
      struct json_object *tuple = json_object_array_get_idx(changes, i);
      const char         *side_s;
      const char         *price_s;
      const char         *size_s;

      if(tuple == NULL) continue;
      if(json_object_array_length(tuple) < 3) continue;

      if(u.n_changes >= COINBASE_WS_L2_MAX_CHANGES)
      {
        u.n_changes_dropped++;
        continue;
      }

      side_s  = json_object_get_string(json_object_array_get_idx(tuple, 0));
      price_s = json_object_get_string(json_object_array_get_idx(tuple, 1));
      size_s  = json_object_get_string(json_object_array_get_idx(tuple, 2));

      if(side_s == NULL || price_s == NULL || size_s == NULL) continue;

      snprintf(u.changes[u.n_changes].side,
          sizeof(u.changes[u.n_changes].side), "%s", side_s);
      u.changes[u.n_changes].price = strtod(price_s, NULL);
      u.changes[u.n_changes].size  = strtod(size_s,  NULL);
      u.n_changes++;
    }

    if(u.n_changes_dropped > 0)
      clam(CLAM_WARN, CB_CTX,
          "ws l2update product=%s: %u deltas dropped (cap %d)",
          u.product_id, u.n_changes_dropped, COINBASE_WS_L2_MAX_CHANGES);
  }

  ev.channel    = COINBASE_CH_LEVEL2;
  ev.product_id = u.product_id[0] ? u.product_id : NULL;
  ev.sequence   = 0;
  ev.gap        = false;
  ev.payload    = &u;

  cb_ws_fanout_locked(&ev);
}

static void
cb_ws_dispatch_status_locked(struct json_object *root)
{
  coinbase_ws_status_t st = {0};
  coinbase_ws_event_t  ev = {0};
  char                 time_str[40];

  if(json_get_str(root, "time", time_str, sizeof(time_str)))
    st.time_ms = cb_ws_parse_iso8601_ms(time_str);

  ev.channel    = COINBASE_CH_STATUS;
  ev.product_id = NULL;
  ev.sequence   = 0;
  ev.gap        = false;
  ev.payload    = &st;

  cb_ws_fanout_locked(&ev);
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

  s = cb_ws_ch.head;
  while(s != NULL)
  {
    n = s->next;
    mem_free(s);
    s = n;
  }
  cb_ws_ch.head    = NULL;
  cb_ws_ch.n_subs  = 0;
  cb_ws_ch.n_slots = 0;

  pthread_mutex_unlock(&cb_ws_ch.mu);

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

void
cb_ws_channels_dispatch(const char *buf, size_t len)
{
  struct json_object *root;
  char                type[64];

  if(!cb_ws_ch.initialized || buf == NULL || len == 0) return;

  root = json_parse_buf(buf, len, "coinbase:ws_recv");
  if(root == NULL) return;

  // The "type" field is a string. json_get_obj only returns nested
  // json objects, so it would silently NULL-out and drop every frame.
  if(!json_get_str(root, "type", type, sizeof(type)))
  {
    json_object_put(root);
    return;
  }

  // Server control messages.
  if(strcmp(type, "subscriptions") == 0)
  {
    clam(CLAM_DEBUG, CB_CTX, "ws subscriptions ack");
    json_object_put(root);
    return;
  }
  if(strcmp(type, "error") == 0)
  {
    char msg[256] = "?";
    json_get_str(root, "message", msg, sizeof(msg));
    clam(CLAM_WARN, CB_CTX, "ws server error: %s", msg);
    json_object_put(root);
    return;
  }

  pthread_mutex_lock(&cb_ws_ch.mu);

  if(strcmp(type, "heartbeat") == 0)
    cb_ws_dispatch_heartbeat_locked(root);
  else if(strcmp(type, "ticker") == 0)
    cb_ws_dispatch_ticker_locked(root, COINBASE_CH_TICKER);
  else if(strcmp(type, "ticker_batch") == 0)
    cb_ws_dispatch_ticker_locked(root, COINBASE_CH_TICKER_BATCH);
  else if(strcmp(type, "match") == 0 || strcmp(type, "last_match") == 0)
    cb_ws_dispatch_match_locked(root);
  else if(strcmp(type, "l2update") == 0 || strcmp(type, "snapshot") == 0)
    cb_ws_dispatch_l2update_locked(root);
  else if(strcmp(type, "status") == 0)
    cb_ws_dispatch_status_locked(root);
  else
    clam(CLAM_DEBUG3, CB_CTX, "ws ignoring frame type=%s", type);

  pthread_mutex_unlock(&cb_ws_ch.mu);

  json_object_put(root);
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
  bool                    need_auth        = false;
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
    if((unsigned)channels[i] >= COINBASE_CH__COUNT)
    {
      clam(CLAM_WARN, CB_CTX, "ws subscribe: invalid channel %d",
          (int)channels[i]);
      return(NULL);
    }

    if(cb_ws_channel_is_per_product(channels[i]))
      has_product_chan = true;

    if(cb_ws_channel_needs_auth(channels[i]))
      need_auth = true;

    channel_mask |= (1u << channels[i]);
  }

  if(has_product_chan && n_products == 0)
  {
    clam(CLAM_WARN, CB_CTX,
        "ws subscribe: product-keyed channel without product_ids");
    return(NULL);
  }

  if(need_auth && !cb_apikey_configured())
  {
    clam(CLAM_WARN, CB_CTX,
        "ws subscribe: auth channel requested but credentials missing");
    return(NULL);
  }

  // Heartbeat + status are always added to the upstream set for
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
    }
    else
    {
      for(i = 0; i < n_products; i++)
      {
        const char *pid = sub->products[i];
        int32_t     idx = cb_ws_slot_find_locked(cch, pid);

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
      }
    }
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
