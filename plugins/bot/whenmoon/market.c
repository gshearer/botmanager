// botmanager — MIT
// whenmoon per-market state: live set, WS fanout, live-ring backfill.
//
// Running markets are owned by the DB (`wm_bot_market`), not a KV.
// The user drives the set through `/bot <name> market start|stop`;
// `wm_market_restore` replays it on bot start. Adding a market
// triggers a 1m-candle live-ring backfill (300 rows via REST) but
// no history catch-up — historical coverage is the strategy layer's
// responsibility (WM-LT-3) and the user-facing `/bot … download …`
// verbs.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "market.h"
#include "dl_schema.h"

#include "db.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        "bot %s: ws resub alloc failed (no live stream)",
        st->bot_name);
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
        "bot %s: ws subscribe failed (no live stream)",
        st->bot_name);

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
  (void)coinbase_fetch_candles_async(ctx->product_id, COINBASE_GRAN_1M,
      0, 0, wm_market_on_candles, ctx);
}

// ------------------------------------------------------------------ //
// DB persistence helpers                                             //
// ------------------------------------------------------------------ //

static bool
wm_bot_market_insert(const char *bot_name, int32_t market_id)
{
  db_result_t *res = NULL;
  char        *e_bot = NULL;
  char         sql[256];
  bool         ok = FAIL;
  int          n;

  if(bot_name == NULL || market_id < 0)
    return(FAIL);

  e_bot = db_escape(bot_name);

  if(e_bot == NULL)
    goto out;

  n = snprintf(sql, sizeof(sql),
      "INSERT INTO wm_bot_market (bot_name, market_id)"
      " VALUES ('%s', %" PRId32 ")"
      " ON CONFLICT (bot_name, market_id) DO NOTHING",
      e_bot, market_id);

  if(n < 0 || (size_t)n >= sizeof(sql))
    goto out;

  res = db_result_alloc();

  if(res == NULL)
    goto out;

  if(db_query(sql, res) == SUCCESS && res->ok)
    ok = SUCCESS;

  else
    clam(CLAM_WARN, WHENMOON_CTX,
        "wm_bot_market insert failed (bot=%s market_id=%" PRId32 "): %s",
        bot_name, market_id,
        res->error[0] != '\0' ? res->error : "(no driver error)");

out:
  if(res   != NULL) db_result_free(res);
  if(e_bot != NULL) mem_free(e_bot);

  return(ok);
}

static bool
wm_bot_market_delete(const char *bot_name, int32_t market_id)
{
  db_result_t *res = NULL;
  char        *e_bot = NULL;
  char         sql[256];
  bool         ok = FAIL;
  int          n;

  if(bot_name == NULL || market_id < 0)
    return(FAIL);

  e_bot = db_escape(bot_name);

  if(e_bot == NULL)
    goto out;

  n = snprintf(sql, sizeof(sql),
      "DELETE FROM wm_bot_market"
      " WHERE bot_name = '%s' AND market_id = %" PRId32,
      e_bot, market_id);

  if(n < 0 || (size_t)n >= sizeof(sql))
    goto out;

  res = db_result_alloc();

  if(res == NULL)
    goto out;

  if(db_query(sql, res) == SUCCESS && res->ok)
    ok = SUCCESS;

  else
    clam(CLAM_WARN, WHENMOON_CTX,
        "wm_bot_market delete failed (bot=%s market_id=%" PRId32 "): %s",
        bot_name, market_id,
        res->error[0] != '\0' ? res->error : "(no driver error)");

out:
  if(res   != NULL) db_result_free(res);
  if(e_bot != NULL) mem_free(e_bot);

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
      pthread_mutex_destroy(&m->arr[i].lock);

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
  mk->market_id = market_id;
  pthread_mutex_init(&mk->lock, NULL);
  m->n_markets++;

  if(persist)
  {
    if(wm_bot_market_insert(st->bot_name, market_id) != SUCCESS)
    {
      // DB failure -- roll back the in-memory slot so the running set
      // matches the persisted state.
      pthread_mutex_destroy(&mk->lock);
      memset(mk, 0, sizeof(*mk));
      m->n_markets--;
      if(err != NULL) snprintf(err, err_cap, "DB insert failed");
      return(FAIL);
    }
  }

  wm_market_resub_ws(st);
  wm_market_kick_backfill(st, product_id);

  clam(CLAM_INFO, WHENMOON_CTX,
      "bot %s: market %s started (market_id=%" PRId32 "%s)",
      st->bot_name, product_id, market_id,
      persist ? "" : ", restored");

  return(SUCCESS);
}

bool
wm_market_remove(whenmoon_state_t *st, const char *product_id,
    bool persist, bool *was_present, char *err, size_t err_cap)
{
  whenmoon_markets_t *m;
  whenmoon_market_t  *mk;
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
    if(wm_bot_market_delete(st->bot_name, market_id) != SUCCESS)
    {
      // In-memory removal already committed; leaving the DB row would
      // cause the market to resurrect on next restart. Log and keep
      // going; the operator can `/bot … market stop` again.
      if(err != NULL)
        snprintf(err, err_cap, "DB delete failed (live set already updated)");
    }
  }

  wm_market_resub_ws(st);

  clam(CLAM_INFO, WHENMOON_CTX,
      "bot %s: market %s stopped (market_id=%" PRId32 "%s)",
      st->bot_name, product_id, market_id,
      persist ? "" : ", untracked");

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Restore                                                            //
// ------------------------------------------------------------------ //

bool
wm_market_restore(whenmoon_state_t *st)
{
  db_result_t *res   = NULL;
  char        *e_bot = NULL;
  char         sql[512];
  uint32_t     i;
  uint32_t     n_restored = 0;
  bool         ok = SUCCESS;

  if(st == NULL || st->bot_name[0] == '\0')
    return(FAIL);

  e_bot = db_escape(st->bot_name);

  if(e_bot == NULL)
    return(FAIL);

  snprintf(sql, sizeof(sql),
      "SELECT wm.exchange, wm.base_asset, wm.quote_asset,"
      "       wm.exchange_symbol"
      "  FROM wm_bot_market bm"
      "  JOIN wm_market     wm ON bm.market_id = wm.id"
      " WHERE bm.bot_name = '%s'"
      " ORDER BY bm.added_at",
      e_bot);

  res = db_result_alloc();

  if(res == NULL)
  {
    ok = FAIL;
    goto out;
  }

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "bot %s: market restore query failed: %s",
        st->bot_name,
        res->error[0] != '\0' ? res->error : "(no driver error)");
    ok = FAIL;
    goto out;
  }

  for(i = 0; i < res->rows; i++)
  {
    const char *exch  = db_result_get(res, i, 0);
    const char *base  = db_result_get(res, i, 1);
    const char *quote = db_result_get(res, i, 2);
    const char *sym   = db_result_get(res, i, 3);

    if(exch == NULL || base == NULL || quote == NULL || sym == NULL)
      continue;

    if(wm_market_add(st, exch, base, quote, sym,
           false, NULL, 0) != SUCCESS)
    {
      clam(CLAM_INFO, WHENMOON_CTX,
          "bot %s: market %s restore failed — skipping",
          st->bot_name, sym);
      continue;
    }

    n_restored++;
  }

  clam(CLAM_INFO, WHENMOON_CTX,
      "bot %s: %u running market(s) restored",
      st->bot_name, n_restored);

out:
  if(res   != NULL) db_result_free(res);
  if(e_bot != NULL) mem_free(e_bot);

  return(ok);
}
