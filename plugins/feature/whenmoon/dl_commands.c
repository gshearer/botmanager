// botmanager — MIT
// Whenmoon candle-downloader admin verbs (state-changing under
// /whenmoon download; observability under /show whenmoon download).

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "market.h"
#include "dl_commands.h"
#include "dl_jobtable.h"
#include "dl_schema.h"
#include "dl_candles.h"
#include "dl_coverage.h"

#include "exchange_api.h"

#include "alloc.h"
#include "cmd.h"
#include "colors.h"
#include "common.h"
#include "userns.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Soft cap on how many row-level gap windows we resolve into jobs
// per invocation. Tunes the operator-visible behaviour when a market
// has many gaps: the helper returns at most this many, and the caller
// reports the truncation. Re-running the command after the first
// batch completes picks up the rest.
#define WM_DL_GAPS_FANOUT_CAP   1024

// ------------------------------------------------------------------ //
// Small helpers                                                       //
// ------------------------------------------------------------------ //

bool
wm_dl_next_token(const char **p, char *out, size_t cap)
{
  size_t n = 0;

  if(p == NULL || *p == NULL || out == NULL || cap == 0)
    return(false);

  while(**p == ' ' || **p == '\t')
    (*p)++;

  while(**p != '\0' && **p != ' ' && **p != '\t' && n + 1 < cap)
    out[n++] = *(*p)++;

  out[n] = '\0';

  while(**p == ' ' || **p == '\t')
    (*p)++;

  return(n > 0);
}

// Parse "<exch>-<base>-<quote>" lowercase + derive wire-form symbol
// (uppercase BASE-QUOTE for coinbase). EX-1 will move the wire-form
// derivation into the per-exchange plugin.
static bool
wm_dl_parse_market_id(const char *in,
    char *exch,   size_t exch_cap,
    char *base,   size_t base_cap,
    char *quote,  size_t quote_cap,
    char *symbol, size_t sym_cap)
{
  size_t i;
  int    n;

  if(wm_market_parse_id(in, exch, exch_cap,
         base, base_cap, quote, quote_cap) != SUCCESS)
    return(FAIL);

  // Coinbase wire form is uppercase BASE-QUOTE. Build it here for now;
  // EX-1 will route this through the exchange plugin.
  if(symbol == NULL || sym_cap == 0)
    return(FAIL);

  n = snprintf(symbol, sym_cap, "%s-%s", base, quote);

  if(n < 0 || (size_t)n >= sym_cap)
    return(FAIL);

  for(i = 0; symbol[i] != '\0'; i++)
    symbol[i] = (char)toupper((unsigned char)symbol[i]);

  return(SUCCESS);
}

// "MM/dd/yyyy" -> "YYYY-MM-DD 00:00:00+00".
static bool
wm_dl_parse_date(const char *in, char *out, size_t cap)
{
  unsigned mm;
  unsigned dd;
  unsigned yyyy;
  int      consumed = 0;
  int      n;

  if(in == NULL || out == NULL || cap == 0)
    return(FAIL);

  if(sscanf(in, "%u/%u/%u%n", &mm, &dd, &yyyy, &consumed) != 3)
    return(FAIL);

  if(in[consumed] != '\0')
    return(FAIL);

  if(mm < 1 || mm > 12 || dd < 1 || dd > 31 ||
     yyyy < 1970 || yyyy > 9999)
    return(FAIL);

  n = snprintf(out, cap, "%04u-%02u-%02u 00:00:00+00", yyyy, mm, dd);

  if(n < 0 || (size_t)n >= cap)
    return(FAIL);

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Handlers                                                            //
// ------------------------------------------------------------------ //

// Enqueue one DL_JOB_CANDLES per gap window. The job_enqueue path no
// longer pre-filters against the coverage store, so we honour the
// gap list exactly as wm_gap_find_row_gaps returned it.
static void
wm_dl_cmd_download_enqueue_gaps(const cmd_ctx_t *ctx,
    whenmoon_state_t *st,
    const char *exch, const char *symbol, int32_t market_id,
    const wm_coverage_t *gaps, uint32_t n_gaps)
{
  char     reply[256];
  char     err[128];
  uint32_t i;
  uint32_t enqueued = 0;
  uint32_t failed   = 0;
  int64_t  job_id;

  for(i = 0; i < n_gaps; i++)
  {
    err[0] = '\0';
    job_id = 0;

    if(wm_dl_job_enqueue(st, DL_JOB_CANDLES, market_id,
           EXCHANGE_PRIO_USER_DOWNLOAD,
           exch, symbol,
           gaps[i].first_ts, gaps[i].last_ts,
           ctx->username != NULL ? ctx->username : "",
           &job_id, err, sizeof(err)) != SUCCESS)
    {
      snprintf(reply, sizeof(reply),
          "gap %u/%u [%s,%s] enqueue failed: %s",
          i + 1, n_gaps, gaps[i].first_ts, gaps[i].last_ts,
          err[0] != '\0' ? err : "unknown");
      cmd_reply(ctx, reply);
      failed++;
      continue;
    }

    enqueued++;
  }

  snprintf(reply, sizeof(reply),
      "%s: %u/%u gap%s queued%s",
      symbol, enqueued, n_gaps, n_gaps == 1 ? "" : "s",
      failed > 0 ? " (some failed — see above)" : "");
  cmd_reply(ctx, reply);
}

// /whenmoon download <market> [MM/dd/yyyy [MM/dd/yyyy]]
//
// Idempotent: scans `wm_candles_<market_id>` for row-level gaps in
// the requested window (default = epoch to now, since dl_candles'
// empty-page termination keeps a too-old start from causing extra
// work) and fires one DL_JOB_CANDLES per gap. Run as many times as
// you like — each invocation just refetches whatever's still
// missing. Anything that persists across runs is genuinely absent
// from the exchange.
static void
wm_dl_cmd_download_market(const cmd_ctx_t *ctx, whenmoon_state_t *st,
    const char *market_tok, const char *rest)
{
  const char    *p;
  char           old_tok[32]    = {0};
  char           new_tok[32]    = {0};
  char           exch[32]       = {0};
  char           base[16]       = {0};
  char           quote[16]      = {0};
  char           symbol[32]     = {0};
  char           oldest_ts[40]  = {0};
  char           newest_ts[40]  = {0};
  char           range_start[40];
  char           range_end[40];
  char           reply[256];
  int32_t        market_id;
  wm_coverage_t *gaps           = NULL;
  uint32_t       n_gaps;
  time_t         now;
  struct tm      tm;

  if(wm_dl_parse_market_id(market_tok, exch, sizeof(exch),
         base, sizeof(base), quote, sizeof(quote),
         symbol, sizeof(symbol)) != SUCCESS)
  {
    cmd_reply(ctx, "bad market id (expected <exch>-<base>-<quote>)");
    return;
  }

  p = rest != NULL ? rest : "";
  (void)wm_dl_next_token(&p, old_tok, sizeof(old_tok));
  (void)wm_dl_next_token(&p, new_tok, sizeof(new_tok));

  if(old_tok[0] != '\0' &&
     wm_dl_parse_date(old_tok, oldest_ts, sizeof(oldest_ts)) != SUCCESS)
  {
    cmd_reply(ctx, "bad oldest date (expected MM/dd/yyyy)");
    return;
  }

  if(new_tok[0] != '\0' &&
     wm_dl_parse_date(new_tok, newest_ts, sizeof(newest_ts)) != SUCCESS)
  {
    cmd_reply(ctx, "bad newest date (expected MM/dd/yyyy)");
    return;
  }

  if(oldest_ts[0] != '\0' && newest_ts[0] != '\0' &&
     strcmp(oldest_ts, newest_ts) > 0)
  {
    cmd_reply(ctx, "oldest date must not be after newest date");
    return;
  }

  market_id = wm_market_lookup_or_create(exch, base, quote, symbol);

  if(market_id < 0)
  {
    cmd_reply(ctx, "market lookup/create failed");
    return;
  }

  if(oldest_ts[0] != '\0')
    snprintf(range_start, sizeof(range_start), "%s", oldest_ts);
  else
    snprintf(range_start, sizeof(range_start),
        "1970-01-01 00:00:00+00");

  if(newest_ts[0] != '\0')
    snprintf(range_end, sizeof(range_end), "%s", newest_ts);
  else
  {
    now = time(NULL);

    if(gmtime_r(&now, &tm) == NULL)
    {
      cmd_reply(ctx, "gmtime_r failed");
      return;
    }

    snprintf(range_end, sizeof(range_end),
        "%04d-%02d-%02d %02d:%02d:%02d+00",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
  }

  gaps = mem_alloc(WM_DL_CTX, "gaps",
      (size_t)WM_DL_GAPS_FANOUT_CAP * sizeof(*gaps));

  if(gaps == NULL)
  {
    cmd_reply(ctx, "out of memory");
    return;
  }

  n_gaps = wm_gap_find_row_gaps(market_id,
      range_start, range_end, gaps, WM_DL_GAPS_FANOUT_CAP);

  if(n_gaps == 0)
  {
    snprintf(reply, sizeof(reply),
        "%s: no gaps in [%s, %s]", symbol, range_start, range_end);
    cmd_reply(ctx, reply);
    mem_free(gaps);
    return;
  }

  if(n_gaps == WM_DL_GAPS_FANOUT_CAP)
    cmd_reply(ctx,
        "gap list truncated at fanout cap;"
        " re-run after these jobs complete to pick up the rest");

  snprintf(reply, sizeof(reply),
      "%s: %u gap%s in [%s, %s]; enqueueing fetch jobs",
      symbol, n_gaps, n_gaps == 1 ? "" : "s",
      range_start, range_end);
  cmd_reply(ctx, reply);

  wm_dl_cmd_download_enqueue_gaps(ctx, st, exch, symbol, market_id,
      gaps, n_gaps);

  mem_free(gaps);
}

static void
wm_dl_cmd_download_cancel(const cmd_ctx_t *ctx, whenmoon_state_t *st,
    const char *rest)
{
  const char *p;
  char        tok[32] = {0};
  char        reply[96];
  char        err[128];
  int64_t     job_id;

  p = rest != NULL ? rest : "";

  if(!wm_dl_next_token(&p, tok, sizeof(tok)))
  {
    cmd_reply(ctx, "usage: /whenmoon download cancel <job_id>");
    return;
  }

  job_id = (int64_t)strtoll(tok, NULL, 10);

  if(job_id <= 0)
  {
    cmd_reply(ctx, "bad job id");
    return;
  }

  if(wm_dl_job_cancel(st, job_id, err, sizeof(err)) != SUCCESS)
  {
    char buf[192];

    snprintf(buf, sizeof(buf), "cancel failed: %s",
        err[0] != '\0' ? err : "unknown");
    cmd_reply(ctx, buf);
    return;
  }

  snprintf(reply, sizeof(reply), "job %" PRId64 " cancelled", job_id);
  cmd_reply(ctx, reply);
}

static void
wm_dl_cmd_download(const cmd_ctx_t *ctx)
{
  whenmoon_state_t *st;
  const char       *p;
  char              first_tok[64] = {0};

  st = whenmoon_get_state();

  if(st == NULL || !st->dl_ready || st->downloader == NULL)
  {
    cmd_reply(ctx, "whenmoon: downloader not ready");
    return;
  }

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, first_tok, sizeof(first_tok)))
  {
    cmd_reply(ctx,
        "usage: /whenmoon download <exch>-<base>-<quote>"
        " [MM/dd/yyyy [MM/dd/yyyy]]"
        " | /whenmoon download cancel <job_id>");
    return;
  }

  if(strcmp(first_tok, "cancel") == 0)
  {
    wm_dl_cmd_download_cancel(ctx, st, p);
    return;
  }

  wm_dl_cmd_download_market(ctx, st, first_tok, p);
}

// ------------------------------------------------------------------ //
// /show whenmoon download status                                      //
// ------------------------------------------------------------------ //

typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         count;
} wm_dl_status_state_t;

static const char *
wm_dl_state_label(dl_job_state_t s)
{
  switch(s)
  {
    case DL_JOB_QUEUED:  return("queued");
    case DL_JOB_RUNNING: return("running");
    case DL_JOB_PAUSED:  return("paused");
    case DL_JOB_DONE:    return("done");
    case DL_JOB_FAILED:  return("failed");
    default:             return("unknown");
  }
}

static void
wm_dl_status_row(const dl_job_t *j, void *user)
{
  wm_dl_status_state_t *s = user;
  char                  line[384];

  snprintf(line, sizeof(line),
      "  job %-6" PRId64 " candles %-12s state=%-8s pages=%-4" PRId32
      " rows=%-10" PRId64 " err=%s",
      j->id,
      j->exchange_symbol[0] != '\0' ? j->exchange_symbol : "?",
      wm_dl_state_label(j->state),
      j->pages_fetched,
      j->rows_written,
      j->last_err[0] != '\0' ? j->last_err : "-");

  cmd_reply(s->ctx, line);
  s->count++;
}

static void
wm_dl_cmd_show_download_status(const cmd_ctx_t *ctx)
{
  whenmoon_state_t     *st;
  wm_dl_status_state_t  sstate = { .ctx = ctx, .count = 0 };

  st = whenmoon_get_state();

  if(st == NULL || st->downloader == NULL)
  {
    cmd_reply(ctx, "whenmoon: downloader not ready");
    return;
  }

  cmd_reply(ctx, CLR_BOLD "download jobs" CLR_RESET);

  wm_dl_job_list_iterate(st, wm_dl_status_row, &sstate);

  if(sstate.count == 0)
    cmd_reply(ctx, "  (no jobs)");
}

// ------------------------------------------------------------------ //
// /show whenmoon download candles <id> <gran> <start> <end>           //
// ------------------------------------------------------------------ //

static void
wm_dl_cmd_show_download_candles(const cmd_ctx_t *ctx)
{
  whenmoon_state_t  *st;
  const char        *p;
  char               pair[64]   = {0};
  char               start_tok[32] = {0};
  char               end_tok[32]   = {0};
  char               exch[32]   = {0};
  char               base[16]   = {0};
  char               quote[16]  = {0};
  char               symbol[32] = {0};
  char               start_ts[40] = {0};
  char               end_ts[40]   = {0};
  char               header[160];
  char               line[256];
  int32_t            market_id;
  uint32_t           n;
  uint32_t           i;
  coinbase_candle_t *rows;

  st = whenmoon_get_state();

  if(st == NULL || !st->dl_ready)
  {
    cmd_reply(ctx, "whenmoon: downloader not ready");
    return;
  }

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, pair,      sizeof(pair))      ||
     !wm_dl_next_token(&p, start_tok, sizeof(start_tok)) ||
     !wm_dl_next_token(&p, end_tok,   sizeof(end_tok)))
  {
    cmd_reply(ctx,
        "usage: /show whenmoon download candles"
        " <exch>-<base>-<quote>"
        " <MM/dd/yyyy> <MM/dd/yyyy>");
    return;
  }

  if(wm_dl_parse_market_id(pair, exch, sizeof(exch),
         base, sizeof(base), quote, sizeof(quote),
         symbol, sizeof(symbol)) != SUCCESS)
  {
    cmd_reply(ctx, "bad market id (expected <exch>-<base>-<quote>)");
    return;
  }

  if(wm_dl_parse_date(start_tok, start_ts, sizeof(start_ts)) != SUCCESS ||
     wm_dl_parse_date(end_tok,   end_ts,   sizeof(end_ts))   != SUCCESS)
  {
    cmd_reply(ctx, "bad date (expected MM/dd/yyyy)");
    return;
  }

  if(strcmp(start_ts, end_ts) > 0)
  {
    cmd_reply(ctx, "start date must not be after end date");
    return;
  }

  market_id = wm_market_lookup_or_create(exch, base, quote, symbol);

  if(market_id < 0)
  {
    cmd_reply(ctx, "market lookup/create failed");
    return;
  }

  rows = mem_alloc("whenmoon.dl", "agg_candles",
      (size_t)WM_DL_CANDLES_OUT_CAP * sizeof(*rows));

  if(rows == NULL)
  {
    cmd_reply(ctx, "out of memory");
    return;
  }

  // Persistence is 1m-only; surface the raw 1m bars through the
  // upsample helper at gran=60 (a passthrough) so callers see the
  // same row shape they always did.
  n = wm_dl_candles_query_aggregated(market_id, 60,
      start_ts, end_ts, rows, WM_DL_CANDLES_OUT_CAP);

  if(n == 0)
  {
    cmd_reply(ctx, "no candle data for this market in the window");
    mem_free(rows);
    return;
  }

  snprintf(header, sizeof(header),
      "candles %s %s -> %s (%u rows, CSV)",
      symbol, start_ts, end_ts, n);
  cmd_reply(ctx, header);
  cmd_reply(ctx, "ts_epoch,low,high,open,close,volume");

  for(i = 0; i < n; i++)
  {
    snprintf(line, sizeof(line),
        "%" PRId64 ",%.10g,%.10g,%.10g,%.10g,%.10g",
        rows[i].time, rows[i].low, rows[i].high,
        rows[i].open, rows[i].close, rows[i].volume);
    cmd_reply(ctx, line);
  }

  mem_free(rows);
}

// ------------------------------------------------------------------ //
// Parent stubs                                                        //
// ------------------------------------------------------------------ //

static void
wm_dl_parent_show_download(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx,
      "usage: /show whenmoon download <status|candles> ...");
}

// ------------------------------------------------------------------ //
// Registration                                                        //
// ------------------------------------------------------------------ //

bool
wm_dl_register_verbs(void)
{
  // /whenmoon download <market> [start] [end]    — gap-fill enqueue
  // /whenmoon download cancel <job_id>           — cancel job
  if(cmd_register("whenmoon", "download",
        "whenmoon download <exch>-<base>-<quote>"
        " [MM/dd/yyyy [MM/dd/yyyy]] | cancel <job_id>",
        "Idempotent candle backfill: scans wm_candles_<id> for"
        " row-level gaps in the requested window (default = epoch to"
        " now) and fires one fetch job per gap. Re-run as needed;"
        " each invocation only refetches what's still missing.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_dl_cmd_download, NULL, "whenmoon", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  // /show whenmoon download parent.
  if(cmd_register("whenmoon", "download",
        "show whenmoon download <verb>",
        "Download engine observability.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_dl_parent_show_download, NULL, "show/whenmoon", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "status",
        "show whenmoon download status",
        "Job list with state, pages, and rows.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_dl_cmd_show_download_status, NULL,
        "show/whenmoon/download", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "candles",
        "show whenmoon download candles"
        " <exch>-<base>-<quote>"
        " <MM/dd/yyyy> <MM/dd/yyyy>",
        "Print the stored 1m candles over the window, CSV-style.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_dl_cmd_show_download_candles, NULL,
        "show/whenmoon/download", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
