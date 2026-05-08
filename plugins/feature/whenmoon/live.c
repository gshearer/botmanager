// botmanager — MIT
// whenmoon real-mode trade execution skeleton (WM-LT-8-B2).
//
// The trade engine's signal entry point delegates here when the book is
// in WM_TRADE_MODE_LIVE. We apply (in order, fail-closed):
//   1. kill-switch KV (`plugin.whenmoon.exchange.<exch>.live`).
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
#include "whenmoon_strategy.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "kv.h"

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

  // Daily-loss anchor (UTC). On every signal, if the book's mark ts
  // crosses a UTC midnight relative to the anchor, we re-anchor to the
  // current cumulative realized_pnl. realized_today is the difference.
  int64_t                day_anchor_utc_ms;
  double                 day_anchor_realized_pnl;

  struct wm_live_book   *next;
} wm_live_book_t;

static struct
{
  pthread_mutex_t   mu;
  wm_live_book_t   *head;
  bool              initialized;
} g_live;

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

static bool
wm_live_kill_switch_open(const char *exchange)
{
  char path[KV_KEY_SZ * 2];

  snprintf(path, sizeof(path),
      "plugin.whenmoon.exchange.%s.live", exchange);

  // Lazy-register the bool default=false so the operator can flip it
  // via /set kv. kv_register is a no-op when the key already exists.
  if(!kv_exists(path))
    (void)kv_register(path, KV_BOOL, "false", NULL, NULL,
        "Real-trading kill switch for the named exchange. Default"
        " false: the live (real) trade path FAILs closed. Flip to"
        " true ONLY after multi-week paper coverage per"
        " feedback_paper_before_live.md.");

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

  // Gate 1: kill-switch.
  if(!wm_live_kill_switch_open(exchange))
  {
    clam(CLAM_WARN, WM_LIVE_CTX,
        "live trading disabled "
        "(plugin.whenmoon.exchange.%s.live=false)", exchange);
    return(FAIL);
  }

  // Gate 2: credentials.
  if(!coinbase_apikey_configured())
  {
    clam(CLAM_WARN, WM_LIVE_CTX,
        "live trading refused: %s credentials not configured",
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

// ----------------------------------------------------------------------- //
// External fill entry point (B2 surface, B3 wires the body)               //
// ----------------------------------------------------------------------- //

void
wm_trade_engine_record_external_fill(const char *market_id_str,
    const char *strategy_name, const wm_fill_t *fill)
{
  if(market_id_str == NULL || strategy_name == NULL || fill == NULL)
    return;

  // B3 wires the actual book update path. Logged at INFO so a future
  // trace shows the surface is reachable; the no-op body keeps B2 from
  // accidentally double-counting against the paper engine while the
  // user-channel and REST-poll consumers are still being authored.
  clam(CLAM_INFO, WM_LIVE_CTX,
      "external fill (b2 skeleton, no book update yet) "
      "%s/%s side=%c qty=%.6g px=%.4f fee=%.4f ts=%lld",
      market_id_str, strategy_name,
      fill->side, fill->qty, fill->price, fill->fee,
      (long long)fill->ts_ms);
}
