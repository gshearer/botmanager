// botmanager — MIT
// whenmoon per-market state: candle backfill + live WS fanout.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "market.h"

#include "kv.h"

#include <string.h>

// Per-product backfill context. Heap-owned; the completion callback is
// the sole owner and frees it after wiring the rows into the market
// struct. Lifetime decoupled from whenmoon_state_t teardown: if the
// state is destroyed before coinbase fires the callback, the callback
// still runs (coinbase owns its request queue) and must not touch the
// state. We therefore track the state pointer but also the market
// index; on callback we re-lookup the product id in the live state to
// decide whether to commit.
typedef struct
{
  whenmoon_state_t   *st;            // borrow; checked via product_id match
  char                product_id[COINBASE_PRODUCT_ID_SZ];
} wm_market_backfill_ctx_t;

// ------------------------------------------------------------------ //
// KV parsing                                                         //
// ------------------------------------------------------------------ //

static uint32_t
wm_parse_markets_kv(const char *raw, char out[][COINBASE_PRODUCT_ID_SZ],
    uint32_t out_cap)
{
  char buf[512];
  char *saveptr;
  char *tok;
  uint32_t n;

  if(raw == NULL || raw[0] == '\0')
    return(0);

  snprintf(buf, sizeof(buf), "%s", raw);

  n = 0;
  saveptr = NULL;

  for(tok = strtok_r(buf, ", \t", &saveptr);
      tok != NULL;
      tok = strtok_r(NULL, ", \t", &saveptr))
  {
    if(tok[0] == '\0')
      continue;

    if(n >= out_cap)
    {
      clam(CLAM_INFO, WHENMOON_CTX,
          "market list truncated at cap=%u (dropped '%s')",
          out_cap, tok);
      break;
    }

    snprintf(out[n], COINBASE_PRODUCT_ID_SZ, "%s", tok);
    n++;
  }

  return(n);
}

// ------------------------------------------------------------------ //
// Lookup                                                             //
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

// ------------------------------------------------------------------ //
// Candle backfill callback                                           //
// ------------------------------------------------------------------ //

void
wm_market_on_candles(const coinbase_candles_result_t *res, void *user)
{
  wm_market_backfill_ctx_t *ctx = user;
  whenmoon_market_t *mk;
  uint32_t i;

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

  // The state pointer is borrowed; destroy may have nulled `markets`
  // before this callback fired. Check both.
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

  // Coinbase returns candles newest-first; push them in reverse so the
  // ring buffer ends up with newest at write_pos-1 and oldest at
  // write_pos after wrap.
  for(i = res->count; i > 0; i--)
  {
    mk->candles[mk->write_pos] = res->rows[i - 1];
    mk->write_pos = (mk->write_pos + 1) % WM_MARKET_CANDLE_CAP;

    if(mk->n_candles < WM_MARKET_CANDLE_CAP)
      mk->n_candles++;
  }

  pthread_mutex_unlock(&mk->lock);

  clam(CLAM_INFO, WHENMOON_CTX,
      "market %s: %u candles backfilled",
      ctx->product_id, res->count);

  mem_free(ctx);
}

// ------------------------------------------------------------------ //
// WebSocket event fanout                                             //
// ------------------------------------------------------------------ //

void
wm_market_on_event(const coinbase_ws_event_t *ev, void *user)
{
  whenmoon_state_t *st = user;
  whenmoon_market_t *mk;

  if(ev == NULL || st == NULL || st->markets == NULL)
    return;

  switch(ev->channel)
  {
    case COINBASE_CH_HEARTBEAT:
      // Liveness only — nothing to record. Could timestamp a
      // per-market keepalive in a future chunk.
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

      // A match is also a price print; fold it into last_px so a silent
      // ticker channel still moves the bot's view.
      pthread_mutex_lock(&mk->lock);
      mk->last_px      = m->price;
      mk->last_tick_ms = m->time_ms;
      pthread_mutex_unlock(&mk->lock);

      clam(CLAM_DEBUG2, WHENMOON_CTX,
          "match %s %s px=%.8g sz=%.8g",
          m->product_id, m->side, m->price, m->size);
      break;
    }

    default:
      // Unsubscribed channels — ignore. The subscription set is fixed
      // at init, so this branch is defensive against upstream additions.
      break;
  }
}

// ------------------------------------------------------------------ //
// Init / destroy                                                     //
// ------------------------------------------------------------------ //

bool
wm_market_init(whenmoon_state_t *st)
{
  char key[128];
  const char *raw;
  char pids[WM_MARKET_MAX][COINBASE_PRODUCT_ID_SZ];
  const char *pid_ptrs[WM_MARKET_MAX];
  uint32_t n;
  uint32_t i;
  whenmoon_markets_t *m;
  static const coinbase_ws_channel_t chans[] = {
    COINBASE_CH_HEARTBEAT,
    COINBASE_CH_TICKER,
    COINBASE_CH_MATCHES,
  };

  if(st == NULL)
    return(FAIL);

  snprintf(key, sizeof(key), "bot.%s.whenmoon.markets", st->bot_name);
  raw = kv_get_str(key);

  n = wm_parse_markets_kv(raw, pids, WM_MARKET_MAX);

  m = mem_alloc("whenmoon", "markets", sizeof(*m));

  if(m == NULL)
    return(FAIL);

  memset(m, 0, sizeof(*m));

  st->markets = m;

  if(n == 0)
  {
    clam(CLAM_INFO, WHENMOON_CTX,
        "bot %s: no markets configured (bot.%s.whenmoon.markets empty)",
        st->bot_name, st->bot_name);
    return(SUCCESS);
  }

  m->arr = mem_alloc("whenmoon", "market_arr", sizeof(*m->arr) * n);

  if(m->arr == NULL)
    return(FAIL);

  memset(m->arr, 0, sizeof(*m->arr) * n);
  m->n_markets = n;

  for(i = 0; i < n; i++)
  {
    memcpy(m->arr[i].product_id, pids[i], sizeof(m->arr[i].product_id));
    m->arr[i].product_id[sizeof(m->arr[i].product_id) - 1] = '\0';
    pthread_mutex_init(&m->arr[i].lock, NULL);
    pid_ptrs[i] = m->arr[i].product_id;
  }

  // Kick off one backfill per product. Each gets its own ctx; the
  // coinbase plugin owns the request until its callback runs.
  for(i = 0; i < n; i++)
  {
    wm_market_backfill_ctx_t *ctx;

    ctx = mem_alloc("whenmoon", "backfill_ctx", sizeof(*ctx));

    if(ctx == NULL)
      continue;

    ctx->st = st;
    memcpy(ctx->product_id, pids[i], sizeof(ctx->product_id));
    ctx->product_id[sizeof(ctx->product_id) - 1] = '\0';

    if(coinbase_fetch_candles_async(ctx->product_id, COINBASE_GRAN_1M,
           0, 0, wm_market_on_candles, ctx) != SUCCESS)
    {
      clam(CLAM_INFO, WHENMOON_CTX,
          "market %s: backfill submit failed", ctx->product_id);
      mem_free(ctx);
    }
  }

  // One combined subscription across every configured product.
  m->ws_sub = coinbase_ws_subscribe(chans,
      sizeof(chans) / sizeof(chans[0]),
      pid_ptrs, n,
      wm_market_on_event, st);

  if(m->ws_sub == NULL)
    clam(CLAM_INFO, WHENMOON_CTX,
        "bot %s: ws subscribe failed (no live stream)", st->bot_name);

  return(SUCCESS);
}

void
wm_market_destroy(whenmoon_state_t *st)
{
  whenmoon_markets_t *m;
  uint32_t i;

  if(st == NULL || st->markets == NULL)
    return;

  m = st->markets;

  // Tear down the subscription first so wm_market_on_event stops
  // firing before we destroy the per-market mutexes.
  if(m->ws_sub != NULL)
  {
    coinbase_ws_unsubscribe(m->ws_sub);
    m->ws_sub = NULL;
  }

  if(m->arr != NULL)
  {
    for(i = 0; i < m->n_markets; i++)
      pthread_mutex_destroy(&m->arr[i].lock);

    mem_free(m->arr);
  }

  // Block new callbacks from finding the state via ->markets before we
  // free. In-flight backfill callbacks will see st->markets == NULL and
  // short-circuit without a deref.
  st->markets = NULL;

  mem_free(m);
}
