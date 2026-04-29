// botmanager — MIT
// Whenmoon backtest admin verbs (WM-LT-5).
//
//   /whenmoon backtest run <market_id> <strategy_name>
//                          <MM/dd/yyyy> <MM/dd/yyyy>
//                          [--fee-bps N] [--slip-bps N]
//                          [--size-frac F] [--cash N]
//   /show whenmoon backtest [<run_id>|list]
//
// run executes a single iteration synchronously on the cmd worker
// thread. The build + replay of one BTC-USD year is on the order of
// a few seconds; cmd dispatch already runs off the main thread so
// blocking the worker for that span is acceptable for WM-LT-5.
// WM-LT-6 will move to a worker pool + async results.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "backtest.h"
#include "dl_commands.h"
#include "dl_schema.h"
#include "market.h"
#include "order.h"
#include "strategy.h"
#include "sweep.h"

#include "cmd.h"
#include "colors.h"
#include "common.h"
#include "userns.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Default list view depth. The /show whenmoon backtest list command
// renders the most-recent N rows; raise via WM_BT_LIST_DEFAULT below.
#define WM_BT_LIST_DEFAULT  20
#define WM_BT_LIST_MAX      256

// ----------------------------------------------------------------------- //
// Date helper                                                             //
// ----------------------------------------------------------------------- //

// "MM/dd/yyyy" -> "YYYY-MM-DD 00:00:00+00". Same shape as the
// downloader's wm_dl_parse_date but local because dl_commands.c
// keeps its copy static. Duplicating one tiny helper is preferable
// to leaking it onto the public surface.
static bool
wm_bt_parse_date(const char *in, char *out, size_t cap)
{
  unsigned mm, dd, yyyy;
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

// ----------------------------------------------------------------------- //
// JSON serialisers                                                        //
// ----------------------------------------------------------------------- //

// Render a wm_backtest_params_t to a static-buffer JSON object. Only
// the `have_*` keys appear; an empty result emits "{}".
static void
wm_bt_params_to_json(const wm_backtest_params_t *p,
    char *out, size_t cap)
{
  size_t off = 0;
  bool   first = true;
  int    n;

  if(out == NULL || cap == 0)
    return;

  out[0] = '\0';

  n = snprintf(out + off, cap - off, "{");
  if(n < 0 || (size_t)n >= cap - off) return;
  off += (size_t)n;

  if(p != NULL && p->have_fee_bps)
  {
    n = snprintf(out + off, cap - off, "%s\"fee_bps\":%.10g",
        first ? "" : ",", p->fee_bps);
    if(n < 0 || (size_t)n >= cap - off) return;
    off += (size_t)n; first = false;
  }

  if(p != NULL && p->have_slip_bps)
  {
    n = snprintf(out + off, cap - off, "%s\"slip_bps\":%.10g",
        first ? "" : ",", p->slip_bps);
    if(n < 0 || (size_t)n >= cap - off) return;
    off += (size_t)n; first = false;
  }

  if(p != NULL && p->have_size_frac)
  {
    n = snprintf(out + off, cap - off, "%s\"size_frac\":%.10g",
        first ? "" : ",", p->size_frac);
    if(n < 0 || (size_t)n >= cap - off) return;
    off += (size_t)n; first = false;
  }

  if(p != NULL && p->have_starting_cash)
  {
    n = snprintf(out + off, cap - off, "%s\"starting_cash\":%.10g",
        first ? "" : ",", p->starting_cash);
    if(n < 0 || (size_t)n >= cap - off) return;
    off += (size_t)n; first = false;
  }

  (void)first;

  if(off + 1 < cap)
  {
    out[off]   = '}';
    out[off+1] = '\0';
  }
  else if(cap > 0)
    out[cap-1] = '\0';
}

// Render the trade snapshot's PnL metrics + headline economics to
// JSON. Used as the wm_backtest_run.metrics JSONB payload.
static void
wm_bt_metrics_to_json(const wm_trade_snapshot_t *snap,
    char *out, size_t cap)
{
  int n;

  if(out == NULL || cap == 0 || snap == NULL)
  {
    if(out != NULL && cap > 0) out[0] = '\0';
    return;
  }

  n = snprintf(out, cap,
      "{"
      "\"trades\":%u,"
      "\"wins\":%u,"
      "\"losses\":%u,"
      "\"win_rate\":%.6f,"
      "\"realized_pnl\":%.6f,"
      "\"fees_paid\":%.6f,"
      "\"profit_factor\":%.6f,"
      "\"max_drawdown\":%.6f,"
      "\"avg_win\":%.6f,"
      "\"avg_loss\":%.6f,"
      "\"sharpe\":%.6f,"
      "\"sortino\":%.6f,"
      "\"final_equity\":%.6f,"
      "\"starting_cash\":%.6f,"
      "\"final_cash\":%.6f"
      "}",
      snap->metrics.n_trades, snap->metrics.n_wins, snap->metrics.n_losses,
      snap->metrics.win_rate, snap->metrics.realized_pnl,
      snap->metrics.fees_paid, snap->metrics.profit_factor,
      snap->metrics.max_drawdown, snap->metrics.avg_win,
      snap->metrics.avg_loss, snap->metrics.sharpe, snap->metrics.sortino,
      snap->equity, snap->starting_cash, snap->cash);

  if(n < 0 || (size_t)n >= cap)
    out[cap - 1] = '\0';
}

// ----------------------------------------------------------------------- //
// /whenmoon backtest run                                                  //
// ----------------------------------------------------------------------- //

// Parse a numeric --flag value. On success, `*out_v` is the parsed
// double and `*out_have` is set true. On failure, leaves both
// untouched and returns FAIL.
static bool
wm_bt_parse_double_flag(const char *tok, double *out_v, bool *out_have)
{
  char *end = NULL;
  double v;

  if(tok == NULL || *tok == '\0')
    return(FAIL);

  errno = 0;
  v     = strtod(tok, &end);

  if(end == tok || errno != 0)
    return(FAIL);

  if(out_v != NULL)    *out_v    = v;
  if(out_have != NULL) *out_have = true;
  return(SUCCESS);
}

// Parse "train=Td:test=Md:step=Sd" into a wm_bt_walk_spec_t.
// Each segment must use a 'd' suffix; days, not weeks/hours. Returns
// SUCCESS on a clean parse with all three keys present.
static bool
wm_bt_parse_walk_spec(const char *tok, wm_bt_walk_spec_t *out)
{
  char     buf[160];
  char    *save = NULL;
  char    *seg;
  uint32_t got_mask = 0;

  if(tok == NULL || out == NULL)
    return(FAIL);

  if(strlen(tok) >= sizeof(buf))
    return(FAIL);

  snprintf(buf, sizeof(buf), "%s", tok);
  memset(out, 0, sizeof(*out));

  for(seg = strtok_r(buf, ":", &save); seg != NULL;
      seg = strtok_r(NULL, ":", &save))
  {
    char     *eq = strchr(seg, '=');
    char     *end = NULL;
    char      key[16];
    long      v;
    size_t    klen;

    if(eq == NULL)
      return(FAIL);

    klen = (size_t)(eq - seg);

    if(klen == 0 || klen >= sizeof(key))
      return(FAIL);

    memcpy(key, seg, klen);
    key[klen] = '\0';

    errno = 0;
    v     = strtol(eq + 1, &end, 10);

    if(end == eq + 1 || errno != 0 || v <= 0)
      return(FAIL);

    // Tolerate a trailing 'd' (the documented suffix); reject others.
    if(*end == 'd' && *(end + 1) == '\0')
      ;  // ok
    else if(*end != '\0')
      return(FAIL);

    if(strcmp(key, "train") == 0)
    {
      out->train_days = (uint32_t)v;
      got_mask |= 1u;
    }
    else if(strcmp(key, "test") == 0)
    {
      out->test_days = (uint32_t)v;
      got_mask |= 2u;
    }
    else if(strcmp(key, "step") == 0)
    {
      out->step_days = (uint32_t)v;
      got_mask |= 4u;
    }
    else
      return(FAIL);
  }

  return((got_mask == 7u) ? SUCCESS : FAIL);
}

// Parse "YYYY-MM-DD HH:MM:SS+00" -> ms epoch. Returns SUCCESS on a
// clean parse. Mirrors the parser inside backtest.c; duplicated here
// because that one is static.
static bool
wm_bt_parse_ts_ms(const char *in, int64_t *out_ms)
{
  struct tm tm;
  unsigned  yyyy, mo, dd, hh, mm, ss;
  int       consumed = 0;
  time_t    t;

  if(in == NULL || out_ms == NULL)
    return(FAIL);

  if(sscanf(in, "%u-%u-%u %u:%u:%u%n",
        &yyyy, &mo, &dd, &hh, &mm, &ss, &consumed) != 6 || consumed < 19)
    return(FAIL);

  memset(&tm, 0, sizeof(tm));
  tm.tm_year = (int)yyyy - 1900;
  tm.tm_mon  = (int)mo - 1;
  tm.tm_mday = (int)dd;
  tm.tm_hour = (int)hh;
  tm.tm_min  = (int)mm;
  tm.tm_sec  = (int)ss;

  t = timegm(&tm);

  if(t == (time_t)-1)
    return(FAIL);

  *out_ms = (int64_t)t * 1000;
  return(SUCCESS);
}

static void
wm_bt_cmd_run(const cmd_ctx_t *ctx)
{
  whenmoon_state_t       *st;
  const char             *p;
  char                    pair_tok[64]                = {0};
  char                    name_tok[WM_STRATEGY_NAME_SZ] = {0};
  char                    start_tok[32]               = {0};
  char                    end_tok[32]                 = {0};
  char                    flag_tok[32]                = {0};
  char                    val_tok[256]                = {0};
  char                    exch[32]                    = {0};
  char                    base[16]                    = {0};
  char                    quote[16]                   = {0};
  char                    symbol[32]                  = {0};
  char                    start_ts[40]                = {0};
  char                    end_ts[40]                  = {0};
  char                    err[256];
  char                    reply[320];
  int32_t                 market_id;
  wm_backtest_params_t    params;
  wm_backtest_snapshot_t *snap;
  wm_backtest_result_t    result;
  loaded_strategy_t      *ls;
  wm_bt_sweep_plan_t      sweep_plan;
  wm_bt_sweep_mode_t      sweep_mode;
  wm_bt_walk_spec_t       walk_spec;
  wm_bt_oos_spec_t        oos_spec;
  bool                    have_sweep        = false;
  bool                    have_walk_forward = false;
  bool                    have_oos_tail     = false;
  uint32_t                min_history_1d = 0;

  st = whenmoon_get_state();

  if(st == NULL || !st->dl_ready)
  {
    cmd_reply(ctx, "whenmoon: downloader not ready");
    return;
  }

  if(st->strategies == NULL)
  {
    cmd_reply(ctx, "whenmoon: strategy registry not ready");
    return;
  }

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, pair_tok,  sizeof(pair_tok))  ||
     !wm_dl_next_token(&p, name_tok,  sizeof(name_tok))  ||
     !wm_dl_next_token(&p, start_tok, sizeof(start_tok)) ||
     !wm_dl_next_token(&p, end_tok,   sizeof(end_tok)))
  {
    cmd_reply(ctx,
        "usage: /whenmoon backtest run <market_id> <strategy_name>"
        " <MM/dd/yyyy> <MM/dd/yyyy>"
        " [--fee-bps N] [--slip-bps N]"
        " [--size-frac F] [--cash N]"
        " [--sweep <param>=<v1,v2,...>|<lo:step:hi>] (repeatable)"
        " [--workers N] [--score realized|sharpe|sortino|equity|pf]"
        " [--top K]"
        " [--walk-forward train=Td:test=Md:step=Sd]"
        " [--oos-tail PCT]");
    return;
  }

  if(wm_market_parse_id(pair_tok, exch, sizeof(exch),
         base, sizeof(base), quote, sizeof(quote)) != SUCCESS)
  {
    cmd_reply(ctx, "bad market id (expected <exch>-<base>-<quote>)");
    return;
  }

  // Coinbase wire form is uppercase BASE-QUOTE; mirrors the path in
  // dl_commands.c. EX-1 deferred lifting this into the exchange
  // abstraction.
  {
    int n = snprintf(symbol, sizeof(symbol), "%s-%s", base, quote);
    size_t i;

    if(n < 0 || (size_t)n >= sizeof(symbol))
    {
      cmd_reply(ctx, "market id overflow");
      return;
    }

    for(i = 0; symbol[i] != '\0'; i++)
      symbol[i] = (char)toupper((unsigned char)symbol[i]);
  }

  if(wm_bt_parse_date(start_tok, start_ts, sizeof(start_ts)) != SUCCESS ||
     wm_bt_parse_date(end_tok,   end_ts,   sizeof(end_ts))   != SUCCESS)
  {
    cmd_reply(ctx, "bad date (expected MM/dd/yyyy)");
    return;
  }

  if(strcmp(start_ts, end_ts) >= 0)
  {
    cmd_reply(ctx, "start date must be strictly before end date");
    return;
  }

  // The strategy must be loaded BEFORE we can validate sweep axes
  // against its declared param schema. Resolve here + cache the
  // min-history requirement so the snapshot sizing is right.
  market_id = wm_market_lookup_or_create(exch, base, quote, symbol);

  if(market_id < 0)
  {
    cmd_reply(ctx, "market lookup/create failed");
    return;
  }

  pthread_mutex_lock(&st->strategies->lock);
  ls = wm_strategy_find_loaded(st, name_tok);

  if(ls != NULL)
  {
    uint32_t i;

    for(i = 0; i < WM_GRAN_MAX; i++)
      if(ls->meta.min_history[i] > min_history_1d)
        min_history_1d = ls->meta.min_history[i];
  }
  pthread_mutex_unlock(&st->strategies->lock);

  if(ls == NULL)
  {
    snprintf(reply, sizeof(reply), "strategy %s not loaded", name_tok);
    cmd_reply(ctx, reply);
    return;
  }

  // Parse optional --flag value pairs. The economic knobs apply to
  // every iteration; --sweep / --workers / --score / --top control the
  // sweep planner. --walk-forward / --oos-tail set window scope.
  memset(&params,     0, sizeof(params));
  memset(&sweep_mode, 0, sizeof(sweep_mode));
  memset(&walk_spec,  0, sizeof(walk_spec));
  memset(&oos_spec,   0, sizeof(oos_spec));
  wm_bt_sweep_plan_init(&sweep_plan);

  while(wm_dl_next_token(&p, flag_tok, sizeof(flag_tok)))
  {
    if(!wm_dl_next_token(&p, val_tok, sizeof(val_tok)))
    {
      snprintf(reply, sizeof(reply),
          "missing value for %s", flag_tok);
      cmd_reply(ctx, reply);
      return;
    }

    if(strcmp(flag_tok, "--fee-bps") == 0)
    {
      if(wm_bt_parse_double_flag(val_tok, &params.fee_bps,
             &params.have_fee_bps) != SUCCESS)
      {
        cmd_reply(ctx, "bad --fee-bps value");
        return;
      }
    }
    else if(strcmp(flag_tok, "--slip-bps") == 0)
    {
      if(wm_bt_parse_double_flag(val_tok, &params.slip_bps,
             &params.have_slip_bps) != SUCCESS)
      {
        cmd_reply(ctx, "bad --slip-bps value");
        return;
      }
    }
    else if(strcmp(flag_tok, "--size-frac") == 0)
    {
      if(wm_bt_parse_double_flag(val_tok, &params.size_frac,
             &params.have_size_frac) != SUCCESS)
      {
        cmd_reply(ctx, "bad --size-frac value");
        return;
      }
    }
    else if(strcmp(flag_tok, "--cash") == 0)
    {
      if(wm_bt_parse_double_flag(val_tok, &params.starting_cash,
             &params.have_starting_cash) != SUCCESS)
      {
        cmd_reply(ctx, "bad --cash value");
        return;
      }
    }
    else if(strcmp(flag_tok, "--sweep") == 0)
    {
      err[0] = '\0';

      if(wm_bt_sweep_axis_add(&sweep_plan, ls, val_tok,
             err, sizeof(err)) != SUCCESS)
      {
        snprintf(reply, sizeof(reply),
            "error: %s", err[0] != '\0' ? err : "bad --sweep value");
        cmd_reply(ctx, reply);
        return;
      }

      have_sweep = true;
    }
    else if(strcmp(flag_tok, "--workers") == 0)
    {
      char *end = NULL;
      long  w;

      errno = 0;
      w     = strtol(val_tok, &end, 10);

      if(end == val_tok || errno != 0 || w < 0)
      {
        cmd_reply(ctx, "bad --workers value");
        return;
      }

      sweep_plan.workers = w == 0 ? 1u : (uint32_t)w;
    }
    else if(strcmp(flag_tok, "--score") == 0)
    {
      if(wm_bt_sweep_score_parse(val_tok, &sweep_plan.score) != SUCCESS)
      {
        cmd_reply(ctx,
            "bad --score (expected realized|sharpe|sortino|equity|pf)");
        return;
      }
    }
    else if(strcmp(flag_tok, "--top") == 0)
    {
      char *end = NULL;
      long  k;

      errno = 0;
      k     = strtol(val_tok, &end, 10);

      if(end == val_tok || errno != 0 || k < 0)
      {
        cmd_reply(ctx, "bad --top value");
        return;
      }

      sweep_plan.top_k = (uint32_t)k;
    }
    else if(strcmp(flag_tok, "--walk-forward") == 0)
    {
      if(wm_bt_parse_walk_spec(val_tok, &walk_spec) != SUCCESS)
      {
        cmd_reply(ctx,
            "bad --walk-forward (expected"
            " train=Td:test=Md:step=Sd, days)");
        return;
      }
      have_walk_forward = true;
    }
    else if(strcmp(flag_tok, "--oos-tail") == 0)
    {
      char *end = NULL;
      long  pct;

      errno = 0;
      pct   = strtol(val_tok, &end, 10);

      if(end == val_tok || errno != 0 || *end != '\0' ||
         pct < 1 || pct > 50)
      {
        cmd_reply(ctx, "bad --oos-tail (expected integer 1..50)");
        return;
      }

      oos_spec.pct  = (uint32_t)pct;
      have_oos_tail = true;
    }
    else
    {
      snprintf(reply, sizeof(reply),
          "unknown flag '%s' (expected --fee-bps/--slip-bps/"
          "--size-frac/--cash/--sweep/--workers/--score/--top/"
          "--walk-forward/--oos-tail)",
          flag_tok);
      cmd_reply(ctx, reply);
      return;
    }
  }

  if(have_walk_forward && have_oos_tail)
  {
    cmd_reply(ctx,
        "--walk-forward and --oos-tail are mutually exclusive");
    return;
  }

  // Default top_k for sweeps when caller didn't specify.
  if(have_sweep && sweep_plan.top_k == 1)
    sweep_plan.top_k = WM_BT_DEFAULT_TOP_K;

  // OOS validation needs at least a few top-K rows to be useful.
  // When --oos-tail is set without --sweep or --top, default top to
  // a small number so the post-pass actually has work to do.
  if(have_oos_tail && sweep_plan.top_k == 1 && !have_sweep)
    sweep_plan.top_k = 1;
  else if(have_oos_tail && sweep_plan.top_k == 1)
    sweep_plan.top_k = WM_BT_DEFAULT_TOP_K;

  if(wm_bt_sweep_plan_finalize(&sweep_plan, err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "error: %s", err[0] != '\0' ? err : "sweep finalize failed");
    cmd_reply(ctx, reply);
    return;
  }

  // Build the sweep mode + windows. Resolves +/- the in-memory range
  // bounds in ms epoch so the window builders can slice deterministically.
  {
    int64_t range_start_ms = 0;
    int64_t range_end_ms   = 0;

    sweep_mode.mode = WM_BT_MODE_FULL;

    if(have_walk_forward || have_oos_tail)
    {
      if(wm_bt_parse_ts_ms(start_ts, &range_start_ms) != SUCCESS ||
         wm_bt_parse_ts_ms(end_ts,   &range_end_ms)   != SUCCESS)
      {
        cmd_reply(ctx, "internal: range timestamp parse failed");
        return;
      }
    }

    if(have_walk_forward)
    {
      err[0] = '\0';

      if(wm_bt_walk_build_windows(&walk_spec, range_start_ms,
             range_end_ms, ls, &sweep_mode.walk,
             err, sizeof(err)) != SUCCESS)
      {
        snprintf(reply, sizeof(reply),
            "walk-forward error: %s",
            err[0] != '\0' ? err : "(unknown)");
        cmd_reply(ctx, reply);
        return;
      }

      sweep_mode.mode = WM_BT_MODE_WALK_FORWARD;
    }

    else if(have_oos_tail)
    {
      err[0] = '\0';

      if(wm_bt_oos_split_range(&oos_spec, range_start_ms, range_end_ms,
             &sweep_mode.oos_head, &sweep_mode.oos_tail,
             err, sizeof(err)) != SUCCESS)
      {
        snprintf(reply, sizeof(reply),
            "oos-tail error: %s",
            err[0] != '\0' ? err : "(unknown)");
        cmd_reply(ctx, reply);
        return;
      }

      sweep_mode.mode = WM_BT_MODE_OOS;
    }
  }

  if(min_history_1d == 0)
    min_history_1d = WM_AGG_DEFAULT_HISTORY_1D;

  // Pre-flight gap check.
  err[0] = '\0';

  if(wm_backtest_preflight_gap(market_id, start_ts, end_ts,
         err, sizeof(err)) != SUCCESS)
  {
    cmd_reply(ctx, err[0] != '\0' ? err
                                  : "1m candle coverage missing");
    return;
  }

  cmd_reply(ctx, "warming snapshot...");

  snap = wm_backtest_snapshot_build(market_id, pair_tok,
      start_ts, end_ts, min_history_1d, err, sizeof(err));

  if(snap == NULL)
  {
    snprintf(reply, sizeof(reply), "snapshot build failed: %s",
        err[0] != '\0' ? err : "unknown");
    cmd_reply(ctx, reply);
    return;
  }

  if(have_sweep || have_walk_forward || have_oos_tail)
  {
    wm_bt_sweep_result_t *sweep_results;
    struct timespec       t0, t1;
    uint64_t              wallclock_ms;
    uint32_t              i;
    uint32_t              n_ok = 0;
    const char           *mode_label =
        sweep_mode.mode == WM_BT_MODE_WALK_FORWARD ? "walk" :
        sweep_mode.mode == WM_BT_MODE_OOS          ? "oos"  :
                                                     "full";

    if(sweep_mode.mode == WM_BT_MODE_WALK_FORWARD)
      snprintf(reply, sizeof(reply),
          "snapshot ready: %u 1m bars; walk-forward N=%u windows=%u"
          " workers=%u score=%s top=%u",
          snap->bars_loaded_1m, sweep_plan.total_iters,
          sweep_mode.walk.n,
          sweep_plan.workers,
          wm_bt_sweep_score_name(sweep_plan.score), sweep_plan.top_k);
    else if(sweep_mode.mode == WM_BT_MODE_OOS)
      snprintf(reply, sizeof(reply),
          "snapshot ready: %u 1m bars; oos head N=%u (oos_tail=%u%%)"
          " workers=%u score=%s top=%u",
          snap->bars_loaded_1m, sweep_plan.total_iters,
          oos_spec.pct, sweep_plan.workers,
          wm_bt_sweep_score_name(sweep_plan.score), sweep_plan.top_k);
    else
      snprintf(reply, sizeof(reply),
          "snapshot ready: %u 1m bars; sweep N=%u workers=%u"
          " score=%s top=%u",
          snap->bars_loaded_1m, sweep_plan.total_iters,
          sweep_plan.workers,
          wm_bt_sweep_score_name(sweep_plan.score), sweep_plan.top_k);
    cmd_reply(ctx, reply);

    sweep_results = mem_alloc("whenmoon.backtest", "sweep_results",
        sizeof(*sweep_results) * (size_t)sweep_plan.total_iters);

    if(sweep_results == NULL)
    {
      cmd_reply(ctx, "out of memory allocating sweep result table");
      wm_backtest_snapshot_free(snap);
      return;
    }

    err[0] = '\0';

    clock_gettime(CLOCK_MONOTONIC, &t0);

    if(wm_bt_sweep_run(st, snap, name_tok, market_id, &sweep_plan,
           &sweep_mode, &params, sweep_results,
           err, sizeof(err)) != SUCCESS)
    {
      snprintf(reply, sizeof(reply),
          "sweep run failed: %s",
          err[0] != '\0' ? err : "unknown");
      cmd_reply(ctx, reply);
      mem_free(sweep_results);
      wm_backtest_snapshot_free(snap);
      return;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);

    wallclock_ms = (uint64_t)((int64_t)(t1.tv_sec - t0.tv_sec) * 1000
                 + (int64_t)(t1.tv_nsec - t0.tv_nsec) / 1000000);

    for(i = 0; i < sweep_plan.total_iters; i++)
      if(sweep_results[i].ok)
        n_ok++;

    snprintf(reply, sizeof(reply),
        "%s complete: %u/%u ok in %" PRIu64 " ms (%.1f iter/s)",
        mode_label,
        n_ok, sweep_plan.total_iters, wallclock_ms,
        wallclock_ms > 0
            ? (double)sweep_plan.total_iters * 1000.0 / (double)wallclock_ms
            : 0.0);
    cmd_reply(ctx, reply);

    if(sweep_mode.mode == WM_BT_MODE_OOS && n_ok > 0)
    {
      err[0] = '\0';

      if(wm_bt_sweep_run_oos_validation(st, snap, name_tok, market_id,
             &sweep_plan, &sweep_mode.oos_tail, &params,
             sweep_results, err, sizeof(err)) != SUCCESS)
      {
        snprintf(reply, sizeof(reply),
            "oos validation: %s",
            err[0] != '\0' ? err : "(no eligible top-K)");
        cmd_reply(ctx, reply);
      }
      else
        cmd_reply(ctx, "oos validation: top-K patched with oos columns");
    }

    wm_bt_sweep_render_topk(ctx, &sweep_plan, &sweep_mode, sweep_results,
        sweep_plan.total_iters);

    mem_free(sweep_results);
    wm_backtest_snapshot_free(snap);
    return;
  }

  snprintf(reply, sizeof(reply),
      "snapshot ready: %u 1m bars, running iteration...",
      snap->bars_loaded_1m);
  cmd_reply(ctx, reply);

  err[0] = '\0';

  if(wm_backtest_run_iteration(st, snap, name_tok, &params,
         &result, err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply), "iteration failed: %s",
        err[0] != '\0' ? err : "unknown");
    cmd_reply(ctx, reply);
    wm_backtest_snapshot_free(snap);
    return;
  }

  // Persist. The DDL inserts return the assigned run_id; we stash it
  // back on the result struct so the success line names a stable id
  // the operator can dig into via /show whenmoon backtest <id>.
  {
    wm_backtest_record_t rec;
    char                 metrics_json[1024];
    char                 params_json[256];
    int64_t              new_run_id = 0;

    memset(&rec, 0, sizeof(rec));
    rec.market_id     = market_id;
    snprintf(rec.strategy_name, sizeof(rec.strategy_name), "%s", name_tok);
    snprintf(rec.range_start, sizeof(rec.range_start), "%s", start_ts);
    snprintf(rec.range_end,   sizeof(rec.range_end),   "%s", end_ts);
    rec.wallclock_ms  = (int64_t)result.wallclock_ms;
    rec.bars_replayed = result.bars_replayed;
    rec.n_trades      = result.trade.metrics.n_trades;
    rec.realized_pnl  = result.trade.metrics.realized_pnl;
    rec.max_drawdown  = result.trade.metrics.max_drawdown;
    rec.sharpe        = result.trade.metrics.sharpe;
    rec.sortino       = result.trade.metrics.sortino;
    rec.final_equity  = result.trade.equity;

    wm_bt_params_to_json(&params, params_json, sizeof(params_json));
    wm_bt_metrics_to_json(&result.trade,
        metrics_json, sizeof(metrics_json));

    if(wm_backtest_persist_run(&rec,
           params.have_fee_bps || params.have_slip_bps ||
           params.have_size_frac || params.have_starting_cash
               ? params_json : NULL,
           metrics_json, &new_run_id) == SUCCESS)
      result.run_id_db = new_run_id;
  }

  wm_backtest_snapshot_free(snap);

  if(result.run_id_db > 0)
  {
    snprintf(reply, sizeof(reply),
        "run %" PRId64 ": bars=%u trades=%u realized=%+.4f"
        " sharpe=%.3f equity=%.2f wallclock_ms=%" PRIu64,
        result.run_id_db, result.bars_replayed,
        result.trade.metrics.n_trades,
        result.trade.metrics.realized_pnl,
        result.trade.metrics.sharpe, result.trade.equity,
        result.wallclock_ms);
    cmd_reply(ctx, reply);
    cmd_reply(ctx, "  detail: /show whenmoon backtest <run_id>");
  }
  else
  {
    snprintf(reply, sizeof(reply),
        "iteration completed but persist failed:"
        " bars=%u trades=%u realized=%+.4f equity=%.2f",
        result.bars_replayed, result.trade.metrics.n_trades,
        result.trade.metrics.realized_pnl, result.trade.equity);
    cmd_reply(ctx, reply);
  }
}

// ----------------------------------------------------------------------- //
// /whenmoon backtest reload <strategy_name>                               //
// ----------------------------------------------------------------------- //
//
// Mirrors /whenmoon strategy reload but runs through the sweep gate
// (waits for in-flight sweeps to drain before dlclose). The strategy
// reload path itself is shared.

static void
wm_bt_cmd_reload(const cmd_ctx_t *ctx)
{
  whenmoon_state_t *st;
  const char       *p;
  char              name_tok[WM_STRATEGY_NAME_SZ] = {0};
  char              err[192];
  char              reply[256];
  uint32_t          n_detached = 0;

  st = whenmoon_get_state();

  if(st == NULL || st->strategies == NULL)
  {
    cmd_reply(ctx, "whenmoon: strategy registry not ready");
    return;
  }

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, name_tok, sizeof(name_tok)))
  {
    cmd_reply(ctx,
        "usage: /whenmoon backtest reload <strategy_name>");
    return;
  }

  err[0] = '\0';

  if(wm_bt_sweep_reload_strategy(st, name_tok, &n_detached,
         err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply), "reload failed: %s",
        err[0] != '\0' ? err : "unknown");
    cmd_reply(ctx, reply);
    return;
  }

  snprintf(reply, sizeof(reply),
      "reloaded %s (n_detached=%u, dlclose+dlopen ok)",
      name_tok, n_detached);
  cmd_reply(ctx, reply);
  cmd_reply(ctx,
      "  re-attach: /whenmoon strategy attach <market_id> <name>");
}

// ----------------------------------------------------------------------- //
// /whenmoon backtest parent                                               //
// ----------------------------------------------------------------------- //

static void
wm_bt_parent_cb(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /whenmoon backtest <run> ...");
}

// ----------------------------------------------------------------------- //
// /show whenmoon backtest                                                 //
// ----------------------------------------------------------------------- //

static void
wm_bt_show_list(const cmd_ctx_t *ctx, uint32_t cap)
{
  wm_backtest_record_t *recs;
  uint32_t              n;
  uint32_t              i;
  char                  line[256];
  char                  hdr[96];

  if(cap == 0)            cap = WM_BT_LIST_DEFAULT;
  if(cap > WM_BT_LIST_MAX) cap = WM_BT_LIST_MAX;

  recs = mem_alloc("whenmoon.backtest", "list_buf",
      sizeof(*recs) * (size_t)cap);

  if(recs == NULL)
  {
    cmd_reply(ctx, "out of memory");
    return;
  }

  n = wm_backtest_recent_runs(recs, cap);

  snprintf(hdr, sizeof(hdr),
      CLR_BOLD "whenmoon backtest runs (%u)" CLR_RESET, n);
  cmd_reply(ctx, hdr);

  if(n == 0)
  {
    cmd_reply(ctx,
        "  (none — issue /whenmoon backtest run <market> <strat>"
        " <start> <end>)");
    mem_free(recs);
    return;
  }

  for(i = 0; i < n; i++)
  {
    const wm_backtest_record_t *r = &recs[i];

    snprintf(line, sizeof(line),
        "  #%-6" PRId64 " %-24s mid=%-4" PRId32 " %.10s..%.10s"
        " trades=%-3u realized=%+9.2f equity=%9.2f sharpe=%6.3f",
        r->run_id, r->strategy_name, r->market_id,
        r->range_start, r->range_end,
        r->n_trades, r->realized_pnl, r->final_equity, r->sharpe);
    cmd_reply(ctx, line);
  }

  mem_free(recs);
}

static void
wm_bt_show_detail(const cmd_ctx_t *ctx, int64_t run_id)
{
  wm_backtest_record_t  rec;
  char                 *metrics_json = NULL;
  char                 *params_json  = NULL;
  char                  line[320];

  if(wm_backtest_lookup_run(run_id, &rec, &metrics_json, &params_json)
     != SUCCESS)
  {
    snprintf(line, sizeof(line), "run %" PRId64 " not found", run_id);
    cmd_reply(ctx, line);
    return;
  }

  snprintf(line, sizeof(line),
      CLR_BOLD "backtest #%" PRId64 CLR_RESET
      "  %s  market_id=%" PRId32,
      rec.run_id, rec.strategy_name, rec.market_id);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  range:    %s .. %s", rec.range_start, rec.range_end);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  created:  %s  wallclock=%" PRId64 " ms  bars_replayed=%u",
      rec.created_at, rec.wallclock_ms, rec.bars_replayed);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  trades=%-3u realized=%+9.4f final_equity=%9.4f"
      " max_dd=%9.4f sharpe=%6.3f sortino=%6.3f",
      rec.n_trades, rec.realized_pnl, rec.final_equity,
      rec.max_drawdown, rec.sharpe, rec.sortino);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  window:   kind=%s n_windows=%u",
      rec.window_kind[0] != '\0' ? rec.window_kind : "full",
      rec.n_windows > 0 ? rec.n_windows : 1);
  cmd_reply(ctx, line);

  if(rec.have_oos)
  {
    snprintf(line, sizeof(line),
        "  oos:      score=%+.4f realized=%+9.4f trades=%u",
        rec.oos_score, rec.oos_realized, rec.oos_n_trades);
    cmd_reply(ctx, line);
  }

  if(params_json != NULL && params_json[0] != '\0' &&
     strcmp(params_json, "null") != 0)
  {
    snprintf(line, sizeof(line), "  params:   %s", params_json);
    cmd_reply(ctx, line);
  }
  else
  {
    cmd_reply(ctx, "  params:   (defaults)");
  }

  if(metrics_json != NULL)
  {
    snprintf(line, sizeof(line), "  metrics:  %s", metrics_json);
    cmd_reply(ctx, line);
  }

  if(metrics_json != NULL) mem_free(metrics_json);
  if(params_json  != NULL) mem_free(params_json);
}

static void
wm_bt_cmd_show(const cmd_ctx_t *ctx)
{
  const char *p;
  char        tok[32] = {0};
  char       *end     = NULL;
  int64_t     run_id;

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, tok, sizeof(tok)) || tok[0] == '\0' ||
     strcmp(tok, "list") == 0)
  {
    wm_bt_show_list(ctx, WM_BT_LIST_DEFAULT);
    return;
  }

  errno  = 0;
  run_id = (int64_t)strtoll(tok, &end, 10);

  if(end == tok || errno != 0 || run_id <= 0)
  {
    cmd_reply(ctx,
        "usage: /show whenmoon backtest [list|<run_id>]");
    return;
  }

  wm_bt_show_detail(ctx, run_id);
}

// ----------------------------------------------------------------------- //
// Registration                                                            //
// ----------------------------------------------------------------------- //

bool
wm_backtest_register_verbs(void)
{
  // /whenmoon backtest parent.
  if(cmd_register("whenmoon", "backtest",
        "whenmoon backtest <verb> ...",
        "Backtest runner + sweep planner.",
        "Subcommands: run <market_id> <strat> <start> <end> [...],"
        " reload <strat>.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_bt_parent_cb, NULL, "whenmoon", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "run",
        "whenmoon backtest run <market_id> <strategy_name>"
        " <MM/dd/yyyy> <MM/dd/yyyy>"
        " [--fee-bps N] [--slip-bps N] [--size-frac F] [--cash N]"
        " [--sweep <param>=<v1,v2,...>|<lo:step:hi>] (repeatable)"
        " [--workers N] [--score realized|sharpe|sortino|equity|pf]"
        " [--top K]"
        " [--walk-forward train=Td:test=Md:step=Sd]"
        " [--oos-tail PCT]",
        "Run a single backtest, a parameter sweep, walk-forward, or"
        " an OOS-tail validation.",
        "Builds an isolated market snapshot from wm_candles_<id>_60"
        " over the given range and runs the strategy through a paper"
        " trade book in PAPER mode. With one or more --sweep axes,"
        " expands the cartesian product of values and dispatches each"
        " iteration through a worker pool (--workers, default 1; max"
        " 64). Each iteration runs on a private trade-book registry so"
        " parallel workers do not contend on a global mutex.\n"
        "--score selects the ranking metric (default realized).\n"
        "--top selects the top-K rows shown after the run (default 20"
        " when sweeping, 1 otherwise).\n"
        "--walk-forward expands each param vector into N test windows"
        " (train days warm the strategy state but only test windows"
        " accumulate fills); the recorded score is the cumulative"
        " test-window result.\n"
        "--oos-tail PCT reserves the last PCT%% of the range as out-of"
        "-sample; the sweep optimises on the head, then the post-pass"
        " runs the top-K on the tail and patches the persisted row"
        " with the OOS columns. PCT clamped to [1, 50].\n"
        "--walk-forward and --oos-tail are mutually exclusive.\n"
        "Every iteration is persisted to wm_backtest_run; --top only"
        " controls render volume. Pre-flight gap check fails fast with"
        " the canonical /whenmoon download candles invocation when 1m"
        " coverage has gaps.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_bt_cmd_run, NULL, "whenmoon/backtest", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "reload",
        "whenmoon backtest reload <strategy_name>",
        "Reload a strategy plugin under the sweep gate.",
        "Detaches all attachments, dlclose+dlopen+resolve+init the"
        " strategy plugin, then re-scans the registry. Acquires the"
        " global reload lock first and waits for in-flight sweep runs"
        " to drain (CLAM_INFO every 5s while waiting) — this prevents"
        " a dlclose from invalidating function pointers cached for an"
        " active worker iteration. Re-attach manually after reload.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_bt_cmd_reload, NULL, "whenmoon/backtest", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  // /show whenmoon backtest
  if(cmd_register("whenmoon", "backtest",
        "show whenmoon backtest [list|<run_id>]",
        "List recent backtest runs (no arg or 'list')"
        " or show detail for one run.",
        "List view (default 20 rows): run_id, strategy, market_id,"
        " range, trades, realized PnL, final equity, sharpe.\n"
        "Detail view: includes params + full JSONB metrics.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_bt_cmd_show, NULL, "show/whenmoon", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
