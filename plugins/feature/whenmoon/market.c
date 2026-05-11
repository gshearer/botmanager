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
#include "dl_schema.h"

#include "db.h"
#include "task.h"
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
// state is destroyed before coinbase fires the callback, the callback
// still runs (coinbase owns its request queue) and must not touch the
// state. We therefore track the state pointer but also the market
// product_id; on callback we re-lookup the product id in the live
// state to decide whether to commit.
typedef struct
{
  whenmoon_state_t   *st;
  char                product_id[COINBASE_PRODUCT_ID_SZ];
} wm_market_backfill_ctx_t;

static const coinbase_ws_channel_t wm_ws_channels[] = {
  COINBASE_CH_HEARTBEAT,
  COINBASE_CH_TICKER,
  COINBASE_CH_MATCHES,
};

// ------------------------------------------------------------------ //
// Container helpers                                                  //
// ------------------------------------------------------------------ //

static whenmoon_market_t *
wm_market_find(whenmoon_markets_t *m, const char *product_id)
{
  uint32_t i;

  if(m == NULL || product_id == NULL)
    return(NULL);

  for(i = 0; i < m->n_markets; i++)
  {
    if(strncmp(m->arr[i].product_id, product_id,
           COINBASE_PRODUCT_ID_SZ) == 0)
      return(&m->arr[i]);
  }

  return(NULL);
}

// Grow `arr` to hold at least `needed` slots. Existing rows are
// copied by mem_realloc; freshly-added tail bytes are zeroed so their
// pthread_mutex_t fields are in a defined "not yet initialised" state
// until wm_market_add fills them. Returns SUCCESS or FAIL.
static bool
wm_market_grow(whenmoon_markets_t *m, uint32_t needed)
{
  whenmoon_market_t *next;
  uint32_t           new_cap;
  size_t             new_sz;
  size_t             old_sz;

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

// Rebuild the WS subscription with the current product set. Called
// after every add/remove. Unsubscribing the old handle first is safe:
// wm_market_on_event shorts on st->markets == NULL, which stays set,
// but the handle close means no new events will fire in parallel.
static void
wm_market_resub_ws(whenmoon_state_t *st)
{
  whenmoon_markets_t *m;
  const char        **pid_ptrs = NULL;
  uint32_t            i;

  if(st == NULL || st->markets == NULL)
    return;

  m = st->markets;

  if(m->ws_sub != NULL)
  {
    coinbase_ws_unsubscribe(m->ws_sub);
    m->ws_sub = NULL;
  }

  if(m->n_markets == 0)
    return;

  pid_ptrs = mem_alloc("whenmoon", "ws_pids",
      sizeof(*pid_ptrs) * m->n_markets);

  if(pid_ptrs == NULL)
  {
    clam(CLAM_INFO, WHENMOON_CTX,
        "ws resub alloc failed (no live stream)");
    return;
  }

  for(i = 0; i < m->n_markets; i++)
    pid_ptrs[i] = m->arr[i].product_id;

  m->ws_sub = coinbase_ws_subscribe(wm_ws_channels,
      sizeof(wm_ws_channels) / sizeof(wm_ws_channels[0]),
      pid_ptrs, m->n_markets,
      wm_market_on_event, st);

  if(m->ws_sub == NULL)
    clam(CLAM_INFO, WHENMOON_CTX,
        "ws subscribe failed (no live stream)");

  // WM-LT-8-B3: piggyback the user-channel resub on the same product
  // set so live-trader fill events reach the engine.
  wm_live_ws_resub(st, pid_ptrs, m->n_markets);

  mem_free(pid_ptrs);
}

// Fire a one-shot candle backfill into the per-market live ring.
// Used on add (fresh product) and indirectly on restore. Callback
// frees the ctx.
static void
wm_market_kick_backfill(whenmoon_state_t *st, const char *product_id)
{
  wm_market_backfill_ctx_t *ctx;

  if(st == NULL || product_id == NULL)
    return;

  ctx = mem_alloc("whenmoon", "backfill_ctx", sizeof(*ctx));

  if(ctx == NULL)
    return;

  ctx->st = st;
  snprintf(ctx->product_id, sizeof(ctx->product_id), "%s", product_id);

  // On FAIL, coinbase invokes wm_market_on_candles with res->err set
  // and that callback frees ctx. Do NOT touch ctx after this call.
  // EX-1: market backfill is system-initiated catchup at start, route
  // through the exchange abstraction at the backfill priority.
  (void)coinbase_fetch_candles_async(ctx->product_id, COINBASE_GRAN_1M,
      0, 0, EXCHANGE_PRIO_MARKET_BACKFILL,
      wm_market_on_candles, ctx);
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

  s->mode          = WM_MARKET_MODE_PAPER;
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
    if(strncmp(m->arr[i].market_id_str, market_id_str,
           WM_MARKET_ID_STR_SZ) == 0)
      return(&m->arr[i]);
  }

  return(NULL);
}

whenmoon_market_t *
wm_market_lookup_by_product_id(whenmoon_state_t *st, const char *product_id)
{
  whenmoon_markets_t *m;
  uint32_t            i;

  if(st == NULL || st->markets == NULL || product_id == NULL)
    return(NULL);

  m = st->markets;

  for(i = 0; i < m->n_markets; i++)
  {
    if(strncmp(m->arr[i].product_id, product_id,
           WM_PRODUCT_ID_SZ) == 0)
      return(&m->arr[i]);
  }

  return(NULL);
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

  out->mode     = s->mode;
  out->position = s->position;

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

// Allowlist of exchange tokens accepted by the parser. EX-1 promotes
// this to a registry lookup against loaded exchange plugins.
static const char *const wm_market_exchange_allowlist[] = {
  "coinbase",
  NULL,
};

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
  uint32_t    i;
  bool        allowed = false;

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

  for(i = 0; wm_market_exchange_allowlist[i] != NULL; i++)
  {
    if(strcmp(exchange, wm_market_exchange_allowlist[i]) == 0)
    {
      allowed = true;
      break;
    }
  }

  if(!allowed)
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
wm_market_on_candles(const coinbase_candles_result_t *res, void *user)
{
  wm_market_backfill_ctx_t *ctx = user;
  whenmoon_market_t        *mk;
  uint32_t                  i;

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

  mk = wm_market_find(ctx->st->markets, ctx->product_id);

  if(mk == NULL)
  {
    mem_free(ctx);
    return;
  }

  pthread_mutex_lock(&mk->lock);

  // Coinbase returns candles newest-first; replay oldest-first so the
  // aggregator's idempotency check (skip ts <= last_close_ms) prunes
  // duplicates correctly when a later REST page overlaps a prior
  // warm-up. The replay path drives the cascade so 5m/15m/1h/6h/1d
  // grains backfill from this single 1m feed.
  for(i = res->count; i > 0; i--)
  {
    const coinbase_candle_t *cb = &res->rows[i - 1];
    wm_candle_full_t         bar;

    memset(&bar, 0, sizeof(bar));
    bar.ts_close_ms = (cb->time + 60) * 1000;   // 1m bucket close
    bar.open        = cb->open;
    bar.high        = cb->high;
    bar.low         = cb->low;
    bar.close       = cb->close;
    bar.volume      = cb->volume;

    wm_aggregator_replay_bar(mk, WM_GRAN_1M, &bar);
  }

  pthread_mutex_unlock(&mk->lock);

  clam(CLAM_INFO, WHENMOON_CTX,
      "market %s: %u candles backfilled",
      ctx->product_id, res->count);

  mem_free(ctx);
}

void
wm_market_on_event(const coinbase_ws_event_t *ev, void *user)
{
  whenmoon_state_t  *st = user;
  whenmoon_market_t *mk;

  if(ev == NULL || st == NULL || st->markets == NULL)
    return;

  switch(ev->channel)
  {
    case COINBASE_CH_HEARTBEAT:
      break;

    case COINBASE_CH_TICKER:
    {
      const coinbase_ws_ticker_t *t = ev->payload;

      if(t == NULL)
        break;

      mk = wm_market_find(st->markets, t->product_id);

      if(mk == NULL)
        break;

      pthread_mutex_lock(&mk->lock);
      mk->last_px      = t->price;
      mk->last_tick_ms = t->time_ms;
      pthread_mutex_unlock(&mk->lock);

      clam(CLAM_DEBUG2, WHENMOON_CTX,
          "tick %s px=%.8g",
          t->product_id, t->price);
      break;
    }

    case COINBASE_CH_MATCHES:
    {
      const coinbase_ws_match_t *m = ev->payload;

      if(m == NULL)
        break;

      mk = wm_market_find(st->markets, m->product_id);

      if(mk == NULL)
        break;

      pthread_mutex_lock(&mk->lock);

      mk->last_px      = m->price;
      mk->last_tick_ms = m->time_ms;

      // Drive the multi-grain cascade. Aggregator owns the close +
      // indicator pass; this hot path stays under one lock acquire.
      if(mk->aggregator != NULL)
        wm_aggregator_on_trade(mk, m->time_ms, m->price, m->size);

      pthread_mutex_unlock(&mk->lock);

      clam(CLAM_DEBUG2, WHENMOON_CTX,
          "match %s %s px=%.8g sz=%.8g",
          m->product_id, m->side, m->price, m->size);
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

  if(m->ws_sub != NULL)
  {
    coinbase_ws_unsubscribe(m->ws_sub);
    m->ws_sub = NULL;
  }

  if(m->arr != NULL)
  {
    for(i = 0; i < m->n_markets; i++)
    {
      whenmoon_market_t *mk = &m->arr[i];

      if(mk->aggregator != NULL)
        wm_aggregator_destroy(mk);

      pthread_mutex_destroy(&mk->lock);
    }

    mem_free(m->arr);
  }

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
    const char *product_id, bool persist,
    char *err, size_t err_cap)
{
  whenmoon_markets_t *m;
  whenmoon_market_t  *mk;
  int32_t             market_id;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(st == NULL || st->markets == NULL || exchange == NULL ||
     base == NULL || quote == NULL || product_id == NULL)
  {
    if(err != NULL) snprintf(err, err_cap, "bad args");
    return(FAIL);
  }

  m = st->markets;

  if(wm_market_find(m, product_id) != NULL)
    return(SUCCESS);   // already present; benign no-op

  market_id = wm_market_lookup_or_create(exchange, base, quote, product_id);

  if(market_id < 0)
  {
    if(err != NULL) snprintf(err, err_cap, "market registry failed");
    return(FAIL);
  }

  if(wm_market_grow(m, m->n_markets + 1) != SUCCESS)
  {
    if(err != NULL) snprintf(err, err_cap, "alloc failed");
    return(FAIL);
  }

  mk = &m->arr[m->n_markets];
  memset(mk, 0, sizeof(*mk));
  snprintf(mk->product_id, sizeof(mk->product_id), "%s", product_id);
  wm_market_format_id(exchange, base, quote,
      mk->market_id_str, sizeof(mk->market_id_str));
  mk->market_id = market_id;
  pthread_mutex_init(&mk->lock, NULL);

  // WM-MK-2: install the per-market position model with default-init
  // values. Lazy KV refresh (Phase C) replaces cached params on first
  // engine call. Persistence restore (Phase D) overwrites the session
  // when a wm_market_state row exists for this market_id.
  wm_market_session_init(&mk->session);

  m->n_markets++;

  if(persist)
  {
    if(wm_market_set_enabled(market_id, true) != SUCCESS)
    {
      // DB failure -- roll back the in-memory slot so the running set
      // matches the persisted state.
      pthread_mutex_destroy(&mk->lock);
      memset(mk, 0, sizeof(*mk));
      m->n_markets--;
      if(err != NULL) snprintf(err, err_cap, "DB enable failed");
      return(FAIL);
    }
  }

  // Aggregator must be in place before resub_ws so the first WS event
  // can already feed it.
  if(wm_aggregator_init(mk, WM_AGG_DEFAULT_HISTORY_1D) != SUCCESS)
  {
    if(persist)
      (void)wm_market_set_enabled(market_id, false);

    pthread_mutex_destroy(&mk->lock);
    memset(mk, 0, sizeof(*mk));
    m->n_markets--;
    if(err != NULL) snprintf(err, err_cap, "aggregator init failed");
    return(FAIL);
  }

  // WM-MK-2: lazy-register + cache per-market KV defaults so operator
  // /set kv values land in the session immediately. Idempotent across
  // restarts (kv_register short-circuits on existing keys; KV values
  // hydrated by kv_load survive); the cached values are also
  // overwritten by wm_market_persist_restore_all when a state row
  // exists for this market.
  wm_market_session_refresh_kv(mk);

  wm_market_resub_ws(st);
  wm_market_kick_backfill(st, product_id);

  // Schedule the DB warm-up: replay 1m bars from
  // wm_candles_<id>_60 chronologically into the aggregator. The
  // deferred task re-resolves the market by product_id at run-time so
  // a stop-before-warm-up bails cleanly. 50 ms after the live-ring
  // backfill kick gives REST a head start without blocking the verb.
  {
    wm_warmup_ctx_t *wctx = mem_alloc("whenmoon", "warmup_ctx",
        sizeof(*wctx));

    if(wctx != NULL)
    {
      wctx->st        = st;
      wctx->market_id = market_id;
      snprintf(wctx->product_id, sizeof(wctx->product_id),
          "%s", product_id);

      if(task_add_deferred("wm_warmup", TASK_ANY, 100, 50,
             wm_aggregator_load_history_task, wctx) == TASK_HANDLE_NONE)
      {
        clam(CLAM_INFO, WHENMOON_CTX,
            "market %s warmup task submit failed", mk->market_id_str);
        mem_free(wctx);
      }
    }
  }

  clam(CLAM_INFO, WHENMOON_CTX,
      "market %s started (market_id=%" PRId32 "%s)",
      mk->market_id_str, market_id, persist ? "" : ", restored");

  return(SUCCESS);
}

bool
wm_market_remove(whenmoon_state_t *st, const char *product_id,
    bool persist, bool *was_present, char *err, size_t err_cap)
{
  whenmoon_markets_t *m;
  whenmoon_market_t  *mk;
  char                id_str[WM_MARKET_ID_STR_SZ];
  uint32_t            idx;
  int32_t             market_id;

  if(was_present != NULL)
    *was_present = false;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(st == NULL || st->markets == NULL || product_id == NULL)
  {
    if(err != NULL) snprintf(err, err_cap, "bad args");
    return(FAIL);
  }

  m  = st->markets;
  mk = wm_market_find(m, product_id);

  if(mk == NULL)
    return(SUCCESS);  // benign no-op; was_present stays false

  if(was_present != NULL)
    *was_present = true;

  idx       = (uint32_t)(mk - m->arr);
  market_id = mk->market_id;
  snprintf(id_str, sizeof(id_str), "%s", mk->market_id_str);

  // Auto-detach any strategy attachments bound to this market. Done
  // before tearing down market state so finalize_fn callbacks fire
  // while the (about-to-be-freed) ctx still has a valid id_str. The
  // ctx->mkt pointer is invalidated regardless.
  wm_strategy_detach_market(st, id_str);

  // Aggregator locks the slot's mutex internally, so destroy it
  // before pthread_mutex_destroy(&mk->lock).
  if(mk->aggregator != NULL)
    wm_aggregator_destroy(mk);

  pthread_mutex_destroy(&mk->lock);

  // Compact the tail over the removed slot. Memmove is safe across
  // overlapping regions. The vacated tail slot is zeroed so a future
  // grow doesn't inherit a bogus mutex.
  if(idx + 1 < m->n_markets)
    memmove(&m->arr[idx], &m->arr[idx + 1],
        sizeof(*m->arr) * (m->n_markets - idx - 1));

  memset(&m->arr[m->n_markets - 1], 0, sizeof(*m->arr));
  m->n_markets--;

  if(persist && market_id >= 0)
  {
    if(wm_market_set_enabled(market_id, false) != SUCCESS)
    {
      // In-memory removal already committed; leaving enabled=true would
      // cause the market to resurrect on next restart. Log and keep
      // going; the operator can `/whenmoon market stop` again.
      if(err != NULL)
        snprintf(err, err_cap,
            "DB disable failed (live set already updated)");
    }

    // WM-MK-2: drop the per-market session row so a stale paper ledger
    // doesn't resurrect when the operator re-enables the market later.
    (void)wm_market_persist_drop(market_id);
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
  bool         sandbox;
  uint32_t     i;
  uint32_t     n_restored = 0;
  uint32_t     n_skipped  = 0;
  bool         ok = SUCCESS;

  if(st == NULL)
    return(FAIL);

  res = db_result_alloc();

  if(res == NULL)
    return(FAIL);

  if(db_query(
         "SELECT exchange, base_asset, quote_asset, exchange_symbol"
         "  FROM wm_market"
         " WHERE enabled = TRUE"
         " ORDER BY id", res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "market restore query failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");
    ok = FAIL;
    goto out;
  }

  sandbox = coinbase_sandbox_active();

  for(i = 0; i < res->rows; i++)
  {
    const char *exch  = db_result_get(res, i, 0);
    const char *base  = db_result_get(res, i, 1);
    const char *quote = db_result_get(res, i, 2);
    const char *sym   = db_result_get(res, i, 3);

    if(exch == NULL || base == NULL || quote == NULL || sym == NULL)
      continue;

    // WM-DC-1: skip rows that don't match the active coinbase
    // environment so a freshstart on the opposite side doesn't try to
    // bring up a market whose WS feed (and trade table) belong to the
    // other env. Only pinning coinbase-family rows here — future
    // exchanges pass through unchanged.
    if(sandbox && strcmp(exch, "coinbase") == 0)
    {
      clam(CLAM_INFO, WHENMOON_CTX,
          "market %s skipped — prod row, sandbox active", sym);
      n_skipped++;
      continue;
    }

    if(!sandbox && strcmp(exch, "coinbase-sb") == 0)
    {
      clam(CLAM_INFO, WHENMOON_CTX,
          "market %s skipped — sandbox row, prod active", sym);
      n_skipped++;
      continue;
    }

    if(wm_market_add(st, exch, base, quote, sym,
           false, NULL, 0) != SUCCESS)
    {
      clam(CLAM_INFO, WHENMOON_CTX,
          "market %s restore failed — skipping", sym);
      continue;
    }

    n_restored++;
  }

  clam(CLAM_INFO, WHENMOON_CTX,
      "%u running market(s) restored, %u env-mismatched skipped"
      " (sandbox=%s)",
      n_restored, n_skipped, sandbox ? "true" : "false");

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
  mk->market_id = -1;     // synth has no DB row

  // Share grain rings — read-only during iteration. The source
  // market's lifetime spans the entire sweep; iteration is a strict
  // subset.
  memcpy(mk->grain_arr, src->grain_arr, sizeof(mk->grain_arr));
  memcpy(mk->grain_n,   src->grain_n,   sizeof(mk->grain_n));
  memcpy(mk->grain_cap, src->grain_cap, sizeof(mk->grain_cap));

  mk->aggregator   = NULL;
  mk->last_px      = src->last_px;
  mk->last_tick_ms = src->last_tick_ms;

  pthread_mutex_init(&mk->lock, NULL);

  wm_market_session_init(&mk->session);
  mk->session.mode = WM_MARKET_MODE_PAPER;

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
