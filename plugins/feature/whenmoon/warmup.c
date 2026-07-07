// botmanager — MIT
// warmup.c — WM-WARMUP-2 market warmup lifecycle: roster-sized,
// DB-first gap-fill + convergence + authoritative tail-fill.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "aggregator.h"
#include "market.h"
#include "strategy.h"
#include "warmup.h"
#include "dl_coverage.h"
#include "dl_jobtable.h"

#include "alloc.h"
#include "task.h"
#include "exchange_api.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Heap context carried (and re-owned) across the self-rescheduling
// re-check / tail-fill deferred tasks. The single in-flight task owns it
// and frees it when it stops. The market is identified by canonical id
// (re-resolved each tick) plus the warmup generation it belongs to, so a
// stopped market or a superseding warmup_begin retires the timer.
typedef struct
{
  whenmoon_state_t *st;
  char              market_id_str[WM_MARKET_ID_STR_SZ];
  uint32_t          gen;
  uint32_t          iters;
} wm_warm_timer_ctx_t;

static void wm_market_warmup_recheck_task(task_t *t);
static void wm_market_warmup_tailfill_task(task_t *t);

const char *
wm_warmup_state_name(wm_warmup_state_t s)
{
  switch(s)
  {
    case WM_WARM_COLD:    return("cold");
    case WM_WARM_WARMING: return("warming");
    case WM_WARM_FINAL:   return("final");
    case WM_WARM_READY:   return("ready");
  }

  return("?");
}

// --------------------------------------------------------------------
// Required-history computation (deepest declared strategy on a market)
// --------------------------------------------------------------------

typedef struct
{
  const char *market_id_str;
  int64_t     max_ms;
} wm_lb_acc_t;

static void
wm_lb_iter_cb(const loaded_strategy_t *ls, void *user)
{
  wm_lb_acc_t                    *acc = user;
  const wm_strategy_attachment_t *a;

  for(a = ls->attachments; a != NULL; a = a->next)
  {
    uint32_t g;

    if(strcmp(a->ctx.market_id_str, acc->market_id_str) != 0)
      continue;

    for(g = 0; g < WM_GRAN_MAX; g++)
    {
      int64_t ms;

      if((ls->meta.grains_mask & (1u << g)) == 0)
        continue;

      if(ls->meta.min_history[g] == 0)
        continue;

      ms = (int64_t)ls->meta.min_history[g]
         * (int64_t)wm_gran_seconds[g] * 1000;

      if(ms > acc->max_ms)
        acc->max_ms = ms;
    }
  }
}

// Deepest declared warmup demand (ms of 1m history) across every
// strategy attached to `mk`. 0 when the market has no attachments.
static int64_t
wm_market_required_lookback_ms(whenmoon_state_t *st, whenmoon_market_t *mk)
{
  wm_lb_acc_t acc;

  acc.market_id_str = mk->market_id_str;
  acc.max_ms        = 0;

  wm_strategy_loaded_iterate(st, wm_lb_iter_cb, &acc);

  return(acc.max_ms);
}

// --------------------------------------------------------------------
// Gap-fill + generation helpers
// --------------------------------------------------------------------

// Enqueue an authoritative DB gap-fill for every missing 1m window in
// [from_ms, to_ms]. Reuses the downloader's row-level gap walker + job
// queue — the candle table stays exchange-authoritative.
static void
wm_warmup_enqueue_gaps(whenmoon_state_t *st, whenmoon_market_t *mk,
    int64_t from_ms, int64_t to_ms)
{
  char          s0[WM_COV_TS_SZ];
  char          s1[WM_COV_TS_SZ];
  wm_coverage_t gaps[WM_WARM_MAX_GAPS];
  uint32_t      n;
  uint32_t      i;

  if(to_ms <= from_ms)
    return;

  wm_pg_ts_from_ms(from_ms, s0, sizeof(s0));
  wm_pg_ts_from_ms(to_ms,   s1, sizeof(s1));

  n = wm_gap_find_row_gaps(mk->market_id, s0, s1, gaps, WM_WARM_MAX_GAPS);

  for(i = 0; i < n; i++)
  {
    int64_t job_id = 0;
    char    derr[128];

    derr[0] = '\0';

    (void)wm_dl_job_enqueue(st, DL_JOB_CANDLES, mk->market_id,
        EXCHANGE_PRIO_USER_DOWNLOAD, mk->exchange_name, mk->product_id,
        gaps[i].first_ts, gaps[i].last_ts, "warmup", &job_id,
        derr, sizeof(derr));
  }
}

// Re-resolve the market and verify the timer still belongs to the live
// warmup generation. Returns NULL (caller frees ctx + ends) when the
// market was stopped or a newer warmup_begin superseded this timer.
//
// WM-MKT-ARR-UAF-1: LOCK-FREE — caller MUST hold
// ctx->st->markets->arr_lock (read) across this call and all use of the
// returned pointer.
static whenmoon_market_t *
wm_warm_timer_live(wm_warm_timer_ctx_t *ctx)
{
  whenmoon_market_t *mk;

  mk = wm_market_lookup_by_id(ctx->st, ctx->market_id_str);

  if(mk == NULL || mk->warmup_gen != ctx->gen)
    return(NULL);

  return(mk);
}

static void
wm_warm_set_state(whenmoon_market_t *mk, wm_warmup_state_t s)
{
  pthread_mutex_lock(&mk->lock);
  mk->warmup_state = s;
  pthread_mutex_unlock(&mk->lock);
}

// Schedule the first authoritative tail-fill for a now-READY market.
static void
wm_warmup_start_tailfill(whenmoon_state_t *st, const char *market_id_str,
    uint32_t gen)
{
  wm_warm_timer_ctx_t *tc;

  tc = mem_alloc("whenmoon", "warm_tailfill", sizeof(*tc));

  if(tc == NULL)
    return;

  tc->st    = st;
  tc->gen   = gen;
  tc->iters = 0;
  snprintf(tc->market_id_str, sizeof(tc->market_id_str), "%s",
      market_id_str);

  if(task_add_deferred("wm_warm_tailfill", TASK_ANY, 220,
         WM_WARM_TAILFILL_INTERVAL_MS, wm_market_warmup_tailfill_task, tc)
         == TASK_HANDLE_NONE)
    mem_free(tc);
}

// --------------------------------------------------------------------
// Timer tasks
// --------------------------------------------------------------------

static void
wm_market_warmup_recheck_task(task_t *t)
{
  wm_warm_timer_ctx_t *ctx;
  whenmoon_market_t   *mk;
  int64_t              lookback;
  int64_t              ring_ms;
  int64_t              eff;
  int64_t              now;
  bool                 converged = false;

  if(t == NULL)
    return;

  ctx = t->data;

  if(ctx == NULL)
  {
    t->state = TASK_ENDED;
    return;
  }

  // WM-MKT-ARR-UAF-1: wm_warm_timer_live returns a bare mk; hold rdlock
  // across the call and ALL use of mk below (until this task ends), so a
  // concurrent market remove cannot free the session under us.
  if(ctx->st == NULL || ctx->st->markets == NULL)
  {
    mem_free(ctx);
    t->state = TASK_ENDED;
    return;
  }

  pthread_rwlock_rdlock(&ctx->st->markets->arr_lock);
  mk = wm_warm_timer_live(ctx);

  if(mk == NULL)
  {
    pthread_rwlock_unlock(&ctx->st->markets->arr_lock);
    mem_free(ctx);
    t->state = TASK_ENDED;
    return;
  }

  lookback = wm_market_required_lookback_ms(ctx->st, mk);
  ring_ms  = (int64_t)mk->grain_cap[WM_GRAN_1M] * 60000;
  eff      = (lookback < ring_ms) ? lookback : ring_ms;
  now      = wm_now_ms();

  // Roster emptied while warming, or a window shorter than the live-close
  // tolerance — promote without chasing more history.
  if(lookback == 0 || eff <= WM_WARM_TAIL_TOLERANCE_MS)
    converged = true;

  if(!converged)
  {
    int64_t newest = wm_candle_newest_ms(mk->market_id);

    // Converged once the gap-fill has brought wm_candles_<id> to within
    // `tolerance` of now — the live feed closes the last sliver. Old
    // internal holes are tolerated (the aggregator smooths over them);
    // only tail recency matters for a warm, current replay.
    if(newest > 0 && (now - newest) < WM_WARM_TAIL_TOLERANCE_MS)
      converged = true;
  }

  if(!converged && ctx->iters >= WM_WARM_MAX_RECHECKS)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "warmup %s: not contiguous after %u rechecks — promoting with"
        " available history", mk->market_id_str, ctx->iters);
    converged = true;
  }

  if(converged)
  {
    uint32_t limit = (eff > 0) ? (uint32_t)(eff / 60000) : 0;

    // Replay the recent window from the (now-filled) DB → cascade warms
    // every grain. load_history takes mk->lock internally.
    wm_aggregator_load_history(ctx->st, mk->market_id_str, limit);
    wm_warm_set_state(mk, WM_WARM_READY);

    clam(CLAM_INFO, WHENMOON_CTX,
        "warmup %s: ready (replayed up to %u 1m bars)",
        mk->market_id_str, limit);

    wm_warmup_start_tailfill(ctx->st, mk->market_id_str, ctx->gen);

    pthread_rwlock_unlock(&ctx->st->markets->arr_lock);
    mem_free(ctx);
    t->state = TASK_ENDED;
    return;
  }

  // Not converged: poll. The initial gap-fill (wm_market_warmup_begin)
  // plus the downloader's own stall supervisor drive the fill — do NOT
  // re-enqueue every tick (that floods the job queue with duplicates
  // while pages are still in flight). Re-issue occasionally (~every 60 s)
  // only as a backstop against permanently dropped jobs.
  if(ctx->iters > 0 && (ctx->iters % 12) == 0)
    wm_warmup_enqueue_gaps(ctx->st, mk, now - eff, now);

  ctx->iters++;

  if(task_add_deferred("wm_warm_recheck", TASK_ANY, 200,
         WM_WARM_RECHECK_INTERVAL_MS, wm_market_warmup_recheck_task, ctx)
         == TASK_HANDLE_NONE)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "warmup %s: recheck reschedule failed — stuck warming",
        mk->market_id_str);
    mem_free(ctx);
  }

  // WM-MKT-ARR-UAF-1: last use of mk done — release the container rdlock.
  pthread_rwlock_unlock(&ctx->st->markets->arr_lock);

  t->state = TASK_ENDED;
}

static void
wm_market_warmup_tailfill_task(task_t *t)
{
  wm_warm_timer_ctx_t *ctx;
  whenmoon_market_t   *mk;
  int64_t              now;

  if(t == NULL)
    return;

  ctx = t->data;

  if(ctx == NULL)
  {
    t->state = TASK_ENDED;
    return;
  }

  // WM-MKT-ARR-UAF-1: wm_warm_timer_live returns a bare mk; hold rdlock
  // across the call and ALL use of mk below (until this task ends).
  if(ctx->st == NULL || ctx->st->markets == NULL)
  {
    mem_free(ctx);
    t->state = TASK_ENDED;
    return;
  }

  pthread_rwlock_rdlock(&ctx->st->markets->arr_lock);
  mk = wm_warm_timer_live(ctx);

  if(mk == NULL)
  {
    pthread_rwlock_unlock(&ctx->st->markets->arr_lock);
    mem_free(ctx);
    t->state = TASK_ENDED;
    return;
  }

  now = wm_now_ms();
  wm_warmup_enqueue_gaps(ctx->st, mk, now - WM_WARM_TAILFILL_WINDOW_MS, now);

  if(task_add_deferred("wm_warm_tailfill", TASK_ANY, 220,
         WM_WARM_TAILFILL_INTERVAL_MS, wm_market_warmup_tailfill_task, ctx)
         == TASK_HANDLE_NONE)
    mem_free(ctx);

  // WM-MKT-ARR-UAF-1: last use of mk done — release the container rdlock.
  pthread_rwlock_unlock(&ctx->st->markets->arr_lock);

  t->state = TASK_ENDED;
}

// --------------------------------------------------------------------
// Public entry
// --------------------------------------------------------------------

void
wm_market_warmup_begin(whenmoon_state_t *st, whenmoon_market_t *mk)
{
  int64_t              lookback;
  int64_t              ring_ms;
  int64_t              eff;
  int64_t              now;
  uint32_t             gen;
  wm_warm_timer_ctx_t *ctx;

  if(st == NULL || mk == NULL)
    return;

  // Bump the generation so any timer from a prior warmup (or a previous
  // life of this market slot) retires on its next tick.
  pthread_mutex_lock(&mk->lock);
  mk->warmup_gen++;
  gen = mk->warmup_gen;
  pthread_mutex_unlock(&mk->lock);

  lookback = wm_market_required_lookback_ms(st, mk);

  if(lookback == 0)
  {
    // Feed-only (no strategies): shallow full-ring DB warm so /show
    // indicators + charts populate, then READY. Nothing gates on advice
    // because no strategy emits any.
    wm_warmup_ctx_t *lc = mem_alloc("whenmoon", "warmup_ctx", sizeof(*lc));

    if(lc != NULL)
    {
      lc->st             = st;
      snprintf(lc->market_id_str, sizeof(lc->market_id_str), "%s",
          mk->market_id_str);
      lc->limit_override = 0;

      if(task_add_deferred("wm_warmup", TASK_ANY, 200, 50,
             wm_aggregator_load_history_task, lc) == TASK_HANDLE_NONE)
        mem_free(lc);
    }

    wm_warm_set_state(mk, WM_WARM_READY);

    clam(CLAM_INFO, WHENMOON_CTX,
        "warmup %s: feed-only (no strategies) -> ready",
        mk->market_id_str);
    return;
  }

  wm_warm_set_state(mk, WM_WARM_WARMING);

  ring_ms = (int64_t)mk->grain_cap[WM_GRAN_1M] * 60000;
  eff     = (lookback < ring_ms) ? lookback : ring_ms;
  now     = wm_now_ms();

  wm_warmup_enqueue_gaps(st, mk, now - eff, now);

  clam(CLAM_INFO, WHENMOON_CTX,
      "warmup %s: warming (lookback=%lld ms, eff=%lld ms)",
      mk->market_id_str, (long long)lookback, (long long)eff);

  ctx = mem_alloc("whenmoon", "warm_recheck", sizeof(*ctx));

  if(ctx == NULL)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "warmup %s: recheck ctx alloc failed — stuck warming",
        mk->market_id_str);
    return;
  }

  ctx->st    = st;
  ctx->gen   = gen;
  ctx->iters = 0;
  snprintf(ctx->market_id_str, sizeof(ctx->market_id_str), "%s",
      mk->market_id_str);

  if(task_add_deferred("wm_warm_recheck", TASK_ANY, 200,
         WM_WARM_RECHECK_INTERVAL_MS, wm_market_warmup_recheck_task, ctx)
         == TASK_HANDLE_NONE)
  {
    mem_free(ctx);
    clam(CLAM_WARN, WHENMOON_CTX,
        "warmup %s: recheck schedule failed — stuck warming",
        mk->market_id_str);
  }
}
