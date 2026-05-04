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
#include "cd_probe.h"

#include "exchange_api.h"

#include "cmd.h"
#include "colors.h"
#include "common.h"
#include "kv.h"
#include "userns.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

// WM-CD-1 Phase 2: `whenmoon download candles <market> max` sugar.
// Reads each per-granularity max_lookback_days cap from KV (populated
// by Phase 1 probe-depth) and fans out one DL_JOB_CANDLES per
// granularity whose cap is non-zero, with oldest_ts = now - cap_days
// and newest_ts = NULL (= now). Errors politely if no cap has been
// probed yet.
static const int32_t WM_CD_MAX_GRANS[] = {
  60, 300, 900, 3600, 21600, 86400
};
#define WM_CD_MAX_GRAN_N \
  ((int32_t)(sizeof(WM_CD_MAX_GRANS) / sizeof(WM_CD_MAX_GRANS[0])))

static void
wm_dl_cmd_download_candles_max(const cmd_ctx_t *ctx,
    whenmoon_state_t *st,
    const char *exch, const char *symbol, int32_t market_id)
{
  char    reply[256];
  char    err[128];
  char    key[80];
  char    oldest_ts[40];
  time_t  now;
  int32_t i;
  int32_t fanout = 0;
  int32_t failed = 0;

  now = time(NULL);

  for(i = 0; i < WM_CD_MAX_GRAN_N; i++)
  {
    int32_t   gran = WM_CD_MAX_GRANS[i];
    uint64_t  cap_days;
    time_t    oldest;
    struct tm tm;
    int64_t   job_id = 0;

    snprintf(key, sizeof(key),
        "plugin.whenmoon.candles.%" PRId32 ".max_lookback_days", gran);

    cap_days = kv_get_uint(key);

    if(cap_days == 0)
      continue;

    oldest = now - (time_t)cap_days * 86400;

    if(gmtime_r(&oldest, &tm) == NULL)
    {
      failed++;
      continue;
    }

    snprintf(oldest_ts, sizeof(oldest_ts),
        "%04d-%02d-%02d %02d:%02d:%02d+00",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);

    err[0] = '\0';

    if(wm_dl_job_enqueue(st, DL_JOB_CANDLES, market_id, gran,
           EXCHANGE_PRIO_USER_DOWNLOAD,
           exch, symbol, oldest_ts, NULL,
           ctx->username != NULL ? ctx->username : "",
           &job_id, err, sizeof(err)) != SUCCESS)
    {
      snprintf(reply, sizeof(reply),
          "gran=%" PRId32 " enqueue failed: %s", gran,
          err[0] != '\0' ? err : "unknown");
      cmd_reply(ctx, reply);
      failed++;
      continue;
    }

    snprintf(reply, sizeof(reply),
        "candles job %" PRId64 " queued (%s gran=%" PRId32
        " oldest=%s newest=now cap=%" PRIu64 "d)",
        job_id, symbol, gran, oldest_ts, cap_days);
    cmd_reply(ctx, reply);
    fanout++;
  }

  if(fanout == 0 && failed == 0)
    cmd_reply(ctx,
        "no probed depth caps yet;"
        " run /whenmoon candles probe-depth <market> first");
  else
  {
    snprintf(reply, sizeof(reply),
        "%" PRId32 " job(s) queued, %" PRId32 " failed", fanout, failed);
    cmd_reply(ctx, reply);
  }
}

static void
wm_dl_cmd_download_candles(const cmd_ctx_t *ctx)
{
  whenmoon_state_t *st;
  const char       *p;
  char              pair[64]      = {0};
  char              old_tok[32]   = {0};
  char              new_tok[32]   = {0};
  char              gran_tok[16]  = {0};
  char              exch[32]      = {0};
  char              base[16]      = {0};
  char              quote[16]     = {0};
  char              symbol[32]    = {0};
  char              oldest_ts[40] = {0};
  char              newest_ts[40] = {0};
  char              reply[256];
  char              err[128];
  int32_t           market_id;
  int32_t           gran   = WM_DL_CANDLE_GRAN_S5;
  int64_t           job_id = 0;

  st = whenmoon_get_state();

  if(st == NULL || !st->dl_ready || st->downloader == NULL)
  {
    cmd_reply(ctx, "whenmoon: downloader not ready");
    return;
  }

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, pair, sizeof(pair)))
  {
    cmd_reply(ctx,
        "usage: /whenmoon download candles <exch>-<base>-<quote>"
        " [MM/dd/yyyy [MM/dd/yyyy [gran_secs]] | max]");
    return;
  }

  (void)wm_dl_next_token(&p, old_tok, sizeof(old_tok));
  (void)wm_dl_next_token(&p, new_tok, sizeof(new_tok));
  (void)wm_dl_next_token(&p, gran_tok, sizeof(gran_tok));

  if(wm_dl_parse_market_id(pair, exch, sizeof(exch),
         base, sizeof(base), quote, sizeof(quote),
         symbol, sizeof(symbol)) != SUCCESS)
  {
    cmd_reply(ctx, "bad market id (expected <exch>-<base>-<quote>)");
    return;
  }

  // WM-CD-1 Phase 2: `max` sugar. Resolve market then fan out across
  // every granularity that has a probed cap.
  if(strcmp(old_tok, "max") == 0)
  {
    market_id = wm_market_lookup_or_create(exch, base, quote, symbol);

    if(market_id < 0)
    {
      cmd_reply(ctx, "market lookup/create failed");
      return;
    }

    wm_dl_cmd_download_candles_max(ctx, st, exch, symbol, market_id);
    return;
  }

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

  if(gran_tok[0] != '\0')
  {
    int64_t gran_parsed = (int64_t)strtoll(gran_tok, NULL, 10);

    if(gran_parsed <= 0 || gran_parsed > INT32_MAX ||
       !wm_dl_granularity_valid((int32_t)gran_parsed))
    {
      cmd_reply(ctx,
          "invalid gran_secs;"
          " must be 60, 300, 900, 3600, 21600, or 86400");
      return;
    }

    gran = (int32_t)gran_parsed;
  }

  market_id = wm_market_lookup_or_create(exch, base, quote, symbol);

  if(market_id < 0)
  {
    cmd_reply(ctx, "market lookup/create failed");
    return;
  }

  if(wm_dl_job_enqueue(st, DL_JOB_CANDLES, market_id,
         gran,
         EXCHANGE_PRIO_USER_DOWNLOAD,
         exch, symbol,
         oldest_ts[0] != '\0' ? oldest_ts : NULL,
         newest_ts[0] != '\0' ? newest_ts : NULL,
         ctx->username != NULL ? ctx->username : "",
         &job_id, err, sizeof(err)) != SUCCESS)
  {
    char buf[192];

    snprintf(buf, sizeof(buf), "enqueue failed: %s",
        err[0] != '\0' ? err : "unknown");
    cmd_reply(ctx, buf);
    return;
  }

  snprintf(reply, sizeof(reply),
      "candles job %" PRId64 " queued (%s gran=%" PRId32
      " oldest=%s newest=%s)",
      job_id, symbol, gran,
      oldest_ts[0] != '\0' ? oldest_ts : "epoch",
      newest_ts[0] != '\0' ? newest_ts : "now");
  cmd_reply(ctx, reply);
}

// ------------------------------------------------------------------ //
// /whenmoon candles probe-depth <market>                              //
// WM-CD-1 Phase 1: kick off the per-granularity bisection probe and   //
// publish discovered max_lookback_days to KV.                          //
// ------------------------------------------------------------------ //

static void
wm_cd_cmd_probe_depth(const cmd_ctx_t *ctx)
{
  whenmoon_state_t *st;
  const char       *p;
  char              pair[64]   = {0};
  char              exch[32]   = {0};
  char              base[16]   = {0};
  char              quote[16]  = {0};
  char              symbol[32] = {0};
  char              reply[192];

  st = whenmoon_get_state();

  if(st == NULL || !st->dl_ready)
  {
    cmd_reply(ctx, "whenmoon: downloader not ready");
    return;
  }

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, pair, sizeof(pair)))
  {
    cmd_reply(ctx,
        "usage: /whenmoon candles probe-depth"
        " <exch>-<base>-<quote>");
    return;
  }

  if(wm_dl_parse_market_id(pair, exch, sizeof(exch),
         base, sizeof(base), quote, sizeof(quote),
         symbol, sizeof(symbol)) != SUCCESS)
  {
    cmd_reply(ctx, "bad market id (expected <exch>-<base>-<quote>)");
    return;
  }

  if(wm_cd_probe_run(symbol) != SUCCESS)
  {
    cmd_reply(ctx, "probe submit failed");
    return;
  }

  snprintf(reply, sizeof(reply),
      "probing %s depth on 6 granularities;"
      " results land in plugin.whenmoon.candles.<gran>.max_lookback_days"
      " (and the daemon log) within ~60s",
      symbol);
  cmd_reply(ctx, reply);
}

static void
wm_dl_cmd_download_cancel(const cmd_ctx_t *ctx)
{
  whenmoon_state_t *st;
  const char       *p;
  char              tok[32] = {0};
  char              reply[96];
  char              err[128];
  int64_t           job_id;

  st = whenmoon_get_state();

  if(st == NULL || st->downloader == NULL)
  {
    cmd_reply(ctx, "whenmoon: downloader not ready");
    return;
  }

  p = ctx->args != NULL ? ctx->args : "";

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
  char               gran_tok[16] = {0};
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
  int32_t            gran;
  int64_t            gran_parsed;
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
     !wm_dl_next_token(&p, gran_tok,  sizeof(gran_tok))  ||
     !wm_dl_next_token(&p, start_tok, sizeof(start_tok)) ||
     !wm_dl_next_token(&p, end_tok,   sizeof(end_tok)))
  {
    cmd_reply(ctx,
        "usage: /show whenmoon download candles"
        " <exch>-<base>-<quote> <gran_secs>"
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

  gran_parsed = (int64_t)strtoll(gran_tok, NULL, 10);

  if(gran_parsed <= 0 || gran_parsed > INT32_MAX ||
     !wm_dl_granularity_valid((int32_t)gran_parsed))
  {
    cmd_reply(ctx,
        "invalid gran_secs;"
        " must be 60, 300, 900, 3600, 21600, or 86400");
    return;
  }

  gran = (int32_t)gran_parsed;

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

  n = wm_dl_candles_query_aggregated(market_id, gran,
      start_ts, end_ts, rows, WM_DL_CANDLES_OUT_CAP);

  if(n == 0)
  {
    cmd_reply(ctx, "no candle data for this market in the window");
    mem_free(rows);
    return;
  }

  snprintf(header, sizeof(header),
      "candles %s gran=%" PRId32 " %s -> %s (%u rows, CSV)",
      symbol, gran, start_ts, end_ts, n);
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
wm_dl_parent_download(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx,
      "usage: /whenmoon download <candles|cancel> ...");
}

static void
wm_cd_parent_candles(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx,
      "usage: /whenmoon candles <probe-depth> ...");
}

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
  // /whenmoon download parent.
  if(cmd_register("whenmoon", "download",
        "whenmoon download <verb> ...",
        "Candle history download controls.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_dl_parent_download, NULL, "whenmoon", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "candles",
        "whenmoon download candles <exch>-<base>-<quote>"
        " [MM/dd/yyyy [MM/dd/yyyy [gran_secs]] | max]",
        "Enqueue a candle backfill job. Optional gran_secs is one of"
        " 60 (1m, default), 300 (5m), 900 (15m), 3600 (1h),"
        " 21600 (6h), 86400 (1d). `max` fans out one job per"
        " granularity using the probed depth caps from"
        " /whenmoon candles probe-depth.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_dl_cmd_download_candles, NULL, "whenmoon/download", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "cancel",
        "whenmoon download cancel <job_id>",
        "Cancel a running or queued download job.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_dl_cmd_download_cancel, NULL, "whenmoon/download", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  // /whenmoon candles parent + probe-depth leaf (WM-CD-1 Phase 1).
  if(cmd_register("whenmoon", "candles",
        "whenmoon candles <verb> ...",
        "Candle-pipeline admin verbs (depth probe, future: bulk).",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_cd_parent_candles, NULL, "whenmoon", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "probe-depth",
        "whenmoon candles probe-depth <exch>-<base>-<quote>",
        "Probe Coinbase candle history depth per granularity."
        " Results land in plugin.whenmoon.candles.<gran>"
        ".max_lookback_days and the daemon log within ~60s.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_cd_cmd_probe_depth, NULL, "whenmoon/candles", NULL,
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
        " <exch>-<base>-<quote> <gran_secs>"
        " <MM/dd/yyyy> <MM/dd/yyyy>",
        "Print aggregated candles over the window, CSV-style,"
        " upsampled from the stored 1m table."
        " gran_secs must be 60, 300, 900, 3600, 21600, or 86400.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_dl_cmd_show_download_candles, NULL,
        "show/whenmoon/download", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
