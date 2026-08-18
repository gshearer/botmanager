// botmanager — MIT
// warmup.c — WM-WARMUP-2 market warmup lifecycle: strategy-sized,
// DB-first gap-fill + convergence + authoritative tail-fill.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "aggregator.h"
#include "market.h"
#include "strategy.h"
#include "warmup.h"
#include "warm_chain.h"
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
// convergence re-check deferred task. The chain owns it — see
// warm_chain.h — and wm_warm_chain_close is the one place it is freed,
// whether the market promoted, retired, or a stop() drained the chain
// out from under it. The market is identified by canonical id
// (re-resolved each tick) plus the warmup generation it belongs to, so a
// stopped market or a superseding warmup_begin retires the timer.
//
// The tail-fill does NOT use this — it is one global periodic sweep keyed
// on the candle table, with no per-session context to carry
// (WM-TAILFILL-COALESCE-1; see warmup.h).
typedef struct
{
  whenmoon_state_t *st;
  char              market_id_str[WM_MARKET_ID_STR_SZ];
  uint32_t          gen;
  uint32_t          iters;
} wm_warm_timer_ctx_t;

static void wm_market_warmup_recheck_task(task_t *t);

static void wm_warm_recheck_rearm(wm_warm_chain_t *, pthread_rwlock_t *,
    const char *);

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
//
// Takes the candle table's identity BY VALUE, not a live
// whenmoon_market_t*: the tail-fill sweep calls this off a snapshot with
// the markets container lock released (the gap walk is a remote-Postgres
// round trip and must not be held under it), and several market sessions
// share one `market_id`. Returns the number of jobs enqueued.
static uint32_t
wm_warmup_enqueue_gaps(whenmoon_state_t *st, int32_t market_id,
    const char *exchange, const char *product_id,
    int64_t from_ms, int64_t to_ms)
{
  char          s0[WM_COV_TS_SZ];
  char          s1[WM_COV_TS_SZ];
  wm_coverage_t gaps[WM_WARM_MAX_GAPS];
  uint32_t      n;
  uint32_t      i;
  uint32_t      queued = 0;

  if(to_ms <= from_ms)
    return(0);

  wm_pg_ts_from_ms(from_ms, s0, sizeof(s0));
  wm_pg_ts_from_ms(to_ms,   s1, sizeof(s1));

  n = wm_gap_find_row_gaps(market_id, s0, s1, gaps, WM_WARM_MAX_GAPS);

  for(i = 0; i < n; i++)
  {
    int64_t job_id = 0;
    char    derr[128];

    derr[0] = '\0';

    if(wm_dl_job_enqueue(st, DL_JOB_CANDLES, market_id,
           EXCHANGE_PRIO_USER_DOWNLOAD, exchange, product_id,
           gaps[i].first_ts, gaps[i].last_ts, "warmup", &job_id,
           derr, sizeof(derr)) == SUCCESS)
      queued++;
  }

  return(queued);
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

// --------------------------------------------------------------------
// Timer tasks
// --------------------------------------------------------------------

// Schedule the next re-check and release the container rdlock, whichever
// way the arm goes. Every non-terminal exit of the body below funnels
// through here: three copies of this ten-line shape had drifted apart on
// which of them logged the refusal.
//
// WM-MKT-ARR-UAF-1: `arr` is the caller's cached container lock and is
// released on both paths — the close is not a substitute for it.
// `market_id_str` is the caller's live mk, read only on the refusal path
// — which is why it is a parameter and not fetched from the chain's ctx:
// past a successful arm the next hop may already have closed the chain,
// and a name taken from it there would be read out of freed memory.
static void
wm_warm_recheck_rearm(wm_warm_chain_t *chain, pthread_rwlock_t *arr,
    const char *market_id_str)
{
  if(wm_warm_chain_arm(chain, "wm_warm_recheck",
         WM_WARM_RECHECK_INTERVAL_MS, wm_market_warmup_recheck_task))
  {
    pthread_rwlock_unlock(arr);
    return;
  }

  // DEBUG, not WARN: since OBS-43 a refused arm has two meanings, and
  // the common one is correct behaviour — stop() is draining and this
  // chain is meant to end here. A warning that fires on every clean
  // unload is the mistake core/plugin.c:361 documents having made for
  // 1,739 lines.
  clam(CLAM_DEBUG, WHENMOON_CTX,
      "warmup %s: recheck not rescheduled (shutting down or submit"
      " failed)", market_id_str);

  pthread_rwlock_unlock(arr);
  wm_warm_chain_close(chain);
}

static void
wm_market_warmup_recheck_task(task_t *t)
{
  wm_warm_chain_t     *chain;
  wm_warm_timer_ctx_t *ctx;
  whenmoon_market_t   *mk;
  pthread_rwlock_t    *arr;
  char                 binding[WM_STRATEGY_NAME_SZ];
  int64_t              lookback;
  int64_t              ring_ms;
  int64_t              eff;
  int64_t              now;
  bool                 converged = false;

  if(t == NULL)
    return;

  chain    = t->data;
  t->state = TASK_ENDED;

  if(chain == NULL)
    return;

  // OBS-43: enter BEFORE anything of the plugin's is touched — above
  // all before arr_lock below. A body that takes the container lock and
  // only then discovers a drain is in progress has already touched the
  // state the drain is holding stop() open to protect.
  if(!wm_warm_chain_enter(chain))
  {
    wm_warm_chain_close(chain);
    return;
  }

  ctx = wm_warm_chain_ctx(chain);

  // WM-MKT-ARR-UAF-1: wm_warm_timer_live returns a bare mk; hold rdlock
  // across the call and ALL use of mk below (until this task ends), so a
  // concurrent market remove cannot free the session under us.
  if(ctx->st == NULL || ctx->st->markets == NULL)
  {
    wm_warm_chain_close(chain);
    return;
  }

  // Cache the container lock: past a successful wm_warm_chain_arm the
  // next hop may already be running and may already have closed the
  // chain, so nothing below an arm may dereference `ctx` again — not
  // even to find the lock it still has to release.
  arr = &ctx->st->markets->arr_lock;

  // OBS-45: read the declared binding BEFORE the container lock, off the
  // ctx's own copy of the id. wm_market_add reads this key with no
  // arr_lock held and that is the tree's only precedent for the pair;
  // taking KV under the container lock would invent a second order for
  // no gain.
  wm_strategy_binding_get(ctx->market_id_str, binding, sizeof(binding));

  pthread_rwlock_rdlock(arr);
  mk = wm_warm_timer_live(ctx);

  if(mk == NULL)
  {
    pthread_rwlock_unlock(arr);
    wm_warm_chain_close(chain);
    return;
  }

  lookback = wm_market_required_lookback_ms(ctx->st, mk);

  // OBS-45: an empty roster on a market that DECLARES an advisor is not
  // convergence — it is a reload cascade, which unloads every strategy
  // .so before whenmoon's own stop() and leaves the registry honestly
  // empty for the length of it. Promoting on that reading warms the
  // market by a roster that is merely invisible, and because eff is then
  // 0 the replay below asks load_history for `0` — which that function
  // reads as the FULL 1m ring, the widest possible window reached from
  // the narrowest possible intent. Measured 2026-08-17: 288,000 bars and
  // ~9.7 s each on three sessions at once, inside stop()'s drain.
  //
  // So hold: re-arm without advancing iters (the herd-cap branch's
  // precedent — this tick did no work worth counting) and without
  // replaying anything. A cascade then drains the chain; a runtime
  // attach bumps warmup_gen and retires it. Nothing here re-attaches,
  // and nothing should: wm_strategy_detach_self already WARNs the
  // operator by name (OBS-57), which is the signal this hold rides on.
  if(lookback == 0 && binding[0] != '\0')
  {
    clam(CLAM_DEBUG, WHENMOON_CTX,
        "warmup %s: roster empty but binding names '%s' — holding"
        " (the advisor's .so is gone, not detached)",
        mk->market_id_str, binding);

    wm_warm_recheck_rearm(chain, arr, mk->market_id_str);
    return;
  }

  ring_ms = (int64_t)mk->grain_cap[WM_GRAN_1M] * 60000;
  eff     = (lookback < ring_ms) ? lookback : ring_ms;
  now     = wm_now_ms();

  // Genuinely feed-only (the binding is clear), or a window shorter than
  // the live-close tolerance — promote without chasing more history.
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

  // WM-WARMUP-HERD-1 STEP 1: converged, but the full-ring replay herd is
  // at the cap — poll again shortly WITHOUT advancing iters or re-walking
  // gaps (we're already contiguous, just waiting for a replay slot). Keeps
  // the bulk-restore remote-DB fetches staggered instead of simultaneous.
  if(converged && !wm_warmup_try_acquire())
  {
    clam(CLAM_INFO, WHENMOON_CTX,
        "warmup %s: converged, %u warmups active (cap %u) — deferring replay",
        mk->market_id_str, wm_warmup_active_count(),
        WM_WARMUP_MAX_CONCURRENT);

    wm_warm_recheck_rearm(chain, arr, mk->market_id_str);
    return;
  }

  if(converged)
  {
    // We hold a warmup slot from the try_acquire above.
    uint32_t limit = (eff > 0) ? (uint32_t)(eff / 60000) : 0;

    // Replay the recent window from the (now-filled) DB → cascade warms
    // every grain. load_history takes mk->lock internally.
    wm_aggregator_load_history(ctx->st, mk->market_id_str, limit);
    wm_warmup_release();
    wm_warm_set_state(mk, WM_WARM_READY);

    clam(CLAM_INFO, WHENMOON_CTX,
        "warmup %s: ready (replayed up to %u 1m bars)",
        mk->market_id_str, limit);

    // WM-TAILFILL-COALESCE-1: nothing to schedule here. The tail-fill is
    // one global sweep keyed on the candle table, and it picks this market
    // up on its next tick now that the state is READY.

    pthread_rwlock_unlock(arr);
    wm_warm_chain_close(chain);
    return;
  }

  // Not converged: poll. The initial gap-fill (wm_market_warmup_begin)
  // plus the downloader's own stall supervisor drive the fill — do NOT
  // re-enqueue every tick (that floods the job queue with duplicates
  // while pages are still in flight). Re-issue occasionally (~every 60 s)
  // only as a backstop against permanently dropped jobs.
  if(ctx->iters > 0 && (ctx->iters % 12) == 0)
    (void)wm_warmup_enqueue_gaps(ctx->st, mk->market_id, mk->exchange_name,
        mk->product_id, now - eff, now);

  ctx->iters++;

  // WM-MKT-ARR-UAF-1: last use of mk done — the re-arm releases the
  // container rdlock on both of its paths.
  wm_warm_recheck_rearm(chain, arr, mk->market_id_str);
}

// --------------------------------------------------------------------
// WM-TAILFILL-COALESCE-1: global tail-fill sweep
//
// ONE periodic task for the plugin. Each tick: snapshot the DISTINCT
// candle tables behind the READY markets, drop the container lock, then
// tail-fill each table exactly once.
//
// Keyed on `market_id` (the table), NOT `market_id_str` (the session).
// Those differ in cardinality — N strategy instances of one product are N
// sessions sharing ONE table — and the gap walk + download enqueue are
// per-table work. See warmup.h.
// --------------------------------------------------------------------

// One distinct candle table, copied out from under the container lock.
typedef struct
{
  int32_t market_id;
  char    exchange[EXCHANGE_NAME_SZ];
  char    product_id[WM_PRODUCT_ID_SZ];
} wm_warm_table_t;

static task_handle_t wm_warm_g_tailfill_task = TASK_HANDLE_NONE;

// Phase A: reduce the READY markets to their distinct candle tables.
// Caller must hold st->markets->arr_lock (read). Returns the count.
static uint32_t
wm_warm_collect_tables(whenmoon_state_t *st, wm_warm_table_t *out,
    uint32_t cap)
{
  uint32_t n = 0;
  uint32_t i;

  for(i = 0; i < st->markets->n_markets; i++)
  {
    whenmoon_market_t *mk = st->markets->arr[i];
    wm_warmup_state_t  ws;
    uint32_t           j;
    bool               dup = false;

    if(mk == NULL)
      continue;

    // Lock order is arr_lock -> mk->lock, never the reverse (market.h:371).
    pthread_mutex_lock(&mk->lock);
    ws = mk->warmup_state;
    pthread_mutex_unlock(&mk->lock);

    // Only READY markets: a WARMING one is already being gap-filled by
    // its own convergence re-check — don't race it.
    if(ws != WM_WARM_READY)
      continue;

    for(j = 0; j < n; j++)
    {
      if(out[j].market_id == mk->market_id)
      {
        dup = true;
        break;
      }
    }

    // The whole point: N sessions of one product collapse to one table.
    if(dup)
      continue;

    if(n >= cap)
    {
      clam(CLAM_WARN, WHENMOON_CTX,
          "tailfill: more than %u distinct candle tables — skipping"
          " market_id=%d this sweep", cap, mk->market_id);
      break;
    }

    out[n].market_id = mk->market_id;
    snprintf(out[n].exchange, sizeof(out[n].exchange), "%s",
        mk->exchange_name);
    snprintf(out[n].product_id, sizeof(out[n].product_id), "%s",
        mk->product_id);
    n++;
  }

  return(n);
}

static void
wm_warm_tailfill_task(task_t *t)
{
  whenmoon_state_t *st;
  wm_warm_table_t   tabs[WM_WARM_TAILFILL_MAX_TABLES];
  uint32_t          n;
  uint32_t          i;
  uint32_t          queued = 0;
  int64_t           now;

  if(t == NULL)
    return;

  st = t->data;

  if(st == NULL || st->markets == NULL)
  {
    t->state = TASK_ENDED;
    return;
  }

  pthread_rwlock_rdlock(&st->markets->arr_lock);
  n = wm_warm_collect_tables(st, tabs, WM_WARM_TAILFILL_MAX_TABLES);
  pthread_rwlock_unlock(&st->markets->arr_lock);

  // Lock released: the gap walk below is a remote-Postgres round trip and
  // must not run under the container rdlock. `tabs` is a by-value snapshot,
  // so a concurrent market remove cannot dangle us (WM-MKT-ARR-UAF-1).
  now = wm_now_ms();

  for(i = 0; i < n; i++)
    queued += wm_warmup_enqueue_gaps(st, tabs[i].market_id,
        tabs[i].exchange, tabs[i].product_id,
        now - WM_WARM_TAILFILL_WINDOW_MS, now);

  // DEBUG, not INFO: the live aggregator never persists closed bars, so
  // the tail is always a few minutes behind and this fires on virtually
  // every 3-minute tick — at INFO it's a heartbeat that drowns real events
  // (and leaks into CLAM subscribers like the #cabal fills announcer).
  if(queued > 0)
    clam(CLAM_DEBUG, WHENMOON_CTX,
        "tailfill: %u gap job(s) enqueued across %u candle table(s)",
        queued, n);

  t->state = TASK_ENDED;
}

bool
wm_warm_tailfill_global_init(whenmoon_state_t *st)
{
  if(wm_warm_g_tailfill_task != TASK_HANDLE_NONE)
    return(SUCCESS);

  if(st == NULL)
    return(FAIL);

  wm_warm_g_tailfill_task = task_add_periodic("wm_warm_tailfill", TASK_ANY,
      220, WM_WARM_TAILFILL_INTERVAL_MS, wm_warm_tailfill_task, st);

  if(wm_warm_g_tailfill_task == TASK_HANDLE_NONE)
  {
    clam(CLAM_WARN, WHENMOON_CTX, "tailfill sweep task submit failed");
    return(FAIL);
  }

  return(SUCCESS);
}

void
wm_warm_tailfill_global_destroy(void)
{
  if(wm_warm_g_tailfill_task != TASK_HANDLE_NONE)
  {
    task_cancel(wm_warm_g_tailfill_task);
    wm_warm_g_tailfill_task = TASK_HANDLE_NONE;
  }
}

// --------------------------------------------------------------------
// Public entry
// --------------------------------------------------------------------

void
wm_market_warmup_begin(whenmoon_state_t *st, whenmoon_market_t *mk)
{
  char                 binding[WM_STRATEGY_NAME_SZ];
  int64_t              lookback;
  int64_t              ring_ms;
  int64_t              eff;
  int64_t              now;
  uint32_t             gen;
  wm_warm_timer_ctx_t *ctx;
  wm_warm_chain_t     *chain;

  if(st == NULL || mk == NULL)
    return;

  // Bump the generation so any timer from a prior warmup (or a previous
  // life of this market slot) retires on its next tick.
  pthread_mutex_lock(&mk->lock);
  mk->warmup_gen++;
  gen = mk->warmup_gen;
  pthread_mutex_unlock(&mk->lock);

  lookback = wm_market_required_lookback_ms(st, mk);

  wm_strategy_binding_get(mk->market_id_str, binding, sizeof(binding));

  // OBS-45: the roster is only half the question — see the recheck
  // above. A market whose binding is CLEAR has no advisor and never
  // will, so an empty roster is the whole truth and full-ring is the
  // deliberate depth. A market whose binding NAMES one and has no
  // attachment is the other case entirely: wm_market_add's binding
  // attach failed (the .so is not loaded, or a cascade has it), and
  // reading that as feed-only would promote straight to READY on an
  // advisor that is merely absent.
  if(lookback == 0 && binding[0] == '\0')
  {
    // Feed-only (no strategies): shallow full-ring DB warm so /show
    // indicators + charts populate, then READY. Nothing gates on advice
    // because no strategy emits any.
    wm_warmup_ctx_t *lc = mem_alloc("whenmoon", "warmup_ctx", sizeof(*lc));
    wm_warm_chain_t *chain;

    lc->st             = st;
    snprintf(lc->market_id_str, sizeof(lc->market_id_str), "%s",
        mk->market_id_str);
    lc->limit_override = 0;

    chain = wm_warm_chain_open(lc, mem_free);

    if(chain == NULL)
      mem_free(lc);

    else if(!wm_warm_chain_arm(chain, "wm_warmup", 50,
        wm_aggregator_load_history_task))
      wm_warm_chain_close(chain);

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

  // eff is 0 in the binding-without-attachment case, and enqueue_gaps
  // answers a zero-width window with 0 before it touches the DB.
  (void)wm_warmup_enqueue_gaps(st, mk->market_id, mk->exchange_name,
      mk->product_id, now - eff, now);

  if(lookback == 0)
    clam(CLAM_WARN, WHENMOON_CTX,
        "warmup %s: binding names strategy '%s' but nothing is attached"
        " — held warming, no advice can reach this market until it is"
        " (/whenmoon strategy attach %s %s)",
        mk->market_id_str, binding, mk->market_id_str, binding);

  else
    clam(CLAM_INFO, WHENMOON_CTX,
        "warmup %s: warming (lookback=%lld ms, eff=%lld ms)",
        mk->market_id_str, (long long)lookback, (long long)eff);

  ctx = mem_alloc("whenmoon", "warm_recheck", sizeof(*ctx));

  ctx->st    = st;
  ctx->gen   = gen;
  ctx->iters = 0;
  snprintf(ctx->market_id_str, sizeof(ctx->market_id_str), "%s",
      mk->market_id_str);

  chain = wm_warm_chain_open(ctx, mem_free);

  if(chain == NULL)
  {
    mem_free(ctx);
    clam(CLAM_WARN, WHENMOON_CTX,
        "warmup %s: recheck not scheduled (plugin stopping) — stuck"
        " warming", mk->market_id_str);
  }

  else if(!wm_warm_chain_arm(chain, "wm_warm_recheck",
      WM_WARM_RECHECK_INTERVAL_MS, wm_market_warmup_recheck_task))
  {
    wm_warm_chain_close(chain);
    clam(CLAM_WARN, WHENMOON_CTX,
        "warmup %s: recheck schedule failed — stuck warming",
        mk->market_id_str);
  }
}
