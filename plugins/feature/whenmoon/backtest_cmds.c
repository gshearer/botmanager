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
#include "wm_bt_file.h"
#include "wm_bt_report.h"

#include "cmd.h"
#include "common.h"
#include "userns.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
        " [--oos-tail PCT]");
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
            "--top-n/--walk-forward/--oos-tail)",
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

  sweep_results = mem_alloc("whenmoon.backtest", "sweep_results",
      sizeof(*sweep_results) * (size_t)sweep_plan.total_iters);

  if(sweep_results == NULL)
  {
    cmd_reply(ctx, "out of memory allocating sweep result table");
    wm_backtest_snapshot_free(snap);
    return;
  }

  err[0] = '\0';

  if(wm_bt_iter_open(&writer, sweep_dir, err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "iterations.jsonl: %s",
        err[0] != '\0' ? err : "(unknown)");
    cmd_reply(ctx, reply);
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

  (void)wm_bt_top_n_placeholder_write(sweep_dir, &sweep_plan);

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

static void
wm_bt_cmd_compile(const cmd_ctx_t *ctx)
{
  whenmoon_state_t       *st;
  const char             *p;
  char                    pair_tok[64]   = {0};
  char                    path_tok[256]  = {0};
  char                    days_tok[32]   = {0};
  char                    exch[32]       = {0};
  char                    base[16]       = {0};
  char                    quote[16]      = {0};
  char                    symbol[32]     = {0};
  char                    start_ts[40]   = {0};
  char                    end_ts[40]     = {0};
  char                    err[320];
  char                    reply[512];
  uint32_t                days = WM_BT_COMPILE_DEFAULT_DAYS;
  int32_t                 market_id;
  int64_t                 latest_ms = 0;
  int64_t                 start_ms = 0;
  int64_t                 end_ms = 0;
  wm_backtest_snapshot_t *snap = NULL;
  size_t                  i;

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

  if(wm_bt_latest_1m_bar_ms(market_id, &latest_ms) != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "no 1m candles persisted for %s; run /whenmoon download %s ...",
        pair_tok, pair_tok);
    cmd_reply(ctx, reply);
    return;
  }

  end_ms = latest_ms;

  if(days == 0)
  {
    if(wm_bt_earliest_1m_bar_ms(market_id, &start_ms) != SUCCESS)
    {
      cmd_reply(ctx, "earliest-bar probe failed");
      return;
    }

    // earliest bar's close ms is one minute after open; the range
    // is half-open [start, end). Subtract 60s so the earliest bar
    // is included on the inclusive side of the snapshot range.
    start_ms -= 60000;
  }
  else
  {
    start_ms = latest_ms - ((int64_t)days * 86400LL * 1000LL);
  }

  if(start_ms >= end_ms)
  {
    cmd_reply(ctx, "compile range degenerate (start >= end)");
    return;
  }

  wm_bt_compile_ms_to_pg(start_ms, start_ts, sizeof(start_ts));
  wm_bt_compile_ms_to_pg(end_ms,   end_ts,   sizeof(end_ts));

  err[0] = '\0';

  if(wm_backtest_preflight_gap(market_id, start_ts, end_ts,
         err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply), "gap: %s",
        err[0] != '\0' ? err : "(no detail)");
    cmd_reply(ctx, reply);
    return;
  }

  snap = wm_backtest_snapshot_build(market_id, pair_tok,
      start_ts, end_ts, WM_AGG_DEFAULT_HISTORY_1D,
      err, sizeof(err));

  if(snap == NULL)
  {
    snprintf(reply, sizeof(reply), "snapshot build failed: %s",
        err[0] != '\0' ? err : "(no detail)");
    cmd_reply(ctx, reply);
    return;
  }

  // Populate range_*_ms before writing so the .wm header carries the
  // epoch range that downstream walk-forward / OOS plumbing in WM-BT-9
  // expects to read back from the file. Heap-built snapshots zero
  // these by default — WM-BT-2 wired the field, WM-BT-3 fills it on
  // the compile path.
  snap->range_start_ms = start_ms;
  snap->range_end_ms   = end_ms;

  err[0] = '\0';

  if(wm_bt_file_write(path_tok, snap, err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply), "write failed: %s",
        err[0] != '\0' ? err : "(no detail)");
    cmd_reply(ctx, reply);
    wm_backtest_snapshot_free(snap);
    return;
  }

  snprintf(reply, sizeof(reply),
      "compiled %s: 1m_bars=%u (5m=%u 15m=%u 1h=%u 4h=%u 1d=%u)"
      " range=[%s..%s]",
      path_tok, snap->bars_loaded_1m,
      snap->mkt.grain_n[WM_GRAN_5M], snap->mkt.grain_n[WM_GRAN_15M],
      snap->mkt.grain_n[WM_GRAN_1H], snap->mkt.grain_n[WM_GRAN_4H],
      snap->mkt.grain_n[WM_GRAN_1D],
      start_ts, end_ts);
  cmd_reply(ctx, reply);

  wm_backtest_snapshot_free(snap);
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
// /whenmoon backtest parent                                               //
// ----------------------------------------------------------------------- //

static void
wm_bt_parent_cb(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /whenmoon backtest <run|compile|inspect|reload> ...");
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
        " compile <market_id> <path.wm> [<days>],"
        " inspect <path.wm>,"
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
        " [--oos-tail PCT]",
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
        " manifest.json, iterations.jsonl, top-N.txt (BT-7"
        " placeholder), and an empty charts/ subdir (filled by"
        " BT-8 when plugin.whenmoon.backtest.charts_enabled=true).",
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
        "Compile a .wm snapshot file from persisted 1m candles.",
        "Builds an isolated wm_backtest_snapshot_t from the"
        " wm_candles_<id> table over the most recent <days> of 1m"
        " history (default 0 = all available history), then serialises"
        " the snapshot to <path.wm> via mmap-friendly host-endian"
        " binary form (host-portable across daemon restarts; WM-BT-2"
        " format magic 0x4D4E4257, version 1).\n"
        "The pre-flight gap check fails fast with the canonical"
        " /whenmoon download <market> invocation when 1m coverage has"
        " gaps. Output is atomic via tmp+fsync+rename. Re-runs"
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

  return(SUCCESS);
}
