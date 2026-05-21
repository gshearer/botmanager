// botmanager — MIT
// Marketwatch substrate (MW-2): per-exchange bulk-ticker polling.
//
// One periodic task per enabled exchange fires
// `exchange_fetch_all_tickers_async` at the cadence read from KV. The
// typed response callback walks the snapshot array under the per-
// exchange lock, finds-or-inserts each pair in a fixed-size table, and
// pushes the row into the pair's ring. No detection signals, no
// `mw.*` CLAM emission — that lands in MW-3.
//
// Threading: one lock per exchange (`ex->lock`) guards pairs[] + ring
// memory + counters; the global `mw_g.mtx` guards only the exchange
// roster + global enabled flag. The two are never held simultaneously.
// Per-exchange tasks run on TASK_THREAD workers; response callbacks
// fire on the curl worker thread of the protocol plugin.
//
// Storage is eager: at mw_start every registered exchange gets one
// pairs[] allocation (MW_PAIRS_CAP rows) and every pair gets one ring
// of `mw_g.ring_n` rows. ~17 MB across three exchanges with defaults
// — within budget and simpler than lazy allocation when ticks fire
// across all exchanges.

#define WHENMOON_INTERNAL
#include "mw.h"
#include "whenmoon.h"
#include "dl_jobtable.h"   // wm_dl_now_ms
#include "exchange_api.h"
#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "kv.h"
#include "method.h"
#include "task.h"

#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------------ //
// Constants                                                           //
// ------------------------------------------------------------------ //

#define MW_PAIRS_CAP            2048
#define MW_RING_N_DEFAULT         60
#define MW_POLL_SEC_DEFAULT       60
#define MW_MAX_EXCH                8
#define MW_INFLIGHT_GUARD_MS    5000
#define MW_EXCH_LIST_CAP          16

#define MW_KV_GLOBAL_ENABLED   "plugin.whenmoon.mw.enabled"
#define MW_KV_RING_N           "plugin.whenmoon.mw.ring_n"

// Per-exchange KV key buffer size. Longest tail is ".enabled" (8
// bytes) below "plugin.whenmoon.mw." (19) + EXCHANGE_NAME_SZ (32) +
// NUL — comfortably fits 96 bytes.
#define MW_KV_KEY_SZ              96

// Output line widths.
#define MW_LINE_SZ                256

// Per-render top-N cutoffs.
#define MW_TOPN                    10

// ------------------------------------------------------------------ //
// State                                                               //
// ------------------------------------------------------------------ //

typedef struct
{
  char                          product_id[EXCHANGE_PRODUCT_ID_SZ];
  int64_t                       last_seen_ms;
  uint32_t                      snap_count;
  uint32_t                      ring_head;
  exchange_ticker_snapshot_t   *ring;
} mw_pair_t;

typedef struct
{
  char                  name[EXCHANGE_NAME_SZ];
  bool                  enabled;
  uint32_t              poll_sec;
  task_handle_t         task;
  pthread_mutex_t       lock;
  uint32_t              pair_count;
  uint32_t              pair_cap;
  mw_pair_t            *pairs;
  int64_t               last_poll_ms;
  int64_t               last_dispatch_ms;
  uint64_t              total_polls;
  uint64_t              total_pairs_seen;
  uint64_t              total_drops_full;
} mw_exch_t;

typedef struct
{
  bool             ready;          // true once mw_init has fully run
  pthread_mutex_t  mtx;
  bool             global_enabled;
  uint32_t         ring_n;
  uint32_t         n_exch;
  mw_exch_t        exch[MW_MAX_EXCH];
} mw_state_t;

static mw_state_t mw_g;

// ------------------------------------------------------------------ //
// Static helpers                                                      //
// ------------------------------------------------------------------ //

static void
mw_kv_key(char *buf, size_t cap, const char *exch, const char *tail)
{
  // Bounded copy so the const-pointer length is provably <
  // EXCHANGE_NAME_SZ to silence -Wformat-truncation on
  // %s-into-fixed-buffer concatenations.
  char ename[EXCHANGE_NAME_SZ];
  size_t n = strnlen(exch, EXCHANGE_NAME_SZ - 1);

  memcpy(ename, exch, n);
  ename[n] = '\0';

  snprintf(buf, cap, "plugin.whenmoon.mw.%s.%s", ename, tail);
}

static mw_exch_t *
mw_find_exch_by_name(const char *name)
{
  uint32_t i;

  if(name == NULL || name[0] == '\0')
    return(NULL);

  for(i = 0; i < mw_g.n_exch; i++)
  {
    if(strncmp(mw_g.exch[i].name, name, EXCHANGE_NAME_SZ) == 0)
      return(&mw_g.exch[i]);
  }

  return(NULL);
}

static uint32_t
mw_pair_find_or_insert(mw_exch_t *ex, const char *product_id)
{
  uint32_t i;

  for(i = 0; i < ex->pair_count; i++)
  {
    if(strncmp(ex->pairs[i].product_id, product_id,
          EXCHANGE_PRODUCT_ID_SZ) == 0)
      return(i);
  }

  if(ex->pair_count >= ex->pair_cap)
    return(UINT32_MAX);

  i = ex->pair_count++;
  snprintf(ex->pairs[i].product_id, sizeof(ex->pairs[i].product_id),
      "%s", product_id);
  ex->pairs[i].snap_count = 0;
  ex->pairs[i].ring_head  = 0;
  return(i);
}

static void
mw_ring_push(mw_pair_t *pp, const exchange_ticker_snapshot_t *snap,
    uint32_t ring_n, int64_t now_ms)
{
  if(ring_n == 0 || pp->ring == NULL)
    return;

  memcpy(&pp->ring[pp->ring_head], snap, sizeof(*snap));
  pp->ring_head = (pp->ring_head + 1) % ring_n;
  pp->last_seen_ms = now_ms;

  if(pp->snap_count < ring_n)
    pp->snap_count++;
}

// Read latest snapshot from a pair's ring without copying. Caller
// holds ex->lock. Returns NULL when snap_count == 0.
static const exchange_ticker_snapshot_t *
mw_pair_latest(const mw_pair_t *pp, uint32_t ring_n)
{
  uint32_t idx;

  if(pp->snap_count == 0 || pp->ring == NULL || ring_n == 0)
    return(NULL);

  // ring_head points to the next write slot — the latest sample is
  // one slot behind, modulo ring_n.
  idx = (pp->ring_head + ring_n - 1) % ring_n;
  return(&pp->ring[idx]);
}

// ------------------------------------------------------------------ //
// Task callbacks                                                      //
// ------------------------------------------------------------------ //

static void
mw_tickers_done_cb(bool success, const char *err,
    const exchange_ticker_snapshot_t *snaps, size_t n, void *user)
{
  mw_exch_t *ex = user;
  size_t     i;
  uint32_t   slot;
  uint32_t   drops_this_tick = 0;
  uint32_t   ring_n;
  int64_t    now_ms;

  if(ex == NULL)
    return;

  if(!success)
  {
    clam(CLAM_WARN, MW_CTX, "%s: tick FAIL: %s",
        ex->name, err != NULL ? err : "?");
    return;
  }

  pthread_mutex_lock(&ex->lock);

  // Disabled between dispatch + completion — drop result.
  if(!ex->enabled)
  {
    pthread_mutex_unlock(&ex->lock);
    return;
  }

  ring_n = mw_g.ring_n;
  now_ms = wm_dl_now_ms();

  for(i = 0; i < n; i++)
  {
    slot = mw_pair_find_or_insert(ex, snaps[i].product_id);

    if(slot == UINT32_MAX)
    {
      drops_this_tick++;
      continue;
    }

    mw_ring_push(&ex->pairs[slot], &snaps[i], ring_n, now_ms);
  }

  ex->last_poll_ms       = now_ms;
  ex->total_polls++;
  ex->total_pairs_seen   = (uint64_t)n;
  ex->total_drops_full  += drops_this_tick;

  pthread_mutex_unlock(&ex->lock);

  if(drops_this_tick > 0)
    clam(CLAM_WARN, MW_CTX, "%s: %u pairs dropped (cap=%u)",
        ex->name, drops_this_tick, (unsigned)MW_PAIRS_CAP);
  else
    clam(CLAM_DEBUG3, MW_CTX, "%s: tick ok n=%zu", ex->name, n);
}

static void
mw_periodic_cb(task_t *t)
{
  mw_exch_t *ex;
  int64_t    now_ms;
  bool       dispatch;

  if(t == NULL)
    return;

  ex = t->data;
  dispatch = true;

  if(ex == NULL)
  {
    t->state = TASK_ENDED;
    return;
  }

  pthread_mutex_lock(&ex->lock);

  now_ms = wm_dl_now_ms();

  // Inflight guard. last_dispatch_ms > last_poll_ms means a previous
  // dispatch has not yet been observed. Skip a tick if the response
  // hasn't shown up within the guard window — prevents stacking 60
  // simultaneous requests on an outage at a 60s cadence.
  if(ex->last_dispatch_ms > ex->last_poll_ms
      && (now_ms - ex->last_dispatch_ms) < MW_INFLIGHT_GUARD_MS)
  {
    dispatch = false;
    clam(CLAM_DEBUG3, MW_CTX,
        "%s: skip tick (inflight %" PRId64 "ms)",
        ex->name, now_ms - ex->last_dispatch_ms);
  }

  else
    ex->last_dispatch_ms = now_ms;

  pthread_mutex_unlock(&ex->lock);

  if(dispatch
      && exchange_fetch_all_tickers_async(ex->name,
            mw_tickers_done_cb, ex) != SUCCESS)
    clam(CLAM_WARN, MW_CTX, "%s: dispatch FAIL", ex->name);

  t->state = TASK_ENDED;   // periodic — "iteration done, reschedule"
}

// ------------------------------------------------------------------ //
// Internal lifecycle helpers                                          //
// ------------------------------------------------------------------ //

// Allocates pairs[] + per-pair rings for one exchange slot. Returns
// SUCCESS on success; on failure any partial allocation is rolled back
// and the slot is left in its zeroed state.
static bool
mw_exch_alloc_pairs(mw_exch_t *ex, uint32_t ring_n)
{
  uint32_t i;

  ex->pairs = mem_alloc(WHENMOON_CTX, "mw.pairs",
      (size_t)MW_PAIRS_CAP * sizeof(*ex->pairs));

  if(ex->pairs == NULL)
    return(FAIL);

  memset(ex->pairs, 0, (size_t)MW_PAIRS_CAP * sizeof(*ex->pairs));
  ex->pair_cap   = MW_PAIRS_CAP;
  ex->pair_count = 0;

  for(i = 0; i < MW_PAIRS_CAP; i++)
  {
    ex->pairs[i].ring = mem_alloc(WHENMOON_CTX, "mw.ring",
        (size_t)ring_n * sizeof(*ex->pairs[i].ring));

    if(ex->pairs[i].ring == NULL)
    {
      // Roll back: free already-allocated rings, then pairs[].
      uint32_t j;

      for(j = 0; j < i; j++)
        mem_free(ex->pairs[j].ring);

      mem_free(ex->pairs);
      ex->pairs    = NULL;
      ex->pair_cap = 0;
      return(FAIL);
    }
  }

  return(SUCCESS);
}

static void
mw_exch_free_pairs(mw_exch_t *ex)
{
  uint32_t i;

  if(ex->pairs == NULL)
    return;

  for(i = 0; i < ex->pair_cap; i++)
  {
    if(ex->pairs[i].ring != NULL)
    {
      mem_free(ex->pairs[i].ring);
      ex->pairs[i].ring = NULL;
    }
  }

  mem_free(ex->pairs);
  ex->pairs      = NULL;
  ex->pair_cap   = 0;
  ex->pair_count = 0;
}

// Spawn the per-exchange periodic task. Caller holds mw_g.mtx; ex
// must already have pairs[] allocated and enabled=true.
static void
mw_exch_kick_task(mw_exch_t *ex)
{
  char tname[TASK_NAME_SZ];

  if(ex->task != TASK_HANDLE_NONE)
    return;

  snprintf(tname, sizeof(tname), "mw.%s", ex->name);

  ex->task = task_add_periodic(tname, TASK_THREAD, 100,
      ex->poll_sec * 1000u, mw_periodic_cb, ex);

  if(ex->task == TASK_HANDLE_NONE)
    clam(CLAM_WARN, MW_CTX,
        "%s: task_add_periodic FAIL — operator can retry"
        " with /whenmoon mw disable/enable", ex->name);
  else
    clam(CLAM_INFO, MW_CTX, "%s: polling every %us",
        ex->name, ex->poll_sec);
}

static void
mw_exch_cancel_task(mw_exch_t *ex)
{
  if(ex->task == TASK_HANDLE_NONE)
    return;

  task_cancel(ex->task);
  ex->task = TASK_HANDLE_NONE;
}

// ------------------------------------------------------------------ //
// Lifecycle                                                           //
// ------------------------------------------------------------------ //

bool
mw_init(void)
{
  memset(&mw_g, 0, sizeof(mw_g));

  if(pthread_mutex_init(&mw_g.mtx, NULL) != 0)
  {
    clam(CLAM_INFO, MW_CTX, "pthread_mutex_init FAIL");
    return(FAIL);
  }

  // Register KV knobs up front so kv_load picks up persisted values
  // before mw_start consults them. Per-exchange KVs are registered
  // lazily at mw_start (one set per registered exchange).
  if(kv_register(MW_KV_GLOBAL_ENABLED, KV_UINT32, "0", NULL, NULL,
        "Global enable for marketwatch (0/1).") != SUCCESS)
  {
    clam(CLAM_INFO, MW_CTX, "kv_register " MW_KV_GLOBAL_ENABLED " FAIL");
    pthread_mutex_destroy(&mw_g.mtx);
    return(FAIL);
  }

  if(kv_register(MW_KV_RING_N, KV_UINT32, "0", NULL, NULL,
        "Per-pair snapshot ring depth (0 = MW_RING_N_DEFAULT).") != SUCCESS)
  {
    clam(CLAM_INFO, MW_CTX, "kv_register " MW_KV_RING_N " FAIL");
    pthread_mutex_destroy(&mw_g.mtx);
    return(FAIL);
  }

  mw_g.ready = true;
  return(SUCCESS);
}

bool
mw_start(void)
{
  char     names[MW_EXCH_LIST_CAP][EXCHANGE_NAME_SZ];
  uint32_t n_reg = 0;
  uint32_t i;
  uint64_t v;

  mw_g.global_enabled = kv_get_uint(MW_KV_GLOBAL_ENABLED) != 0;

  v = kv_get_uint(MW_KV_RING_N);

  if(v == 0)
    mw_g.ring_n = MW_RING_N_DEFAULT;
  else
    mw_g.ring_n = (uint32_t)v;

  if(exchange_name_list(names, MW_EXCH_LIST_CAP, &n_reg) != SUCCESS)
  {
    clam(CLAM_INFO, MW_CTX, "exchange_name_list FAIL");
    return(SUCCESS);   // empty roster is not fatal
  }

  if(n_reg > MW_EXCH_LIST_CAP)
    n_reg = MW_EXCH_LIST_CAP;

  if(n_reg > MW_MAX_EXCH)
    n_reg = MW_MAX_EXCH;

  pthread_mutex_lock(&mw_g.mtx);

  mw_g.n_exch = 0;

  for(i = 0; i < n_reg; i++)
  {
    mw_exch_t *ex = &mw_g.exch[mw_g.n_exch];
    char       key[MW_KV_KEY_SZ];
    char       help[160];

    memset(ex, 0, sizeof(*ex));
    snprintf(ex->name, sizeof(ex->name), "%s", names[i]);

    if(pthread_mutex_init(&ex->lock, NULL) != 0)
    {
      clam(CLAM_WARN, MW_CTX,
          "%s: pthread_mutex_init FAIL — skipping", ex->name);
      continue;
    }

    // Per-exchange KV registration. Keys are stable strings owned by
    // a single mw_kv_key fill; kv_register copies them internally.
    // A non-SUCCESS return here means the key was already registered
    // (e.g. on a re-entry); the persisted value still applies, so we
    // just continue past with a DBG note.
    mw_kv_key(key, sizeof(key), ex->name, "enabled");
    snprintf(help, sizeof(help),
        "Per-exchange marketwatch enable for %s (0/1).", ex->name);

    if(kv_register(key, KV_UINT32, "0", NULL, NULL, help) != SUCCESS)
      clam(CLAM_DEBUG, MW_CTX,
          "%s: kv_register .enabled already present", ex->name);

    mw_kv_key(key, sizeof(key), ex->name, "poll_sec");
    snprintf(help, sizeof(help),
        "Per-exchange marketwatch poll cadence (s) for %s"
        " (0 = MW_POLL_SEC_DEFAULT).", ex->name);

    if(kv_register(key, KV_UINT32, "0", NULL, NULL, help) != SUCCESS)
      clam(CLAM_DEBUG, MW_CTX,
          "%s: kv_register .poll_sec already present", ex->name);

    // Read persisted values back.
    mw_kv_key(key, sizeof(key), ex->name, "enabled");
    ex->enabled = kv_get_uint(key) != 0;

    mw_kv_key(key, sizeof(key), ex->name, "poll_sec");
    v = kv_get_uint(key);
    ex->poll_sec = v == 0 ? MW_POLL_SEC_DEFAULT : (uint32_t)v;

    ex->task = TASK_HANDLE_NONE;

    if(mw_exch_alloc_pairs(ex, mw_g.ring_n) != SUCCESS)
    {
      clam(CLAM_WARN, MW_CTX,
          "%s: pairs allocation FAIL — skipping", ex->name);
      pthread_mutex_destroy(&ex->lock);
      continue;
    }

    mw_g.n_exch++;

    if(mw_g.global_enabled && ex->enabled)
      mw_exch_kick_task(ex);
  }

  pthread_mutex_unlock(&mw_g.mtx);

  clam(CLAM_INFO, MW_CTX, "started: global=%s ring_n=%u tracked=%u",
      mw_g.global_enabled ? "enabled" : "disabled",
      mw_g.ring_n, mw_g.n_exch);

  return(SUCCESS);
}

void
mw_stop(void)
{
  uint32_t i;

  if(!mw_g.ready)
    return;

  pthread_mutex_lock(&mw_g.mtx);

  for(i = 0; i < mw_g.n_exch; i++)
    mw_exch_cancel_task(&mw_g.exch[i]);

  pthread_mutex_unlock(&mw_g.mtx);
}

void
mw_deinit(void)
{
  uint32_t i;

  if(!mw_g.ready)
    return;

  pthread_mutex_lock(&mw_g.mtx);

  for(i = 0; i < mw_g.n_exch; i++)
  {
    mw_exch_t *ex = &mw_g.exch[i];

    // Tasks were cancelled at mw_stop. task_cancel does not block on a
    // running callback — but the per-callback ex->lock acquisition
    // serialises us against any tail iteration that snuck through.
    pthread_mutex_lock(&ex->lock);
    mw_exch_free_pairs(ex);
    pthread_mutex_unlock(&ex->lock);
    pthread_mutex_destroy(&ex->lock);
  }

  mw_g.n_exch = 0;

  pthread_mutex_unlock(&mw_g.mtx);
  pthread_mutex_destroy(&mw_g.mtx);
  mw_g.ready = false;
}

// ------------------------------------------------------------------ //
// Runtime ops                                                         //
// ------------------------------------------------------------------ //

bool
mw_enable_exch(const char *name)
{
  mw_exch_t *ex;
  char       key[MW_KV_KEY_SZ];
  uint64_t   v;
  bool       kick;

  // Re-read poll_sec at enable time so an operator's `set kv
  // plugin.whenmoon.mw.<exch>.poll_sec <n>` between mw_start and the
  // enable verb actually takes effect on the spawned task.
  mw_kv_key(key, sizeof(key), name, "poll_sec");
  v = kv_get_uint(key);

  pthread_mutex_lock(&mw_g.mtx);

  ex = mw_find_exch_by_name(name);

  if(ex == NULL)
  {
    pthread_mutex_unlock(&mw_g.mtx);
    return(FAIL);
  }

  ex->enabled = true;
  if(v != 0)
    ex->poll_sec = (uint32_t)v;
  kick = mw_g.global_enabled;

  if(kick)
    mw_exch_kick_task(ex);

  pthread_mutex_unlock(&mw_g.mtx);

  mw_kv_key(key, sizeof(key), name, "enabled");
  (void)kv_set_uint(key, 1);

  return(SUCCESS);
}

bool
mw_disable_exch(const char *name)
{
  mw_exch_t *ex;
  char       key[MW_KV_KEY_SZ];
  uint32_t   i;

  pthread_mutex_lock(&mw_g.mtx);

  ex = mw_find_exch_by_name(name);

  if(ex == NULL)
  {
    pthread_mutex_unlock(&mw_g.mtx);
    return(FAIL);
  }

  ex->enabled = false;
  mw_exch_cancel_task(ex);

  // Clear pair table (slot 0 byte → empty). Ring memory stays
  // allocated for re-enable. Done under ex->lock so an in-flight
  // response cb (which checks ex->enabled at the top under the same
  // lock) cannot observe a half-cleared table.
  pthread_mutex_lock(&ex->lock);

  for(i = 0; i < ex->pair_count; i++)
    ex->pairs[i].product_id[0] = '\0';

  ex->pair_count = 0;

  pthread_mutex_unlock(&ex->lock);
  pthread_mutex_unlock(&mw_g.mtx);

  mw_kv_key(key, sizeof(key), name, "enabled");
  (void)kv_set_uint(key, 0);

  return(SUCCESS);
}

bool
mw_set_global_enabled(bool on)
{
  uint32_t i;

  pthread_mutex_lock(&mw_g.mtx);

  mw_g.global_enabled = on;

  for(i = 0; i < mw_g.n_exch; i++)
  {
    mw_exch_t *ex = &mw_g.exch[i];

    if(!ex->enabled)
      continue;

    if(on)
      mw_exch_kick_task(ex);
    else
      mw_exch_cancel_task(ex);
  }

  pthread_mutex_unlock(&mw_g.mtx);

  (void)kv_set_uint(MW_KV_GLOBAL_ENABLED, on ? 1 : 0);

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Render helpers                                                      //
// ------------------------------------------------------------------ //

static void
mw_fmt_age(char *buf, size_t cap, int64_t now_ms, int64_t then_ms)
{
  int64_t age_s;

  if(then_ms <= 0)
  {
    snprintf(buf, cap, "-");
    return;
  }

  age_s = (now_ms - then_ms) / 1000;

  if(age_s < 0)
    age_s = 0;

  if(age_s < 120)
    snprintf(buf, cap, "%llds ago", (long long)age_s);
  else if(age_s < 7200)
    snprintf(buf, cap, "%lldm ago", (long long)(age_s / 60));
  else
    snprintf(buf, cap, "%lldh ago", (long long)(age_s / 3600));
}

static void
mw_send(method_inst_t *inst, const char *target, const char *text)
{
  if(inst == NULL || target == NULL || target[0] == '\0' || text == NULL)
    return;

  method_send(inst, target, text);
}

void
mw_render_status(method_inst_t *inst, const char *target)
{
  char     line[MW_LINE_SZ];
  uint32_t i;
  int64_t  now_ms;

  pthread_mutex_lock(&mw_g.mtx);

  now_ms = wm_dl_now_ms();

  snprintf(line, sizeof(line),
      "marketwatch global=%s ring_n=%u",
      mw_g.global_enabled ? "enabled" : "disabled",
      mw_g.ring_n);
  mw_send(inst, target, line);

  snprintf(line, sizeof(line),
      "%-12s %-7s %-9s %-15s %6s %6s %6s",
      "exchange", "enabled", "poll_sec", "last_poll",
      "pairs", "polls", "drops");
  mw_send(inst, target, line);

  for(i = 0; i < mw_g.n_exch; i++)
  {
    mw_exch_t *ex = &mw_g.exch[i];
    char       age[32];
    uint32_t   pair_count;
    uint64_t   polls;
    uint64_t   drops;
    int64_t    last_poll_ms;

    pthread_mutex_lock(&ex->lock);
    pair_count   = ex->pair_count;
    polls        = ex->total_polls;
    drops        = ex->total_drops_full;
    last_poll_ms = ex->last_poll_ms;
    pthread_mutex_unlock(&ex->lock);

    mw_fmt_age(age, sizeof(age), now_ms, last_poll_ms);

    snprintf(line, sizeof(line),
        "%-12s %-7s %-9u %-15s %6u %6" PRIu64 " %6" PRIu64,
        ex->name,
        ex->enabled ? "yes" : "no",
        ex->poll_sec,
        age,
        pair_count,
        polls,
        drops);
    mw_send(inst, target, line);
  }

  if(mw_g.n_exch == 0)
    mw_send(inst, target, "(no exchanges registered)");

  pthread_mutex_unlock(&mw_g.mtx);
}

// One per-pair render row, with a pre-computed sort key (|pct_24h| or
// vol_24h_quote). Built per-render under ex->lock; only the latest
// snapshot is read per pair.
typedef struct
{
  char    product_id[EXCHANGE_PRODUCT_ID_SZ];
  double  key;
  double  price;
  double  pct_24h;
  double  vol_24h_quote;
} mw_topn_row_t;

static int
mw_topn_cmp_desc(const void *a, const void *b)
{
  const mw_topn_row_t *ra = a;
  const mw_topn_row_t *rb = b;

  // NaNs sink to the bottom — they never win a top-N spot.
  if(isnan(ra->key) && isnan(rb->key)) return(0);
  if(isnan(ra->key)) return( 1);
  if(isnan(rb->key)) return(-1);
  if(ra->key < rb->key) return( 1);
  if(ra->key > rb->key) return(-1);
  return(0);
}

// Build a top-N table sorted by the given key extractor. `out` is a
// caller-owned array of capacity MW_TOPN; returns the count actually
// populated (<= MW_TOPN). Caller holds ex->lock for the snapshot read.
static uint32_t
mw_build_topn(mw_topn_row_t *out, const mw_exch_t *ex, uint32_t ring_n,
    bool by_vol)
{
  // Heap-allocate the working set so very wide exchanges (Kraken
  // ~1500 pairs) don't blow the thread stack at ~24 bytes/row.
  mw_topn_row_t *work;
  uint32_t       i;
  uint32_t       w = 0;
  uint32_t       n_out;

  if(ex->pair_count == 0)
    return(0);

  work = mem_alloc(WHENMOON_CTX, "mw.topn",
      (size_t)ex->pair_count * sizeof(*work));

  if(work == NULL)
    return(0);

  for(i = 0; i < ex->pair_count; i++)
  {
    const exchange_ticker_snapshot_t *s =
        mw_pair_latest(&ex->pairs[i], ring_n);

    if(s == NULL || ex->pairs[i].product_id[0] == '\0')
      continue;

    snprintf(work[w].product_id, sizeof(work[w].product_id),
        "%s", ex->pairs[i].product_id);
    work[w].price         = s->price;
    work[w].pct_24h       = s->pct_24h;
    work[w].vol_24h_quote = s->vol_24h_quote;

    if(by_vol)
      work[w].key = s->vol_24h_quote;
    else
      work[w].key = isnan(s->pct_24h) ? NAN : fabs(s->pct_24h);

    w++;
  }

  if(w == 0)
  {
    mem_free(work);
    return(0);
  }

  qsort(work, w, sizeof(*work), mw_topn_cmp_desc);

  n_out = w < MW_TOPN ? w : MW_TOPN;

  // Filter out rows whose sort key is NaN — they convey no signal.
  while(n_out > 0 && isnan(work[n_out - 1].key))
    n_out--;

  memcpy(out, work, (size_t)n_out * sizeof(*out));

  mem_free(work);
  return(n_out);
}

static void
mw_fmt_dbl(char *buf, size_t cap, const char *fmt, double v)
{
  if(isnan(v))
    snprintf(buf, cap, "-");
  else
    snprintf(buf, cap, fmt, v);
}

static void
mw_render_topn(method_inst_t *inst, const char *target,
    const mw_topn_row_t *rows, uint32_t n)
{
  char line[MW_LINE_SZ];
  uint32_t i;

  snprintf(line, sizeof(line),
      "  %-14s %-12s %8s %16s",
      "product_id", "price", "pct_24h", "vol_24h_q");
  mw_send(inst, target, line);

  for(i = 0; i < n; i++)
  {
    char price_s[32];
    char pct_s[16];
    char vol_s[32];

    mw_fmt_dbl(price_s, sizeof(price_s), "%.4g", rows[i].price);
    mw_fmt_dbl(pct_s,   sizeof(pct_s),   "%.2f", rows[i].pct_24h);
    mw_fmt_dbl(vol_s,   sizeof(vol_s),   "%.2f", rows[i].vol_24h_quote);

    snprintf(line, sizeof(line),
        "  %-14s %-12s %8s %16s",
        rows[i].product_id, price_s, pct_s, vol_s);
    mw_send(inst, target, line);
  }
}

bool
mw_render_status_exch(method_inst_t *inst, const char *target,
    const char *exch)
{
  mw_exch_t *ex;
  char       line[MW_LINE_SZ];
  char       age[32];
  uint32_t   pair_count;
  uint64_t   polls;
  uint64_t   drops;
  int64_t    last_poll_ms;
  int64_t    now_ms;
  uint32_t   ring_n;
  bool       enabled;
  uint32_t   poll_sec;
  mw_topn_row_t *rows_pct;
  mw_topn_row_t *rows_vol;
  uint32_t       n_pct;
  uint32_t       n_vol;
  bool           any_vol;

  pthread_mutex_lock(&mw_g.mtx);

  ex = mw_find_exch_by_name(exch);

  if(ex == NULL)
  {
    pthread_mutex_unlock(&mw_g.mtx);
    return(FAIL);
  }

  ring_n   = mw_g.ring_n;
  enabled  = ex->enabled;
  poll_sec = ex->poll_sec;
  now_ms   = wm_dl_now_ms();

  // Allocate render buffers outside the inner lock so the qsort runs
  // hold ex->lock briefly (snapshot copy only).
  rows_pct = mem_alloc(WHENMOON_CTX, "mw.topn.pct",
      (size_t)MW_TOPN * sizeof(*rows_pct));
  rows_vol = mem_alloc(WHENMOON_CTX, "mw.topn.vol",
      (size_t)MW_TOPN * sizeof(*rows_vol));

  if(rows_pct == NULL || rows_vol == NULL)
  {
    pthread_mutex_unlock(&mw_g.mtx);
    if(rows_pct != NULL) mem_free(rows_pct);
    if(rows_vol != NULL) mem_free(rows_vol);
    return(FAIL);
  }

  pthread_mutex_lock(&ex->lock);

  pair_count   = ex->pair_count;
  polls        = ex->total_polls;
  drops        = ex->total_drops_full;
  last_poll_ms = ex->last_poll_ms;

  n_pct = mw_build_topn(rows_pct, ex, ring_n, false);
  n_vol = mw_build_topn(rows_vol, ex, ring_n, true);

  // Decide whether the vol section is worth printing — Gemini reports
  // NaN vol_24h_quote, so n_vol ends up zero after NaN filtering.
  any_vol = false;
  {
    uint32_t i;

    for(i = 0; i < n_vol; i++)
    {
      if(!isnan(rows_vol[i].vol_24h_quote)
          && rows_vol[i].vol_24h_quote > 0.0)
      {
        any_vol = true;
        break;
      }
    }
  }

  pthread_mutex_unlock(&ex->lock);
  pthread_mutex_unlock(&mw_g.mtx);

  mw_fmt_age(age, sizeof(age), now_ms, last_poll_ms);

  snprintf(line, sizeof(line),
      "marketwatch exchange=%s enabled=%s poll_sec=%u"
      " last_poll=%s pairs=%u polls=%" PRIu64 " drops=%" PRIu64,
      exch, enabled ? "yes" : "no", poll_sec, age, pair_count,
      polls, drops);
  mw_send(inst, target, line);

  if(pair_count == 0)
  {
    mw_send(inst, target, "(no pairs collected)");
    mem_free(rows_pct);
    mem_free(rows_vol);
    return(SUCCESS);
  }

  if(n_pct > 0)
  {
    mw_send(inst, target, "top-10 by |pct_24h|:");
    mw_render_topn(inst, target, rows_pct, n_pct);
  }

  else
    mw_send(inst, target, "top-10 by |pct_24h|: (no data)");

  if(any_vol)
  {
    mw_send(inst, target, "top-10 by vol_24h_quote:");
    mw_render_topn(inst, target, rows_vol, n_vol);
  }

  mem_free(rows_pct);
  mem_free(rows_vol);
  return(SUCCESS);
}
