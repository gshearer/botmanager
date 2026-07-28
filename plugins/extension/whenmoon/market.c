// botmanager — MIT
// whenmoon plugin-global market state: live set, WS fanout, live-ring
// backfill.
//
// Markets are plugin-scoped (post WM-G1). The on/off knob lives in
// `wm_market.enabled`; `/whenmoon market start|stop` flips it, and
// `wm_market_restore` replays the enabled set on plugin start. Adding
// a market triggers a 1m-candle live-ring backfill (300 rows via REST)
// but no history catch-up — historical coverage is the strategy layer
// (WM-LT-3) and the user-facing `/whenmoon download …` verbs.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "aggregator.h"
#include "live.h"
#include "market.h"
#include "market_engine.h"
#include "market_persist.h"
#include "strategy.h"
#include "warmup.h"
#include "dl_schema.h"

#include "db.h"
#include "kv.h"
#include "exchange_api.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>      // strcasecmp (WM-MK-2 mode parser)

// Per-product backfill context. Heap-owned; the completion callback is
// the sole owner and frees it after wiring the rows into the market
// struct. Lifetime decoupled from whenmoon_state_t teardown: if the
// state is destroyed before the exchange fires the callback, the
// callback still runs (the protocol plugin owns its request queue)
// and must not touch the state. We therefore track the state pointer
// but also the market product_id; on callback we re-lookup the
// product id in the live state to decide whether to commit.
typedef struct
{
  whenmoon_state_t   *st;
  char                exchange_name[EXCHANGE_NAME_SZ];
  char                product_id[WM_PRODUCT_ID_SZ];
  // WM-MI-1: which instance's live ring to backfill. The re-lookup on
  // callback keys on (exchange, product, instance) so an add of a
  // second instance feeds its OWN ring, not the first match's.
  char                instance[WM_INSTANCE_LABEL_SZ];
} wm_market_backfill_ctx_t;

// KR-2: heartbeat is implicit at the protocol plugin layer (coinbase
// adds it for liveness accounting; kraken's own ping/pong handles it).
// The generic surface exposes only the consumer-visible channels.
static const exchange_ws_channel_t wm_ws_channels[] = {
  EXCH_WS_TICKER,
  EXCH_WS_TRADES,
};

// ------------------------------------------------------------------ //
// Container helpers                                                  //
// ------------------------------------------------------------------ //

// WM-MI-1: find by (exchange, product_id, instance) — the running-set
// dedup key. Multi-exchange running sets can carry the same product_id
// (e.g. "BTC-USD") on more than one exchange, and multiple instances can
// share one (exchange, product); the triple is the unique key. A NULL
// `instance` is treated as "" (the sole/legacy instance).
//
// WM-MKT-ARR-UAF-1: LOCK-FREE. The caller MUST hold `m->arr_lock` (read or
// write) across this call AND all use of the returned pointer — the rwlock
// keeps `arr` stable and the returned session alive for that span.
static whenmoon_market_t *
wm_market_find_instance(whenmoon_markets_t *m,
    const char *exchange_name, const char *product_id,
    const char *instance)
{
  uint32_t i;

  if(m == NULL || exchange_name == NULL || product_id == NULL)
    return(NULL);

  if(instance == NULL)
    instance = "";

  for(i = 0; i < m->n_markets; i++)
  {
    if(strncmp(m->arr[i]->exchange_name, exchange_name,
           EXCHANGE_NAME_SZ) == 0
        && strncmp(m->arr[i]->product_id, product_id,
           WM_PRODUCT_ID_SZ) == 0
        && strncmp(m->arr[i]->instance, instance,
           WM_INSTANCE_LABEL_SZ) == 0)
      return(m->arr[i]);
  }

  return(NULL);
}

// Grow the `arr` POINTER block to hold at least `needed` slots. Existing
// pointers are copied by mem_realloc (the sessions they reference never
// move — see whenmoon_markets_t); freshly-added tail slots are zeroed to
// NULL. WM-MKT-ARR-UAF-1: caller MUST hold `m->arr_lock` for writing (this
// is only ever reached from wm_market_add's publish section). Returns
// SUCCESS or FAIL.
static bool
wm_market_grow(whenmoon_markets_t *m, uint32_t needed)
{
  whenmoon_market_t **next;
  uint32_t            new_cap;
  size_t              new_sz;
  size_t              old_sz;

  if(m == NULL)
    return(FAIL);

  if(needed <= m->cap)
    return(SUCCESS);

  new_cap = m->cap == 0 ? WM_MARKET_INIT_CAP : m->cap;

  while(new_cap < needed)
    new_cap *= 2;

  new_sz = (size_t)new_cap * sizeof(*m->arr);

  if(m->arr == NULL)
  {
    next = mem_alloc("whenmoon", "market_arr", new_sz);

    if(next == NULL)
      return(FAIL);

    memset(next, 0, new_sz);
  }

  else
  {
    old_sz = (size_t)m->cap * sizeof(*m->arr);

    next = mem_realloc(m->arr, new_sz);

    if(next == NULL)
      return(FAIL);

    memset((char *)next + old_sz, 0, new_sz - old_sz);
  }

  m->arr = next;
  m->cap = new_cap;
  return(SUCCESS);
}

// Rebuild the WS subscription set with the current running markets.
// Called after every add/remove. Tearing down the prior bindings first
// is safe: wm_market_on_event shorts on st->markets == NULL (which
// stays set), but the handle close means no new events fire in
// parallel.
//
// One ws_sub per distinct exchange in the running set; products owned
// by that exchange ride a single subscribe call. The live trader's
// user-channel reconcile (wm_live_ws_resub_all) runs once at the end
// against the same partitioning.
static void
wm_market_resub_ws(whenmoon_state_t *st)
{
  whenmoon_markets_t *m;
  const char        **pid_ptrs = NULL;
  uint32_t            i;
  uint32_t            j;

  if(st == NULL || st->markets == NULL)
    return;

  m = st->markets;

  // WM-RESUB-COALESCE-1: during a bulk restore, per-add resubs are
  // suppressed; wm_market_restore issues ONE resub after its add-loop.
  // Rebuilding the whole subscription N times storms coinbase into
  // starving the feed (see finding_ws_resub_storm_starves_feed).
  if(m->defer_resub)
    return;

  // WM-MKT-ARR-UAF-1: rdlock across the whole rebuild — the arr walks below
  // must see a stable pointer block, and the pid pointers gathered into
  // pid_ptrs alias session-owned (pointer-stable) memory. This is always
  // reached OUTSIDE the writer's wrlock (add/remove call resub_ws after
  // releasing it). Deadlock-safe: everything nested here (wm_live_ws_resub_all,
  // any tick callback) only ever takes rdlock, and recursive read-locking is
  // permitted under the default reader-preferring attributes. (ws_bindings
  // writer-vs-writer serialization is a separate, pre-existing concern.)
  pthread_rwlock_rdlock(&m->arr_lock);

  for(i = 0; i < m->n_ws_bindings; i++)
  {
    if(m->ws_bindings[i].ws_sub != NULL)
      exchange_ws_unsubscribe(m->ws_bindings[i].exchange_name,
          m->ws_bindings[i].ws_sub);

    m->ws_bindings[i].ws_sub           = NULL;
    m->ws_bindings[i].exchange_name[0] = '\0';
  }
  m->n_ws_bindings = 0;

  if(m->n_markets == 0)
  {
    wm_live_ws_resub_all(st);
    pthread_rwlock_unlock(&m->arr_lock);
    return;
  }

  pid_ptrs = mem_alloc("whenmoon", "ws_pids",
      sizeof(*pid_ptrs) * m->n_markets);

  if(pid_ptrs == NULL)
  {
    clam(CLAM_INFO, WHENMOON_CTX,
        "ws resub alloc failed (no live stream)");
    wm_live_ws_resub_all(st);
    pthread_rwlock_unlock(&m->arr_lock);
    return;
  }

  // For each distinct exchange in the running set, gather its
  // product_ids and issue one subscribe. The "have we already bound
  // this exchange?" check is O(n_ws_bindings) which is bounded by 8;
  // O(N²) over the running set is fine at these scales.
  for(i = 0; i < m->n_markets; i++)
  {
    const char             *exch = m->arr[i]->exchange_name;
    wm_market_ws_binding_t *b;
    uint32_t                n_pids;
    bool                    seen;

    seen = false;
    for(j = 0; j < m->n_ws_bindings; j++)
    {
      if(strncmp(m->ws_bindings[j].exchange_name, exch,
            EXCHANGE_NAME_SZ) == 0)
      {
        seen = true;
        break;
      }
    }

    if(seen)
      continue;

    if(m->n_ws_bindings >= WM_MARKET_MAX_WS_BINDINGS)
    {
      clam(CLAM_WARN, WHENMOON_CTX,
          "ws bindings cap (%u) exceeded; %s skipped",
          (unsigned)WM_MARKET_MAX_WS_BINDINGS, exch);
      continue;
    }

    // WM-MI-1: dedup product_ids across instances — N instances of one
    // product must yield ONE subscription, not N. O(n_pids) inner scan
    // is fine at these scales (a handful of products per exchange).
    n_pids = 0;
    for(j = i; j < m->n_markets; j++)
    {
      uint32_t k;
      bool     dup;

      if(strncmp(m->arr[j]->exchange_name, exch, EXCHANGE_NAME_SZ) != 0)
        continue;

      dup = false;
      for(k = 0; k < n_pids; k++)
      {
        if(strncmp(pid_ptrs[k], m->arr[j]->product_id,
              WM_PRODUCT_ID_SZ) == 0)
        {
          dup = true;
          break;
        }
      }

      if(!dup)
        pid_ptrs[n_pids++] = m->arr[j]->product_id;
    }

    b = &m->ws_bindings[m->n_ws_bindings];
    snprintf(b->exchange_name, sizeof(b->exchange_name), "%s", exch);
    b->ws_sub = NULL;

    // Pass the binding pointer as user so wm_market_on_event can read
    // the originating exchange directly. Binding slot is stable for
    // the binding's lifetime: tear-down zeroes the slot before the
    // next rebuild reuses it, and exchange_ws_unsubscribe is
    // synchronous w.r.t. callbacks (kraken_ws_channels.c +
    // coinbase_ws_channels.c both drain inflight callbacks before
    // returning).
    if(exchange_ws_subscribe(exch,
          wm_ws_channels,
          sizeof(wm_ws_channels) / sizeof(wm_ws_channels[0]),
          pid_ptrs, n_pids,
          wm_market_on_event, b,
          &b->ws_sub) != SUCCESS || b->ws_sub == NULL)
    {
      clam(CLAM_INFO, WHENMOON_CTX,
          "ws subscribe failed for %s (no live stream)", exch);
      b->ws_sub = NULL;
    }

    m->n_ws_bindings++;
  }

  mem_free(pid_ptrs);

  wm_live_ws_resub_all(st);
  pthread_rwlock_unlock(&m->arr_lock);
}

// Fire a one-shot candle backfill into the per-market live ring.
// Used on add (fresh product) and indirectly on restore. Callback
// frees the ctx.
static void
wm_market_kick_backfill(whenmoon_state_t *st,
    const char *exchange_name, const char *product_id,
    const char *instance)
{
  wm_market_backfill_ctx_t *ctx;

  if(st == NULL || exchange_name == NULL || product_id == NULL)
    return;

  if(instance == NULL)
    instance = "";

  ctx = mem_alloc("whenmoon", "backfill_ctx", sizeof(*ctx));

  if(ctx == NULL)
    return;

  ctx->st = st;
  snprintf(ctx->exchange_name, sizeof(ctx->exchange_name), "%s",
      exchange_name);
  snprintf(ctx->product_id, sizeof(ctx->product_id), "%s", product_id);
  snprintf(ctx->instance, sizeof(ctx->instance), "%s", instance);

  // On FAIL, the exchange abstraction fires wm_market_on_candles with
  // res->err set and that callback frees ctx. Do NOT touch ctx after
  // this call. Backfill priority is honoured inside the protocol
  // adapter (the public shim does not surface a priority arg today —
  // candle fetches are always EXCHANGE_PRIO_MARKET_BACKFILL on the
  // coinbase adapter).
  (void)exchange_fetch_candles_async(ctx->exchange_name, ctx->product_id,
      EXCH_GRAN_1M, 0, 0, wm_market_on_candles, ctx);
}

// ------------------------------------------------------------------ //
// WM-MK-2: session helpers                                           //
// ------------------------------------------------------------------ //

bool
wm_market_mode_parse(const char *tok, wm_market_mode_t *out)
{
  if(tok == NULL || out == NULL)
    return(FAIL);

  if(strcasecmp(tok, "manual") == 0)
  {
    *out = WM_MARKET_MODE_MANUAL;
    return(SUCCESS);
  }

  if(strcasecmp(tok, "paper") == 0)
  {
    *out = WM_MARKET_MODE_PAPER;
    return(SUCCESS);
  }

  if(strcasecmp(tok, "real") == 0)
  {
    *out = WM_MARKET_MODE_REAL;
    return(SUCCESS);
  }

  return(FAIL);
}

const char *
wm_market_mode_name(wm_market_mode_t m)
{
  switch(m)
  {
    case WM_MARKET_MODE_MANUAL: return("manual");
    case WM_MARKET_MODE_PAPER:  return("paper");
    case WM_MARKET_MODE_REAL:   return("real");
  }

  return("?");
}

void
wm_market_session_init(wm_market_session_t *s)
{
  uint32_t i;

  if(s == NULL)
    return;

  memset(s, 0, sizeof(*s));

  // WM-WARMUP-2: new markets default to MANUAL (disarmed). `manual` is
  // the safe state — strategies see ticks and log advice but the engine
  // never auto-acts; the operator arms by switching to paper/real.
  s->mode          = WM_MARKET_MODE_MANUAL;
  s->position.side = WM_MARKET_POS_FLAT;

  for(i = 0; i < WM_MARKET_MODE_COUNT; i++)
  {
    s->stats[i].starting_cash   = WM_MARKET_DEFAULT_STARTING_CASH;
    s->stats[i].cash            = WM_MARKET_DEFAULT_STARTING_CASH;
    s->stats[i].daily_anchor_ms = 0;
  }

  s->fee_bps        = WM_MARKET_DEFAULT_FEE_BPS;
  s->slip_bps       = WM_MARKET_DEFAULT_SLIP_BPS;
  s->size_frac      = WM_MARKET_DEFAULT_SIZE_FRAC;
  s->max_notional   = WM_MARKET_DEFAULT_MAX_NOTIONAL;
  s->daily_loss_bps = WM_MARKET_DEFAULT_DAILY_LOSS_BPS;
  s->pending_cap    = WM_MARKET_DEFAULT_PENDING_CAP;
}

// WM-MKT-ARR-UAF-1: LOCK-FREE. The caller MUST hold
// `st->markets->arr_lock` (read or write) across this call AND all use of
// the returned pointer.
whenmoon_market_t *
wm_market_lookup_by_id(whenmoon_state_t *st, const char *market_id_str)
{
  whenmoon_markets_t *m;
  uint32_t            i;

  if(st == NULL || st->markets == NULL || market_id_str == NULL)
    return(NULL);

  m = st->markets;

  for(i = 0; i < m->n_markets; i++)
  {
    if(strncmp(m->arr[i]->market_id_str, market_id_str,
           WM_MARKET_ID_STR_SZ) == 0)
      return(m->arr[i]);
  }

  return(NULL);
}

bool
wm_market_exchange_has_real_mode(whenmoon_state_t *st,
    const char *exchange_name)
{
  whenmoon_markets_t *m;
  uint32_t            i;

  if(st == NULL || st->markets == NULL || exchange_name == NULL)
    return(false);

  m = st->markets;

  // WM-MKT-ARR-UAF-1: self-contained reader — takes rdlock over its own
  // walk. Returns only a bool (no escaping pointer), so releasing the lock
  // here is safe.
  pthread_rwlock_rdlock(&m->arr_lock);

  for(i = 0; i < m->n_markets; i++)
  {
    if(m->arr[i]->session.mode != WM_MARKET_MODE_REAL)
      continue;

    if(strcmp(m->arr[i]->exchange_name, exchange_name) == 0)
    {
      pthread_rwlock_unlock(&m->arr_lock);
      return(true);
    }
  }

  pthread_rwlock_unlock(&m->arr_lock);
  return(false);
}

bool
wm_market_session_snapshot(whenmoon_market_t *mk,
    wm_market_session_snapshot_t *out)
{
  wm_market_session_t *s;
  uint32_t             m;

  memset(out, 0, sizeof(*out));

  pthread_mutex_lock(&mk->lock);

  s = &mk->session;

  snprintf(out->market_id_str, sizeof(out->market_id_str), "%s",
      mk->market_id_str);
  snprintf(out->product_id, sizeof(out->product_id), "%s",
      mk->product_id);

  out->mode                = s->mode;
  out->warmup_state        = mk->warmup_state;
  out->real_cash_synced_ms = mk->real_cash_synced_ms;
  out->position            = s->position;

  for(m = 0; m < WM_MARKET_MODE_COUNT; m++)
    out->stats[m] = s->stats[m];

  // Extract the recent-fills tail per mode in oldest→newest order.
  // fills_head[m] points at the next write slot, so the newest fill
  // is at (head - 1) mod cap and the oldest of the tail is at
  // (head - count) mod cap. count is min(fills_n, RECENT_FILLS).
  for(m = 0; m < WM_MARKET_MODE_COUNT; m++)
  {
    uint32_t total = (s->fills_n[m] > (uint64_t)WM_MARKET_FILL_RING_CAP)
                   ? WM_MARKET_FILL_RING_CAP
                   : (uint32_t)s->fills_n[m];
    uint32_t count = (total > WM_MK_OBS_RECENT_FILLS)
                   ? WM_MK_OBS_RECENT_FILLS
                   : total;
    uint32_t i;

    for(i = 0; i < count; i++)
    {
      uint32_t idx = (s->fills_head[m] + WM_MARKET_FILL_RING_CAP
                      - count + i) % WM_MARKET_FILL_RING_CAP;
      out->recent_fills[m][i] = s->fills[m][idx];
    }
    out->recent_fills_n[m] = count;
  }

  out->pending_n      = s->pending_n;
  out->last_mark_px   = s->last_mark_px;
  out->last_mark_ms   = s->last_mark_ms;
  out->last_ticker_px = mk->last_px;
  out->last_ticker_ms = mk->last_tick_ms;

  out->fee_bps        = s->fee_bps;
  out->slip_bps       = s->slip_bps;
  out->size_frac      = s->size_frac;
  out->max_notional   = s->max_notional;
  out->daily_loss_bps = s->daily_loss_bps;
  out->pending_cap    = s->pending_cap;

  // WM-MK-6: pre-compute risk-adjusted metrics under the lock so
  // off-lock renderers + sweep scoring read scalars instead of
  // re-walking the equity ring (which lives behind `mk->lock`).
  out->sharpe  = wm_market_stats_sharpe(NULL,
      s->equity_samples, s->equity_n, s->equity_head);
  out->sortino = wm_market_stats_sortino(NULL,
      s->equity_samples, s->equity_n, s->equity_head);

  pthread_mutex_unlock(&mk->lock);

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Canonical id parsing / formatting                                  //
// ------------------------------------------------------------------ //

// KR-2: validate an exchange token against the live registry instead
// of a hard-coded list. A new exchange becomes acceptable the moment
// its plugin self-registers via exchange_register() — no whenmoon
// edit required.
#define WM_EXCH_LIST_CAP   8

static bool
wm_market_exchange_is_known(const char *name)
{
  char     names[WM_EXCH_LIST_CAP][EXCHANGE_NAME_SZ];
  uint32_t count = 0;
  uint32_t i;

  if(name == NULL || name[0] == '\0')
    return(FAIL);

  if(exchange_name_list(names, WM_EXCH_LIST_CAP, &count) != SUCCESS)
    return(FAIL);

  for(i = 0; i < count && i < WM_EXCH_LIST_CAP; i++)
  {
    if(strcmp(names[i], name) == 0)
      return(SUCCESS);
  }

  return(FAIL);
}

static bool
wm_market_lower_token_copy(const char *src, size_t len,
    char *out, size_t out_sz)
{
  size_t i;

  if(out == NULL || out_sz == 0 || len == 0 || len + 1 > out_sz)
    return(FAIL);

  for(i = 0; i < len; i++)
  {
    unsigned char c = (unsigned char)src[i];

    if(c == '\0' || c == '-')
      return(FAIL);

    out[i] = (char)tolower(c);
  }

  out[len] = '\0';
  return(SUCCESS);
}

bool
wm_market_parse_id(const char *id,
    char *exchange, size_t exch_sz,
    char *base,     size_t base_sz,
    char *quote,    size_t quote_sz)
{
  const char *d1;
  const char *d2;
  size_t      len;

  if(id == NULL || exchange == NULL || base == NULL || quote == NULL)
    return(FAIL);

  d1 = strchr(id, '-');

  if(d1 == NULL || d1 == id)
    return(FAIL);

  d2 = strchr(d1 + 1, '-');

  if(d2 == NULL || d2 == d1 + 1 || *(d2 + 1) == '\0')
    return(FAIL);

  // Reject a 4th token (extra dashes).
  if(strchr(d2 + 1, '-') != NULL)
    return(FAIL);

  len = (size_t)(d1 - id);

  if(wm_market_lower_token_copy(id, len, exchange, exch_sz) != SUCCESS)
    return(FAIL);

  len = (size_t)(d2 - (d1 + 1));

  if(wm_market_lower_token_copy(d1 + 1, len, base, base_sz) != SUCCESS)
    return(FAIL);

  len = strlen(d2 + 1);

  if(wm_market_lower_token_copy(d2 + 1, len, quote, quote_sz) != SUCCESS)
    return(FAIL);

  if(wm_market_exchange_is_known(exchange) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

void
wm_market_format_id(const char *exchange, const char *base,
    const char *quote, char *out, size_t out_sz)
{
  size_t pos = 0;
  const char *parts[3];
  uint32_t i;
  uint32_t j;

  if(out == NULL || out_sz == 0)
    return;

  out[0] = '\0';

  if(exchange == NULL || base == NULL || quote == NULL)
    return;

  parts[0] = exchange;
  parts[1] = base;
  parts[2] = quote;

  for(i = 0; i < 3; i++)
  {
    if(i > 0)
    {
      if(pos + 1 >= out_sz) { out[out_sz - 1] = '\0'; return; }
      out[pos++] = '-';
    }

    for(j = 0; parts[i][j] != '\0'; j++)
    {
      if(pos + 1 >= out_sz) { out[out_sz - 1] = '\0'; return; }
      out[pos++] = (char)tolower((unsigned char)parts[i][j]);
    }
  }

  out[pos] = '\0';
}

// WM-MI-1: validate an instance label. Non-empty labels must be
// [a-z0-9_] and fit WM_INSTANCE_LABEL_SZ-1 chars. The empty label is
// valid (the sole/legacy instance). Rejects '-'/'@'/'.'/whitespace and
// uppercase, which would corrupt the dash-split, the '@' split, or KV
// path segments.
static bool
wm_market_validate_label(const char *label)
{
  size_t i;

  if(label == NULL)
    return(FAIL);

  if(strlen(label) >= WM_INSTANCE_LABEL_SZ)
    return(FAIL);

  for(i = 0; label[i] != '\0'; i++)
  {
    unsigned char c = (unsigned char)label[i];

    if(!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
      return(FAIL);
  }

  return(SUCCESS);
}

bool
wm_market_parse_instance_id(const char *id,
    char *exchange, size_t exch_sz,
    char *base,     size_t base_sz,
    char *quote,    size_t quote_sz,
    char *instance, size_t inst_sz)
{
  const char *at;
  char        triple[WM_MARKET_ID_STR_SZ];
  size_t      triple_len;

  if(id == NULL || instance == NULL || inst_sz == 0)
    return(FAIL);

  instance[0] = '\0';

  // Split on the LAST '@' so a label is unambiguous even if the triple
  // never contains one (it can't — '@' is rejected everywhere else).
  at = strrchr(id, '@');

  if(at != NULL)
  {
    size_t label_len = strlen(at + 1);

    if(label_len == 0 || label_len >= inst_sz
        || label_len >= WM_INSTANCE_LABEL_SZ)
      return(FAIL);

    snprintf(instance, inst_sz, "%s", at + 1);

    if(wm_market_validate_label(instance) != SUCCESS)
      return(FAIL);

    triple_len = (size_t)(at - id);

    if(triple_len == 0 || triple_len >= sizeof(triple))
      return(FAIL);

    memcpy(triple, id, triple_len);
    triple[triple_len] = '\0';
    id = triple;
  }

  return(wm_market_parse_id(id, exchange, exch_sz, base, base_sz,
      quote, quote_sz));
}

bool
wm_market_format_instance_id(const char *exchange, const char *base,
    const char *quote, const char *instance, char *out, size_t out_sz)
{
  size_t len;

  if(out == NULL || out_sz == 0)
    return(FAIL);

  wm_market_format_id(exchange, base, quote, out, out_sz);

  len = strlen(out);

  // wm_market_format_id truncates silently on overflow; a valid triple
  // is well under WM_MARKET_ID_STR_SZ, but guard anyway.
  if(len == 0)
    return(FAIL);

  if(instance == NULL || instance[0] == '\0')
    return(SUCCESS);

  // "@<label>" must fit: len + 1 ('@') + strlen(label) + 1 (NUL).
  if(len + 1 + strlen(instance) + 1 > out_sz)
  {
    out[len] = '\0';
    return(FAIL);
  }

  out[len] = '@';
  snprintf(out + len + 1, out_sz - len - 1, "%s", instance);

  return(SUCCESS);
}

void
wm_market_wire_symbol(const char *base, const char *quote,
    char *out, size_t out_sz)
{
  size_t i;
  int    n;

  if(out == NULL || out_sz == 0)
    return;

  if(base == NULL || quote == NULL)
  {
    out[0] = '\0';
    return;
  }

  n = snprintf(out, out_sz, "%s-%s", base, quote);

  if(n < 0 || (size_t)n >= out_sz)
  {
    out[0] = '\0';
    return;
  }

  for(i = 0; out[i] != '\0'; i++)
    out[i] = (char)toupper((unsigned char)out[i]);
}

// ------------------------------------------------------------------ //
// Enabled-flag persistence on wm_market                              //
// ------------------------------------------------------------------ //

static bool
wm_market_set_enabled(int32_t market_id, bool enabled)
{
  db_result_t *res = NULL;
  char         sql[192];
  bool         ok  = FAIL;
  int          n;

  if(market_id < 0)
    return(FAIL);

  n = snprintf(sql, sizeof(sql),
      "UPDATE wm_market SET enabled = %s WHERE id = %" PRId32,
      enabled ? "TRUE" : "FALSE", market_id);

  if(n < 0 || (size_t)n >= sizeof(sql))
    return(FAIL);

  res = db_result_alloc();

  if(res == NULL)
    return(FAIL);

  if(db_query(sql, res) == SUCCESS && res->ok)
    ok = SUCCESS;

  else
    clam(CLAM_WARN, WHENMOON_CTX,
        "wm_market enabled flip failed (market_id=%" PRId32
        " enabled=%d): %s",
        market_id, (int)enabled,
        res->error[0] != '\0' ? res->error : "(no driver error)");

  db_result_free(res);
  return(ok);
}

// ------------------------------------------------------------------ //
// Callbacks                                                          //
// ------------------------------------------------------------------ //

void
wm_market_on_candles(const exchange_candles_result_t *res, void *user)
{
  wm_market_backfill_ctx_t *ctx = user;
  whenmoon_market_t        *mk;
  uint32_t                  i;
  uint32_t                  synthesized = 0;

  if(ctx == NULL)
    return;

  if(res == NULL || res->err[0] != '\0')
  {
    clam(CLAM_INFO, WHENMOON_CTX,
        "market %s: backfill failed: %s",
        ctx->product_id,
        (res != NULL && res->err[0] != '\0') ? res->err : "(no result)");
    mem_free(ctx);
    return;
  }

  if(ctx->st == NULL || ctx->st->markets == NULL)
  {
    mem_free(ctx);
    return;
  }

  // WM-MKT-ARR-UAF-1: hold rdlock across find→lock→replay. This keeps the
  // resolved session alive for the (potentially long) replay; it only ever
  // blocks a concurrent remove of a market — ticks (other readers) run in
  // parallel.
  pthread_rwlock_rdlock(&ctx->st->markets->arr_lock);

  mk = wm_market_find_instance(ctx->st->markets, ctx->exchange_name,
      ctx->product_id, ctx->instance);

  if(mk == NULL)
  {
    pthread_rwlock_unlock(&ctx->st->markets->arr_lock);
    mem_free(ctx);
    return;
  }

  pthread_mutex_lock(&mk->lock);

  // Coinbase returns candles newest-first; replay oldest-first so the
  // aggregator's idempotency check (skip ts <= last_close_ms) prunes
  // duplicates correctly when a later REST page overlaps a prior
  // warm-up. The replay path drives the cascade so 5m/15m/1h/4h/1d
  // grains backfill from this single 1m feed. exchange_candle_t carries
  // bucket open in ms, so bar close = open + 60s.
  for(i = res->count; i > 0; i--)
  {
    const exchange_candle_t *src = &res->rows[i - 1];
    wm_candle_full_t         bar;

    memset(&bar, 0, sizeof(bar));
    bar.ts_close_ms = src->ts_open_ms + 60 * 1000;
    bar.open        = src->open;
    bar.high        = src->high;
    bar.low         = src->low;
    bar.close       = src->close;
    bar.volume      = src->volume;

    synthesized += wm_aggregator_replay_bar(mk, WM_GRAN_1M, &bar);
  }

  pthread_mutex_unlock(&mk->lock);
  pthread_rwlock_unlock(&ctx->st->markets->arr_lock);

  clam(CLAM_INFO, WHENMOON_CTX,
      "market %s: %u candles backfilled (synth=%u)",
      ctx->product_id, res->count, synthesized);

  mem_free(ctx);
}

void
wm_market_on_event(const exchange_ws_event_t *ev, void *user)
{
  const wm_market_ws_binding_t *binding = user;
  whenmoon_state_t             *st;
  whenmoon_market_t            *mk;
  const char                   *exch;

  if(ev == NULL || binding == NULL)
    return;

  exch = binding->exchange_name;
  st   = whenmoon_get_state();

  if(st == NULL || st->markets == NULL || exch == NULL || exch[0] == '\0')
    return;

  switch(ev->channel)
  {
    case EXCH_WS_TICKER:
    {
      const exchange_ws_ticker_t *t = &ev->payload.ticker;
      uint32_t                    i;

      // WM-MI-1: fan the tick out to EVERY instance on this
      // (exchange, product_id) — each holds its own session/position, so
      // one wire tick must advance all of them. One lock acquire per
      // matching market, released before the next: no nested locking.
      // WM-MKT-ARR-UAF-1: rdlock over the whole fan-out — arr stays stable
      // and no session is freed by a concurrent remove while we hold it.
      pthread_rwlock_rdlock(&st->markets->arr_lock);
      for(i = 0; i < st->markets->n_markets; i++)
      {
        mk = st->markets->arr[i];

        if(strncmp(mk->exchange_name, exch, EXCHANGE_NAME_SZ) != 0 ||
           strncmp(mk->product_id, t->product_id, WM_PRODUCT_ID_SZ) != 0)
          continue;

        pthread_mutex_lock(&mk->lock);
        mk->last_px      = t->price;
        mk->last_tick_ms = t->time_ms;
        pthread_mutex_unlock(&mk->lock);
      }
      pthread_rwlock_unlock(&st->markets->arr_lock);

      clam(CLAM_DEBUG2, WHENMOON_CTX,
          "tick %s/%s px=%.8g",
          exch, t->product_id, t->price);
      break;
    }

    case EXCH_WS_TRADES:
    {
      const exchange_ws_match_t *m = &ev->payload.match;
      uint32_t                   i;

      // WM-MI-1: fan the trade out to EVERY instance on this
      // (exchange, product_id). Each instance owns its aggregator, so it
      // must see the same trade to advance its own grain cascade +
      // position. One lock per matching market; no nested locking.
      // WM-MKT-ARR-UAF-1: rdlock over the whole fan-out (see TICKER).
      pthread_rwlock_rdlock(&st->markets->arr_lock);
      for(i = 0; i < st->markets->n_markets; i++)
      {
        mk = st->markets->arr[i];

        if(strncmp(mk->exchange_name, exch, EXCHANGE_NAME_SZ) != 0 ||
           strncmp(mk->product_id, m->product_id, WM_PRODUCT_ID_SZ) != 0)
          continue;

        pthread_mutex_lock(&mk->lock);

        mk->last_px      = m->price;
        mk->last_tick_ms = m->time_ms;

        // Drive the multi-grain cascade. Aggregator owns the close +
        // indicator pass; this hot path stays under one lock acquire.
        if(mk->aggregator != NULL)
          wm_aggregator_on_trade(mk, m->time_ms, m->price, m->size);

        pthread_mutex_unlock(&mk->lock);
      }
      pthread_rwlock_unlock(&st->markets->arr_lock);

      clam(CLAM_DEBUG2, WHENMOON_CTX,
          "match %s/%s %s px=%.8g sz=%.8g",
          exch, m->product_id, m->side, m->price, m->size);
      break;
    }

    default:
      break;
  }
}

// ------------------------------------------------------------------ //
// Init / destroy                                                     //
// ------------------------------------------------------------------ //

bool
wm_market_init(whenmoon_state_t *st)
{
  whenmoon_markets_t *m;

  if(st == NULL)
    return(FAIL);

  m = mem_alloc("whenmoon", "markets", sizeof(*m));

  if(m == NULL)
    return(FAIL);

  memset(m, 0, sizeof(*m));

  // WM-MKT-ARR-UAF-1: default (reader-preferring) attributes — see the
  // arr_lock field comment. Init cannot fail on a zeroed attr, but honor
  // the return so a hostile pthread config surfaces rather than silently
  // leaving an unusable lock.
  if(pthread_rwlock_init(&m->arr_lock, NULL) != 0)
  {
    mem_free(m);
    return(FAIL);
  }

  st->markets = m;

  return(SUCCESS);
}

void
wm_market_destroy(whenmoon_state_t *st)
{
  whenmoon_markets_t *m;
  uint32_t            i;

  if(st == NULL || st->markets == NULL)
    return;

  m = st->markets;

  for(i = 0; i < m->n_ws_bindings; i++)
  {
    if(m->ws_bindings[i].ws_sub != NULL)
      exchange_ws_unsubscribe(m->ws_bindings[i].exchange_name,
          m->ws_bindings[i].ws_sub);
    m->ws_bindings[i].ws_sub           = NULL;
    m->ws_bindings[i].exchange_name[0] = '\0';
  }
  m->n_ws_bindings = 0;

  if(m->arr != NULL)
  {
    for(i = 0; i < m->n_markets; i++)
    {
      whenmoon_market_t *mk = m->arr[i];

      if(mk == NULL)
        continue;

      if(mk->aggregator != NULL)
        wm_aggregator_destroy(mk);

      pthread_mutex_destroy(&mk->lock);

      // WM-MKT-ARR-UAF-1: each session is individually heap-owned.
      mem_free(mk);
    }

    mem_free(m->arr);   // the pointer block itself
  }

  pthread_rwlock_destroy(&m->arr_lock);

  // Block new callbacks from finding the state via ->markets before we
  // free. In-flight backfill callbacks will see st->markets == NULL and
  // short-circuit without a deref.
  st->markets = NULL;

  mem_free(m);
}

// ------------------------------------------------------------------ //
// Dynamic add / remove / restore                                     //
// ------------------------------------------------------------------ //

bool
wm_market_add(whenmoon_state_t *st,
    const char *exchange, const char *base, const char *quote,
    const char *product_id, const char *instance, bool persist,
    char *err, size_t err_cap)
{
  whenmoon_markets_t *m;
  whenmoon_market_t  *mk;
  int32_t             market_id;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(instance == NULL)
    instance = "";

  if(st == NULL || st->markets == NULL || exchange == NULL ||
     base == NULL || quote == NULL || product_id == NULL)
  {
    if(err != NULL) snprintf(err, err_cap, "bad args");
    return(FAIL);
  }

  // WM-MI-1: reject a malformed instance label up front (the id parser
  // already validates on the command path, but wm_market_restore and
  // internal callers reach here directly).
  if(wm_market_validate_label(instance) != SUCCESS)
  {
    if(err != NULL) snprintf(err, err_cap, "bad instance label");
    return(FAIL);
  }

  m = st->markets;

  // WM-MI-1 / WM-MKT-ARR-UAF-1: fast-path dedup on the
  // (exchange, product, instance) triple. The authoritative re-check runs
  // under the write lock below (closing the add/add TOCTOU); this cheap
  // read-locked probe just avoids building a whole session for the common
  // "already present" no-op.
  pthread_rwlock_rdlock(&m->arr_lock);
  if(wm_market_find_instance(m, exchange, product_id, instance) != NULL)
  {
    pthread_rwlock_unlock(&m->arr_lock);
    return(SUCCESS);   // already present; benign no-op
  }
  pthread_rwlock_unlock(&m->arr_lock);

  market_id = wm_market_lookup_or_create(exchange, base, quote, product_id);

  if(market_id < 0)
  {
    if(err != NULL) snprintf(err, err_cap, "market registry failed");
    return(FAIL);
  }

  // WM-MKT-ARR-UAF-1: build + fully initialise the session PRIVATELY. It is
  // not yet linked into arr, so no reader can observe it. Only once it is
  // complete do we take the write lock and publish it. A failure on this
  // path is a simple free — no arr touch, no rollback of a live slot.
  mk = mem_alloc("whenmoon", "market", sizeof(*mk));

  if(mk == NULL)
  {
    if(err != NULL) snprintf(err, err_cap, "alloc failed");
    return(FAIL);
  }

  memset(mk, 0, sizeof(*mk));
  snprintf(mk->exchange_name, sizeof(mk->exchange_name), "%s", exchange);
  snprintf(mk->product_id,    sizeof(mk->product_id),    "%s", product_id);
  snprintf(mk->instance,      sizeof(mk->instance),      "%s", instance);

  // WM-MI-1: the canonical id carries "@<instance>" for a non-empty
  // label; the empty-label form is byte-identical to the legacy triple.
  if(wm_market_format_instance_id(exchange, base, quote, instance,
         mk->market_id_str, sizeof(mk->market_id_str)) != SUCCESS)
  {
    if(err != NULL) snprintf(err, err_cap, "market id too long");
    mem_free(mk);
    return(FAIL);
  }

  mk->market_id = market_id;
  pthread_mutex_init(&mk->lock, NULL);

  // WM-MK-2: install the per-market position model with default-init
  // values. Lazy KV refresh replaces cached params on first engine call;
  // persistence restore overwrites the session when a wm_market_state row
  // exists for this market_id.
  wm_market_session_init(&mk->session);

  // Aggregator must be in place before the market is published so the very
  // first WS event fan-out can already feed it.
  if(wm_aggregator_init(mk, WM_AGG_DEFAULT_HISTORY_1D) != SUCCESS)
  {
    pthread_mutex_destroy(&mk->lock);
    mem_free(mk);
    if(err != NULL) snprintf(err, err_cap, "aggregator init failed");
    return(FAIL);
  }

  // WM-MK-2: lazy-register + cache per-market KV defaults so operator
  // /set kv values land in the session immediately. Idempotent across
  // restarts (kv_register short-circuits on existing keys; KV values
  // hydrated by kv_load survive); the cached values are also overwritten
  // by wm_market_persist_restore_all when a state row exists.
  wm_market_session_refresh_kv(mk);

  if(persist && wm_market_set_enabled(market_id, true) != SUCCESS)
  {
    wm_aggregator_destroy(mk);
    pthread_mutex_destroy(&mk->lock);
    mem_free(mk);
    if(err != NULL) snprintf(err, err_cap, "DB enable failed");
    return(FAIL);
  }

  // WM-MKT-ARR-UAF-1: PUBLISH under the write lock — the commit point.
  pthread_rwlock_wrlock(&m->arr_lock);

  // Re-check the dedup key: another worker may have published the same
  // triple while we were building ours.
  if(wm_market_find_instance(m, exchange, product_id, instance) != NULL)
  {
    pthread_rwlock_unlock(&m->arr_lock);
    // Lost the race. Discard our private session. The product-level enabled
    // flag we may have set is already true for the winner — nothing to undo.
    wm_aggregator_destroy(mk);
    pthread_mutex_destroy(&mk->lock);
    mem_free(mk);
    return(SUCCESS);
  }

  if(wm_market_grow(m, m->n_markets + 1) != SUCCESS)
  {
    pthread_rwlock_unlock(&m->arr_lock);
    if(persist)
      (void)wm_market_set_enabled(market_id, false);
    wm_aggregator_destroy(mk);
    pthread_mutex_destroy(&mk->lock);
    mem_free(mk);
    if(err != NULL) snprintf(err, err_cap, "alloc failed");
    return(FAIL);
  }

  m->arr[m->n_markets] = mk;
  m->n_markets++;

  pthread_rwlock_unlock(&m->arr_lock);

  // ---- Post-publish. mk is pointer-stable; these callees take arr_lock
  //      themselves, so they MUST run outside the write section. ----

  // WM-MI-2: write the initial per-instance state row (enabled=TRUE) the
  // moment the instance joins the running set, so a restart before the
  // first fill still restores it. The freshly-init session overwrites any
  // stale disabled row from a prior stop (ON CONFLICT), so no old ledger
  // resurrects. Restores pass persist=false and rely on
  // wm_market_persist_restore_all to rehydrate instead.
  if(persist)
  {
    pthread_mutex_lock(&mk->lock);
    (void)wm_market_persist_locked(mk);
    pthread_mutex_unlock(&mk->lock);
  }

  wm_market_resub_ws(st);
  wm_market_kick_backfill(st, exchange, product_id, instance);

  // WM-WARMUP-2 / WM-MI-3: auto-attach the declared strategy binding,
  // then begin the warmup lifecycle. The binding KV names the ONE
  // strategy this market runs; warmup depth derives from its declared
  // min_history. An empty binding = feed-only (manual hand-trade) and
  // wm_market_warmup_begin promotes straight to READY.
  {
    char        binding_key[160];
    const char *binding;

    snprintf(binding_key, sizeof(binding_key),
        "plugin.whenmoon.market.%s.strategy", mk->market_id_str);

    // Register before reading: kv_set (used by the attach/detach binding
    // sync) rejects unregistered keys, and kv_register adopts any
    // DB-persisted value so a restored binding is visible here. Idempotent
    // across restarts.
    (void)kv_register(binding_key, KV_STR, "", NULL, NULL,
        "strategy auto-attached + warmed on market start (\"\" = feed-only)");

    binding = kv_get_str(binding_key);

    if(binding != NULL && binding[0] != '\0')
    {
      char name[WM_STRATEGY_NAME_SZ];
      char rerr[128];

      // Copy out of the KV-owned pointer before the attach, which
      // issues its own KV writes that could invalidate `binding`.
      snprintf(name, sizeof(name), "%s", binding);
      rerr[0] = '\0';

      if(wm_strategy_attach(st, mk->market_id_str, name,
             rerr, sizeof(rerr)) != WM_ATTACH_OK)
        clam(CLAM_INFO, WHENMOON_CTX,
            "market %s: binding attach '%s' failed: %s",
            mk->market_id_str, name, rerr[0] != '\0' ? rerr : "?");
    }
  }

  wm_market_warmup_begin(st, mk);

  clam(CLAM_INFO, WHENMOON_CTX,
      "market %s started (market_id=%" PRId32 "%s)",
      mk->market_id_str, market_id, persist ? "" : ", restored");

  return(SUCCESS);
}

bool
wm_market_remove(whenmoon_state_t *st,
    const char *exchange, const char *product_id, const char *instance,
    bool persist, bool *was_present, char *err, size_t err_cap)
{
  whenmoon_markets_t *m;
  whenmoon_market_t  *mk;
  char                id_str[WM_MARKET_ID_STR_SZ];
  char                inst_str[WM_INSTANCE_LABEL_SZ];
  uint32_t            idx;
  int32_t             market_id;
  bool                product_still_live = false;

  if(was_present != NULL)
    *was_present = false;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(instance == NULL)
    instance = "";

  if(st == NULL || st->markets == NULL || exchange == NULL
      || product_id == NULL)
  {
    if(err != NULL) snprintf(err, err_cap, "bad args");
    return(FAIL);
  }

  m  = st->markets;

  // WM-MKT-ARR-UAF-1: take the write lock for the whole structural edit.
  // Under wrlock no reader holds any interior mk and none can find one, so
  // once the session is unlinked it is private — safe to tear down and free
  // after the lock with no barrier.
  pthread_rwlock_wrlock(&m->arr_lock);

  // WM-MI-1: disambiguate which instance's session to drop.
  mk = wm_market_find_instance(m, exchange, product_id, instance);

  if(mk == NULL)
  {
    pthread_rwlock_unlock(&m->arr_lock);
    return(SUCCESS);  // benign no-op; was_present stays false
  }

  if(was_present != NULL)
    *was_present = true;

  // Locate the pointer slot holding this session.
  for(idx = 0; idx < m->n_markets; idx++)
    if(m->arr[idx] == mk)
      break;

  market_id = mk->market_id;
  snprintf(id_str,   sizeof(id_str),   "%s", mk->market_id_str);
  snprintf(inst_str, sizeof(inst_str), "%s", mk->instance);

  // Unlink: compact the POINTER tail over the removed slot (sessions never
  // move — only pointers shift). Overlapping memmove is safe; clear the
  // vacated tail pointer.
  if(idx + 1 < m->n_markets)
    memmove(&m->arr[idx], &m->arr[idx + 1],
        sizeof(*m->arr) * (m->n_markets - idx - 1));

  m->arr[m->n_markets - 1] = NULL;
  m->n_markets--;

  // WM-MI-2: does any OTHER instance of this product still run? The slot is
  // already compacted out, so a remaining match means the product must stay
  // enabled (downloads/candles) even though this instance is leaving.
  {
    uint32_t k;

    for(k = 0; k < m->n_markets; k++)
    {
      if(m->arr[k]->market_id == market_id)
      {
        product_still_live = true;
        break;
      }
    }
  }

  pthread_rwlock_unlock(&m->arr_lock);

  // ---- mk is now unlinked + unreferenced. Tear it down OUTSIDE the lock
  //      (these callees may take arr_lock themselves). ----

  // Auto-detach any strategy attachments bound to this market. Done before
  // freeing so finalize_fn callbacks fire while id_str is still valid; the
  // ctx->mkt pointer is invalidated regardless.
  wm_strategy_detach_market(st, id_str);

  // Aggregator locks the session mutex internally, so destroy it before
  // pthread_mutex_destroy(&mk->lock).
  if(mk->aggregator != NULL)
    wm_aggregator_destroy(mk);

  pthread_mutex_destroy(&mk->lock);
  mem_free(mk);   // WM-MKT-ARR-UAF-1: the session is individually heap-owned

  if(persist && market_id >= 0)
  {
    // Product-level enabled flag (downloads/candles) only flips off when
    // the LAST instance of the product stops.
    if(!product_still_live &&
        wm_market_set_enabled(market_id, false) != SUCCESS)
    {
      // In-memory removal already committed; leaving enabled=true would
      // cause the product to resurrect on next restart. Log and keep going;
      // the operator can `/whenmoon market stop` again.
      if(err != NULL)
        snprintf(err, err_cap,
            "DB disable failed (live set already updated)");
    }

    // WM-MI-2: drop THIS instance out of the per-instance running set
    // (enabled=FALSE) so it no longer restores, while peers on the same
    // product and this instance's final P&L are preserved.
    (void)wm_market_persist_disable(market_id, inst_str);
  }

  wm_market_resub_ws(st);

  clam(CLAM_INFO, WHENMOON_CTX,
      "market %s stopped (market_id=%" PRId32 "%s)",
      id_str, market_id, persist ? "" : ", untracked");

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Restore                                                            //
// ------------------------------------------------------------------ //

bool
wm_market_restore(whenmoon_state_t *st)
{
  db_result_t *res = NULL;
  uint32_t     i;
  uint32_t     n_restored = 0;
  bool         ok = SUCCESS;

  if(st == NULL)
    return(FAIL);

  res = db_result_alloc();

  if(res == NULL)
    return(FAIL);

  // WM-MI-2: the per-instance running set lives in wm_market_state
  // (enabled=TRUE), joined to wm_market for the product coordinates. One
  // row per running instance — a product with three instances yields
  // three sessions, each re-added under its own label.
  if(db_query(
         "SELECT m.exchange, m.base_asset, m.quote_asset,"
         " m.exchange_symbol, s.instance"
         "  FROM wm_market_state s"
         "  JOIN wm_market m ON s.market_id = m.id"
         " WHERE s.enabled = TRUE"
         " ORDER BY m.id, s.instance", res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "market restore query failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");
    ok = FAIL;
    goto out;
  }

  // WM-RESUB-COALESCE-1: suppress the per-add WS resubs across the whole
  // restore. Each wm_market_add below would otherwise rebuild the entire
  // per-exchange subscription, so N instances = N full unsub/resub cycles
  // that storm coinbase into starving the live feed. We issue ONE resub
  // after the loop instead. The query-fail `goto out` above is BEFORE this
  // flag is set, so no cleanup is needed on that path.
  st->markets->defer_resub = true;

  for(i = 0; i < res->rows; i++)
  {
    const char *exch  = db_result_get(res, i, 0);
    const char *base  = db_result_get(res, i, 1);
    const char *quote = db_result_get(res, i, 2);
    const char *sym   = db_result_get(res, i, 3);
    const char *inst  = db_result_get(res, i, 4);

    if(exch == NULL || base == NULL || quote == NULL || sym == NULL)
      continue;

    if(inst == NULL)
      inst = "";

    // Re-add under the persisted instance label. persist=false is
    // critical: it suppresses wm_market_add's initial-row upsert, which
    // would otherwise enqueue a CLEAN (freshly-init) session and race
    // wm_market_persist_restore_all to clobber the persisted P&L. The
    // mode + full session ledger are rehydrated by restore_all next; the
    // market sits in warmup until then, so no trade fires meanwhile.
    if(wm_market_add(st, exch, base, quote, sym, inst,
           false, NULL, 0) != SUCCESS)
    {
      clam(CLAM_INFO, WHENMOON_CTX,
          "market %s@%s restore failed — skipping", sym, inst);
      continue;
    }

    n_restored++;
  }

  // WM-RESUB-COALESCE-1: clear the gate and issue ONE resub for the whole
  // restored set (one subscribe per exchange + the user-channel sub),
  // instead of the N cycles the per-add path would have fired.
  st->markets->defer_resub = false;
  wm_market_resub_ws(st);

  clam(CLAM_INFO, WHENMOON_CTX,
      "%u running market(s) restored", n_restored);

out:
  if(res != NULL) db_result_free(res);

  return(ok);
}

// ------------------------------------------------------------------ //
// WM-MK-5: synthetic backtest markets                                 //
// ------------------------------------------------------------------ //

bool
wm_market_create_synthetic(const char *market_id_str,
    const whenmoon_market_t *src, whenmoon_market_t **out_mk,
    char *errbuf, size_t errbuf_sz)
{
  whenmoon_market_t *mk;

  if(out_mk != NULL)
    *out_mk = NULL;

  if(market_id_str == NULL || src == NULL || out_mk == NULL)
  {
    if(errbuf != NULL && errbuf_sz > 0)
      snprintf(errbuf, errbuf_sz, "bad args");
    return(FAIL);
  }

  mk = mem_alloc(WHENMOON_CTX, "market_synth", sizeof(*mk));

  if(mk == NULL)
  {
    if(errbuf != NULL && errbuf_sz > 0)
      snprintf(errbuf, errbuf_sz, "oom");
    return(FAIL);
  }

  memset(mk, 0, sizeof(*mk));

  snprintf(mk->market_id_str, sizeof(mk->market_id_str), "%s",
      market_id_str);
  snprintf(mk->product_id, sizeof(mk->product_id), "%s", src->product_id);
  snprintf(mk->exchange_name, sizeof(mk->exchange_name), "%s",
      src->exchange_name);
  mk->market_id = -1;     // synth has no DB row

  // Share grain rings — POINTERS ARE BORROWED, never copied. The
  // source market owns the rings; the synth market reads through them
  // and MUST NOT free or extend them. WM-BT-2 relies on this: when
  // the source is a `wm_bt_file_open` mmap'd snapshot, the rings are
  // read-only pages mapped from disk and shared across every sweep
  // worker. The source market's lifetime spans the entire sweep;
  // iteration is a strict subset, so the pointers stay valid.
  memcpy(mk->grain_arr, src->grain_arr, sizeof(mk->grain_arr));
  memcpy(mk->grain_n,   src->grain_n,   sizeof(mk->grain_n));
  memcpy(mk->grain_cap, src->grain_cap, sizeof(mk->grain_cap));

  mk->aggregator   = NULL;
  mk->last_px      = src->last_px;
  mk->last_tick_ms = src->last_tick_ms;

  pthread_mutex_init(&mk->lock, NULL);

  wm_market_session_init(&mk->session);
  mk->session.mode = WM_MARKET_MODE_PAPER;

  // WM-WARMUP-2: synthetic markets carry a full pre-loaded ring (the
  // backtest snapshot), so they are warm by construction. Force READY so
  // the engine's readiness gate never suppresses backtest fills.
  mk->warmup_state = WM_WARM_READY;

  *out_mk = mk;
  return(SUCCESS);
}

void
wm_market_destroy_synthetic(whenmoon_market_t *mk)
{
  if(mk == NULL)
    return;

  pthread_mutex_destroy(&mk->lock);
  mem_free(mk);
}
