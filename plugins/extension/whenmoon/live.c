// botmanager — MIT
// whenmoon real-mode trade execution (per-market path).
//
// One path post-WM-MK-5: wm_market_engine_on_signal calls
// wm_market_engine_real_submit_locked under mk->lock when
// session.mode == WM_MARKET_MODE_REAL. That helper runs the cred +
// daily-loss + pending-cap + max-notional-clip cascade, registers a
// wm_market_pending_t in mk->session.pending[], and dispatches
// exchange_place_order_async on mk->exchange_name. The done callback
// (wm_live_market_order_done) reaps the pending row on gateway reject
// or records gateway_accepted=true on accept. Fills land asynchronously
// through the WS user-channel + REST /fills consumers
// (wm_live_handle_ws_fill / wm_live_on_fills) which route into
// wm_market_engine_record_external_fill.
//
// Operator-side halt: /whenmoon manual flips every market into MANUAL
// mode (bypassing the flat-position rule), which short-circuits the
// real-submit path on the next signal. No per-exchange enable switch
// exists — registration of the exchange (creds present, market in
// REAL mode) is the only gate beyond the per-market risk caps.
//
// User-channel WS state: g_live carries an array of per-exchange
// bindings. wm_live_ws_resub_all walks the running market set and
// reconciles one binding per distinct exchange whose credentials are
// available. Exchanges without creds are skipped (next market
// mutation binds them). OBS-41: the reconcile is a diff — a binding
// whose product set has not changed is left alone.
//
// Locking:
//   - g_live.mu protects g_live.last_fills_cursor_ms.
//   - mk->lock protects mk->session.pending[].
//   - The two locks are independent; no path takes both.

#define WHENMOON_INTERNAL
#include "live.h"

#include "market.h"
#include "market_engine.h"
#include "numeraire.h"
#include "whenmoon.h"
#include "whenmoon_strategy.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "kv.h"
#include "task.h"

#include "exchange_api.h"
#include "market_persist.h"
#include "wm_exch_query.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <time.h>

#define WM_LIVE_CTX  "whenmoon.live"

// Compile-time ceiling on the per-exchange list the boot reconciler
// walks. Mirrors the account refresher's cap.
#define WM_LIVE_MAX_EXCHANGES   8

// ----------------------------------------------------------------------- //
// Engine global state                                                     //
// ----------------------------------------------------------------------- //

// OBS-41: mirrors wm_market_ws_binding_t, including `products` — the
// set this binding actually subscribed with, which is what makes the
// reconcile a diff instead of a rebuild.
//
// ⚠ The ASYMMETRY with the market side is deliberate and load-bearing:
// the market side hands the driver the BINDING SLOT as its `user`
// pointer, so its slots may never move while a binding is live. This
// side passes `user = NULL` (see the subscribe below) and has no such
// constraint. The tombstone discipline is followed here anyway, so the
// two loops read alike — but they are not alike, and "harmonising" the
// market side onto a compacting array would silently misattribute
// ticks. Do not.
typedef struct
{
  char                 exchange_name[EXCHANGE_NAME_SZ];
  exchange_ws_sub_t   *ws_sub;
  wm_ws_product_set_t  products;
} wm_live_ws_binding_t;

static struct
{
  pthread_mutex_t      mu;

  bool                 initialized;

  // Per-exchange user-channel bindings. One slot per distinct
  // exchange in the running set with credentials configured.
  // Reconciled — not rebuilt — on every wm_live_ws_resub_all call, and
  // like the market side `n_ws_bindings` is a HIGH-WATER MARK: slots
  // are tombstoned in place, so every walk skips
  // `exchange_name[0] == '\0'`.
  wm_live_ws_binding_t ws_bindings[WM_LIVE_MAX_EXCHANGES];
  uint32_t             n_ws_bindings;
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
static void wm_live_binding_drop(wm_live_ws_binding_t *b);
static void wm_live_fills_poll_tick(task_t *t);
static void wm_live_treasury_register_kvs(void);

#define WM_LIVE_FILLS_POLL_SEC         30
#define WM_LIVE_FILLS_POLL_OVERLAP_MS  (60 * 1000)

// OBS-64: how far back a START may reach for the fills of orders that
// were already outstanding. The steady-state cursor needs no bound — it
// only ever moves forward — but a restart has to re-derive it, and an
// order resting since last week would otherwise make the first poll ask
// for a week of history against a 100-row page and push the recent fills
// off it. A day is the same bound `WM_AGG_MAX_CATCHUP_1M` takes for the
// same reason on the candle side.
#define WM_LIVE_FILLS_MAX_CATCHUP_MS   (24 * 60 * 60 * 1000LL)

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
wm_live_engine_stop(void)
{
  if(!g_live.initialized) return;

  task_cancel(g_live.fills_poll_task);
  g_live.fills_poll_task = TASK_HANDLE_NONE;
}

void
wm_live_engine_destroy(void)
{
  if(!g_live.initialized) return;

  // Idempotent; whenmoon_stop has normally already run it. The tick
  // takes g_live.mu and mkts->arr_lock, both destroyed on this path, so
  // the cancel on its own was never the barrier it read as.
  wm_live_engine_stop();

  // Drop WS subscriptions before destroying the lock — the WS reader
  // can fire callbacks on a worker thread; better to let those see
  // g_live.initialized = false (still true here, but the ws_sub
  // null-out below means no new events).
  for(uint32_t i = 0; i < g_live.n_ws_bindings; i++)
    wm_live_binding_drop(&g_live.ws_bindings[i]);

  g_live.n_ws_bindings = 0;
  g_live.ws_st         = NULL;

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

  // WM-MKT-ARR-UAF-1: hold rdlock across lookup + the mk->lock reap below
  // so a concurrent market remove cannot free the session under us.
  pthread_rwlock_rdlock(&st->markets->arr_lock);
  mk = wm_market_lookup_by_id(st, ctx->market_id_str);

  if(mk == NULL)
  {
    // Market torn down between submit and ack. The pending row went
    // with the market state; nothing to reap.
    pthread_rwlock_unlock(&st->markets->arr_lock);
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
  pthread_rwlock_unlock(&st->markets->arr_lock);
  mem_free(ctx);
}

bool
wm_market_engine_real_submit_locked(whenmoon_market_t *mk,
    char side, double qty, double mark_px, int64_t mark_ms,
    const wm_strategy_signal_t *sig, bool px_named,
    char *errbuf, size_t errbuf_sz)
{
  exchange_place_order_req_t  req;
  exchange_capabilities_t     caps;
  wm_live_market_done_ctx_t  *ctx;
  wm_market_pending_t        *pending;
  char                        why[192];
  double                      clipped_qty;
  double                      limit_px;
  int64_t                     mark_max_age_ms;
  int64_t                     mark_age_ms;
  int64_t                     book_age_ms;
  const char                 *side_str;
  bool                        is_buy;
  bool                        post_only;

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

  // Gate 1: credentials — consult the exchange capability surface.
  if(exchange_get_capabilities(mk->exchange_name, &caps) != SUCCESS
      || !caps.has_credentials)
  {
    ERRSET("no exchange credentials (%s)", mk->exchange_name);
    clam(CLAM_WARN, WM_LIVE_CTX,
        "%s real submit refused: %s credentials not configured",
        mk->market_id_str, mk->exchange_name);
    return(FAIL);
  }

  // Gate 2: daily drawdown, measured in SATOSHIS (WM-NUMERAIRE-1). Its
  // predecessor measured realized PnL in quote currency, and under a
  // bitcoin mandate that is wrong in both directions: it halted a
  // market through the bitcoin dip the office had already agreed to
  // sit through, and it stayed silent through a round trip that ended
  // holding fewer satoshis than doing nothing. The satoshi stack is
  // flat while the book holds bitcoin and moves only when it deviates,
  // which is the thing worth breaking on. Cap of 0 disables the gate;
  // the whole test lives beside the anchor it reads.
  if(wm_market_daily_drawdown_tripped_locked(mk, mark_px, wm_now_ms(),
      why, sizeof(why)))
  {
    ERRSET("%s", why);
    clam(CLAM_WARN, WM_LIVE_CTX, "%s real submit FAIL: %s",
        mk->market_id_str, why);
    return(FAIL);
  }

  // Gate 3: pending-cap.
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

  // Gate 4: per-order cap, in SATOSHIS (WM-NUMERAIRE-1), converted to a
  // quote notional at this market's own mark. A breach CLIPS the qty
  // rather than rejecting it — degraded sizing matches paper-mode
  // behaviour — but a cap that cannot be converted REFUSES, because a
  // cap the engine cannot express is not a larger one.
  clipped_qty = qty;

  if(mk->session.max_notional_sats > 0.0)
  {
    double cap_quote;

    if(!wm_numeraire_sats_to_quote(wm_numeraire_leg(mk->product_id),
        mark_px, mk->session.max_notional_sats, &cap_quote))
    {
      ERRSET("per-order cap armed (%.0f sats) but %s cannot be priced in"
          " satoshis (product %s, mark %.4f)",
          mk->session.max_notional_sats, mk->market_id_str,
          mk->product_id, mark_px);
      clam(CLAM_WARN, WM_LIVE_CTX,
          "%s real submit FAIL: max_notional_sats=%.0f but product %s"
          " has no bitcoin leg to price it against",
          mk->market_id_str, mk->session.max_notional_sats,
          mk->product_id);
      return(FAIL);
    }

    if(clipped_qty * mark_px > cap_quote)
    {
      double next = cap_quote / mark_px;

      clam(CLAM_INFO, WM_LIVE_CTX,
          "%s real submit notional clip qty %.6g -> %.6g"
          " (cap=%.0f sats = %.4f quote, mark=%.4f)",
          mk->market_id_str, clipped_qty, next,
          mk->session.max_notional_sats, cap_quote, mark_px);

      clipped_qty = next;
    }
  }

  // Gate 5: mark staleness (OBS-62). Every gate above asks a question
  // about us; this one asks whether the market being priced against is
  // still there. It measures `mk->last_feed_ms` — when a ticker or trade
  // for this product last reached us — because neither stamp travelling
  // with the price can answer: a strategy sets `mark_ms` for itself, and
  // a synthesized catch-up bar wears a current timestamp over a
  // carried-forward close. `px_named` waives the gate: the operator
  // priced that order, and refusing it would take the manual exit away
  // exactly when the feed has gone quiet.
  mark_max_age_ms = px_named ? 0 : wm_mk_mark_max_age_ms(mk);

  if(mark_max_age_ms > 0)
  {
    if(mk->last_feed_ms == 0)
    {
      ERRSET("no ticker or trade observed on %s yet", mk->market_id_str);
      clam(CLAM_WARN, WM_LIVE_CTX,
          "%s real submit FAIL: no price observed yet",
          mk->market_id_str);
      return(FAIL);
    }

    mark_age_ms = wm_now_ms() - mk->last_feed_ms;

    if(mark_age_ms > mark_max_age_ms)
    {
      ERRSET("mark stale: last price %lldms ago, bound %lldms",
          (long long)mark_age_ms, (long long)mark_max_age_ms);
      clam(CLAM_WARN, WM_LIVE_CTX,
          "%s real submit FAIL: last price %lldms ago >"
          " mark_max_age_ms %lldms",
          mk->market_id_str, (long long)mark_age_ms,
          (long long)mark_max_age_ms);
      return(FAIL);
    }
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
  // WM-MAKER-1: maker execution is per-market and off by default. Read
  // fresh, under mk->lock, for the same reason the knobs above are — the
  // per-market keys carry NULL change-callbacks, so a KV read here can
  // never re-enter market code.
  post_only = wm_mk_post_only(mk);
  limit_px  = mark_px;

  // Gate 6 (WM-MAKER-REST-1): a resting order is priced at the TOUCH.
  // Every gate above asks whether we may trade; this one asks at what
  // price a maker order can actually rest.
  //
  // `mark_px` is the last TRADE. An order posted there crosses about
  // half the time, the venue refuses it as post-only, the done callback
  // reaps the pending row, and nothing reprices — which is exactly why
  // post_only shipped disarmed. Resting on our own side of the book
  // cannot cross by construction: a buy joins the bid, a sell the ask.
  //
  // ⛔ A missing or stale book REFUSES the submit. It must never fall
  // back to `mark_px` — that silent fallback IS the defect this gate
  // removes, and it would reintroduce it precisely when the feed is
  // least trustworthy. A check that could not complete denies.
  //
  // Not chasing a rejection is deliberate, not an omission: a post-only
  // refusal means the book moved through our price, and repricing to
  // follow it is what turns a maker strategy into a taker strategy with
  // extra steps — at 120 against 60 bps that is the whole edge. If
  // rejections prove common once measured, that buys a bounded retry.
  //
  // `px_named` keeps the operator's price: they priced that order, and
  // an operator naming a limit has already chosen where it rests.
  if(post_only && !px_named)
  {
    double touch = is_buy ? mk->last_bid_px : mk->last_ask_px;

    if(mk->last_book_ms == 0 || touch <= 0.0)
    {
      ERRSET("post_only armed but no top of book seen on %s"
          " (driver publishes no ticker book?)", mk->market_id_str);
      clam(CLAM_WARN, WM_LIVE_CTX,
          "%s real submit FAIL: post_only armed, no book to rest on",
          mk->market_id_str);
      return(FAIL);
    }

    book_age_ms = wm_now_ms() - mk->last_book_ms;

    if(mark_max_age_ms > 0 && book_age_ms > mark_max_age_ms)
    {
      ERRSET("post_only armed but book stale: top of book %lldms ago,"
          " bound %lldms", (long long)book_age_ms,
          (long long)mark_max_age_ms);
      clam(CLAM_WARN, WM_LIVE_CTX,
          "%s real submit FAIL: post_only armed, book %lldms old >"
          " mark_max_age_ms %lldms",
          mk->market_id_str, (long long)book_age_ms,
          (long long)mark_max_age_ms);
      return(FAIL);
    }

    limit_px = touch;

    clam(CLAM_INFO, WM_LIVE_CTX,
        "%s resting %s at the touch %.8g (mark %.8g, book %lldms old)",
        mk->market_id_str, side_str, limit_px, mark_px,
        (long long)book_age_ms);
  }

  {
    char coid[EXCHANGE_CLIENT_OID_SZ];

    if(wm_live_uuid_v4(coid, sizeof(coid)) != SUCCESS)
    {
      ERRSET("client_oid mint failed");
      return(FAIL);
    }

    wm_live_build_place_order_req(&req, mk->product_id, side_str,
        clipped_qty, limit_px, coid, post_only);

    // Append the pending row BEFORE the async call so a synchronous-
    // FAIL done_cb finds it. The done_cb owns the post-fail reap.
    pending = &mk->session.pending[mk->session.pending_n++];
    memset(pending, 0, sizeof(*pending));

    snprintf(pending->coid, sizeof(pending->coid), "%s", coid);
    snprintf(pending->side, sizeof(pending->side), "%s", side_str);
    pending->limit_px      = limit_px;
    pending->submitted_qty = clipped_qty;
    pending->submitted_ms  = mark_ms;

    ctx = mem_alloc(WM_LIVE_CTX, "live_market_done_ctx", sizeof(*ctx));

    snprintf(ctx->market_id_str, sizeof(ctx->market_id_str), "%s",
        mk->market_id_str);
    snprintf(ctx->coid, sizeof(ctx->coid), "%s", coid);
  }

  if(exchange_place_order_async(mk->exchange_name, &req,
        wm_live_market_order_done, ctx) != ASYNC_AIRBORNE)
  {
    // The done_cb has already reaped the pending row and freed ctx.
    // Do NOT touch state.
    ERRSET("exchange_place_order_async refused the submit"
        " (done_cb already fired)");
    return(FAIL);
  }

  clam(CLAM_INFO, WM_LIVE_CTX,
      "%s submit %s qty=%.6g px=%.4f%s coid=%s",
      mk->market_id_str, side_str, clipped_qty, mark_px,
      post_only ? " post_only" : "", req.client_oid);

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
// the per-market scan. Each market's lock is taken in turn.
//
// WM-MKT-ARR-UAF-1: the CALLER MUST hold `st->markets->arr_lock` (read)
// across this call AND until it releases the `mk->lock` returned held on
// success — arr traversal and the returned session pointer are only valid
// under that rdlock.
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
    whenmoon_market_t *mk = mkts->arr[i];

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

// Same contract as the coid finder above: on match returns with mk->lock
// held. Fallback key for exchanges whose fills feed omits client_order_id
// (Coinbase REST /fills) — the accept path and WS order events both stamp
// pending[].order_id.
static bool
wm_live_find_pending_by_order_id_market_locked(const char *order_id,
    whenmoon_market_t **out_mk, uint32_t *out_idx)
{
  whenmoon_state_t   *st;
  whenmoon_markets_t *mkts;
  uint32_t            i;
  uint32_t            j;

  if(out_mk != NULL)  *out_mk  = NULL;
  if(out_idx != NULL) *out_idx = 0;

  if(order_id == NULL || order_id[0] == '\0')
    return(false);

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
    return(false);

  mkts = st->markets;

  for(i = 0; i < mkts->n_markets; i++)
  {
    whenmoon_market_t *mk = mkts->arr[i];

    pthread_mutex_lock(&mk->lock);

    for(j = 0; j < mk->session.pending_n; j++)
    {
      if(mk->session.pending[j].order_id[0] != '\0' &&
         strncmp(mk->session.pending[j].order_id, order_id,
             sizeof(mk->session.pending[j].order_id)) == 0)
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

// Stable nonzero synthetic trade id for aggregate fills booked off a WS
// order snapshot (which carries no exchange trade id). FNV-1a 64.
static int64_t
wm_live_order_id_synth_tid(const char *s)
{
  uint64_t h = 1469598103934665603ULL;

  while(*s != '\0')
  {
    h ^= (uint8_t)*s++;
    h *= 1099511628211ULL;
  }

  return(h != 0 ? (int64_t)h : 1);
}

// ----------------------------------------------------------------------- //
// User-channel event handlers                                             //
// ----------------------------------------------------------------------- //

static void
wm_live_handle_ws_fill(const exchange_ws_user_fill_t *f)
{
  whenmoon_state_t    *st;
  whenmoon_market_t   *mk = NULL;
  uint32_t             pidx = 0;
  wm_market_pending_t *p;
  uint8_t              k;
  bool                 reap = false;
  char                 market_id_copy[WM_MARKET_ID_STR_SZ];
  char                 side_ch;

  if(f == NULL || f->trade_id == 0 || f->size <= 0.0 || f->price <= 0.0)
    return;

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
    return;

  // WM-MKT-ARR-UAF-1: the finder returns with the owning market's
  // mk->lock held; hold the container rdlock from before the finder until
  // AFTER we release that mk->lock, so no concurrent remove frees it.
  pthread_rwlock_rdlock(&st->markets->arr_lock);

  if(!wm_live_find_pending_by_coid_market_locked(f->client_order_id,
         &mk, &pidx))
  {
    // Orphan: WS may have raced ahead of submit, or this fill belongs
    // to an unrelated principal (the user channel emits per-account,
    // not per-product). REST /fills poll handles late-bound dedup.
    pthread_rwlock_unlock(&st->markets->arr_lock);
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
      pthread_rwlock_unlock(&st->markets->arr_lock);
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
  // WM-MKT-ARR-UAF-1: mk no longer touched below (record_external_fill
  // re-resolves via market_id_copy under its own rdlock) — release here.
  pthread_rwlock_unlock(&st->markets->arr_lock);

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
  whenmoon_state_t    *st;
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

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
    return;

  // WM-MKT-ARR-UAF-1: hold the container rdlock from before the finder
  // (which returns with mk->lock held) until after we release mk->lock.
  pthread_rwlock_rdlock(&st->markets->arr_lock);

  if(!wm_live_find_pending_by_coid_market_locked(o->client_order_id,
         &mk, &pidx))
  {
    pthread_rwlock_unlock(&st->markets->arr_lock);
    return;
  }

  // mk->lock held here.
  p = &mk->session.pending[pidx];

  if(o->order_id[0] != '\0')
    snprintf(p->order_id, sizeof(p->order_id), "%s", o->order_id);

  if(strcmp(status, "OPEN") == 0)
    p->gateway_accepted = true;

  if(reap)
  {
    uint32_t shift;
    bool     book_agg  = false;
    int64_t  synth_tid = 0;
    char     side_ch   = 's';

    // Coinbase's user channel never emits per-fill events — this order
    // snapshot's aggregates are the only WS record of executed money.
    // Book them before the row dies: once reaped, the REST poll has no
    // row to match and the fill would be dropped as an orphan (the exact
    // failure WM-DISC-1 Part D exposed live, 2026-07-22).
    if(p->n_recorded_trades == 0 && o->cumulative_quantity > 0.0)
    {
      if(o->avg_price > 0.0)
      {
        book_agg  = true;
        synth_tid = wm_live_order_id_synth_tid(o->order_id);
        side_ch   = (o->side[0] == 'b' || o->side[0] == 'B') ? 'b' : 's';
      }

      else
      {
        // Degenerate: terminal with executed qty but no price. Keep the
        // row — the REST poll books the real fill rows via the order_id
        // fallback and reaps on filled_qty. (A partial CANCELLED that
        // never completes can park the row; visible in the card's
        // pending count.)
        pthread_mutex_unlock(&mk->lock);
        pthread_rwlock_unlock(&st->markets->arr_lock);

        clam(CLAM_WARN, WM_LIVE_CTX,
            "ws order %s status=%s cum_qty=%.10g without avg_price —"
            " row kept for REST fill recovery",
            o->order_id, status, o->cumulative_quantity);
        return;
      }
    }

    snprintf(market_id_copy, sizeof(market_id_copy), "%s",
        mk->market_id_str);

    for(shift = pidx + 1; shift < mk->session.pending_n; shift++)
      mk->session.pending[shift - 1] = mk->session.pending[shift];

    mk->session.pending_n--;

    pthread_mutex_unlock(&mk->lock);
    pthread_rwlock_unlock(&st->markets->arr_lock);

    if(book_agg)
    {
      // Advance the REST cursor so the poll doesn't chase rows this
      // aggregate already covers (the overlap window still re-reads the
      // tail; the resulting orphans dedup to a debug line).
      pthread_mutex_lock(&g_live.mu);

      if(o->time_ms > g_live.last_fills_cursor_ms)
        g_live.last_fills_cursor_ms = o->time_ms;

      pthread_mutex_unlock(&g_live.mu);

      wm_market_engine_record_external_fill(market_id_copy, synth_tid,
          side_ch, o->cumulative_quantity, o->avg_price, o->total_fees,
          o->time_ms, "ws-order-agg");
    }

    clam(failed ? CLAM_WARN : CLAM_INFO, WM_LIVE_CTX,
        "ws order %s coid=%s order_id=%s status=%s -> %s",
        market_id_copy, o->client_order_id, o->order_id, status,
        book_agg ? "aggregate booked + reaped" : "reaped");
    return;
  }

  pthread_mutex_unlock(&mk->lock);
  pthread_rwlock_unlock(&st->markets->arr_lock);
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

// The slot bound to `exch`, or NULL. Tombstones are skipped explicitly
// — see the ws_bindings comment on g_live.
static wm_live_ws_binding_t *
wm_live_binding_find(const char *exch)
{
  uint32_t i;

  for(i = 0; i < g_live.n_ws_bindings; i++)
  {
    if(g_live.ws_bindings[i].exchange_name[0] == '\0')
      continue;

    if(strncmp(g_live.ws_bindings[i].exchange_name, exch,
          EXCHANGE_NAME_SZ) == 0)
      return(&g_live.ws_bindings[i]);
  }

  return(NULL);
}

// First tombstone, else the next never-used index, else NULL (cap).
static wm_live_ws_binding_t *
wm_live_binding_alloc(void)
{
  uint32_t i;

  for(i = 0; i < g_live.n_ws_bindings; i++)
  {
    if(g_live.ws_bindings[i].exchange_name[0] == '\0')
      return(&g_live.ws_bindings[i]);
  }

  if(g_live.n_ws_bindings >= WM_LIVE_MAX_EXCHANGES)
    return(NULL);

  return(&g_live.ws_bindings[g_live.n_ws_bindings++]);
}

// Unsubscribe and tombstone in place. A no-op on a slot already empty.
static void
wm_live_binding_drop(wm_live_ws_binding_t *b)
{
  if(b->ws_sub != NULL)
    exchange_ws_unsubscribe(b->exchange_name, b->ws_sub);

  b->ws_sub           = NULL;
  b->exchange_name[0] = '\0';
  wm_ws_product_set_clear(&b->products);
}

static void
wm_live_binding_bind(wm_live_ws_binding_t *b, const char *exch,
    const char *const *pids, uint32_t n_pids)
{
  static const exchange_ws_channel_t channels[] = { EXCH_WS_USER };

  strlcpy(b->exchange_name, exch, sizeof(b->exchange_name));
  b->ws_sub = NULL;

  if(!wm_ws_product_set_record(&b->products, pids, n_pids))
    clam(CLAM_WARN, WM_LIVE_CTX,
        "%s: %u products exceeds the %u a binding remembers; "
        "every mutation will resubscribe its user channel",
        exch, (unsigned)n_pids, (unsigned)WM_WS_BINDING_MAX_PRODUCTS);

  // `user` is NULL here — unlike the market side, which passes the
  // binding slot itself. That is the whole reason this side needs no
  // slot-stability rule; see wm_live_ws_binding_t.
  if(exchange_ws_subscribe(exch, channels,
        sizeof(channels) / sizeof(channels[0]),
        pids, n_pids,
        wm_live_ws_user_event_cb, NULL,
        &b->ws_sub) != SUCCESS || b->ws_sub == NULL)
  {
    clam(CLAM_WARN, WM_LIVE_CTX,
        "user-channel WS subscribe failed (exchange=%s n_products=%u)",
        exch, (unsigned)n_pids);
    b->ws_sub = NULL;
  }
  else
  {
    clam(CLAM_INFO, WM_LIVE_CTX,
        "user-channel WS subscribed (exchange=%s n_products=%u)",
        exch, (unsigned)n_pids);
  }
}

// OBS-41: a diff, exactly like the market-side reconcile that calls it,
// and with the same `stale_exchange` meaning — NULL for a market
// mutation, otherwise the provider that just (re)registered and whose
// handles are dead regardless of what its product set says.
void
wm_live_ws_resub_all(whenmoon_state_t *st, const char *stale_exchange)
{
  whenmoon_markets_t   *m;
  const char          **pid_ptrs = NULL;
  bool                  wanted[WM_LIVE_MAX_EXCHANGES];
  uint32_t              i;
  uint32_t              j;

  if(!g_live.initialized) return;

  memset(wanted, 0, sizeof(wanted));
  g_live.ws_st = st;

  m = st != NULL ? st->markets : NULL;

  if(m != NULL && m->n_markets > 0)
  {
    // WM-MKT-ARR-UAF-1: rdlock across the whole gather+subscribe — pid_ptrs
    // alias the sessions' product_id buffers and are consumed by
    // exchange_ws_subscribe below, so no concurrent remove may free a
    // session until we are done. Recursive rdlock is safe: this is
    // sometimes reached from wm_market_resub_ws which already holds rdlock.
    pthread_rwlock_rdlock(&m->arr_lock);

    pid_ptrs = mem_alloc("whenmoon.live", "ws_pids",
        sizeof(*pid_ptrs) * m->n_markets);

    // For each distinct exchange in the running set, gather its
    // product_ids and (if creds are configured) reconcile one
    // user-channel subscription. Capacity, dedup and skip-on-no-creds
    // mirror the market-side reconcile.
    for(i = 0; i < m->n_markets; i++)
    {
      const char             *exch = m->arr[i]->exchange_name;
      exchange_capabilities_t caps;
      wm_live_ws_binding_t   *b    = wm_live_binding_find(exch);
      uint32_t                n_pids;

      if(b != NULL && wanted[(uint32_t)(b - g_live.ws_bindings)])
        continue;

      // Creds are required for the user channel; the exchange-side
      // subscribe FAILs closed when they are absent. Skip — leaving
      // `wanted` unset, so a binding this exchange still holds is
      // dropped below and no slot is created while creds are missing.
      //
      // ⚠ OBS-41 D4: that missing slot IS the creds-appeared retry, and
      // the diff must not lose it. A diff visits every exchange in the
      // running set on every mutation, and "no binding exists but one
      // is now possible" is a change like any other — so the first
      // mutation after creds appear binds it, exactly as the wholesale
      // rebuild used to.
      if(exchange_get_capabilities(exch, &caps) != SUCCESS
          || !caps.has_credentials)
      {
        clam(CLAM_DEBUG, WM_LIVE_CTX,
            "user-channel sub deferred: no creds for %s yet", exch);
        continue;
      }

      // WM-MI-1: dedup product_ids across instances — N instances of one
      // product must yield ONE user-channel subscription, not N. Without
      // this the duplicate ids scale with instance count and overflow the
      // exchange driver's fixed per-sub product array (coinbase caps at
      // CB_WS_CH_MAX_PRODUCTS_PER_SUB). Mirrors the market-side reconcile
      // in market.c:wm_market_resub_ws.
      n_pids = 0;
      for(j = i; j < m->n_markets; j++)
      {
        uint32_t k;
        bool     dup;

        if(strncmp(m->arr[j]->exchange_name, exch, EXCHANGE_NAME_SZ) != 0)
          continue;

        dup = false;
        for(k = 0; k < n_pids; k++)
        {
          if(strncmp(pid_ptrs[k], m->arr[j]->product_id,
                WM_PRODUCT_ID_SZ) == 0)
          {
            dup = true;
            break;
          }
        }

        if(!dup)
          pid_ptrs[n_pids++] = m->arr[j]->product_id;
      }

      if(b == NULL)
      {
        b = wm_live_binding_alloc();

        if(b == NULL)
        {
          clam(CLAM_WARN, WM_LIVE_CTX,
              "user-channel binding cap (%u) reached; %s skipped",
              (unsigned)WM_LIVE_MAX_EXCHANGES, exch);
          continue;
        }

        wm_live_binding_bind(b, exch, pid_ptrs, n_pids);
      }

      else if(b->ws_sub == NULL
          || (stale_exchange != NULL
              && strncmp(exch, stale_exchange, EXCHANGE_NAME_SZ) == 0)
          || wm_ws_product_set_differs(&b->products, pid_ptrs, n_pids))
      {
        wm_live_binding_drop(b);
        wm_live_binding_bind(b, exch, pid_ptrs, n_pids);
      }

      wanted[(uint32_t)(b - g_live.ws_bindings)] = true;
    }

    mem_free(pid_ptrs);
    pthread_rwlock_unlock(&m->arr_lock);
  }

  // Bindings the running set no longer wants — also the whole of the
  // "no markets at all" case.
  for(i = 0; i < g_live.n_ws_bindings; i++)
  {
    if(!wanted[i] && g_live.ws_bindings[i].exchange_name[0] != '\0')
      wm_live_binding_drop(&g_live.ws_bindings[i]);
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

  // WM-MKT-ARR-UAF-1: rdlock across the whole read-only walk so no
  // concurrent remove frees a session while we take its mk->lock.
  pthread_rwlock_rdlock(&mkts->arr_lock);

  for(i = 0; i < mkts->n_markets; i++)
  {
    whenmoon_market_t *mk = mkts->arr[i];
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
    {
      pthread_rwlock_unlock(&mkts->arr_lock);
      return(true);
    }
  }

  pthread_rwlock_unlock(&mkts->arr_lock);
  return(false);
}

static void
wm_live_on_fills(const exchange_fills_result_t *res, void *user)
{
  wm_live_fills_ctx_t *ctx = user;
  whenmoon_state_t    *st;

  if(ctx == NULL || res == NULL) goto done;

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL) goto done;

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

    // WM-MKT-ARR-UAF-1: hold the container rdlock across the finder (which
    // returns mk->lock held on match) and until we release that mk->lock.
    pthread_rwlock_rdlock(&st->markets->arr_lock);

    // coid first (exchanges that echo it), then order_id — Coinbase's
    // /fills rows omit client_order_id entirely.
    if(!wm_live_find_pending_by_coid_market_locked(f->client_oid,
           &mk, &pidx) &&
       !wm_live_find_pending_by_order_id_market_locked(f->order_id,
           &mk, &pidx))
    {
      // Orphan: WS likely already reaped this pending row. Confirm
      // dedup against every running market's recorded_trade_ids[]
      // before logging — that distinguishes "WS got there first" from
      // "fill landed for an unknown order". (seen_any_market takes its own
      // rdlock; recursive read-lock is safe.)
      if(!wm_live_trade_id_seen_any_market(f->trade_id))
        clam(CLAM_DEBUG, WM_LIVE_CTX,
            "fills poll: orphan tid=%lld coid=%s product=%s",
            (long long)f->trade_id, f->client_oid, f->product_id);
      pthread_rwlock_unlock(&st->markets->arr_lock);
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
      pthread_rwlock_unlock(&st->markets->arr_lock);
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
    // WM-MKT-ARR-UAF-1: mk no longer touched below — release rdlock.
    pthread_rwlock_unlock(&st->markets->arr_lock);

    // Advance the global REST cursor.
    pthread_mutex_lock(&g_live.mu);

    if(f->time_ms > g_live.last_fills_cursor_ms)
      g_live.last_fills_cursor_ms = f->time_ms;

    pthread_mutex_unlock(&g_live.mu);

    side_ch = (f->side[0] == 'b' || f->side[0] == 'B') ? 'b' : 's';

    clam(CLAM_INFO, WM_LIVE_CTX,
        "fills poll: applied tid=%lld coid=%s order_id=%s",
        (long long)f->trade_id, f->client_oid, f->order_id);

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

  // WM-MKT-ARR-UAF-1: rdlock across the whole walk — sessions read here
  // must not be freed by a concurrent remove. (on_fills, fired below,
  // takes its own rdlock; recursive read-lock is safe.)
  pthread_rwlock_rdlock(&mkts->arr_lock);

  for(uint32_t i = 0; i < mkts->n_markets; i++)
  {
    wm_live_fills_ctx_t    *ctx;
    exchange_capabilities_t caps;

    if(mkts->arr[i]->exchange_name[0] == '\0')
      continue;

    // Only real-mode markets have live fills to reconcile. Paper /
    // manual markets must never touch the authenticated account —
    // doing so spams the exchange with reads against an account that
    // isn't trading, which reads as anomalous to their fraud tooling.
    if(mkts->arr[i]->session.mode != WM_MARKET_MODE_REAL)
      continue;

    // Skip when creds are not configured for this exchange.
    if(exchange_get_capabilities(mkts->arr[i]->exchange_name,
           &caps) != SUCCESS || !caps.has_credentials)
      continue;

    ctx = mem_alloc(WM_LIVE_CTX, "fills_ctx", sizeof(*ctx));

    snprintf(ctx->market_id_str, sizeof(ctx->market_id_str), "%s",
        mkts->arr[i]->market_id_str);
    snprintf(ctx->exchange_name, sizeof(ctx->exchange_name), "%s",
        mkts->arr[i]->exchange_name);
    snprintf(ctx->product_id, sizeof(ctx->product_id), "%s",
        mkts->arr[i]->product_id);

    // Nothing to check: a refusal is ASYNC_FAILED_DELIVERED, so
    // wm_live_on_fills has already run with res->err set and freed ctx.
    (void)exchange_list_fills_async(ctx->exchange_name, NULL,
        ctx->product_id, cursor_ms, wm_live_on_fills, ctx);
  }

  pthread_rwlock_unlock(&mkts->arr_lock);
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

// OBS-64: the submit time of the oldest pending row anywhere in the
// running set, or 0 when nothing is outstanding. Called from the start
// hook after wm_market_persist_restore_all, so these are the rows that
// SURVIVED whatever took the daemon away — the window the fills poll has
// to be able to reach back into.
static int64_t
wm_live_oldest_pending_ms(whenmoon_state_t *st, uint32_t *out_n)
{
  int64_t  oldest = 0;
  uint32_t total  = 0;
  uint32_t i;

  if(st == NULL || st->markets == NULL)
    return(0);

  pthread_rwlock_rdlock(&st->markets->arr_lock);

  for(i = 0; i < st->markets->n_markets; i++)
  {
    whenmoon_market_t *mk = st->markets->arr[i];
    uint32_t           k;

    pthread_mutex_lock(&mk->lock);

    for(k = 0; k < mk->session.pending_n; k++)
    {
      int64_t ms = mk->session.pending[k].submitted_ms;

      total++;

      if(ms > 0 && (oldest == 0 || ms < oldest))
        oldest = ms;
    }

    pthread_mutex_unlock(&mk->lock);
  }

  pthread_rwlock_unlock(&st->markets->arr_lock);

  if(out_n != NULL)
    *out_n = total;

  return(oldest);
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
  int64_t           oldest_ms;
  int64_t           seed_ms;
  char              names[WM_LIVE_MAX_EXCHANGES][EXCHANGE_NAME_SZ];
  uint32_t          n_names   = 0;
  uint32_t          n_pending = 0;
  uint32_t          i;

  if(!g_live.initialized) return;

  st = whenmoon_get_state();
  if(st == NULL) return;

  // WM-NUMERAIRE-1: surface the treasury knobs to /set kv
  // before any fill can consult them.
  wm_live_treasury_register_kvs();

  if(exchange_name_list(names, WM_LIVE_MAX_EXCHANGES, &n_names) != SUCCESS)
    n_names = 0;

  if(n_names > WM_LIVE_MAX_EXCHANGES)
    n_names = WM_LIVE_MAX_EXCHANGES;

  // Seed the global REST-poll cursor at "now - overlap" so the first
  // poll fetches the recent past. Without this seed the first poll
  // pulls every fill the server is willing to page back (hundreds of
  // potentially stale rows), and the orphan dedup walk would log them
  // all.
  //
  // OBS-64: but "the recent past" is the wrong window when orders were
  // already outstanding. In steady state the cursor is a high-water mark
  // over applied fills and covers an outage of any length; it lives in
  // this mapping, though, so a start — or a `/plugin reload whenmoon` —
  // re-derives it, and nothing else recovers a fill that landed while we
  // were gone: the boot reconcile below lists only OPEN orders, and a
  // fill whose pending row it cannot match is dropped as an orphan. So
  // the floor is the oldest restored row's `submitted_ms`, bounded by
  // WM_LIVE_FILLS_MAX_CATCHUP_MS.
  now_ms    = wm_now_ms();
  oldest_ms = wm_live_oldest_pending_ms(st, &n_pending);
  seed_ms   = now_ms > WM_LIVE_FILLS_POLL_OVERLAP_MS
      ? now_ms - WM_LIVE_FILLS_POLL_OVERLAP_MS
      : 0;

  if(oldest_ms > 0)
  {
    int64_t floor_ms = oldest_ms > WM_LIVE_FILLS_POLL_OVERLAP_MS
        ? oldest_ms - WM_LIVE_FILLS_POLL_OVERLAP_MS
        : 0;
    int64_t bound_ms = now_ms > WM_LIVE_FILLS_MAX_CATCHUP_MS
        ? now_ms - WM_LIVE_FILLS_MAX_CATCHUP_MS
        : 0;
    char    age[32];
    char    age2[32];

    if(floor_ms < bound_ms)
      floor_ms = bound_ms;

    if(floor_ms < seed_ms)
      seed_ms = floor_ms;

    clam(CLAM_INFO, WM_LIVE_CTX,
        "%u pending order(s) restored, oldest submitted %s ago:"
        " fills-poll will re-read from %s back",
        n_pending, wm_fmt_age(now_ms - oldest_ms, age, sizeof(age)),
        wm_fmt_age(now_ms - seed_ms, age2, sizeof(age2)));
  }

  pthread_mutex_lock(&g_live.mu);

  if(g_live.last_fills_cursor_ms == 0)
    g_live.last_fills_cursor_ms = seed_ms;

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

    // Skip exchanges with no real-mode market — an authenticated
    // open-orders listing against a paper-only account is needless
    // live-account traffic.
    if(!wm_market_exchange_has_real_mode(st, names[i]))
      continue;

    snprintf(wm_live_reconcile_names[i],
        sizeof(wm_live_reconcile_names[i]), "%.*s",
        (int)(sizeof(wm_live_reconcile_names[i]) - 1), names[i]);

    if(exchange_list_orders_async(names[i], "OPEN", NULL,
           wm_live_boot_reconcile_cb,
           wm_live_reconcile_names[i]) != ASYNC_AIRBORNE)
      clam(CLAM_DEBUG, WM_LIVE_CTX,
          "boot reconcile submit failed (%s)", names[i]);
  }
}

// ----------------------------------------------------------------------- //
// WM-REAL-CASH-1: real-mode cash reconciliation                           //
//                                                                         //
// The market session carries a per-mode cash ledger. Paper mode is a pure //
// simulation seeded at WM_MARKET_DEFAULT_STARTING_CASH; real mode must    //
// instead deploy a fraction of the ACTUAL quote-currency balance on the   //
// bound exchange, or order sizing (size_frac * cash) bets a fictional     //
// bankroll. This helper pulls the live `available` quote balance into the //
// real ledger; the synchronous fetch confines it to the command thread.   //
// ----------------------------------------------------------------------- //

static void
wm_reconcile_on_accounts(const exchange_accounts_result_t *res, void *user)
{
  if(res != NULL)
  {
    wm_sync_fetch_complete(user, res, sizeof(*res));
    return;
  }

  // A NULL result becomes a typed error so the waiter's copy-out carries a
  // message rather than a zeroed "ok, 0 currencies" (cf. wm_bal_on_accounts).
  {
    exchange_accounts_result_t err;

    memset(&err, 0, sizeof(err));
    snprintf(err.err, sizeof(err.err), "no result delivered");
    wm_sync_fetch_complete(user, &err, sizeof(err));
  }
}

// Resolve the quote-currency available balance for `market_id` from an
// accounts snapshot. Case-insensitive: ids are "...-usd" while exchanges
// report "USD". Returns true + *out_avail on a match.
static bool
wm_live_quote_available(const char *market_id,
    const exchange_account_t *rows, uint32_t n, double *out_avail)
{
  char     exch[EXCHANGE_NAME_SZ];
  char     base[16];
  char     quote[16];
  uint32_t i;

  if(market_id == NULL || rows == NULL)
    return(false);

  if(wm_market_parse_id(market_id, exch, sizeof(exch), base, sizeof(base),
         quote, sizeof(quote)) != SUCCESS)
    return(false);

  for(i = 0; i < n; i++)
    if(strcasecmp(rows[i].currency, quote) == 0)
    {
      if(out_avail != NULL)
        *out_avail = rows[i].available;
      return(true);
    }

  return(false);
}

// WM-QUOTE-ALLOC-1: bound `avail` quote units by the per-market
// allocation knobs so N real markets sharing one quote currency don't
// each claim the full balance and collectively over-deploy.
// quote_alloc_frac (default 1.0 = whole balance) and quote_alloc_max
// (default 0.0 = uncapped) compose — most-restrictive wins. Knobs are
// read FRESH here (not the session-cached params, which only refresh at
// market start) so an operator `/set kv …quote_alloc_frac` takes effect
// on the very next reconcile. Reading KV under mk->lock is safe:
// whenmoon registers every per-market KV with NULL change-callbacks, so
// kv ops never re-enter market code (mk->lock is a per-market leaf
// lock; there is no kv->mk lock-ordering path), and reconcile is
// infrequent. Split from wm_live_apply_real_cash_locked so the
// WM-DISC-1 over-subscription audit can price a market's claim without
// applying it. Caller holds mk->lock.
static double
wm_live_quote_alloc_bound_locked(whenmoon_market_t *mk, double avail)
{
  double frac;
  double max;
  double capped = avail;

  frac = wm_mk_kv_get_double(mk->market_id_str, "quote_alloc_frac",
      "1.0", WM_MARKET_DEFAULT_QUOTE_ALLOC_FRAC,
      "Real-mode quote-balance allocation cap: fraction of the exchange"
      " 'available' quote balance this market may deploy (1.0 = whole"
      " balance). Bounds the cash ledger (bankroll); size_frac then sizes"
      " each order off the capped cash and max_notional_sats caps"
      " per-order notional. Composes with quote_alloc_max — most-restrictive wins.");

  max = wm_mk_kv_get_double(mk->market_id_str, "quote_alloc_max",
      "0.0", WM_MARKET_DEFAULT_QUOTE_ALLOC_MAX,
      "Real-mode quote-balance allocation cap: absolute ceiling in quote"
      " currency this market may deploy (0 = uncapped). Composes with"
      " quote_alloc_frac — most-restrictive wins.");

  if(frac >= 0.0 && frac < 1.0 && (avail * frac) < capped)
    capped = avail * frac;

  if(max > 0.0 && max < capped)
    capped = max;

  return(capped);
}

// Bind the real-mode cash ledger to the deployable portion of `avail`,
// returning the value actually applied (after the WM-QUOTE-ALLOC-1 cap).
// Caller holds mk->lock. `reset_baseline` (true for a deliberate operator
// reconcile) also re-anchors starting_cash, the daily-loss-cap denominator;
// the periodic auto-reconcile passes false so it only tracks deployable
// `cash` and leaves an established risk baseline alone — but the very first
// sync always anchors it (off real funds, not the $10k placeholder). The
// capped value feeds BOTH cash and starting_cash, so the daily-loss cap is
// relative to *allocated* capital.
static double
wm_live_apply_real_cash_locked(whenmoon_market_t *mk, double avail,
    bool reset_baseline)
{
  wm_market_stats_t *rs     = &mk->session.stats[WM_MARKET_MODE_REAL];
  double             capped = wm_live_quote_alloc_bound_locked(mk, avail);

  rs->cash = capped;

  if(reset_baseline || mk->real_cash_synced_ms == 0)
    rs->starting_cash = capped;

  mk->real_cash_synced_ms = wm_now_ms();
  (void)wm_market_persist_locked(mk);

  return(capped);
}

bool
wm_market_reconcile_real_cash(whenmoon_market_t *mk, double *out_cash,
    char *errbuf, size_t errbuf_sz)
{
  exchange_capabilities_t     caps;
  exchange_accounts_result_t  res;
  wm_sync_fetch_t            *w;
  char                        exch[EXCHANGE_NAME_SZ];
  char                        base[16];
  char                        quote[16];
  double                      avail   = -1.0;
  double                      applied = -1.0;

  #define RC_ERR(...) do { \
      if(errbuf != NULL && errbuf_sz > 0) \
        snprintf(errbuf, errbuf_sz, __VA_ARGS__); \
    } while(0)

  if(errbuf != NULL && errbuf_sz > 0)
    errbuf[0] = '\0';

  if(mk == NULL)
  {
    RC_ERR("null market");
    return(FAIL);
  }

  if(mk->exchange_name[0] == '\0')
  {
    RC_ERR("market %s has no bound exchange", mk->market_id_str);
    return(FAIL);
  }

  if(wm_market_parse_id(mk->market_id_str, exch, sizeof(exch),
         base, sizeof(base), quote, sizeof(quote)) != SUCCESS)
  {
    RC_ERR("cannot parse quote currency from %s", mk->market_id_str);
    return(FAIL);
  }

  // Gate on credentials up front — same check the real submit path makes,
  // and the reason a paper-only key can't reconcile.
  if(exchange_get_capabilities(mk->exchange_name, &caps) != SUCCESS
      || !caps.has_credentials)
  {
    RC_ERR("no credentials for %s", mk->exchange_name);
    return(FAIL);
  }

  // Synchronous bridge over the async accounts fetch. The handle owns the
  // result buffer, so a late callback after a timed-out wait cannot
  // use-after-free our stack (cf. finding_gemini_prime_use_after_free).
  w = wm_sync_fetch_begin(sizeof(res));

  (void)exchange_get_accounts_async(mk->exchange_name,
      wm_reconcile_on_accounts, w);

  if(!wm_sync_fetch_wait(w, &res, sizeof(res), WM_EXCH_QUERY_WAIT_MS))
  {
    RC_ERR("account fetch timed out (%s)", mk->exchange_name);
    return(FAIL);
  }

  if(res.err[0] != '\0')
  {
    RC_ERR("%s", res.err);
    return(FAIL);
  }

  if(!wm_live_quote_available(mk->market_id_str, res.rows, res.count,
         &avail))
  {
    RC_ERR("quote currency %s not held on %s", quote, mk->exchange_name);
    return(FAIL);
  }

  // Operator-initiated reconcile (mode->real switch or /whenmoon market
  // sync): re-anchor the full baseline. The applied (deployable) cash is
  // the WM-QUOTE-ALLOC-1-capped value, which is what the operator's
  // bankroll actually becomes — report that, not the raw balance.
  pthread_mutex_lock(&mk->lock);
  applied = wm_live_apply_real_cash_locked(mk, avail, true);
  pthread_mutex_unlock(&mk->lock);

  if(out_cash != NULL)
    *out_cash = applied;

  if(applied < avail)
    clam(CLAM_INFO, WM_LIVE_CTX,
        "market %s real cash reconciled: %s available=%.2f"
        " deployable=%.2f (capped by quote_alloc)",
        mk->market_id_str, quote, avail, applied);
  else
    clam(CLAM_INFO, WM_LIVE_CTX,
        "market %s real cash reconciled: %s available=%.2f",
        mk->market_id_str, quote, avail);

  #undef RC_ERR

  return(SUCCESS);
}

// WM-DISC-1 A1: per-quote-currency accumulator for the reconcile
// walk's over-subscription audit — one slot per distinct quote currency
// seen among the exchange's real-mode markets.
#define WM_LIVE_AUDIT_QUOTES 8

typedef struct
{
  char     quote[16];
  double   avail;
  double   sum;
  uint32_t n;
  char     detail[512];
  size_t   detail_len;
} wm_live_alloc_audit_t;

// Caller holds mk->lock (the bound helper reads KVs under it). Slots
// past the compile cap are dropped silently — 8 distinct quote
// currencies on one exchange exceeds anything we deploy.
static void
wm_live_alloc_audit_add(wm_live_alloc_audit_t *audit, uint32_t *n_audit,
    whenmoon_market_t *mk, double avail)
{
  wm_live_alloc_audit_t *slot = NULL;
  char                   exch[EXCHANGE_NAME_SZ];
  char                   base[16];
  char                   quote[16];
  double                 bound;
  uint32_t               i;

  if(wm_market_parse_id(mk->market_id_str, exch, sizeof(exch),
         base, sizeof(base), quote, sizeof(quote)) != SUCCESS)
    return;

  for(i = 0; i < *n_audit; i++)
  {
    if(strncmp(audit[i].quote, quote, sizeof(quote)) == 0)
    {
      slot = &audit[i];
      break;
    }
  }

  if(slot == NULL)
  {
    if(*n_audit >= WM_LIVE_AUDIT_QUOTES)
      return;

    slot = &audit[(*n_audit)++];
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->quote, sizeof(slot->quote), "%s", quote);
    slot->avail = avail;
  }

  bound = wm_live_quote_alloc_bound_locked(mk, avail);

  slot->sum += bound;
  slot->n++;

  if(slot->detail_len < sizeof(slot->detail))
  {
    int len = snprintf(slot->detail + slot->detail_len,
        sizeof(slot->detail) - slot->detail_len,
        " %s=%.2f", mk->market_id_str, bound);

    if(len > 0)
      slot->detail_len += (size_t)len;

    if(slot->detail_len > sizeof(slot->detail))
      slot->detail_len = sizeof(slot->detail);
  }
}

void
wm_live_reconcile_from_accounts(const char *exchange,
    const exchange_account_t *rows, uint32_t n)
{
  whenmoon_state_t      *st;
  whenmoon_markets_t    *mkts;
  wm_live_alloc_audit_t  audit[WM_LIVE_AUDIT_QUOTES];
  uint32_t               n_audit = 0;
  uint32_t               i;

  if(exchange == NULL || exchange[0] == '\0' || rows == NULL)
    return;

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
    return;

  mkts = st->markets;

  // WM-MKT-ARR-UAF-1: rdlock across the whole walk so no concurrent remove
  // frees a session while we take its mk->lock and reconcile it.
  pthread_rwlock_rdlock(&mkts->arr_lock);

  for(i = 0; i < mkts->n_markets; i++)
  {
    whenmoon_market_t *mk = mkts->arr[i];
    double             avail = 0.0;

    if(strncmp(mk->exchange_name, exchange, EXCHANGE_NAME_SZ) != 0)
      continue;

    if(!wm_live_quote_available(mk->market_id_str, rows, n, &avail))
      continue;

    pthread_mutex_lock(&mk->lock);

    // WM-DISC-1 A1: tally this real-mode market's allocation claim so
    // the post-walk audit below can flag quote-currency
    // over-subscription. Position state is irrelevant here — the claim
    // is configuration, not deployment.
    if(mk->session.mode == WM_MARKET_MODE_REAL)
      wm_live_alloc_audit_add(audit, &n_audit, mk, avail);

    // Reconcile only when flat. An open long means part of the capital
    // sits in the base asset, so the quote `available` understates the
    // market's deployable cash; the market's own fill ledger is the
    // source of truth until it closes flat, when the next snapshot
    // re-syncs. reset_baseline=false so this never moves an established
    // daily-loss baseline (only the first sync anchors it).
    if(mk->session.position.side == WM_MARKET_POS_FLAT)
    {
      double applied = wm_live_apply_real_cash_locked(mk, avail, false);

      clam(CLAM_DEBUG2, WM_LIVE_CTX,
          "market %s real cash auto-reconciled: available=%.2f"
          " deployable=%.2f",
          mk->market_id_str, avail, applied);
    }

    pthread_mutex_unlock(&mk->lock);
  }

  pthread_rwlock_unlock(&mkts->arr_lock);

  // WM-DISC-1 A1: warn — never refuse; refusing mid-reconcile could
  // strand live positions — when the real-mode markets sharing a quote
  // currency are collectively promised more than the balance holds.
  // A single market's bound can never exceed `avail` by construction,
  // so only multi-market sums can over-subscribe. The operator fixes
  // the quote_alloc KVs.
  for(i = 0; i < n_audit; i++)
  {
    wm_live_alloc_audit_t *a = &audit[i];

    if(a->n >= 2 && a->sum > a->avail)
      clam(CLAM_WARN, WM_LIVE_CTX,
          "quote over-subscription on %s %s: allocation bounds sum %.2f"
          " > available %.2f across %u real market(s):%s",
          exchange, a->quote, a->sum, a->avail, a->n, a->detail);
  }
}

// ----------------------------------------------------------------------- //
// WM-NUMERAIRE-1: treasury freeze tripwire (CFO.md sec. 3)                //
// ----------------------------------------------------------------------- //

// All three knobs default inert; the tripwire arms only when every one
// is set. Registered at engine start so /set kv finds them before the
// first fill; read FRESH at each real fill, breaker-style.
//
// This was WM-DISC-1's `disc.*` namespace, and it measured the fund's
// equity in dollars against a dollar deposit. Under the 2026-08-28
// mandate that is a machine for doing the one thing CFO.md sec. 2
// forbids: a bitcoin dip would drop the dollar equity through the
// floor and flip every market to manual, on a move the operator had
// already accepted unconditionally. The floor is now the office's
// opening SATOSHI stack, which a bitcoin move does not touch — the
// tripwire fires when the fund's deviations lost satoshis, and only
// then. The two-treasury split the old name described is gone with the
// same amendment; there is one treasury.
#define WM_TREASURY_KV_BASELINE  "plugin.whenmoon.treasury.baseline_sats"
#define WM_TREASURY_KV_FRAC      "plugin.whenmoon.treasury.freeze_frac"
#define WM_TREASURY_KV_MARKETS   "plugin.whenmoon.treasury.markets"

// Compile cap on designated markets + list-KV working buffer.
#define WM_TREASURY_MAX_MARKETS  16
#define WM_TREASURY_LIST_BUF_SZ  (WM_TREASURY_MAX_MARKETS * WM_MARKET_ID_STR_SZ)

static void
wm_live_treasury_register_kvs(void)
{
  if(!kv_exists(WM_TREASURY_KV_BASELINE) &&
     kv_register(WM_TREASURY_KV_BASELINE, KV_DOUBLE, "0.0", NULL, NULL,
         "Treasury baseline in SATOSHIS (whenmoon CFO.md sec. 1): the"
         " stack the freeze floor is measured against. 0 = unconfigured."
         " The freeze tripwire arms only when baseline_sats, freeze_frac"
         " and markets are all set.") != SUCCESS)
    clam(CLAM_WARN, WM_LIVE_CTX, "kv_register failed: %s",
        WM_TREASURY_KV_BASELINE);

  if(!kv_exists(WM_TREASURY_KV_FRAC) &&
     kv_register(WM_TREASURY_KV_FRAC, KV_DOUBLE, "0.0", NULL, NULL,
         "Treasury freeze fraction: a summed stack below baseline_sats *"
         " (1 - freeze_frac) after a fill on a designated market flips"
         " every designated market to MANUAL (positions kept) and emits"
         " one TREASURY-FREEZE warn. Measured in satoshis, so holding"
         " bitcoin through a price move cannot trip it. 0 disables.")
         != SUCCESS)
    clam(CLAM_WARN, WM_LIVE_CTX, "kv_register failed: %s",
        WM_TREASURY_KV_FRAC);

  if(!kv_exists(WM_TREASURY_KV_MARKETS) &&
     kv_register(WM_TREASURY_KV_MARKETS, KV_STR, "", NULL, NULL,
         "Treasury designated markets: comma-separated market_id_str"
         " list, exact match, no whitespace. The treasury trades ONLY"
         " through these; the freeze tripwire sums each market's stack"
         " in satoshis (paper-mode markets read their paper book, all"
         " others the real book). A designated market with no bitcoin"
         " leg cannot be summed and is skipped with a warn.") != SUCCESS)
    clam(CLAM_WARN, WM_LIVE_CTX, "kv_register failed: %s",
        WM_TREASURY_KV_MARKETS);
}

// Exact-match membership test against the comma-separated designated-
// market list. No whitespace tolerance — the KV help states the format.
static bool
wm_treasury_market_listed(const char *list, const char *market_id_str)
{
  const char *p    = list;
  size_t      want = strlen(market_id_str);

  while(*p != '\0')
  {
    const char *comma = strchr(p, ',');
    size_t      len   = (comma != NULL) ? (size_t)(comma - p) : strlen(p);

    if(len == want && strncmp(p, market_id_str, want) == 0)
      return(true);

    if(comma == NULL)
      break;

    p = comma + 1;
  }

  return(false);
}

// Evaluated after the treasury's only two order paths — real exchange
// fills (wm_market_engine_record_external_fill) and operator force
// trades in synth modes (the /whenmoon market force verb) — always
// AFTER the fill's locks are released. Strategy-driven paper fills are
// deliberately not hooked: strategies never trade the treasury.
// Mirrors the WM-BREAKER-1 shape at fund scope: sum the designated
// markets' stacks in satoshis (paper-mode markets read their paper
// book so a rehearsal can drill the tripwire; REAL and frozen MANUAL
// markets read the real book), breach -> every designated market flips
// MANUAL.
//
// Locking: the walk takes one mk->lock at a time under the arr rdlock,
// never two — two designated markets filling concurrently must not
// ABBA-deadlock. The summed stack is therefore a near-instant
// composite, not an atomic snapshot: breaker-grade arithmetic, not
// accounting. Each market is marked at its own freshest mark; for the
// just-filled market that IS the fill px (apply_fill_locked updates
// last_mark_px before we run). A designated market that is not
// running, or whose product has no bitcoin leg to price, contributes
// nothing — conservative by construction, since invisible capital
// leans the tripwire toward freezing. That is why the skip warns.
void
wm_live_treasury_freeze_check(const char *filled_market_id_str)
{
  whenmoon_state_t  *st;
  whenmoon_market_t *fund[WM_TREASURY_MAX_MARKETS];
  char               list[WM_TREASURY_LIST_BUF_SZ];
  char              *tok;
  char              *save = NULL;
  const char        *val;
  double             baseline;
  double             frac;
  double             floor_sats;
  double             stack     = 0.0;
  uint32_t           n_fund    = 0;
  uint32_t           n_paper   = 0;
  uint32_t           n_real    = 0;
  uint32_t           n_skipped = 0;
  uint32_t           flipped   = 0;
  uint32_t           i;

  if(filled_market_id_str == NULL)
    return;

  baseline = kv_get_double(WM_TREASURY_KV_BASELINE);
  frac     = kv_get_double(WM_TREASURY_KV_FRAC);

  if(baseline <= 0.0 || frac <= 0.0)
    return;

  val = kv_get_str(WM_TREASURY_KV_MARKETS);

  if(val == NULL || val[0] == '\0')
    return;

  snprintf(list, sizeof(list), "%s", val);

  if(!wm_treasury_market_listed(list, filled_market_id_str))
    return;

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
    return;

  floor_sats = baseline * (1.0 - frac);

  pthread_rwlock_rdlock(&st->markets->arr_lock);

  for(tok = strtok_r(list, ",", &save);
      tok != NULL && n_fund < WM_TREASURY_MAX_MARKETS;
      tok = strtok_r(NULL, ",", &save))
  {
    whenmoon_market_t *mk = wm_market_lookup_by_id(st, tok);

    if(mk == NULL)
      continue;

    pthread_mutex_lock(&mk->lock);

    {
      // Book selection: a PAPER-mode designated market contributes its
      // paper book (a rehearsal fund is all-paper); everything else —
      // REAL, and MANUAL after a freeze — contributes the real book. A
      // fund must never mix books: paper cash in a real fund masks a
      // real breach, hence the warn below.
      wm_market_mode_t book =
          (mk->session.mode == WM_MARKET_MODE_PAPER)
              ? WM_MARKET_MODE_PAPER : WM_MARKET_MODE_REAL;
      double mk_sats;

      if(wm_market_stack_sats_locked(mk, book,
          wm_market_valuation_px_locked(mk), &mk_sats))
      {
        stack += mk_sats;

        if(book == WM_MARKET_MODE_PAPER)
          n_paper++;
        else
          n_real++;
      }

      else
      {
        n_skipped++;
        clam(CLAM_WARN, WM_LIVE_CTX,
            "treasury: designated market %s (product %s, mark %.4f) has"
            " no satoshi price and is omitted from the fund stack — an"
            " omission leans the tripwire toward freezing; fix %s",
            mk->market_id_str, mk->product_id,
            wm_market_valuation_px_locked(mk), WM_TREASURY_KV_MARKETS);
      }
    }

    pthread_mutex_unlock(&mk->lock);
    fund[n_fund++] = mk;
  }

  if(n_paper > 0 && n_real > 0)
    clam(CLAM_WARN, WM_LIVE_CTX,
        "treasury fund mixes books: %u paper-mode + %u real/manual"
        " designated market(s) — paper cash inflates the fund stack;"
        " fix %s", n_paper, n_real, WM_TREASURY_KV_MARKETS);

  if(stack >= floor_sats)
  {
    pthread_rwlock_unlock(&st->markets->arr_lock);
    return;
  }

  // Trip: flip every designated market to MANUAL, position kept — the
  // WM-BREAKER-1 semantics. MANUAL blocks strategy auto-trades but not
  // manual orders; enforcing "closes only, no opens" during a freeze is
  // CFO discipline pending operator review (CFO.md sec. 3). Alert once
  // per trip: if nothing flipped, an earlier fill already announced
  // this freeze (in-flight orders can still fill after the flip).
  for(i = 0; i < n_fund; i++)
  {
    whenmoon_market_t *mk = fund[i];

    pthread_mutex_lock(&mk->lock);

    if(mk->session.mode != WM_MARKET_MODE_MANUAL)
    {
      mk->session.mode = WM_MARKET_MODE_MANUAL;
      (void)wm_market_persist_locked(mk);
      flipped++;
    }

    pthread_mutex_unlock(&mk->lock);
  }

  pthread_rwlock_unlock(&st->markets->arr_lock);

  if(flipped > 0)
    clam(CLAM_WARN, WM_LIVE_CTX,
        "TREASURY-FREEZE: fund stack %.0f sats breached floor %.0f"
        " (baseline %.0f, freeze_frac %.4g, %u market(s) omitted) — %u"
        " designated market(s) -> manual, positions kept, pending"
        " operator review (tripping fill: %s)",
        stack, floor_sats, baseline, frac, n_skipped, flipped,
        filled_market_id_str);
}
