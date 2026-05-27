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
#define WM_BT_REPORT_DIR_MODE_PUB   ((mode_t)0755)
#define WM_BT_REPORT_DIR_MODE_PRV   ((mode_t)0700)

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
// path-separator byte at a time) and mkdir's each segment with `parent_mode`
// for every interior segment, `leaf_mode` for the final one. Empty
// segments (`//`) are skipped. EEXIST is success. Anything else FAILs
// with errno set.

static bool
wm_bt_mkdir_p(char *path, mode_t parent_mode, mode_t leaf_mode,
    char *err, size_t err_cap)
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
    bool is_last;

    if(i < len && path[i] != '/')
      continue;

    is_last = (i == len);
    saved   = path[i];
    path[i] = '\0';

    if(path[0] != '\0' && strcmp(path, "/") != 0)
    {
      mode_t m = is_last ? leaf_mode : parent_mode;

      if(mkdir(path, m) != 0 && errno != EEXIST)
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

  if(wm_bt_mkdir_p(buf,
         WM_BT_REPORT_DIR_MODE_PUB, WM_BT_REPORT_DIR_MODE_PRV,
         err, err_cap) != SUCCESS)
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

  if(mkdir(out_path, WM_BT_REPORT_DIR_MODE_PRV) != 0 && errno != EEXIST)
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

  if(mkdir(charts, WM_BT_REPORT_DIR_MODE_PRV) != 0 && errno != EEXIST)
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
