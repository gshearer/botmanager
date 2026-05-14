// botmanager — MIT
// warmup.c — WM-WARMUP-1 per-grain REST history warmup at strategy attach.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "aggregator.h"
#include "market.h"
#include "strategy.h"
#include "warmup.h"

#include "exchange_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Heap context carried through one async REST candle fetch. The
// completion callback (wm_warmup_on_candles) is the sole owner and
// frees it. The market is identified by canonical id rather than a
// pointer because it can be stopped between the fetch kick and the
// callback — the callback re-resolves and bails cleanly on a miss.
typedef struct
{
  whenmoon_state_t *st;
  char              market_id_str[WM_MARKET_ID_STR_SZ];
  wm_gran_t         gran;
} wm_grain_warmup_ctx_t;

// Human-readable grain label for log lines. Mirrors the file-local
// rendering in strategy_cmds.c / backtest.c — the plugin has no
// shared wm_gran_name helper.
static const char *
wm_warmup_gran_label(wm_gran_t g)
{
  static const char *const labels[WM_GRAN_MAX] = {
    "1m", "5m", "15m", "1h", "4h", "1d"
  };

  if((unsigned)g >= WM_GRAN_MAX)
    return("?");

  return(labels[g]);
}

// Map a whenmoon grain onto the exchange abstraction's granularity
// enum. Total after WM-GRAN-4H-1 — every whenmoon grain has a native
// exchange-side equivalent; the default arm is reachable only for
// WM_GRAN_MAX or a future unmapped grain.
static bool
wm_gran_to_exchange(wm_gran_t g, exchange_granularity_t *out)
{
  switch(g)
  {
    case WM_GRAN_1M:  *out = EXCH_GRAN_1M;  return(SUCCESS);
    case WM_GRAN_5M:  *out = EXCH_GRAN_5M;  return(SUCCESS);
    case WM_GRAN_15M: *out = EXCH_GRAN_15M; return(SUCCESS);
    case WM_GRAN_1H:  *out = EXCH_GRAN_1H;  return(SUCCESS);
    case WM_GRAN_4H:  *out = EXCH_GRAN_4H;  return(SUCCESS);
    case WM_GRAN_1D:  *out = EXCH_GRAN_1D;  return(SUCCESS);
    default:          return(FAIL);
  }
}

// qsort comparator — ascending by ts_close_ms. Exchange row order is
// not uniform (Coinbase newest-first; Kraken its own), so the fetched
// page is sorted before it is replayed into the ring.
static int
wm_warmup_bar_cmp(const void *va, const void *vb)
{
  const wm_candle_full_t *a = va;
  const wm_candle_full_t *b = vb;

  if(a->ts_close_ms < b->ts_close_ms) return(-1);
  if(a->ts_close_ms > b->ts_close_ms) return(1);

  return(0);
}

// exchange_done_candles_cb_t completion callback. Runs on the curl /
// exchange thread: re-resolve the market, convert + sort the rows,
// seed the grain ring under mk->lock, and free the ctx. Fires even on
// submit failure, so every exit path frees ctx.
static void
wm_warmup_on_candles(const exchange_candles_result_t *res, void *user)
{
  wm_grain_warmup_ctx_t *ctx = user;
  whenmoon_market_t     *mk;
  wm_candle_full_t      *scratch;
  const char            *label;
  uint32_t               i;

  if(ctx == NULL)
    return;

  label = wm_warmup_gran_label(ctx->gran);

  if(res == NULL || res->err[0] != '\0')
  {
    clam(CLAM_INFO, WHENMOON_CTX,
        "warmup %s %s: fetch failed: %s",
        ctx->market_id_str, label,
        (res != NULL && res->err[0] != '\0') ? res->err : "(no result)");
    mem_free(ctx);
    return;
  }

  if(ctx->st == NULL || ctx->st->markets == NULL)
  {
    mem_free(ctx);
    return;
  }

  mk = wm_market_lookup_by_id(ctx->st, ctx->market_id_str);

  if(mk == NULL)
  {
    mem_free(ctx);
    return;
  }

  if(res->count == 0)
  {
    clam(CLAM_INFO, WHENMOON_CTX,
        "warmup %s %s: 0 bars", ctx->market_id_str, label);
    mem_free(ctx);
    return;
  }

  scratch = mem_alloc("whenmoon", "warmup_bars",
      sizeof(*scratch) * (size_t)res->count);

  if(scratch == NULL)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "warmup %s %s: scratch alloc failed (%u bars)",
        ctx->market_id_str, label, res->count);
    mem_free(ctx);
    return;
  }

  // exchange_candle_t carries the bucket open in ms; the ring stores
  // bar close, so close = open + grain seconds. ind[] stays zeroed —
  // wm_aggregator_push_bar recomputes the indicator block per bar.
  for(i = 0; i < res->count; i++)
  {
    const exchange_candle_t *src = &res->rows[i];
    wm_candle_full_t        *dst = &scratch[i];

    memset(dst, 0, sizeof(*dst));
    dst->ts_close_ms = src->ts_open_ms
        + (int64_t)wm_gran_seconds[ctx->gran] * 1000;
    dst->open        = src->open;
    dst->high        = src->high;
    dst->low         = src->low;
    dst->close       = src->close;
    dst->volume      = src->volume;
  }

  qsort(scratch, res->count, sizeof(*scratch), wm_warmup_bar_cmp);

  pthread_mutex_lock(&mk->lock);
  wm_aggregator_warmup_grain(mk, ctx->gran, scratch, res->count);
  pthread_mutex_unlock(&mk->lock);

  clam(CLAM_INFO, WHENMOON_CTX,
      "warmup %s %s: seeded %u bars",
      ctx->market_id_str, label, res->count);

  mem_free(scratch);
  mem_free(ctx);
}

void
wm_warmup_for_attachment(whenmoon_state_t *st, whenmoon_market_t *mk,
    const loaded_strategy_t *ls)
{
  uint32_t g;

  if(st == NULL || mk == NULL || ls == NULL)
    return;

  for(g = 0; g < WM_GRAN_MAX; g++)
  {
    exchange_granularity_t  eg;
    wm_grain_warmup_ctx_t  *ctx;
    uint32_t                need;
    uint32_t                have;

    if((ls->meta.grains_mask & (1u << g)) == 0)
      continue;

    if(ls->meta.min_history[g] == 0)
      continue;

    if(wm_gran_to_exchange((wm_gran_t)g, &eg) != SUCCESS)
    {
      clam(CLAM_INFO, WHENMOON_CTX,
          "warmup %s %s: no exchange granularity mapping, skipped",
          mk->market_id_str, wm_warmup_gran_label((wm_gran_t)g));
      continue;
    }

    // Depth check: skip the fetch when the ring already holds
    // min(min_history, grain_cap) bars — another strategy warmed it,
    // or the market has run live long enough on its own.
    need = ls->meta.min_history[g];

    pthread_mutex_lock(&mk->lock);

    if(need > mk->grain_cap[g])
      need = mk->grain_cap[g];

    have = mk->grain_n[g];

    pthread_mutex_unlock(&mk->lock);

    if(have >= need)
      continue;

    ctx = mem_alloc("whenmoon", "warmup_ctx", sizeof(*ctx));

    if(ctx == NULL)
    {
      clam(CLAM_WARN, WHENMOON_CTX,
          "warmup %s %s: ctx alloc failed",
          mk->market_id_str, wm_warmup_gran_label((wm_gran_t)g));
      continue;
    }

    ctx->st   = st;
    ctx->gran = (wm_gran_t)g;
    snprintf(ctx->market_id_str, sizeof(ctx->market_id_str), "%s",
        mk->market_id_str);

    // Mirror wm_market_kick_backfill: the completion callback owns ctx
    // and fires even on submit failure — do NOT touch ctx after this
    // call. since/until 0,0 = the exchange's default most-recent page
    // (Kraken 720 bars at the interval; Coinbase its default page).
    (void)exchange_fetch_candles_async(mk->exchange_name, mk->product_id,
        eg, 0, 0, wm_warmup_on_candles, ctx);
  }
}
