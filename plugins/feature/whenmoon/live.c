// botmanager — MIT
// whenmoon real-mode trade execution skeleton (WM-LT-8-B2/B3).
//
// The trade engine's signal entry point delegates here when the book is
// in WM_TRADE_MODE_REAL. We apply (in order, fail-closed):
//   1. master force_manual KV (`plugin.whenmoon.force_manual`).
//   2. credentials present (cb_apikey_configured).
//   3. daily-loss cap (realized PnL since UTC midnight).
//   4. pending-order ring not full.
//   5. sizer says non-HOLD.
//   6. max-notional clip (clip qty, do not reject).
// On every gate pass we mint a fresh v4 client_order_id, register a
// pending row, and submit the order asynchronously through
// coinbase_place_order_async. The done callback (curl worker thread)
// records the gateway-assigned order_id or drops the pending row on
// reject.
//
// State layout. The pending ring + daily-loss anchor live in a
// per-(market, strategy) side table keyed by string concatenation,
// guarded by a dedicated mutex. The trade-engine registry lock and
// this lock are NOT acquired together — wm_live_engine_on_signal_locked
// drops the registry lock for the brief window where it acquires
// g_live.mu. The done callback takes only g_live.mu.
//
// B2 scope: kill-switch, risk gates, pending ring, place-order path,
// done callback that records ack/reject. The external-fill API surface
// is shipped (wm_trade_engine_record_external_fill) but its body is a
// skeleton — B3 wires the user-channel consumer + REST poll that drive
// it, at which point the body learns to update the trade book.

#define WHENMOON_INTERNAL
#include "live.h"

#include "market.h"
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

typedef struct wm_live_book
{
  char                   market_id_str[WM_MARKET_ID_STR_SZ];
  char                   strategy_name[WM_STRATEGY_NAME_SZ];

  wm_trade_pending_t     pending[WM_TRADE_PENDING_CAP];
  uint32_t               pending_n;     // populated rows

  // Trade-id dedup ring. Pushed on every fill that touches this
  // (market, strategy) — both WS user-channel and REST /fills poll.
  // Survives pending-row reaping so late safety-net polls don't
  // double-apply.
  int64_t                recent_trade_ids[WM_LIVE_TRADE_DEDUP_CAP];
  uint8_t                recent_trade_n;     // 0..CAP
  uint8_t                recent_trade_head;  // next write slot

  // Cursor for the REST /fills poll (max time_ms ever seen).
  int64_t                last_fill_ms;

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
  wm_live_book_t      *head;
  bool                 initialized;

  // WS user-channel subscription (one for the entire live trader; AT
  // user channel emits across every product the auth principal owns).
  // Refreshed by wm_live_ws_resub when the market product set changes.
  coinbase_ws_sub_t   *ws_sub;
  whenmoon_state_t    *ws_st;

  // REST /fills safety-net poll periodic.
  task_handle_t        fills_poll_task;
} g_live;

// Forward declarations — definitions further down.
static void wm_live_ws_user_event_cb(const coinbase_ws_event_t *ev,
    void *user);
static void wm_live_fills_poll_tick(task_t *t);
static bool wm_live_dedup_seen_locked(wm_live_book_t *lb, int64_t trade_id);
static void wm_live_dedup_record_locked(wm_live_book_t *lb,
    int64_t trade_id);

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
// Trade-id dedup ring (caller holds g_live.mu)                            //
// ----------------------------------------------------------------------- //

static bool
wm_live_dedup_seen_locked(wm_live_book_t *lb, int64_t trade_id)
{
  if(lb == NULL || trade_id == 0) return(false);

  for(uint8_t i = 0; i < lb->recent_trade_n; i++)
    if(lb->recent_trade_ids[i] == trade_id) return(true);

  return(false);
}

static void
wm_live_dedup_record_locked(wm_live_book_t *lb, int64_t trade_id)
{
  if(lb == NULL || trade_id == 0) return;

  lb->recent_trade_ids[lb->recent_trade_head] = trade_id;
  lb->recent_trade_head =
      (uint8_t)((lb->recent_trade_head + 1) % WM_LIVE_TRADE_DEDUP_CAP);

  if(lb->recent_trade_n < WM_LIVE_TRADE_DEDUP_CAP)
    lb->recent_trade_n++;
}

// Find the live-book + pending row that owns this client_order_id.
// Returns the wm_live_book_t and writes the pending index into *pidx.
// Returns NULL if no pending row matches. Caller holds g_live.mu.
static wm_live_book_t *
wm_live_find_pending_by_coid_locked(const char *coid, uint32_t *pidx)
{
  if(coid == NULL || coid[0] == '\0') return(NULL);

  for(wm_live_book_t *lb = g_live.head; lb != NULL; lb = lb->next)
  {
    for(uint32_t i = 0; i < lb->pending_n; i++)
    {
      if(strcmp(lb->pending[i].coid, coid) == 0)
      {
        if(pidx != NULL) *pidx = i;
        return(lb);
      }
    }
  }
  return(NULL);
}

// Reap a pending row (swap-with-last + shrink). Caller holds g_live.mu.
static void
wm_live_reap_pending_locked(wm_live_book_t *lb, uint32_t idx)
{
  if(lb == NULL || idx >= lb->pending_n) return;

  if(idx != lb->pending_n - 1)
    lb->pending[idx] = lb->pending[lb->pending_n - 1];

  lb->pending_n--;
}

// ----------------------------------------------------------------------- //
// User-channel event handlers                                             //
// ----------------------------------------------------------------------- //

static void
wm_live_handle_ws_fill(const coinbase_ws_user_fill_t *f)
{
  wm_live_book_t *lb;
  uint32_t        pidx     = 0;
  wm_fill_t       fill_eng = {0};
  char            market_id_str[WM_MARKET_ID_STR_SZ];
  char            strategy_name[WM_STRATEGY_NAME_SZ];
  bool            reap     = false;

  if(f == NULL || f->trade_id == 0 || f->size <= 0.0 || f->price <= 0.0)
    return;

  pthread_mutex_lock(&g_live.mu);

  lb = wm_live_find_pending_by_coid_locked(f->client_order_id, &pidx);

  if(lb == NULL)
  {
    pthread_mutex_unlock(&g_live.mu);
    clam(CLAM_WARN, WM_LIVE_CTX,
        "ws fill: no pending row for coid=%s order_id=%s tid=%lld"
        " (orphan; skipping)",
        f->client_order_id, f->order_id, (long long)f->trade_id);
    return;
  }

  if(wm_live_dedup_seen_locked(lb, f->trade_id))
  {
    pthread_mutex_unlock(&g_live.mu);
    clam(CLAM_DEBUG2, WM_LIVE_CTX,
        "ws fill: dup trade_id=%lld coid=%s",
        (long long)f->trade_id, f->client_order_id);
    return;
  }

  wm_live_dedup_record_locked(lb, f->trade_id);

  lb->pending[pidx].filled_qty += f->size;
  if(lb->pending[pidx].filled_qty >=
      lb->pending[pidx].submitted_qty - 1e-12)
    reap = true;

  if(f->time_ms > lb->last_fill_ms)
    lb->last_fill_ms = f->time_ms;

  snprintf(market_id_str, sizeof(market_id_str), "%s", lb->market_id_str);
  snprintf(strategy_name, sizeof(strategy_name), "%s", lb->strategy_name);

  if(reap)
    wm_live_reap_pending_locked(lb, pidx);

  pthread_mutex_unlock(&g_live.mu);

  fill_eng.ts_ms        = f->time_ms;
  fill_eng.side         = (f->side[0] == 'b' || f->side[0] == 'B')
                            ? 'b' : 's';
  fill_eng.qty          = f->size;
  fill_eng.price        = f->price;
  fill_eng.fee          = f->fee;
  snprintf(fill_eng.reason, sizeof(fill_eng.reason),
      "ws-fill tid=%lld", (long long)f->trade_id);

  wm_trade_engine_record_external_fill(market_id_str, strategy_name,
      &fill_eng);
}

static void
wm_live_handle_ws_order(const coinbase_ws_user_order_t *o)
{
  wm_live_book_t *lb;
  uint32_t        pidx     = 0;
  bool            reap     = false;
  bool            failed   = false;
  const char     *status;

  if(o == NULL) return;

  status = o->status;

  // Map AT terminal states to reap. OPEN/PENDING are non-terminal.
  if(strcmp(status, "FILLED") == 0
      || strcmp(status, "CANCELLED") == 0
      || strcmp(status, "EXPIRED")  == 0)
    reap = true;
  else if(strcmp(status, "FAILED") == 0)
    reap = failed = true;

  pthread_mutex_lock(&g_live.mu);

  lb = wm_live_find_pending_by_coid_locked(o->client_order_id, &pidx);

  if(lb != NULL)
  {
    if(o->order_id[0] != '\0')
      snprintf(lb->pending[pidx].order_id,
          sizeof(lb->pending[pidx].order_id),
          "%s", o->order_id);

    if(strcmp(status, "OPEN") == 0)
      lb->pending[pidx].gateway_accepted = true;

    if(reap)
    {
      char market[WM_MARKET_ID_STR_SZ];
      char strat[WM_STRATEGY_NAME_SZ];

      snprintf(market, sizeof(market), "%s", lb->market_id_str);
      snprintf(strat,  sizeof(strat),  "%s", lb->strategy_name);

      wm_live_reap_pending_locked(lb, pidx);
      pthread_mutex_unlock(&g_live.mu);

      clam(failed ? CLAM_WARN : CLAM_INFO, WM_LIVE_CTX,
          "ws order %s/%s coid=%s order_id=%s status=%s -> reaped",
          market, strat, o->client_order_id, o->order_id, status);
      return;
    }
  }

  pthread_mutex_unlock(&g_live.mu);
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
    wm_live_book_t        *lb;
    uint32_t               pidx = 0;
    wm_fill_t              fill_eng = {0};
    char                   market[WM_MARKET_ID_STR_SZ];
    char                   strat[WM_STRATEGY_NAME_SZ];
    bool                   reap = false;

    if(f->trade_id == 0 || f->size <= 0.0 || f->price <= 0.0)
      continue;

    pthread_mutex_lock(&g_live.mu);

    lb = wm_live_find_pending_by_coid_locked(f->client_oid, &pidx);

    if(lb == NULL)
    {
      // Orphan: WS likely already applied this fill and reaped the
      // pending row. Walk every live book so we still register the
      // dedup record and bump cursors.
      bool any_dup = false;
      for(wm_live_book_t *p = g_live.head; p != NULL; p = p->next)
      {
        if(wm_live_dedup_seen_locked(p, f->trade_id))
          { any_dup = true; break; }
      }
      pthread_mutex_unlock(&g_live.mu);
      if(!any_dup)
        clam(CLAM_DEBUG, WM_LIVE_CTX,
            "fills poll: orphan tid=%lld coid=%s product=%s",
            (long long)f->trade_id, f->client_oid, f->product_id);
      continue;
    }

    if(wm_live_dedup_seen_locked(lb, f->trade_id))
    {
      pthread_mutex_unlock(&g_live.mu);
      continue;
    }

    wm_live_dedup_record_locked(lb, f->trade_id);

    lb->pending[pidx].filled_qty += f->size;
    if(lb->pending[pidx].filled_qty >=
        lb->pending[pidx].submitted_qty - 1e-12)
      reap = true;

    if(f->time_ms > lb->last_fill_ms)
      lb->last_fill_ms = f->time_ms;

    snprintf(market, sizeof(market), "%s", lb->market_id_str);
    snprintf(strat,  sizeof(strat),  "%s", lb->strategy_name);

    if(reap)
      wm_live_reap_pending_locked(lb, pidx);

    pthread_mutex_unlock(&g_live.mu);

    fill_eng.ts_ms = f->time_ms;
    fill_eng.side  = (f->side[0] == 'b' || f->side[0] == 'B') ? 'b' : 's';
    fill_eng.qty   = f->size;
    fill_eng.price = f->price;
    fill_eng.fee   = f->fee;
    snprintf(fill_eng.reason, sizeof(fill_eng.reason),
        "rest-fill tid=%lld", (long long)f->trade_id);

    clam(CLAM_INFO, WM_LIVE_CTX,
        "fills poll: applied tid=%lld coid=%s",
        (long long)f->trade_id, f->client_oid);

    wm_trade_engine_record_external_fill(market, strat, &fill_eng);
  }

done:
  if(ctx != NULL) mem_free(ctx);
}

static void
wm_live_fills_poll_tick(task_t *t)
{
  whenmoon_state_t   *st = t->data;
  whenmoon_markets_t *mkts;
  int64_t             cursor_ms = 0;

  t->state = TASK_ENDED;

  if(st == NULL || st->markets == NULL) return;
  if(!g_live.initialized) return;
  if(!coinbase_apikey_configured()) return;

  // Compute one cursor across all live books — AT pages globally on
  // sequence_timestamp, so a per-product overhang doesn't help. Use
  // (min(last_fill_ms across books) - overlap) so we don't miss a
  // fill that landed slower on one product than another.
  pthread_mutex_lock(&g_live.mu);
  for(wm_live_book_t *lb = g_live.head; lb != NULL; lb = lb->next)
  {
    if(lb->last_fill_ms == 0) continue;
    if(cursor_ms == 0 || lb->last_fill_ms < cursor_ms)
      cursor_ms = lb->last_fill_ms;
  }
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

  if(!g_live.initialized) return;

  st = whenmoon_get_state();
  if(st == NULL) return;

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
