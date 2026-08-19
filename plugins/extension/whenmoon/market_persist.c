// botmanager — MIT
// market_persist.c — durable per-market session queue (WM-MK-2).
//
// Mirror of book_persist.c but keyed on `market_id` and serializing
// the new `wm_market_session_t` into a JSONB-rich `wm_market_state`
// row. Producers (`wm_market_apply_fill_locked`, mode/reset paths)
// hand off a fully-formed UPSERT statement keyed by `market_id`.
// Coalescing: a second enqueue for the same key replaces the first
// and frees its prior SQL, so the pending list size is bounded by
// the live market count.
//
// Drain runs on a plugin-global periodic task tick (1 s) AND inline
// at SIGTERM via `wm_market_persist_flush_all` so no snapshot is
// lost across restart. Statements run off-lock.

#define WHENMOON_INTERNAL
#include "market_persist.h"
#include "market.h"
#include "strategy.h"   // WM-SR-2: wm_strategy_seed_replay_cursors
#include "whenmoon.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "db.h"
#include "task.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WM_MP_FLUSH_INTERVAL_MS  1000
#define WM_MP_BUF_INIT_CAP       1024
#define WM_MP_SQL_INIT_CAP       (16 * 1024)
#define WM_MP_JSON_INIT_CAP      (8  * 1024)

// ------------------------------------------------------------------ //
// Buffer helpers (local — see book_persist.c for the symmetric set)  //
// ------------------------------------------------------------------ //

typedef struct
{
  char  *buf;
  size_t off;
  size_t cap;
  bool   oom;
} wm_mp_buf_t;

static bool
wm_mp_buf_grow(wm_mp_buf_t *b, size_t need)
{
  size_t new_cap;

  if(b == NULL || b->oom)
    return(FAIL);

  if(b->off + need + 1 <= b->cap)
    return(SUCCESS);

  new_cap = b->cap > 0 ? b->cap : WM_MP_BUF_INIT_CAP;

  while(new_cap < b->off + need + 1)
    new_cap *= 2;

  b->buf = mem_realloc(b->buf, new_cap);
  b->cap = new_cap;
  return(SUCCESS);
}

static void
wm_mp_buf_putc(wm_mp_buf_t *b, char c)
{
  if(b == NULL || b->oom)
    return;

  if(wm_mp_buf_grow(b, 1) != SUCCESS)
    return;

  b->buf[b->off++] = c;
  b->buf[b->off]   = '\0';
}

static void
wm_mp_buf_puts(wm_mp_buf_t *b, const char *s)
{
  size_t n;

  if(b == NULL || s == NULL || b->oom)
    return;

  n = strlen(s);

  if(wm_mp_buf_grow(b, n) != SUCCESS)
    return;

  memcpy(b->buf + b->off, s, n);
  b->off         += n;
  b->buf[b->off]  = '\0';
}

static void
wm_mp_buf_printf(wm_mp_buf_t *b, const char *fmt, ...)
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

  if(wm_mp_buf_grow(b, (size_t)n) != SUCCESS)
  {
    va_end(ap2);
    return;
  }

  vsnprintf(b->buf + b->off, b->cap - b->off, fmt, ap2);
  va_end(ap2);

  b->off += (size_t)n;
}

static void
wm_mp_json_escape(wm_mp_buf_t *b, const char *src)
{
  const unsigned char *p;

  if(b == NULL || src == NULL)
    return;

  for(p = (const unsigned char *)src; *p != '\0'; p++)
  {
    switch(*p)
    {
      case '"':  wm_mp_buf_puts(b, "\\\"");  break;
      case '\\': wm_mp_buf_puts(b, "\\\\");  break;
      case '\b': wm_mp_buf_puts(b, "\\b");   break;
      case '\f': wm_mp_buf_puts(b, "\\f");   break;
      case '\n': wm_mp_buf_puts(b, "\\n");   break;
      case '\r': wm_mp_buf_puts(b, "\\r");   break;
      case '\t': wm_mp_buf_puts(b, "\\t");   break;
      default:
        if(*p < 0x20)
          wm_mp_buf_printf(b, "\\u%04x", (unsigned)*p);
        else
          wm_mp_buf_putc(b, (char)*p);
        break;
    }
  }
}

static void
wm_mp_emit_json_in_sql(wm_mp_buf_t *out, const char *json)
{
  const char *p;

  if(out == NULL || json == NULL || json[0] == '\0')
  {
    wm_mp_buf_puts(out, "'null'::jsonb");
    return;
  }

  wm_mp_buf_putc(out, '\'');

  for(p = json; *p != '\0'; p++)
  {
    if(*p == '\'')
      wm_mp_buf_putc(out, '\'');

    wm_mp_buf_putc(out, *p);
  }

  wm_mp_buf_puts(out, "'::jsonb");
}

static const char *
wm_mp_pos_side_name(wm_market_position_side_t s)
{
  switch(s)
  {
    case WM_MARKET_POS_FLAT: return("flat");
    case WM_MARKET_POS_LONG: return("long");
  }

  return("flat");
}

static wm_market_position_side_t
wm_mp_pos_side_parse(const char *s)
{
  if(s == NULL)                     return(WM_MARKET_POS_FLAT);
  if(strcmp(s, "long")     == 0)    return(WM_MARKET_POS_LONG);

  return(WM_MARKET_POS_FLAT);
}

// ------------------------------------------------------------------ //
// JSON serialization                                                 //
// ------------------------------------------------------------------ //

static void
wm_mp_json_signal(wm_mp_buf_t *out, const wm_strategy_signal_t *sig)
{
  if(out == NULL)
    return;

  if(sig == NULL)
  {
    wm_mp_buf_puts(out, "null");
    return;
  }

  wm_mp_buf_puts(out, "{\"ts_ms\":");
  wm_mp_buf_printf(out, "%" PRId64, sig->ts_ms);
  wm_mp_buf_puts(out, ",\"score\":");
  wm_mp_buf_printf(out, "%.10g", sig->score);
  wm_mp_buf_puts(out, ",\"confidence\":");
  wm_mp_buf_printf(out, "%.10g", sig->confidence);
  wm_mp_buf_puts(out, ",\"reason\":\"");
  wm_mp_json_escape(out, sig->reason);
  wm_mp_buf_puts(out, "\"}");
}

static void
wm_mp_json_one_fill(wm_mp_buf_t *out, const wm_market_fill_t *f)
{
  wm_mp_buf_puts(out, "{\"ts_ms\":");
  wm_mp_buf_printf(out, "%" PRId64, f->ts_ms);
  wm_mp_buf_puts(out, ",\"side\":\"");
  wm_mp_buf_putc(out, f->side != '\0' ? f->side : 'b');
  wm_mp_buf_puts(out, "\",\"qty\":");
  wm_mp_buf_printf(out, "%.10g", f->qty);
  wm_mp_buf_puts(out, ",\"price\":");
  wm_mp_buf_printf(out, "%.10g", f->price);
  wm_mp_buf_puts(out, ",\"fee\":");
  wm_mp_buf_printf(out, "%.10g", f->fee);
  wm_mp_buf_puts(out, ",\"slippage\":");
  wm_mp_buf_printf(out, "%.10g", f->slippage);
  wm_mp_buf_puts(out, ",\"realized_pnl\":");
  wm_mp_buf_printf(out, "%.10g", f->realized_pnl);
  wm_mp_buf_puts(out, ",\"cash_after\":");
  wm_mp_buf_printf(out, "%.10g", f->cash_after);
  wm_mp_buf_puts(out, ",\"position_after\":");
  wm_mp_buf_printf(out, "%.10g", f->position_after);
  wm_mp_buf_puts(out, ",\"reason\":\"");
  wm_mp_json_escape(out, f->reason);
  wm_mp_buf_puts(out, "\"}");
}

static void
wm_mp_json_fills_ring(wm_mp_buf_t *out, const wm_market_session_t *s,
    wm_market_mode_t mode)
{
  uint32_t i;

  wm_mp_buf_putc(out, '[');

  for(i = 0; i < WM_MARKET_FILL_RING_CAP; i++)
  {
    if(i > 0)
      wm_mp_buf_putc(out, ',');

    wm_mp_json_one_fill(out, &s->fills[mode][i]);
  }

  wm_mp_buf_putc(out, ']');
}

static void
wm_mp_json_one_pending(wm_mp_buf_t *out, const wm_market_pending_t *p)
{
  uint8_t i;

  wm_mp_buf_puts(out, "{\"coid\":\"");
  wm_mp_json_escape(out, p->coid);
  wm_mp_buf_puts(out, "\",\"order_id\":\"");
  wm_mp_json_escape(out, p->order_id);
  wm_mp_buf_puts(out, "\",\"side\":\"");
  wm_mp_json_escape(out, p->side);
  wm_mp_buf_puts(out, "\",\"limit_px\":");
  wm_mp_buf_printf(out, "%.10g", p->limit_px);
  wm_mp_buf_puts(out, ",\"submitted_qty\":");
  wm_mp_buf_printf(out, "%.10g", p->submitted_qty);
  wm_mp_buf_puts(out, ",\"filled_qty\":");
  wm_mp_buf_printf(out, "%.10g", p->filled_qty);
  wm_mp_buf_puts(out, ",\"submitted_ms\":");
  wm_mp_buf_printf(out, "%" PRId64, p->submitted_ms);
  wm_mp_buf_puts(out, ",\"gateway_accepted\":");
  wm_mp_buf_puts(out, p->gateway_accepted ? "true" : "false");
  wm_mp_buf_puts(out, ",\"trade_ids\":[");

  for(i = 0; i < p->n_recorded_trades; i++)
  {
    if(i > 0)
      wm_mp_buf_putc(out, ',');

    wm_mp_buf_printf(out, "%" PRId64, p->recorded_trade_ids[i]);
  }

  wm_mp_buf_puts(out, "]}");
}

static void
wm_mp_json_pending_ring(wm_mp_buf_t *out, const wm_market_session_t *s)
{
  uint32_t i;

  wm_mp_buf_putc(out, '[');

  for(i = 0; i < s->pending_n; i++)
  {
    if(i > 0)
      wm_mp_buf_putc(out, ',');

    wm_mp_json_one_pending(out, &s->pending[i]);
  }

  wm_mp_buf_putc(out, ']');
}

static void
wm_mp_json_one_stats(wm_mp_buf_t *out, const wm_market_stats_t *st)
{
  wm_mp_buf_puts(out, "{\"starting_cash\":");
  wm_mp_buf_printf(out, "%.10g", st->starting_cash);
  wm_mp_buf_puts(out, ",\"cash\":");
  wm_mp_buf_printf(out, "%.10g", st->cash);
  wm_mp_buf_puts(out, ",\"realized_pnl_lifetime\":");
  wm_mp_buf_printf(out, "%.10g", st->realized_pnl_lifetime);
  wm_mp_buf_puts(out, ",\"realized_pnl_today\":");
  wm_mp_buf_printf(out, "%.10g", st->realized_pnl_today);
  wm_mp_buf_puts(out, ",\"daily_anchor_ms\":");
  wm_mp_buf_printf(out, "%" PRId64, st->daily_anchor_ms);
  wm_mp_buf_puts(out, ",\"lifetime_fees\":");
  wm_mp_buf_printf(out, "%.10g", st->lifetime_fees);
  wm_mp_buf_puts(out, ",\"lifetime_fills_count\":");
  wm_mp_buf_printf(out, "%" PRIu64, st->lifetime_fills_count);
  wm_mp_buf_puts(out, ",\"last_fill_ms\":");
  wm_mp_buf_printf(out, "%" PRId64, st->last_fill_ms);
  // WM-MK-6: round-trip + risk counters. Restored on next plugin start
  // so live markets carry forward their P&L profile; the equity-samples
  // ring itself is intentionally NOT persisted (per-iteration synth
  // markets carry their own ring; live markets begin a fresh window
  // after restart, which Sharpe/Sortino computes from there).
  wm_mp_buf_puts(out, ",\"n_trades\":");
  wm_mp_buf_printf(out, "%u", st->n_trades);
  wm_mp_buf_puts(out, ",\"n_wins\":");
  wm_mp_buf_printf(out, "%u", st->n_wins);
  wm_mp_buf_puts(out, ",\"n_losses\":");
  wm_mp_buf_printf(out, "%u", st->n_losses);
  wm_mp_buf_puts(out, ",\"gross_profit\":");
  wm_mp_buf_printf(out, "%.10g", st->gross_profit);
  wm_mp_buf_puts(out, ",\"gross_loss\":");
  wm_mp_buf_printf(out, "%.10g", st->gross_loss);
  wm_mp_buf_puts(out, ",\"max_drawdown\":");
  wm_mp_buf_printf(out, "%.10g", st->max_drawdown);
  wm_mp_buf_puts(out, ",\"equity_peak\":");
  wm_mp_buf_printf(out, "%.10g", st->equity_peak);
  wm_mp_buf_putc(out, '}');
}

// ------------------------------------------------------------------ //
// SQL builder                                                        //
// ------------------------------------------------------------------ //

// Build the heap-allocated UPSERT for the snapshot. Returns NULL on
// OOM or unresolvable id. Caller takes ownership of the returned
// buffer.
static char *
wm_mp_build_upsert_locked(const whenmoon_market_t *mk)
{
  wm_mp_buf_t                stats_paper = { 0 };
  wm_mp_buf_t                stats_real  = { 0 };
  wm_mp_buf_t                fills_paper = { 0 };
  wm_mp_buf_t                fills_real  = { 0 };
  wm_mp_buf_t                pending     = { 0 };
  wm_mp_buf_t                last_signal = { 0 };
  wm_mp_buf_t                sql         = { 0 };
  const wm_market_session_t *s;
  bool                       oom = false;

  if(mk == NULL || mk->market_id < 0)
    return(NULL);

  s = &mk->session;

  stats_paper.buf = mem_alloc("whenmoon", "mp_json_stats_p", WM_MP_JSON_INIT_CAP);
  stats_real.buf  = mem_alloc("whenmoon", "mp_json_stats_r", WM_MP_JSON_INIT_CAP);
  fills_paper.buf = mem_alloc("whenmoon", "mp_json_fills_p", WM_MP_JSON_INIT_CAP);
  fills_real.buf  = mem_alloc("whenmoon", "mp_json_fills_r", WM_MP_JSON_INIT_CAP);
  pending.buf     = mem_alloc("whenmoon", "mp_json_pending", WM_MP_JSON_INIT_CAP);
  last_signal.buf = mem_alloc("whenmoon", "mp_json_signal",  WM_MP_BUF_INIT_CAP);
  sql.buf         = mem_alloc("whenmoon", "mp_sql",          WM_MP_SQL_INIT_CAP);

  stats_paper.cap = WM_MP_JSON_INIT_CAP;
  stats_real.cap  = WM_MP_JSON_INIT_CAP;
  fills_paper.cap = WM_MP_JSON_INIT_CAP;
  fills_real.cap  = WM_MP_JSON_INIT_CAP;
  pending.cap     = WM_MP_JSON_INIT_CAP;
  last_signal.cap = WM_MP_BUF_INIT_CAP;
  sql.cap         = WM_MP_SQL_INIT_CAP;

  // Initialise zero-len strings.
  stats_paper.buf[0] = '\0';
  stats_real.buf[0]  = '\0';
  fills_paper.buf[0] = '\0';
  fills_real.buf[0]  = '\0';
  pending.buf[0]     = '\0';
  last_signal.buf[0] = '\0';
  sql.buf[0]         = '\0';

  wm_mp_json_one_stats(&stats_paper, &s->stats[WM_MARKET_MODE_PAPER]);
  wm_mp_json_one_stats(&stats_real,  &s->stats[WM_MARKET_MODE_REAL]);

  wm_mp_json_fills_ring(&fills_paper, s, WM_MARKET_MODE_PAPER);
  wm_mp_json_fills_ring(&fills_real,  s, WM_MARKET_MODE_REAL);

  wm_mp_json_pending_ring(&pending, s);

  if(s->has_last_acted_signal)
    wm_mp_json_signal(&last_signal, &s->last_acted_signal);
  else
    wm_mp_buf_puts(&last_signal, "null");

  if(stats_paper.oom || stats_real.oom || fills_paper.oom ||
     fills_real.oom || pending.oom || last_signal.oom)
  {
    oom = true;
    goto out;
  }

  // Build the UPSERT.
  wm_mp_buf_puts(&sql,
      "INSERT INTO wm_market_state ("
      "market_id, instance, enabled, mode,"
      " position_side, position_qty, position_avg,"
      " position_opened_ms,"
      " stats_paper, stats_real, fills_paper, fills_real, pending,"
      " last_mark_px, last_mark_ms, last_signal, has_last_signal,"
      " fills_n_paper, fills_head_paper,"
      " fills_n_real, fills_head_real,"
      " fee_bps, slip_bps, size_frac, max_notional, daily_loss_bps,"
      " pending_cap, pending_n)"
      " VALUES (");

  wm_mp_buf_printf(&sql, "%" PRId32 ",", mk->market_id);
  // WM-MI-2: instance label is validated [a-z0-9_] on the add path, so
  // it needs no SQL escaping. A running upsert always carries the
  // instance into the running set (enabled=TRUE); the LAST-instance stop
  // is the only writer of enabled=FALSE (wm_market_persist_disable).
  wm_mp_buf_printf(&sql, "'%s',", mk->instance);
  wm_mp_buf_puts(&sql, "TRUE,");
  wm_mp_buf_printf(&sql, "'%s',", wm_market_mode_name(s->mode));
  wm_mp_buf_printf(&sql, "'%s',", wm_mp_pos_side_name(s->position.side));
  wm_mp_buf_printf(&sql, "%.10g,", s->position.qty);
  wm_mp_buf_printf(&sql, "%.10g,", s->position.avg_entry_px);
  wm_mp_buf_printf(&sql, "%" PRId64 ",", s->position.opened_at_ms);

  wm_mp_emit_json_in_sql(&sql, stats_paper.buf); wm_mp_buf_putc(&sql, ',');
  wm_mp_emit_json_in_sql(&sql, stats_real.buf);  wm_mp_buf_putc(&sql, ',');
  wm_mp_emit_json_in_sql(&sql, fills_paper.buf); wm_mp_buf_putc(&sql, ',');
  wm_mp_emit_json_in_sql(&sql, fills_real.buf);  wm_mp_buf_putc(&sql, ',');
  wm_mp_emit_json_in_sql(&sql, pending.buf);     wm_mp_buf_putc(&sql, ',');

  wm_mp_buf_printf(&sql, "%.10g,",     s->last_mark_px);
  wm_mp_buf_printf(&sql, "%" PRId64 ",", s->last_mark_ms);

  wm_mp_emit_json_in_sql(&sql, last_signal.buf);
  wm_mp_buf_putc(&sql, ',');

  wm_mp_buf_puts(&sql, s->has_last_acted_signal ? "TRUE," : "FALSE,");

  wm_mp_buf_printf(&sql, "%" PRIu64 ",", s->fills_n[WM_MARKET_MODE_PAPER]);
  wm_mp_buf_printf(&sql, "%u,",          s->fills_head[WM_MARKET_MODE_PAPER]);
  wm_mp_buf_printf(&sql, "%" PRIu64 ",", s->fills_n[WM_MARKET_MODE_REAL]);
  wm_mp_buf_printf(&sql, "%u,",          s->fills_head[WM_MARKET_MODE_REAL]);

  wm_mp_buf_printf(&sql, "%.10g,", s->fee_bps);
  wm_mp_buf_printf(&sql, "%.10g,", s->slip_bps);
  wm_mp_buf_printf(&sql, "%.10g,", s->size_frac);
  wm_mp_buf_printf(&sql, "%.10g,", s->max_notional);
  wm_mp_buf_printf(&sql, "%.10g,", s->daily_loss_bps);
  wm_mp_buf_printf(&sql, "%u,",    s->pending_cap);
  wm_mp_buf_printf(&sql, "%u",     s->pending_n);

  wm_mp_buf_puts(&sql, ") ON CONFLICT (market_id, instance) DO UPDATE SET"
      " enabled = EXCLUDED.enabled,"
      " mode = EXCLUDED.mode,"
      " position_side = EXCLUDED.position_side,"
      " position_qty = EXCLUDED.position_qty,"
      " position_avg = EXCLUDED.position_avg,"
      " position_opened_ms = EXCLUDED.position_opened_ms,"
      " stats_paper = EXCLUDED.stats_paper,"
      " stats_real = EXCLUDED.stats_real,"
      " fills_paper = EXCLUDED.fills_paper,"
      " fills_real = EXCLUDED.fills_real,"
      " pending = EXCLUDED.pending,"
      " last_mark_px = EXCLUDED.last_mark_px,"
      " last_mark_ms = EXCLUDED.last_mark_ms,"
      " last_signal = EXCLUDED.last_signal,"
      " has_last_signal = EXCLUDED.has_last_signal,"
      " fills_n_paper = EXCLUDED.fills_n_paper,"
      " fills_head_paper = EXCLUDED.fills_head_paper,"
      " fills_n_real = EXCLUDED.fills_n_real,"
      " fills_head_real = EXCLUDED.fills_head_real,"
      " fee_bps = EXCLUDED.fee_bps,"
      " slip_bps = EXCLUDED.slip_bps,"
      " size_frac = EXCLUDED.size_frac,"
      " max_notional = EXCLUDED.max_notional,"
      " daily_loss_bps = EXCLUDED.daily_loss_bps,"
      " pending_cap = EXCLUDED.pending_cap,"
      " pending_n = EXCLUDED.pending_n,"
      " updated_at = NOW()");

  if(sql.oom)
    oom = true;

out:
  if(stats_paper.buf != NULL) mem_free(stats_paper.buf);
  if(stats_real.buf  != NULL) mem_free(stats_real.buf);
  if(fills_paper.buf != NULL) mem_free(fills_paper.buf);
  if(fills_real.buf  != NULL) mem_free(fills_real.buf);
  if(pending.buf     != NULL) mem_free(pending.buf);
  if(last_signal.buf != NULL) mem_free(last_signal.buf);

  if(oom)
  {
    if(sql.buf != NULL) mem_free(sql.buf);
    return(NULL);
  }

  return(sql.buf);
}

// ------------------------------------------------------------------ //
// Pending queue                                                      //
// ------------------------------------------------------------------ //

typedef struct wm_mp_entry
{
  int32_t              market_id;
  // WM-MI-2: instances share a market_id but persist independent rows,
  // so the coalescing key is (market_id, instance) — keying on market_id
  // alone would let one instance's upsert clobber a peer's still-pending
  // one before the drain runs.
  char                 instance[WM_INSTANCE_LABEL_SZ];
  char                *sql;       // owned; mem_free on drain
  struct wm_mp_entry  *next;
} wm_mp_entry_t;

static pthread_mutex_t  wm_mp_g_lock = PTHREAD_MUTEX_INITIALIZER;
static wm_mp_entry_t   *wm_mp_g_head = NULL;
static task_handle_t    wm_mp_g_task = TASK_HANDLE_NONE;

static wm_mp_entry_t *
wm_mp_find_locked(int32_t market_id, const char *instance)
{
  wm_mp_entry_t *e;

  for(e = wm_mp_g_head; e != NULL; e = e->next)
  {
    if(e->market_id == market_id && strcmp(e->instance, instance) == 0)
      return(e);
  }

  return(NULL);
}

static bool
wm_mp_enqueue_owned(int32_t market_id, const char *instance,
    char *sql_owned)
{
  wm_mp_entry_t *e;

  if(sql_owned == NULL || market_id < 0)
    return(FAIL);

  if(instance == NULL)
    instance = "";

  pthread_mutex_lock(&wm_mp_g_lock);

  e = wm_mp_find_locked(market_id, instance);

  if(e != NULL)
  {
    if(e->sql != NULL)
      mem_free(e->sql);

    e->sql = sql_owned;
    pthread_mutex_unlock(&wm_mp_g_lock);
    return(SUCCESS);
  }

  e = mem_alloc("whenmoon", "mp_entry", sizeof(*e));

  memset(e, 0, sizeof(*e));
  e->market_id = market_id;
  snprintf(e->instance, sizeof(e->instance), "%s", instance);
  e->sql       = sql_owned;
  e->next      = wm_mp_g_head;
  wm_mp_g_head = e;

  pthread_mutex_unlock(&wm_mp_g_lock);
  return(SUCCESS);
}

bool
wm_market_persist_locked(whenmoon_market_t *mk)
{
  char *sql;

  if(mk == NULL || mk->market_id < 0)
    return(FAIL);

  sql = wm_mp_build_upsert_locked(mk);

  if(sql == NULL)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "market %s: persist build failed (OOM)", mk->market_id_str);
    return(FAIL);
  }

  if(wm_mp_enqueue_owned(mk->market_id, mk->instance, sql) != SUCCESS)
  {
    mem_free(sql);
    return(FAIL);
  }

  return(SUCCESS);
}

bool
wm_market_persist_disable(int32_t market_id, const char *instance)
{
  char  *sql;
  size_t cap = 160;

  if(market_id < 0)
    return(FAIL);

  if(instance == NULL)
    instance = "";

  sql = mem_alloc("whenmoon", "mp_disable_sql", cap);

  // WM-MI-2: drop this instance out of the running set but KEEP the row
  // so its final paper P&L stays inspectable. The instance label is
  // validated [a-z0-9_], so it needs no SQL escaping. A later re-start of
  // the same instance overwrites the row from a freshly-init session
  // (wm_market_persist_locked with enabled=TRUE), so no stale ledger
  // resurrects.
  snprintf(sql, cap,
      "UPDATE wm_market_state SET enabled = FALSE"
      " WHERE market_id = %" PRId32 " AND instance = '%s'",
      market_id, instance);

  if(wm_mp_enqueue_owned(market_id, instance, sql) != SUCCESS)
  {
    mem_free(sql);
    return(FAIL);
  }

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Drain                                                              //
// ------------------------------------------------------------------ //

static void
wm_mp_drain_locked(wm_mp_entry_t **out)
{
  if(out == NULL)
    return;

  *out          = wm_mp_g_head;
  wm_mp_g_head  = NULL;
}

static void
wm_mp_run_drained(wm_mp_entry_t *head)
{
  wm_mp_entry_t *e;
  wm_mp_entry_t *next;
  db_result_t   *res;

  for(e = head; e != NULL; e = next)
  {
    next = e->next;

    if(e->sql != NULL)
    {
      res = db_result_alloc();

      if(db_query(e->sql, res) != SUCCESS || !res->ok)
        clam(CLAM_WARN, WHENMOON_CTX,
            "market-state persist failed (market_id=%" PRId32 "): %s",
            e->market_id,
            res->error[0] != '\0' ? res->error : "(no driver error)");

      else
        clam(CLAM_DEBUG2, WHENMOON_CTX,
            "market-state persisted (market_id=%" PRId32 ", affected=%u)",
            e->market_id, res->rows_affected);

      db_result_free(res);

      mem_free(e->sql);
    }

    mem_free(e);
  }
}

void
wm_market_persist_flush_all(void)
{
  wm_mp_entry_t *drained = NULL;

  pthread_mutex_lock(&wm_mp_g_lock);
  wm_mp_drain_locked(&drained);
  pthread_mutex_unlock(&wm_mp_g_lock);

  if(drained != NULL)
    wm_mp_run_drained(drained);
}

static void
wm_mp_flush_task(task_t *t)
{
  wm_mp_entry_t *drained = NULL;

  if(t == NULL)
    return;

  pthread_mutex_lock(&wm_mp_g_lock);
  wm_mp_drain_locked(&drained);
  pthread_mutex_unlock(&wm_mp_g_lock);

  if(drained != NULL)
    wm_mp_run_drained(drained);

  t->state = TASK_ENDED;
}

bool
wm_market_persist_global_init(void)
{
  if(wm_mp_g_task != TASK_HANDLE_NONE)
    return(SUCCESS);

  wm_mp_g_task = task_add_periodic("wm_market_flush", TASK_ANY, 200,
      WM_MP_FLUSH_INTERVAL_MS, wm_mp_flush_task, NULL);

  if(wm_mp_g_task == TASK_HANDLE_NONE)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "market-persist flush task submit failed");
    return(FAIL);
  }

  return(SUCCESS);
}

void
wm_market_persist_global_stop(void)
{
  if(wm_mp_g_task == TASK_HANDLE_NONE)
    return;

  task_cancel(wm_mp_g_task);
  wm_mp_g_task = TASK_HANDLE_NONE;
}

void
wm_market_persist_global_destroy(void)
{
  wm_market_persist_flush_all();
  wm_market_persist_global_stop();
}

// ------------------------------------------------------------------ //
// Restore                                                            //
// ------------------------------------------------------------------ //

#include <json-c/json.h>

static double
wm_mp_jdouble(struct json_object *obj, const char *key, double def)
{
  struct json_object *v;

  if(obj == NULL || !json_object_object_get_ex(obj, key, &v))
    return(def);

  if(json_object_is_type(v, json_type_double))
    return(json_object_get_double(v));

  if(json_object_is_type(v, json_type_int))
    return((double)json_object_get_int64(v));

  return(def);
}

static int64_t
wm_mp_jint64(struct json_object *obj, const char *key, int64_t def)
{
  struct json_object *v;

  if(obj == NULL || !json_object_object_get_ex(obj, key, &v))
    return(def);

  if(json_object_is_type(v, json_type_int))
    return(json_object_get_int64(v));

  return(def);
}

static const char *
wm_mp_jstr(struct json_object *obj, const char *key)
{
  struct json_object *v;

  if(obj == NULL || !json_object_object_get_ex(obj, key, &v))
    return(NULL);

  if(json_object_is_type(v, json_type_string))
    return(json_object_get_string(v));

  return(NULL);
}

static void
wm_mp_load_stats(struct json_object *obj, wm_market_stats_t *out)
{
  if(obj == NULL || out == NULL)
    return;

  out->starting_cash         = wm_mp_jdouble(obj, "starting_cash",         out->starting_cash);
  out->cash                  = wm_mp_jdouble(obj, "cash",                  out->cash);
  out->realized_pnl_lifetime = wm_mp_jdouble(obj, "realized_pnl_lifetime", 0.0);
  out->realized_pnl_today    = wm_mp_jdouble(obj, "realized_pnl_today",    0.0);
  out->daily_anchor_ms       = wm_mp_jint64 (obj, "daily_anchor_ms",       0);
  out->lifetime_fees         = wm_mp_jdouble(obj, "lifetime_fees",         0.0);
  out->lifetime_fills_count  = (uint64_t)wm_mp_jint64(obj,
      "lifetime_fills_count", 0);
  out->last_fill_ms          = wm_mp_jint64 (obj, "last_fill_ms",          0);
  // WM-MK-6: round-trip + risk counters. Missing keys default to 0
  // so a pre-WM-MK-6 row restores as a clean slate (counters rebuild
  // as new closing fills land).
  out->n_trades              = (uint32_t)wm_mp_jint64(obj, "n_trades", 0);
  out->n_wins                = (uint32_t)wm_mp_jint64(obj, "n_wins",   0);
  out->n_losses              = (uint32_t)wm_mp_jint64(obj, "n_losses", 0);
  out->gross_profit          = wm_mp_jdouble(obj, "gross_profit", 0.0);
  out->gross_loss            = wm_mp_jdouble(obj, "gross_loss",   0.0);
  out->max_drawdown          = wm_mp_jdouble(obj, "max_drawdown", 0.0);
  out->equity_peak           = wm_mp_jdouble(obj, "equity_peak",  0.0);
}

static void
wm_mp_load_one_fill(struct json_object *obj, wm_market_fill_t *out)
{
  const char *side;

  if(obj == NULL || out == NULL)
    return;

  out->ts_ms          = wm_mp_jint64 (obj, "ts_ms", 0);

  side                = wm_mp_jstr(obj, "side");
  out->side           = (side != NULL && side[0] != '\0') ? side[0] : '\0';

  out->qty            = wm_mp_jdouble(obj, "qty",            0.0);
  out->price          = wm_mp_jdouble(obj, "price",          0.0);
  out->fee            = wm_mp_jdouble(obj, "fee",            0.0);
  out->slippage       = wm_mp_jdouble(obj, "slippage",       0.0);
  out->realized_pnl   = wm_mp_jdouble(obj, "realized_pnl",   0.0);
  out->cash_after     = wm_mp_jdouble(obj, "cash_after",     0.0);
  out->position_after = wm_mp_jdouble(obj, "position_after", 0.0);

  side = wm_mp_jstr(obj, "reason");

  if(side != NULL)
    snprintf(out->reason, sizeof(out->reason), "%s", side);
  else
    out->reason[0] = '\0';
}

static void
wm_mp_load_fills_ring(struct json_object *arr, wm_market_session_t *s,
    wm_market_mode_t mode)
{
  size_t                len;
  size_t                i;
  struct json_object   *one;

  if(arr == NULL || !json_object_is_type(arr, json_type_array))
    return;

  len = json_object_array_length(arr);

  if(len > WM_MARKET_FILL_RING_CAP)
    len = WM_MARKET_FILL_RING_CAP;

  for(i = 0; i < len; i++)
  {
    one = json_object_array_get_idx(arr, i);
    wm_mp_load_one_fill(one, &s->fills[mode][i]);
  }
}

static void
wm_mp_load_one_pending(struct json_object *obj, wm_market_pending_t *out)
{
  struct json_object *trade_arr;
  const char         *s;
  size_t              len;
  size_t              i;

  if(obj == NULL || out == NULL)
    return;

  s = wm_mp_jstr(obj, "coid");
  if(s != NULL) snprintf(out->coid, sizeof(out->coid), "%s", s);

  s = wm_mp_jstr(obj, "order_id");
  if(s != NULL) snprintf(out->order_id, sizeof(out->order_id), "%s", s);

  s = wm_mp_jstr(obj, "side");
  if(s != NULL) snprintf(out->side, sizeof(out->side), "%s", s);

  out->limit_px         = wm_mp_jdouble(obj, "limit_px",         0.0);
  out->submitted_qty    = wm_mp_jdouble(obj, "submitted_qty",    0.0);
  out->filled_qty       = wm_mp_jdouble(obj, "filled_qty",       0.0);
  out->submitted_ms     = wm_mp_jint64 (obj, "submitted_ms",     0);

  if(json_object_object_get_ex(obj, "gateway_accepted", &trade_arr))
    out->gateway_accepted = json_object_get_boolean(trade_arr);

  if(json_object_object_get_ex(obj, "trade_ids", &trade_arr) &&
     json_object_is_type(trade_arr, json_type_array))
  {
    len = json_object_array_length(trade_arr);

    if(len > WM_MARKET_TRADE_DEDUP)
      len = WM_MARKET_TRADE_DEDUP;

    for(i = 0; i < len; i++)
    {
      struct json_object *e = json_object_array_get_idx(trade_arr, i);

      if(json_object_is_type(e, json_type_int))
        out->recorded_trade_ids[i] = json_object_get_int64(e);
    }

    out->n_recorded_trades = (uint8_t)len;
  }
}

static void
wm_mp_load_pending_ring(struct json_object *arr, wm_market_session_t *s)
{
  size_t              len;
  size_t              i;
  struct json_object *one;

  if(arr == NULL || !json_object_is_type(arr, json_type_array))
    return;

  len = json_object_array_length(arr);

  if(len > WM_MARKET_PENDING_CAP)
    len = WM_MARKET_PENDING_CAP;

  for(i = 0; i < len; i++)
  {
    one = json_object_array_get_idx(arr, i);
    wm_mp_load_one_pending(one, &s->pending[i]);
  }

  s->pending_n = (uint32_t)len;
}

static void
wm_mp_load_signal(struct json_object *obj, wm_market_session_t *s,
    bool has_signal)
{
  const char *str;

  if(!has_signal || obj == NULL || json_object_is_type(obj, json_type_null))
  {
    s->has_last_acted_signal = false;
    memset(&s->last_acted_signal, 0, sizeof(s->last_acted_signal));
    return;
  }

  s->last_acted_signal.ts_ms      = wm_mp_jint64 (obj, "ts_ms",      0);
  s->last_acted_signal.score      = wm_mp_jdouble(obj, "score",      0.0);
  s->last_acted_signal.confidence = wm_mp_jdouble(obj, "confidence", 0.0);

  str = wm_mp_jstr(obj, "reason");

  if(str != NULL)
    snprintf(s->last_acted_signal.reason,
        sizeof(s->last_acted_signal.reason), "%s", str);
  else
    s->last_acted_signal.reason[0] = '\0';

  s->has_last_acted_signal = true;
}

static struct json_object *
wm_mp_parse_json_or_null(const char *raw)
{
  if(raw == NULL || raw[0] == '\0')
    return(NULL);

  return(json_tokener_parse(raw));
}

bool
wm_market_persist_restore_all(whenmoon_state_t *st)
{
  db_result_t        *res;
  bool                ok = SUCCESS;
  uint32_t            n_restored = 0;
  uint32_t            n_skipped  = 0;
  uint32_t            i;

  if(st == NULL || st->markets == NULL)
    return(FAIL);

  res = db_result_alloc();

  if(db_query(
         "SELECT market_id, mode, position_side, position_qty,"
         " position_avg, position_opened_ms,"
         " stats_paper::text, stats_real::text,"
         " fills_paper::text, fills_real::text, pending::text,"
         " last_mark_px, last_mark_ms,"
         " last_signal::text, has_last_signal,"
         " fills_n_paper, fills_head_paper,"
         " fills_n_real, fills_head_real,"
         " fee_bps, slip_bps, size_frac, max_notional,"
         " daily_loss_bps, pending_cap, pending_n,"
         " instance"   // WM-MI-2: col 26, matched against mk->instance
         "  FROM wm_market_state", res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "market_state restore query failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");
    ok = FAIL;
    goto out;
  }

  for(i = 0; i < res->rows; i++)
  {
    int32_t              market_id;
    const char          *cell;
    whenmoon_market_t   *mk = NULL;
    wm_market_mode_t     mode;
    uint32_t             j;
    struct json_object  *jstats_paper;
    struct json_object  *jstats_real;
    struct json_object  *jfills_paper;
    struct json_object  *jfills_real;
    struct json_object  *jpending;
    struct json_object  *jsignal;
    bool                 has_signal;

    cell = db_result_get(res, i, 0);

    if(cell == NULL)
      continue;

    market_id = (int32_t)strtol(cell, NULL, 10);

    // WM-MI-2: match on the (market_id, instance) pair — a bare
    // market_id match would hydrate the wrong instance's session onto a
    // peer sharing the same wm_market row.
    //
    // WM-MKT-ARR-UAF-1: hold rdlock across the match walk AND the mk->lock
    // hydrate below — the resolved `mk` must not be freed/moved by a
    // concurrent remove while we hydrate it. Released at every exit of this
    // row iteration once acquired.
    pthread_rwlock_rdlock(&st->markets->arr_lock);

    {
      const char *inst = db_result_get(res, i, 26);

      if(inst == NULL)
        inst = "";

      // Look up the running instance by (id, label) (linear walk; small N).
      for(j = 0; j < st->markets->n_markets; j++)
      {
        if(st->markets->arr[j]->market_id == market_id &&
           strcmp(st->markets->arr[j]->instance, inst) == 0)
        {
          mk = st->markets->arr[j];
          break;
        }
      }
    }

    if(mk == NULL)
    {
      pthread_rwlock_unlock(&st->markets->arr_lock);
      n_skipped++;
      continue;
    }

    pthread_mutex_lock(&mk->lock);

    cell = db_result_get(res, i, 1);
    if(cell != NULL && wm_market_mode_parse(cell, &mode) == SUCCESS)
      mk->session.mode = mode;

    cell = db_result_get(res, i, 2);
    mk->session.position.side = wm_mp_pos_side_parse(cell);

    cell = db_result_get(res, i, 3);
    mk->session.position.qty = (cell != NULL) ? strtod(cell, NULL) : 0.0;

    cell = db_result_get(res, i, 4);
    mk->session.position.avg_entry_px = (cell != NULL)
        ? strtod(cell, NULL) : 0.0;

    cell = db_result_get(res, i, 5);
    mk->session.position.opened_at_ms = (cell != NULL)
        ? (int64_t)strtoll(cell, NULL, 10) : 0;

    jstats_paper = wm_mp_parse_json_or_null(db_result_get(res, i, 6));
    jstats_real  = wm_mp_parse_json_or_null(db_result_get(res, i, 7));
    jfills_paper = wm_mp_parse_json_or_null(db_result_get(res, i, 8));
    jfills_real  = wm_mp_parse_json_or_null(db_result_get(res, i, 9));
    jpending     = wm_mp_parse_json_or_null(db_result_get(res, i, 10));

    wm_mp_load_stats(jstats_paper, &mk->session.stats[WM_MARKET_MODE_PAPER]);
    wm_mp_load_stats(jstats_real,  &mk->session.stats[WM_MARKET_MODE_REAL]);
    wm_mp_load_fills_ring(jfills_paper, &mk->session, WM_MARKET_MODE_PAPER);
    wm_mp_load_fills_ring(jfills_real,  &mk->session, WM_MARKET_MODE_REAL);
    wm_mp_load_pending_ring(jpending,   &mk->session);

    cell = db_result_get(res, i, 11);
    mk->session.last_mark_px = (cell != NULL) ? strtod(cell, NULL) : 0.0;

    cell = db_result_get(res, i, 12);
    mk->session.last_mark_ms = (cell != NULL)
        ? (int64_t)strtoll(cell, NULL, 10) : 0;

    cell       = db_result_get(res, i, 14);
    has_signal = (cell != NULL && (cell[0] == 't' || cell[0] == 'T' ||
                                   cell[0] == '1'));
    jsignal    = wm_mp_parse_json_or_null(db_result_get(res, i, 13));
    wm_mp_load_signal(jsignal, &mk->session, has_signal);

    cell = db_result_get(res, i, 15);
    mk->session.fills_n[WM_MARKET_MODE_PAPER] = (cell != NULL)
        ? (uint64_t)strtoull(cell, NULL, 10) : 0;

    cell = db_result_get(res, i, 16);
    mk->session.fills_head[WM_MARKET_MODE_PAPER] = (cell != NULL)
        ? (uint32_t)strtoul(cell, NULL, 10) : 0;

    cell = db_result_get(res, i, 17);
    mk->session.fills_n[WM_MARKET_MODE_REAL] = (cell != NULL)
        ? (uint64_t)strtoull(cell, NULL, 10) : 0;

    cell = db_result_get(res, i, 18);
    mk->session.fills_head[WM_MARKET_MODE_REAL] = (cell != NULL)
        ? (uint32_t)strtoul(cell, NULL, 10) : 0;

    cell = db_result_get(res, i, 19);
    if(cell != NULL) mk->session.fee_bps = strtod(cell, NULL);

    cell = db_result_get(res, i, 20);
    if(cell != NULL) mk->session.slip_bps = strtod(cell, NULL);

    cell = db_result_get(res, i, 21);
    if(cell != NULL) mk->session.size_frac = strtod(cell, NULL);

    cell = db_result_get(res, i, 22);
    if(cell != NULL) mk->session.max_notional = strtod(cell, NULL);

    cell = db_result_get(res, i, 23);
    if(cell != NULL) mk->session.daily_loss_bps = strtod(cell, NULL);

    cell = db_result_get(res, i, 24);
    if(cell != NULL) mk->session.pending_cap = (uint32_t)strtoul(cell, NULL, 10);

    cell = db_result_get(res, i, 25);
    if(cell != NULL) mk->session.pending_n = (uint32_t)strtoul(cell, NULL, 10);

    pthread_mutex_unlock(&mk->lock);

    // WM-SR-2: the session is now hydrated, so re-seed the replay cursor on
    // every attachment wm_market_restore created. Must happen here — after
    // the hydrate, before warmup replay feeds a bar — because the seed taken
    // at attach time ran against a zeroed session. Called with mk->lock
    // released (it takes mk->lock itself) but still under arr_lock, which is
    // what keeps `mk` alive.
    wm_strategy_seed_replay_cursors(st, mk);

    // WM-MKT-ARR-UAF-1: last use of `mk` done — release the container rdlock.
    pthread_rwlock_unlock(&st->markets->arr_lock);

    if(jstats_paper != NULL) json_object_put(jstats_paper);
    if(jstats_real  != NULL) json_object_put(jstats_real);
    if(jfills_paper != NULL) json_object_put(jfills_paper);
    if(jfills_real  != NULL) json_object_put(jfills_real);
    if(jpending     != NULL) json_object_put(jpending);
    if(jsignal      != NULL) json_object_put(jsignal);

    n_restored++;
  }

  clam(CLAM_INFO, WHENMOON_CTX,
      "market_state: %u session(s) restored, %u skipped (not running)",
      n_restored, n_skipped);

out:
  db_result_free(res);

  return(ok);
}
