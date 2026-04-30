// botmanager — MIT
// Whenmoon trade-book registry + paper-mode fill engine (WM-LT-4).
//
// One registry per process. Each registered book is keyed by
// (market_id_str, strategy_name); books are created lazily by the
// /whenmoon trade … verbs (and by direct refresh-from-KV calls during
// attach when the strategy declares a non-OFF default mode).
//
// Live + paper share this same book. Backtest (WM-LT-5) will run
// snapshot iterations against private books that never enter the
// registry, so the live registry stays small (one per attached
// strategy/market pair).
//
// Locking: a single registry mutex serialises all reads + writes
// (rationale in order.h). The signal entry point holds the registry
// lock end-to-end; the work inside is bounded (sizer + arithmetic).

#define WHENMOON_INTERNAL
#include "order.h"

#include "backtest.h"
#include "market.h"
#include "pnl.h"
#include "sizer.h"
#include "strategy.h"
#include "trade_persist.h"
#include "whenmoon.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "db.h"

#include <inttypes.h>
#include <json-c/json.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WM_TRADE_CTX        "whenmoon.trade"

// KV defaults — used when no global / per-attachment override exists.
// Picked to be safe-ish for paper testing: 0.40% fee matches Coinbase
// retail spot, 5 bps slippage is conservative for liquid pairs at
// retail size, $10k starting cash is a tractable demo balance.
#define WM_TRADE_DEF_FEE_BPS         40.0
#define WM_TRADE_DEF_SLIP_BPS        5.0
#define WM_TRADE_DEF_SIZE_FRAC       0.10
#define WM_TRADE_DEF_MAX_POSITION    0.0     // 0 = no cap
#define WM_TRADE_DEF_STARTING_CASH   10000.0

// ----------------------------------------------------------------------- //
// Registry — global singleton + TLS active-registry plumbing              //
// ----------------------------------------------------------------------- //
//
// Every public book API resolves the calling thread's "active" registry
// via wm_trade_get_active_registry. Threads that have never bound a
// private registry (live + paper paths, cmd workers, etc.) see the
// global. Backtest iterations (WM-LT-6) bind a freshly-created private
// registry via wm_trade_engine_use_registry; subsequent ops on that
// thread, including signal emission from inside a strategy callback,
// route to the private book without touching the global mutex.

static wm_trade_registry_t g_registry;
static pthread_key_t       g_bt_registry_key;
static bool                g_bt_registry_key_inited = false;

static wm_trade_registry_t *
wm_trade_get_active_registry(void)
{
  wm_trade_registry_t *r;

  if(!g_registry.initialized)
    return(NULL);

  if(!g_bt_registry_key_inited)
    return(&g_registry);

  r = (wm_trade_registry_t *)pthread_getspecific(g_bt_registry_key);
  return(r != NULL ? r : &g_registry);
}

// ----------------------------------------------------------------------- //
// Mode helpers                                                            //
// ----------------------------------------------------------------------- //

bool
wm_trade_mode_parse(const char *tok, wm_trade_mode_t *out)
{
  if(tok == NULL || out == NULL)
    return(FAIL);

  if(strcasecmp(tok, "off")      == 0) { *out = WM_TRADE_MODE_OFF;      return(SUCCESS); }
  if(strcasecmp(tok, "paper")    == 0) { *out = WM_TRADE_MODE_PAPER;    return(SUCCESS); }
  if(strcasecmp(tok, "backtest") == 0) { *out = WM_TRADE_MODE_BACKTEST; return(SUCCESS); }
  if(strcasecmp(tok, "live")     == 0) { *out = WM_TRADE_MODE_LIVE;     return(SUCCESS); }

  return(FAIL);
}

const char *
wm_trade_mode_name(wm_trade_mode_t m)
{
  switch(m)
  {
    case WM_TRADE_MODE_OFF:       return("off");
    case WM_TRADE_MODE_PAPER:     return("paper");
    case WM_TRADE_MODE_BACKTEST:  return("backtest");
    case WM_TRADE_MODE_LIVE:      return("live");
  }

  return("?");
}

// ----------------------------------------------------------------------- //
// Lifecycle                                                               //
// ----------------------------------------------------------------------- //

bool
wm_trade_engine_init(void)
{
  if(g_registry.initialized)
    return(SUCCESS);

  if(pthread_mutex_init(&g_registry.lock, NULL) != 0)
    return(FAIL);

  // Per-thread active-registry slot. The destructor is NULL — every
  // caller that pins a private registry via use_registry is responsible
  // for unbinding before its thread exits (and for destroying the
  // registry).
  if(pthread_key_create(&g_bt_registry_key, NULL) != 0)
  {
    pthread_mutex_destroy(&g_registry.lock);
    return(FAIL);
  }

  g_bt_registry_key_inited = true;
  g_registry.head          = NULL;
  g_registry.n_books       = 0;
  g_registry.initialized   = true;

  clam(CLAM_INFO, WM_TRADE_CTX, "trade engine initialized");
  return(SUCCESS);
}

static void
wm_trade_book_free_locked(wm_trade_book_t *book)
{
  if(book == NULL)
    return;

  if(book->pnl != NULL)
    wm_pnl_acc_free(book->pnl);

  mem_free(book);
}

// Forward decl for the WM-PT-3 hydrate path; the body lives near the
// other static helpers below the persist + hydrate block.
static void wm_trade_book_refresh_kv_locked(wm_trade_book_t *book);

// ----------------------------------------------------------------------- //
// Book-state durable persist (WM-PT-3)                                    //
// ----------------------------------------------------------------------- //
//
// Live + paper books are snapshotted into wm_trade_book_state on every
// state-changing event (paper fill, mode set, reset). Backtest books
// (mode BACKTEST or market_id_str starting with "bt:") and books living
// in private registries (sweep workers) are skipped — those exist only
// for the lifetime of one iteration.
//
// Coalescing + async write happens in trade_persist.c's book queue;
// this layer just decides "should we persist" and builds the UPSERT.
// All persist helpers are called UNDER the registry lock so the
// snapshot is consistent.

#define WM_BOOK_PERSIST_SQL_INIT_CAP  (16 * 1024)
#define WM_BOOK_JSON_BUF_INIT_CAP     (8  * 1024)

typedef struct
{
  char  *buf;
  size_t off;
  size_t cap;
  bool   oom;
} wm_book_buf_t;

static bool
wm_book_buf_grow(wm_book_buf_t *b, size_t need)
{
  size_t  new_cap;
  char   *p;

  if(b == NULL || b->oom)
    return(FAIL);

  if(b->off + need + 1 <= b->cap)
    return(SUCCESS);

  new_cap = b->cap > 0 ? b->cap : 1024;

  while(new_cap < b->off + need + 1)
    new_cap *= 2;

  p = mem_realloc(b->buf, new_cap);

  if(p == NULL)
  {
    b->oom = true;
    return(FAIL);
  }

  b->buf = p;
  b->cap = new_cap;
  return(SUCCESS);
}

static void
wm_book_buf_putc(wm_book_buf_t *b, char c)
{
  if(b == NULL || b->oom)
    return;

  if(wm_book_buf_grow(b, 1) != SUCCESS)
    return;

  b->buf[b->off++] = c;
  b->buf[b->off]   = '\0';
}

static void
wm_book_buf_puts(wm_book_buf_t *b, const char *s)
{
  size_t n;

  if(b == NULL || s == NULL || b->oom)
    return;

  n = strlen(s);

  if(wm_book_buf_grow(b, n) != SUCCESS)
    return;

  memcpy(b->buf + b->off, s, n);
  b->off            += n;
  b->buf[b->off]     = '\0';
}

static void
wm_book_buf_printf(wm_book_buf_t *b, const char *fmt, ...)
{
  va_list ap;
  va_list ap2;
  int     n;

  if(b == NULL || fmt == NULL || b->oom)
    return;

  va_start(ap, fmt);
  va_copy(ap2, ap);

  n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);

  if(n < 0)
  {
    va_end(ap2);
    b->oom = true;
    return;
  }

  if(wm_book_buf_grow(b, (size_t)n) != SUCCESS)
  {
    va_end(ap2);
    return;
  }

  vsnprintf(b->buf + b->off, b->cap - b->off, fmt, ap2);
  va_end(ap2);

  b->off += (size_t)n;
}

// JSON-escape a string into the buffer (no surrounding quotes — caller
// emits them). Backslash, double-quote, and control chars (< 0x20) are
// escaped per RFC 8259. Non-ASCII bytes pass through untouched (UTF-8
// is valid JSON when consumers are byte-clean).
static void
wm_book_json_escape(wm_book_buf_t *b, const char *src)
{
  const unsigned char *p;

  if(b == NULL || src == NULL)
    return;

  for(p = (const unsigned char *)src; *p != '\0'; p++)
  {
    switch(*p)
    {
      case '"':  wm_book_buf_puts(b, "\\\"");  break;
      case '\\': wm_book_buf_puts(b, "\\\\");  break;
      case '\b': wm_book_buf_puts(b, "\\b");   break;
      case '\f': wm_book_buf_puts(b, "\\f");   break;
      case '\n': wm_book_buf_puts(b, "\\n");   break;
      case '\r': wm_book_buf_puts(b, "\\r");   break;
      case '\t': wm_book_buf_puts(b, "\\t");   break;
      default:
        if(*p < 0x20)
          wm_book_buf_printf(b, "\\u%04x", (unsigned)*p);
        else
          wm_book_buf_putc(b, (char)*p);
        break;
    }
  }
}

// Embed a JSON blob inside a single-quoted SQL literal — doubles every
// single quote, brackets the result with the literal cast suffix.
static void
wm_book_emit_json_in_sql(wm_book_buf_t *out, const char *json)
{
  const char *p;

  if(out == NULL || json == NULL)
  {
    wm_book_buf_puts(out, "'null'::jsonb");
    return;
  }

  wm_book_buf_putc(out, '\'');

  for(p = json; *p != '\0'; p++)
  {
    if(*p == '\'')
      wm_book_buf_putc(out, '\'');

    wm_book_buf_putc(out, *p);
  }

  wm_book_buf_puts(out, "'::jsonb");
}

static const char *
wm_book_pos_side_name(wm_position_side_t s)
{
  switch(s)
  {
    case WM_POS_FLAT:  return("flat");
    case WM_POS_LONG:  return("long");
    case WM_POS_SHORT: return("short");
  }

  return("flat");
}

static wm_position_side_t
wm_book_pos_side_parse(const char *s)
{
  if(s == NULL)                  return(WM_POS_FLAT);
  if(strcasecmp(s, "long")  == 0) return(WM_POS_LONG);
  if(strcasecmp(s, "short") == 0) return(WM_POS_SHORT);
  return(WM_POS_FLAT);
}

// Resolve market_id_str → wm_market.id (int) by walking the live
// markets container. -1 if not found. Caller must hold no market locks
// (the container itself is unlocked; element pointers are stable
// because add/remove serialise on the cmd worker thread).
static int32_t
wm_book_resolve_market_id(const char *market_id_str)
{
  whenmoon_state_t   *st;
  whenmoon_markets_t *m;
  uint32_t            i;

  if(market_id_str == NULL)
    return(-1);

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
    return(-1);

  m = st->markets;

  for(i = 0; i < m->n_markets; i++)
    if(strncmp(m->arr[i].market_id_str, market_id_str,
           WM_MARKET_ID_STR_SZ) == 0)
      return(m->arr[i].market_id);

  return(-1);
}

// Should this (registry, book) be persisted? Three guards layered:
// 1) the registry is the global one (private registries belong to
//    sweep workers and never persist),
// 2) the book's market_id_str does not start with WM_BACKTEST_ID_PREFIX
//    (synthetic backtest ids never reach DB),
// 3) the mode is not WM_TRADE_MODE_BACKTEST (operator-set backtest
//    on a real id; treat as transient).
static bool
wm_book_should_persist(wm_trade_registry_t *reg,
    const wm_trade_book_t *book)
{
  if(reg != &g_registry)
    return(false);

  if(book == NULL)
    return(false);

  if(book->mode == WM_TRADE_MODE_BACKTEST)
    return(false);

  if(strncmp(book->market_id_str, WM_BACKTEST_ID_PREFIX,
         strlen(WM_BACKTEST_ID_PREFIX)) == 0)
    return(false);

  return(true);
}

static void
wm_book_json_signal(wm_book_buf_t *out, const wm_strategy_signal_t *sig)
{
  if(out == NULL)
    return;

  if(sig == NULL)
  {
    wm_book_buf_puts(out, "null");
    return;
  }

  wm_book_buf_puts(out, "{\"ts_ms\":");
  wm_book_buf_printf(out, "%" PRId64, sig->ts_ms);
  wm_book_buf_puts(out, ",\"score\":");
  wm_book_buf_printf(out, "%.10g", sig->score);
  wm_book_buf_puts(out, ",\"confidence\":");
  wm_book_buf_printf(out, "%.10g", sig->confidence);
  wm_book_buf_puts(out, ",\"reason\":\"");
  wm_book_json_escape(out, sig->reason);
  wm_book_buf_puts(out, "\"}");
}

// Encode the full fills ring as a JSON array. The on-disk shape is the
// raw ring (capacity = WM_FILL_RING_CAP) — fill_n + fill_head index
// into it on hydrate so the ring is reconstructed byte-equivalent.
// Slots beyond the populated count carry zeroed defaults (qty=0).
static void
wm_book_json_fills(wm_book_buf_t *out, const wm_trade_book_t *book)
{
  uint32_t i;
  uint32_t n = WM_FILL_RING_CAP;

  if(out == NULL || book == NULL)
  {
    wm_book_buf_puts(out, "[]");
    return;
  }

  wm_book_buf_putc(out, '[');

  for(i = 0; i < n; i++)
  {
    const wm_fill_t *f = &book->fills[i];

    if(i > 0)
      wm_book_buf_putc(out, ',');

    wm_book_buf_puts(out, "{\"ts_ms\":");
    wm_book_buf_printf(out, "%" PRId64, f->ts_ms);
    wm_book_buf_puts(out, ",\"side\":\"");
    wm_book_buf_putc(out, f->side != '\0' ? f->side : 'b');
    wm_book_buf_puts(out, "\",\"qty\":");
    wm_book_buf_printf(out, "%.10g", f->qty);
    wm_book_buf_puts(out, ",\"price\":");
    wm_book_buf_printf(out, "%.10g", f->price);
    wm_book_buf_puts(out, ",\"fee\":");
    wm_book_buf_printf(out, "%.10g", f->fee);
    wm_book_buf_puts(out, ",\"slippage\":");
    wm_book_buf_printf(out, "%.10g", f->slippage);
    wm_book_buf_puts(out, ",\"realized_pnl\":");
    wm_book_buf_printf(out, "%.10g", f->realized_pnl);
    wm_book_buf_puts(out, ",\"cash_after\":");
    wm_book_buf_printf(out, "%.10g", f->cash_after);
    wm_book_buf_puts(out, ",\"position_after\":");
    wm_book_buf_printf(out, "%.10g", f->position_after);
    wm_book_buf_puts(out, ",\"reason\":\"");
    wm_book_json_escape(out, f->reason);
    wm_book_buf_puts(out, "\"}");
  }

  wm_book_buf_putc(out, ']');
}

static void
wm_book_json_pnl(wm_book_buf_t *out, const wm_pnl_acc_t *acc)
{
  uint32_t i;

  if(out == NULL)
    return;

  if(acc == NULL)
  {
    wm_book_buf_puts(out, "null");
    return;
  }

  wm_book_buf_puts(out, "{\"n_trades\":");
  wm_book_buf_printf(out, "%u", acc->n_trades);
  wm_book_buf_puts(out, ",\"n_wins\":");
  wm_book_buf_printf(out, "%u", acc->n_wins);
  wm_book_buf_puts(out, ",\"n_losses\":");
  wm_book_buf_printf(out, "%u", acc->n_losses);
  wm_book_buf_puts(out, ",\"realized_pnl\":");
  wm_book_buf_printf(out, "%.10g", acc->realized_pnl);
  wm_book_buf_puts(out, ",\"gross_profit\":");
  wm_book_buf_printf(out, "%.10g", acc->gross_profit);
  wm_book_buf_puts(out, ",\"gross_loss\":");
  wm_book_buf_printf(out, "%.10g", acc->gross_loss);
  wm_book_buf_puts(out, ",\"fees_paid\":");
  wm_book_buf_printf(out, "%.10g", acc->fees_paid);
  wm_book_buf_puts(out, ",\"equity_peak\":");
  wm_book_buf_printf(out, "%.10g", acc->equity_peak);
  wm_book_buf_puts(out, ",\"max_drawdown\":");
  wm_book_buf_printf(out, "%.10g", acc->max_drawdown);
  wm_book_buf_puts(out, ",\"have_peak\":");
  wm_book_buf_puts(out, acc->have_peak ? "true" : "false");
  wm_book_buf_puts(out, ",\"returns_n\":");
  wm_book_buf_printf(out, "%u", acc->returns_n);
  wm_book_buf_puts(out, ",\"returns_head\":");
  wm_book_buf_printf(out, "%u", acc->returns_head);
  wm_book_buf_puts(out, ",\"returns\":[");

  if(acc->returns != NULL)
  {
    for(i = 0; i < acc->returns_cap; i++)
    {
      if(i > 0)
        wm_book_buf_putc(out, ',');

      wm_book_buf_printf(out, "%.10g", acc->returns[i]);
    }
  }

  wm_book_buf_puts(out, "]}");
}

// Serialize one book to a heap-allocated UPSERT statement. Returns
// NULL on alloc failure or unresolvable market_id. Caller takes
// ownership of the returned buffer.
static char *
wm_book_persist_build_sql_locked(const wm_trade_book_t *book,
    int32_t market_id)
{
  wm_book_buf_t  signal_buf = { 0 };
  wm_book_buf_t  fills_buf  = { 0 };
  wm_book_buf_t  pnl_buf    = { 0 };
  wm_book_buf_t  sql        = { 0 };
  char          *escaped_strat = NULL;

  if(book == NULL)
    return(NULL);

  // Build JSON sub-blobs first so the final SQL pass is a single
  // grow-and-emit. JSON content needs SQL-escape (single-quote
  // doubling) at the embedding boundary.
  signal_buf.buf = mem_alloc("whenmoon", "book_persist_sig_json",
      WM_BOOK_JSON_BUF_INIT_CAP);
  fills_buf.buf  = mem_alloc("whenmoon", "book_persist_fills_json",
      WM_BOOK_JSON_BUF_INIT_CAP);
  pnl_buf.buf    = mem_alloc("whenmoon", "book_persist_pnl_json",
      WM_BOOK_JSON_BUF_INIT_CAP);
  sql.buf        = mem_alloc("whenmoon", "book_persist_sql",
      WM_BOOK_PERSIST_SQL_INIT_CAP);

  if(signal_buf.buf == NULL || fills_buf.buf == NULL ||
     pnl_buf.buf    == NULL || sql.buf       == NULL)
    goto fail;

  signal_buf.cap = fills_buf.cap = pnl_buf.cap = WM_BOOK_JSON_BUF_INIT_CAP;
  sql.cap        = WM_BOOK_PERSIST_SQL_INIT_CAP;
  signal_buf.buf[0] = fills_buf.buf[0] = pnl_buf.buf[0] = sql.buf[0] = '\0';

  if(book->has_last_signal)
    wm_book_json_signal(&signal_buf, &book->last_signal);
  else
    wm_book_buf_puts(&signal_buf, "null");

  wm_book_json_fills(&fills_buf, book);
  wm_book_json_pnl(&pnl_buf, book->pnl);

  if(signal_buf.oom || fills_buf.oom || pnl_buf.oom)
    goto fail;

  escaped_strat = db_escape(book->strategy_name);

  if(escaped_strat == NULL)
    goto fail;

  // INSERT … ON CONFLICT DO UPDATE so the per-(market, strategy) row
  // is owned exclusively by the trade book; subsequent fills overwrite
  // their own row in place. EXCLUDED is the proposed-insert tuple.
  wm_book_buf_printf(&sql,
      "INSERT INTO wm_trade_book_state ("
      "market_id, strategy_name, mode, starting_cash, cash,"
      " position_side, position_qty, position_avg, position_opened_ms,"
      " fee_bps, slip_bps, size_frac, max_position,"
      " last_mark_px, last_mark_ms,"
      " last_signal, has_last_signal,"
      " fill_n, fill_head, fills_ring, pnl, updated_at)"
      " VALUES (%" PRId32 ", '%s', '%s',"
      " %.10g, %.10g, '%s', %.10g, %.10g, %" PRId64 ","
      " %.10g, %.10g, %.10g, %.10g,"
      " %.10g, %" PRId64 ", ",
      market_id, escaped_strat, wm_trade_mode_name(book->mode),
      book->starting_cash, book->cash,
      wm_book_pos_side_name(book->position.side),
      book->position.qty, book->position.avg_entry_px,
      book->position.opened_at_ms,
      book->fee_bps, book->slip_bps, book->size_frac, book->max_position,
      book->last_mark_px, book->last_mark_ms);

  wm_book_emit_json_in_sql(&sql, signal_buf.buf);
  wm_book_buf_puts(&sql, ", ");
  wm_book_buf_puts(&sql, book->has_last_signal ? "true" : "false");
  wm_book_buf_printf(&sql, ", %" PRIu64 ", %u, ",
      book->fill_n, book->fill_head);
  wm_book_emit_json_in_sql(&sql, fills_buf.buf);
  wm_book_buf_puts(&sql, ", ");
  wm_book_emit_json_in_sql(&sql, pnl_buf.buf);
  wm_book_buf_puts(&sql,
      ", NOW())"
      " ON CONFLICT (market_id, strategy_name) DO UPDATE SET"
      " mode = EXCLUDED.mode,"
      " starting_cash = EXCLUDED.starting_cash,"
      " cash = EXCLUDED.cash,"
      " position_side = EXCLUDED.position_side,"
      " position_qty = EXCLUDED.position_qty,"
      " position_avg = EXCLUDED.position_avg,"
      " position_opened_ms = EXCLUDED.position_opened_ms,"
      " fee_bps = EXCLUDED.fee_bps,"
      " slip_bps = EXCLUDED.slip_bps,"
      " size_frac = EXCLUDED.size_frac,"
      " max_position = EXCLUDED.max_position,"
      " last_mark_px = EXCLUDED.last_mark_px,"
      " last_mark_ms = EXCLUDED.last_mark_ms,"
      " last_signal = EXCLUDED.last_signal,"
      " has_last_signal = EXCLUDED.has_last_signal,"
      " fill_n = EXCLUDED.fill_n,"
      " fill_head = EXCLUDED.fill_head,"
      " fills_ring = EXCLUDED.fills_ring,"
      " pnl = EXCLUDED.pnl,"
      " updated_at = NOW()");

  if(sql.oom)
    goto fail;

  mem_free(signal_buf.buf);
  mem_free(fills_buf.buf);
  mem_free(pnl_buf.buf);
  mem_free(escaped_strat);

  return(sql.buf);

fail:
  if(signal_buf.buf != NULL) mem_free(signal_buf.buf);
  if(fills_buf.buf  != NULL) mem_free(fills_buf.buf);
  if(pnl_buf.buf    != NULL) mem_free(pnl_buf.buf);
  if(sql.buf        != NULL) mem_free(sql.buf);
  if(escaped_strat  != NULL) mem_free(escaped_strat);
  return(NULL);
}

// Public-within-file: gate, build, enqueue. Caller holds reg->lock.
static void
wm_book_persist_locked(wm_trade_registry_t *reg,
    const wm_trade_book_t *book)
{
  int32_t  market_id;
  char    *sql;

  if(!wm_book_should_persist(reg, book))
    return;

  market_id = wm_book_resolve_market_id(book->market_id_str);

  if(market_id < 0)
  {
    clam(CLAM_DEBUG2, WM_TRADE_CTX,
        "book persist skipped (market_id unresolved): %s/%s",
        book->market_id_str, book->strategy_name);
    return;
  }

  sql = wm_book_persist_build_sql_locked(book, market_id);

  if(sql == NULL)
  {
    clam(CLAM_WARN, WM_TRADE_CTX,
        "book persist sql alloc failed: %s/%s",
        book->market_id_str, book->strategy_name);
    return;
  }

  if(wm_book_persist_enqueue(market_id, book->strategy_name, sql)
         != SUCCESS)
  {
    mem_free(sql);
    clam(CLAM_WARN, WM_TRADE_CTX,
        "book persist enqueue failed: %s/%s",
        book->market_id_str, book->strategy_name);
  }
}

// Enqueue a DELETE for the (market, strategy) row. Same gating as
// persist. Called under reg->lock from book-remove paths.
static void
wm_book_persist_drop_locked(wm_trade_registry_t *reg,
    const wm_trade_book_t *book)
{
  int32_t market_id;

  // Drop has a looser gate than persist: a row may exist in DB even
  // for a book that's now in BACKTEST mode (transition via set_mode),
  // and we want the DELETE to clean it up. Skip only when the
  // registry isn't the global one OR the market id is synthetic.
  if(reg != &g_registry || book == NULL)
    return;

  if(strncmp(book->market_id_str, WM_BACKTEST_ID_PREFIX,
         strlen(WM_BACKTEST_ID_PREFIX)) == 0)
    return;

  market_id = wm_book_resolve_market_id(book->market_id_str);

  if(market_id < 0)
    return;

  if(wm_book_persist_drop(market_id, book->strategy_name) != SUCCESS)
    clam(CLAM_WARN, WM_TRADE_CTX,
        "book persist-drop enqueue failed: %s/%s",
        book->market_id_str, book->strategy_name);
}

// ----------------------------------------------------------------------- //
// Book-state hydrate (WM-PT-3)                                            //
// ----------------------------------------------------------------------- //
//
// Decode a wm_trade_book_state row into a freshly-allocated book and
// link it into `reg`. Returns the book pointer on success, NULL when
// no row exists OR on decode failure (caller falls back to fresh
// create). Caller holds reg->lock.

static void
wm_book_hydrate_signal(json_object *root, wm_strategy_signal_t *out)
{
  json_object *v;

  if(root == NULL || out == NULL)
    return;

  memset(out, 0, sizeof(*out));

  if(json_object_object_get_ex(root, "ts_ms", &v))
    out->ts_ms = (int64_t)json_object_get_int64(v);
  if(json_object_object_get_ex(root, "score", &v))
    out->score = json_object_get_double(v);
  if(json_object_object_get_ex(root, "confidence", &v))
    out->confidence = json_object_get_double(v);
  if(json_object_object_get_ex(root, "reason", &v))
  {
    const char *s = json_object_get_string(v);

    if(s != NULL)
      snprintf(out->reason, sizeof(out->reason), "%s", s);
  }
}

static void
wm_book_hydrate_fills(json_object *arr, wm_trade_book_t *book)
{
  size_t n;
  size_t i;

  if(arr == NULL || book == NULL ||
     json_object_get_type(arr) != json_type_array)
    return;

  n = json_object_array_length(arr);

  if(n > WM_FILL_RING_CAP)
    n = WM_FILL_RING_CAP;

  for(i = 0; i < n; i++)
  {
    json_object *e = json_object_array_get_idx(arr, i);
    wm_fill_t   *f = &book->fills[i];
    json_object *v;
    const char  *s;

    memset(f, 0, sizeof(*f));

    if(e == NULL || json_object_get_type(e) != json_type_object)
      continue;

    if(json_object_object_get_ex(e, "ts_ms", &v))
      f->ts_ms = (int64_t)json_object_get_int64(v);
    if(json_object_object_get_ex(e, "side", &v))
    {
      s = json_object_get_string(v);
      f->side = (s != NULL && s[0] != '\0') ? s[0] : 'b';
    }
    if(json_object_object_get_ex(e, "qty", &v))
      f->qty = json_object_get_double(v);
    if(json_object_object_get_ex(e, "price", &v))
      f->price = json_object_get_double(v);
    if(json_object_object_get_ex(e, "fee", &v))
      f->fee = json_object_get_double(v);
    if(json_object_object_get_ex(e, "slippage", &v))
      f->slippage = json_object_get_double(v);
    if(json_object_object_get_ex(e, "realized_pnl", &v))
      f->realized_pnl = json_object_get_double(v);
    if(json_object_object_get_ex(e, "cash_after", &v))
      f->cash_after = json_object_get_double(v);
    if(json_object_object_get_ex(e, "position_after", &v))
      f->position_after = json_object_get_double(v);
    if(json_object_object_get_ex(e, "reason", &v))
    {
      s = json_object_get_string(v);
      if(s != NULL)
        snprintf(f->reason, sizeof(f->reason), "%s", s);
    }
  }
}

static void
wm_book_hydrate_pnl(json_object *root, wm_pnl_acc_t *acc)
{
  json_object *v;

  if(root == NULL || acc == NULL ||
     json_object_get_type(root) != json_type_object)
    return;

  if(json_object_object_get_ex(root, "n_trades", &v))
    acc->n_trades = (uint32_t)json_object_get_int(v);
  if(json_object_object_get_ex(root, "n_wins", &v))
    acc->n_wins = (uint32_t)json_object_get_int(v);
  if(json_object_object_get_ex(root, "n_losses", &v))
    acc->n_losses = (uint32_t)json_object_get_int(v);
  if(json_object_object_get_ex(root, "realized_pnl", &v))
    acc->realized_pnl = json_object_get_double(v);
  if(json_object_object_get_ex(root, "gross_profit", &v))
    acc->gross_profit = json_object_get_double(v);
  if(json_object_object_get_ex(root, "gross_loss", &v))
    acc->gross_loss = json_object_get_double(v);
  if(json_object_object_get_ex(root, "fees_paid", &v))
    acc->fees_paid = json_object_get_double(v);
  if(json_object_object_get_ex(root, "equity_peak", &v))
    acc->equity_peak = json_object_get_double(v);
  if(json_object_object_get_ex(root, "max_drawdown", &v))
    acc->max_drawdown = json_object_get_double(v);
  if(json_object_object_get_ex(root, "have_peak", &v))
    acc->have_peak = json_object_get_boolean(v);
  if(json_object_object_get_ex(root, "returns_n", &v))
    acc->returns_n = (uint32_t)json_object_get_int(v);
  if(json_object_object_get_ex(root, "returns_head", &v))
    acc->returns_head = (uint32_t)json_object_get_int(v);

  if(acc->returns_n > acc->returns_cap)
    acc->returns_n = acc->returns_cap;
  if(acc->returns_head >= acc->returns_cap)
    acc->returns_head = 0;

  if(json_object_object_get_ex(root, "returns", &v) &&
     json_object_get_type(v) == json_type_array && acc->returns != NULL)
  {
    size_t n = json_object_array_length(v);
    size_t i;

    if(n > acc->returns_cap)
      n = acc->returns_cap;

    for(i = 0; i < n; i++)
    {
      json_object *e = json_object_array_get_idx(v, i);

      acc->returns[i] = (e != NULL) ? json_object_get_double(e) : 0.0;
    }
  }
}

static wm_trade_book_t *
wm_book_hydrate_locked(wm_trade_registry_t *reg,
    const char *market_id_str, const char *strategy_name)
{
  int32_t          market_id;
  char            *escaped_strat = NULL;
  char             sql[512];
  db_result_t     *res = NULL;
  wm_trade_book_t *book = NULL;
  json_tokener    *tok = NULL;
  json_object     *parsed = NULL;
  const char      *cell;

  if(reg != &g_registry || market_id_str == NULL || strategy_name == NULL)
    return(NULL);

  market_id = wm_book_resolve_market_id(market_id_str);

  if(market_id < 0)
    return(NULL);

  escaped_strat = db_escape(strategy_name);

  if(escaped_strat == NULL)
    return(NULL);

  snprintf(sql, sizeof(sql),
      "SELECT mode, starting_cash, cash, position_side, position_qty,"
      " position_avg, position_opened_ms, fee_bps, slip_bps, size_frac,"
      " max_position, last_mark_px, last_mark_ms, last_signal,"
      " has_last_signal, fill_n, fill_head, fills_ring, pnl"
      "  FROM wm_trade_book_state"
      " WHERE market_id = %" PRId32 " AND strategy_name = '%s'",
      market_id, escaped_strat);

  mem_free(escaped_strat);
  escaped_strat = NULL;

  res = db_result_alloc();

  if(res == NULL)
    return(NULL);

  if(db_query(sql, res) != SUCCESS || !res->ok || res->rows == 0)
    goto cleanup;

  book = mem_alloc("whenmoon", "trade_book", sizeof(*book));

  if(book == NULL)
    goto cleanup;

  memset(book, 0, sizeof(*book));
  snprintf(book->market_id_str, sizeof(book->market_id_str), "%s",
      market_id_str);
  snprintf(book->strategy_name, sizeof(book->strategy_name), "%s",
      strategy_name);

  book->pnl = wm_pnl_acc_create(0);

  if(book->pnl == NULL)
  {
    mem_free(book);
    book = NULL;
    goto cleanup;
  }

  // Scalar columns (col index follows the SELECT order above).
  {
    wm_trade_mode_t mode;

    cell = db_result_get(res, 0, 0);
    if(cell != NULL && wm_trade_mode_parse(cell, &mode) == SUCCESS)
      book->mode = mode;
  }
  if((cell = db_result_get(res, 0, 1)) != NULL)
    book->starting_cash = strtod(cell, NULL);
  if((cell = db_result_get(res, 0, 2)) != NULL)
    book->cash = strtod(cell, NULL);
  if((cell = db_result_get(res, 0, 3)) != NULL)
    book->position.side = wm_book_pos_side_parse(cell);
  if((cell = db_result_get(res, 0, 4)) != NULL)
    book->position.qty = strtod(cell, NULL);
  if((cell = db_result_get(res, 0, 5)) != NULL)
    book->position.avg_entry_px = strtod(cell, NULL);
  if((cell = db_result_get(res, 0, 6)) != NULL)
    book->position.opened_at_ms = (int64_t)strtoll(cell, NULL, 10);
  if((cell = db_result_get(res, 0, 7)) != NULL)
    book->fee_bps = strtod(cell, NULL);
  if((cell = db_result_get(res, 0, 8)) != NULL)
    book->slip_bps = strtod(cell, NULL);
  if((cell = db_result_get(res, 0, 9)) != NULL)
    book->size_frac = strtod(cell, NULL);
  if((cell = db_result_get(res, 0, 10)) != NULL)
    book->max_position = strtod(cell, NULL);
  if((cell = db_result_get(res, 0, 11)) != NULL)
    book->last_mark_px = strtod(cell, NULL);
  if((cell = db_result_get(res, 0, 12)) != NULL)
    book->last_mark_ms = (int64_t)strtoll(cell, NULL, 10);

  // last_signal JSONB (col 13) — parse if non-null.
  if((cell = db_result_get(res, 0, 13)) != NULL && cell[0] != '\0')
  {
    tok    = json_tokener_new();
    parsed = (tok != NULL) ? json_tokener_parse_ex(tok, cell,
        (int)strlen(cell)) : NULL;

    if(parsed != NULL && json_object_get_type(parsed) == json_type_object)
      wm_book_hydrate_signal(parsed, &book->last_signal);

    if(parsed != NULL) json_object_put(parsed);
    if(tok    != NULL) json_tokener_free(tok);
    parsed = NULL;
    tok    = NULL;
  }

  if((cell = db_result_get(res, 0, 14)) != NULL)
    book->has_last_signal = (cell[0] == 't' || cell[0] == 'T' ||
                             cell[0] == '1');
  if((cell = db_result_get(res, 0, 15)) != NULL)
    book->fill_n = (uint64_t)strtoull(cell, NULL, 10);
  if((cell = db_result_get(res, 0, 16)) != NULL)
    book->fill_head = (uint32_t)strtoul(cell, NULL, 10);

  if(book->fill_head >= WM_FILL_RING_CAP)
    book->fill_head = 0;

  // fills_ring JSONB (col 17).
  if((cell = db_result_get(res, 0, 17)) != NULL && cell[0] != '\0')
  {
    tok    = json_tokener_new();
    parsed = (tok != NULL) ? json_tokener_parse_ex(tok, cell,
        (int)strlen(cell)) : NULL;

    if(parsed != NULL && json_object_get_type(parsed) == json_type_array)
      wm_book_hydrate_fills(parsed, book);

    if(parsed != NULL) json_object_put(parsed);
    if(tok    != NULL) json_tokener_free(tok);
    parsed = NULL;
    tok    = NULL;
  }

  // pnl JSONB (col 18).
  if((cell = db_result_get(res, 0, 18)) != NULL && cell[0] != '\0')
  {
    tok    = json_tokener_new();
    parsed = (tok != NULL) ? json_tokener_parse_ex(tok, cell,
        (int)strlen(cell)) : NULL;

    if(parsed != NULL && json_object_get_type(parsed) == json_type_object)
      wm_book_hydrate_pnl(parsed, book->pnl);

    if(parsed != NULL) json_object_put(parsed);
    if(tok    != NULL) json_tokener_free(tok);
    parsed = NULL;
    tok    = NULL;
  }

  // Link into registry.
  book->next = reg->head;
  reg->head  = book;
  reg->n_books++;

  // Re-resolve fee/slip/size/max_position from live KV so an operator
  // who edited those between sessions gets the new values. cash,
  // position, fills, pnl, mode, starting_cash all stick at the
  // hydrated values (DB is authoritative for runtime state; KV is
  // authoritative for live config knobs).
  wm_trade_book_refresh_kv_locked(book);

  clam(CLAM_INFO, WM_TRADE_CTX,
      "book hydrated: %s/%s mode=%s cash=%.2f pos=%s/%.6g fills=%" PRIu64,
      market_id_str, strategy_name,
      wm_trade_mode_name(book->mode), book->cash,
      wm_book_pos_side_name(book->position.side),
      book->position.qty, book->fill_n);

cleanup:
  if(res != NULL)
    db_result_free(res);
  return(book);
}

void
wm_trade_engine_destroy(void)
{
  wm_trade_book_t *b;
  wm_trade_book_t *next;

  if(!g_registry.initialized)
    return;

  pthread_mutex_lock(&g_registry.lock);

  b = g_registry.head;

  while(b != NULL)
  {
    next = b->next;
    wm_trade_book_free_locked(b);
    b    = next;
  }

  g_registry.head    = NULL;
  g_registry.n_books = 0;

  pthread_mutex_unlock(&g_registry.lock);
  pthread_mutex_destroy(&g_registry.lock);

  if(g_bt_registry_key_inited)
  {
    pthread_key_delete(g_bt_registry_key);
    g_bt_registry_key_inited = false;
  }

  g_registry.initialized = false;

  clam(CLAM_INFO, WM_TRADE_CTX, "trade engine destroyed");
}

// ----------------------------------------------------------------------- //
// Private registry lifecycle (WM-LT-6)                                    //
// ----------------------------------------------------------------------- //

wm_trade_registry_t *
wm_trade_registry_create(void)
{
  wm_trade_registry_t *r;

  if(!g_registry.initialized)
    return(NULL);

  r = mem_alloc("whenmoon", "trade_registry", sizeof(*r));

  if(r == NULL)
    return(NULL);

  memset(r, 0, sizeof(*r));

  if(pthread_mutex_init(&r->lock, NULL) != 0)
  {
    mem_free(r);
    return(NULL);
  }

  r->initialized = true;
  return(r);
}

void
wm_trade_registry_destroy(wm_trade_registry_t *r)
{
  wm_trade_book_t *b;
  wm_trade_book_t *next;

  if(r == NULL)
    return;

  pthread_mutex_lock(&r->lock);

  b = r->head;

  while(b != NULL)
  {
    next = b->next;
    wm_trade_book_free_locked(b);
    b    = next;
  }

  r->head        = NULL;
  r->n_books     = 0;
  r->initialized = false;

  pthread_mutex_unlock(&r->lock);
  pthread_mutex_destroy(&r->lock);

  mem_free(r);
}

void
wm_trade_engine_use_registry(wm_trade_registry_t *r)
{
  if(!g_bt_registry_key_inited)
    return;

  pthread_setspecific(g_bt_registry_key, r);
}

// ----------------------------------------------------------------------- //
// Book lookup                                                             //
// ----------------------------------------------------------------------- //

wm_trade_book_t *
wm_trade_book_find(wm_trade_registry_t *reg,
    const char *market_id_str, const char *strategy_name)
{
  wm_trade_book_t *b;

  if(reg == NULL || market_id_str == NULL || strategy_name == NULL)
    return(NULL);

  for(b = reg->head; b != NULL; b = b->next)
  {
    if(strncmp(b->market_id_str, market_id_str,
           sizeof(b->market_id_str)) != 0)
      continue;

    if(strncmp(b->strategy_name, strategy_name,
           sizeof(b->strategy_name)) != 0)
      continue;

    return(b);
  }

  return(NULL);
}

// Resolve every cached parameter from the strategy KV. Caller holds
// the registry lock. Mode is reset only when book->mode is OFF (the
// default initial state); subsequent /whenmoon trade mode calls own
// the mode field outright.
static void
wm_trade_book_refresh_kv_locked(wm_trade_book_t *book)
{
  const char     *mid;
  const char     *strat;
  const char     *mode_str;
  wm_trade_mode_t parsed_mode;

  if(book == NULL)
    return;

  mid   = book->market_id_str;
  strat = book->strategy_name;

  book->fee_bps =
      wm_strategy_kv_get_dbl(mid, strat, "fee_bps",
          WM_TRADE_DEF_FEE_BPS);
  book->slip_bps =
      wm_strategy_kv_get_dbl(mid, strat, "slip_bps",
          WM_TRADE_DEF_SLIP_BPS);
  book->size_frac =
      wm_strategy_kv_get_dbl(mid, strat, "size_frac",
          WM_TRADE_DEF_SIZE_FRAC);
  book->max_position =
      wm_strategy_kv_get_dbl(mid, strat, "max_position",
          WM_TRADE_DEF_MAX_POSITION);

  // starting_cash + mode resolve only on first seed (mode == OFF +
  // starting_cash == 0 means "fresh book"); after that they are owned
  // by the runtime.
  if(book->starting_cash == 0.0)
  {
    book->starting_cash =
        wm_strategy_kv_get_dbl(mid, strat, "starting_cash",
            WM_TRADE_DEF_STARTING_CASH);

    if(book->cash == 0.0)
      book->cash = book->starting_cash;
  }

  if(book->mode == WM_TRADE_MODE_OFF)
  {
    mode_str = wm_strategy_kv_get_str(mid, strat, "mode", "off");

    if(wm_trade_mode_parse(mode_str, &parsed_mode) == SUCCESS)
      book->mode = parsed_mode;
  }
}

void
wm_trade_book_refresh_kv(wm_trade_book_t *book)
{
  wm_trade_registry_t *reg;

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return;

  pthread_mutex_lock(&reg->lock);
  wm_trade_book_refresh_kv_locked(book);
  pthread_mutex_unlock(&reg->lock);
}

static wm_trade_book_t *
wm_trade_book_create_locked(wm_trade_registry_t *reg,
    const char *market_id_str, const char *strategy_name)
{
  wm_trade_book_t *book;

  book = mem_alloc("whenmoon", "trade_book", sizeof(*book));

  if(book == NULL)
    return(NULL);

  memset(book, 0, sizeof(*book));

  snprintf(book->market_id_str, sizeof(book->market_id_str),
      "%s", market_id_str);
  snprintf(book->strategy_name, sizeof(book->strategy_name),
      "%s", strategy_name);

  book->mode             = WM_TRADE_MODE_OFF;
  book->position.side    = WM_POS_FLAT;

  book->pnl = wm_pnl_acc_create(0);

  if(book->pnl == NULL)
  {
    mem_free(book);
    return(NULL);
  }

  // Pull initial param values + starting_cash + KV-default mode.
  wm_trade_book_refresh_kv_locked(book);

  // Link.
  book->next  = reg->head;
  reg->head   = book;
  reg->n_books++;

  clam(CLAM_INFO, WM_TRADE_CTX,
      "book created: %s/%s mode=%s cash=%.2f size_frac=%.4f"
      " fee_bps=%.2f slip_bps=%.2f",
      market_id_str, strategy_name,
      wm_trade_mode_name(book->mode),
      book->cash, book->size_frac, book->fee_bps, book->slip_bps);

  return(book);
}

// Find-or-create with hydrate. On miss, tries to hydrate from
// wm_trade_book_state (global registry only); on hydrate miss falls
// through to fresh create. Caller holds reg->lock.
static wm_trade_book_t *
wm_trade_book_find_or_create_locked(wm_trade_registry_t *reg,
    const char *market_id_str, const char *strategy_name)
{
  wm_trade_book_t *b;

  b = wm_trade_book_find(reg, market_id_str, strategy_name);

  if(b == NULL)
    b = wm_book_hydrate_locked(reg, market_id_str, strategy_name);

  if(b == NULL)
    b = wm_trade_book_create_locked(reg, market_id_str, strategy_name);

  return(b);
}

wm_trade_book_t *
wm_trade_book_get_or_create(const char *market_id_str,
    const char *strategy_name)
{
  wm_trade_registry_t *reg;
  wm_trade_book_t     *b;

  if(market_id_str == NULL || strategy_name == NULL)
    return(NULL);

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return(NULL);

  pthread_mutex_lock(&reg->lock);
  b = wm_trade_book_find_or_create_locked(reg, market_id_str,
      strategy_name);
  pthread_mutex_unlock(&reg->lock);

  return(b);
}

// ----------------------------------------------------------------------- //
// Book removal                                                            //
// ----------------------------------------------------------------------- //

void
wm_trade_book_remove(const char *market_id_str, const char *strategy_name)
{
  wm_trade_registry_t  *reg;
  wm_trade_book_t     **pp;
  wm_trade_book_t      *target = NULL;

  if(market_id_str == NULL || strategy_name == NULL)
    return;

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return;

  pthread_mutex_lock(&reg->lock);

  pp = &reg->head;

  while(*pp != NULL)
  {
    if(strncmp((*pp)->market_id_str, market_id_str,
           sizeof((*pp)->market_id_str)) == 0 &&
       strncmp((*pp)->strategy_name, strategy_name,
           sizeof((*pp)->strategy_name)) == 0)
    {
      target = *pp;
      *pp    = target->next;
      reg->n_books--;
      break;
    }

    pp = &(*pp)->next;
  }

  if(target != NULL)
    wm_trade_book_free_locked(target);

  pthread_mutex_unlock(&reg->lock);

  if(target != NULL)
    clam(CLAM_INFO, WM_TRADE_CTX,
        "book removed: %s/%s", market_id_str, strategy_name);
}

uint32_t
wm_trade_books_remove_market(const char *market_id_str)
{
  wm_trade_registry_t  *reg;
  wm_trade_book_t     **pp;
  uint32_t              n_removed = 0;

  if(market_id_str == NULL)
    return(0);

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return(0);

  pthread_mutex_lock(&reg->lock);

  pp = &reg->head;

  while(*pp != NULL)
  {
    wm_trade_book_t *cur = *pp;

    if(strncmp(cur->market_id_str, market_id_str,
           sizeof(cur->market_id_str)) == 0)
    {
      *pp = cur->next;
      reg->n_books--;
      wm_trade_book_free_locked(cur);
      n_removed++;
      continue;
    }

    pp = &cur->next;
  }

  pthread_mutex_unlock(&reg->lock);

  if(n_removed > 0)
    clam(CLAM_INFO, WM_TRADE_CTX,
        "removed %u trade book(s) on market: %s",
        n_removed, market_id_str);

  return(n_removed);
}

// ----------------------------------------------------------------------- //
// Mode + reset                                                            //
// ----------------------------------------------------------------------- //

bool
wm_trade_book_set_mode(const char *market_id_str,
    const char *strategy_name, wm_trade_mode_t mode)
{
  wm_trade_registry_t *reg;
  wm_trade_book_t     *b;
  wm_trade_mode_t      prev;

  if(mode != WM_TRADE_MODE_OFF && mode != WM_TRADE_MODE_PAPER &&
     mode != WM_TRADE_MODE_BACKTEST && mode != WM_TRADE_MODE_LIVE)
    return(FAIL);

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return(FAIL);

  pthread_mutex_lock(&reg->lock);

  b = wm_trade_book_find_or_create_locked(reg, market_id_str,
      strategy_name);

  if(b == NULL)
  {
    pthread_mutex_unlock(&reg->lock);
    return(FAIL);
  }

  prev    = b->mode;
  b->mode = mode;

  // WM-PT-3: persist the new mode (or drop the row when transitioning
  // to BACKTEST / "bt:" id, which the persist gate forbids — emit a
  // DELETE so the stale row is collected).
  if(wm_book_should_persist(reg, b))
    wm_book_persist_locked(reg, b);
  else
    wm_book_persist_drop_locked(reg, b);

  pthread_mutex_unlock(&reg->lock);

  clam(CLAM_INFO, WM_TRADE_CTX,
      "%s/%s mode %s -> %s",
      market_id_str, strategy_name,
      wm_trade_mode_name(prev), wm_trade_mode_name(mode));

  return(SUCCESS);
}

void
wm_trade_book_override_params(const char *market_id_str,
    const char *strategy_name,
    bool have_fee_bps,       double fee_bps,
    bool have_slip_bps,      double slip_bps,
    bool have_size_frac,     double size_frac,
    bool have_starting_cash, double starting_cash)
{
  wm_trade_registry_t *reg;
  wm_trade_book_t     *b;

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return;

  pthread_mutex_lock(&reg->lock);

  b = wm_trade_book_find(reg, market_id_str, strategy_name);

  if(b != NULL)
  {
    if(have_fee_bps)        b->fee_bps   = fee_bps;
    if(have_slip_bps)       b->slip_bps  = slip_bps;
    if(have_size_frac)      b->size_frac = size_frac;

    if(have_starting_cash)
    {
      b->starting_cash = starting_cash;
      b->cash          = starting_cash;
    }

    clam(CLAM_INFO, WM_TRADE_CTX,
        "%s/%s override: fee=%.2f slip=%.2f size_frac=%.4f cash=%.2f",
        market_id_str, strategy_name,
        b->fee_bps, b->slip_bps, b->size_frac, b->cash);
  }

  pthread_mutex_unlock(&reg->lock);
}

void
wm_trade_book_reset(const char *market_id_str, const char *strategy_name)
{
  wm_trade_registry_t *reg;
  wm_trade_book_t     *b;

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return;

  pthread_mutex_lock(&reg->lock);

  b = wm_trade_book_find(reg, market_id_str, strategy_name);

  if(b != NULL)
  {
    b->cash             = b->starting_cash;
    b->position.side    = WM_POS_FLAT;
    b->position.qty     = 0.0;
    b->position.avg_entry_px = 0.0;
    b->position.opened_at_ms = 0;
    b->fill_n           = 0;
    b->fill_head        = 0;
    b->has_last_signal  = false;
    memset(&b->last_signal, 0, sizeof(b->last_signal));
    memset(b->fills, 0, sizeof(b->fills));

    if(b->pnl != NULL)
      wm_pnl_acc_reset(b->pnl);

    // Re-pull cached params in case the user adjusted KV between
    // attach and reset.
    wm_trade_book_refresh_kv_locked(b);

    // WM-PT-3: snapshot the post-reset book so a restart sees the
    // cleared state, not the prior accumulated one.
    wm_book_persist_locked(reg, b);

    clam(CLAM_INFO, WM_TRADE_CTX,
        "%s/%s reset (cash=%.2f)",
        market_id_str, strategy_name, b->cash);
  }

  pthread_mutex_unlock(&reg->lock);
}

// ----------------------------------------------------------------------- //
// Fill engine (paper)                                                     //
// ----------------------------------------------------------------------- //

// Apply a synthetic fill to the book under the registry lock. Updates
// position (open / scale / partial close / full close / flip),
// cash, fees, ring, and PnL accumulators.
//
// `intent` is BUY or SELL with qty > 0; `mark_px` is the pre-slippage
// mark; ts_ms is the originating signal's timestamp.
//
// Slippage convention: a buy pays `mark + slip`, a sell receives
// `mark - slip`. Fee is taken on notional = qty * fill_px and paid
// out of cash.
static void
wm_trade_apply_paper_fill_locked(wm_trade_book_t *b,
    const wm_sizer_intent_t *intent, double mark_px, int64_t ts_ms,
    const wm_strategy_signal_t *sig)
{
  wm_fill_t *slot;
  double     slip;
  double     fill_px;
  double     notional;
  double     fee;
  double     signed_pos;
  double     signed_delta;
  double     close_qty;
  double     open_qty;
  double     realized;
  double     close_cost;
  double     equity;

  if(b == NULL || intent == NULL || sig == NULL)
    return;

  if(intent->action == WM_SIZER_HOLD || intent->qty <= 0.0)
    return;

  // Slippage is one-sided against the trader: positive slip on a buy
  // (pay above mark), negative slip on a sell (receive below mark).
  slip    = mark_px * (b->slip_bps / 10000.0);
  fill_px = intent->action == WM_SIZER_BUY ? mark_px + slip
                                           : mark_px - slip;
  if(fill_px <= 0.0)
    return;

  notional = intent->qty * fill_px;
  fee      = notional * (b->fee_bps / 10000.0);

  signed_pos   = (b->position.side == WM_POS_LONG)  ?  b->position.qty
               : (b->position.side == WM_POS_SHORT) ? -b->position.qty
                                                    :  0.0;
  signed_delta = intent->action == WM_SIZER_BUY ?  intent->qty
                                                : -intent->qty;

  // Decompose the fill into closing-portion + opening-portion. A flip
  // (e.g. long -> short) closes the entire existing position then
  // opens a fresh one in the opposite direction.
  close_qty = 0.0;
  open_qty  = intent->qty;
  realized  = 0.0;

  if(signed_pos != 0.0 && (signed_pos * signed_delta) < 0.0)
  {
    double existing_abs = fabs(signed_pos);

    close_qty = intent->qty <= existing_abs ? intent->qty : existing_abs;
    open_qty  = intent->qty - close_qty;

    // Realized PnL on the closed portion: (entry - exit) * qty for a
    // short close, (exit - entry) * qty for a long close. Equivalent
    // to signed_pos_sign * (fill_px - avg_entry_px) * close_qty.
    {
      double pos_sign = signed_pos > 0.0 ? 1.0 : -1.0;

      realized = pos_sign *
          (fill_px - b->position.avg_entry_px) * close_qty;
    }
  }

  // Apply cash: a buy spends notional + fee, a sell receives notional
  // - fee. Position economics are accounted in the realized PnL on
  // closes; the cash side just tracks the actual transfers.
  if(intent->action == WM_SIZER_BUY)
    b->cash -= notional;
  else
    b->cash += notional;

  b->cash -= fee;

  // Update position. Three cases:
  //   1) close_qty == intent->qty -> partial-or-full close, no open
  //   2) close_qty > 0 && open_qty > 0 -> flip, close then re-open
  //   3) close_qty == 0 -> open / scale in same direction
  if(close_qty > 0.0 && open_qty == 0.0)
  {
    // Pure close.
    if(b->position.side == WM_POS_LONG)
      b->position.qty -= close_qty;
    else
      b->position.qty -= close_qty;

    if(b->position.qty <= 1e-12)
    {
      b->position.side = WM_POS_FLAT;
      b->position.qty  = 0.0;
      b->position.avg_entry_px = 0.0;
      b->position.opened_at_ms = 0;
    }
  }
  else if(close_qty > 0.0 && open_qty > 0.0)
  {
    // Flip. Close existing, open opposite at fill_px.
    b->position.side    = intent->action == WM_SIZER_BUY ? WM_POS_LONG
                                                         : WM_POS_SHORT;
    b->position.qty     = open_qty;
    b->position.avg_entry_px = fill_px;
    b->position.opened_at_ms = ts_ms;
  }
  else
  {
    // Open or scale-in same direction. VWAP entry across the now-larger
    // position (existing notional + new notional) / new total qty.
    double new_qty;
    double new_notional;

    if(b->position.side == WM_POS_FLAT)
    {
      b->position.side    = intent->action == WM_SIZER_BUY ? WM_POS_LONG
                                                           : WM_POS_SHORT;
      b->position.qty     = open_qty;
      b->position.avg_entry_px = fill_px;
      b->position.opened_at_ms = ts_ms;
    }
    else
    {
      new_qty      = b->position.qty + open_qty;
      new_notional = b->position.qty * b->position.avg_entry_px
                   + open_qty * fill_px;
      b->position.qty          = new_qty;
      b->position.avg_entry_px = new_notional / new_qty;
      // opened_at_ms preserved on scale-in.
    }
  }

  signed_pos = (b->position.side == WM_POS_LONG)  ?  b->position.qty
             : (b->position.side == WM_POS_SHORT) ? -b->position.qty
                                                  :  0.0;

  // Append to the fills ring.
  slot = &b->fills[b->fill_head];
  memset(slot, 0, sizeof(*slot));
  slot->ts_ms          = ts_ms;
  slot->side           = intent->action == WM_SIZER_BUY ? 'b' : 's';
  slot->qty            = intent->qty;
  slot->price          = fill_px;
  slot->fee            = fee;
  slot->slippage       = (intent->action == WM_SIZER_BUY ? slip : -slip)
                       * intent->qty;
  slot->realized_pnl   = realized;
  slot->cash_after     = b->cash;
  slot->position_after = signed_pos;
  snprintf(slot->reason, sizeof(slot->reason), "%s", sig->reason);

  b->fill_head = (b->fill_head + 1) % WM_FILL_RING_CAP;
  b->fill_n++;

  // Record into PnL accumulators.
  wm_pnl_acc_record_fee(b->pnl, fee);

  if(close_qty > 0.0)
  {
    close_cost = b->position.avg_entry_px * close_qty;

    // For a flip, avg_entry_px now reflects the new opening side.
    // Fall back to the fill_px so the cost-basis ratio is sensible.
    if(close_qty > 0.0 && open_qty > 0.0)
      close_cost = mark_px * close_qty;

    wm_pnl_acc_record_close(b->pnl, realized, close_cost);
  }

  // Equity sample for drawdown.
  equity = b->cash + signed_pos * mark_px;
  wm_pnl_acc_record_equity(b->pnl, equity);

  clam(CLAM_INFO, WM_TRADE_CTX,
      "%s/%s fill: %c qty=%.6g px=%.6g fee=%.4f realized=%.4f"
      " cash=%.2f pos=%.6g (%s)",
      b->market_id_str, b->strategy_name,
      slot->side, slot->qty, slot->price, slot->fee,
      slot->realized_pnl, slot->cash_after, slot->position_after,
      sig->reason);
}

// ----------------------------------------------------------------------- //
// Signal entry point                                                      //
// ----------------------------------------------------------------------- //

void
wm_trade_engine_on_signal(const char *market_id_str,
    const char *strategy_name, double mark_px, int64_t mark_ms,
    const wm_strategy_signal_t *sig)
{
  wm_trade_registry_t *reg;
  wm_trade_book_t     *b;
  wm_sizer_intent_t    intent;
  double               equity_pre;
  double               signed_pos;

  if(sig == NULL)
    return;

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return;

  pthread_mutex_lock(&reg->lock);

  b = wm_trade_book_find(reg, market_id_str, strategy_name);

  if(b == NULL || b->mode == WM_TRADE_MODE_OFF)
  {
    pthread_mutex_unlock(&reg->lock);
    return;
  }

  // Update the mark cache + record an equity sample for drawdown
  // before any fill so the peak-tracking sees the pre-trade state.
  if(mark_px > 0.0)
  {
    b->last_mark_px = mark_px;
    b->last_mark_ms = mark_ms;

    signed_pos = (b->position.side == WM_POS_LONG)  ?  b->position.qty
               : (b->position.side == WM_POS_SHORT) ? -b->position.qty
                                                    :  0.0;
    equity_pre = b->cash + signed_pos * mark_px;
    wm_pnl_acc_record_equity(b->pnl, equity_pre);
  }

  b->last_signal     = *sig;
  b->has_last_signal = true;

  // Backtest + live are placeholders for WM-LT-5 + WM-LT-8. Fall
  // through to the no-op tail rather than firing a paper fill, so an
  // operator who set a mode that isn't wired yet sees signals
  // recorded but no fills (the safe behaviour).
  if(b->mode != WM_TRADE_MODE_PAPER)
  {
    pthread_mutex_unlock(&reg->lock);
    return;
  }

  if(mark_px <= 0.0)
  {
    pthread_mutex_unlock(&reg->lock);
    return;
  }

  wm_sizer_compute(b, mark_px, sig, &intent);

  if(intent.action != WM_SIZER_HOLD)
  {
    wm_trade_apply_paper_fill_locked(b, &intent, mark_px, sig->ts_ms,
        sig);

    // WM-PT-3: snapshot the post-fill book to wm_trade_book_state.
    // Async via the trade-persist worker; coalesces across rapid
    // fills within one 1 s tick.
    wm_book_persist_locked(reg, b);
  }

  pthread_mutex_unlock(&reg->lock);
}

void
wm_trade_book_update_mark(const char *market_id_str,
    const char *strategy_name, double mark_px, int64_t mark_ms)
{
  wm_trade_registry_t *reg;
  wm_trade_book_t     *b;
  double               signed_pos;
  double               equity;

  if(mark_px <= 0.0)
    return;

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return;

  pthread_mutex_lock(&reg->lock);

  b = wm_trade_book_find(reg, market_id_str, strategy_name);

  if(b != NULL)
  {
    b->last_mark_px = mark_px;
    b->last_mark_ms = mark_ms;

    signed_pos = (b->position.side == WM_POS_LONG)  ?  b->position.qty
               : (b->position.side == WM_POS_SHORT) ? -b->position.qty
                                                    :  0.0;
    equity = b->cash + signed_pos * mark_px;

    wm_pnl_acc_record_equity(b->pnl, equity);
  }

  pthread_mutex_unlock(&reg->lock);
}

// ----------------------------------------------------------------------- //
// Snapshot                                                                //
// ----------------------------------------------------------------------- //

// Fill the snapshot from a book. Caller holds the registry lock.
static void
wm_trade_book_snapshot_locked(const wm_trade_book_t *b,
    wm_trade_snapshot_t *out)
{
  uint32_t to_copy;
  uint32_t i;
  uint32_t src_idx;
  uint32_t start;
  double   signed_pos;

  memset(out, 0, sizeof(*out));

  snprintf(out->market_id_str, sizeof(out->market_id_str),
      "%s", b->market_id_str);
  snprintf(out->strategy_name, sizeof(out->strategy_name),
      "%s", b->strategy_name);

  out->mode             = b->mode;
  out->starting_cash    = b->starting_cash;
  out->cash             = b->cash;
  out->position         = b->position;
  out->fee_bps          = b->fee_bps;
  out->slip_bps         = b->slip_bps;
  out->size_frac        = b->size_frac;
  out->max_position     = b->max_position;
  out->last_mark_px     = b->last_mark_px;
  out->last_mark_ms     = b->last_mark_ms;
  out->last_signal      = b->last_signal;
  out->has_last_signal  = b->has_last_signal;
  out->fill_total       = b->fill_n;

  wm_pnl_compute(b->pnl, &out->metrics);

  signed_pos = (b->position.side == WM_POS_LONG)  ?  b->position.qty
             : (b->position.side == WM_POS_SHORT) ? -b->position.qty
                                                  :  0.0;

  out->equity = b->cash + signed_pos * b->last_mark_px;
  out->unrealized_pnl = signed_pos *
      (b->last_mark_px - b->position.avg_entry_px);

  // Tail of the fills ring (newest last). We always emit the most
  // recent WM_TRADE_SNAPSHOT_FILLS entries, oldest of those first.
  to_copy = b->fill_n < WM_TRADE_SNAPSHOT_FILLS ? (uint32_t)b->fill_n
                                                : WM_TRADE_SNAPSHOT_FILLS;

  if(to_copy > 0)
  {
    // Reconstruct the absolute newest index. The ring's next-write
    // slot is fill_head; the newest entry is at (fill_head - 1) mod cap
    // when the ring has wrapped, or at (fill_n - 1) before wrap.
    if(b->fill_n <= WM_FILL_RING_CAP)
      start = (uint32_t)(b->fill_n) - to_copy;
    else
      start = (b->fill_head + WM_FILL_RING_CAP - to_copy)
            % WM_FILL_RING_CAP;

    for(i = 0; i < to_copy; i++)
    {
      if(b->fill_n <= WM_FILL_RING_CAP)
        src_idx = start + i;
      else
        src_idx = (start + i) % WM_FILL_RING_CAP;

      out->fills[i] = b->fills[src_idx];
    }

    out->n_fills = to_copy;
  }
}

bool
wm_trade_book_snapshot(const char *market_id_str,
    const char *strategy_name, wm_trade_snapshot_t *out)
{
  wm_trade_registry_t *reg;
  wm_trade_book_t     *b;
  bool                 ok = FAIL;

  if(out == NULL)
    return(FAIL);

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return(FAIL);

  pthread_mutex_lock(&reg->lock);

  b = wm_trade_book_find(reg, market_id_str, strategy_name);

  if(b != NULL)
  {
    wm_trade_book_snapshot_locked(b, out);
    ok = SUCCESS;
  }

  pthread_mutex_unlock(&reg->lock);

  return(ok);
}

void
wm_trade_books_iterate(wm_trade_book_iter_cb_t cb, void *user)
{
  wm_trade_registry_t *reg;
  wm_trade_book_t     *b;
  wm_trade_snapshot_t  snap;

  if(cb == NULL)
    return;

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return;

  pthread_mutex_lock(&reg->lock);

  for(b = reg->head; b != NULL; b = b->next)
  {
    wm_trade_book_snapshot_locked(b, &snap);
    cb(&snap, user);
  }

  pthread_mutex_unlock(&reg->lock);
}

uint32_t
wm_trade_books_count(void)
{
  wm_trade_registry_t *reg;
  uint32_t             n;

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return(0);

  pthread_mutex_lock(&reg->lock);
  n = reg->n_books;
  pthread_mutex_unlock(&reg->lock);

  return(n);
}

// ----------------------------------------------------------------------- //
// Reconciliation (WM-PT-2)                                                //
// ----------------------------------------------------------------------- //

// Fold one fill into the running expected_* accumulators. Used by the
// reconcile walker for both full and ring-truncated modes.
static void
wm_trade_reconcile_apply_fill(const wm_fill_t *f,
    double *expected_cash, double *expected_position,
    double *expected_fees)
{
  double signed_qty;

  signed_qty = (f->side == 'b') ?  f->qty
             : (f->side == 's') ? -f->qty
                                :  0.0;

  *expected_cash     -= signed_qty * f->price;
  *expected_cash     -= f->fee;
  *expected_position += signed_qty;
  *expected_fees     += f->fee;
}

static bool
wm_trade_reconcile_within_eps(double delta)
{
  return((delta < 0.0 ? -delta : delta) < WM_TRADE_RECONCILE_EPS);
}

// Reconcile a book against its fills ring. Caller holds the registry
// lock. Pure compute; no allocations beyond the supplied result struct.
static void
wm_trade_book_reconcile_locked(const wm_trade_book_t *b,
    wm_trade_reconcile_t *out)
{
  uint32_t i;
  uint32_t walk;
  uint32_t src_idx;
  double   signed_pos;

  memset(out, 0, sizeof(*out));

  snprintf(out->market_id_str, sizeof(out->market_id_str),
      "%s", b->market_id_str);
  snprintf(out->strategy_name, sizeof(out->strategy_name),
      "%s", b->strategy_name);

  out->fills_total     = b->fill_n;
  out->ring_truncated  = b->fill_n > WM_FILL_RING_CAP;
  out->fees_reconciled = !out->ring_truncated;

  // Live state captured under the lock.
  signed_pos = (b->position.side == WM_POS_LONG)  ?  b->position.qty
             : (b->position.side == WM_POS_SHORT) ? -b->position.qty
                                                  :  0.0;

  out->actual_cash     = b->cash;
  out->actual_position = signed_pos;
  out->actual_fees     = b->pnl != NULL ? b->pnl->fees_paid : 0.0;

  if(!out->ring_truncated)
  {
    // Full walk: every lifetime fill is in the ring, so we start from
    // starting_cash + flat position and apply every fill in order.
    out->expected_cash     = b->starting_cash;
    out->expected_position = 0.0;
    out->expected_fees     = 0.0;

    walk = (uint32_t)b->fill_n;

    for(i = 0; i < walk; i++)
      wm_trade_reconcile_apply_fill(&b->fills[i],
          &out->expected_cash, &out->expected_position,
          &out->expected_fees);

    out->fills_walked = walk;
  }
  else
  {
    // Ring-truncated walk. The ring's oldest live fill sits at the
    // next-write slot (fill_head); everything older was overwritten.
    // Anchor at that fill's (cash_after, position_after) — the state
    // *just after* the oldest fill executed — and replay the cap-1
    // fills that follow it. expected_fees can't be reconstructed from
    // a partial walk against the lifetime fees_paid accumulator, so
    // the fee delta is left unreconciled.
    const wm_fill_t *anchor = &b->fills[b->fill_head];

    out->expected_cash     = anchor->cash_after;
    out->expected_position = anchor->position_after;
    out->expected_fees     = 0.0;

    for(i = 1; i < WM_FILL_RING_CAP; i++)
    {
      src_idx = (b->fill_head + i) % WM_FILL_RING_CAP;
      wm_trade_reconcile_apply_fill(&b->fills[src_idx],
          &out->expected_cash, &out->expected_position,
          &out->expected_fees);
    }

    out->fills_walked = WM_FILL_RING_CAP;
  }

  out->cash_delta     = out->actual_cash     - out->expected_cash;
  out->position_delta = out->actual_position - out->expected_position;
  out->fee_delta      = out->fees_reconciled
      ? out->actual_fees - out->expected_fees : 0.0;

  out->ok = wm_trade_reconcile_within_eps(out->cash_delta) &&
            wm_trade_reconcile_within_eps(out->position_delta) &&
            (!out->fees_reconciled ||
                wm_trade_reconcile_within_eps(out->fee_delta));
}

bool
wm_trade_book_reconcile(const char *market_id_str,
    const char *strategy_name, wm_trade_reconcile_t *out)
{
  wm_trade_registry_t *reg;
  wm_trade_book_t     *b;
  bool                 ok = FAIL;

  if(out == NULL)
    return(FAIL);

  reg = wm_trade_get_active_registry();

  if(reg == NULL)
    return(FAIL);

  pthread_mutex_lock(&reg->lock);

  b = wm_trade_book_find(reg, market_id_str, strategy_name);

  if(b != NULL)
  {
    wm_trade_book_reconcile_locked(b, out);
    ok = SUCCESS;
  }

  pthread_mutex_unlock(&reg->lock);

  return(ok);
}
