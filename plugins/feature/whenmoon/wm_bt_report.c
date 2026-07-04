// botmanager — MIT
// Whenmoon backtest on-disk report emitter (WM-BT-6).
//
// Each `/whenmoon backtest run` invocation creates a sweep directory
// under the report-root path containing:
//   manifest.json     — sweep-level metadata, written atomically
//                       (tmp + rename) before the worker pool starts
//   iterations.jsonl  — one JSONL line per completed iteration
//   top-N.txt         — human-friendly placeholder (WM-BT-7 replaces)
//
// Manifest + iterations are JSON; the JSONL row's metrics block keeps
// the same scalar field surface as `wm_market_stats_t` (+ sharpe /
// sortino from the per-snapshot ring) so downstream renderers don't
// need access to the in-memory snapshot.

#define WHENMOON_INTERNAL
#include "wm_bt_report.h"

#include "backtest.h"
#include "market.h"
#include "market_engine.h"
#include "sweep.h"
#include "wm_bt_assets.h"
#include "wm_bt_chart.h"
#include "wm_bt_metrics.h"
#include "whenmoon_strategy.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "kv.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>

#define WM_BT_REPORT_CTX            "whenmoon.bt.report"
#define WM_BT_KV_REPORT_PATH        "plugin.whenmoon.backtest.report_path"
#define WM_BT_REPORT_DEFAULT_REL    ".local/share/botmanager/backtests"

// Inline-SVG P/L distribution geometry (WM-BT-RPT-3). Up to _BINS equal-
// width buckets across a _W x _H viewBox; _PADX/_PADT/_PADB are the
// left+right / top / bottom plot insets (the bottom inset holds the
// min/zero/max tick labels).
#define WM_BT_HIST_BINS             21
#define WM_BT_HIST_W                720
#define WM_BT_HIST_H                220
#define WM_BT_HIST_PADX             8
#define WM_BT_HIST_PADT             10
#define WM_BT_HIST_PADB             28

// Sweep-dashboard inline-SVG geometry (WM-BT-RPT-5). Per-axis marginal
// best is a zero-baselined bar chart (one bar per swept value); the 2-axis
// score heatmap is a cell grid with left (y) + bottom (x) tick gutters and
// a color legend below. Cells are capped per axis so a 64x64 plan stays a
// sane width (a truncation note is emitted past the cap).
#define WM_BT_MARG_W                720
#define WM_BT_MARG_H                200
#define WM_BT_MARG_PADX             8
#define WM_BT_MARG_PADT             12
#define WM_BT_MARG_PADB             30
#define WM_BT_MARG_LBL_MAX          16u    // most x-axis labels drawn
#define WM_BT_HEAT_MAX_VALUES       32u    // cells per axis cap (layout)
#define WM_BT_HEAT_CELL             30     // px per cell
#define WM_BT_HEAT_PADL             104    // left gutter (y-axis labels)
#define WM_BT_HEAT_PADT             12
#define WM_BT_HEAT_PADR             16
#define WM_BT_HEAT_PADB             64     // bottom gutter (x labels + legend)

// wm_bt_sweep_score_value collapses zero-trade / unavailable-metric
// iterations to NOSCORE (-DBL_MAX, sweep.c) so they sort to the bottom.
// -DBL_MAX is finite, so an isfinite() guard alone won't catch it — treat
// any non-finite or extreme-negative score as "no score" for the visuals.
#define WM_BT_SCORE_MISSING(s)      (!isfinite(s) || (s) <= -DBL_MAX / 2.0)

// ----------------------------------------------------------------------- //
// Mode label                                                              //
// ----------------------------------------------------------------------- //

static const char *
wm_bt_report_mode_str(wm_bt_run_mode_t m)
{
  switch(m)
  {
    case WM_BT_MODE_FULL:         return("full");
    case WM_BT_MODE_WALK_FORWARD: return("walk");
    case WM_BT_MODE_OOS:          return("oos");
  }

  return("unknown");
}

// ----------------------------------------------------------------------- //
// JSON numeric helper: NaN/Inf → JSON null so the file is valid           //
// ----------------------------------------------------------------------- //

static struct json_object *
wm_bt_jdouble(double v)
{
  if(!isfinite(v))
    return(NULL);

  return(json_object_new_double(v));
}

static void
wm_bt_obj_add_double(struct json_object *parent, const char *key, double v)
{
  struct json_object *o = wm_bt_jdouble(v);

  if(o == NULL)
    o = NULL;  // json_object_object_add(parent, key, NULL) emits "null".

  json_object_object_add(parent, key, o);
}

// ----------------------------------------------------------------------- //
// Recursive mkdir                                                         //
// ----------------------------------------------------------------------- //
//
// Walks the absolute-path string in-place (mutating + restoring a single
// path-separator byte at a time) and mkdir's each segment with `mode`.
// Empty segments (`//`) are skipped. EEXIST is success. Anything else
// FAILs with errno set.

static bool
wm_bt_mkdir_p(char *path, mode_t mode, char *err, size_t err_cap)
{
  size_t i;
  size_t len;

  if(path == NULL || path[0] == '\0')
  {
    if(err != NULL)
      snprintf(err, err_cap, "mkdir_p: empty path");
    return(FAIL);
  }

  len = strlen(path);

  for(i = 1; i <= len; i++)
  {
    char saved;

    if(i < len && path[i] != '/')
      continue;

    saved   = path[i];
    path[i] = '\0';

    if(path[0] != '\0' && strcmp(path, "/") != 0)
    {
      if(mkdir(path, mode) != 0 && errno != EEXIST)
      {
        if(err != NULL)
          snprintf(err, err_cap,
              "mkdir('%s') failed: %s", path, strerror(errno));

        path[i] = saved;
        return(FAIL);
      }
    }

    path[i] = saved;
  }

  return(SUCCESS);
}

// ----------------------------------------------------------------------- //
// Report path resolve                                                     //
// ----------------------------------------------------------------------- //

bool
wm_bt_report_path_resolve(char *out, size_t cap, char *err, size_t err_cap)
{
  const char *kv;
  const char *home;
  char        buf[1024];
  int         n;

  if(out == NULL || cap == 0)
  {
    if(err != NULL)
      snprintf(err, err_cap, "report_path_resolve: bad out buffer");
    return(FAIL);
  }

  kv = kv_get_str(WM_BT_KV_REPORT_PATH);

  if(kv != NULL && kv[0] != '\0')
  {
    n = snprintf(buf, sizeof(buf), "%s", kv);
  }
  else
  {
    home = getenv("HOME");

    if(home == NULL || home[0] == '\0')
    {
      if(err != NULL)
        snprintf(err, err_cap, "$HOME unset; set %s explicitly",
            WM_BT_KV_REPORT_PATH);
      return(FAIL);
    }

    n = snprintf(buf, sizeof(buf), "%s/%s",
        home, WM_BT_REPORT_DEFAULT_REL);
  }

  if(n < 0 || (size_t)n >= sizeof(buf))
  {
    if(err != NULL)
      snprintf(err, err_cap, "report path overflow");
    return(FAIL);
  }

  // Trim a trailing slash so downstream "<root>/<sweep_id>" composition
  // produces canonical paths without "//" mid-string.
  while(n > 1 && buf[n - 1] == '/')
    buf[--n] = '\0';

  if(wm_bt_mkdir_p(buf, WM_BT_REPORT_DIR_MODE, err, err_cap) != SUCCESS)
    return(FAIL);

  if((size_t)n >= cap)
  {
    if(err != NULL)
      snprintf(err, err_cap, "report path overflow (out cap %zu)", cap);
    return(FAIL);
  }

  memcpy(out, buf, (size_t)n + 1);

  return(SUCCESS);
}

// ----------------------------------------------------------------------- //
// Sweep id                                                                //
// ----------------------------------------------------------------------- //

void
wm_bt_sweep_id_generate(const char *strategy, const char *source_market_id,
    char *out, size_t cap)
{
  time_t      t = time(NULL);
  struct tm   tm;
  const char *hyphen;
  const char *short_market;

  if(out == NULL || cap == 0)
    return;

  if(gmtime_r(&t, &tm) == NULL)
    memset(&tm, 0, sizeof(tm));

  hyphen       = (source_market_id != NULL) ? strchr(source_market_id, '-')
                                            : NULL;
  short_market = (hyphen != NULL) ? hyphen + 1
                                  : (source_market_id != NULL ? source_market_id
                                                              : "unknown");

  snprintf(out, cap, "%04d%02d%02d-%02d%02d%02d-%s-%s",
      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
      tm.tm_hour, tm.tm_min, tm.tm_sec,
      strategy != NULL ? strategy : "unknown",
      short_market);
}

// ----------------------------------------------------------------------- //
// Sweep dir                                                               //
// ----------------------------------------------------------------------- //

bool
wm_bt_sweep_dir_create(const char *report_path, const char *sweep_id,
    char *out_path, size_t cap, char *err, size_t err_cap)
{
  char charts[1024];
  int  n;

  if(report_path == NULL || sweep_id == NULL ||
     out_path == NULL || cap == 0)
  {
    if(err != NULL)
      snprintf(err, err_cap, "sweep_dir_create: bad args");
    return(FAIL);
  }

  n = snprintf(out_path, cap, "%s/%s", report_path, sweep_id);

  if(n < 0 || (size_t)n >= cap)
  {
    if(err != NULL)
      snprintf(err, err_cap, "sweep dir path overflow");
    return(FAIL);
  }

  if(mkdir(out_path, WM_BT_REPORT_DIR_MODE) != 0 && errno != EEXIST)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "mkdir('%s') failed: %s", out_path, strerror(errno));
    return(FAIL);
  }

  n = snprintf(charts, sizeof(charts), "%s/charts", out_path);

  if(n < 0 || (size_t)n >= sizeof(charts))
  {
    if(err != NULL)
      snprintf(err, err_cap, "charts subdir path overflow");
    return(FAIL);
  }

  if(mkdir(charts, WM_BT_REPORT_DIR_MODE) != 0 && errno != EEXIST)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "mkdir('%s') failed: %s", charts, strerror(errno));
    return(FAIL);
  }

  return(SUCCESS);
}

// ----------------------------------------------------------------------- //
// JSONL writer                                                            //
// ----------------------------------------------------------------------- //

bool
wm_bt_iter_open(wm_bt_iterations_writer_t *w, const char *sweep_dir,
    char *err, size_t err_cap)
{
  char path[1024];
  int  n;

  if(w == NULL || sweep_dir == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "iter_open: bad args");
    return(FAIL);
  }

  w->fp        = NULL;
  w->n_written = 0;

  n = snprintf(path, sizeof(path), "%s/iterations.jsonl", sweep_dir);

  if(n < 0 || (size_t)n >= sizeof(path))
  {
    if(err != NULL)
      snprintf(err, err_cap, "iterations path overflow");
    return(FAIL);
  }

  w->fp = fopen(path, "w");

  if(w->fp == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "fopen('%s') failed: %s", path, strerror(errno));
    return(FAIL);
  }

  if(pthread_mutex_init(&w->lock, NULL) != 0)
  {
    fclose(w->fp);
    w->fp = NULL;

    if(err != NULL)
      snprintf(err, err_cap, "pthread_mutex_init failed");
    return(FAIL);
  }

  return(SUCCESS);
}

// Pack the per-axis chosen value at iter as either a JSON int (for INT /
// UINT) or a double (for DOUBLE). Returns a json_object (caller owns
// reference) or NULL on bad type.
static struct json_object *
wm_bt_pack_axis_value(const wm_bt_sweep_axis_t *axis, double v)
{
  switch(axis->type)
  {
    case WM_PARAM_INT:
      return(json_object_new_int64((int64_t)llround(v)));

    case WM_PARAM_UINT:
      return(json_object_new_int64((int64_t)llround(v < 0.0 ? 0 : v)));

    case WM_PARAM_DOUBLE:
      return(wm_bt_jdouble(v));

    case WM_PARAM_STR:
    default:
      return(NULL);
  }
}

static struct json_object *
wm_bt_build_params_obj(const wm_bt_sweep_plan_t *plan,
    const uint32_t *indices)
{
  struct json_object *obj = json_object_new_object();
  uint32_t            a;

  if(obj == NULL)
    return(NULL);

  for(a = 0; a < plan->n_axes; a++)
  {
    const wm_bt_sweep_axis_t *axis = &plan->axes[a];
    double                    v    = axis->values[indices[a]];
    struct json_object       *jv   = wm_bt_pack_axis_value(axis, v);

    json_object_object_add(obj, axis->name, jv);
  }

  return(obj);
}

double
wm_bt_compute_equity(const wm_market_session_snapshot_t *snap)
{
  const wm_market_stats_t *st;
  double                   position_value;

  if(snap == NULL)
    return(0.0);

  st = &snap->stats[WM_MARKET_MODE_PAPER];

  position_value = (snap->position.side == WM_MARKET_POS_LONG)
      ? snap->position.qty * snap->last_mark_px
      : 0.0;

  return(st->cash + position_value);
}

static struct json_object *
wm_bt_build_metrics_obj(const wm_bt_sweep_result_t *result)
{
  struct json_object             *obj = json_object_new_object();
  const wm_market_session_snapshot_t *snap;
  const wm_market_stats_t           *st;
  uint32_t                           n_round_trips;
  double                             win_rate;
  double                             pf;

  if(obj == NULL)
    return(NULL);

  snap          = &result->trade;
  st            = &snap->stats[WM_MARKET_MODE_PAPER];
  n_round_trips = st->n_wins + st->n_losses;
  win_rate      = (n_round_trips > 0)
                  ? (double)st->n_wins / (double)n_round_trips
                  : 0.0;
  pf            = wm_market_stats_profit_factor(st);

  json_object_object_add(obj, "trades",
      json_object_new_int64((int64_t)n_round_trips));
  json_object_object_add(obj, "fills",
      json_object_new_int64((int64_t)st->lifetime_fills_count));
  wm_bt_obj_add_double(obj, "realized_pnl", st->realized_pnl_lifetime);
  wm_bt_obj_add_double(obj, "sharpe",        snap->sharpe);
  wm_bt_obj_add_double(obj, "sortino",       snap->sortino);
  wm_bt_obj_add_double(obj, "pf",            pf);
  wm_bt_obj_add_double(obj, "max_drawdown",  st->max_drawdown);
  wm_bt_obj_add_double(obj, "final_equity",  wm_bt_compute_equity(snap));
  json_object_object_add(obj, "n_wins",
      json_object_new_int64((int64_t)st->n_wins));
  json_object_object_add(obj, "n_losses",
      json_object_new_int64((int64_t)st->n_losses));
  wm_bt_obj_add_double(obj, "win_rate",      win_rate);
  wm_bt_obj_add_double(obj, "gross_profit",  st->gross_profit);
  wm_bt_obj_add_double(obj, "gross_loss",    st->gross_loss);
  wm_bt_obj_add_double(obj, "equity_peak",   st->equity_peak);

  return(obj);
}

static struct json_object *
wm_bt_build_oos_obj(const wm_bt_sweep_result_t *result)
{
  struct json_object *obj = json_object_new_object();

  if(obj == NULL)
    return(NULL);

  if(result->have_oos)
  {
    json_object_object_add(obj, "have", json_object_new_boolean(1));
    wm_bt_obj_add_double(obj, "score",    result->oos_score);
    wm_bt_obj_add_double(obj, "realized", result->oos_realized);
    json_object_object_add(obj, "n_trades",
        json_object_new_int64((int64_t)result->oos_n_trades));

    if(result->oos_err[0] != '\0')
      json_object_object_add(obj, "err",
          json_object_new_string(result->oos_err));
  }
  else
  {
    json_object_object_add(obj, "have", json_object_new_boolean(0));
  }

  return(obj);
}

// WM-BT-WF-PERFOLD-1: per-test-window (fold) array for walk-forward rows.
// One object per fold with the window bounds + independent (non-
// compounding) realized PnL / return / equity / trade count. Empty array
// when no folds were measured (non-top-K rows, or non-walk-forward modes).
static struct json_object *
wm_bt_build_windows_arr(const wm_bt_sweep_result_t *result)
{
  struct json_object *arr = json_object_new_array();
  uint32_t            i;

  if(arr == NULL)
    return(NULL);

  for(i = 0; i < result->n_folds; i++)
  {
    const wm_bt_fold_metric_t *f   = &result->folds[i];
    struct json_object        *obj = json_object_new_object();

    if(obj == NULL)
      continue;

    json_object_object_add(obj, "fold",
        json_object_new_int64((int64_t)i));
    json_object_object_add(obj, "start_ts_ms",
        json_object_new_int64(f->start_ts_ms));
    json_object_object_add(obj, "end_ts_ms",
        json_object_new_int64(f->end_ts_ms));
    json_object_object_add(obj, "ok",
        json_object_new_boolean(f->ok ? 1 : 0));
    json_object_object_add(obj, "trades",
        json_object_new_int64((int64_t)f->n_trades));
    wm_bt_obj_add_double(obj, "realized_pnl", f->realized_pnl);
    wm_bt_obj_add_double(obj, "return",       f->return_frac);
    wm_bt_obj_add_double(obj, "final_equity", f->final_equity);

    json_object_array_add(arr, obj);
  }

  return(arr);
}

bool
wm_bt_iter_append(wm_bt_iterations_writer_t *w,
    const wm_bt_sweep_plan_t *plan, uint32_t iter,
    const wm_bt_sweep_result_t *result)
{
  struct json_object *row;
  struct json_object *params_obj;
  const char         *json_str;
  bool                ok = SUCCESS;

  if(w == NULL || plan == NULL || result == NULL || w->fp == NULL)
    return(FAIL);

  row = json_object_new_object();

  if(row == NULL)
    return(FAIL);

  json_object_object_add(row, "iter",
      json_object_new_int64((int64_t)iter));

  params_obj = wm_bt_build_params_obj(plan, result->indices);
  json_object_object_add(row, "params", params_obj);

  json_object_object_add(row, "ok",
      json_object_new_boolean(result->ok ? 1 : 0));

  if(result->ok)
  {
    json_object_object_add(row, "metrics", wm_bt_build_metrics_obj(result));
    json_object_object_add(row, "wallclock_ms",
        json_object_new_int64((int64_t)result->wallclock_ms));
    json_object_object_add(row, "bars_replayed",
        json_object_new_int64((int64_t)result->bars_replayed));
    json_object_object_add(row, "n_windows",
        json_object_new_int64((int64_t)result->n_windows));
    json_object_object_add(row, "oos", wm_bt_build_oos_obj(result));

    // WM-BT-WF-PERFOLD-1: per-fold breakdown (walk-forward top-K rows).
    if(result->n_folds > 0 && result->folds != NULL)
      json_object_object_add(row, "windows",
          wm_bt_build_windows_arr(result));
  }
  else
  {
    json_object_object_add(row, "err",
        json_object_new_string(result->err[0] != '\0' ? result->err
                                                     : "(unknown)"));
  }

  json_str = json_object_to_json_string_ext(row,
      JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);

  if(json_str == NULL)
  {
    ok = FAIL;
    goto out;
  }

  pthread_mutex_lock(&w->lock);

  if(fputs(json_str, w->fp) < 0 || fputc('\n', w->fp) == EOF)
    ok = FAIL;
  else
    w->n_written++;

  pthread_mutex_unlock(&w->lock);

out:
  json_object_put(row);
  return(ok);
}

void
wm_bt_iter_close(wm_bt_iterations_writer_t *w)
{
  if(w == NULL)
    return;

  if(w->fp != NULL)
  {
    fflush(w->fp);
    fclose(w->fp);
    w->fp = NULL;
  }

  pthread_mutex_destroy(&w->lock);
}

// ----------------------------------------------------------------------- //
// Manifest                                                                //
// ----------------------------------------------------------------------- //

static struct json_object *
wm_bt_build_axes_array(const wm_bt_sweep_plan_t *plan)
{
  struct json_object *arr = json_object_new_array();
  uint32_t            a;
  uint32_t            i;

  if(arr == NULL)
    return(NULL);

  for(a = 0; a < plan->n_axes; a++)
  {
    const wm_bt_sweep_axis_t *axis   = &plan->axes[a];
    struct json_object       *entry  = json_object_new_object();
    struct json_object       *values = json_object_new_array();

    if(entry == NULL || values == NULL)
    {
      if(entry  != NULL) json_object_put(entry);
      if(values != NULL) json_object_put(values);

      continue;
    }

    json_object_object_add(entry, "name",
        json_object_new_string(axis->name));
    json_object_object_add(entry, "n_values",
        json_object_new_int64((int64_t)axis->n_values));

    for(i = 0; i < axis->n_values; i++)
    {
      struct json_object *jv = wm_bt_pack_axis_value(axis, axis->values[i]);

      json_object_array_add(values, jv);
    }

    json_object_object_add(entry, "values", values);
    json_object_array_add(arr, entry);
  }

  return(arr);
}

static struct json_object *
wm_bt_build_fixed_params_obj(const wm_backtest_params_t *p)
{
  struct json_object *obj = json_object_new_object();

  if(obj == NULL || p == NULL)
    return(obj);

  if(p->have_fee_bps)
    wm_bt_obj_add_double(obj, "fee_bps", p->fee_bps);

  if(p->have_slip_bps)
    wm_bt_obj_add_double(obj, "slip_bps", p->slip_bps);

  if(p->have_size_frac)
    wm_bt_obj_add_double(obj, "size_frac", p->size_frac);

  if(p->have_starting_cash)
    wm_bt_obj_add_double(obj, "starting_cash", p->starting_cash);

  return(obj);
}

// Write the whole NUL-terminated string `content` atomically to `path`
// via tmp + fwrite + '\n' + fflush + fsync + rename. A trailing newline is
// always appended.
bool
wm_bt_write_atomic(const char *path, const char *content,
    char *err, size_t err_cap)
{
  char   tmp[1280];
  size_t len;
  FILE  *fp;
  int    fd;
  int    n;

  if(path == NULL || content == NULL)
    return(FAIL);

  len = strlen(content);

  n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);

  if(n < 0 || (size_t)n >= sizeof(tmp))
  {
    if(err != NULL)
      snprintf(err, err_cap, "manifest tmp path overflow");
    return(FAIL);
  }

  fp = fopen(tmp, "w");

  if(fp == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "fopen('%s') failed: %s", tmp, strerror(errno));
    return(FAIL);
  }

  if(fwrite(content, 1, len, fp) != len ||
     fputc('\n', fp) == EOF)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "fwrite('%s') failed: %s", tmp, strerror(errno));
    fclose(fp);
    unlink(tmp);
    return(FAIL);
  }

  if(fflush(fp) != 0)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "fflush('%s') failed: %s", tmp, strerror(errno));
    fclose(fp);
    unlink(tmp);
    return(FAIL);
  }

  fd = fileno(fp);

  if(fd >= 0 && fsync(fd) != 0)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "fsync('%s') failed: %s", tmp, strerror(errno));
    fclose(fp);
    unlink(tmp);
    return(FAIL);
  }

  fclose(fp);

  if(rename(tmp, path) != 0)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "rename('%s' → '%s') failed: %s",
          tmp, path, strerror(errno));
    unlink(tmp);
    return(FAIL);
  }

  return(SUCCESS);
}

bool
wm_bt_manifest_write(const char *sweep_dir,
    const char *sweep_id, const char *wm_path,
    const wm_backtest_snapshot_t *snap,
    const char *strategy,
    const wm_bt_sweep_plan_t *plan,
    const wm_bt_sweep_mode_t *mode,
    const wm_backtest_params_t *fixed_params,
    uint64_t wallclock_ms, uint32_t ok_count, uint32_t fail_count,
    char *err, size_t err_cap)
{
  char                 path[1024];
  struct json_object  *root;
  const char          *json_str;
  bool                 ok;
  int                  n;
  wm_bt_run_mode_t     mode_val;

  if(sweep_dir == NULL || sweep_id == NULL || snap == NULL ||
     plan == NULL || strategy == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "manifest_write: bad args");
    return(FAIL);
  }

  n = snprintf(path, sizeof(path), "%s/manifest.json", sweep_dir);

  if(n < 0 || (size_t)n >= sizeof(path))
  {
    if(err != NULL)
      snprintf(err, err_cap, "manifest path overflow");
    return(FAIL);
  }

  root = json_object_new_object();

  if(root == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "manifest: json_object_new_object failed");
    return(FAIL);
  }

  mode_val = (mode != NULL) ? mode->mode : WM_BT_MODE_FULL;

  json_object_object_add(root, "sweep_id",
      json_object_new_string(sweep_id));
  json_object_object_add(root, "wm_file",
      json_object_new_string(wm_path != NULL ? wm_path : ""));
  json_object_object_add(root, "source_market_id",
      json_object_new_string(snap->source_market_id));
  json_object_object_add(root, "range_start",
      json_object_new_string(snap->range_start));
  json_object_object_add(root, "range_end",
      json_object_new_string(snap->range_end));
  json_object_object_add(root, "range_start_ms",
      json_object_new_int64(snap->range_start_ms));
  json_object_object_add(root, "range_end_ms",
      json_object_new_int64(snap->range_end_ms));
  json_object_object_add(root, "bars_loaded_1m",
      json_object_new_int64((int64_t)snap->bars_loaded_1m));
  json_object_object_add(root, "strategy",
      json_object_new_string(strategy));
  json_object_object_add(root, "axes", wm_bt_build_axes_array(plan));
  json_object_object_add(root, "fixed_params",
      wm_bt_build_fixed_params_obj(fixed_params));
  json_object_object_add(root, "mode",
      json_object_new_string(wm_bt_report_mode_str(mode_val)));

  if(mode_val == WM_BT_MODE_WALK_FORWARD && mode != NULL)
    json_object_object_add(root, "walk_n_windows",
        json_object_new_int64((int64_t)mode->walk.n));

  if(mode_val == WM_BT_MODE_OOS && mode != NULL)
  {
    struct json_object *oos = json_object_new_object();

    if(oos != NULL)
    {
      json_object_object_add(oos, "head_start_ms",
          json_object_new_int64(mode->oos_head.start_ts_ms));
      json_object_object_add(oos, "head_end_ms",
          json_object_new_int64(mode->oos_head.end_ts_ms));
      json_object_object_add(oos, "tail_start_ms",
          json_object_new_int64(mode->oos_tail.start_ts_ms));
      json_object_object_add(oos, "tail_end_ms",
          json_object_new_int64(mode->oos_tail.end_ts_ms));
      json_object_object_add(root, "oos_windows", oos);
    }
  }

  json_object_object_add(root, "rank_by",
      json_object_new_string(wm_bt_sweep_score_name(plan->score)));
  json_object_object_add(root, "top_n",
      json_object_new_int64((int64_t)plan->top_k));
  json_object_object_add(root, "threads",
      json_object_new_int64((int64_t)plan->workers));
  json_object_object_add(root, "total_iters",
      json_object_new_int64((int64_t)plan->total_iters));
  json_object_object_add(root, "ok_count",
      json_object_new_int64((int64_t)ok_count));
  json_object_object_add(root, "fail_count",
      json_object_new_int64((int64_t)fail_count));
  json_object_object_add(root, "wallclock_ms",
      json_object_new_int64((int64_t)wallclock_ms));
  json_object_object_add(root, "wm_indicator_schema_version",
      json_object_new_int64((int64_t)WM_INDICATOR_SCHEMA_VERSION));

  json_str = json_object_to_json_string_ext(root,
      JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);

  if(json_str == NULL)
  {
    json_object_put(root);

    if(err != NULL)
      snprintf(err, err_cap, "manifest: serialization failed");
    return(FAIL);
  }

  ok = wm_bt_write_atomic(path, json_str, err, err_cap);

  json_object_put(root);

  return(ok);
}

// ----------------------------------------------------------------------- //
// top-N.txt (WM-BT-7)                                                     //
// ----------------------------------------------------------------------- //
//
// ANSI-stripped sibling of the cmd_reply top-N output. Renders the
// same sorted slice into a tmp file, fsync's, then renames into place
// so an interrupted write never leaves a partial file.

bool
wm_bt_render_topn_txt(const char *sweep_dir,
    const wm_bt_sweep_plan_t *plan, const wm_bt_sweep_mode_t *mode,
    const wm_bt_sweep_result_t *results, uint32_t n_results,
    uint32_t n_ok, char *err, size_t err_cap)
{
  char       path[1024];
  char       tmp[1280];
  uint32_t  *indices  = NULL;
  uint32_t   top_k;
  FILE      *fp       = NULL;
  bool       ok       = SUCCESS;
  int        fd;
  int        n;

  if(sweep_dir == NULL || plan == NULL || results == NULL ||
     n_results == 0)
  {
    if(err != NULL)
      snprintf(err, err_cap, "topn_txt: bad args");
    return(FAIL);
  }

  n = snprintf(path, sizeof(path), "%s/top-N.txt", sweep_dir);

  if(n < 0 || (size_t)n >= sizeof(path))
  {
    if(err != NULL)
      snprintf(err, err_cap, "top-N.txt path overflow");
    return(FAIL);
  }

  n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);

  if(n < 0 || (size_t)n >= sizeof(tmp))
  {
    if(err != NULL)
      snprintf(err, err_cap, "top-N.txt tmp path overflow");
    return(FAIL);
  }

  indices = mem_alloc(WM_BT_REPORT_CTX, "topn_indices",
      sizeof(*indices) * (size_t)n_results);

  if(indices == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "topn_indices alloc failed");
    return(FAIL);
  }

  top_k = wm_bt_topk_compute(results, n_results, plan->top_k, indices);

  fp = fopen(tmp, "w");

  if(fp == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "fopen('%s') failed: %s", tmp, strerror(errno));
    ok = FAIL;
    goto out;
  }

  if(wm_bt_topk_to_file(fp, plan, mode, results, indices, top_k,
         n_results, n_ok) != SUCCESS)
  {
    if(err != NULL)
      snprintf(err, err_cap, "top-N.txt: render failed");
    ok = FAIL;
    goto out;
  }

  if(fflush(fp) != 0)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "fflush('%s') failed: %s", tmp, strerror(errno));
    ok = FAIL;
    goto out;
  }

  fd = fileno(fp);

  if(fd >= 0 && fsync(fd) != 0)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "fsync('%s') failed: %s", tmp, strerror(errno));
    ok = FAIL;
    goto out;
  }

  fclose(fp);
  fp = NULL;

  if(rename(tmp, path) != 0)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "rename('%s' → '%s') failed: %s",
          tmp, path, strerror(errno));
    ok = FAIL;
    goto out;
  }

out:
  if(fp != NULL)
    fclose(fp);

  if(ok != SUCCESS)
    unlink(tmp);

  if(indices != NULL)
    mem_free(indices);

  return(ok);
}

// ----------------------------------------------------------------------- //
// report.md (WM-BT-7)                                                     //
// ----------------------------------------------------------------------- //
//
// Self-contained markdown summary: header bullets, sweep-axes table,
// top-N table (mode-aware), per-axis marginal-best tables, failures
// list. Atomic via tmp + rename.

// Render a comma-separated value list for one axis into `out`. Handles
// integer + double types per axis->type. Truncates with " (…)" if the
// resulting string would overflow the buffer.
static void
wm_bt_md_render_axis_values(const wm_bt_sweep_axis_t *axis,
    char *out, size_t cap)
{
  size_t   off = 0;
  uint32_t i;
  int      w;

  if(out == NULL || cap == 0)
    return;

  out[0] = '\0';

  for(i = 0; i < axis->n_values; i++)
  {
    if(off + 16 >= cap)
    {
      // Truncate marker fits.
      snprintf(out + off, cap - off, " …");
      return;
    }

    if(axis->type == WM_PARAM_DOUBLE)
      w = snprintf(out + off, cap - off,
          "%s%.6g", i == 0 ? "" : ", ", axis->values[i]);
    else
      w = snprintf(out + off, cap - off,
          "%s%" PRId64,
          i == 0 ? "" : ", ", (int64_t)llround(axis->values[i]));

    if(w < 0)
      return;

    off += (size_t)w;
  }
}

// Render one cell value for a top-N row. Integer axes drop fractional
// noise; doubles use compact %.6g.
static void
wm_bt_md_render_axis_cell(const wm_bt_sweep_axis_t *axis,
    double v, char *out, size_t cap)
{
  if(out == NULL || cap == 0)
    return;

  if(axis->type == WM_PARAM_DOUBLE)
    snprintf(out, cap, "%.6g", v);
  else
    snprintf(out, cap, "%" PRId64, (int64_t)llround(v));
}

// Format a finite double for a markdown cell. NaN/Inf → "-" so the
// table stays narrow + readable.
static void
wm_bt_md_fmt_double(double v, const char *fmt, char *out, size_t cap)
{
  if(out == NULL || cap == 0)
    return;

  if(!isfinite(v))
  {
    snprintf(out, cap, "-");
    return;
  }

  snprintf(out, cap, fmt, v);
}

// Write everything accumulated in `fp` atomically: fflush + fsync +
// fclose + rename. Mirrors wm_bt_write_atomic but for FILE * already
// open on the tmp path.
static bool
wm_bt_finalize_file(FILE *fp, const char *tmp, const char *final_path,
    char *err, size_t err_cap)
{
  int fd;

  if(fp == NULL)
    return(FAIL);

  if(fflush(fp) != 0)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "fflush('%s') failed: %s", tmp, strerror(errno));
    fclose(fp);
    unlink(tmp);
    return(FAIL);
  }

  fd = fileno(fp);

  if(fd >= 0 && fsync(fd) != 0)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "fsync('%s') failed: %s", tmp, strerror(errno));
    fclose(fp);
    unlink(tmp);
    return(FAIL);
  }

  fclose(fp);

  if(rename(tmp, final_path) != 0)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "rename('%s' → '%s') failed: %s",
          tmp, final_path, strerror(errno));
    unlink(tmp);
    return(FAIL);
  }

  return(SUCCESS);
}

// Defined below (near the index-html renderers); forward-declared so the
// walk-forward per-fold table in wm_bt_render_report_md can format window
// bounds as "YYYY-MM-DD HH:MM" UTC.
static void wm_bt_idx_fmt_ts(int64_t ts_ms, char *out, size_t cap);

bool
wm_bt_render_report_md(const char *sweep_dir,
    const char *sweep_id, const char *wm_path,
    const wm_backtest_snapshot_t *snap, const char *strategy,
    const wm_bt_sweep_plan_t *plan, const wm_bt_sweep_mode_t *mode,
    const wm_bt_sweep_result_t *results, uint32_t n_results,
    uint32_t n_ok, uint32_t n_fail, uint64_t wallclock_ms,
    char *err, size_t err_cap)
{
  char              path[1024];
  char              tmp[1280];
  char              cell[64];
  char              values_buf[256];
  char              params_buf[160];
  uint32_t         *indices    = NULL;
  uint32_t          top_k      = 0;
  FILE             *fp         = NULL;
  wm_bt_run_mode_t  mode_val;
  bool              is_oos;
  int               n;
  uint32_t          i;
  uint32_t          a;
  uint32_t          v;

  if(sweep_dir == NULL || sweep_id == NULL || snap == NULL ||
     strategy == NULL || plan == NULL || results == NULL ||
     n_results == 0)
  {
    if(err != NULL)
      snprintf(err, err_cap, "report_md: bad args");
    return(FAIL);
  }

  mode_val = (mode != NULL) ? mode->mode : WM_BT_MODE_FULL;
  is_oos   = (mode_val == WM_BT_MODE_OOS);

  n = snprintf(path, sizeof(path), "%s/report.md", sweep_dir);

  if(n < 0 || (size_t)n >= sizeof(path))
  {
    if(err != NULL)
      snprintf(err, err_cap, "report.md path overflow");
    return(FAIL);
  }

  n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);

  if(n < 0 || (size_t)n >= sizeof(tmp))
  {
    if(err != NULL)
      snprintf(err, err_cap, "report.md tmp path overflow");
    return(FAIL);
  }

  indices = mem_alloc(WM_BT_REPORT_CTX, "report_indices",
      sizeof(*indices) * (size_t)n_results);

  if(indices == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "report_indices alloc failed");
    return(FAIL);
  }

  top_k = wm_bt_topk_compute(results, n_results, plan->top_k, indices);

  fp = fopen(tmp, "w");

  if(fp == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "fopen('%s') failed: %s", tmp, strerror(errno));
    mem_free(indices);
    return(FAIL);
  }

  // Header.
  fprintf(fp, "# Backtest sweep: %s\n\n", sweep_id);

  // Cross-link to the interactive dashboard (the HTML index links back to
  // this report.md + the other siblings; WM-BT-RPT-6).
  fprintf(fp, "> Interactive dashboard: [index.html](index.html)\n\n");

  fprintf(fp, "- **Strategy:** %s\n", strategy);
  fprintf(fp, "- **Market:** %s\n", snap->source_market_id);

  if(wm_path != NULL && wm_path[0] != '\0')
    fprintf(fp, "- **Source file:** `%s`\n", wm_path);

  fprintf(fp,
      "- **Range:** %s → %s (%u 1m bars)\n",
      snap->range_start, snap->range_end, snap->bars_loaded_1m);
  fprintf(fp,
      "- **Mode:** %s\n", wm_bt_report_mode_str(mode_val));
  fprintf(fp,
      "- **Threads:** %u\n", plan->workers);
  fprintf(fp,
      "- **Total iterations:** %u (ok=%u, fail=%u)\n",
      plan->total_iters, n_ok, n_fail);
  fprintf(fp,
      "- **Wallclock:** %.3f s\n", (double)wallclock_ms / 1000.0);
  fprintf(fp,
      "- **Rank by:** %s\n",
      wm_bt_sweep_score_name(plan->score));

  if(mode_val == WM_BT_MODE_WALK_FORWARD && mode != NULL)
    fprintf(fp,
        "- **Walk-forward windows:** %u\n", mode->walk.n);

  if(is_oos && mode != NULL)
    fprintf(fp,
        "- **OOS windows:** head=[%" PRId64 "..%" PRId64
        "] tail=[%" PRId64 "..%" PRId64 "]\n",
        mode->oos_head.start_ts_ms, mode->oos_head.end_ts_ms,
        mode->oos_tail.start_ts_ms, mode->oos_tail.end_ts_ms);

  fputc('\n', fp);

  // Sweep axes.
  fprintf(fp, "## Sweep axes\n\n");

  if(plan->n_axes == 0)
    fprintf(fp, "(no sweep axes — single iteration)\n\n");
  else
  {
    fprintf(fp, "| Param | n | Values |\n");
    fprintf(fp, "|---|---|---|\n");

    for(a = 0; a < plan->n_axes; a++)
    {
      const wm_bt_sweep_axis_t *axis = &plan->axes[a];

      wm_bt_md_render_axis_values(axis, values_buf, sizeof(values_buf));
      fprintf(fp, "| %s | %u | %s |\n",
          axis->name, axis->n_values, values_buf);
    }

    fputc('\n', fp);
  }

  // Top-N table.
  fprintf(fp, "## Top %u by %s\n\n",
      top_k, wm_bt_sweep_score_name(plan->score));

  // Header row: Rank, Iter, [Score | Head/OOS Score], <axes>,
  // trades, fills, realized, sharpe, sortino, pf, drawdown, equity, ms.
  fputs("| Rank | Iter |", fp);

  if(is_oos)
    fputs(" Head | OOS |", fp);
  else
    fputs(" Score |", fp);

  for(a = 0; a < plan->n_axes; a++)
    fprintf(fp, " %s |", plan->axes[a].name);

  fputs(" trades | fills | realized | sharpe | sortino | pf |"
        " drawdown | equity | ms |\n", fp);

  // Separator.
  fputs("|---|---|", fp);

  if(is_oos)
    fputs("---|---|", fp);
  else
    fputs("---|", fp);

  for(a = 0; a < plan->n_axes; a++)
    fputs("---|", fp);

  fputs("---|---|---|---|---|---|---|---|---|\n", fp);

  // Rows.
  for(i = 0; i < top_k; i++)
  {
    const wm_bt_sweep_result_t *r = &results[indices[i]];
    const wm_market_stats_t    *st_paper;
    uint32_t                    n_round_trips;
    uint32_t                    n_fills;
    double                      pf;
    double                      equity;

    fprintf(fp, "| %u | %" PRId64 " |",
        i + 1,
        r->run_id_db > 0 ? r->run_id_db : (int64_t)(indices[i] + 1));

    if(!r->ok)
    {
      // FAIL row: collapse the metric columns to "-" so the table
      // still parses cleanly.
      if(is_oos)
        fputs(" - | - |", fp);
      else
        fputs(" - |", fp);

      for(a = 0; a < plan->n_axes; a++)
      {
        wm_bt_md_render_axis_cell(&plan->axes[a],
            plan->axes[a].values[r->indices[a]],
            cell, sizeof(cell));
        fprintf(fp, " %s |", cell);
      }

      fputs(" - | - | FAIL | - | - | - | - | - | - |\n", fp);
      continue;
    }

    wm_bt_md_fmt_double(r->score, "%+.4f", cell, sizeof(cell));
    fprintf(fp, " %s |", cell);

    if(is_oos)
    {
      if(r->have_oos)
        wm_bt_md_fmt_double(r->oos_score, "%+.4f", cell, sizeof(cell));
      else
        snprintf(cell, sizeof(cell), "n/a");
      fprintf(fp, " %s |", cell);
    }

    for(a = 0; a < plan->n_axes; a++)
    {
      wm_bt_md_render_axis_cell(&plan->axes[a],
          plan->axes[a].values[r->indices[a]],
          cell, sizeof(cell));
      fprintf(fp, " %s |", cell);
    }

    st_paper      = &r->trade.stats[WM_MARKET_MODE_PAPER];
    n_round_trips = st_paper->n_wins + st_paper->n_losses;
    n_fills       = (uint32_t)st_paper->lifetime_fills_count;
    pf            = wm_market_stats_profit_factor(st_paper);
    equity        = wm_bt_compute_equity(&r->trade);

    fprintf(fp, " %u | %u |", n_round_trips, n_fills);

    wm_bt_md_fmt_double(st_paper->realized_pnl_lifetime, "%+.4f",
        cell, sizeof(cell));
    fprintf(fp, " %s |", cell);

    wm_bt_md_fmt_double(r->trade.sharpe,  "%.3f", cell, sizeof(cell));
    fprintf(fp, " %s |", cell);

    wm_bt_md_fmt_double(r->trade.sortino, "%.3f", cell, sizeof(cell));
    fprintf(fp, " %s |", cell);

    wm_bt_md_fmt_double(pf, "%.3f", cell, sizeof(cell));
    fprintf(fp, " %s |", cell);

    wm_bt_md_fmt_double(st_paper->max_drawdown, "%.4f",
        cell, sizeof(cell));
    fprintf(fp, " %s |", cell);

    wm_bt_md_fmt_double(equity, "%.4f", cell, sizeof(cell));
    fprintf(fp, " %s |", cell);

    fprintf(fp, " %" PRIu64 " |\n", r->wallclock_ms);
  }

  fputc('\n', fp);

  // WM-BT-WF-PERFOLD-1: per-test-window (fold) breakdown for the rank-1
  // (best-scoring) walk-forward row. Each fold is measured INDEPENDENTLY
  // (own book from starting_cash, warmed by the full pre-window history),
  // so realized/return are non-compounding per-regime readings — the
  // consistency view the aggregate row can't give. Only the top row is
  // tabled here; every top-K row's folds are in iterations.jsonl.
  if(mode_val == WM_BT_MODE_WALK_FORWARD && top_k > 0 &&
     indices != NULL)
  {
    const wm_bt_sweep_result_t *best = &results[indices[0]];

    if(best->n_folds > 0 && best->folds != NULL)
    {
      uint32_t f;
      uint32_t n_pos = 0;
      uint32_t n_ok_folds = 0;

      fprintf(fp, "## Walk-forward per-window (fold) breakdown"
                  " — rank 1\n\n");
      fprintf(fp,
          "Each fold is an INDEPENDENT out-of-sample test window"
          " (own book from starting cash, strategy warmed by prior"
          " history); realized PnL / return are non-compounding.\n\n");
      fprintf(fp, "| Fold | Start (UTC) | End (UTC) | Trades |"
                  " Realized | Return %% | Equity | ok |\n");
      fprintf(fp, "|---|---|---|---|---|---|---|---|\n");

      for(f = 0; f < best->n_folds; f++)
      {
        const wm_bt_fold_metric_t *fm = &best->folds[f];
        char start_s[32];
        char end_s[32];

        wm_bt_idx_fmt_ts(fm->start_ts_ms, start_s, sizeof(start_s));
        wm_bt_idx_fmt_ts(fm->end_ts_ms,   end_s,   sizeof(end_s));

        fprintf(fp, "| %u | %s | %s | %u |", f, start_s, end_s,
            fm->n_trades);

        wm_bt_md_fmt_double(fm->realized_pnl, "%+.4f",
            cell, sizeof(cell));
        fprintf(fp, " %s |", cell);

        wm_bt_md_fmt_double(fm->return_frac * 100.0, "%+.2f",
            cell, sizeof(cell));
        fprintf(fp, " %s |", cell);

        wm_bt_md_fmt_double(fm->final_equity, "%.2f",
            cell, sizeof(cell));
        fprintf(fp, " %s | %s |\n", cell, fm->ok ? "y" : "n");

        if(fm->ok)
        {
          n_ok_folds++;
          if(fm->realized_pnl > 0.0)
            n_pos++;
        }
      }

      fputc('\n', fp);

      // Consistency summary the WM-BT-WF-PERFOLD-1 gate keys on.
      fprintf(fp,
          "- **Net-positive folds:** %u / %u measured (%.1f%%)\n\n",
          n_pos, n_ok_folds,
          n_ok_folds > 0 ? 100.0 * (double)n_pos / (double)n_ok_folds
                         : 0.0);
    }
  }

  // Per-axis marginal best — for each axis, for each value, scan all
  // iterations and pick the highest score for that value. Skip
  // single-iteration plans (no axes ⇒ nothing to marginalise).
  if(plan->n_axes > 0)
  {
    fprintf(fp, "## Per-axis marginal best\n\n");

    for(a = 0; a < plan->n_axes; a++)
    {
      const wm_bt_sweep_axis_t *axis = &plan->axes[a];

      fprintf(fp, "### %s\n\n", axis->name);
      fprintf(fp, "| value | best score | iter |\n");
      fprintf(fp, "|---|---|---|\n");

      for(v = 0; v < axis->n_values; v++)
      {
        double   best_score = -INFINITY;
        int64_t  best_iter  = -1;
        uint32_t k;

        for(k = 0; k < n_results; k++)
        {
          if(!results[k].ok)
            continue;

          if(results[k].indices[a] != v)
            continue;

          if(results[k].score > best_score)
          {
            best_score = results[k].score;
            best_iter  = results[k].run_id_db > 0
                ? results[k].run_id_db
                : (int64_t)(k + 1);
          }
        }

        wm_bt_md_render_axis_cell(axis, axis->values[v],
            cell, sizeof(cell));
        fprintf(fp, "| %s |", cell);

        if(best_iter < 0)
          fputs(" - | - |\n", fp);
        else
        {
          char score_cell[64];

          wm_bt_md_fmt_double(best_score, "%+.4f",
              score_cell, sizeof(score_cell));
          fprintf(fp, " %s | %" PRId64 " |\n",
              score_cell, best_iter);
        }
      }

      fputc('\n', fp);
    }
  }

  // Failures.
  fprintf(fp, "## Failures\n\n");

  if(n_fail == 0)
    fprintf(fp, "(none)\n");
  else
  {
    fprintf(fp, "| Iter | Params | Reason |\n");
    fprintf(fp, "|---|---|---|\n");

    for(i = 0; i < n_results; i++)
    {
      const wm_bt_sweep_result_t *r = &results[i];

      if(r->ok)
        continue;

      params_buf[0] = '\0';

      for(a = 0; a < plan->n_axes; a++)
      {
        size_t off;

        wm_bt_md_render_axis_cell(&plan->axes[a],
            plan->axes[a].values[r->indices[a]],
            cell, sizeof(cell));

        off = strlen(params_buf);

        if(off + 1 >= sizeof(params_buf))
          break;

        snprintf(params_buf + off, sizeof(params_buf) - off,
            "%s%s=%s", off == 0 ? "" : " ",
            plan->axes[a].name, cell);
      }

      fprintf(fp, "| %" PRId64 " | %s | %.120s |\n",
          r->run_id_db > 0 ? r->run_id_db : (int64_t)(i + 1),
          params_buf,
          r->err[0] != '\0' ? r->err : "(unknown)");
    }
  }

  mem_free(indices);

  return(wm_bt_finalize_file(fp, tmp, path, err, err_cap));
}

// ----------------------------------------------------------------------- //
// index.html — browser-friendly sweep landing page                       //
// ----------------------------------------------------------------------- //
//
// Self-contained, JS-free (native <details> collapsibles) HTML report.
// Reuses the report.md statics above (axis-cell render, equity compute,
// double formatting, mode label). Trades are paired from the per-iter
// deep-fills capture exactly as the chart emitter pairs them, so the
// generated `charts/iter-K/trade-M-<gran>.html` hrefs line up with the
// files the chart pass wrote.

// wm_bt_html_escape moved to wm_bt_assets.c (WM-BT-RPT-4) — shared by the
// chart emitter too; declared in wm_bt_assets.h.

// "YYYY-MM-DD HH:MM" UTC, or numeric epoch on gmtime_r failure.
static void
wm_bt_idx_fmt_ts(int64_t ts_ms, char *out, size_t cap)
{
  time_t    t = (time_t)(ts_ms / 1000);
  struct tm tm;

  if(gmtime_r(&t, &tm) == NULL)
  {
    snprintf(out, cap, "%" PRId64, ts_ms);
    return;
  }

  snprintf(out, cap, "%04d-%02d-%02d %02d:%02d",
      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
      tm.tm_hour, tm.tm_min);
}

// Compact human duration: "3d 4h", "5h 12m", "47m", "30s".
static void
wm_bt_idx_fmt_dur(int64_t ms, char *out, size_t cap)
{
  int64_t s = ms / 1000;
  int64_t d;
  int64_t h;
  int64_t m;

  if(s < 0)
    s = 0;

  d = s / 86400; s %= 86400;
  h = s / 3600;  s %= 3600;
  m = s / 60;    s %= 60;

  if(d > 0)
    snprintf(out, cap, "%" PRId64 "d %" PRId64 "h", d, h);
  else if(h > 0)
    snprintf(out, cap, "%" PRId64 "h %" PRId64 "m", h, m);
  else if(m > 0)
    snprintf(out, cap, "%" PRId64 "m", m);
  else
    snprintf(out, cap, "%" PRId64 "s", s);
}

// Build "name=val name=val ..." for one iteration from the swept axes.
static void
wm_bt_idx_params_str(const wm_bt_sweep_plan_t *plan,
    const uint32_t *indices, char *out, size_t cap)
{
  uint32_t a;
  size_t   off = 0;

  if(out == NULL || cap == 0)
    return;

  out[0] = '\0';

  if(plan->n_axes == 0)
  {
    snprintf(out, cap, "(defaults)");
    return;
  }

  for(a = 0; a < plan->n_axes; a++)
  {
    char cell[64];
    int  w;

    wm_bt_md_render_axis_cell(&plan->axes[a],
        plan->axes[a].values[indices[a]], cell, sizeof(cell));

    w = snprintf(out + off, cap - off, "%s%s=%s",
        off == 0 ? "" : " ", plan->axes[a].name, cell);

    if(w < 0 || (size_t)w >= cap - off)
      break;

    off += (size_t)w;
  }
}

// Emit the per-grain chart links for trade `tidx` of charted rank
// `rank` (1-based). One link per grain in `emit_mask` — BUT only when
// the chart file actually exists on disk: the chart pass skips a
// (grain, trade) pair when no bars fall in its slice window (e.g. an
// intraday trade has no 1d slice), so we stat each candidate to avoid
// dangling links. `sweep_dir` is the absolute report dir the relative
// hrefs are anchored to.
static void
wm_bt_idx_emit_chart_links(FILE *fp, const char *sweep_dir,
    uint32_t rank, uint32_t tidx, uint16_t emit_mask)
{
  uint32_t g;
  bool     any = false;

  for(g = 0; g < WM_GRAN_MAX; g++)
  {
    const char *gname;
    char        full[1024];
    struct stat sb;
    int         w;

    if(((emit_mask >> g) & 1u) == 0)
      continue;

    gname = wm_bt_chart_gran_name((wm_gran_t)g);

    w = snprintf(full, sizeof(full),
        "%s/charts/iter-%u/trade-%u-%s.html", sweep_dir, rank, tidx, gname);

    if(w < 0 || (size_t)w >= sizeof(full))
      continue;

    if(stat(full, &sb) != 0 || !S_ISREG(sb.st_mode))
      continue;

    fprintf(fp,
        "<a class=\"chart-link\" target=\"_blank\" rel=\"noopener\""
        " href=\"charts/iter-%u/trade-%u-%s.html\">%s&#8599;</a>",
        rank, tidx, gname, gname);

    any = true;
  }

  if(!any)
    fputs("<span class=\"muted\">&mdash;</span>", fp);
}

// Shared buy->sell pairing iterator (WM-BT-RPT-3; declared in
// wm_bt_report.h). See the header for the contract + drive loop.
bool
wm_bt_trade_next(wm_bt_trade_iter_t *it,
    const wm_market_fill_t **entry, const wm_market_fill_t **exit_out)
{
  uint32_t f;

  if(it == NULL || it->fills == NULL || entry == NULL || exit_out == NULL)
    return(false);

  for(f = it->pos; f < it->n; f++)
  {
    uint32_t e;

    if(it->fills[f].side != 'b')
      continue;

    *entry    = &it->fills[f];
    *exit_out = NULL;

    for(e = f + 1; e < it->n; e++)
    {
      if(it->fills[e].side == 's')
      {
        *exit_out = &it->fills[e];
        it->pos   = e + 1;          // resume past the closing sell
        return(true);
      }
    }

    it->pos = f + 1;                 // open-at-end: resume after the buy
    return(true);
  }

  it->pos = it->n;
  return(false);
}

// Per-trade analytics derived from the captured fills (WM-BT-RPT-3).
// All fields cover CLOSED round-trips only — an open trade left at
// snapshot end is excluded from every stat (it counts only in the
// table's "Open" filter). avg_loss is signed negative; payoff =
// avg_win / |avg_loss| and is non-finite (rendered "n/a") when there
// are no losing trades. expectancy is the mean realized P/L per closed
// trade. avg_hold_ms is the mean (exit - entry) duration.
typedef struct
{
  uint32_t n;
  uint32_t wins;
  uint32_t losses;
  double   avg_win;
  double   avg_loss;
  double   best;
  double   worst;
  double   expectancy;
  double   payoff;
  int64_t  avg_hold_ms;
} wm_bt_trade_stats_t;

static void
wm_bt_trade_stats_compute(const wm_market_fill_t *fills, uint32_t n_fills,
    wm_bt_trade_stats_t *out)
{
  wm_bt_trade_iter_t      it = { fills, n_fills, 0 };
  const wm_market_fill_t *entry;
  const wm_market_fill_t *exit_fill;
  double                  sum_pnl  = 0.0;
  double                  sum_win  = 0.0;
  double                  sum_loss = 0.0;
  int64_t                 sum_hold = 0;

  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->best  = 0.0;
  out->worst = 0.0;

  while(wm_bt_trade_next(&it, &entry, &exit_fill))
  {
    double pnl;

    if(exit_fill == NULL)        // open trade — not a closed round-trip
      continue;

    pnl = exit_fill->realized_pnl;

    if(out->n == 0)
    {
      out->best  = pnl;
      out->worst = pnl;
    }

    else
    {
      if(pnl > out->best)  out->best  = pnl;
      if(pnl < out->worst) out->worst = pnl;
    }

    out->n++;
    sum_pnl  += pnl;
    sum_hold += exit_fill->ts_ms - entry->ts_ms;

    if(pnl > 0.0)
    {
      out->wins++;
      sum_win += pnl;
    }

    else if(pnl < 0.0)
    {
      out->losses++;
      sum_loss += pnl;
    }
  }

  if(out->n == 0)
  {
    out->payoff = NAN;             // nothing closed -> "n/a"
    return;
  }

  out->avg_win     = out->wins   > 0 ? sum_win  / (double)out->wins   : 0.0;
  out->avg_loss    = out->losses > 0 ? sum_loss / (double)out->losses : 0.0;
  out->expectancy  = sum_pnl / (double)out->n;
  out->avg_hold_ms = sum_hold / (int64_t)out->n;
  out->payoff      = (out->losses > 0 && out->avg_loss != 0.0)
      ? out->avg_win / fabs(out->avg_loss)
      : NAN;
}

// Inline-SVG P/L distribution histogram (WM-BT-RPT-3). Bins closed-trade
// realized P/L (dollars) into up to WM_BT_HIST_BINS equal-width buckets;
// bars whose bucket centre is >= 0 use the --win colour, the rest --loss.
// Three x-axis ticks (min / 0 / max) are labelled via wm_bt_fmt_usd. The
// SVG scales to its container width (viewBox + width:100%). Caller
// guarantees at least one closed trade.
static void
wm_bt_emit_pnl_histogram_svg(FILE *fp, const wm_market_fill_t *fills,
    uint32_t n_fills)
{
  wm_bt_trade_iter_t      it = { fills, n_fills, 0 };
  const wm_market_fill_t *entry;
  const wm_market_fill_t *exit_fill;
  uint32_t                bins[WM_BT_HIST_BINS];
  uint32_t                nbins;
  uint32_t                bmax  = 0;
  uint32_t                total = 0;
  uint32_t                b;
  double                  lo   =  INFINITY;
  double                  hi   = -INFINITY;
  double                  span;
  double                  plot_w;
  double                  plot_h;
  double                  bar_w;
  char                    lo_str[48];
  char                    hi_str[48];

  if(fp == NULL)
    return;

  // Pass 1: range of closed-trade P/L.
  while(wm_bt_trade_next(&it, &entry, &exit_fill))
  {
    double pnl;

    if(exit_fill == NULL)
      continue;

    pnl = exit_fill->realized_pnl;

    if(!isfinite(pnl))
      continue;

    if(pnl < lo) lo = pnl;
    if(pnl > hi) hi = pnl;
  }

  if(!isfinite(lo) || !isfinite(hi))
    return;                        // no finite closed trades

  span  = hi - lo;
  nbins = (span <= 0.0) ? 1u : (uint32_t)WM_BT_HIST_BINS;

  for(b = 0; b < nbins; b++)
    bins[b] = 0;

  // Pass 2: bin.
  it.pos = 0;

  while(wm_bt_trade_next(&it, &entry, &exit_fill))
  {
    double pnl;
    double frac;

    if(exit_fill == NULL)
      continue;

    pnl = exit_fill->realized_pnl;

    if(!isfinite(pnl))
      continue;

    if(span <= 0.0)
      b = 0;
    else
    {
      frac = (pnl - lo) / span;
      b    = (uint32_t)(frac * (double)nbins);

      if(b >= nbins)
        b = nbins - 1;             // hi value lands in the last bin
    }

    bins[b]++;
    total++;

    if(bins[b] > bmax)
      bmax = bins[b];
  }

  if(bmax == 0)
    return;

  wm_bt_fmt_usd(lo, lo_str, sizeof(lo_str));
  wm_bt_fmt_usd(hi, hi_str, sizeof(hi_str));

  plot_w = (double)(WM_BT_HIST_W - 2 * WM_BT_HIST_PADX);
  plot_h = (double)(WM_BT_HIST_H - WM_BT_HIST_PADT - WM_BT_HIST_PADB);
  bar_w  = plot_w / (double)nbins;

  fprintf(fp,
      "<svg class=\"hist\" viewBox=\"0 0 %d %d\" role=\"img\""
      " aria-label=\"P/L distribution across %u closed trades,"
      " from %s to %s\" preserveAspectRatio=\"none\">\n"
      "<title>Per-trade P/L distribution</title>\n",
      WM_BT_HIST_W, WM_BT_HIST_H, total, lo_str, hi_str);

  for(b = 0; b < nbins; b++)
  {
    double      x = (double)WM_BT_HIST_PADX + (double)b * bar_w;
    double      h = plot_h * ((double)bins[b] / (double)bmax);
    double      y = (double)WM_BT_HIST_PADT + (plot_h - h);
    double      centre = (span <= 0.0)
        ? lo
        : lo + ((double)b + 0.5) / (double)nbins * span;
    const char *cls = centre >= 0.0 ? "win" : "loss";

    if(bins[b] == 0)
      continue;

    fprintf(fp,
        "<rect class=\"%s\" x=\"%.2f\" y=\"%.2f\" width=\"%.2f\""
        " height=\"%.2f\"><title>%u trade(s)</title></rect>\n",
        cls, x + 0.5, y, bar_w > 1.0 ? bar_w - 1.0 : bar_w, h, bins[b]);
  }

  // Baseline + min/zero/max tick labels.
  fprintf(fp,
      "<line class=\"axis\" x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\"/>\n",
      WM_BT_HIST_PADX, WM_BT_HIST_H - WM_BT_HIST_PADB,
      WM_BT_HIST_W - WM_BT_HIST_PADX, WM_BT_HIST_H - WM_BT_HIST_PADB);

  fprintf(fp,
      "<text class=\"lbl\" x=\"%d\" y=\"%d\">%s</text>\n",
      WM_BT_HIST_PADX, WM_BT_HIST_H - 8, lo_str);
  fprintf(fp,
      "<text class=\"lbl\" x=\"%d\" y=\"%d\" text-anchor=\"end\">%s</text>\n",
      WM_BT_HIST_W - WM_BT_HIST_PADX, WM_BT_HIST_H - 8, hi_str);

  if(span > 0.0 && lo < 0.0 && hi > 0.0)
  {
    double zx = (double)WM_BT_HIST_PADX + (-lo / span) * plot_w;

    fprintf(fp,
        "<line class=\"axis\" x1=\"%.2f\" y1=\"%d\" x2=\"%.2f\" y2=\"%d\""
        " stroke-dasharray=\"3 3\"/>\n",
        zx, WM_BT_HIST_PADT, zx, WM_BT_HIST_H - WM_BT_HIST_PADB);
    fprintf(fp,
        "<text class=\"lbl\" x=\"%.2f\" y=\"%d\" text-anchor=\"middle\">"
        "$0</text>\n", zx, WM_BT_HIST_H - 8);
  }

  fputs("</svg>\n", fp);
}

// WM-BT-RPT-6: one-line plain-English definition for a metric key, used as
// a `title=` hover tooltip on table headers + card labels across both index
// surfaces. Strings are ASCII and free of `"`/`<`/`&` so they embed safely
// in an HTML attribute without escaping. Returns "" for an unknown key
// (callers suppress the title attribute when the def is empty).
static const char *
wm_bt_metric_def(const char *key)
{
  static const struct { const char *k; const char *d; } defs[] = {
    { "equity",       "Account equity at the end: cash plus the marked"
                      " value of any open position." },
    { "realized_pnl", "Realized profit/loss: net result of all closed"
                      " round-trip trades." },
    { "pf",           "Profit factor: gross profit / gross loss. Above 1"
                      " is profitable." },
    { "win_rate",     "Win rate: share of round trips that closed in"
                      " profit." },
    { "round_trips",  "Round trips: completed buy-then-sell trades." },
    { "trades",       "Round trips: completed buy-then-sell trades." },
    { "max_dd",       "Maximum drawdown: largest peak-to-trough equity"
                      " drop, as a percent." },
    { "sharpe",       "Sharpe ratio: return per unit of total volatility."
                      " Higher is better." },
    { "sortino",      "Sortino ratio: return per unit of downside"
                      " volatility. Higher is better." },
    { "score",        "Ranking score for this run (the metric chosen with"
                      " --rank-by)." },
    { "iter",         "Iteration index within the sweep." },
    { "avg_win",      "Average win: mean profit across winning trades." },
    { "avg_loss",     "Average loss: mean loss across losing trades." },
    { "payoff",       "Payoff ratio: average win / average loss." },
    { "expectancy",   "Expectancy: average profit/loss per round trip." },
    { "avg_hold",     "Average hold: mean time a position stayed open." },
    { "best",         "Best trade: largest single-trade profit." },
    { "worst",        "Worst trade: largest single-trade loss." },
    { "gross",        "Gross profit and gross loss across all closed"
                      " trades." },
    { "held",         "Holding time from the entry fill to the exit"
                      " fill." },
    { "pl",           "Profit/loss on this trade, net of fees and"
                      " slippage." },
    { "pl_pct",       "Profit/loss on this trade as a percent of the entry"
                      " value." },
    { "balance",      "Account balance after this trade closed." },
    { "entry_px",     "Fill price when the position was opened." },
    { "exit_px",      "Fill price when the position was closed." },
    { "qty",          "Position size for this trade, in base units." },
  };
  size_t i;

  if(key == NULL)
    return("");

  for(i = 0; i < sizeof(defs) / sizeof(defs[0]); i++)
    if(strcmp(defs[i].k, key) == 0)
      return(defs[i].d);

  return("");
}

// WM-BT-RPT-6: emit one headline / analytics stat card. `def_key` (NULL or
// unknown ⇒ no tooltip) adds a hover definition on the label; `vcls` is the
// extra value class ("pos"/"neg"/""); `value_html` is already formatted +
// safe. Dedups the per-card fprintf shape shared by both index surfaces.
static void
wm_bt_emit_card(FILE *fp, const char *label, const char *def_key,
    const char *vcls, const char *value_html)
{
  const char *def = wm_bt_metric_def(def_key);
  const char *sep = (vcls != NULL && vcls[0] != '\0') ? " " : "";

  if(vcls == NULL)
    vcls = "";

  if(def[0] != '\0')
    fprintf(fp,
        "<div class=\"card\"><div class=\"k\" title=\"%s\">%s</div>"
        "<div class=\"v mono%s%s\">%s</div></div>\n",
        def, label, sep, vcls, value_html);
  else
    fprintf(fp,
        "<div class=\"card\"><div class=\"k\">%s</div>"
        "<div class=\"v mono%s%s\">%s</div></div>\n",
        label, sep, vcls, value_html);
}

// WM-BT-RPT-6: plain-English verdict banner at the very top of both index
// surfaces. `best` is the rank-1 config's PAPER stats; `final_equity` is its
// end-of-run equity (wm_bt_compute_equity), `start_cash` the opening
// balance, `sharpe` the rank-1 Sharpe. Emits a PROFITABLE / UNPROFITABLE
// pill + a two-line takeaway derived from the stats (active voice, second
// person, numerals — per the Web Interface Guidelines content rules).
// No-op on NULL fp / stats.
static void
wm_bt_emit_verdict(FILE *fp, const wm_market_stats_t *best,
    double start_cash, double sharpe, double final_equity)
{
  uint32_t rt;
  double   wr;
  double   pf;
  double   ret;
  bool     win;
  char     pf_str[32];
  char     wr_str[32];
  char     dd_str[32];
  char     sh_str[32];

  if(fp == NULL || best == NULL)
    return;

  rt  = best->n_wins + best->n_losses;
  wr  = rt > 0 ? (double)best->n_wins / (double)rt * 100.0 : 0.0;
  pf  = wm_market_stats_profit_factor(best);
  ret = (isfinite(final_equity) && start_cash > 0.0)
      ? (final_equity - start_cash) / start_cash * 100.0 : 0.0;
  win = isfinite(final_equity) && final_equity > start_cash;

  if(rt == 0)
  {
    fputs("<div class=\"verdict loss\"><span class=\"pill\">No trades"
          "</span><div class=\"takeaway\"><b>This configuration made no"
          " completed round trips over the tested range.</b>"
          "<span class=\"sub2\">There is nothing to evaluate &mdash; widen"
          " the date range or loosen the entry rules.</span></div></div>\n",
        fp);
    return;
  }

  wm_bt_fmt_pct(wr, 0, wr_str, sizeof(wr_str));
  wm_bt_fmt_pct(best->max_drawdown * 100.0, 0, dd_str, sizeof(dd_str));
  wm_bt_fmt_num(pf, 2, pf_str, sizeof(pf_str));
  wm_bt_fmt_num(isfinite(sharpe) ? sharpe : 0.0, 2, sh_str, sizeof(sh_str));

  fprintf(fp,
      "<div class=\"verdict %s\"><span class=\"pill\">%s</span>"
      "<div class=\"takeaway\"><b>This configuration %s %s%.1f%% net.</b>"
      "<span class=\"sub2\">Profit factor %s &middot; %s win rate &middot;"
      " %s maximum drawdown &middot; Sharpe %s over %u round trips."
      "</span></div></div>\n",
      win ? "win" : "loss",
      win ? "Profitable" : "Unprofitable",
      win ? "returned" : "lost",
      win ? "+" : "",
      win ? ret : -ret,
      pf_str, wr_str, dd_str, sh_str, rt);
}

// Emit the stat-card row + P/L distribution for one config's closed
// trades (WM-BT-RPT-3). No-op visual when there are no closed trades.
static void
wm_bt_idx_emit_trade_analytics(FILE *fp, const wm_bt_sweep_result_t *r)
{
  wm_bt_trade_stats_t s;
  char                avg_win_str[48];
  char                avg_loss_str[48];
  char                best_str[48];
  char                worst_str[48];
  char                exp_str[48];
  char                payoff_str[32];
  char                hold_str[32];

  if(fp == NULL || r->fills == NULL || r->n_fills == 0)
    return;

  wm_bt_trade_stats_compute(r->fills, r->n_fills, &s);

  if(s.n == 0)
    return;                        // only open trades — nothing to derive

  fputs("<h3 class=\"sub-h\">P/L distribution</h3>\n", fp);
  wm_bt_emit_pnl_histogram_svg(fp, r->fills, r->n_fills);

  wm_bt_fmt_usd(s.avg_win,    avg_win_str,  sizeof(avg_win_str));
  wm_bt_fmt_usd(s.avg_loss,   avg_loss_str, sizeof(avg_loss_str));
  wm_bt_fmt_usd(s.best,       best_str,     sizeof(best_str));
  wm_bt_fmt_usd(s.worst,      worst_str,    sizeof(worst_str));
  wm_bt_fmt_usd(s.expectancy, exp_str,      sizeof(exp_str));
  wm_bt_fmt_num(s.payoff, 2, payoff_str, sizeof(payoff_str));
  wm_bt_idx_fmt_dur(s.avg_hold_ms, hold_str, sizeof(hold_str));

  fputs("<div class=\"cards\">\n", fp);
  wm_bt_emit_card(fp, "Avg win",      "avg_win",    "pos", avg_win_str);
  wm_bt_emit_card(fp, "Avg loss",     "avg_loss",   "neg", avg_loss_str);
  wm_bt_emit_card(fp, "Payoff ratio", "payoff",     "",    payoff_str);
  wm_bt_emit_card(fp, "Expectancy",   "expectancy", "",    exp_str);
  wm_bt_emit_card(fp, "Avg hold",     "avg_hold",   "",    hold_str);
  wm_bt_emit_card(fp, "Best",         "best",       "pos", best_str);
  wm_bt_emit_card(fp, "Worst",        "worst",      "neg", worst_str);
  fputs("</div>\n", fp);
}

// One iteration's trade table. Pairs buy→sell via wm_bt_trade_next
// (long-only / flat ⇒ strict buy/sell alternation, but defensive
// against an unmatched trailing buy = open-at-end). Rows carry
// `data-cls` (win/loss/open) + `data-pnl` and the `<th>`s are
// click-to-sort (report.js wmSortTable); both are progressive
// enhancement — the server-rendered order + full row set survive with
// JS off. Returns the number of paired trades rendered.
static uint32_t
wm_bt_idx_emit_trades(FILE *fp, const char *sweep_dir,
    const wm_bt_sweep_result_t *r, uint32_t rank, uint16_t emit_mask)
{
  wm_bt_trade_iter_t      it = { r->fills, r->n_fills, 0 };
  const wm_market_fill_t *entry;
  const wm_market_fill_t *exit_fill;
  uint32_t                tidx = 0;

  // Sortable headers: data-col is the 0-based column index, data-type
  // the sort comparator (num|text). The held column sorts on each row's
  // data-v (milliseconds) since its text ("2h 5m") isn't numerically
  // parseable. report.js wires the click/keyboard handlers + arrow.
  fputs("<table class=\"trades\"><thead><tr>"
        "<th aria-sort=\"none\"><button type=\"button\" class=\"sort\""
        " data-col=\"0\" data-type=\"num\">#<span class=\"arrow\""
        " aria-hidden=\"true\"></span></button></th>"
        "<th aria-sort=\"none\"><button type=\"button\" class=\"sort\""
        " data-col=\"1\" data-type=\"text\">entry (UTC)<span class=\"arrow\""
        " aria-hidden=\"true\"></span></button></th>"
        "<th aria-sort=\"none\"><button type=\"button\" class=\"sort\""
        " data-col=\"2\" data-type=\"text\">exit (UTC)<span class=\"arrow\""
        " aria-hidden=\"true\"></span></button></th>"
        "<th aria-sort=\"none\" title=\"Holding time from the entry fill"
        " to the exit fill.\"><button type=\"button\" class=\"sort\""
        " data-col=\"3\" data-type=\"num\">held<span class=\"arrow\""
        " aria-hidden=\"true\"></span></button></th>"
        "<th class=\"num\" aria-sort=\"none\" title=\"Fill price when the"
        " position was opened.\"><button type=\"button\""
        " class=\"sort\" data-col=\"4\" data-type=\"num\">entry"
        "<span class=\"arrow\" aria-hidden=\"true\"></span></button></th>"
        "<th class=\"num\" aria-sort=\"none\" title=\"Fill price when the"
        " position was closed.\"><button type=\"button\""
        " class=\"sort\" data-col=\"5\" data-type=\"num\">exit"
        "<span class=\"arrow\" aria-hidden=\"true\"></span></button></th>"
        "<th class=\"num\" aria-sort=\"none\" title=\"Position size for"
        " this trade, in base units.\"><button type=\"button\""
        " class=\"sort\" data-col=\"6\" data-type=\"num\">qty"
        "<span class=\"arrow\" aria-hidden=\"true\"></span></button></th>"
        "<th class=\"num\" aria-sort=\"none\" title=\"Profit/loss on this"
        " trade, net of fees and slippage.\"><button type=\"button\""
        " class=\"sort\" data-col=\"7\" data-type=\"num\">P/L"
        "<span class=\"arrow\" aria-hidden=\"true\"></span></button></th>"
        "<th class=\"num\" aria-sort=\"none\" title=\"Profit/loss on this"
        " trade as a percent of the entry value.\"><button type=\"button\""
        " class=\"sort\" data-col=\"8\" data-type=\"num\">P/L %"
        "<span class=\"arrow\" aria-hidden=\"true\"></span></button></th>"
        "<th class=\"num\" aria-sort=\"none\" title=\"Account balance after"
        " this trade closed.\"><button type=\"button\""
        " class=\"sort\" data-col=\"9\" data-type=\"num\">balance"
        "<span class=\"arrow\" aria-hidden=\"true\"></span></button></th>"
        "<th>exit reason</th><th>chart</th></tr></thead><tbody>\n", fp);

  while(wm_bt_trade_next(&it, &entry, &exit_fill))
  {
    char                    entry_ts[64];
    char                    exit_ts[64];
    char                    held[32];
    char                    reason_esc[128];
    double                  pnl;
    double                  notional;
    double                  pct;
    int64_t                 hold_ms;
    const char             *cls;

    tidx++;

    wm_bt_idx_fmt_ts(entry->ts_ms, entry_ts, sizeof(entry_ts));
    notional = entry->price * entry->qty;

    if(exit_fill != NULL)
    {
      pnl     = exit_fill->realized_pnl;
      pct     = (notional > 0.0) ? (pnl / notional) * 100.0 : 0.0;
      hold_ms = exit_fill->ts_ms - entry->ts_ms;
      cls     = (pnl > 0.0) ? "win" : (pnl < 0.0) ? "loss" : "open";

      wm_bt_idx_fmt_ts(exit_fill->ts_ms, exit_ts, sizeof(exit_ts));
      wm_bt_idx_fmt_dur(hold_ms, held, sizeof(held));
      wm_bt_html_escape(exit_fill->reason, reason_esc,
          sizeof(reason_esc));
    }

    else
    {
      pnl     = 0.0;
      pct     = 0.0;
      hold_ms = 0;
      cls     = "open";
      snprintf(exit_ts, sizeof(exit_ts), "(open)");
      snprintf(held,    sizeof(held),    "&mdash;");
      wm_bt_html_escape(entry->reason, reason_esc, sizeof(reason_esc));
    }

    if(exit_fill != NULL)
      fprintf(fp, "<tr class=\"%s\" data-cls=\"%s\" data-pnl=\"%.4f\">",
          cls, cls, pnl);
    else
      fprintf(fp, "<tr class=\"open\" data-cls=\"open\" data-pnl=\"\">");

    fprintf(fp,
        "<td>%u</td><td>%s</td><td>%s</td>"
        "<td data-v=\"%" PRId64 "\">%s</td>"
        "<td class=\"num\">%.2f</td>",
        tidx, entry_ts, exit_ts, hold_ms, held, entry->price);

    if(exit_fill != NULL)
      fprintf(fp, "<td class=\"num\">%.2f</td>", exit_fill->price);
    else
      fputs("<td class=\"num muted\">&mdash;</td>", fp);

    fprintf(fp, "<td class=\"num\">%.6f</td>", entry->qty);

    if(exit_fill != NULL)
    {
      fprintf(fp,
          "<td class=\"num pnl\">%+.2f</td>"
          "<td class=\"num pnl\">%+.2f%%</td>"
          "<td class=\"num\">%.2f</td>",
          pnl, pct, exit_fill->cash_after);
    }

    else
    {
      fputs("<td class=\"num muted\">&mdash;</td>"
            "<td class=\"num muted\">&mdash;</td>"
            "<td class=\"num muted\">&mdash;</td>", fp);
    }

    fprintf(fp, "<td class=\"reason\">%s</td><td>", reason_esc);
    wm_bt_idx_emit_chart_links(fp, sweep_dir, rank, tidx, emit_mask);
    fputs("</td></tr>\n", fp);
  }

  fputs("</tbody></table>\n", fp);

  return(tidx);
}

// Emit the equity/drawdown series as a JSON island that wmRenderEquity
// (report.js) reads. Time is epoch SECONDS — Lightweight Charts' unit —
// and must be strictly ascending + unique, so points sharing a second are
// nudged forward by 1s (display-only; equity/dd values untouched). The
// builder already drops non-finite equities; the isfinite guard here is
// the last gate before the chart library, which voids a whole series on a
// single NaN.
static void
wm_bt_emit_equity_json(FILE *fp, const wm_bt_equity_point_t *pts, uint32_t n)
{
  int64_t  prev_sec = INT64_MIN;
  bool     first    = true;
  uint32_t i;

  if(fp == NULL || pts == NULL)
    return;

  fputs("<script type=\"application/json\" id=\"eq-data\">[", fp);

  for(i = 0; i < n; i++)
  {
    int64_t sec;

    if(!isfinite(pts[i].equity) || !isfinite(pts[i].dd_pct))
      continue;

    sec = pts[i].t_ms / 1000;

    if(sec <= prev_sec)
      sec = prev_sec + 1;

    prev_sec = sec;

    fprintf(fp, "%s{\"t\":%" PRId64 ",\"e\":%.4f,\"dd\":%.4f}",
        first ? "" : ",", sec, pts[i].equity, pts[i].dd_pct);

    first = false;
  }

  fputs("]</script>\n", fp);
}

// The shared <head> + design-system stylesheet now live in
// wm_bt_assets.c; the index links them by relative path, pulls in the
// Lightweight Charts CDN build (deferred, ahead of report.js so it is
// defined when the equity renderer runs), and the shared client bootstrap
// (report.js).
static void
wm_bt_idx_write_head(FILE *fp, const char *title_esc)
{
  wm_bt_html_doc_open(fp, title_esc, "assets/report.css");
  fprintf(fp, "<script defer src=\"%s\"></script>\n", WM_BT_CHART_LIB_URL);
  fputs("<script defer src=\"assets/report.js\"></script>\n", fp);
}

bool
wm_bt_render_index_html(const char *sweep_dir,
    const char *sweep_id, const char *wm_path,
    const wm_backtest_snapshot_t *snap, const char *strategy,
    const wm_bt_sweep_plan_t *plan, const wm_bt_sweep_mode_t *mode,
    const wm_backtest_params_t *fixed_params,
    const wm_bt_sweep_result_t *results, uint32_t n_results,
    uint32_t n_ok, uint32_t n_fail, uint64_t wallclock_ms,
    char *err, size_t err_cap)
{
  char              path[1024];
  char              tmp[1280];
  char              esc[256];
  char              esc2[256];
  uint32_t         *indices = NULL;
  uint32_t          top_k;
  uint32_t          cap_kv;
  uint32_t          top_n;
  uint16_t          emit_mask;
  uint32_t          g;
  FILE             *fp = NULL;
  wm_bt_run_mode_t  mode_val;
  int               n;
  uint32_t          i;

  if(sweep_dir == NULL || sweep_id == NULL || snap == NULL ||
     strategy == NULL || plan == NULL || results == NULL ||
     n_results == 0)
  {
    if(err != NULL)
      snprintf(err, err_cap, "index_html: bad args");
    return(FAIL);
  }

  mode_val = (mode != NULL) ? mode->mode : WM_BT_MODE_FULL;

  // Mirror the chart pass exactly so every link resolves: cap top-K by
  // charts_top_n, and chart-link every grain the snapshot carries
  // (grain_n[g] > 0) rather than just the strategy's subscribed grains.
  cap_kv = (uint32_t)kv_get_uint("plugin.whenmoon.backtest.charts_top_n");
  top_n  = plan->top_k;

  if(cap_kv > 0 && top_n > cap_kv)
    top_n = cap_kv;

  emit_mask = 0;

  for(g = 0; g < WM_GRAN_MAX; g++)
    if(snap->mkt.grain_n[g] > 0)
      emit_mask |= (uint16_t)(1u << g);

  n = snprintf(path, sizeof(path), "%s/index.html", sweep_dir);

  if(n < 0 || (size_t)n >= sizeof(path))
  {
    if(err != NULL)
      snprintf(err, err_cap, "index.html path overflow");
    return(FAIL);
  }

  n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);

  if(n < 0 || (size_t)n >= sizeof(tmp))
  {
    if(err != NULL)
      snprintf(err, err_cap, "index.html tmp path overflow");
    return(FAIL);
  }

  indices = mem_alloc(WM_BT_REPORT_CTX, "index_indices",
      sizeof(*indices) * (size_t)n_results);

  if(indices == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "index_indices alloc failed");
    return(FAIL);
  }

  top_k = wm_bt_topk_compute(results, n_results, top_n, indices);

  fp = fopen(tmp, "w");

  if(fp == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "fopen('%s') failed: %s", tmp, strerror(errno));
    mem_free(indices);
    return(FAIL);
  }

  // ---- head + sticky header ----
  wm_bt_html_escape(sweep_id, esc, sizeof(esc));
  wm_bt_idx_write_head(fp, esc);

  wm_bt_html_escape(strategy, esc, sizeof(esc));
  wm_bt_html_escape(snap->source_market_id, esc2, sizeof(esc2));
  fprintf(fp,
      "<header><h1>%s &middot; %s</h1>"
      "<div class=\"sub\">%s &rarr; %s &middot; %u 1m bars &middot;"
      " mode <b>%s</b> &middot; ranked by <b>%s</b></div></header>\n",
      esc, esc2, snap->range_start, snap->range_end,
      snap->bars_loaded_1m, wm_bt_report_mode_str(mode_val),
      wm_bt_sweep_score_name(plan->score));

  fputs("<div class=\"wrap\" id=\"main\">\n", fp);

  // ---- verdict banner + headline cards (best config = top-K rank 1) ----
  if(top_k > 0 && results[indices[0]].ok)
  {
    const wm_bt_sweep_result_t *best = &results[indices[0]];
    const wm_market_stats_t    *st   = &best->trade.stats[WM_MARKET_MODE_PAPER];
    uint32_t rt   = st->n_wins + st->n_losses;
    double   wr   = rt > 0 ? (double)st->n_wins / (double)rt * 100.0 : 0.0;
    double   pf   = wm_market_stats_profit_factor(st);
    double   eq   = wm_bt_compute_equity(&best->trade);
    double   rpnl = st->realized_pnl_lifetime;
    double   start_cash =
        (fixed_params != NULL && fixed_params->have_starting_cash)
        ? fixed_params->starting_cash : WM_MARKET_DEFAULT_STARTING_CASH;
    const char *ec = eq >= WM_MARKET_DEFAULT_STARTING_CASH ? "pos" : "neg";
    const char *rc = rpnl >= 0.0 ? "pos" : "neg";
    char     eq_str[48];
    char     rpnl_str[48];
    char     wr_str[32];
    char     dd_str[32];
    char     pf_str[32];
    char     rt_str[32];

    wm_bt_emit_verdict(fp, st, start_cash, best->trade.sharpe, eq);

    wm_bt_fmt_usd(eq,   eq_str,   sizeof(eq_str));
    wm_bt_fmt_usd(rpnl, rpnl_str, sizeof(rpnl_str));
    wm_bt_fmt_pct(wr, 1, wr_str, sizeof(wr_str));
    wm_bt_fmt_pct(st->max_drawdown * 100.0, 1, dd_str, sizeof(dd_str));
    wm_bt_fmt_num(pf, 2, pf_str, sizeof(pf_str));
    snprintf(rt_str, sizeof(rt_str), "%u", rt);

    fputs("<div class=\"cards\">\n", fp);
    wm_bt_emit_card(fp, "Best equity",   "equity",       ec, eq_str);
    wm_bt_emit_card(fp, "Realized P/L",  "realized_pnl", rc, rpnl_str);
    wm_bt_emit_card(fp, "Profit factor", "pf",           "", pf_str);
    wm_bt_emit_card(fp, "Win rate",      "win_rate",     "", wr_str);
    wm_bt_emit_card(fp, "Round trips",   "round_trips",  "", rt_str);
    wm_bt_emit_card(fp, "Max drawdown",  "max_dd",       "", dd_str);
    fputs("</div>\n", fp);
  }

  // ---- equity & drawdown (best config, reconstructed from fills) ----
  if(top_k > 0 && results[indices[0]].ok)
  {
    const wm_bt_sweep_result_t *best = &results[indices[0]];
    double start_cash =
        (fixed_params != NULL && fixed_params->have_starting_cash)
        ? fixed_params->starting_cash
        : WM_MARKET_DEFAULT_STARTING_CASH;

    fputs("<section><h2>Equity &amp; drawdown</h2>\n", fp);

    if(best->n_fills == 0 || best->fills == NULL)
    {
      fputs("<p class=\"note\">No trades to plot.</p>\n", fp);
    }
    else
    {
      wm_bt_equity_point_t *pts   = NULL;
      uint32_t              n_pts = 0;
      char                  eqerr[160];
      bool                  built;

      built = wm_bt_equity_series_build(best->fills, best->n_fills,
          start_cash, snap->range_start_ms, &pts, &n_pts,
          eqerr, sizeof(eqerr));

      if(built == SUCCESS && n_pts > 0)
      {
        // A trailing buy with no matching sell leaves the position open
        // at snapshot end; flag it so the curve's last leg isn't read as
        // a realized result.
        bool open_end = (best->fills[best->n_fills - 1].side == 'b');

        fputs("<div id=\"eq\" class=\"chartbox\"></div>\n"
              "<div id=\"dd\" class=\"ddbox\"></div>\n", fp);
        fprintf(fp,
            "<p class=\"note\">Per-fill equity (cash + mark&middot;qty at"
            " each fill); intra-trade mark-to-market is not sampled.%s</p>\n",
            open_end ? " Final position still open at snapshot end." : "");
        wm_bt_emit_equity_json(fp, pts, n_pts);
      }
      else
      {
        clam(CLAM_WARN, WM_BT_REPORT_CTX,
            "equity series unavailable: %s",
            eqerr[0] != '\0' ? eqerr : "(empty)");
        fputs("<p class=\"note\">Equity curve unavailable.</p>\n", fp);
      }

      if(pts != NULL)
        mem_free(pts);
    }

    fputs("</section>\n", fp);
  }

  // ---- run metadata ----
  fputs("<section><h2>Run</h2><div class=\"meta\">\n", fp);

  if(wm_path != NULL && wm_path[0] != '\0')
  {
    wm_bt_html_escape(wm_path, esc, sizeof(esc));
    fprintf(fp, "<div><dt>source</dt><div class=\"vv mono\">%s</div></div>\n",
        esc);
  }

  fprintf(fp,
      "<div><dt>iterations</dt><div class=\"vv\">%u total"
      " &middot; %u ok &middot; %u failed</div></div>\n",
      plan->total_iters, n_ok, n_fail);
  fprintf(fp,
      "<div><dt>wallclock</dt><div class=\"vv\">%.3f s</div></div>\n",
      (double)wallclock_ms / 1000.0);

  // Fixed economics — fall through to engine defaults when unset.
  {
    double fee  = (fixed_params != NULL && fixed_params->have_fee_bps)
        ? fixed_params->fee_bps : WM_MARKET_DEFAULT_FEE_BPS;
    double slip = (fixed_params != NULL && fixed_params->have_slip_bps)
        ? fixed_params->slip_bps : WM_MARKET_DEFAULT_SLIP_BPS;
    double sf   = (fixed_params != NULL && fixed_params->have_size_frac)
        ? fixed_params->size_frac : WM_MARKET_DEFAULT_SIZE_FRAC;
    double cash = (fixed_params != NULL && fixed_params->have_starting_cash)
        ? fixed_params->starting_cash : WM_MARKET_DEFAULT_STARTING_CASH;
    char   size_str[32];
    char   cash_str[48];

    wm_bt_fmt_pct(sf * 100.0, 0, size_str, sizeof(size_str));
    wm_bt_fmt_usd(cash, cash_str, sizeof(cash_str));

    fprintf(fp,
        "<div><dt>economics</dt><div class=\"vv mono\">fee %.1f bps"
        " &middot; slip %.1f bps &middot; size %s &middot;"
        " cash %s</div></div>\n",
        fee, slip, size_str, cash_str);
  }

  if(mode_val == WM_BT_MODE_WALK_FORWARD && mode != NULL)
    fprintf(fp,
        "<div><dt>walk-forward</dt><div class=\"vv\">%u windows</div>"
        "</div>\n", mode->walk.n);

  fputs("</div></section>\n", fp);

  // ---- per-config detail (charted top-K) ----
  fprintf(fp,
      "<section><h2>Configurations &amp; trades (top %u)</h2>\n", top_k);

  if(emit_mask == 0)
    fputs("<p class=\"note\">No subscribed grains to chart; trade rows"
          " below have no linked visualizations.</p>\n", fp);

  for(i = 0; i < top_k; i++)
  {
    const wm_bt_sweep_result_t *r = &results[indices[i]];
    const wm_market_stats_t    *st;
    char     params[256];
    uint32_t rt;
    double   wr;
    double   pf;
    double   eq;
    double   rpnl;
    uint32_t n_trades;
    const char *bcls;

    wm_bt_idx_params_str(plan, r->indices, params, sizeof(params));
    wm_bt_html_escape(params, esc, sizeof(esc));

    if(!r->ok)
    {
      fprintf(fp,
          "<details><summary><span class=\"rank\">#%u</span>"
          "<span class=\"params mono\">%s</span><span class=\"spacer\">"
          "</span><span class=\"badge loss\">FAIL</span></summary>"
          "<p class=\"note\">%s</p></details>\n",
          i + 1, esc, r->err[0] != '\0' ? r->err : "(unknown)");
      continue;
    }

    st       = &r->trade.stats[WM_MARKET_MODE_PAPER];
    rt       = st->n_wins + st->n_losses;
    wr       = rt > 0 ? (double)st->n_wins / (double)rt * 100.0 : 0.0;
    pf       = wm_market_stats_profit_factor(st);
    eq       = wm_bt_compute_equity(&r->trade);
    rpnl     = st->realized_pnl_lifetime;
    bcls     = rpnl >= 0.0 ? "win" : "loss";

    fprintf(fp,
        "<details%s><summary><span class=\"rank\">#%u</span>"
        "<span class=\"params mono\">%s</span>"
        "<span class=\"spacer\"></span>"
        "<span class=\"badge\">PF %.2f</span>"
        "<span class=\"badge %s\">%+.0f</span>"
        "<span class=\"badge\">eq $%.0f</span></summary>\n",
        i == 0 ? " open" : "", i + 1, esc, pf, bcls, rpnl, eq);

    // Metric strip.
    fprintf(fp,
        "<div class=\"strip\">"
        "<div class=\"it\"><span title=\"Round trips: completed"
        " buy-then-sell trades.\">round trips</span>%u</div>"
        "<div class=\"it\"><span title=\"Win rate: share of round trips"
        " that closed in profit.\">win rate</span>%.1f%%</div>"
        "<div class=\"it\"><span>wins / losses</span>%u / %u</div>"
        "<div class=\"it\"><span title=\"Gross profit / gross loss across"
        " all closed trades.\">gross +/-</span>%.0f / -%.0f</div>"
        "<div class=\"it\"><span title=\"Maximum drawdown: largest"
        " peak-to-trough equity drop.\">max drawdown</span>%.1f%%</div>"
        "<div class=\"it\"><span title=\"Sharpe ratio: return per unit of"
        " total volatility. Higher is better.\">Sharpe</span>%.3f</div>"
        "<div class=\"it\"><span title=\"Sortino ratio: return per unit of"
        " downside volatility. Higher is better.\">Sortino</span>%.3f</div>"
        "</div>\n",
        rt, wr, st->n_wins, st->n_losses,
        st->gross_profit, st->gross_loss, st->max_drawdown * 100.0,
        r->trade.sharpe, r->trade.sortino);

    if(r->n_fills == 0 || r->fills == NULL)
    {
      fputs("<p class=\"note\">No fills captured for this iteration.</p>\n",
          fp);
    }
    else
    {
      // Fills are lossless for a backtest (the incremental drain captures
      // every PAPER fill, so n_fills == lifetime_fills_count) — no
      // capture-ring truncation note is needed.
      wm_bt_idx_emit_trade_analytics(fp, r);

      // Filter chips (progressive enhancement — inert + keyboard-
      // focusable with JS off; report.js wmFilterTrades wires them).
      fputs("<div class=\"chips\" role=\"group\""
            " aria-label=\"Filter trades\">"
            "<button type=\"button\" class=\"chip\" data-kind=\"all\""
            " aria-pressed=\"true\">All</button>"
            "<button type=\"button\" class=\"chip\" data-kind=\"win\""
            " aria-pressed=\"false\">Wins</button>"
            "<button type=\"button\" class=\"chip\" data-kind=\"loss\""
            " aria-pressed=\"false\">Losses</button>"
            "<button type=\"button\" class=\"chip\" data-kind=\"open\""
            " aria-pressed=\"false\">Open</button></div>\n", fp);

      fputs("<div class=\"tbl-scroll\">\n", fp);
      n_trades = wm_bt_idx_emit_trades(fp, sweep_dir, r, i + 1, emit_mask);
      fputs("</div>\n", fp);

      if(n_trades == 0)
        fputs("<p class=\"note\">No paired trades.</p>\n", fp);
    }

    fputs("</details>\n", fp);
  }

  fputs("</section>\n", fp);

  // ---- footer ----
  fputs("<footer>Generated by whenmoon backtest &middot; "
        "siblings: <a href=\"report.md\">report.md</a>, "
        "<a href=\"manifest.json\">manifest.json</a>, "
        "<a href=\"iterations.jsonl\">iterations.jsonl</a>, "
        "<a href=\"top-N.txt\">top-N.txt</a>. Charts use "
        "Lightweight Charts (loads from CDN; needs network on first "
        "view).</footer>\n", fp);

  fputs("</div></body></html>\n", fp);

  mem_free(indices);

  return(wm_bt_finalize_file(fp, tmp, path, err, err_cap));
}

// ----------------------------------------------------------------------- //
// index.html — sweep dashboard (WM-BT-RPT-5)                              //
// ----------------------------------------------------------------------- //
//
// The counterpart to wm_bt_render_index_html for a parameter sweep
// (total_iters > 1): a chart-free overview built entirely from the
// in-memory results table. Time-series (none here) would need Lightweight
// Charts; the distributions/marginals/heatmap are static inline SVG, so
// the dashboard needs no CDN — only the shared assets/report.css + the
// report.js wmSortTable handler (the top-K table reuses class "trades").

// Format one score value for a cell / label / SVG title. Diverging score
// metrics (realized P/L, equity) read as currency; ratio metrics (pf,
// sharpe, sortino) as a grouped 3-decimal number. NOSCORE → "n/a".
static void
wm_bt_fmt_score(wm_bt_sweep_score_t sc, double v, char *out, size_t cap)
{
  if(WM_BT_SCORE_MISSING(v))
  {
    snprintf(out, cap, "n/a");
    return;
  }

  if(sc == WM_BT_SCORE_REALIZED || sc == WM_BT_SCORE_EQUITY)
    wm_bt_fmt_usd(v, out, cap);
  else
    wm_bt_fmt_num(v, 3, out, cap);
}

// Best (highest) score among ok results whose axis `axis` sits at value
// index `vidx`, with every other axis free. Returns false when no ok,
// finite-scored iteration matches (caller renders the slot as missing).
static bool
wm_bt_marginal_best(const wm_bt_sweep_result_t *results, uint32_t n,
    uint32_t axis, uint32_t vidx, double *out_best, int64_t *out_iter)
{
  double   best = -INFINITY;
  int64_t  iter = -1;
  uint32_t k;

  for(k = 0; k < n; k++)
  {
    double s;

    if(!results[k].ok || results[k].indices[axis] != vidx)
      continue;

    s = results[k].score;

    if(WM_BT_SCORE_MISSING(s))
      continue;

    if(s > best)
    {
      best = s;
      iter = results[k].run_id_db > 0
          ? results[k].run_id_db : (int64_t)(k + 1);
    }
  }

  if(iter < 0)
    return(false);

  *out_best = best;

  if(out_iter != NULL)
    *out_iter = iter;

  return(true);
}

// Best score over the (ax_x == xi, ax_y == yi) slice — the heatmap cell
// value. Other axes free. Returns false on an empty/all-missing cell.
static bool
wm_bt_heat_cell_best(const wm_bt_sweep_result_t *results, uint32_t n,
    uint32_t ax_x, uint32_t xi, uint32_t ax_y, uint32_t yi,
    double *out_best)
{
  double   best = -INFINITY;
  bool     any  = false;
  uint32_t k;

  for(k = 0; k < n; k++)
  {
    double s;

    if(!results[k].ok)
      continue;

    if(results[k].indices[ax_x] != xi || results[k].indices[ax_y] != yi)
      continue;

    s = results[k].score;

    if(WM_BT_SCORE_MISSING(s))
      continue;

    if(s > best)
    {
      best = s;
      any  = true;
    }
  }

  if(!any)
    return(false);

  *out_best = best;
  return(true);
}

// Per-axis marginal-best bar chart: one zero-baselined bar per swept
// value, height proportional to that value's best score, win/loss colored
// by sign. Inline SVG, no JS. Reuses the .hist design-system classes.
static void
wm_bt_emit_marginal_svg(FILE *fp, const wm_bt_sweep_plan_t *plan,
    const wm_bt_sweep_result_t *results, uint32_t n, uint32_t axis_idx)
{
  const wm_bt_sweep_axis_t *axis = &plan->axes[axis_idx];
  const char               *sname = wm_bt_sweep_score_name(plan->score);
  double    best[WM_BT_SWEEP_MAX_VALUES];
  bool      have[WM_BT_SWEEP_MAX_VALUES];
  double    ymin   = 0.0;          // zero is always in range (baseline)
  double    ymax   = 0.0;
  double    span;
  double    plot_w;
  double    plot_h;
  double    bar_w;
  double    zero_y;
  bool      any    = false;
  uint32_t  nv     = axis->n_values;
  uint32_t  step;
  uint32_t  v;
  char      aname_esc[64];

  if(fp == NULL || nv == 0)
    return;

  if(nv > WM_BT_SWEEP_MAX_VALUES)
    nv = WM_BT_SWEEP_MAX_VALUES;

  for(v = 0; v < nv; v++)
  {
    have[v] = wm_bt_marginal_best(results, n, axis_idx, v, &best[v], NULL);

    if(have[v])
    {
      any = true;
      if(best[v] < ymin) ymin = best[v];
      if(best[v] > ymax) ymax = best[v];
    }
  }

  if(!any)
  {
    fputs("<p class=\"note\">No scored iterations on this axis.</p>\n", fp);
    return;
  }

  span = ymax - ymin;

  if(span <= 0.0)
    span = (ymax != 0.0) ? fabs(ymax) : 1.0;   // degenerate: avoid /0

  plot_w = (double)(WM_BT_MARG_W - 2 * WM_BT_MARG_PADX);
  plot_h = (double)(WM_BT_MARG_H - WM_BT_MARG_PADT - WM_BT_MARG_PADB);
  bar_w  = plot_w / (double)nv;
  zero_y = (double)WM_BT_MARG_PADT + (ymax - 0.0) / span * plot_h;

  step = (nv + WM_BT_MARG_LBL_MAX - 1u) / WM_BT_MARG_LBL_MAX;
  if(step == 0u)
    step = 1u;

  wm_bt_html_escape(axis->name, aname_esc, sizeof(aname_esc));

  fprintf(fp,
      "<svg class=\"hist\" viewBox=\"0 0 %d %d\" role=\"img\""
      " aria-label=\"Best %s by %s across %u swept values\""
      " preserveAspectRatio=\"none\">\n"
      "<title>Marginal best %s per %s value</title>\n",
      WM_BT_MARG_W, WM_BT_MARG_H, sname, aname_esc, nv, sname, aname_esc);

  for(v = 0; v < nv; v++)
  {
    double      x = (double)WM_BT_MARG_PADX + (double)v * bar_w;
    double      sy;
    double      top;
    double      h;
    const char *cls;
    char        vcell[64];
    char        scell[48];

    wm_bt_md_render_axis_cell(axis, axis->values[v], vcell, sizeof(vcell));

    if(have[v])
    {
      sy  = (double)WM_BT_MARG_PADT + (ymax - best[v]) / span * plot_h;
      cls = best[v] >= 0.0 ? "win" : "loss";
      top = best[v] >= 0.0 ? sy : zero_y;
      h   = fabs(sy - zero_y);

      if(h < 0.5)
        h = 0.5;                   // keep a near-zero bar visible

      wm_bt_fmt_score(plan->score, best[v], scell, sizeof(scell));
      fprintf(fp,
          "<rect class=\"%s\" x=\"%.2f\" y=\"%.2f\" width=\"%.2f\""
          " height=\"%.2f\"><title>%s = %s: best %s</title></rect>\n",
          cls, x + 0.5, top, bar_w > 1.0 ? bar_w - 1.0 : bar_w, h,
          aname_esc, vcell, scell);
    }

    if(v % step == 0u)
      fprintf(fp,
          "<text class=\"lbl\" x=\"%.2f\" y=\"%d\" text-anchor=\"middle\">"
          "%s</text>\n",
          x + bar_w / 2.0, WM_BT_MARG_H - 8, vcell);
  }

  fprintf(fp,
      "<line class=\"axis\" x1=\"%d\" y1=\"%.2f\" x2=\"%d\" y2=\"%.2f\"/>\n",
      WM_BT_MARG_PADX, zero_y, WM_BT_MARG_W - WM_BT_MARG_PADX, zero_y);

  fputs("</svg>\n", fp);
}

// Pick the two widest axes (most values) for the heatmap. ax_x is the
// widest, ax_y the next-widest distinct axis. Stable on ties (lowest
// index wins). Returns false when fewer than two axes exist.
static bool
wm_bt_pick_heatmap_axes(const wm_bt_sweep_plan_t *plan,
    uint32_t *ax_x, uint32_t *ax_y)
{
  uint32_t x;
  uint32_t y;
  uint32_t a;

  if(plan->n_axes < 2)
    return(false);

  x = 0;

  for(a = 1; a < plan->n_axes; a++)
    if(plan->axes[a].n_values > plan->axes[x].n_values)
      x = a;

  y = (x == 0) ? 1u : 0u;

  for(a = 0; a < plan->n_axes; a++)
  {
    if(a == x)
      continue;

    if(plan->axes[a].n_values > plan->axes[y].n_values)
      y = a;
  }

  *ax_x = x;
  *ax_y = y;
  return(true);
}

// 2-axis score heatmap: a color-scaled cell grid over the two widest
// axes, with left (y) + bottom (x) tick gutters and a color legend.
// Diverging metrics (realized/equity) color win above / loss below zero;
// ratio metrics use a sequential accent ramp. Cells are var(--*) fills
// with a magnitude-scaled fill-opacity so color lives in the design
// system. Missing cells are muted. Inline SVG, no JS.
static void
wm_bt_emit_heatmap_svg(FILE *fp, const wm_bt_sweep_plan_t *plan,
    const wm_bt_sweep_result_t *results, uint32_t n,
    uint32_t ax_x, uint32_t ax_y)
{
  const wm_bt_sweep_axis_t *axx = &plan->axes[ax_x];
  const wm_bt_sweep_axis_t *axy = &plan->axes[ax_y];
  const char               *sname = wm_bt_sweep_score_name(plan->score);
  bool      diverging = (plan->score == WM_BT_SCORE_REALIZED ||
                         plan->score == WM_BT_SCORE_EQUITY);
  uint32_t  nx        = axx->n_values;
  uint32_t  ny        = axy->n_values;
  double    gmin      =  INFINITY;
  double    gmax      = -INFINITY;
  bool      any       = false;
  int       w;
  int       h;
  int       grid_w;
  uint32_t  xstep;
  uint32_t  ystep;
  uint32_t  cx;
  uint32_t  cy;
  char      axn_esc[64];
  char      ayn_esc[64];
  char      lo_str[48];
  char      hi_str[48];

  if(fp == NULL)
    return;

  if(nx > WM_BT_HEAT_MAX_VALUES) nx = WM_BT_HEAT_MAX_VALUES;
  if(ny > WM_BT_HEAT_MAX_VALUES) ny = WM_BT_HEAT_MAX_VALUES;

  // Global score range over rendered cells (drives color scaling + legend).
  for(cx = 0; cx < nx; cx++)
    for(cy = 0; cy < ny; cy++)
    {
      double s;

      if(!wm_bt_heat_cell_best(results, n, ax_x, cx, ax_y, cy, &s))
        continue;

      any = true;
      if(s < gmin) gmin = s;
      if(s > gmax) gmax = s;
    }

  if(!any)
  {
    fputs("<p class=\"note\">No scored iterations to map.</p>\n", fp);
    return;
  }

  grid_w = (int)nx * WM_BT_HEAT_CELL;
  w      = WM_BT_HEAT_PADL + grid_w + WM_BT_HEAT_PADR;
  h      = WM_BT_HEAT_PADT + (int)ny * WM_BT_HEAT_CELL + WM_BT_HEAT_PADB;

  xstep = (nx + WM_BT_MARG_LBL_MAX - 1u) / WM_BT_MARG_LBL_MAX;
  ystep = (ny + WM_BT_MARG_LBL_MAX - 1u) / WM_BT_MARG_LBL_MAX;
  if(xstep == 0u) xstep = 1u;
  if(ystep == 0u) ystep = 1u;

  wm_bt_html_escape(axx->name, axn_esc, sizeof(axn_esc));
  wm_bt_html_escape(axy->name, ayn_esc, sizeof(ayn_esc));

  fprintf(fp,
      "<svg class=\"heat\" viewBox=\"0 0 %d %d\" role=\"img\""
      " aria-label=\"Best %s heatmap over %s (x) by %s (y)\">\n"
      "<title>Best %s by %s and %s</title>\n",
      w, h, sname, axn_esc, ayn_esc, sname, axn_esc, ayn_esc);

  // Legend gradient definition.
  if(diverging)
    fputs("<defs><linearGradient id=\"wmHeat\">"
          "<stop offset=\"0%\" stop-color=\"var(--loss)\"/>"
          "<stop offset=\"50%\" stop-color=\"var(--surface2)\"/>"
          "<stop offset=\"100%\" stop-color=\"var(--win)\"/>"
          "</linearGradient></defs>\n", fp);
  else
    fputs("<defs><linearGradient id=\"wmHeat\">"
          "<stop offset=\"0%\" stop-color=\"var(--accent)\""
          " stop-opacity=\"0.12\"/>"
          "<stop offset=\"100%\" stop-color=\"var(--accent)\""
          " stop-opacity=\"1\"/></linearGradient></defs>\n", fp);

  // Cells (y value 0 at the bottom row).
  for(cy = 0; cy < ny; cy++)
  {
    int ry = WM_BT_HEAT_PADT + (int)(ny - 1u - cy) * WM_BT_HEAT_CELL;

    for(cx = 0; cx < nx; cx++)
    {
      int    rx = WM_BT_HEAT_PADL + (int)cx * WM_BT_HEAT_CELL;
      double s;
      char   xcell[64];
      char   ycell[64];
      char   scell[48];

      wm_bt_md_render_axis_cell(axx, axx->values[cx], xcell, sizeof(xcell));
      wm_bt_md_render_axis_cell(axy, axy->values[cy], ycell, sizeof(ycell));

      if(!wm_bt_heat_cell_best(results, n, ax_x, cx, ax_y, cy, &s))
      {
        fprintf(fp,
            "<rect class=\"miss\" x=\"%d\" y=\"%d\" width=\"%d\""
            " height=\"%d\"><title>%s=%s, %s=%s: no iteration</title>"
            "</rect>\n",
            rx, ry, WM_BT_HEAT_CELL - 1, WM_BT_HEAT_CELL - 1,
            axn_esc, xcell, ayn_esc, ycell);
        continue;
      }

      wm_bt_fmt_score(plan->score, s, scell, sizeof(scell));

      if(diverging)
      {
        const char *fill = s >= 0.0 ? "var(--win)" : "var(--loss)";
        double      mag;

        if(s >= 0.0)
          mag = (gmax > 0.0) ? s / gmax : 0.0;
        else
          mag = (gmin < 0.0) ? s / gmin : 0.0;   // both negative → +ratio

        if(mag < 0.12) mag = 0.12;               // floor so small ≠ blank
        if(mag > 1.0)  mag = 1.0;

        fprintf(fp,
            "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\""
            " fill=\"%s\" fill-opacity=\"%.3f\">"
            "<title>%s=%s, %s=%s: %s</title></rect>\n",
            rx, ry, WM_BT_HEAT_CELL - 1, WM_BT_HEAT_CELL - 1, fill, mag,
            axn_esc, xcell, ayn_esc, ycell, scell);
      }
      else
      {
        double range = gmax - gmin;
        double mag   = (range > 0.0) ? (s - gmin) / range : 1.0;

        if(mag < 0.12) mag = 0.12;
        if(mag > 1.0)  mag = 1.0;

        fprintf(fp,
            "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\""
            " fill=\"var(--accent)\" fill-opacity=\"%.3f\">"
            "<title>%s=%s, %s=%s: %s</title></rect>\n",
            rx, ry, WM_BT_HEAT_CELL - 1, WM_BT_HEAT_CELL - 1, mag,
            axn_esc, xcell, ayn_esc, ycell, scell);
      }
    }
  }

  // X tick labels (bottom).
  for(cx = 0; cx < nx; cx += xstep)
  {
    int  lx = WM_BT_HEAT_PADL + (int)cx * WM_BT_HEAT_CELL
        + WM_BT_HEAT_CELL / 2;
    char xcell[64];

    wm_bt_md_render_axis_cell(axx, axx->values[cx], xcell, sizeof(xcell));
    fprintf(fp,
        "<text class=\"lbl\" x=\"%d\" y=\"%d\" text-anchor=\"middle\">"
        "%s</text>\n",
        lx, WM_BT_HEAT_PADT + (int)ny * WM_BT_HEAT_CELL + 15, xcell);
  }

  // Y tick labels (left).
  for(cy = 0; cy < ny; cy += ystep)
  {
    int  ly = WM_BT_HEAT_PADT + (int)(ny - 1u - cy) * WM_BT_HEAT_CELL
        + WM_BT_HEAT_CELL / 2 + 4;
    char ycell[64];

    wm_bt_md_render_axis_cell(axy, axy->values[cy], ycell, sizeof(ycell));
    fprintf(fp,
        "<text class=\"lbl\" x=\"%d\" y=\"%d\" text-anchor=\"end\">%s</text>\n",
        WM_BT_HEAT_PADL - 8, ly, ycell);
  }

  // Color legend bar + min/max (and 0 for diverging) labels.
  {
    int legend_y = WM_BT_HEAT_PADT + (int)ny * WM_BT_HEAT_CELL + 26;
    int legend_w = grid_w < 240 ? grid_w : 240;

    wm_bt_fmt_score(plan->score, gmin, lo_str, sizeof(lo_str));
    wm_bt_fmt_score(plan->score, gmax, hi_str, sizeof(hi_str));

    fprintf(fp,
        "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"10\""
        " fill=\"url(#wmHeat)\" stroke=\"var(--border)\"/>\n",
        WM_BT_HEAT_PADL, legend_y, legend_w);
    fprintf(fp,
        "<text class=\"lbl\" x=\"%d\" y=\"%d\">%s</text>\n",
        WM_BT_HEAT_PADL, legend_y + 22, lo_str);
    fprintf(fp,
        "<text class=\"lbl\" x=\"%d\" y=\"%d\" text-anchor=\"end\">%s</text>\n",
        WM_BT_HEAT_PADL + legend_w, legend_y + 22, hi_str);

    if(diverging && gmin < 0.0 && gmax > 0.0)
      fprintf(fp,
          "<text class=\"lbl\" x=\"%d\" y=\"%d\" text-anchor=\"middle\">0"
          "</text>\n",
          WM_BT_HEAT_PADL + legend_w / 2, legend_y + 22);
  }

  fputs("</svg>\n", fp);
}

// One sortable numeric `<th>` for the sweep top-K table. Mirrors the
// per-trade table header shape (RPT-3) so report.js wmSortTable drives it:
// data-col is the 0-based column index, data-type is always "num" (every
// dashboard column sorts numeric). `label` is already HTML-escaped. `def`
// (WM-BT-RPT-6; NULL/empty ⇒ none) adds a hover-tooltip metric definition —
// the dense ~11-col header keeps short labels but carries the meaning here.
static void
wm_bt_sweep_th(FILE *fp, int col, const char *label, const char *def)
{
  if(def != NULL && def[0] != '\0')
    fprintf(fp,
        "<th class=\"num\" aria-sort=\"none\" title=\"%s\">"
        "<button type=\"button\" class=\"sort\" data-col=\"%d\""
        " data-type=\"num\">%s<span class=\"arrow\" aria-hidden=\"true\">"
        "</span></button></th>",
        def, col, label);
  else
    fprintf(fp,
        "<th class=\"num\" aria-sort=\"none\"><button type=\"button\""
        " class=\"sort\" data-col=\"%d\" data-type=\"num\">%s"
        "<span class=\"arrow\" aria-hidden=\"true\"></span></button></th>",
        col, label);
}

// Emit the sortable top-K table for the sweep dashboard. Reuses class
// "trades" so report.js wmInitTrades wires the click-to-sort headers (no
// filter chips → wmFilterTrades simply never fires). Columns, in order:
// rank, iter, score, <one per axis>, trades, pf, sharpe, sortino, max-dd,
// equity. All columns sort numeric; non-finite / NOSCORE cells carry an
// empty data-v so they fall to the bottom.
static void
wm_bt_emit_sweep_topk_table(FILE *fp, const wm_bt_sweep_plan_t *plan,
    const wm_bt_sweep_result_t *results, const uint32_t *indices,
    uint32_t top_k)
{
  int      col = 0;
  uint32_t a;
  uint32_t i;
  char     aname_esc[64];

  fputs("<table class=\"trades\"><thead><tr>", fp);

  wm_bt_sweep_th(fp, col++, "#",      NULL);
  wm_bt_sweep_th(fp, col++, "iter",   wm_bt_metric_def("iter"));
  wm_bt_sweep_th(fp, col++, "score",  wm_bt_metric_def("score"));

  for(a = 0; a < plan->n_axes; a++)
  {
    wm_bt_html_escape(plan->axes[a].name, aname_esc, sizeof(aname_esc));
    wm_bt_sweep_th(fp, col++, aname_esc, "Swept parameter value for this"
        " configuration.");
  }

  wm_bt_sweep_th(fp, col++, "trades",  wm_bt_metric_def("trades"));
  wm_bt_sweep_th(fp, col++, "pf",      wm_bt_metric_def("pf"));
  wm_bt_sweep_th(fp, col++, "sharpe",  wm_bt_metric_def("sharpe"));
  wm_bt_sweep_th(fp, col++, "sortino", wm_bt_metric_def("sortino"));
  wm_bt_sweep_th(fp, col++, "max dd",  wm_bt_metric_def("max_dd"));
  wm_bt_sweep_th(fp, col++, "equity",  wm_bt_metric_def("equity"));

  fputs("</tr></thead><tbody>\n", fp);

  for(i = 0; i < top_k; i++)
  {
    const wm_bt_sweep_result_t *r = &results[indices[i]];
    const wm_market_stats_t    *st;
    int64_t  iter;
    char     cell[64];

    iter = r->run_id_db > 0 ? r->run_id_db : (int64_t)(indices[i] + 1);

    fprintf(fp, "<tr><td class=\"num\">%u</td>"
        "<td class=\"num\">%" PRId64 "</td>", i + 1, iter);

    // Score.
    if(!r->ok || WM_BT_SCORE_MISSING(r->score))
      fputs("<td class=\"num muted\" data-v=\"\">n/a</td>", fp);
    else
    {
      wm_bt_fmt_score(plan->score, r->score, cell, sizeof(cell));
      fprintf(fp, "<td class=\"num\" data-v=\"%.6f\">%s</td>",
          r->score, cell);
    }

    // Axis values (always known, ok or fail).
    for(a = 0; a < plan->n_axes; a++)
    {
      wm_bt_md_render_axis_cell(&plan->axes[a],
          plan->axes[a].values[r->indices[a]], cell, sizeof(cell));
      fprintf(fp, "<td class=\"num\">%s</td>", cell);
    }

    if(!r->ok)
    {
      // FAIL row: dash the metric columns, keep the column count.
      fputs("<td class=\"num muted\" data-v=\"\">&mdash;</td>"
            "<td class=\"num muted\" data-v=\"\">&mdash;</td>"
            "<td class=\"num muted\" data-v=\"\">&mdash;</td>"
            "<td class=\"num muted\" data-v=\"\">&mdash;</td>"
            "<td class=\"num muted\" data-v=\"\">&mdash;</td>"
            "<td class=\"num muted\" data-v=\"\">FAIL</td></tr>\n", fp);
      continue;
    }

    st = &r->trade.stats[WM_MARKET_MODE_PAPER];

    {
      uint32_t rt   = st->n_wins + st->n_losses;
      double   pf   = wm_market_stats_profit_factor(st);
      double   eq   = wm_bt_compute_equity(&r->trade);
      char     pf_s[32];
      char     sh_s[32];
      char     so_s[32];
      char     dd_s[32];
      char     eq_s[48];

      wm_bt_md_fmt_double(pf,             "%.3f", pf_s, sizeof(pf_s));
      wm_bt_md_fmt_double(r->trade.sharpe,  "%.3f", sh_s, sizeof(sh_s));
      wm_bt_md_fmt_double(r->trade.sortino, "%.3f", so_s, sizeof(so_s));
      wm_bt_fmt_pct(st->max_drawdown * 100.0, 1, dd_s, sizeof(dd_s));
      wm_bt_fmt_usd(eq, eq_s, sizeof(eq_s));

      fprintf(fp,
          "<td class=\"num\">%u</td>"
          "<td class=\"num\">%s</td>"
          "<td class=\"num\">%s</td>"
          "<td class=\"num\">%s</td>"
          "<td class=\"num\">%s</td>"
          "<td class=\"num\" data-v=\"%.4f\">%s</td></tr>\n",
          rt, pf_s, sh_s, so_s, dd_s, eq, eq_s);
    }
  }

  fputs("</tbody></table>\n", fp);
}

bool
wm_bt_render_sweep_html(const char *sweep_dir,
    const char *sweep_id, const char *wm_path,
    const wm_backtest_snapshot_t *snap, const char *strategy,
    const wm_bt_sweep_plan_t *plan, const wm_bt_sweep_mode_t *mode,
    const wm_backtest_params_t *fixed_params,
    const wm_bt_sweep_result_t *results, uint32_t n_results,
    uint32_t n_ok, uint32_t n_fail, uint64_t wallclock_ms,
    char *err, size_t err_cap)
{
  char              path[1024];
  char              tmp[1280];
  char              esc[256];
  char              esc2[256];
  uint32_t         *indices = NULL;
  uint32_t          top_k;
  FILE             *fp = NULL;
  wm_bt_run_mode_t  mode_val;
  int               n;
  uint32_t          a;

  if(sweep_dir == NULL || sweep_id == NULL || snap == NULL ||
     strategy == NULL || plan == NULL || results == NULL ||
     n_results == 0)
  {
    if(err != NULL)
      snprintf(err, err_cap, "sweep_html: bad args");
    return(FAIL);
  }

  mode_val = (mode != NULL) ? mode->mode : WM_BT_MODE_FULL;

  n = snprintf(path, sizeof(path), "%s/index.html", sweep_dir);

  if(n < 0 || (size_t)n >= sizeof(path))
  {
    if(err != NULL)
      snprintf(err, err_cap, "sweep index.html path overflow");
    return(FAIL);
  }

  n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);

  if(n < 0 || (size_t)n >= sizeof(tmp))
  {
    if(err != NULL)
      snprintf(err, err_cap, "sweep index.html tmp path overflow");
    return(FAIL);
  }

  indices = mem_alloc(WM_BT_REPORT_CTX, "sweep_indices",
      sizeof(*indices) * (size_t)n_results);

  if(indices == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "sweep_indices alloc failed");
    return(FAIL);
  }

  top_k = wm_bt_topk_compute(results, n_results, plan->top_k, indices);

  fp = fopen(tmp, "w");

  if(fp == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "fopen('%s') failed: %s", tmp, strerror(errno));
    mem_free(indices);
    return(FAIL);
  }

  // ---- head + header ----
  wm_bt_html_escape(sweep_id, esc, sizeof(esc));
  wm_bt_idx_write_head(fp, esc);

  wm_bt_html_escape(strategy, esc, sizeof(esc));
  wm_bt_html_escape(snap->source_market_id, esc2, sizeof(esc2));
  fprintf(fp,
      "<header><h1>%s &middot; %s</h1>"
      "<div class=\"sub\">%s &rarr; %s &middot; %u 1m bars &middot;"
      " mode <b>%s</b> &middot; %u iterations &middot; ranked by"
      " <b>%s</b></div></header>\n",
      esc, esc2, snap->range_start, snap->range_end,
      snap->bars_loaded_1m, wm_bt_report_mode_str(mode_val),
      plan->total_iters, wm_bt_sweep_score_name(plan->score));

  fputs("<div class=\"wrap\" id=\"main\">\n", fp);

  // ---- verdict banner + headline cards (rank-1 config) ----
  if(top_k > 0 && results[indices[0]].ok)
  {
    const wm_bt_sweep_result_t *best = &results[indices[0]];
    const wm_market_stats_t    *st   =
        &best->trade.stats[WM_MARKET_MODE_PAPER];
    uint32_t    rt   = st->n_wins + st->n_losses;
    double      wr   = rt > 0 ? (double)st->n_wins / (double)rt * 100.0 : 0.0;
    double      pf   = wm_market_stats_profit_factor(st);
    double      eq   = wm_bt_compute_equity(&best->trade);
    double      rpnl = st->realized_pnl_lifetime;
    double      start_cash =
        (fixed_params != NULL && fixed_params->have_starting_cash)
        ? fixed_params->starting_cash : WM_MARKET_DEFAULT_STARTING_CASH;
    const char *ec   = eq >= WM_MARKET_DEFAULT_STARTING_CASH ? "pos" : "neg";
    const char *rc   = rpnl >= 0.0 ? "pos" : "neg";
    char        eq_str[48];
    char        rpnl_str[48];
    char        wr_str[32];
    char        dd_str[32];
    char        pf_str[32];
    char        rt_str[32];
    char        params[256];

    wm_bt_emit_verdict(fp, st, start_cash, best->trade.sharpe, eq);

    wm_bt_idx_params_str(plan, best->indices, params, sizeof(params));
    wm_bt_html_escape(params, esc, sizeof(esc));

    // Name the winning config (the verdict above describes its result).
    fprintf(fp,
        "<p class=\"note\">Best of %u configurations: <span class=\"mono\">"
        "%s</span></p>\n", plan->total_iters, esc);

    wm_bt_fmt_usd(eq,   eq_str,   sizeof(eq_str));
    wm_bt_fmt_usd(rpnl, rpnl_str, sizeof(rpnl_str));
    wm_bt_fmt_pct(wr, 1, wr_str, sizeof(wr_str));
    wm_bt_fmt_pct(st->max_drawdown * 100.0, 1, dd_str, sizeof(dd_str));
    wm_bt_fmt_num(pf, 2, pf_str, sizeof(pf_str));
    snprintf(rt_str, sizeof(rt_str), "%u", rt);

    fputs("<div class=\"cards\">\n", fp);
    wm_bt_emit_card(fp, "Best equity",   "equity",       ec, eq_str);
    wm_bt_emit_card(fp, "Realized P/L",  "realized_pnl", rc, rpnl_str);
    wm_bt_emit_card(fp, "Profit factor", "pf",           "", pf_str);
    wm_bt_emit_card(fp, "Win rate",      "win_rate",     "", wr_str);
    wm_bt_emit_card(fp, "Round trips",   "round_trips",  "", rt_str);
    wm_bt_emit_card(fp, "Max drawdown",  "max_dd",       "", dd_str);
    fputs("</div>\n", fp);
  }

  // ---- run metadata ----
  fputs("<section><h2>Run</h2><div class=\"meta\">\n", fp);

  if(wm_path != NULL && wm_path[0] != '\0')
  {
    wm_bt_html_escape(wm_path, esc, sizeof(esc));
    fprintf(fp, "<div><dt>source</dt><div class=\"vv mono\">%s</div></div>\n",
        esc);
  }

  fprintf(fp,
      "<div><dt>iterations</dt><div class=\"vv\">%u total"
      " &middot; %u ok &middot; %u failed</div></div>\n",
      plan->total_iters, n_ok, n_fail);
  fprintf(fp,
      "<div><dt>threads</dt><div class=\"vv\">%u</div></div>\n",
      plan->workers);
  fprintf(fp,
      "<div><dt>wallclock</dt><div class=\"vv\">%.3f s</div></div>\n",
      (double)wallclock_ms / 1000.0);

  {
    double fee  = (fixed_params != NULL && fixed_params->have_fee_bps)
        ? fixed_params->fee_bps : WM_MARKET_DEFAULT_FEE_BPS;
    double slip = (fixed_params != NULL && fixed_params->have_slip_bps)
        ? fixed_params->slip_bps : WM_MARKET_DEFAULT_SLIP_BPS;
    double sf   = (fixed_params != NULL && fixed_params->have_size_frac)
        ? fixed_params->size_frac : WM_MARKET_DEFAULT_SIZE_FRAC;
    double cash = (fixed_params != NULL && fixed_params->have_starting_cash)
        ? fixed_params->starting_cash : WM_MARKET_DEFAULT_STARTING_CASH;
    char   size_str[32];
    char   cash_str[48];

    wm_bt_fmt_pct(sf * 100.0, 0, size_str, sizeof(size_str));
    wm_bt_fmt_usd(cash, cash_str, sizeof(cash_str));

    fprintf(fp,
        "<div><dt>economics</dt><div class=\"vv mono\">fee %.1f bps"
        " &middot; slip %.1f bps &middot; size %s &middot;"
        " cash %s</div></div>\n",
        fee, slip, size_str, cash_str);
  }

  if(mode_val == WM_BT_MODE_WALK_FORWARD && mode != NULL)
    fprintf(fp,
        "<div><dt>walk-forward</dt><div class=\"vv\">%u windows</div>"
        "</div>\n", mode->walk.n);

  fputs("</div></section>\n", fp);

  // ---- swept axes ----
  fputs("<section><h2>Swept axes</h2><div class=\"meta\">\n", fp);

  if(plan->n_axes == 0)
    fputs("<div><dt>axes</dt><div class=\"vv\">(none — single config)"
          "</div></div>\n", fp);

  for(a = 0; a < plan->n_axes; a++)
  {
    char vbuf[256];

    wm_bt_md_render_axis_values(&plan->axes[a], vbuf, sizeof(vbuf));
    wm_bt_html_escape(plan->axes[a].name, esc, sizeof(esc));
    wm_bt_html_escape(vbuf, esc2, sizeof(esc2));
    fprintf(fp,
        "<div><dt>%s</dt><div class=\"vv mono\">%u values: %s</div></div>\n",
        esc, plan->axes[a].n_values, esc2);
  }

  fputs("</div></section>\n", fp);

  // ---- top-K table ----
  fprintf(fp,
      "<section><h2>Top %u by %s</h2>\n", top_k,
      wm_bt_sweep_score_name(plan->score));

  if(top_k == 0)
    fputs("<p class=\"note\">No ranked configurations.</p>\n", fp);
  else
  {
    fputs("<div class=\"tbl-scroll\">\n", fp);
    wm_bt_emit_sweep_topk_table(fp, plan, results, indices, top_k);
    fputs("</div>\n", fp);
  }

  fputs("</section>\n", fp);

  // ---- per-axis marginal best ----
  if(plan->n_axes > 0)
  {
    fputs("<section><h2>Per-axis marginal best</h2>\n", fp);
    fprintf(fp,
        "<p class=\"note\">For each swept value, the best %s achieved by"
        " any iteration holding that value (other axes free).</p>\n",
        wm_bt_sweep_score_name(plan->score));

    for(a = 0; a < plan->n_axes; a++)
    {
      wm_bt_html_escape(plan->axes[a].name, esc, sizeof(esc));
      fprintf(fp, "<h3 class=\"sub-h\">%s</h3>\n", esc);
      wm_bt_emit_marginal_svg(fp, plan, results, n_results, a);
    }

    fputs("</section>\n", fp);
  }

  // ---- 2-axis score heatmap ----
  {
    uint32_t ax_x;
    uint32_t ax_y;

    if(wm_bt_pick_heatmap_axes(plan, &ax_x, &ax_y))
    {
      char xn_esc[64];
      char yn_esc[64];

      wm_bt_html_escape(plan->axes[ax_x].name, xn_esc, sizeof(xn_esc));
      wm_bt_html_escape(plan->axes[ax_y].name, yn_esc, sizeof(yn_esc));

      fputs("<section><h2>Score heatmap</h2>\n", fp);
      fprintf(fp,
          "<p class=\"note\">Best %s over <b>%s</b> (x) &times; <b>%s</b>"
          " (y) — the two widest axes; any other axes are free per"
          " cell.%s%s</p>\n",
          wm_bt_sweep_score_name(plan->score), xn_esc, yn_esc,
          plan->axes[ax_x].n_values > WM_BT_HEAT_MAX_VALUES ||
          plan->axes[ax_y].n_values > WM_BT_HEAT_MAX_VALUES
              ? " Axes wider than 32 values are truncated to the first 32"
                " cells." : "",
          plan->n_axes > 2
              ? " (>2 axes swept; the remaining axes are marginalised"
                " into each cell's best.)" : "");
      wm_bt_emit_heatmap_svg(fp, plan, results, n_results, ax_x, ax_y);
      fputs("</section>\n", fp);
    }
  }

  // ---- footer ----
  fputs("<footer>Generated by whenmoon backtest &middot; "
        "siblings: <a href=\"report.md\">report.md</a>, "
        "<a href=\"manifest.json\">manifest.json</a>, "
        "<a href=\"iterations.jsonl\">iterations.jsonl</a>, "
        "<a href=\"top-N.txt\">top-N.txt</a>. Per-trade charts are not "
        "emitted for a sweep — re-run the chosen config with no sweep "
        "axes to chart it.</footer>\n", fp);

  fputs("</div></body></html>\n", fp);

  mem_free(indices);

  return(wm_bt_finalize_file(fp, tmp, path, err, err_cap));
}

// ----------------------------------------------------------------------- //
// wm_bt_dir_listdir (WM-BT-7)                                             //
// ----------------------------------------------------------------------- //

static int
wm_bt_strdesc_cmp(const void *a, const void *b)
{
  const char *const *sa = a;
  const char *const *sb = b;

  return(strcmp(*sb, *sa));
}

static bool
wm_bt_is_sweep_id_prefix(const char *name)
{
  uint8_t i;

  if(name == NULL)
    return(FAIL);

  for(i = 0; i < 8; i++)
    if(name[i] < '0' || name[i] > '9')
      return(FAIL);

  return(name[8] == '-');
}

void
wm_bt_dir_listing_free(wm_bt_dir_listing_t *l)
{
  uint32_t i;

  if(l == NULL)
    return;

  if(l->names != NULL)
  {
    for(i = 0; i < l->n; i++)
      mem_free(l->names[i]);

    mem_free(l->names);
    l->names = NULL;
  }

  l->n = 0;
}

bool
wm_bt_dir_listdir(const char *path, wm_bt_dir_listing_t *out,
    char *err, size_t err_cap)
{
  DIR           *dir;
  struct dirent *de;
  char         **names    = NULL;
  uint32_t       n        = 0;
  uint32_t       cap      = 0;

  if(path == NULL || out == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "dir_listdir: bad args");
    return(FAIL);
  }

  out->names = NULL;
  out->n     = 0;

  dir = opendir(path);

  if(dir == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "opendir('%s') failed: %s", path, strerror(errno));
    return(FAIL);
  }

  while((de = readdir(dir)) != NULL)
  {
    char       child[2048];
    struct stat sb;
    char       *copy;
    size_t      len;
    int         nch;

    if(de->d_name[0] == '.')
      continue;

    if(!wm_bt_is_sweep_id_prefix(de->d_name))
      continue;

    nch = snprintf(child, sizeof(child), "%s/%s", path, de->d_name);

    if(nch < 0 || (size_t)nch >= sizeof(child))
      continue;

    if(stat(child, &sb) != 0 || !S_ISDIR(sb.st_mode))
      continue;

    if(n == cap)
    {
      uint32_t   new_cap = cap == 0 ? 32 : cap * 2;
      char     **grown;

      grown = mem_alloc(WM_BT_REPORT_CTX, "dir_names",
          sizeof(*grown) * (size_t)new_cap);

      if(grown == NULL)
      {
        closedir(dir);

        if(err != NULL)
          snprintf(err, err_cap, "dir_names alloc failed");

        if(names != NULL)
        {
          uint32_t k;
          for(k = 0; k < n; k++) mem_free(names[k]);
          mem_free(names);
        }

        return(FAIL);
      }

      if(names != NULL)
      {
        memcpy(grown, names, sizeof(*grown) * (size_t)n);
        mem_free(names);
      }

      names = grown;
      cap   = new_cap;
    }

    len  = strlen(de->d_name);
    copy = mem_alloc(WM_BT_REPORT_CTX, "dir_name", len + 1);

    if(copy == NULL)
    {
      closedir(dir);

      if(err != NULL)
        snprintf(err, err_cap, "dir_name alloc failed");

      if(names != NULL)
      {
        uint32_t k;
        for(k = 0; k < n; k++) mem_free(names[k]);
        mem_free(names);
      }

      return(FAIL);
    }

    memcpy(copy, de->d_name, len + 1);
    names[n++] = copy;
  }

  closedir(dir);

  if(n > 1)
    qsort(names, n, sizeof(*names), wm_bt_strdesc_cmp);

  out->names = names;
  out->n     = n;

  return(SUCCESS);
}
