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
#include "wm_bt_chart.h"
#include "whenmoon_strategy.h"

#include "alloc.h"
#include "common.h"
#include "kv.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

static double
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

// Write `json_str` atomically to `path` via tmp + rename.
static bool
wm_bt_write_atomic(const char *path, const char *json_str,
    char *err, size_t err_cap)
{
  char   tmp[1280];
  size_t len;
  FILE  *fp;
  int    fd;
  int    n;

  if(path == NULL || json_str == NULL)
    return(FAIL);

  len = strlen(json_str);

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

  if(fwrite(json_str, 1, len, fp) != len ||
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

// Escape the five HTML-significant characters into `out`. Always
// NUL-terminates; silently stops at the buffer edge.
static void
wm_bt_html_escape(const char *in, char *out, size_t cap)
{
  size_t o = 0;

  if(out == NULL || cap == 0)
    return;

  if(in == NULL)
    in = "";

  for(; *in != '\0' && o + 1 < cap; in++)
  {
    const char *rep = NULL;
    size_t      rl;

    switch(*in)
    {
      case '&':  rep = "&amp;";  break;
      case '<':  rep = "&lt;";   break;
      case '>':  rep = "&gt;";   break;
      case '"':  rep = "&quot;"; break;
      case '\'': rep = "&#39;";  break;
      default:
        out[o++] = *in;
        continue;
    }

    rl = strlen(rep);

    if(o + rl >= cap)
      break;

    memcpy(out + o, rep, rl);
    o += rl;
  }

  out[o] = '\0';
}

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
        "<a class=\"chart-link\" target=\"_blank\""
        " href=\"charts/iter-%u/trade-%u-%s.html\">%s&#8599;</a>",
        rank, tidx, gname, gname);

    any = true;
  }

  if(!any)
    fputs("<span class=\"muted\">&mdash;</span>", fp);
}

// One iteration's trade table. Pairs buy→sell over the captured fills
// ring (long-only / flat ⇒ strict buy/sell alternation, but defensive
// against an unmatched trailing buy = open-at-end). Returns the number
// of paired trades rendered.
static uint32_t
wm_bt_idx_emit_trades(FILE *fp, const char *sweep_dir,
    const wm_bt_sweep_result_t *r, uint32_t rank, uint16_t emit_mask)
{
  uint32_t f;
  uint32_t tidx = 0;

  fputs("<table class=\"trades\"><thead><tr>"
        "<th>#</th><th>entry (UTC)</th><th>exit (UTC)</th><th>held</th>"
        "<th class=\"num\">entry</th><th class=\"num\">exit</th>"
        "<th class=\"num\">qty</th><th class=\"num\">P/L</th>"
        "<th class=\"num\">P/L %</th><th class=\"num\">balance</th>"
        "<th>exit reason</th><th>chart</th></tr></thead><tbody>\n", fp);

  for(f = 0; f < r->n_fills; f++)
  {
    const wm_market_fill_t *entry = &r->fills[f];
    const wm_market_fill_t *exit_fill = NULL;
    uint32_t                e;
    char                    entry_ts[64];
    char                    exit_ts[64];
    char                    held[32];
    char                    reason_esc[128];
    double                  pnl;
    double                  notional;
    double                  pct;
    const char             *cls;

    if(entry->side != 'b')
      continue;

    for(e = f + 1; e < r->n_fills; e++)
    {
      if(r->fills[e].side == 's')
      {
        exit_fill = &r->fills[e];
        f         = e;
        break;
      }
    }

    tidx++;

    wm_bt_idx_fmt_ts(entry->ts_ms, entry_ts, sizeof(entry_ts));
    notional = entry->price * entry->qty;

    if(exit_fill != NULL)
    {
      pnl = exit_fill->realized_pnl;
      pct = (notional > 0.0) ? (pnl / notional) * 100.0 : 0.0;
      cls = (pnl > 0.0) ? "win" : (pnl < 0.0) ? "loss" : "flat";

      wm_bt_idx_fmt_ts(exit_fill->ts_ms, exit_ts, sizeof(exit_ts));
      wm_bt_idx_fmt_dur(exit_fill->ts_ms - entry->ts_ms,
          held, sizeof(held));
      wm_bt_html_escape(exit_fill->reason, reason_esc,
          sizeof(reason_esc));
    }
    else
    {
      pnl = 0.0;
      pct = 0.0;
      cls = "open";
      snprintf(exit_ts, sizeof(exit_ts), "(open)");
      snprintf(held,    sizeof(held),    "&mdash;");
      wm_bt_html_escape(entry->reason, reason_esc, sizeof(reason_esc));
    }

    fprintf(fp,
        "<tr class=\"%s\"><td>%u</td><td>%s</td><td>%s</td><td>%s</td>"
        "<td class=\"num\">%.2f</td>",
        cls, tidx, entry_ts, exit_ts, held, entry->price);

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

// The full <style> block + page chrome are static, so they go through
// fputs (no printf %-escaping headaches with CSS percentages).
static void
wm_bt_idx_write_head(FILE *fp, const char *title_esc)
{
  fprintf(fp,
      "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
      "<meta name=\"viewport\" content=\"width=device-width,"
      "initial-scale=1\">\n<title>%s</title>\n", title_esc);

  fputs(
      "<style>\n"
      ":root{--bg:#0e0f13;--surface:#171922;--surface2:#1e2230;"
      "--border:#2a2f3e;--text:#e6e8ee;--muted:#8b93a7;--accent:#5b8cff;"
      "--win:#2bb673;--loss:#e0533d;--gold:#f5c451}\n"
      "*{box-sizing:border-box}\n"
      "body{margin:0;background:var(--bg);color:var(--text);"
      "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,"
      "Helvetica,Arial,sans-serif;line-height:1.5}\n"
      ".mono{font-family:ui-monospace,'SF Mono',Menlo,Consolas,monospace}\n"
      "header{position:sticky;top:0;z-index:10;background:"
      "rgba(14,15,19,.92);backdrop-filter:blur(8px);"
      "border-bottom:1px solid var(--border);padding:18px 28px}\n"
      "header h1{margin:0 0 2px;font-size:19px;font-weight:650}\n"
      "header .sub{color:var(--muted);font-size:13px}\n"
      ".wrap{max-width:1180px;margin:0 auto;padding:24px 28px 64px}\n"
      ".cards{display:grid;grid-template-columns:repeat(auto-fit,"
      "minmax(150px,1fr));gap:14px;margin:22px 0}\n"
      ".card{background:var(--surface);border:1px solid var(--border);"
      "border-radius:12px;padding:14px 16px}\n"
      ".card .k{color:var(--muted);font-size:12px;text-transform:uppercase;"
      "letter-spacing:.04em}\n"
      ".card .v{font-size:23px;font-weight:650;margin-top:4px}\n"
      ".card .v.mono{font-size:20px}\n"
      "section{margin:30px 0}\n"
      "h2{font-size:15px;font-weight:600;color:var(--muted);"
      "text-transform:uppercase;letter-spacing:.05em;"
      "border-bottom:1px solid var(--border);padding-bottom:8px}\n"
      "table{border-collapse:collapse;width:100%;font-size:13px}\n"
      "th,td{padding:7px 10px;text-align:left;border-bottom:1px solid "
      "var(--border);white-space:nowrap}\n"
      "th{color:var(--muted);font-weight:600;font-size:11px;"
      "text-transform:uppercase;letter-spacing:.03em}\n"
      ".num{text-align:right;font-family:ui-monospace,'SF Mono',Menlo,"
      "Consolas,monospace}\n"
      "td.reason{color:var(--muted);font-size:12px;white-space:normal}\n"
      ".meta dt{color:var(--muted);font-size:12px;text-transform:uppercase;"
      "letter-spacing:.03em}\n"
      ".meta{display:grid;grid-template-columns:repeat(auto-fit,"
      "minmax(220px,1fr));gap:10px 28px;margin:8px 0}\n"
      ".meta div{border-bottom:1px solid var(--border);padding:6px 0}\n"
      ".meta .vv{font-size:14px}\n"
      "details{background:var(--surface);border:1px solid var(--border);"
      "border-radius:12px;margin:14px 0;overflow:hidden}\n"
      "details[open]{box-shadow:0 0 0 1px var(--accent) inset}\n"
      "summary{cursor:pointer;padding:14px 18px;list-style:none;"
      "display:flex;align-items:center;gap:14px;flex-wrap:wrap}\n"
      "summary::-webkit-details-marker{display:none}\n"
      "summary .rank{background:var(--surface2);border:1px solid "
      "var(--border);border-radius:8px;padding:2px 10px;font-weight:700;"
      "font-size:13px}\n"
      "summary .params{font-size:13px}\n"
      "summary .spacer{flex:1}\n"
      ".badge{font-family:ui-monospace,monospace;font-size:13px;"
      "padding:2px 9px;border-radius:8px;border:1px solid var(--border)}\n"
      ".badge.win{color:var(--win);border-color:rgba(43,182,115,.4)}\n"
      ".badge.loss{color:var(--loss);border-color:rgba(224,83,61,.4)}\n"
      ".strip{display:flex;flex-wrap:wrap;gap:8px 22px;padding:4px 18px "
      "14px;border-bottom:1px solid var(--border)}\n"
      ".strip .it{font-size:13px}\n"
      ".strip .it span{color:var(--muted);margin-right:6px}\n"
      ".tbl-scroll{max-height:560px;overflow:auto;padding:0 6px 6px}\n"
      "tr.win td.pnl{color:var(--win)}\n"
      "tr.loss td.pnl{color:var(--loss)}\n"
      "tr.win:hover,tr.loss:hover,tr.open:hover{background:var(--surface2)}\n"
      ".pos{color:var(--win)}.neg{color:var(--loss)}\n"
      ".muted{color:var(--muted)}\n"
      "a{color:var(--accent);text-decoration:none}a:hover{text-decoration:"
      "underline}\n"
      ".chart-link{font-family:ui-monospace,monospace;font-size:12px;"
      "margin-right:8px}\n"
      ".note{color:var(--muted);font-size:12px;padding:10px 18px}\n"
      "footer{color:var(--muted);font-size:12px;margin-top:40px;"
      "border-top:1px solid var(--border);padding-top:16px}\n"
      "</style>\n</head><body>\n", fp);
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

  fputs("<div class=\"wrap\">\n", fp);

  // ---- headline cards (best config = top-K rank 1) ----
  if(top_k > 0 && results[indices[0]].ok)
  {
    const wm_bt_sweep_result_t *best = &results[indices[0]];
    const wm_market_stats_t    *st   = &best->trade.stats[WM_MARKET_MODE_PAPER];
    uint32_t rt   = st->n_wins + st->n_losses;
    double   wr   = rt > 0 ? (double)st->n_wins / (double)rt * 100.0 : 0.0;
    double   pf   = wm_market_stats_profit_factor(st);
    double   eq   = wm_bt_compute_equity(&best->trade);
    double   rpnl = st->realized_pnl_lifetime;
    const char *ec = eq >= WM_MARKET_DEFAULT_STARTING_CASH ? "pos" : "neg";
    const char *rc = rpnl >= 0.0 ? "pos" : "neg";

    fputs("<div class=\"cards\">\n", fp);
    fprintf(fp,
        "<div class=\"card\"><div class=\"k\">Best equity</div>"
        "<div class=\"v mono %s\">$%.0f</div></div>\n", ec, eq);
    fprintf(fp,
        "<div class=\"card\"><div class=\"k\">Realized P/L</div>"
        "<div class=\"v mono %s\">%+.0f</div></div>\n", rc, rpnl);
    fprintf(fp,
        "<div class=\"card\"><div class=\"k\">Profit factor</div>"
        "<div class=\"v mono\">%.2f</div></div>\n", pf);
    fprintf(fp,
        "<div class=\"card\"><div class=\"k\">Win rate</div>"
        "<div class=\"v mono\">%.1f%%</div></div>\n", wr);
    fprintf(fp,
        "<div class=\"card\"><div class=\"k\">Round trips</div>"
        "<div class=\"v mono\">%u</div></div>\n", rt);
    fprintf(fp,
        "<div class=\"card\"><div class=\"k\">Max drawdown</div>"
        "<div class=\"v mono\">%.1f%%</div></div>\n",
        st->max_drawdown * 100.0);
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

    fprintf(fp,
        "<div><dt>economics</dt><div class=\"vv mono\">fee %.1f bps"
        " &middot; slip %.1f bps &middot; size %.0f%% &middot;"
        " cash $%.0f</div></div>\n",
        fee, slip, sf * 100.0, cash);
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
        "<div class=\"it\"><span>round trips</span>%u</div>"
        "<div class=\"it\"><span>win rate</span>%.1f%%</div>"
        "<div class=\"it\"><span>wins / losses</span>%u / %u</div>"
        "<div class=\"it\"><span>gross +/-</span>%.0f / -%.0f</div>"
        "<div class=\"it\"><span>max dd</span>%.1f%%</div>"
        "<div class=\"it\"><span>sharpe</span>%.3f</div>"
        "<div class=\"it\"><span>sortino</span>%.3f</div>"
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
      // Captured fills ring caps at WM_MARKET_FILL_RING_CAP; warn when
      // the full count exceeded it so the table's partiality is clear.
      if((uint64_t)st->lifetime_fills_count > (uint64_t)r->n_fills)
        fprintf(fp,
            "<p class=\"note\">Showing the most recent %u of %"
            PRIu64 " fills (capture ring cap); earlier trades omitted."
            "</p>\n", r->n_fills,
            (uint64_t)st->lifetime_fills_count);

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
