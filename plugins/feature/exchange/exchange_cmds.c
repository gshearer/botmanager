// botmanager — MIT
// /exchange operator verbs (MW-1 diagnostic surface).
//
// Today this file owns one verb: `/exchange tickers <exch>` — a thin
// admin-only diagnostic that dumps the bulk-ticker snapshot table for
// the named exchange.
//
// Reply discipline: the command dispatch task waits on a pthread_cond_t
// for the underlying `exchange_fetch_all_tickers_async` callback to
// fire, then renders the snapshot via `cmd_reply`. This is required for
// botmanctl, whose protocol driver routes `cmd_reply` / `method_send`
// to the requesting client only while the dispatch task is on the call
// stack (`bctl_reply_target` in core/botmanctl.c is set only between
// cmd_dispatch_as begin/end). An async-from-curl-thread reply path
// would arrive after the dispatch task returns and would be silently
// dropped. The fetch typically completes in ~150-700 ms (single REST
// hit + token-bucket dispatch); the wait is hard-capped at 30 s with a
// timeout reply so a wedged exchange never leaves the worker thread
// blocked forever.
//
// MW-2..MW-5 will add stateful marketwatch verbs under
// `/whenmoon mw` / `/show whenmoon mw`; this file stays narrowly scoped
// to exchange-level diagnostics.

#define EXCHANGE_INTERNAL
#include "exchange.h"

#include "cmd.h"
#include "method.h"
#include "userns.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define EXCH_TICKERS_REPLY_CAP   50
#define EXCH_TICKERS_LINE_SZ    256
#define EXCH_TICKERS_WAIT_MS  60000

// Heap-owned rendezvous between the dispatch task (waiter) and the
// curl worker (signaller). Two refs: the dispatch task drops one when
// it gives up (success render or timeout); the cb drops one when it
// fires. The rendezvous self-frees once both refs are gone. This
// avoids the stack-rendezvous use-after-free hazard on a late-arriving
// cb.
typedef struct
{
  pthread_mutex_t              mu;
  pthread_cond_t               cv;
  uint32_t                     refs;     // 2 at birth (waiter + cb)
  bool                         done;
  bool                         success;
  char                         err[EXCHANGE_ERR_SZ];
  exchange_ticker_snapshot_t  *rows;     // heap, mem_alloc; rendezvous-owned
  size_t                       n;
} exch_tickers_wait_t;

// Drop one ref under the lock; free when the count hits zero. Caller
// must NOT hold w->mu (the function takes it). Returns true when the
// rendezvous was freed so the caller knows not to touch *w further.
static bool
exch_tickers_wait_release(exch_tickers_wait_t *w)
{
  uint32_t left;

  if(w == NULL)
    return(false);

  pthread_mutex_lock(&w->mu);
  w->refs--;
  left = w->refs;
  pthread_mutex_unlock(&w->mu);

  if(left > 0)
    return(false);

  if(w->rows != NULL)
    mem_free(w->rows);
  pthread_cond_destroy (&w->cv);
  pthread_mutex_destroy(&w->mu);
  mem_free(w);
  return(true);
}

static const char *
exch_tickers_status_name(exchange_ticker_status_t s)
{
  switch(s)
  {
    case EXCH_TICK_ONLINE:     return("online");
    case EXCH_TICK_OFFLINE:    return("offline");
    case EXCH_TICK_LIMIT_ONLY: return("limit_only");
    case EXCH_TICK_POST_ONLY:  return("post_only");
    case EXCH_TICK_UNKNOWN:
    default:                   return("unknown");
  }
}

// Render a double sentinel-aware: NaN prints as "-".
static void
exch_tickers_fmt_double(char *out, size_t cap, double v, const char *fmt)
{
  if(out == NULL || cap == 0)
    return;

  if(isnan(v))
  {
    snprintf(out, cap, "%s", "-");
    return;
  }

  snprintf(out, cap, fmt, v);
}

static void
exch_tickers_fmt_uint64(char *out, size_t cap, uint64_t v)
{
  if(out == NULL || cap == 0)
    return;

  if(v == UINT64_MAX)
  {
    snprintf(out, cap, "%s", "-");
    return;
  }

  snprintf(out, cap, "%" PRIu64, v);
}

// Callback fired by exchange_fetch_all_tickers_async on the curl worker.
// Copies the snapshot rows into the rendezvous-owned buffer so the
// caller's adapter is free to release its own buffers before we render.
// Drops one ref via exch_tickers_wait_release before returning.
static void
exch_tickers_done(bool success, const char *err,
    const exchange_ticker_snapshot_t *snaps, size_t n, void *user)
{
  exch_tickers_wait_t *w = user;

  if(w == NULL)
    return;

  pthread_mutex_lock(&w->mu);

  // If the waiter already gave up (refs has been decremented to 1
  // outside this cb), still capture the outcome so a debug-tail
  // observer can see what arrived, then signal and drop our ref.
  w->success = success;
  w->err[0]  = '\0';

  if(!success && err != NULL)
    snprintf(w->err, sizeof(w->err), "%s", err);

  if(success && n > 0 && snaps != NULL)
  {
    w->rows = mem_alloc(EXCHANGE_CTX, "tickers.rows",
        n * sizeof(*w->rows));

    if(w->rows == NULL)
    {
      w->success = false;
      snprintf(w->err, sizeof(w->err), "%s", "out of memory");
      w->n = 0;
    }
    else
    {
      memcpy(w->rows, snaps, n * sizeof(*w->rows));
      w->n = n;
    }
  }

  w->done = true;
  pthread_cond_signal(&w->cv);
  pthread_mutex_unlock(&w->mu);

  (void)exch_tickers_wait_release(w);
}

// Render to cmd_reply (sync — caller still holds the dispatch task).
static void
exch_tickers_render(const cmd_ctx_t *ctx, const char *exch_name,
    const exchange_ticker_snapshot_t *rows, size_t n)
{
  char    line[EXCH_TICKERS_LINE_SZ];
  size_t  i;
  size_t  shown;

  snprintf(line, sizeof(line),
      "exchange=%s  ticker_count=%zu", exch_name, n);
  cmd_reply(ctx, line);

  if(n == 0)
  {
    cmd_reply(ctx, "  (no rows)");
    return;
  }

  cmd_reply(ctx,
      "product_id        price           pct_24h    vol_24h_b"
      "         vol_24h_q          hi_24h      lo_24h      "
      "vwap_24h    trades_24h  status");

  shown = n < EXCH_TICKERS_REPLY_CAP ? n : EXCH_TICKERS_REPLY_CAP;

  for(i = 0; i < shown; i++)
  {
    const exchange_ticker_snapshot_t *r = &rows[i];
    char price [24];
    char pct   [16];
    char volb  [24];
    char volq  [24];
    char hi    [16];
    char lo    [16];
    char vwap  [16];
    char trades[24];

    exch_tickers_fmt_double(price, sizeof(price), r->price,         "%.4f");
    exch_tickers_fmt_double(pct,   sizeof(pct),   r->pct_24h,       "%+.2f");
    exch_tickers_fmt_double(volb,  sizeof(volb),  r->vol_24h_base,  "%.4f");
    exch_tickers_fmt_double(volq,  sizeof(volq),  r->vol_24h_quote, "%.2f");
    exch_tickers_fmt_double(hi,    sizeof(hi),    r->hi_24h,        "%.2f");
    exch_tickers_fmt_double(lo,    sizeof(lo),    r->lo_24h,        "%.2f");
    exch_tickers_fmt_double(vwap,  sizeof(vwap),  r->vwap_24h,      "%.2f");
    exch_tickers_fmt_uint64(trades, sizeof(trades), r->num_trades_24h);

    snprintf(line, sizeof(line),
        "%-16s  %-14s  %-9s  %-18s %-18s %-11s %-11s %-11s %-10s  %s",
        r->product_id, price, pct, volb, volq, hi, lo, vwap, trades,
        exch_tickers_status_name(r->status));
    cmd_reply(ctx, line);
  }

  if(n > shown)
  {
    snprintf(line, sizeof(line), "... %zu more", n - shown);
    cmd_reply(ctx, line);
  }
}

// Strip leading whitespace and copy the first token (up to whitespace
// or end) into `out`. Returns true when a non-empty token was extracted.
static bool
exch_first_token(const char *in, char *out, size_t cap)
{
  size_t i = 0;
  size_t j = 0;

  if(out == NULL || cap == 0)
    return(false);

  out[0] = '\0';

  if(in == NULL)
    return(false);

  while(in[i] == ' ' || in[i] == '\t')
    i++;

  while(in[i] != '\0' && in[i] != ' ' && in[i] != '\t' && j + 1 < cap)
    out[j++] = in[i++];

  out[j] = '\0';
  return(j > 0);
}

static void
exch_cmd_tickers(const cmd_ctx_t *ctx)
{
  char                 exch_tok[EXCHANGE_NAME_SZ];
  exch_tickers_wait_t *w;
  struct timespec      deadline;
  char                 line[EXCH_TICKERS_LINE_SZ];
  bool                 timed_out = false;
  int                  rc        = 0;

  if(!exch_first_token(ctx != NULL ? ctx->args : NULL,
        exch_tok, sizeof(exch_tok)))
  {
    cmd_reply(ctx, "usage: /exchange tickers <exchange>");
    return;
  }

  w = mem_alloc(EXCHANGE_CTX, "tickers.wait", sizeof(*w));

  if(w == NULL)
  {
    cmd_reply(ctx, "out of memory");
    return;
  }

  memset(w, 0, sizeof(*w));
  w->refs = 2;   // waiter + cb
  pthread_mutex_init(&w->mu, NULL);
  pthread_cond_init (&w->cv, NULL);

  // exchange_fetch_all_tickers_async fires exch_tickers_done on every
  // outcome (pre-flight FAIL, transport error, success). On synchronous
  // FAIL the cb has already run (and dropped its ref) before we return
  // here; `w->done` is set and the cond_timedwait below sees it
  // immediately. On SUCCESS the cb fires later from the curl worker.
  (void)exchange_fetch_all_tickers_async(exch_tok,
      exch_tickers_done, w);

  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec  += EXCH_TICKERS_WAIT_MS / 1000;
  deadline.tv_nsec += (long)(EXCH_TICKERS_WAIT_MS % 1000) * 1000000L;
  if(deadline.tv_nsec >= 1000000000L)
  {
    deadline.tv_sec  += 1;
    deadline.tv_nsec -= 1000000000L;
  }

  pthread_mutex_lock(&w->mu);
  while(!w->done && rc == 0)
    rc = pthread_cond_timedwait(&w->cv, &w->mu, &deadline);

  if(!w->done && rc == ETIMEDOUT)
    timed_out = true;
  pthread_mutex_unlock(&w->mu);

  if(timed_out)
  {
    snprintf(line, sizeof(line),
        "exchange tickers %s: timed out after %d ms",
        exch_tok, EXCH_TICKERS_WAIT_MS);
    cmd_reply(ctx, line);
  }
  else if(!w->success)
  {
    snprintf(line, sizeof(line),
        "exchange tickers %s: error: %s",
        exch_tok, w->err[0] != '\0' ? w->err : "unknown");
    cmd_reply(ctx, line);
  }
  else
  {
    exch_tickers_render(ctx, exch_tok, w->rows, w->n);
  }

  // Drop the waiter's ref. On timeout the cb still holds one and will
  // free the rendezvous when it eventually fires (or leaks safely if
  // the curl side never completes — same fate as any other dangling
  // curl request).
  (void)exch_tickers_wait_release(w);
}

// Parent verb — observability only; the leaf carries the work.
static void
exch_cmd_root(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /exchange tickers <exchange>");
}

bool
exchange_register_verbs(void)
{
  if(cmd_register("exchange", "exchange",
        "exchange <subcommand>",
        "Exchange abstraction operator verbs.",
        "Root for /exchange ... operator verbs. Today: `tickers`.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        exch_cmd_root, NULL, NULL, NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("exchange", "tickers",
        "exchange tickers <exchange>",
        "Bulk-ticker snapshot from one exchange.",
        "Calls the named exchange's bulk all-pairs ticker endpoint"
        " and prints the snapshot table (first 50 rows, with a"
        " '... N more' trailer when truncated). Public market data;"
        " no auth gate. NaN / UINT64_MAX sentinels print as '-'.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        exch_cmd_tickers, NULL, "exchange", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
