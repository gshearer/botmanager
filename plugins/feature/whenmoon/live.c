// botmanager — MIT
// whenmoon real-mode trade execution.
//
// Two coexisting paths after WM-MK-3-B:
//
//   1. Per-market path (production). wm_market_engine_on_signal calls
//      wm_market_engine_real_submit_locked under mk->lock when
//      session.mode == WM_MARKET_MODE_REAL. That helper runs the
//      master kill-switch + cred + daily-loss + pending-cap +
//      max-notional-clip cascade, registers a wm_market_pending_t in
//      mk->session.pending[], and dispatches
//      coinbase_place_order_async. The done callback
//      (wm_live_market_order_done) reaps the pending row on gateway
//      reject or records gateway_accepted=true on accept. Fills land
//      asynchronously through the WS user-channel + REST /fills
//      consumers (wm_live_handle_ws_fill / wm_live_on_fills) which
//      route into wm_market_engine_record_external_fill.
//
//   2. Legacy per-(market, strategy) path. wm_live_engine_on_signal_locked
//      and wm_live_engine_operator_submit_locked back the
//      `/whenmoon trade *` verb tree. Pending rows live on a private
//      wm_live_book_t chain in g_live.head (own dedicated mutex).
//      WM-MK-5 deletes both entry points + the legacy chain.
//
// Master kill-switches:
//   - plugin.whenmoon.force_manual (legacy path; true = block).
//   - plugin.whenmoon.exchange.coinbase.live (per-market path; true =
//     allow). Distinct knobs, distinct polarity, distinct subsystems.
//
// Locking:
//   - g_live.mu protects g_live.head (legacy chain) +
//     g_live.last_fills_cursor_ms.
//   - mk->lock protects mk->session.pending[] (per-market path).
//   - The two locks are independent; no path takes both.

#define WHENMOON_INTERNAL
#include "live.h"

#include "market.h"
#include "market_engine.h"
#include "order.h"
#include "pnl.h"
#include "sizer.h"
#include "whenmoon.h"
#include "whenmoon_strategy.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "kv.h"
#include "task.h"

#include "coinbase_api.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

#define WM_LIVE_CTX  "whenmoon.live"

// ----------------------------------------------------------------------- //
// Side-table state                                                        //
// ----------------------------------------------------------------------- //

// Legacy per-(market, strategy) book row. WM-MK-3-B keeps this for
// `wm_live_engine_on_signal_locked` + `wm_live_engine_operator_submit_locked`
// (back the `/whenmoon trade *` verbs); WM-MK-5 rips both entry points
// and this struct in one cut. The pending ring + daily-loss anchor
// stay alive on this struct for the legacy path. Trade-id dedup +
// last_fill_ms tracking moved to per-market state in WM-MK-3-B (see
// wm_market_session_t.pending[].recorded_trade_ids[] + the global
// g_live.last_fills_cursor_ms).
typedef struct wm_live_book
{
  char                   market_id_str[WM_MARKET_ID_STR_SZ];
  char                   strategy_name[WM_STRATEGY_NAME_SZ];

  wm_trade_pending_t     pending[WM_TRADE_PENDING_CAP];
  uint32_t               pending_n;     // populated rows

  // Daily-loss anchor (UTC). On every signal, if the book's mark ts
  // crosses a UTC midnight relative to the anchor, we re-anchor to the
  // current cumulative realized_pnl. realized_today is the difference.
  int64_t                day_anchor_utc_ms;
  double                 day_anchor_realized_pnl;

  struct wm_live_book   *next;
} wm_live_book_t;

static struct
{
  pthread_mutex_t      mu;

  // Legacy per-(market, strategy) book chain. Used by
  // wm_live_engine_on_signal_locked + _operator_submit_locked which
  // back the `/whenmoon trade *` verb tree. WM-MK-3-B left these
  // entries in place; WM-MK-5 rips both entry points + the chain in
  // one cut.
  wm_live_book_t      *head;
  bool                 initialized;

  // WS user-channel subscription (one for the entire live trader; AT
  // user channel emits across every product the auth principal owns).
  // Refreshed by wm_live_ws_resub when the market product set changes.
  coinbase_ws_sub_t   *ws_sub;
  whenmoon_state_t    *ws_st;

  // REST /fills safety-net poll periodic.
  task_handle_t        fills_poll_task;

  // WM-MK-3-B: single global cursor for the REST /fills poll. Coinbase
  // pages /fills globally on sequence_timestamp, not per-product, so a
  // per-market cursor doesn't help. Initialized to "now - overlap" on
  // engine start; advanced on every applied fill via the consumers.
  // Guarded by g_live.mu.
  int64_t              last_fills_cursor_ms;
} g_live;

// Forward declarations — definitions further down.
static void wm_live_ws_user_event_cb(const coinbase_ws_event_t *ev,
    void *user);
static void wm_live_fills_poll_tick(task_t *t);

#define WM_LIVE_FILLS_POLL_SEC   30
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

  // Pre-register the master kill-switch KV so operators can `/set kv`
  // it at any time, including before the first real-mode signal would
  // have lazy-registered it. wm_live_master_live_enabled() still calls
  // the lazy path defensively if init order regresses.
  (void)kv_register("plugin.whenmoon.exchange.coinbase.live", KV_BOOL,
      "false", NULL, NULL,
      "Master kill-switch for the per-market real-mode submit path."
      " Default false; flip to true to enable live order placement on"
      " coinbase. Per-market risk caps still apply when this is true.");

  clam(CLAM_DEBUG, WM_LIVE_CTX, "live engine initialized");
  return(SUCCESS);
}

void
wm_live_engine_destroy(void)
{
  wm_live_book_t *b;
  wm_live_book_t *next;

  if(!g_live.initialized) return;

  // Cancel periodic first so no fresh tick fires on freed state.
  task_cancel(g_live.fills_poll_task);
  g_live.fills_poll_task = TASK_HANDLE_NONE;

  // Drop WS subscription before grabbing the lock — coinbase_ws_unsub
  // can fire callbacks on the WS reader thread; better to let those
  // see g_live.initialized=false (still true here, but the ws_sub
  // pointer null-out below means no new events).
  if(g_live.ws_sub != NULL)
  {
    coinbase_ws_unsubscribe(g_live.ws_sub);
    g_live.ws_sub = NULL;
  }
  g_live.ws_st = NULL;

  pthread_mutex_lock(&g_live.mu);

  b = g_live.head;
  while(b != NULL)
  {
    next = b->next;
    mem_free(b);
    b = next;
  }
  g_live.head = NULL;

  pthread_mutex_unlock(&g_live.mu);
  pthread_mutex_destroy(&g_live.mu);
  g_live.initialized = false;

  clam(CLAM_DEBUG, WM_LIVE_CTX, "live engine torn down");
}

// ----------------------------------------------------------------------- //
// Side-table helpers (caller holds g_live.mu)                             //
// ----------------------------------------------------------------------- //

static wm_live_book_t *
wm_live_book_find_locked(const char *market_id_str,
    const char *strategy_name)
{
  for(wm_live_book_t *lb = g_live.head; lb != NULL; lb = lb->next)
  {
    if(strcmp(lb->market_id_str, market_id_str) == 0
        && strcmp(lb->strategy_name, strategy_name) == 0)
      return(lb);
  }
  return(NULL);
}

static wm_live_book_t *
wm_live_book_get_locked(const char *market_id_str,
    const char *strategy_name)
{
  wm_live_book_t *lb;

  lb = wm_live_book_find_locked(market_id_str, strategy_name);
  if(lb != NULL) return(lb);

  lb = mem_alloc(WM_LIVE_CTX, "live_book", sizeof(*lb));
  if(lb == NULL) return(NULL);

  memset(lb, 0, sizeof(*lb));
  snprintf(lb->market_id_str, sizeof(lb->market_id_str), "%s",
      market_id_str);
  snprintf(lb->strategy_name, sizeof(lb->strategy_name), "%s",
      strategy_name);

  lb->next   = g_live.head;
  g_live.head = lb;
  return(lb);
}

// ----------------------------------------------------------------------- //
// KV + UTC helpers                                                        //
// ----------------------------------------------------------------------- //

static int64_t
wm_live_utc_day_start_ms(int64_t epoch_ms)
{
  time_t    t  = (time_t)(epoch_ms / 1000);
  struct tm tm = {0};

  gmtime_r(&t, &tm);
  tm.tm_hour = 0;
  tm.tm_min  = 0;
  tm.tm_sec  = 0;

  return((int64_t)timegm(&tm) * 1000);
}

// True iff the plugin-wide master "force every real book to behave as
// manual" KV is set. When true, every WM_TRADE_MODE_REAL book is
// downgraded for signal-driven submission only — operator-issued
// /whenmoon trade buy|sell still routes normally (the operator
// override is explicit and not affected by this knob).
static bool
wm_live_force_manual(void)
{
  static const char *path = "plugin.whenmoon.force_manual";

  if(!kv_exists(path))
    (void)kv_register(path, KV_BOOL, "false", NULL, NULL,
        "Plugin-wide master switch: when true, every real-mode book"
        " behaves as if it were in manual mode for signal-driven"
        " trades. Operator-issued /whenmoon trade buy|sell still"
        " executes normally. Use as a single-knob stop for all real"
        " trading without touching per-book mode state. Default"
        " false. Per-book risk caps still apply when this is false.");

  return(kv_get_uint(path) != 0);
}

static double
wm_live_get_daily_loss_bps(const wm_trade_book_t *book)
{
  char    path[KV_KEY_SZ * 2];
  double  bps;

  snprintf(path, sizeof(path),
      "plugin.whenmoon.market.%s.strategy.%s.daily_loss_bps",
      book->market_id_str, book->strategy_name);

  if(!kv_exists(path))
    (void)kv_register(path, KV_DOUBLE, "200.0", NULL, NULL,
        "Daily realized-loss cap, basis points of starting_cash."
        " Real-mode signals FAIL closed when realized PnL since the"
        " live engine's day-anchor breaches -starting_cash * bps/10000.");

  bps = kv_get_double(path);
  if(bps > 0.0) return(bps);

  return((double)WM_TRADE_RISK_DEFAULT_DAILY_LOSS_BPS);
}

static double
wm_live_get_max_notional(const wm_trade_book_t *book)
{
  char path[KV_KEY_SZ * 2];

  snprintf(path, sizeof(path),
      "plugin.whenmoon.market.%s.strategy.%s.max_notional",
      book->market_id_str, book->strategy_name);

  if(!kv_exists(path))
    (void)kv_register(path, KV_DOUBLE, "0.0", NULL, NULL,
        "Per-order notional cap (quote currency). 0 = uncapped."
        " When |intent.qty * mark| exceeds this, qty is clipped to"
        " fit; the order is NOT rejected.");

  return(kv_get_double(path));
}

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

// Build a coinbase_place_order_req_t for a GTC limit order. Pure
// plumbing — no state reads, no allocation. `product_id` is the wire-
// form ("BTC-USD"), `side_str` is "buy" or "sell", `coid` is a
// pre-minted v4 UUID. Caller zero-inits `out` before calling; this
// helper overwrites the whole struct.
static void
wm_live_build_place_order_req(coinbase_place_order_req_t *out,
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

// Master kill-switch for the per-market real submit path. Lazy-
// registers `plugin.whenmoon.exchange.coinbase.live` (KV_BOOL, default
// false) on first read so the operator can flip it via `/set kv`. The
// helper FAILs closed when the KV is missing or false — explicit opt-in
// is required to enable real trading. Distinct from the legacy
// `plugin.whenmoon.force_manual` knob, which the legacy book-engine
// path uses with the opposite polarity.
static bool
wm_live_master_live_enabled(void)
{
  static const char *path = "plugin.whenmoon.exchange.coinbase.live";

  if(!kv_exists(path))
    (void)kv_register(path, KV_BOOL, "false", NULL, NULL,
        "Master kill-switch for the per-market real-mode submit"
        " path. Default false; flip to true to enable live order"
        " placement on coinbase. Per-market risk caps still apply"
        " when this is true.");

  return(kv_get_uint(path) != 0);
}

// ----------------------------------------------------------------------- //
// Place-order done callback (curl worker thread)                          //
// ----------------------------------------------------------------------- //

typedef struct wm_live_done_ctx
{
  char  market_id_str[WM_MARKET_ID_STR_SZ];
  char  strategy_name[WM_STRATEGY_NAME_SZ];
  char  coid[WM_TRADE_COID_SZ];
} wm_live_done_ctx_t;

static void
wm_live_order_done(const coinbase_order_result_t *res, void *user)
{
  wm_live_done_ctx_t *ctx;
  wm_live_book_t     *lb;
  bool                err;

  ctx = (wm_live_done_ctx_t *)user;
  if(ctx == NULL) return;

  err = (res != NULL && res->err[0] != '\0');

  pthread_mutex_lock(&g_live.mu);

  lb = wm_live_book_find_locked(ctx->market_id_str, ctx->strategy_name);

  if(lb != NULL)
  {
    for(uint32_t i = 0; i < lb->pending_n; i++)
    {
      if(strcmp(lb->pending[i].coid, ctx->coid) != 0) continue;

      if(err)
      {
        clam(CLAM_WARN, WM_LIVE_CTX,
            "%s/%s order rejected coid=%s err=%s",
            lb->market_id_str, lb->strategy_name, ctx->coid, res->err);

        lb->pending[i] = lb->pending[lb->pending_n - 1];
        lb->pending_n--;
      }
      else
      {
        snprintf(lb->pending[i].order_id, sizeof(lb->pending[i].order_id),
            "%s", res->order.order_id);
        lb->pending[i].gateway_accepted = true;

        clam(CLAM_INFO, WM_LIVE_CTX,
            "%s/%s order accepted coid=%s order_id=%s",
            lb->market_id_str, lb->strategy_name,
            ctx->coid, res->order.order_id);
      }
      break;
    }
  }

  pthread_mutex_unlock(&g_live.mu);
  mem_free(ctx);
}

// ----------------------------------------------------------------------- //
// Signal entry point                                                      //
// ----------------------------------------------------------------------- //

bool
wm_live_engine_on_signal_locked(wm_trade_book_t *book,
    double mark_px, int64_t mark_ms, const wm_strategy_signal_t *sig)
{
  wm_sizer_intent_t           intent;
  wm_live_book_t             *lb;
  wm_trade_pending_t         *pending;
  coinbase_place_order_req_t  req     = {0};
  wm_live_done_ctx_t         *ctx;
  char                        exchange[16];
  char                        base[8];
  char                        quote[8];
  char                        wire_product[16];
  double                      daily_loss_bps;
  double                      daily_loss_cap;
  double                      max_notional;
  double                      realized_pnl;
  double                      realized_today;
  int64_t                     today_start;
  const char                 *side_str;
  bool                        gate_ok;

  if(book == NULL || sig == NULL || mark_px <= 0.0)
    return(FAIL);

  if(wm_market_parse_id(book->market_id_str, exchange, sizeof(exchange),
         base, sizeof(base), quote, sizeof(quote)) != SUCCESS)
  {
    clam(CLAM_WARN, WM_LIVE_CTX,
        "%s: malformed market id, refusing live submit",
        book->market_id_str);
    return(FAIL);
  }

  // Gate 1: master force_manual override.
  if(wm_live_force_manual())
  {
    clam(CLAM_WARN, WM_LIVE_CTX,
        "real submit suppressed: plugin.whenmoon.force_manual=true"
        " (book %s/%s temporarily acting as manual)",
        book->market_id_str, book->strategy_name);
    return(FAIL);
  }

  // Gate 2: credentials.
  if(!coinbase_apikey_configured())
  {
    clam(CLAM_WARN, WM_LIVE_CTX,
        "real submit refused: %s credentials not configured",
        exchange);
    return(FAIL);
  }

  // Gates 3 + 4 require the side-table.
  pthread_mutex_lock(&g_live.mu);

  lb = wm_live_book_get_locked(book->market_id_str, book->strategy_name);
  if(lb == NULL)
  {
    pthread_mutex_unlock(&g_live.mu);
    clam(CLAM_WARN, WM_LIVE_CTX, "%s/%s: live state alloc failed",
        book->market_id_str, book->strategy_name);
    return(FAIL);
  }

  // Daily-loss anchor. On first contact (anchor uninitialised) we leave
  // both fields at zero so realized_today equals lifetime realized PnL —
  // this conservatively surfaces any losses already on the book at boot
  // time. Once the daemon has been alive across a UTC midnight, the
  // anchor advances to that midnight's realized PnL and subsequent
  // checks are properly "since midnight". Persisting the anchor across
  // reboots is a future refinement; v1 errs on the side of fail-closed.
  today_start = wm_live_utc_day_start_ms(mark_ms > 0 ? mark_ms
      : (int64_t)time(NULL) * 1000);

  realized_pnl = (book->pnl != NULL) ? book->pnl->realized_pnl : 0.0;

  if(lb->day_anchor_utc_ms == 0)
    lb->day_anchor_utc_ms = today_start;
  else if(lb->day_anchor_utc_ms != today_start)
  {
    lb->day_anchor_utc_ms       = today_start;
    lb->day_anchor_realized_pnl = realized_pnl;
  }

  realized_today = realized_pnl - lb->day_anchor_realized_pnl;

  daily_loss_bps = wm_live_get_daily_loss_bps(book);
  daily_loss_cap = book->starting_cash * daily_loss_bps / 10000.0;

  // Gate 3: daily-loss cap.
  if(realized_today < -daily_loss_cap)
  {
    pthread_mutex_unlock(&g_live.mu);
    clam(CLAM_WARN, WM_LIVE_CTX,
        "%s/%s daily loss cap tripped: realized_today=%.4f cap=%.4f",
        book->market_id_str, book->strategy_name,
        realized_today, daily_loss_cap);
    return(FAIL);
  }

  // Gate 4: pending count.
  gate_ok = (lb->pending_n < WM_TRADE_PENDING_CAP);
  pthread_mutex_unlock(&g_live.mu);

  if(!gate_ok)
  {
    clam(CLAM_WARN, WM_LIVE_CTX,
        "%s/%s pending cap reached (%u); dropping signal",
        book->market_id_str, book->strategy_name,
        WM_TRADE_PENDING_CAP);
    return(FAIL);
  }

  // Gate 5: sizer.
  wm_sizer_compute(book, mark_px, sig, &intent);

  if(intent.action == WM_SIZER_HOLD)
    return(FAIL);

  // Gate 6: max-notional clip.
  max_notional = wm_live_get_max_notional(book);

  if(max_notional > 0.0)
  {
    double notional = intent.qty * mark_px;
    if(notional > max_notional)
    {
      double clipped = max_notional / mark_px;
      clam(CLAM_INFO, WM_LIVE_CTX,
          "%s/%s notional clip qty %.6g -> %.6g (cap=%.4f mark=%.4f)",
          book->market_id_str, book->strategy_name,
          intent.qty, clipped, max_notional, mark_px);
      intent.qty = clipped;
    }
  }

  side_str = (intent.action == WM_SIZER_BUY) ? "buy" : "sell";

  // Wire-form product id is the uppercase dash form. Build into a
  // tightly-sized intermediate so the compiler can prove no truncation
  // into req.product_id.
  snprintf(wire_product, sizeof(wire_product), "%s-%s", base, quote);
  for(char *p = wire_product; *p != '\0'; p++)
    if(*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);

  snprintf(req.product_id, sizeof(req.product_id), "%s", wire_product);

  if(wm_live_uuid_v4(req.client_oid, sizeof(req.client_oid)) != SUCCESS)
  {
    clam(CLAM_WARN, WM_LIVE_CTX,
        "%s/%s: client_oid mint failed",
        book->market_id_str, book->strategy_name);
    return(FAIL);
  }

  snprintf(req.side, sizeof(req.side), "%s", side_str);
  snprintf(req.type, sizeof(req.type), "limit");
  snprintf(req.tif,  sizeof(req.tif),  "GTC");
  req.price     = mark_px;
  req.size      = intent.qty;
  req.post_only = false;

  // Register the pending row before submit so the done_cb finds it on
  // the curl worker thread.
  pthread_mutex_lock(&g_live.mu);

  if(lb->pending_n >= WM_TRADE_PENDING_CAP)
  {
    pthread_mutex_unlock(&g_live.mu);
    return(FAIL);
  }

  pending = &lb->pending[lb->pending_n++];
  memset(pending, 0, sizeof(*pending));

  snprintf(pending->coid, sizeof(pending->coid), "%s", req.client_oid);
  snprintf(pending->side, sizeof(pending->side), "%s",
      (intent.action == WM_SIZER_BUY) ? "buy" : "sell");
  pending->limit_px      = mark_px;
  pending->submitted_qty = intent.qty;
  pending->submitted_ms  = mark_ms;

  pthread_mutex_unlock(&g_live.mu);

  ctx = mem_alloc(WM_LIVE_CTX, "live_done_ctx", sizeof(*ctx));
  if(ctx == NULL)
  {
    pthread_mutex_lock(&g_live.mu);
    if(lb->pending_n > 0) lb->pending_n--;
    pthread_mutex_unlock(&g_live.mu);
    return(FAIL);
  }

  snprintf(ctx->market_id_str, sizeof(ctx->market_id_str), "%s",
      book->market_id_str);
  snprintf(ctx->strategy_name, sizeof(ctx->strategy_name), "%s",
      book->strategy_name);
  snprintf(ctx->coid, sizeof(ctx->coid), "%s", req.client_oid);

  if(coinbase_place_order_async(&req, wm_live_order_done, ctx) != SUCCESS)
  {
    // coinbase_place_order_async fires the done callback synchronously
    // with res->err set when it returns FAIL — the done_cb has already
    // dropped the pending row and freed ctx by the time we get here.
    // Do NOT free ctx again or roll back pending_n; both have happened.
    clam(CLAM_WARN, WM_LIVE_CTX,
        "%s/%s coinbase_place_order_async returned FAIL"
        " (done_cb already fired)",
        book->market_id_str, book->strategy_name);
    return(FAIL);
  }

  clam(CLAM_INFO, WM_LIVE_CTX,
      "%s/%s submit %s qty=%.6g px=%.4f coid=%s",
      book->market_id_str, book->strategy_name,
      side_str, intent.qty, mark_px, req.client_oid);

  return(SUCCESS);
}

// External fill entry point body lives in order.c (it touches the
// trade-book registry + book persistence directly). See
// wm_trade_engine_record_external_fill in order.c.

// ----------------------------------------------------------------------- //
// Operator-issued real submit (manual mode + ad-hoc real)                 //
// ----------------------------------------------------------------------- //

bool
wm_live_engine_operator_submit_locked(wm_trade_book_t *book,
    char side, double qty, double limit_px,
    char *errbuf, size_t errbuf_sz)
{
  wm_live_book_t             *lb;
  wm_trade_pending_t         *pending;
  coinbase_place_order_req_t  req     = {0};
  wm_live_done_ctx_t         *ctx;
  char                        exchange[16];
  char                        base[8];
  char                        quote[8];
  char                        wire_product[16];
  double                      daily_loss_bps;
  double                      daily_loss_cap;
  double                      max_notional;
  double                      realized_pnl;
  double                      realized_today;
  int64_t                     today_start;
  int64_t                     now_ms;
  bool                        gate_ok;
  bool                        is_buy;

  #define ERRSET(...) do { \
      if(errbuf != NULL && errbuf_sz > 0) \
        snprintf(errbuf, errbuf_sz, __VA_ARGS__); \
    } while(0)

  if(book == NULL || qty <= 0.0)
    { ERRSET("invalid args"); return(FAIL); }

  if(side != 'b' && side != 's')
    { ERRSET("side must be 'b' or 's'"); return(FAIL); }

  is_buy = (side == 'b');

  if(wm_market_parse_id(book->market_id_str, exchange, sizeof(exchange),
         base, sizeof(base), quote, sizeof(quote)) != SUCCESS)
    { ERRSET("malformed market id"); return(FAIL); }

  // Operator-issued bypasses force_manual; that knob exists to halt
  // automated signal flow, not to override explicit human action.

  if(!coinbase_apikey_configured())
    { ERRSET("%s credentials not configured", exchange); return(FAIL); }

  if(limit_px <= 0.0)
    limit_px = book->last_mark_px;
  if(limit_px <= 0.0)
    { ERRSET("no mark price; specify limit_px"); return(FAIL); }

  now_ms = (int64_t)time(NULL) * 1000;
  if(book->last_mark_ms > now_ms) now_ms = book->last_mark_ms;

  pthread_mutex_lock(&g_live.mu);

  lb = wm_live_book_get_locked(book->market_id_str, book->strategy_name);
  if(lb == NULL)
    {
      pthread_mutex_unlock(&g_live.mu);
      ERRSET("live state alloc failed");
      return(FAIL);
    }

  today_start = wm_live_utc_day_start_ms(now_ms);
  realized_pnl = (book->pnl != NULL) ? book->pnl->realized_pnl : 0.0;

  if(lb->day_anchor_utc_ms == 0)
    lb->day_anchor_utc_ms = today_start;
  else if(lb->day_anchor_utc_ms != today_start)
    {
      lb->day_anchor_utc_ms       = today_start;
      lb->day_anchor_realized_pnl = realized_pnl;
    }

  realized_today = realized_pnl - lb->day_anchor_realized_pnl;
  daily_loss_bps = wm_live_get_daily_loss_bps(book);
  daily_loss_cap = book->starting_cash * daily_loss_bps / 10000.0;

  if(realized_today < -daily_loss_cap)
    {
      pthread_mutex_unlock(&g_live.mu);
      ERRSET("daily loss cap tripped (%.4f < -%.4f)",
          realized_today, daily_loss_cap);
      return(FAIL);
    }

  gate_ok = (lb->pending_n < WM_TRADE_PENDING_CAP);
  pthread_mutex_unlock(&g_live.mu);

  if(!gate_ok)
    { ERRSET("pending cap reached (%u)", WM_TRADE_PENDING_CAP);
      return(FAIL); }

  max_notional = wm_live_get_max_notional(book);
  if(max_notional > 0.0 && qty * limit_px > max_notional)
    {
      double clipped = max_notional / limit_px;
      clam(CLAM_INFO, WM_LIVE_CTX,
          "%s/%s operator notional clip qty %.6g -> %.6g",
          book->market_id_str, book->strategy_name, qty, clipped);
      qty = clipped;
    }

  snprintf(wire_product, sizeof(wire_product), "%s-%s", base, quote);
  for(char *p = wire_product; *p != '\0'; p++)
    if(*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);

  snprintf(req.product_id, sizeof(req.product_id), "%s", wire_product);

  if(wm_live_uuid_v4(req.client_oid, sizeof(req.client_oid)) != SUCCESS)
    { ERRSET("client_oid mint failed"); return(FAIL); }

  snprintf(req.side, sizeof(req.side), "%s", is_buy ? "buy" : "sell");
  snprintf(req.type, sizeof(req.type), "limit");
  snprintf(req.tif,  sizeof(req.tif),  "GTC");
  req.price     = limit_px;
  req.size      = qty;
  req.post_only = false;

  pthread_mutex_lock(&g_live.mu);
  if(lb->pending_n >= WM_TRADE_PENDING_CAP)
    {
      pthread_mutex_unlock(&g_live.mu);
      ERRSET("pending cap raced full");
      return(FAIL);
    }

  pending = &lb->pending[lb->pending_n++];
  memset(pending, 0, sizeof(*pending));
  snprintf(pending->coid, sizeof(pending->coid), "%s", req.client_oid);
  snprintf(pending->side, sizeof(pending->side), "%s",
      is_buy ? "buy" : "sell");
  pending->limit_px      = limit_px;
  pending->submitted_qty = qty;
  pending->submitted_ms  = now_ms;

  pthread_mutex_unlock(&g_live.mu);

  ctx = mem_alloc(WM_LIVE_CTX, "live_done_ctx", sizeof(*ctx));
  if(ctx == NULL)
    {
      pthread_mutex_lock(&g_live.mu);
      if(lb->pending_n > 0) lb->pending_n--;
      pthread_mutex_unlock(&g_live.mu);
      ERRSET("oom");
      return(FAIL);
    }

  snprintf(ctx->market_id_str, sizeof(ctx->market_id_str), "%s",
      book->market_id_str);
  snprintf(ctx->strategy_name, sizeof(ctx->strategy_name), "%s",
      book->strategy_name);
  snprintf(ctx->coid, sizeof(ctx->coid), "%s", req.client_oid);

  if(coinbase_place_order_async(&req, wm_live_order_done, ctx) != SUCCESS)
    { ERRSET("place_order_async returned FAIL"); return(FAIL); }

  clam(CLAM_INFO, WM_LIVE_CTX,
      "%s/%s operator submit %s qty=%.6g px=%.4f coid=%s",
      book->market_id_str, book->strategy_name,
      is_buy ? "buy" : "sell", qty, limit_px, req.client_oid);

  return(SUCCESS);

  #undef ERRSET
}

// ----------------------------------------------------------------------- //
// Per-market real-mode submit (WM-MK-3-B)                                 //
// ----------------------------------------------------------------------- //

// Heap-owned ctx for the per-market done callback. Allocated under
// `mk->lock` in wm_market_engine_real_submit_locked, freed by the
// done_cb (curl worker thread).
typedef struct wm_live_market_done_ctx
{
  char  market_id_str[WM_MARKET_ID_STR_SZ];
  char  coid[COINBASE_CLIENT_OID_SZ];
} wm_live_market_done_ctx_t;

// Done callback for the per-market submit path. Runs on the curl
// worker thread; takes `mk->lock` independently of any caller. On a
// gateway error, reaps the pending row + frees ctx. On success,
// records `order_id` + sets `gateway_accepted = true` so the WS
// user-channel order updates land on a populated row.
//
// FAIL semantics from coinbase_place_order_async: on synchronous-FAIL
// the caller pre-fills `res->err` and the done_cb is invoked
// synchronously; the caller must not touch the pending row or ctx
// after the call returns. On asynchronous failures the same callback
// fires off-thread.
static void
wm_live_market_order_done(const coinbase_order_result_t *res, void *user)
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
  coinbase_place_order_req_t  req;
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

  if(side != 'b' && side != 's')
  {
    ERRSET("side must be 'b' or 's'");
    return(FAIL);
  }

  is_buy   = (side == 'b');
  side_str = is_buy ? "buy" : "sell";

  // Gate 1: master kill-switch. Distinct from the legacy
  // `plugin.whenmoon.force_manual` knob; this one is the per-market
  // path's explicit opt-in.
  if(!wm_live_master_live_enabled())
  {
    ERRSET("live trading disabled"
        " (plugin.whenmoon.exchange.coinbase.live=false)");
    clam(CLAM_WARN, WM_LIVE_CTX,
        "%s real submit refused: live=false",
        mk->market_id_str);
    return(FAIL);
  }

  // Gate 2: credentials.
  if(!coinbase_apikey_configured())
  {
    ERRSET("no exchange credentials");
    clam(CLAM_WARN, WM_LIVE_CTX,
        "%s real submit refused: coinbase credentials not configured",
        mk->market_id_str);
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
    char coid[COINBASE_CLIENT_OID_SZ];

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

  if(coinbase_place_order_async(&req, wm_live_market_order_done, ctx)
      != SUCCESS)
  {
    // coinbase_place_order_async fires the done_cb synchronously with
    // res->err set when it returns FAIL. The done_cb has already
    // reaped the pending row + freed ctx by the time we get here.
    // Do NOT touch state.
    ERRSET("coinbase_place_order_async returned FAIL"
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
// Per-market pending lookup (WM-MK-3-B)                                   //
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
wm_live_handle_ws_fill(const coinbase_ws_user_fill_t *f)
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
    // to an unrelated principal (the AT user channel emits per-account,
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
    // Shift-down preserving order. Same shape as the legacy reap.
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
wm_live_handle_ws_order(const coinbase_ws_user_order_t *o)
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
wm_live_ws_user_event_cb(const coinbase_ws_event_t *ev, void *user)
{
  const coinbase_ws_user_event_t *u;

  (void)user;

  if(ev == NULL || ev->channel != COINBASE_CH_USER || ev->payload == NULL)
    return;

  u = ev->payload;

  switch(u->kind)
  {
    case COINBASE_WS_USER_KIND_ORDER:
      wm_live_handle_ws_order(&u->u.order);
      break;
    case COINBASE_WS_USER_KIND_FILL:
      wm_live_handle_ws_fill(&u->u.fill);
      break;
  }
}

// ----------------------------------------------------------------------- //
// WS resub (called from market.c)                                         //
// ----------------------------------------------------------------------- //

void
wm_live_ws_resub(whenmoon_state_t *st,
    const char *const *product_ids, size_t n_products)
{
  static const coinbase_ws_channel_t channels[] = { COINBASE_CH_USER };

  if(!g_live.initialized) return;

  if(g_live.ws_sub != NULL)
  {
    coinbase_ws_unsubscribe(g_live.ws_sub);
    g_live.ws_sub = NULL;
  }

  g_live.ws_st = st;

  if(n_products == 0 || product_ids == NULL)
    return;

  // CDP creds are required for the user channel; AT subscribe FAILs
  // closed when they are absent. Skip silently — once creds appear the
  // next market mutation (or whenmoon_start re-entry) will retry.
  if(!coinbase_apikey_configured())
  {
    clam(CLAM_DEBUG, WM_LIVE_CTX,
        "user-channel sub deferred: no CDP creds yet");
    return;
  }

  g_live.ws_sub = coinbase_ws_subscribe(channels,
      sizeof(channels) / sizeof(channels[0]),
      product_ids, n_products,
      wm_live_ws_user_event_cb, NULL);

  if(g_live.ws_sub == NULL)
    clam(CLAM_WARN, WM_LIVE_CTX,
        "user-channel WS subscribe failed (n_products=%zu)", n_products);
  else
    clam(CLAM_INFO, WM_LIVE_CTX,
        "user-channel WS subscribed (n_products=%zu)", n_products);
}

// ----------------------------------------------------------------------- //
// REST /fills safety-net poll                                             //
// ----------------------------------------------------------------------- //

typedef struct wm_live_fills_ctx
{
  char  market_id_str[WM_MARKET_ID_STR_SZ];
  char  product_id[16];
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
wm_live_on_fills(const coinbase_fills_result_t *res, void *user)
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
    const coinbase_fill_t *f = &res->rows[i];
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
  if(!coinbase_apikey_configured()) return;

  // Single global cursor: AT pages /fills globally on
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
    wm_live_fills_ctx_t *ctx;

    ctx = mem_alloc(WM_LIVE_CTX, "fills_ctx", sizeof(*ctx));
    if(ctx == NULL) continue;

    snprintf(ctx->market_id_str, sizeof(ctx->market_id_str), "%s",
        mkts->arr[i].market_id_str);
    snprintf(ctx->product_id, sizeof(ctx->product_id), "%s",
        mkts->arr[i].product_id);

    if(coinbase_list_fills_async(NULL, ctx->product_id, cursor_ms,
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
wm_live_boot_reconcile_cb(const coinbase_orders_result_t *res, void *user)
{
  (void)user;

  if(res == NULL) return;

  if(res->err[0] != '\0')
  {
    clam(CLAM_DEBUG, WM_LIVE_CTX,
        "boot reconcile (list orders) err=%s", res->err);
    return;
  }

  if(res->count == 0)
  {
    clam(CLAM_INFO, WM_LIVE_CTX,
        "boot reconcile: no resting orders at gateway");
    return;
  }

  clam(CLAM_WARN, WM_LIVE_CTX,
      "boot reconcile: %u open order(s) found at gateway:",
      res->count);

  for(uint32_t i = 0; i < res->count; i++)
  {
    const coinbase_order_t *o = &res->rows[i];

    clam(CLAM_WARN, WM_LIVE_CTX,
        "  order_id=%s coid=%s product=%s side=%s status=%s"
        " price=%.4f size=%.6g filled=%.6g",
        o->order_id, o->client_oid, o->product_id, o->side, o->status,
        o->price, o->size, o->filled_size);
  }

  clam(CLAM_WARN, WM_LIVE_CTX,
      "boot reconcile: not auto-attaching to local pending state."
      " Inspect via Coinbase UI; cancel manually if undesired.");
}

// ----------------------------------------------------------------------- //
// Late-stage start                                                        //
// ----------------------------------------------------------------------- //

void
wm_live_engine_start(void)
{
  whenmoon_state_t *st;
  int64_t           now_ms;

  if(!g_live.initialized) return;

  st = whenmoon_get_state();
  if(st == NULL) return;

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

  // Boot reconcile: list open orders at gateway. Advisory-only (does
  // not auto-attach to pending state in v1).
  if(coinbase_apikey_configured())
  {
    if(coinbase_list_orders_async("OPEN", NULL,
           wm_live_boot_reconcile_cb, NULL) != SUCCESS)
      clam(CLAM_DEBUG, WM_LIVE_CTX,
          "boot reconcile submit failed");
  }
}
