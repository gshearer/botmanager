// botmanager — MIT
// Whenmoon backtest admin verbs.
//
//   /whenmoon backtest run     <path.wm> <strategy>
//                              [name=value ...] [--flag value ...]
//   /whenmoon backtest reload  <strategy>
//   /whenmoon backtest compile <market_id> <path.wm> [<days>]
//   /whenmoon backtest inspect <path.wm>
//
// `run` mmap's a compiled .wm snapshot and dispatches the parameter
// space (positional `name=value` axes form a cartesian product;
// N=0 collapses to a single iteration) through the sweep worker
// pool. Workers each construct a per-iteration synthetic
// whenmoon_market_t and never touch the live market registry. Each
// invocation emits a sweep directory under
// `plugin.whenmoon.backtest.report_path` carrying `manifest.json`,
// `iterations.jsonl`, a top-N.txt placeholder (BT-7 fills), and an
// empty `charts/` subdir (BT-8 fills when charts_enabled). Top-N
// also renders to the caller's cmd_ctx for immediate feedback.
//
// History: WM-LT-5 shipped synchronous single-iter on the cmd worker;
// WM-LT-6 added the worker pool + sweep planner; WM-BT-1 ripped DB
// persistence; WM-BT-2/3 added the .wm binary format + compile/inspect
// verbs; WM-BT-4 extended sweep syntax (brackets + JSON --config);
// WM-BT-5 made the thread pool host-friendly (sysconf-2 default, nice
// 19 workers); WM-BT-6 collapses the run path to mmap-only + disk
// artifacts and renames `--workers/--score/--top` to
// `--threads/--rank-by/--top-n`.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "aggregator.h"
#include "backtest.h"
#include "dl_commands.h"
#include "dl_schema.h"
#include "market.h"
#include "strategy.h"
#include "sweep.h"
#include "wm_bt_assets.h"
#include "wm_bt_chart.h"
#include "wm_bt_file.h"
#include "wm_bt_report.h"

#include "alloc.h"
#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "kv.h"
#include "task.h"
#include "userns.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include <json-c/json.h>

// ----------------------------------------------------------------------- //
// /whenmoon backtest run                                                  //
// ----------------------------------------------------------------------- //

// WM-BT-8: free every per-row deep fills buffer attached to a sweep
// results table. Safe on a memset-zeroed table (NULL pointers no-op).
// Resets pointers + counters so a follow-up double-free attempt also
// no-ops.
static void
wm_bt_results_free_fills(wm_bt_sweep_result_t *results, uint32_t n)
{
  uint32_t i;

  if(results == NULL)
    return;

  for(i = 0; i < n; i++)
  {
    if(results[i].fills != NULL)
    {
      mem_free(results[i].fills);
      results[i].fills   = NULL;
      results[i].n_fills = 0;
    }
  }
}

// WM-BT-8: linear-scan slice-window builder. Returns the first and
// one-past-last indices into `ring` such that bars [start, end) cover
// [entry_ts - WM_BT_CHART_PADDING_BARS, exit_ts + WM_BT_CHART_PADDING_BARS]
// clamped to [0, ring_n). Linear because rings cap at ~tens of
// thousands at the deepest grain; a tighter binary search would not
// matter at this scale. Returns false only when the ring is empty
// (ring_n == 0); a trade that spans no bar close at this grain (e.g. a
// sub-day trade never crossing a 00:00 UTC daily close, on the 1d ring)
// is anchored on the nearest bar so every non-empty grain is still
// charted with surrounding context.
static bool
wm_bt_chart_slice_indices(const wm_candle_full_t *ring, uint32_t ring_n,
    int64_t entry_ts_ms, int64_t exit_ts_ms,
    uint32_t *out_start, uint32_t *out_end)
{
  uint32_t b;
  uint32_t start;
  uint32_t end;

  if(ring == NULL || ring_n == 0)
    return(false);

  // First bar whose ts_close_ms >= entry_ts.
  start = ring_n;

  for(b = 0; b < ring_n; b++)
  {
    if(ring[b].ts_close_ms >= entry_ts_ms)
    {
      start = b;
      break;
    }
  }

  // Last bar whose ts_close_ms <= exit_ts (end exclusive => +1).
  end = 0;

  for(b = ring_n; b > 0; b--)
  {
    if(ring[b - 1].ts_close_ms <= exit_ts_ms)
    {
      end = b;
      break;
    }
  }

  // Empty raw window: the trade falls between two consecutive bar closes
  // (e.g. a 4h trade on the 1d ring, never crossing a 00:00 UTC daily
  // close) or sits entirely outside the ring. Anchor on the nearest bar
  // so the padding below still yields a context window and the grain is
  // charted, rather than dropping it.
  if(start >= ring_n || end <= start)
  {
    uint32_t anchor = (start < ring_n) ? start : (ring_n - 1);

    start = anchor;
    end   = anchor + 1;
  }

  // Pad both sides; clamp to ring bounds.
  if(start >= WM_BT_CHART_PADDING_BARS)
    start -= WM_BT_CHART_PADDING_BARS;
  else
    start = 0;

  if(end + WM_BT_CHART_PADDING_BARS < ring_n)
    end += WM_BT_CHART_PADDING_BARS;
  else
    end = ring_n;

  *out_start = start;
  *out_end   = end;
  return(true);
}

// WM-BT-8: walk top-K iterations × every snapshot grain × matched
// buy→sell fill pairs, emit one Lightweight Charts HTML per trade. All
// grains the snapshot carries (1m..1d) are charted, not just the
// strategy's subscribed grain(s), so a trade can be reviewed across
// timeframes; empty rings are skipped.
// Per-iteration directory `<sweep_dir>/charts/iter-K/` (1-based K) is
// created lazily; mkdir EEXIST is benign. Skips iterations with zero
// fills + unpairable fill sequences. Caller-visible:
//   - cmd_reply "charts: emitted N file(s) across K iteration(s)"
//   - cmd_reply "warn: ..." per chart_emit / mkdir failure (continues)
//   - cmd_reply "warn: large sweep" once if total file count crosses
//     WM_BT_CHARTS_WARN_THRESHOLD (a soft signal — 6 grains × many
//     trades × a large top-N adds up fast).
#define WM_BT_CHARTS_WARN_THRESHOLD  1000u

static void
wm_bt_cmd_run_emit_charts(const cmd_ctx_t *ctx,
    const wm_bt_sweep_result_t *results, uint32_t n_results,
    const wm_bt_sweep_plan_t *plan,
    const wm_backtest_snapshot_t *snap,
    const char *sweep_dir)
{
  uint64_t  cap_kv;
  uint32_t  top_n;
  uint32_t *top_idx;
  uint32_t  actual;
  uint32_t  k;
  uint32_t  charts_emitted = 0;
  bool      warned_threshold = false;
  char      reply[640];

  if(plan->top_k == 0 || n_results == 0)
    return;

  cap_kv = kv_get_uint("plugin.whenmoon.backtest.charts_top_n");
  top_n  = plan->top_k;

  if(cap_kv > 0 && (uint64_t)top_n > cap_kv)
    top_n = (uint32_t)cap_kv;

  top_idx = mem_alloc("whenmoon.backtest", "charts_top_idx",
      sizeof(*top_idx) * (size_t)top_n);

  if(top_idx == NULL)
  {
    cmd_reply(ctx, "warn: charts: top-K alloc failed");
    return;
  }

  actual = wm_bt_topk_compute(results, n_results, top_n, top_idx);

  // Chart every grain the snapshot carries (1m..1d), not just the
  // strategy's subscribed grain(s): the aggregator computes all grains
  // during warmup regardless of what the strategy reads, so each trade
  // can be reviewed across timeframes. Empty rings are skipped below.
  for(k = 0; k < actual; k++)
  {
    const wm_bt_sweep_result_t *res = &results[top_idx[k]];
    char                        iter_dir[1280];
    int                         n;
    uint32_t                    g;

    if(res->n_fills == 0 || res->fills == NULL)
      continue;

    n = snprintf(iter_dir, sizeof(iter_dir),
        "%s/charts/iter-%u", sweep_dir, k + 1);

    if(n < 0 || (size_t)n >= sizeof(iter_dir))
    {
      snprintf(reply, sizeof(reply),
          "warn: charts: iter-%u dir path overflow", k + 1);
      cmd_reply(ctx, reply);
      continue;
    }

    if(mkdir(iter_dir, WM_BT_REPORT_DIR_MODE) != 0 && errno != EEXIST)
    {
      snprintf(reply, sizeof(reply),
          "warn: charts: mkdir('%.512s') failed: %s",
          iter_dir, strerror(errno));
      cmd_reply(ctx, reply);
      continue;
    }

    for(g = 0; g < WM_GRAN_MAX; g++)
    {
      const wm_candle_full_t *ring;
      uint32_t                ring_n;
      uint32_t                trade_idx = 0;
      wm_bt_trade_iter_t      it;
      const wm_market_fill_t *entry;
      const wm_market_fill_t *exit_fill;

      ring   = snap->mkt.grain_arr[g];
      ring_n = snap->mkt.grain_n[g];

      if(ring == NULL || ring_n == 0)
        continue;

      // Pair walk via the shared iterator (WM-BT-RPT-3): buy -> matching
      // sell, defensive against an unmatched trailing buy (open-at-end).
      it.fills = res->fills;
      it.n     = res->n_fills;
      it.pos   = 0;

      while(wm_bt_trade_next(&it, &entry, &exit_fill))
      {
        uint32_t                start_idx;
        uint32_t                end_idx;
        uint32_t                slice_n;
        int64_t                 entry_ts;
        int64_t                 exit_ts;
        char                    chart_err[160];

        trade_idx++;

        entry_ts = entry->ts_ms;
        exit_ts  = (exit_fill != NULL) ? exit_fill->ts_ms : entry_ts;

        if(!wm_bt_chart_slice_indices(ring, ring_n,
               entry_ts, exit_ts, &start_idx, &end_idx))
          continue;

        slice_n = end_idx - start_idx;

        chart_err[0] = '\0';

        if(wm_bt_chart_emit(iter_dir, k + 1, trade_idx, (wm_gran_t)g,
               &ring[start_idx], slice_n,
               entry, exit_fill,
               chart_err, sizeof(chart_err)) != SUCCESS)
        {
          snprintf(reply, sizeof(reply),
              "warn: charts: iter-%u trade-%u %s: %s",
              k + 1, trade_idx, wm_bt_chart_gran_name((wm_gran_t)g),
              chart_err[0] != '\0' ? chart_err : "(unknown)");
          cmd_reply(ctx, reply);
          continue;
        }

        charts_emitted++;

        if(!warned_threshold &&
           charts_emitted > WM_BT_CHARTS_WARN_THRESHOLD)
        {
          snprintf(reply, sizeof(reply),
              "warn: charts: %u files emitted so far (one per round-trip"
              " trade x every grain); this is a long backtest — the"
              " charts/ dir will be large",
              charts_emitted);
          cmd_reply(ctx, reply);
          warned_threshold = true;
        }
      }
    }
  }

  mem_free(top_idx);

  snprintf(reply, sizeof(reply),
      "charts: emitted %u file(s) across %u top-K iteration(s)"
      " -> %s/charts/",
      charts_emitted, actual, sweep_dir);
  cmd_reply(ctx, reply);
}

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

// WM-BT-6 unified entry. argv shape:
//   <path.wm> <strategy> [name=value ...] [--flag value ...]
//
// `name=value` tokens are positional sweep specs handed to
// wm_bt_sweep_axis_add (same parser as the WM-BT-3..5 `--sweep` flag).
// Iterations always run through wm_bt_sweep_run — the N=1 "single
// iter" path is just a sweep_plan with zero axes (total_iters = 1).
// Range timestamps come from the mmap'd .wm header, so the verb no
// longer takes start/end dates.
//
// Each invocation emits a sweep directory under
// `plugin.whenmoon.backtest.report_path` (defaulting to
// `$HOME/.local/share/botmanager/backtests/<sweep_id>/`) containing
// `manifest.json`, `iterations.jsonl`, `top-N.txt` (BT-7 placeholder),
// and an empty `charts/` (BT-8 fills it).
static void
wm_bt_cmd_run(const cmd_ctx_t *ctx)
{
  whenmoon_state_t          *st;
  const char                *p;
  char                       path_tok[256]                 = {0};
  char                       name_tok[WM_STRATEGY_NAME_SZ] = {0};
  char                       tok[256]                      = {0};
  char                       val_tok[256]                  = {0};
  char                       err[320];
  char                       reply[640];
  char                       sweep_id[160]                 = {0};
  char                       report_root[1024]             = {0};
  char                       sweep_dir[1024]               = {0};
  wm_backtest_params_t       params;
  wm_backtest_snapshot_t    *snap          = NULL;
  loaded_strategy_t         *ls;
  wm_bt_sweep_plan_t         sweep_plan;
  wm_bt_sweep_mode_t         sweep_mode;
  wm_bt_walk_spec_t          walk_spec;
  wm_bt_oos_spec_t           oos_spec;
  wm_bt_sweep_result_t      *sweep_results = NULL;
  wm_bt_iterations_writer_t  writer;
  struct timespec            t0;
  struct timespec            t1;
  uint64_t                   wallclock_ms = 0;
  uint32_t                   i;
  uint32_t                   n_ok          = 0;
  uint32_t                   n_fail        = 0;
  bool                       writer_open   = false;
  bool                       have_axes     = false;
  bool                       have_walk     = false;
  bool                       have_oos      = false;
  bool                       charts_force  = false;  // --charts seen

  st = whenmoon_get_state();

  if(st == NULL || st->strategies == NULL)
  {
    cmd_reply(ctx, "whenmoon: strategy registry not ready");
    return;
  }

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, path_tok, sizeof(path_tok)) ||
     !wm_dl_next_token(&p, name_tok, sizeof(name_tok)))
  {
    cmd_reply(ctx,
        "usage: /whenmoon backtest run <path.wm> <strategy>"
        " [<name>=<v|[v,...]|lo:step:hi>] (repeatable)"
        " [--fee-bps N] [--slip-bps N] [--size-frac F] [--cash N]"
        " [--config <path.json>] [--threads N]"
        " [--rank-by realized|sharpe|sortino|equity|pf]"
        " [--top-n K]"
        " [--walk-forward train=Td:test=Md:step=Sd]"
        " [--oos-tail PCT]"
        " [--charts]");
    return;
  }

  pthread_mutex_lock(&st->strategies->lock);
  ls = wm_strategy_find_loaded(st, name_tok);
  pthread_mutex_unlock(&st->strategies->lock);

  if(ls == NULL)
  {
    snprintf(reply, sizeof(reply), "strategy %s not loaded", name_tok);
    cmd_reply(ctx, reply);
    return;
  }

  err[0] = '\0';
  snap = wm_bt_file_open(path_tok, err, sizeof(err));

  if(snap == NULL)
  {
    snprintf(reply, sizeof(reply), ".wm open failed: %s",
        err[0] != '\0' ? err : path_tok);
    cmd_reply(ctx, reply);
    return;
  }

  memset(&params,     0, sizeof(params));
  memset(&sweep_mode, 0, sizeof(sweep_mode));
  memset(&walk_spec,  0, sizeof(walk_spec));
  memset(&oos_spec,   0, sizeof(oos_spec));
  wm_bt_sweep_plan_init(&sweep_plan);

  // Pass 1: --config only. Loaded first so inline `name=value` axes
  // (parsed in Pass 2) can replace its entries via the
  // wm_bt_sweep_axis_add replace-on-collision semantic — preserving
  // "inline beats file" regardless of argv order.
  {
    const char *p_pre = p;
    char        f1[64];
    char        v1[256];

    while(wm_dl_next_token(&p_pre, f1, sizeof(f1)))
    {
      if(f1[0] != '-' || f1[1] != '-')
        continue;

      if(!wm_dl_next_token(&p_pre, v1, sizeof(v1)))
        break;

      if(strcmp(f1, "--config") != 0)
        continue;

      err[0] = '\0';

      if(wm_bt_load_config_file(&sweep_plan, ls, v1,
             err, sizeof(err)) != SUCCESS)
      {
        snprintf(reply, sizeof(reply),
            "error: %s", err[0] != '\0' ? err : "bad --config value");
        cmd_reply(ctx, reply);
        wm_backtest_snapshot_free(snap);
        return;
      }

      have_axes = true;
    }
  }

  // Pass 2: positional `name=value` sweep specs + every other --flag.
  while(wm_dl_next_token(&p, tok, sizeof(tok)))
  {
    if(tok[0] == '-' && tok[1] == '-')
    {
      // Value-less flags (WM-BT-8). Recognise first so they don't
      // accidentally steal the next flag's value token.
      if(strcmp(tok, "--charts") == 0)
      {
        charts_force = true;
        continue;
      }

      if(!wm_dl_next_token(&p, val_tok, sizeof(val_tok)))
      {
        snprintf(reply, sizeof(reply),
            "missing value for %s", tok);
        cmd_reply(ctx, reply);
        wm_backtest_snapshot_free(snap);
        return;
      }

      if(strcmp(tok, "--config") == 0)
        continue;          // handled in Pass 1

      if(strcmp(tok, "--fee-bps") == 0)
      {
        if(wm_bt_parse_double_flag(val_tok, &params.fee_bps,
               &params.have_fee_bps) != SUCCESS)
        {
          cmd_reply(ctx, "bad --fee-bps value");
          wm_backtest_snapshot_free(snap);
          return;
        }
      }
      else if(strcmp(tok, "--slip-bps") == 0)
      {
        if(wm_bt_parse_double_flag(val_tok, &params.slip_bps,
               &params.have_slip_bps) != SUCCESS)
        {
          cmd_reply(ctx, "bad --slip-bps value");
          wm_backtest_snapshot_free(snap);
          return;
        }
      }
      else if(strcmp(tok, "--size-frac") == 0)
      {
        if(wm_bt_parse_double_flag(val_tok, &params.size_frac,
               &params.have_size_frac) != SUCCESS)
        {
          cmd_reply(ctx, "bad --size-frac value");
          wm_backtest_snapshot_free(snap);
          return;
        }
      }
      else if(strcmp(tok, "--cash") == 0)
      {
        if(wm_bt_parse_double_flag(val_tok, &params.starting_cash,
               &params.have_starting_cash) != SUCCESS)
        {
          cmd_reply(ctx, "bad --cash value");
          wm_backtest_snapshot_free(snap);
          return;
        }
      }
      else if(strcmp(tok, "--threads") == 0)
      {
        char *end = NULL;
        long  w;

        errno = 0;
        w     = strtol(val_tok, &end, 10);

        if(end == val_tok || errno != 0 || w < 0)
        {
          cmd_reply(ctx, "bad --threads value");
          wm_backtest_snapshot_free(snap);
          return;
        }

        sweep_plan.workers = w == 0 ? 1u : (uint32_t)w;
      }
      else if(strcmp(tok, "--rank-by") == 0)
      {
        if(wm_bt_sweep_score_parse(val_tok,
               &sweep_plan.score) != SUCCESS)
        {
          cmd_reply(ctx,
              "bad --rank-by"
              " (expected realized|sharpe|sortino|equity|pf)");
          wm_backtest_snapshot_free(snap);
          return;
        }
      }
      else if(strcmp(tok, "--top-n") == 0)
      {
        char *end = NULL;
        long  k;

        errno = 0;
        k     = strtol(val_tok, &end, 10);

        if(end == val_tok || errno != 0 || k < 0)
        {
          cmd_reply(ctx, "bad --top-n value");
          wm_backtest_snapshot_free(snap);
          return;
        }

        sweep_plan.top_k = (uint32_t)k;
      }
      else if(strcmp(tok, "--walk-forward") == 0)
      {
        if(wm_bt_parse_walk_spec(val_tok, &walk_spec) != SUCCESS)
        {
          cmd_reply(ctx,
              "bad --walk-forward"
              " (expected train=Td:test=Md:step=Sd, days)");
          wm_backtest_snapshot_free(snap);
          return;
        }
        have_walk = true;
      }
      else if(strcmp(tok, "--oos-tail") == 0)
      {
        char *end = NULL;
        long  pct;

        errno = 0;
        pct   = strtol(val_tok, &end, 10);

        if(end == val_tok || errno != 0 || *end != '\0' ||
           pct < 1 || pct > 50)
        {
          cmd_reply(ctx, "bad --oos-tail (expected integer 1..50)");
          wm_backtest_snapshot_free(snap);
          return;
        }

        oos_spec.pct = (uint32_t)pct;
        have_oos     = true;
      }
      else
      {
        snprintf(reply, sizeof(reply),
            "unknown flag '%s' (expected --fee-bps/--slip-bps/"
            "--size-frac/--cash/--config/--threads/--rank-by/"
            "--top-n/--walk-forward/--oos-tail/--charts)",
            tok);
        cmd_reply(ctx, reply);
        wm_backtest_snapshot_free(snap);
        return;
      }
    }
    else if(strchr(tok, '=') != NULL)
    {
      err[0] = '\0';

      if(wm_bt_sweep_axis_add(&sweep_plan, ls, tok,
             err, sizeof(err)) != SUCCESS)
      {
        snprintf(reply, sizeof(reply),
            "error: %s", err[0] != '\0' ? err : "bad param spec");
        cmd_reply(ctx, reply);
        wm_backtest_snapshot_free(snap);
        return;
      }

      have_axes = true;
    }
    else
    {
      snprintf(reply, sizeof(reply),
          "unrecognised token '%s' (expected --flag or name=value)",
          tok);
      cmd_reply(ctx, reply);
      wm_backtest_snapshot_free(snap);
      return;
    }
  }

  if(have_walk && have_oos)
  {
    cmd_reply(ctx,
        "--walk-forward and --oos-tail are mutually exclusive");
    wm_backtest_snapshot_free(snap);
    return;
  }

  // Default top_k when sweeping. When --oos-tail is set together with
  // sweep axes, the OOS post-pass also wants a non-trivial top-K so
  // the validation iteration has work to do.
  if(have_axes && sweep_plan.top_k == 1)
    sweep_plan.top_k = WM_BT_DEFAULT_TOP_K;

  if(wm_bt_sweep_plan_finalize(&sweep_plan,
         err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "error: %s", err[0] != '\0' ? err : "sweep finalize failed");
    cmd_reply(ctx, reply);
    wm_backtest_snapshot_free(snap);
    return;
  }

  // Resolve walk / OOS windows off the .wm header range. The header
  // already carries the timestamps the walk + OOS builders need;
  // WM-BT-2 promoted range_start_ms / range_end_ms to first-class
  // snapshot fields so no string→ms reparse is required.
  sweep_mode.mode = WM_BT_MODE_FULL;

  if(have_walk)
  {
    err[0] = '\0';

    if(wm_bt_walk_build_windows(&walk_spec,
           snap->range_start_ms, snap->range_end_ms,
           ls, &sweep_mode.walk, err, sizeof(err)) != SUCCESS)
    {
      snprintf(reply, sizeof(reply),
          "walk-forward error: %s",
          err[0] != '\0' ? err : "(unknown)");
      cmd_reply(ctx, reply);
      wm_backtest_snapshot_free(snap);
      return;
    }

    sweep_mode.mode = WM_BT_MODE_WALK_FORWARD;
  }
  else if(have_oos)
  {
    err[0] = '\0';

    if(wm_bt_oos_split_range(&oos_spec,
           snap->range_start_ms, snap->range_end_ms,
           &sweep_mode.oos_head, &sweep_mode.oos_tail,
           err, sizeof(err)) != SUCCESS)
    {
      snprintf(reply, sizeof(reply),
          "oos-tail error: %s",
          err[0] != '\0' ? err : "(unknown)");
      cmd_reply(ctx, reply);
      wm_backtest_snapshot_free(snap);
      return;
    }

    sweep_mode.mode = WM_BT_MODE_OOS;
  }

  err[0] = '\0';

  if(wm_bt_report_path_resolve(report_root, sizeof(report_root),
         err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "report path: %s", err[0] != '\0' ? err : "(unknown)");
    cmd_reply(ctx, reply);
    wm_backtest_snapshot_free(snap);
    return;
  }

  wm_bt_sweep_id_generate(name_tok, snap->source_market_id,
      sweep_id, sizeof(sweep_id));

  err[0] = '\0';

  if(wm_bt_sweep_dir_create(report_root, sweep_id,
         sweep_dir, sizeof(sweep_dir), err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "sweep dir: %s", err[0] != '\0' ? err : "(unknown)");
    cmd_reply(ctx, reply);
    wm_backtest_snapshot_free(snap);
    return;
  }

  // Shared CSS/JS assets for every emitted HTML page (index + per-trade
  // charts + the WM-BT-RPT-5 sweep dashboard). Emit once, right after the
  // sweep dir exists; warn + continue on failure (an unstyled page is
  // still readable) — same discipline as the manifest pre-write below.
  err[0] = '\0';

  if(wm_bt_assets_emit(sweep_dir, err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "warn: assets emit failed: %s (pages render unstyled)",
        err[0] != '\0' ? err : "(unknown)");
    cmd_reply(ctx, reply);
  }

  sweep_results = mem_alloc("whenmoon.backtest", "sweep_results",
      sizeof(*sweep_results) * (size_t)sweep_plan.total_iters);

  if(sweep_results == NULL)
  {
    cmd_reply(ctx, "out of memory allocating sweep result table");
    wm_backtest_snapshot_free(snap);
    return;
  }

  // WM-BT-8: zero the table so the per-row fills pointer starts at
  // NULL; this lets the free-fills walker run safely on any exit path
  // even before workers have populated rows.
  memset(sweep_results, 0,
      sizeof(*sweep_results) * (size_t)sweep_plan.total_iters);

  err[0] = '\0';

  if(wm_bt_iter_open(&writer, sweep_dir, err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "iterations.jsonl: %s",
        err[0] != '\0' ? err : "(unknown)");
    cmd_reply(ctx, reply);
    wm_bt_results_free_fills(sweep_results, sweep_plan.total_iters);
    mem_free(sweep_results);
    wm_backtest_snapshot_free(snap);
    return;
  }

  writer_open = true;

  snprintf(reply, sizeof(reply),
      "mmap'd %u 1m bars from %s [%s..%s];"
      " mode=%s N=%u threads=%u rank_by=%s top_n=%u dir=%s",
      snap->bars_loaded_1m, snap->source_market_id,
      snap->range_start, snap->range_end,
      sweep_mode.mode == WM_BT_MODE_WALK_FORWARD ? "walk" :
      sweep_mode.mode == WM_BT_MODE_OOS          ? "oos"  : "full",
      sweep_plan.total_iters, sweep_plan.workers,
      wm_bt_sweep_score_name(sweep_plan.score), sweep_plan.top_k,
      sweep_id);
  cmd_reply(ctx, reply);

  // Pre-run manifest write — fixed metadata before workers start so a
  // crash mid-sweep still leaves an audit trail. Post-run rewrite
  // adds wallclock_ms + ok_count + fail_count.
  err[0] = '\0';

  if(wm_bt_manifest_write(sweep_dir, sweep_id, path_tok, snap,
         name_tok, &sweep_plan, &sweep_mode, &params,
         0, 0, 0, err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "warn: manifest pre-write failed: %s",
        err[0] != '\0' ? err : "(unknown)");
    cmd_reply(ctx, reply);
  }

  clock_gettime(CLOCK_MONOTONIC, &t0);

  err[0] = '\0';

  // market_id_db is unused since WM-BT-1 ripped DB persistence — pass
  // 0 verbatim until WM-BT-7+ drops it from the wm_bt_sweep_run ABI.
  if(wm_bt_sweep_run(st, snap, name_tok, /*market_id_db=*/0,
         &sweep_plan, &sweep_mode, &params, sweep_results,
         err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "sweep run failed: %s",
        err[0] != '\0' ? err : "unknown");
    cmd_reply(ctx, reply);
    wm_bt_iter_close(&writer);
    wm_bt_results_free_fills(sweep_results, sweep_plan.total_iters);
    mem_free(sweep_results);
    wm_backtest_snapshot_free(snap);
    return;
  }

  clock_gettime(CLOCK_MONOTONIC, &t1);

  wallclock_ms = (uint64_t)((int64_t)(t1.tv_sec - t0.tv_sec) * 1000
               + (int64_t)(t1.tv_nsec - t0.tv_nsec) / 1000000);

  for(i = 0; i < sweep_plan.total_iters; i++)
  {
    if(sweep_results[i].ok)
      n_ok++;
    else
      n_fail++;
  }

  if(sweep_mode.mode == WM_BT_MODE_OOS && n_ok > 0)
  {
    err[0] = '\0';

    if(wm_bt_sweep_run_oos_validation(st, snap, name_tok, /*market_id_db=*/0,
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

  // Main-thread JSONL flush — single-writer-by-construction. Workers
  // never touched the writer; sweep_results is now stable.
  for(i = 0; i < sweep_plan.total_iters; i++)
    (void)wm_bt_iter_append(&writer, &sweep_plan, i, &sweep_results[i]);

  wm_bt_iter_close(&writer);
  writer_open = false;

  // Post-run manifest rewrite with final stats. Atomic via tmp+rename
  // so a partial pre-write is never visible.
  err[0] = '\0';

  if(wm_bt_manifest_write(sweep_dir, sweep_id, path_tok, snap,
         name_tok, &sweep_plan, &sweep_mode, &params,
         wallclock_ms, n_ok, n_fail, err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "warn: manifest post-write failed: %s",
        err[0] != '\0' ? err : "(unknown)");
    cmd_reply(ctx, reply);
  }

  err[0] = '\0';

  if(wm_bt_render_topn_txt(sweep_dir, &sweep_plan, &sweep_mode,
         sweep_results, sweep_plan.total_iters, n_ok,
         err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "warn: top-N.txt write failed: %s",
        err[0] != '\0' ? err : "(unknown)");
    cmd_reply(ctx, reply);
  }

  err[0] = '\0';

  if(wm_bt_render_report_md(sweep_dir, sweep_id, path_tok, snap,
         name_tok, &sweep_plan, &sweep_mode,
         sweep_results, sweep_plan.total_iters,
         n_ok, n_fail, wallclock_ms,
         err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "warn: report.md write failed: %s",
        err[0] != '\0' ? err : "(unknown)");
    cmd_reply(ctx, reply);
  }

  // Chart + index.html emission. --charts forces on; otherwise the KV
  // `plugin.whenmoon.backtest.charts_enabled` BOOL gate decides. Charts
  // are a per-trade analysis artifact for ONE config — every grain the
  // snapshot carries is plotted per matched trade, so the file count is
  // (round-trip trades * grains) for a single run. A parameter sweep
  // would multiply that by the charted top-K (default 20), producing
  // tens of thousands of files for a tuning run that wants the ranked
  // metrics table, not per-trade charts. So charts/index are emitted
  // ONLY for a single-config run (total_iters == 1); a sweep gets the
  // metrics artifacts (report.md / iterations.jsonl / top-N) and a note
  // to re-run the chosen config on its own to chart it. Skipped when no
  // iterations succeeded (top-K would be empty).
  if(n_ok > 0)
  {
    bool emit_charts = charts_force
        ? true
        : (kv_get_int("plugin.whenmoon.backtest.charts_enabled") != 0);

    if(emit_charts && sweep_plan.total_iters > 1)
    {
      cmd_reply(ctx,
          "charts: skipped — this is a parameter sweep; charts + index"
          " are emitted only for single-config runs (a sweep would emit"
          " trades x grains x top-K files). Re-run the chosen config with"
          " no sweep axes to chart it.");
    }
    else if(emit_charts)
    {
      wm_bt_cmd_run_emit_charts(ctx, sweep_results,
          sweep_plan.total_iters, &sweep_plan, snap, sweep_dir);

      // index.html ties the emitted charts together: a per-config trade
      // table where each row links to its chart. Written after the
      // chart pass so every href points at an existing file.
      err[0] = '\0';

      if(wm_bt_render_index_html(sweep_dir, sweep_id, path_tok, snap,
             name_tok, &sweep_plan, &sweep_mode, &params,
             sweep_results, sweep_plan.total_iters,
             n_ok, n_fail, wallclock_ms,
             err, sizeof(err)) != SUCCESS)
      {
        snprintf(reply, sizeof(reply),
            "warn: index.html write failed: %s",
            err[0] != '\0' ? err : "(unknown)");
        cmd_reply(ctx, reply);
      }
      else
      {
        snprintf(reply, sizeof(reply),
            "index: open %s/index.html", sweep_dir);
        cmd_reply(ctx, reply);
      }
    }
  }

  snprintf(reply, sizeof(reply),
      "complete: %u/%u ok, %u failed in %" PRIu64 " ms"
      " (%.1f iter/s); jsonl=%u rows -> %s/",
      n_ok, sweep_plan.total_iters, n_fail, wallclock_ms,
      wallclock_ms > 0
          ? (double)sweep_plan.total_iters * 1000.0
              / (double)wallclock_ms
          : 0.0,
      sweep_plan.total_iters, sweep_dir);
  cmd_reply(ctx, reply);

  wm_bt_sweep_render_topk(ctx, &sweep_plan, &sweep_mode, sweep_results,
      sweep_plan.total_iters);

  if(writer_open)
    wm_bt_iter_close(&writer);

  wm_bt_results_free_fills(sweep_results, sweep_plan.total_iters);
  mem_free(sweep_results);
  wm_backtest_snapshot_free(snap);
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
// /whenmoon backtest compile  (WM-BT-3)                                   //
// ----------------------------------------------------------------------- //

// `days = 0` sentinel = "all available 1m history".
#define WM_BT_COMPILE_DEFAULT_DAYS  0u

// Compile is heavy (millions of rows + indicator pyramid + file
// write); it runs on a worker task at the lowest possible priority
// (task.h: 0 = highest, 254 = lowest) so it never delays interactive
// command dispatch.
#define WM_BT_COMPILE_TASK_PRIORITY 254u

// clam context for the async compile task (WM_BT_CTX itself is private
// to backtest.c; mirror its value here).
#define WM_BT_CMD_CTX  "whenmoon.backtest"

// Render epoch ms back to the canonical "YYYY-MM-DD HH:MM:SS+00" form
// the snapshot builder + .wm header carry. Mirrors the formatter at
// dl_candles.c:74 but inline because that one is static.
static void
wm_bt_compile_ms_to_pg(int64_t ms, char *out, size_t cap)
{
  struct tm  tm;
  time_t     t = (time_t)(ms / 1000);
  int        year;

  if(out == NULL || cap == 0)
    return;

  if(t < 0) t = 0;

  if(gmtime_r(&t, &tm) == NULL)
  {
    snprintf(out, cap, "1970-01-01 00:00:00+00");
    return;
  }

  year = tm.tm_year + 1900;

  if(year < 0)    year = 0;
  if(year > 9999) year = 9999;

  snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d+00",
      year, tm.tm_mon + 1, tm.tm_mday,
      tm.tm_hour, tm.tm_min, tm.tm_sec);
}

// Async payload for a backtest-compile task. The command handler
// validates cheaply, copies the request here, and hands it to a
// lowest-priority worker so the (multi-second, multi-million-row)
// snapshot build + .wm write never blocks the issuing IRC/botmanctl
// session. Owned by the task; freed in wm_bt_compile_task_cb.
typedef struct
{
  int32_t  market_id;
  uint32_t days;
  char     market[64];     // canonical <exch>-<base>-<quote> as given
  char     path[256];      // output .wm path
} wm_bt_compile_task_t;

// Worker body: derive the range, pre-flight, build the snapshot, write
// the .wm. All slow work (the DB row scan, indicator pyramid, file
// write) lives here so the dispatcher returns immediately. There is no
// live session to reply to once the handler has returned, so outcomes
// — success and every failure — are reported through the log; progress
// is observable via /show tasks.
static void
wm_bt_compile_task_cb(task_t *t)
{
  wm_bt_compile_task_t   *job = t->data;
  char                    start_ts[40] = {0};
  char                    end_ts[40]   = {0};
  char                    err[320];
  int64_t                 latest_ms = 0;
  int64_t                 start_ms  = 0;
  int64_t                 end_ms    = 0;
  wm_backtest_snapshot_t *snap = NULL;

  if(wm_bt_latest_1m_bar_ms(job->market_id, &latest_ms) != SUCCESS)
  {
    clam(CLAM_WARN, WM_BT_CMD_CTX,
        "compile %s: no 1m candles persisted;"
        " run /whenmoon download %s ...",
        job->market, job->market);
    goto done;
  }

  end_ms = latest_ms;

  if(job->days == 0)
  {
    if(wm_bt_earliest_1m_bar_ms(job->market_id, &start_ms) != SUCCESS)
    {
      clam(CLAM_WARN, WM_BT_CMD_CTX,
          "compile %s: earliest-bar probe failed", job->market);
      goto done;
    }

    // earliest bar's close ms is one minute after open; the range
    // is half-open [start, end). Subtract 60s so the earliest bar
    // is included on the inclusive side of the snapshot range.
    start_ms -= 60000;
  }
  else
  {
    start_ms = latest_ms - ((int64_t)job->days * 86400LL * 1000LL);
  }

  if(start_ms >= end_ms)
  {
    clam(CLAM_WARN, WM_BT_CMD_CTX,
        "compile %s: range degenerate (start >= end)", job->market);
    goto done;
  }

  wm_bt_compile_ms_to_pg(start_ms, start_ts, sizeof(start_ts));
  wm_bt_compile_ms_to_pg(end_ms,   end_ts,   sizeof(end_ts));

  err[0] = '\0';

  if(wm_backtest_preflight_gap(job->market_id, job->market,
         start_ts, end_ts, err, sizeof(err)) != SUCCESS)
  {
    clam(CLAM_WARN, WM_BT_CMD_CTX, "compile %s gap: %s",
        job->market, err[0] != '\0' ? err : "(no detail)");
    goto done;
  }

  err[0] = '\0';

  snap = wm_backtest_snapshot_build(job->market_id, job->market,
      start_ts, end_ts, WM_AGG_DEFAULT_HISTORY_1D,
      err, sizeof(err));

  if(snap == NULL)
  {
    clam(CLAM_WARN, WM_BT_CMD_CTX, "compile %s: snapshot build failed: %s",
        job->market, err[0] != '\0' ? err : "(no detail)");
    goto done;
  }

  // Populate range_*_ms before writing so the .wm header carries the
  // epoch range that downstream walk-forward / OOS plumbing in WM-BT-9
  // expects to read back from the file. Heap-built snapshots zero
  // these by default — WM-BT-2 wired the field, WM-BT-3 fills it on
  // the compile path.
  snap->range_start_ms = start_ms;
  snap->range_end_ms   = end_ms;

  err[0] = '\0';

  if(wm_bt_file_write(job->path, snap, err, sizeof(err)) != SUCCESS)
  {
    clam(CLAM_WARN, WM_BT_CMD_CTX, "compile %s: write failed: %s",
        job->market, err[0] != '\0' ? err : "(no detail)");
    goto done;
  }

  clam(CLAM_INFO, WM_BT_CMD_CTX,
      "compiled %s: 1m_bars=%u (5m=%u 15m=%u 1h=%u 4h=%u 1d=%u)"
      " range=[%s..%s] -> %s",
      job->market, snap->bars_loaded_1m,
      snap->mkt.grain_n[WM_GRAN_5M], snap->mkt.grain_n[WM_GRAN_15M],
      snap->mkt.grain_n[WM_GRAN_1H], snap->mkt.grain_n[WM_GRAN_4H],
      snap->mkt.grain_n[WM_GRAN_1D],
      start_ts, end_ts, job->path);

done:
  if(snap != NULL)
    wm_backtest_snapshot_free(snap);

  mem_free(job);
  t->state = TASK_ENDED;
}

static void
wm_bt_cmd_compile(const cmd_ctx_t *ctx)
{
  whenmoon_state_t     *st;
  const char           *p;
  char                  pair_tok[64]   = {0};
  char                  path_tok[256]  = {0};
  char                  days_tok[32]   = {0};
  char                  exch[32]       = {0};
  char                  base[16]       = {0};
  char                  quote[16]      = {0};
  char                  symbol[32]     = {0};
  char                  reply[512];
  char                  task_name[TASK_NAME_SZ];
  uint32_t              days = WM_BT_COMPILE_DEFAULT_DAYS;
  int32_t               market_id;
  wm_bt_compile_task_t *job;
  task_t               *t;
  size_t                i;

  st = whenmoon_get_state();

  if(st == NULL || !st->dl_ready)
  {
    cmd_reply(ctx, "whenmoon: downloader not ready");
    return;
  }

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, pair_tok, sizeof(pair_tok)) ||
     !wm_dl_next_token(&p, path_tok, sizeof(path_tok)))
  {
    cmd_reply(ctx,
        "usage: /whenmoon backtest compile <market> <path.wm> [<days>]");
    return;
  }

  // Optional days arg. Absence or "0" means "full available history".
  if(wm_dl_next_token(&p, days_tok, sizeof(days_tok)))
  {
    char  *end_p = NULL;
    long   v;

    errno = 0;
    v = strtol(days_tok, &end_p, 10);

    if(end_p == days_tok || errno != 0 || v < 0)
    {
      cmd_reply(ctx, "bad <days> (expected non-negative integer)");
      return;
    }

    days = (uint32_t)v;
  }

  if(wm_market_parse_id(pair_tok, exch, sizeof(exch),
         base, sizeof(base), quote, sizeof(quote)) != SUCCESS)
  {
    cmd_reply(ctx, "bad market id (expected <exch>-<base>-<quote>)");
    return;
  }

  // Wire-form symbol: uppercase BASE-QUOTE (mirrors run path's
  // canonicalisation).
  {
    int n = snprintf(symbol, sizeof(symbol), "%s-%s", base, quote);

    if(n < 0 || (size_t)n >= sizeof(symbol))
    {
      cmd_reply(ctx, "market id overflow");
      return;
    }

    for(i = 0; symbol[i] != '\0'; i++)
      symbol[i] = (char)toupper((unsigned char)symbol[i]);
  }

  market_id = wm_market_lookup_or_create(exch, base, quote, symbol);

  if(market_id < 0)
  {
    cmd_reply(ctx, "market registry lookup failed");
    return;
  }

  // Hand the heavy lifting — earliest/latest probe, gap pre-flight, the
  // multi-million-row snapshot build, and the .wm write — to a
  // lowest-priority worker task so the issuing session returns now.
  job = mem_alloc("whenmoon.backtest", "compile_task", sizeof(*job));

  if(job == NULL)
  {
    cmd_reply(ctx, "out of memory");
    return;
  }

  memset(job, 0, sizeof(*job));
  job->market_id = market_id;
  job->days      = days;
  snprintf(job->market, sizeof(job->market), "%s", pair_tok);
  snprintf(job->path,   sizeof(job->path),   "%s", path_tok);

  snprintf(task_name, sizeof(task_name), "wm-btcompile:%s", pair_tok);

  t = task_add(task_name, TASK_THREAD, WM_BT_COMPILE_TASK_PRIORITY,
      wm_bt_compile_task_cb, job);

  if(t == NULL)
  {
    mem_free(job);
    cmd_reply(ctx, "failed to submit compile task");
    return;
  }

  snprintf(reply, sizeof(reply),
      "task created: '%s' (pri %u) — compiling %s%s -> %s;"
      " watch /show tasks, result lands in the log",
      task_name, WM_BT_COMPILE_TASK_PRIORITY, pair_tok,
      days == 0 ? " (full history)" : "", path_tok);
  cmd_reply(ctx, reply);
}

// ----------------------------------------------------------------------- //
// /whenmoon backtest inspect  (WM-BT-3)                                   //
// ----------------------------------------------------------------------- //

static void
wm_bt_cmd_inspect(const cmd_ctx_t *ctx)
{
  const char *p;
  char        path_tok[256] = {0};
  char        summary[1024];
  char        err[320];
  char        line[256];
  const char *cursor;
  const char *nl;

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, path_tok, sizeof(path_tok)))
  {
    cmd_reply(ctx, "usage: /whenmoon backtest inspect <path.wm>");
    return;
  }

  err[0] = '\0';

  if(wm_bt_file_inspect(path_tok, summary, sizeof(summary),
         err, sizeof(err)) != SUCCESS)
  {
    char reply[400];

    snprintf(reply, sizeof(reply), "inspect failed: %s",
        err[0] != '\0' ? err : "(no detail)");
    cmd_reply(ctx, reply);
    return;
  }

  // wm_bt_file_inspect renders a multi-line block; split on '\n' and
  // emit one cmd_reply per line. The summary always fits in 1024 B
  // (12 header lines + 6 grain rows, each ~70 chars).
  cursor = summary;

  while(cursor != NULL && *cursor != '\0')
  {
    size_t len;

    nl = strchr(cursor, '\n');

    if(nl == NULL)
      len = strlen(cursor);
    else
      len = (size_t)(nl - cursor);

    if(len >= sizeof(line))
      len = sizeof(line) - 1u;

    memcpy(line, cursor, len);
    line[len] = '\0';

    cmd_reply(ctx, line);

    if(nl == NULL)
      break;

    cursor = nl + 1;
  }

  // Stale-schema NOTE is in err on a SUCCESS return; surface it.
  if(err[0] != '\0')
    cmd_reply(ctx, err);
}

// ----------------------------------------------------------------------- //
// /whenmoon backtest list  (WM-BT-7)                                      //
// ----------------------------------------------------------------------- //
//
// Walk the on-disk sweep tree (newest-first) and emit one line per
// sweep. Each line carries the sweep_id, strategy, total_iters,
// rank_by, and ok/fail counts pulled from manifest.json. Sweeps with
// no manifest (interrupted writes or pre-WM-BT-6) are still listed
// with "(no manifest)" so the operator can see them.

static void
wm_bt_cmd_list(const cmd_ctx_t *ctx)
{
  char                  report_root[1024] = {0};
  char                  err[320]          = {0};
  char                  reply[640];
  char                  manifest_path[2048];
  wm_bt_dir_listing_t   listing;
  uint32_t              i;

  if(wm_bt_report_path_resolve(report_root, sizeof(report_root),
         err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "report path: %s", err[0] != '\0' ? err : "(unknown)");
    cmd_reply(ctx, reply);
    return;
  }

  memset(&listing, 0, sizeof(listing));

  if(wm_bt_dir_listdir(report_root, &listing,
         err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "list: %s", err[0] != '\0' ? err : "(unknown)");
    cmd_reply(ctx, reply);
    return;
  }

  snprintf(reply, sizeof(reply),
      "%u sweep%s under %s",
      listing.n, listing.n == 1 ? "" : "s", report_root);
  cmd_reply(ctx, reply);

  if(listing.n == 0)
  {
    wm_bt_dir_listing_free(&listing);
    return;
  }

  for(i = 0; i < listing.n; i++)
  {
    struct json_object *manifest = NULL;
    struct json_object *jv;
    const char         *strategy   = "?";
    const char         *rank_by    = "?";
    int64_t             total_iters = -1;
    int64_t             ok_count    = -1;
    int64_t             fail_count  = -1;
    int                 n_path;

    n_path = snprintf(manifest_path, sizeof(manifest_path),
        "%s/%s/manifest.json", report_root, listing.names[i]);

    if(n_path > 0 && (size_t)n_path < sizeof(manifest_path))
      manifest = json_object_from_file(manifest_path);

    if(manifest != NULL)
    {
      if(json_object_object_get_ex(manifest, "strategy", &jv))
        strategy = json_object_get_string(jv);

      if(json_object_object_get_ex(manifest, "rank_by", &jv))
        rank_by = json_object_get_string(jv);

      if(json_object_object_get_ex(manifest, "total_iters", &jv))
        total_iters = json_object_get_int64(jv);

      if(json_object_object_get_ex(manifest, "ok_count", &jv))
        ok_count = json_object_get_int64(jv);

      if(json_object_object_get_ex(manifest, "fail_count", &jv))
        fail_count = json_object_get_int64(jv);
    }

    if(manifest == NULL || total_iters < 0)
      snprintf(reply, sizeof(reply),
          "  %s  (no manifest)", listing.names[i]);
    else
      snprintf(reply, sizeof(reply),
          "  %s  %s rank=%s n=%" PRId64
          " ok=%" PRId64 " fail=%" PRId64,
          listing.names[i], strategy, rank_by,
          total_iters, ok_count, fail_count);

    cmd_reply(ctx, reply);

    if(manifest != NULL)
      json_object_put(manifest);
  }

  wm_bt_dir_listing_free(&listing);
}

// ----------------------------------------------------------------------- //
// /whenmoon backtest show  (WM-BT-7)                                      //
// ----------------------------------------------------------------------- //
//
// Cat `<report_root>/<sweep_id>/report.md` line-by-line into cmd_reply.
// The renderer materialises everything to disk at sweep end so this
// verb stays minimal: no JSON parsing, no recomputation. Sweeps from
// before WM-BT-7 land here without a report.md — surface a terse
// note instead of synthesising one.

static void
wm_bt_cmd_show(const cmd_ctx_t *ctx)
{
  const char *p;
  char        sweep_tok[160]     = {0};
  char        report_root[1024]  = {0};
  char        err[320]           = {0};
  char        path[2048];
  char        reply[640];
  char        line[640];
  FILE       *fp;

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, sweep_tok, sizeof(sweep_tok)))
  {
    cmd_reply(ctx, "usage: /whenmoon backtest show <sweep_id>");
    return;
  }

  if(wm_bt_report_path_resolve(report_root, sizeof(report_root),
         err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "report path: %s", err[0] != '\0' ? err : "(unknown)");
    cmd_reply(ctx, reply);
    return;
  }

  // Defensive: reject any token containing '/' or starting with '.'
  // so the verb cannot read outside the report root.
  if(sweep_tok[0] == '.' || strchr(sweep_tok, '/') != NULL)
  {
    cmd_reply(ctx, "bad sweep id");
    return;
  }

  if(snprintf(path, sizeof(path),
         "%s/%s/report.md", report_root, sweep_tok) >= (int)sizeof(path))
  {
    cmd_reply(ctx, "show: path overflow");
    return;
  }

  fp = fopen(path, "r");

  if(fp == NULL)
  {
    snprintf(reply, sizeof(reply),
        "no report.md for %s under %s (legacy sweep, or never written)",
        sweep_tok, report_root);
    cmd_reply(ctx, reply);
    return;
  }

  while(fgets(line, sizeof(line), fp) != NULL)
  {
    size_t len = strlen(line);

    if(len > 0 && line[len - 1] == '\n')
      line[len - 1] = '\0';

    cmd_reply(ctx, line);
  }

  fclose(fp);
}

// ----------------------------------------------------------------------- //
// /whenmoon backtest parent                                               //
// ----------------------------------------------------------------------- //

static void
wm_bt_parent_cb(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx,
      "usage: /whenmoon backtest"
      " <run|compile|inspect|list|show|reload> ...");
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
        "Subcommands: run <path.wm> <strat> [name=value ...],"
        " compile <market_id> <path.wm> [<days>],"
        " inspect <path.wm>,"
        " list,"
        " show <sweep_id>,"
        " reload <strat>.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_bt_parent_cb, NULL, "whenmoon", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "run",
        "whenmoon backtest run <path.wm> <strategy>"
        " [<name>=<v|[v,...]|lo:step:hi>] (repeatable)"
        " [--fee-bps N] [--slip-bps N] [--size-frac F] [--cash N]"
        " [--config <path.json>] [--threads N]"
        " [--rank-by realized|sharpe|sortino|equity|pf]"
        " [--top-n K]"
        " [--walk-forward train=Td:test=Md:step=Sd]"
        " [--oos-tail PCT]"
        " [--charts]",
        "Run a backtest against a compiled .wm snapshot — single"
        " iteration, parameter sweep, walk-forward, or OOS-tail"
        " validation.",
        "mmap's the .wm file (compiled via /whenmoon backtest compile)"
        " and walks the strategy through a paper trade book. Range +"
        " corpus come from the .wm header; the verb no longer takes"
        " dates. Positional `name=value` tokens are sweep axes routed"
        " through the same parser as previous `--sweep` flags;"
        " accepted value forms are bare list `v1,v2,v3`, bracketed"
        " list `[v1,v2,v3]`, or range `lo:step:hi`. Zero axes = one"
        " iteration; N axes = cartesian-product sweep.\n"
        "--threads defaults to max(1, nproc - 2) so the host keeps"
        " two cores free, further capped by the KV"
        " plugin.whenmoon.backtest.max_threads (0 = no cap) and"
        " clamped to [1, 64]. Workers run at nice 19 (lowest"
        " priority) so a long sweep never starves IRC, marketwatch,"
        " or the live engine. Each iteration runs on a private"
        " trade-book registry so parallel workers do not contend on"
        " a global mutex.\n"
        "--config <path.json> loads a sweep matrix from a JSON file"
        " shaped {\"params\": {\"name\": <scalar|list|{start,step,end}>,"
        " ...}}. Inline `name=value` axes override matching entries"
        " loaded from --config, regardless of argv order.\n"
        "--rank-by selects the ranking metric (default realized).\n"
        "--top-n caps the number of top rows shown after the run"
        " (default 20 when sweeping, 1 otherwise) and the number of"
        " rows the OOS post-pass validates.\n"
        "--walk-forward expands each param vector into N test windows"
        " (train days warm the strategy state but only test windows"
        " accumulate fills); the recorded score is the cumulative"
        " test-window result.\n"
        "--oos-tail PCT reserves the last PCT%% of the range as out-of"
        "-sample; the sweep optimises on the head, then the post-pass"
        " runs the top-N on the tail and stamps the OOS columns on"
        " each row. PCT clamped to [1, 50].\n"
        "--walk-forward and --oos-tail are mutually exclusive.\n"
        "Each invocation writes a sweep directory under"
        " plugin.whenmoon.backtest.report_path (defaulting to"
        " $HOME/.local/share/botmanager/backtests/) containing"
        " manifest.json, iterations.jsonl, top-N.txt, report.md, and"
        " a charts/ subdir.\n"
        "--charts forces Lightweight Charts HTML emission for this"
        " run (default-off unless"
        " plugin.whenmoon.backtest.charts_enabled=true). SINGLE-CONFIG"
        " RUNS ONLY: charts are a per-trade analysis artifact, so a"
        " parameter sweep skips them (it would emit trades x grains x"
        " top-K files) and emits only the ranked metrics — re-run the"
        " chosen config with no sweep axes to chart it. One file per"
        " matched buy→sell trade pair, for EVERY grain the snapshot"
        " carries (1m..1d), written to charts/trade-M-<gran>.html, so"
        " the count is round-trip-trades x grains. An index.html landing"
        " page is also written at the sweep root: summary cards, swept"
        " args + metrics, and a per-trade P/L table whose rows link to"
        " each trade's per-grain charts — open it first.",
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

  if(cmd_register("whenmoon", "compile",
        "whenmoon backtest compile <market_id> <path.wm> [<days>]",
        "Compile a .wm snapshot file from persisted 1m candles (async).",
        "Validates the request, then returns immediately with a"
        " 'task created' acknowledgement: the build runs on a worker"
        " task at the lowest priority (254) so it never delays"
        " interactive commands. Track it with /show tasks; the result"
        " (or any failure) is reported in the log on completion.\n"
        "The task builds an isolated wm_backtest_snapshot_t from the"
        " wm_candles_<id> table over the most recent <days> of 1m"
        " history (default 0 = all available history), then serialises"
        " the snapshot to <path.wm> via mmap-friendly host-endian"
        " binary form (host-portable across daemon restarts; WM-BT-2"
        " format magic 0x4D4E4257, version 1).\n"
        "The pre-flight tolerates gaps of any size (illiquid early"
        " history is legitimately sparse) and only refuses an entirely"
        " empty range. Output is atomic via tmp+fsync+rename. Re-runs"
        " overwrite an existing file at <path>.\n"
        "Compiled .wm files survive daemon restarts and are the input"
        " to /whenmoon backtest run in WM-BT-6.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_bt_cmd_compile, NULL, "whenmoon/backtest", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "inspect",
        "whenmoon backtest inspect <path.wm>",
        "Render the .wm file's header without loading any candles.",
        "Reads just the wm_bt_file_header_t at offset 0 and prints"
        " each field on its own line: magic, file_version, indicator"
        " schema version, bar_size, source_market_id, range, per-grain"
        " bar counts + offsets. Stale-schema files inspect cleanly"
        " (emits a NOTE) but cannot be loaded — recompile with the"
        " current daemon to refresh the indicator schema.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_bt_cmd_inspect, NULL, "whenmoon/backtest", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "list",
        "whenmoon backtest list",
        "List on-disk sweeps, newest-first.",
        "Walks plugin.whenmoon.backtest.report_path (default"
        " $HOME/.local/share/botmanager/backtests/) and prints one"
        " line per sweep directory whose name matches the canonical"
        " YYYYMMDD-HHMMSS-<strategy>-<short_market> prefix. Each line"
        " shows the sweep id, strategy, total iterations, ranking"
        " metric, and ok/fail counts pulled from manifest.json."
        " Sweeps with no manifest (interrupted writes) still appear,"
        " tagged '(no manifest)'.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_bt_cmd_list, NULL, "whenmoon/backtest", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "show",
        "whenmoon backtest show <sweep_id>",
        "Cat <sweep_dir>/report.md line-by-line.",
        "Looks up <sweep_id> under plugin.whenmoon.backtest.report_path"
        " and streams its report.md back via cmd_reply. The renderer"
        " materialises everything to disk at sweep end, so this verb"
        " stays minimal — no JSON parsing, no recomputation. Sweeps"
        " from before WM-BT-7 land here without a report.md and"
        " surface a one-line note instead of a synthesised summary.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_bt_cmd_show, NULL, "whenmoon/backtest", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
