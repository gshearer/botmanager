// botmanager — MIT
// whenmoon real-mode trade execution (per-market path).
//
// One path post-WM-MK-5: wm_market_engine_on_signal calls
// wm_market_engine_real_submit_locked under mk->lock when
// session.mode == WM_MARKET_MODE_REAL. That helper runs the master
// kill-switch + cred + daily-loss + pending-cap + max-notional-clip
// cascade, registers a wm_market_pending_t in mk->session.pending[],
// and dispatches exchange_place_order_async on mk->exchange_name.
// The done callback (wm_live_market_order_done) reaps the pending row
// on gateway reject or records gateway_accepted=true on accept. Fills
// land asynchronously through the WS user-channel + REST /fills
// consumers (wm_live_handle_ws_fill / wm_live_on_fills) which route
// into wm_market_engine_record_external_fill.
//
// Master kill-switch: plugin.whenmoon.exchange.<exch>.live (KV_BOOL,
// default false), one per registered exchange. Operator must opt in
// explicitly per exchange before any real order leaves the daemon.
//
// KR-2 single-exchange WS state: g_live owns one user-channel sub at
// a time, bound to whichever exchange the market set shares. KR-5
// will partition this when a second exchange enters the running set.
//
// Locking:
//   - g_live.mu protects g_live.last_fills_cursor_ms.
//   - mk->lock protects mk->session.pending[].
//   - The two locks are independent; no path takes both.

#define WHENMOON_INTERNAL
#include "live.h"

#include "market.h"
#include "market_engine.h"
#include "whenmoon.h"
#include "whenmoon_strategy.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "kv.h"
#include "task.h"

#include "exchange_api.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

#define WM_LIVE_CTX  "whenmoon.live"

// Compile-time ceiling on the per-exchange list the boot reconciler
// walks. Mirrors the account refresher's cap.
#define WM_LIVE_MAX_EXCHANGES   8

// ----------------------------------------------------------------------- //
// Engine global state                                                     //
// ----------------------------------------------------------------------- //

static struct
{
  pthread_mutex_t      mu;

  bool                 initialized;

  // WS user-channel subscription. Bound to whichever exchange the
  // running market set shares (one at a time today; KR-5 partitions).
  exchange_ws_sub_t   *ws_sub;
  char                 ws_exchange[EXCHANGE_NAME_SZ];
  whenmoon_state_t    *ws_st;

  // REST /fills safety-net poll periodic.
  task_handle_t        fills_poll_task;

  // Single global cursor for the REST /fills poll. Exchanges page
  // /fills globally on sequence_timestamp, not per-product, so a
  // per-market cursor wouldn't help. Initialized to "now - overlap"
  // on engine start; advanced on every applied fill via the
  // consumers. Guarded by g_live.mu.
  int64_t              last_fills_cursor_ms;
} g_live;

// Forward declarations — definitions further down.
static void wm_live_ws_user_event_cb(const exchange_ws_event_t *ev,
    void *user);
static void wm_live_fills_poll_tick(task_t *t);

#define WM_LIVE_FILLS_POLL_SEC         30
#define WM_LIVE_FILLS_POLL_OVERLAP_MS  (60 * 1000)

// ----------------------------------------------------------------------- //
// Lifecycle                                                               //
// ----------------------------------------------------------------------- //

bool
wm_live_engine_init(void)
{
  if(g_live.initialized) return(SUCCESS);

  memset(&g_live, 0, sizeof(g_live));
  pthread_mutex_init(&g_live.mu, NULL);
  g_live.initialized = true;

  clam(CLAM_DEBUG, WM_LIVE_CTX, "live engine initialized");
  return(SUCCESS);
}

void
wm_live_engine_destroy(void)
{
  if(!g_live.initialized) return;

  // Cancel periodic first so no fresh tick fires on freed state.
  task_cancel(g_live.fills_poll_task);
  g_live.fills_poll_task = TASK_HANDLE_NONE;

  // Drop WS subscription before destroying the lock — the WS reader
  // can fire callbacks on a worker thread; better to let those see
  // g_live.initialized = false (still true here, but the ws_sub
  // pointer null-out below means no new events).
  if(g_live.ws_sub != NULL)
  {
    exchange_ws_unsubscribe(g_live.ws_exchange, g_live.ws_sub);
    g_live.ws_sub = NULL;
  }
  g_live.ws_exchange[0] = '\0';
  g_live.ws_st = NULL;

  pthread_mutex_destroy(&g_live.mu);
  g_live.initialized = false;

  clam(CLAM_DEBUG, WM_LIVE_CTX, "live engine torn down");
}

// ----------------------------------------------------------------------- //
// UUID + place-order builder                                              //
// ----------------------------------------------------------------------- //

// Mint a v4 UUID into out (caller buffer >= 37 bytes).
static bool
wm_live_uuid_v4(char *out, size_t cap)
{
  unsigned char buf[16];
  ssize_t       n;

  if(out == NULL || cap < 37) return(FAIL);

  n = getrandom(buf, sizeof(buf), 0);
  if(n != (ssize_t)sizeof(buf)) return(FAIL);

  buf[6] = (unsigned char)((buf[6] & 0x0f) | 0x40);
  buf[8] = (unsigned char)((buf[8] & 0x3f) | 0x80);

  snprintf(out, cap,
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
      "%02x%02x%02x%02x%02x%02x",
      buf[0], buf[1], buf[2],  buf[3],  buf[4],  buf[5],
      buf[6], buf[7], buf[8],  buf[9],  buf[10], buf[11],
      buf[12], buf[13], buf[14], buf[15]);

  return(SUCCESS);
}

// Build an exchange_place_order_req_t for a GTC limit order. Pure
// plumbing — no state reads, no allocation. `product_id` is the wire-
// form ("BTC-USD"), `side_str` is "buy" or "sell", `coid` is a
// pre-minted v4 UUID. Caller zero-inits `out` before calling; this
// helper overwrites the whole struct.
static void
wm_live_build_place_order_req(exchange_place_order_req_t *out,
    const char *product_id, const char *side_str, double qty,
    double limit_px, const char *coid, bool post_only)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));

  if(product_id != NULL)
    snprintf(out->product_id, sizeof(out->product_id), "%s", product_id);

  if(side_str != NULL)
    snprintf(out->side, sizeof(out->side), "%s", side_str);

  snprintf(out->type, sizeof(out->type), "limit");
  snprintf(out->tif,  sizeof(out->tif),  "GTC");

  if(coid != NULL)
    snprintf(out->client_oid, sizeof(out->client_oid), "%s", coid);

  out->price     = limit_px;
  out->size      = qty;
  out->post_only = post_only;
}

// Master kill-switch for the per-market real submit path. Reads the
// per-exchange KV at plugin.whenmoon.exchange.<exch>.live (KV_BOOL,
// default false). Lazy-registers on first read so the operator can
// flip it via `/set kv` even before any other code has touched it.
// FAILs closed when the KV is missing or false — explicit opt-in is
// required per exchange.
static bool
wm_live_master_live_enabled(const char *exchange_name)
{
  char path[160];
  int  n;

  if(exchange_name == NULL || exchange_name[0] == '\0')
    return(FAIL);

  n = snprintf(path, sizeof(path),
      "plugin.whenmoon.exchange.%s.live", exchange_name);

  if(n < 0 || (size_t)n >= sizeof(path))
    return(FAIL);

  if(!kv_exists(path))
    (void)kv_register(path, KV_BOOL, "false", NULL, NULL,
        "Master kill-switch for the per-market real-mode submit"
        " path on this exchange. Default false; flip to true to"
        " enable live order placement. Per-market risk caps still"
        " apply when this is true.");

  return(kv_get_uint(path) != 0);
}

// ----------------------------------------------------------------------- //
// Per-market real-mode submit                                             //
// ----------------------------------------------------------------------- //

// Heap-owned ctx for the per-market done callback. Allocated under
// `mk->lock` in wm_market_engine_real_submit_locked, freed by the
// done_cb (curl worker thread).
typedef struct wm_live_market_done_ctx
{
  char  market_id_str[WM_MARKET_ID_STR_SZ];
  char  coid[EXCHANGE_CLIENT_OID_SZ];
} wm_live_market_done_ctx_t;

// Done callback for the per-market submit path. Runs on the curl
// worker thread; takes `mk->lock` independently of any caller. On a
// gateway error, reaps the pending row + frees ctx. On success,
// records `order_id` + sets `gateway_accepted = true` so the WS
// user-channel order updates land on a populated row.
//
// FAIL semantics from exchange_place_order_async: on synchronous-FAIL
// the caller pre-fills `res->err` and the done_cb is invoked
// synchronously; the caller must not touch the pending row or ctx
// after the call returns. On asynchronous failures the same callback
// fires off-thread.
static void
wm_live_market_order_done(const exchange_order_result_t *res, void *user)
{
  wm_live_market_done_ctx_t *ctx;
  whenmoon_state_t          *st;
  whenmoon_market_t         *mk;
  bool                       err;
  uint32_t                   i;

  ctx = (wm_live_market_done_ctx_t *)user;

  if(ctx == NULL) return;

  err = (res != NULL && res->err[0] != '\0');

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
  {
    mem_free(ctx);
    return;
  }

  mk = wm_market_lookup_by_id(st, ctx->market_id_str);

  if(mk == NULL)
  {
    // Market torn down between submit and ack. The pending row went
    // with the market state; nothing to reap.
    mem_free(ctx);
    return;
  }

  pthread_mutex_lock(&mk->lock);

  for(i = 0; i < mk->session.pending_n; i++)
  {
    wm_market_pending_t *p = &mk->session.pending[i];

    if(strncmp(p->coid, ctx->coid, sizeof(p->coid)) != 0)
      continue;

    if(err)
    {
      uint32_t shift;

      clam(CLAM_WARN, WM_LIVE_CTX,
          "%s order rejected coid=%s err=%s",
          mk->market_id_str, ctx->coid, res->err);

      for(shift = i + 1; shift < mk->session.pending_n; shift++)
        mk->session.pending[shift - 1] = mk->session.pending[shift];

      mk->session.pending_n--;
    }

    else
    {
      if(res->order.order_id[0] != '\0')
        snprintf(p->order_id, sizeof(p->order_id),
            "%s", res->order.order_id);

      p->gateway_accepted = true;

      clam(CLAM_DEBUG, WM_LIVE_CTX,
          "%s order accepted coid=%s order_id=%s",
          mk->market_id_str, ctx->coid, res->order.order_id);
    }

    break;
  }

  pthread_mutex_unlock(&mk->lock);
  mem_free(ctx);
}

bool
wm_market_engine_real_submit_locked(whenmoon_market_t *mk,
    char side, double qty, double mark_px, int64_t mark_ms,
    const wm_strategy_signal_t *sig,
    char *errbuf, size_t errbuf_sz)
{
  exchange_place_order_req_t  req;
  exchange_capabilities_t     caps;
  wm_live_market_done_ctx_t  *ctx;
  wm_market_pending_t        *pending;
  double                      starting_cash;
  double                      daily_cap;
  double                      clipped_qty;
  const char                 *side_str;
  bool                        is_buy;

  (void)sig;   // reserved for future audit hooks

  #define ERRSET(...) do { \
      if(errbuf != NULL && errbuf_sz > 0) \
        snprintf(errbuf, errbuf_sz, __VA_ARGS__); \
    } while(0)

  if(errbuf != NULL && errbuf_sz > 0)
    errbuf[0] = '\0';

  if(mk == NULL || qty <= 0.0 || mark_px <= 0.0)
  {
    ERRSET("invalid args");
    return(FAIL);
  }

  if(mk->exchange_name[0] == '\0')
  {
    ERRSET("market %s has no bound exchange", mk->market_id_str);
    return(FAIL);
  }

  if(side != 'b' && side != 's')
  {
    ERRSET("side must be 'b' or 's'");
    return(FAIL);
  }

  is_buy   = (side == 'b');
  side_str = is_buy ? "buy" : "sell";

  // Gate 1: master kill-switch for this exchange.
  if(!wm_live_master_live_enabled(mk->exchange_name))
  {
    ERRSET("live trading disabled"
        " (plugin.whenmoon.exchange.%s.live=false)",
        mk->exchange_name);
    clam(CLAM_WARN, WM_LIVE_CTX,
        "%s real submit refused: live=false (%s)",
        mk->market_id_str, mk->exchange_name);
    return(FAIL);
  }

  // Gate 2: credentials — consult the exchange capability surface.
  if(exchange_get_capabilities(mk->exchange_name, &caps) != SUCCESS
      || !caps.has_credentials)
  {
    ERRSET("no exchange credentials (%s)", mk->exchange_name);
    clam(CLAM_WARN, WM_LIVE_CTX,
        "%s real submit refused: %s credentials not configured",
        mk->market_id_str, mk->exchange_name);
    return(FAIL);
  }

  // Gate 3: daily-loss cap. wm_market_apply_fill_locked maintains the
  // daily anchor — we read state here, not compute. Cap of 0 disables
  // the gate.
  starting_cash = mk->session.stats[WM_MARKET_MODE_REAL].starting_cash;
  daily_cap     = starting_cash * (mk->session.daily_loss_bps / 10000.0);

  if(daily_cap > 0.0
      && mk->session.stats[WM_MARKET_MODE_REAL].realized_pnl_today
         <= -daily_cap)
  {
    ERRSET("daily loss cap tripped (%.4f <= -%.4f)",
        mk->session.stats[WM_MARKET_MODE_REAL].realized_pnl_today,
        daily_cap);
    clam(CLAM_WARN, WM_LIVE_CTX,
        "%s real submit FAIL: daily loss %.4f <= cap %.4f",
        mk->market_id_str,
        mk->session.stats[WM_MARKET_MODE_REAL].realized_pnl_today,
        -daily_cap);
    return(FAIL);
  }

  // Gate 4: pending-cap.
  if(mk->session.pending_n >= mk->session.pending_cap)
  {
    ERRSET("pending ring full (n=%u cap=%u)",
        mk->session.pending_n, mk->session.pending_cap);
    clam(CLAM_WARN, WM_LIVE_CTX,
        "%s real submit FAIL: pending ring full (%u/%u)",
        mk->market_id_str, mk->session.pending_n,
        mk->session.pending_cap);
    return(FAIL);
  }

  // Gate 5: max-notional. Clip qty rather than reject — degraded
  // sizing matches paper-mode behaviour.
  clipped_qty = qty;

  if(mk->session.max_notional > 0.0
      && clipped_qty * mark_px > mk->session.max_notional)
  {
    double next = mk->session.max_notional / mark_px;

    clam(CLAM_INFO, WM_LIVE_CTX,
        "%s real submit notional clip qty %.6g -> %.6g"
        " (cap=%.4f mark=%.4f)",
        mk->market_id_str, clipped_qty, next,
        mk->session.max_notional, mark_px);

    clipped_qty = next;
  }

  // Build the place-order request. mk->product_id already carries the
  // wire-form symbol ("BTC-USD"); no exchange-specific casing needed.
  if(mk->product_id[0] == '\0')
  {
    ERRSET("market %s has no wire-form product id", mk->market_id_str);
    return(FAIL);
  }

  // Mint COID into a local buffer first; only commit to the pending
  // row after every fail-able op succeeds.
  {
    char coid[EXCHANGE_CLIENT_OID_SZ];

    if(wm_live_uuid_v4(coid, sizeof(coid)) != SUCCESS)
    {
      ERRSET("client_oid mint failed");
      return(FAIL);
    }

    wm_live_build_place_order_req(&req, mk->product_id, side_str,
        clipped_qty, mark_px, coid, false);

    // Append the pending row BEFORE the async call so a synchronous-
    // FAIL done_cb finds it. The done_cb owns the post-fail reap.
    pending = &mk->session.pending[mk->session.pending_n++];
    memset(pending, 0, sizeof(*pending));

    snprintf(pending->coid, sizeof(pending->coid), "%s", coid);
    snprintf(pending->side, sizeof(pending->side), "%s", side_str);
    pending->limit_px      = mark_px;
    pending->submitted_qty = clipped_qty;
    pending->submitted_ms  = mark_ms;

    ctx = mem_alloc(WM_LIVE_CTX, "live_market_done_ctx", sizeof(*ctx));

    if(ctx == NULL)
    {
      // Roll back the pending row.
      mk->session.pending_n--;
      ERRSET("oom");
      return(FAIL);
    }

    snprintf(ctx->market_id_str, sizeof(ctx->market_id_str), "%s",
        mk->market_id_str);
    snprintf(ctx->coid, sizeof(ctx->coid), "%s", coid);
  }

  if(exchange_place_order_async(mk->exchange_name, &req,
        wm_live_market_order_done, ctx) != SUCCESS)
  {
    // exchange_place_order_async fires the done_cb synchronously with
    // res->err set when it returns FAIL. The done_cb has already
    // reaped the pending row + freed ctx by the time we get here.
    // Do NOT touch state.
    ERRSET("exchange_place_order_async returned FAIL"
        " (done_cb already fired)");
    return(FAIL);
  }

  clam(CLAM_INFO, WM_LIVE_CTX,
      "%s submit %s qty=%.6g px=%.4f coid=%s",
      mk->market_id_str, side_str, clipped_qty, mark_px, req.client_oid);

  return(SUCCESS);

  #undef ERRSET
}

// ----------------------------------------------------------------------- //
// Per-market pending lookup                                               //
// ----------------------------------------------------------------------- //

// Walk every running market looking for a pending row whose coid
// matches. On hit, returns true with `*out_mk` pointing at the owning
// market and `*out_idx` set to the pending row's index — `mk->lock`
// IS HELD by this helper on success; the caller is responsible for
// unlocking after dedup + reap work. On miss, returns false with no
// lock held.
//
// Locking discipline: this helper does NOT hold `g_live.mu` during
// the per-market scan. Each market's lock is taken in turn. The
// `st->markets->arr` snapshot pointer is stable for the duration of a
// single dispatch (per `wm_market_lookup_by_id` contract).
static bool
wm_live_find_pending_by_coid_market_locked(const char *coid,
    whenmoon_market_t **out_mk, uint32_t *out_idx)
{
  whenmoon_state_t   *st;
  whenmoon_markets_t *mkts;
  uint32_t            i;
  uint32_t            j;

  if(out_mk != NULL)  *out_mk  = NULL;
  if(out_idx != NULL) *out_idx = 0;

  if(coid == NULL || coid[0] == '\0')
    return(false);

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
    return(false);

  mkts = st->markets;

  for(i = 0; i < mkts->n_markets; i++)
  {
    whenmoon_market_t *mk = &mkts->arr[i];

    pthread_mutex_lock(&mk->lock);

    for(j = 0; j < mk->session.pending_n; j++)
    {
      if(strncmp(mk->session.pending[j].coid, coid,
             sizeof(mk->session.pending[j].coid)) == 0)
      {
        if(out_mk  != NULL) *out_mk  = mk;
        if(out_idx != NULL) *out_idx = j;
        // Lock stays held — caller releases after dedup + reap.
        return(true);
      }
    }

    pthread_mutex_unlock(&mk->lock);
  }

  return(false);
}

// ----------------------------------------------------------------------- //
// User-channel event handlers                                             //
// ----------------------------------------------------------------------- //

static void
wm_live_handle_ws_fill(const exchange_ws_user_fill_t *f)
{
  whenmoon_market_t   *mk = NULL;
  uint32_t             pidx = 0;
  wm_market_pending_t *p;
  uint8_t              k;
  bool                 reap = false;
  char                 market_id_copy[WM_MARKET_ID_STR_SZ];
  char                 side_ch;

  if(f == NULL || f->trade_id == 0 || f->size <= 0.0 || f->price <= 0.0)
    return;

  if(!wm_live_find_pending_by_coid_market_locked(f->client_order_id,
         &mk, &pidx))
  {
    // Orphan: WS may have raced ahead of submit, or this fill belongs
    // to an unrelated principal (the user channel emits per-account,
    // not per-product). REST /fills poll handles late-bound dedup.
    clam(CLAM_DEBUG, WM_LIVE_CTX,
        "ws fill orphan coid=%s order_id=%s tid=%lld"
        " (REST poll safety-net handles dedup)",
        f->client_order_id, f->order_id, (long long)f->trade_id);
    return;
  }

  // mk->lock held here.
  p = &mk->session.pending[pidx];

  for(k = 0; k < p->n_recorded_trades; k++)
  {
    if(p->recorded_trade_ids[k] == f->trade_id)
    {
      pthread_mutex_unlock(&mk->lock);
      clam(CLAM_DEBUG2, WM_LIVE_CTX,
          "ws fill: dup trade_id=%lld coid=%s",
          (long long)f->trade_id, f->client_order_id);
      return;
    }
  }

  if(p->n_recorded_trades < WM_MARKET_TRADE_DEDUP)
    p->recorded_trade_ids[p->n_recorded_trades++] = f->trade_id;

  p->filled_qty += f->size;
  reap = (p->filled_qty >= p->submitted_qty - 1e-12);

  snprintf(market_id_copy, sizeof(market_id_copy), "%s",
      mk->market_id_str);

  if(reap)
  {
    // Shift-down preserving order.
    uint32_t shift;

    for(shift = pidx + 1; shift < mk->session.pending_n; shift++)
      mk->session.pending[shift - 1] = mk->session.pending[shift];

    mk->session.pending_n--;
  }

  pthread_mutex_unlock(&mk->lock);

  // Advance the global REST cursor under g_live.mu so a concurrent
  // poll-tick reads a consistent view.
  pthread_mutex_lock(&g_live.mu);

  if(f->time_ms > g_live.last_fills_cursor_ms)
    g_live.last_fills_cursor_ms = f->time_ms;

  pthread_mutex_unlock(&g_live.mu);

  side_ch = (f->side[0] == 'b' || f->side[0] == 'B') ? 'b' : 's';

  // Engine takes mk->lock again internally. We released above, so no
  // double-lock.
  wm_market_engine_record_external_fill(market_id_copy, f->trade_id,
      side_ch, f->size, f->price, f->fee, f->time_ms,
      "ws-fill");
}

static void
wm_live_handle_ws_order(const exchange_ws_user_order_t *o)
{
  whenmoon_market_t   *mk = NULL;
  uint32_t             pidx = 0;
  wm_market_pending_t *p;
  bool                 reap   = false;
  bool                 failed = false;
  const char          *status;
  char                 market_id_copy[WM_MARKET_ID_STR_SZ];

  if(o == NULL) return;

  status = o->status;

  // Map AT terminal states to reap. OPEN/PENDING are non-terminal.
  if(strcmp(status, "FILLED") == 0
      || strcmp(status, "CANCELLED") == 0
      || strcmp(status, "EXPIRED")  == 0)
    reap = true;
  else if(strcmp(status, "FAILED") == 0)
    reap = failed = true;

  if(!wm_live_find_pending_by_coid_market_locked(o->client_order_id,
         &mk, &pidx))
    return;

  // mk->lock held here.
  p = &mk->session.pending[pidx];

  if(o->order_id[0] != '\0')
    snprintf(p->order_id, sizeof(p->order_id), "%s", o->order_id);

  if(strcmp(status, "OPEN") == 0)
    p->gateway_accepted = true;

  if(reap)
  {
    uint32_t shift;

    snprintf(market_id_copy, sizeof(market_id_copy), "%s",
        mk->market_id_str);

    for(shift = pidx + 1; shift < mk->session.pending_n; shift++)
      mk->session.pending[shift - 1] = mk->session.pending[shift];

    mk->session.pending_n--;

    pthread_mutex_unlock(&mk->lock);

    clam(failed ? CLAM_WARN : CLAM_INFO, WM_LIVE_CTX,
        "ws order %s coid=%s order_id=%s status=%s -> reaped",
        market_id_copy, o->client_order_id, o->order_id, status);
    return;
  }

  pthread_mutex_unlock(&mk->lock);
}

static void
wm_live_ws_user_event_cb(const exchange_ws_event_t *ev, void *user)
{
  (void)user;

  if(ev == NULL || ev->channel != EXCH_WS_USER)
    return;

  switch(ev->payload.user.kind)
  {
    case EXCH_WS_USER_KIND_ORDER:
      wm_live_handle_ws_order(&ev->payload.user.u.order);
      break;
    case EXCH_WS_USER_KIND_FILL:
      wm_live_handle_ws_fill(&ev->payload.user.u.fill);
      break;
  }
}

// ----------------------------------------------------------------------- //
// WS resub (called from market.c)                                         //
// ----------------------------------------------------------------------- //

void
wm_live_ws_resub(whenmoon_state_t *st,
    const char *exchange_name,
    const char *const *product_ids, size_t n_products)
{
  static const exchange_ws_channel_t channels[] = { EXCH_WS_USER };
  exchange_capabilities_t            caps;

  if(!g_live.initialized) return;

  if(g_live.ws_sub != NULL)
  {
    exchange_ws_unsubscribe(g_live.ws_exchange, g_live.ws_sub);
    g_live.ws_sub          = NULL;
    g_live.ws_exchange[0]  = '\0';
  }

  g_live.ws_st = st;

  if(n_products == 0 || product_ids == NULL
      || exchange_name == NULL || exchange_name[0] == '\0')
    return;

  // Creds are required for the user channel; the exchange-side
  // subscribe FAILs closed when they are absent. Skip silently — once
  // creds appear the next market mutation will retry.
  if(exchange_get_capabilities(exchange_name, &caps) != SUCCESS
      || !caps.has_credentials)
  {
    clam(CLAM_DEBUG, WM_LIVE_CTX,
        "user-channel sub deferred: no creds for %s yet",
        exchange_name);
    return;
  }

  if(exchange_ws_subscribe(exchange_name, channels,
        sizeof(channels) / sizeof(channels[0]),
        product_ids, (uint32_t)n_products,
        wm_live_ws_user_event_cb, NULL,
        &g_live.ws_sub) != SUCCESS || g_live.ws_sub == NULL)
  {
    clam(CLAM_WARN, WM_LIVE_CTX,
        "user-channel WS subscribe failed (exchange=%s n_products=%zu)",
        exchange_name, n_products);
    g_live.ws_sub = NULL;
  }
  else
  {
    snprintf(g_live.ws_exchange, sizeof(g_live.ws_exchange), "%s",
        exchange_name);
    clam(CLAM_INFO, WM_LIVE_CTX,
        "user-channel WS subscribed (exchange=%s n_products=%zu)",
        exchange_name, n_products);
  }
}

// ----------------------------------------------------------------------- //
// REST /fills safety-net poll                                             //
// ----------------------------------------------------------------------- //

typedef struct wm_live_fills_ctx
{
  char  market_id_str[WM_MARKET_ID_STR_SZ];
  char  exchange_name[EXCHANGE_NAME_SZ];
  char  product_id[WM_PRODUCT_ID_SZ];
} wm_live_fills_ctx_t;

// True iff any running market has already recorded this trade_id on
// any of its pending rows. Walks `st->markets->arr[i]` taking each
// `mk->lock` in turn — same locking shape as
// wm_live_find_pending_by_coid_market_locked but read-only and with no
// lock held on return.
static bool
wm_live_trade_id_seen_any_market(int64_t trade_id)
{
  whenmoon_state_t   *st;
  whenmoon_markets_t *mkts;
  uint32_t            i;
  uint32_t            j;
  uint8_t             k;

  if(trade_id == 0)
    return(false);

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
    return(false);

  mkts = st->markets;

  for(i = 0; i < mkts->n_markets; i++)
  {
    whenmoon_market_t *mk = &mkts->arr[i];
    bool               hit = false;

    pthread_mutex_lock(&mk->lock);

    for(j = 0; j < mk->session.pending_n && !hit; j++)
    {
      const wm_market_pending_t *p = &mk->session.pending[j];

      for(k = 0; k < p->n_recorded_trades; k++)
      {
        if(p->recorded_trade_ids[k] == trade_id)
        {
          hit = true;
          break;
        }
      }
    }

    pthread_mutex_unlock(&mk->lock);

    if(hit)
      return(true);
  }

  return(false);
}

static void
wm_live_on_fills(const exchange_fills_result_t *res, void *user)
{
  wm_live_fills_ctx_t *ctx = user;

  if(ctx == NULL || res == NULL) goto done;

  if(res->err[0] != '\0')
  {
    clam(CLAM_DEBUG, WM_LIVE_CTX,
        "fills poll %s err=%s", ctx->product_id, res->err);
    goto done;
  }

  for(uint32_t i = 0; i < res->count; i++)
  {
    const exchange_fill_t *f = &res->rows[i];
    whenmoon_market_t     *mk = NULL;
    uint32_t               pidx = 0;
    wm_market_pending_t   *p;
    uint8_t                k;
    bool                   reap = false;
    bool                   dedup_hit = false;
    char                   market_id_copy[WM_MARKET_ID_STR_SZ];
    char                   side_ch;

    if(f->trade_id == 0 || f->size <= 0.0 || f->price <= 0.0)
      continue;

    if(!wm_live_find_pending_by_coid_market_locked(f->client_oid,
           &mk, &pidx))
    {
      // Orphan: WS likely already reaped this pending row. Confirm
      // dedup against every running market's recorded_trade_ids[]
      // before logging — that distinguishes "WS got there first" from
      // "fill landed for an unknown order".
      if(!wm_live_trade_id_seen_any_market(f->trade_id))
        clam(CLAM_DEBUG, WM_LIVE_CTX,
            "fills poll: orphan tid=%lld coid=%s product=%s",
            (long long)f->trade_id, f->client_oid, f->product_id);
      continue;
    }

    // mk->lock held here.
    p = &mk->session.pending[pidx];

    for(k = 0; k < p->n_recorded_trades; k++)
    {
      if(p->recorded_trade_ids[k] == f->trade_id)
      {
        dedup_hit = true;
        break;
      }
    }

    if(dedup_hit)
    {
      pthread_mutex_unlock(&mk->lock);
      continue;
    }

    if(p->n_recorded_trades < WM_MARKET_TRADE_DEDUP)
      p->recorded_trade_ids[p->n_recorded_trades++] = f->trade_id;

    p->filled_qty += f->size;
    reap = (p->filled_qty >= p->submitted_qty - 1e-12);

    snprintf(market_id_copy, sizeof(market_id_copy), "%s",
        mk->market_id_str);

    if(reap)
    {
      uint32_t shift;

      for(shift = pidx + 1; shift < mk->session.pending_n; shift++)
        mk->session.pending[shift - 1] = mk->session.pending[shift];

      mk->session.pending_n--;
    }

    pthread_mutex_unlock(&mk->lock);

    // Advance the global REST cursor.
    pthread_mutex_lock(&g_live.mu);

    if(f->time_ms > g_live.last_fills_cursor_ms)
      g_live.last_fills_cursor_ms = f->time_ms;

    pthread_mutex_unlock(&g_live.mu);

    side_ch = (f->side[0] == 'b' || f->side[0] == 'B') ? 'b' : 's';

    clam(CLAM_INFO, WM_LIVE_CTX,
        "fills poll: applied tid=%lld coid=%s",
        (long long)f->trade_id, f->client_oid);

    wm_market_engine_record_external_fill(market_id_copy, f->trade_id,
        side_ch, f->size, f->price, f->fee, f->time_ms,
        "rest-fill");
  }

done:
  if(ctx != NULL) mem_free(ctx);
}

static void
wm_live_fills_poll_tick(task_t *t)
{
  whenmoon_state_t   *st = t->data;
  whenmoon_markets_t *mkts;
  int64_t             cursor_ms;

  t->state = TASK_ENDED;

  if(st == NULL || st->markets == NULL) return;
  if(!g_live.initialized) return;

  // Single global cursor: exchanges page /fills globally on
  // sequence_timestamp, not per-product. Subtract the overlap window so
  // we don't miss a fill whose envelope timestamp jitters slightly
  // backward between polls.
  pthread_mutex_lock(&g_live.mu);
  cursor_ms = g_live.last_fills_cursor_ms;
  pthread_mutex_unlock(&g_live.mu);

  if(cursor_ms > WM_LIVE_FILLS_POLL_OVERLAP_MS)
    cursor_ms -= WM_LIVE_FILLS_POLL_OVERLAP_MS;
  else
    cursor_ms = 0;   // first poll: unbounded (server caps at limit=100)

  mkts = st->markets;

  for(uint32_t i = 0; i < mkts->n_markets; i++)
  {
    wm_live_fills_ctx_t    *ctx;
    exchange_capabilities_t caps;

    if(mkts->arr[i].exchange_name[0] == '\0')
      continue;

    // Skip when creds are not configured for this exchange.
    if(exchange_get_capabilities(mkts->arr[i].exchange_name,
           &caps) != SUCCESS || !caps.has_credentials)
      continue;

    ctx = mem_alloc(WM_LIVE_CTX, "fills_ctx", sizeof(*ctx));
    if(ctx == NULL) continue;

    snprintf(ctx->market_id_str, sizeof(ctx->market_id_str), "%s",
        mkts->arr[i].market_id_str);
    snprintf(ctx->exchange_name, sizeof(ctx->exchange_name), "%s",
        mkts->arr[i].exchange_name);
    snprintf(ctx->product_id, sizeof(ctx->product_id), "%s",
        mkts->arr[i].product_id);

    if(exchange_list_fills_async(ctx->exchange_name, NULL,
           ctx->product_id, cursor_ms,
           wm_live_on_fills, ctx) != SUCCESS)
    {
      // wm_live_on_fills already invoked synchronously with res->err
      // set on FAIL; ctx already freed.
    }
  }
}

// ----------------------------------------------------------------------- //
// Boot reconcile                                                          //
// ----------------------------------------------------------------------- //

static void
wm_live_boot_reconcile_cb(const exchange_orders_result_t *res, void *user)
{
  const char *exchange_name = user;

  if(res == NULL || exchange_name == NULL) return;

  if(res->err[0] != '\0')
  {
    clam(CLAM_DEBUG, WM_LIVE_CTX,
        "boot reconcile (%s list orders) err=%s",
        exchange_name, res->err);
    return;
  }

  if(res->count == 0)
  {
    clam(CLAM_INFO, WM_LIVE_CTX,
        "boot reconcile (%s): no resting orders at gateway",
        exchange_name);
    return;
  }

  clam(CLAM_WARN, WM_LIVE_CTX,
      "boot reconcile (%s): %u open order(s) found at gateway:",
      exchange_name, res->count);

  for(uint32_t i = 0; i < res->count; i++)
  {
    const exchange_order_t *o = &res->rows[i];

    clam(CLAM_WARN, WM_LIVE_CTX,
        "  order_id=%s coid=%s product=%s side=%s status=%s"
        " price=%.4f size=%.6g filled=%.6g",
        o->order_id, o->client_oid, o->product_id, o->side, o->status,
        o->price, o->size, o->filled_size);
  }

  clam(CLAM_WARN, WM_LIVE_CTX,
      "boot reconcile (%s): not auto-attaching to local pending state."
      " Inspect via exchange UI; cancel manually if undesired.",
      exchange_name);
}

// Static storage for the per-exchange reconcile user-pointer. The
// callback receives the const char * by-ref so we don't need to heap
// the name; the slots live for the lifetime of the daemon.
static char wm_live_reconcile_names[WM_LIVE_MAX_EXCHANGES][EXCHANGE_NAME_SZ];

// ----------------------------------------------------------------------- //
// Late-stage start                                                        //
// ----------------------------------------------------------------------- //

void
wm_live_engine_start(void)
{
  whenmoon_state_t *st;
  int64_t           now_ms;
  char              names[WM_LIVE_MAX_EXCHANGES][EXCHANGE_NAME_SZ];
  uint32_t          n_names = 0;
  uint32_t          i;

  if(!g_live.initialized) return;

  st = whenmoon_get_state();
  if(st == NULL) return;

  // Pre-register the master kill-switch KV per registered exchange so
  // operators can `/set kv` it at any time. Lazy-register in
  // wm_live_master_live_enabled still guards against init-order
  // regressions.
  if(exchange_name_list(names, WM_LIVE_MAX_EXCHANGES, &n_names) != SUCCESS)
    n_names = 0;

  if(n_names > WM_LIVE_MAX_EXCHANGES)
    n_names = WM_LIVE_MAX_EXCHANGES;

  for(i = 0; i < n_names; i++)
  {
    char path[160];
    int  n;

    n = snprintf(path, sizeof(path),
        "plugin.whenmoon.exchange.%s.live", names[i]);

    if(n > 0 && (size_t)n < sizeof(path))
      (void)kv_register(path, KV_BOOL, "false", NULL, NULL,
          "Master kill-switch for the per-market real-mode submit"
          " path on this exchange. Default false; flip to true to"
          " enable live order placement. Per-market risk caps still"
          " apply when this is true.");
  }

  // Seed the global REST-poll cursor at "now - overlap" so the first
  // poll fetches the recent past. Without this seed the first poll
  // pulls every fill the server is willing to page back (hundreds of
  // potentially stale rows), and the orphan dedup walk would log them
  // all.
  now_ms = (int64_t)time(NULL) * 1000;

  pthread_mutex_lock(&g_live.mu);

  if(g_live.last_fills_cursor_ms == 0)
  {
    g_live.last_fills_cursor_ms = now_ms > WM_LIVE_FILLS_POLL_OVERLAP_MS
        ? now_ms - WM_LIVE_FILLS_POLL_OVERLAP_MS
        : 0;
  }

  pthread_mutex_unlock(&g_live.mu);

  // Schedule the periodic regardless of cred state — the tick re-checks
  // each fire and skips when creds are absent.
  if(g_live.fills_poll_task == TASK_HANDLE_NONE)
  {
    g_live.fills_poll_task = task_add_periodic("wm.live.fills",
        TASK_ANY, 200,
        WM_LIVE_FILLS_POLL_SEC * 1000,
        wm_live_fills_poll_tick, st);

    if(g_live.fills_poll_task == TASK_HANDLE_NONE)
      clam(CLAM_WARN, WM_LIVE_CTX,
          "fills-poll periodic submit failed");
    else
      clam(CLAM_INFO, WM_LIVE_CTX,
          "fills-poll scheduled every %us", WM_LIVE_FILLS_POLL_SEC);
  }

  // Boot reconcile: list open orders at gateway, per credentialed
  // exchange. Advisory-only (does not auto-attach to pending state).
  for(i = 0; i < n_names; i++)
  {
    exchange_capabilities_t caps;

    if(exchange_get_capabilities(names[i], &caps) != SUCCESS
        || !caps.has_credentials)
      continue;

    snprintf(wm_live_reconcile_names[i],
        sizeof(wm_live_reconcile_names[i]), "%s", names[i]);

    if(exchange_list_orders_async(names[i], "OPEN", NULL,
           wm_live_boot_reconcile_cb,
           wm_live_reconcile_names[i]) != SUCCESS)
      clam(CLAM_DEBUG, WM_LIVE_CTX,
          "boot reconcile submit failed (%s)", names[i]);
  }
}
