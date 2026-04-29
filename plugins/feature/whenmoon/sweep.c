// botmanager — MIT
// Whenmoon backtest sweep planner + worker pool + reload gate (WM-LT-6).
//
// One sweep = N iterations of wm_backtest_run_iteration_with_id over a
// shared snapshot. Each iteration uses a different parameter vector
// drawn from the cartesian product of the declared sweep axes; each
// runs on its own worker thread with a private wm_trade_registry so
// the per-iteration trade book never contends with the live registry
// or with any other worker.
//
// Per-iteration parameter delivery uses the strategy KV resolver's
// per-market override slot. The worker pre-allocates a synthetic id,
// kv_register's the per-iter slot under
// "plugin.whenmoon.market.<id>.strategy.<name>.<axis>", kv_set's the
// sweep value, then hands the same id to wm_backtest_run_iteration_-
// with_id; the strategy's init_fn picks up the override via the
// existing two-tier resolver. After the iteration completes (success or
// fail) the worker drops the per-iter KV slots via kv_delete_prefix.
//
// Reload safety: a sweep increments wm_bt_sweep_active_inc on entry;
// /whenmoon backtest reload acquires the global reload lock and waits
// for the active counter to hit zero before performing dlclose+dlopen.
// This prevents a strategy unload from invalidating the function
// pointers cached for an in-flight iteration.

#define WHENMOON_INTERNAL
#include "sweep.h"

#include "backtest.h"
#include "order.h"
#include "pnl.h"
#include "strategy.h"
#include "whenmoon.h"
#include "whenmoon_strategy.h"

#include "alloc.h"
#include "clam.h"
#include "cmd.h"
#include "colors.h"
#include "common.h"
#include "kv.h"

#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WM_SWEEP_CTX                "whenmoon.sweep"
#define WM_BT_KV_PREFIX             "plugin.whenmoon.market.bt:"
#define WM_BT_RELOAD_WAIT_LOG_SEC   5
#define WM_BT_RESULT_NOSCORE        (-DBL_MAX)

// ----------------------------------------------------------------------- //
// Reload-coordination state                                               //
// ----------------------------------------------------------------------- //
//
// Sweeps and the /whenmoon backtest reload verb coordinate via a single
// process-wide mutex + an active-sweep refcount. Sweep entry takes the
// mutex briefly to bump the refcount, then drops it; reload acquires
// the mutex (blocking new sweeps from starting) and waits for the
// refcount to fall to zero before touching the strategy plugin.

static atomic_uint_fast32_t g_bt_active_sweeps = 0;
static pthread_mutex_t      g_bt_reload_lock   = PTHREAD_MUTEX_INITIALIZER;

// ----------------------------------------------------------------------- //
// Score helpers                                                           //
// ----------------------------------------------------------------------- //

bool
wm_bt_sweep_score_parse(const char *tok, wm_bt_sweep_score_t *out)
{
  if(tok == NULL || out == NULL)
    return(FAIL);

  if(strcasecmp(tok, "realized") == 0) { *out = WM_BT_SCORE_REALIZED;      return(SUCCESS); }
  if(strcasecmp(tok, "sharpe")   == 0) { *out = WM_BT_SCORE_SHARPE;        return(SUCCESS); }
  if(strcasecmp(tok, "sortino")  == 0) { *out = WM_BT_SCORE_SORTINO;       return(SUCCESS); }
  if(strcasecmp(tok, "equity")   == 0) { *out = WM_BT_SCORE_EQUITY;        return(SUCCESS); }
  if(strcasecmp(tok, "pf")       == 0) { *out = WM_BT_SCORE_PROFIT_FACTOR; return(SUCCESS); }

  return(FAIL);
}

const char *
wm_bt_sweep_score_name(wm_bt_sweep_score_t s)
{
  switch(s)
  {
    case WM_BT_SCORE_REALIZED:      return("realized");
    case WM_BT_SCORE_SHARPE:        return("sharpe");
    case WM_BT_SCORE_SORTINO:       return("sortino");
    case WM_BT_SCORE_EQUITY:        return("equity");
    case WM_BT_SCORE_PROFIT_FACTOR: return("pf");
  }

  return("?");
}

double
wm_bt_sweep_score_value(const wm_trade_snapshot_t *snap,
    wm_bt_sweep_score_t score)
{
  double v;

  if(snap == NULL)
    return(WM_BT_RESULT_NOSCORE);

  switch(score)
  {
    case WM_BT_SCORE_REALIZED:      v = snap->metrics.realized_pnl; break;
    case WM_BT_SCORE_SHARPE:        v = snap->metrics.sharpe;       break;
    case WM_BT_SCORE_SORTINO:       v = snap->metrics.sortino;      break;
    case WM_BT_SCORE_EQUITY:        v = snap->equity;               break;
    case WM_BT_SCORE_PROFIT_FACTOR: v = snap->metrics.profit_factor;break;
    default:                        return(WM_BT_RESULT_NOSCORE);
  }

  // NaN / -inf collapse to NOSCORE so they sort to the bottom.
  if(!isfinite(v))
    return(WM_BT_RESULT_NOSCORE);

  return(v);
}

// ----------------------------------------------------------------------- //
// Plan: init / axis_add / finalize / iter_indices                         //
// ----------------------------------------------------------------------- //

void
wm_bt_sweep_plan_init(wm_bt_sweep_plan_t *plan)
{
  if(plan == NULL)
    return;

  memset(plan, 0, sizeof(*plan));

  plan->n_axes      = 0;
  plan->total_iters = 1;
  plan->score       = WM_BT_SCORE_REALIZED;
  plan->top_k       = 1;
  plan->workers     = 1;
}

static const wm_strategy_param_t *
wm_bt_sweep_find_param(const loaded_strategy_t *ls, const char *name,
    char *avail_buf, size_t avail_cap)
{
  uint32_t i;
  size_t   off = 0;

  if(ls == NULL || name == NULL)
    return(NULL);

  // Build available-name list as we walk; renderer uses it on miss.
  if(avail_buf != NULL && avail_cap > 0)
    avail_buf[0] = '\0';

  for(i = 0; i < ls->meta.n_params; i++)
  {
    const wm_strategy_param_t *p = &ls->meta.params[i];

    if(p->name != NULL && avail_buf != NULL && off < avail_cap)
    {
      int n = snprintf(avail_buf + off, avail_cap - off,
          "%s%s", off == 0 ? "" : ", ", p->name);

      if(n > 0)
        off += (size_t)n;
    }
  }

  for(i = 0; i < ls->meta.n_params; i++)
  {
    const wm_strategy_param_t *p = &ls->meta.params[i];

    if(p->name != NULL && strcmp(p->name, name) == 0)
      return(p);
  }

  return(NULL);
}

// Parse a single numeric token (int or double). Sets *out and returns
// SUCCESS on a clean parse; FAIL otherwise.
static bool
wm_bt_sweep_parse_double(const char *tok, double *out)
{
  char  *end = NULL;
  double v;

  if(tok == NULL || tok[0] == '\0' || out == NULL)
    return(FAIL);

  errno = 0;
  v     = strtod(tok, &end);

  if(end == tok || errno != 0 || (end != NULL && *end != '\0'))
    return(FAIL);

  if(!isfinite(v))
    return(FAIL);

  *out = v;
  return(SUCCESS);
}

static bool
wm_bt_sweep_parse_value_list(const char *list,
    wm_bt_sweep_axis_t *axis, char *err, size_t err_cap)
{
  char         buf[1024];
  char        *save = NULL;
  char        *tok;
  uint32_t     n = 0;

  if(list == NULL || axis == NULL)
    return(FAIL);

  if(strlen(list) >= sizeof(buf))
  {
    if(err != NULL)
      snprintf(err, err_cap, "value list too long");
    return(FAIL);
  }

  snprintf(buf, sizeof(buf), "%s", list);

  for(tok = strtok_r(buf, ",", &save); tok != NULL;
      tok = strtok_r(NULL, ",", &save))
  {
    double v;

    if(n >= WM_BT_SWEEP_MAX_VALUES)
    {
      if(err != NULL)
        snprintf(err, err_cap, "axis '%s' has > %u values",
            axis->name, (unsigned)WM_BT_SWEEP_MAX_VALUES);
      return(FAIL);
    }

    if(wm_bt_sweep_parse_double(tok, &v) != SUCCESS)
    {
      if(err != NULL)
        snprintf(err, err_cap, "axis '%s': bad value '%s'",
            axis->name, tok);
      return(FAIL);
    }

    axis->values[n++] = v;
  }

  if(n == 0)
  {
    if(err != NULL)
      snprintf(err, err_cap, "axis '%s' has no values", axis->name);
    return(FAIL);
  }

  axis->n_values = n;
  return(SUCCESS);
}

static bool
wm_bt_sweep_parse_range(const char *expr,
    wm_bt_sweep_axis_t *axis, char *err, size_t err_cap)
{
  char    buf[256];
  char   *colon1;
  char   *colon2;
  double  lo, step, hi;
  double  v;
  uint32_t n = 0;
  // Tolerance for the upper-bound comparison: half a step is plenty.
  // Without this, accumulated FP error can drop the last point on a
  // range like "30:10:70" where 30+4*10 should equal 70 exactly but
  // could end up as 69.9999.... slipping past the strict <= test.
  double  hi_tol;

  if(expr == NULL || axis == NULL)
    return(FAIL);

  if(strlen(expr) >= sizeof(buf))
  {
    if(err != NULL)
      snprintf(err, err_cap, "range expression too long");
    return(FAIL);
  }

  snprintf(buf, sizeof(buf), "%s", expr);

  colon1 = strchr(buf, ':');

  if(colon1 == NULL)
    return(FAIL);

  *colon1 = '\0';
  colon2  = strchr(colon1 + 1, ':');

  if(colon2 == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "axis '%s': range needs lo:step:hi", axis->name);
    return(FAIL);
  }

  *colon2 = '\0';

  if(wm_bt_sweep_parse_double(buf,        &lo)   != SUCCESS ||
     wm_bt_sweep_parse_double(colon1 + 1, &step) != SUCCESS ||
     wm_bt_sweep_parse_double(colon2 + 1, &hi)   != SUCCESS)
  {
    if(err != NULL)
      snprintf(err, err_cap, "axis '%s': bad range", axis->name);
    return(FAIL);
  }

  if(step <= 0.0)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "axis '%s': step must be > 0 (got %.6g)", axis->name, step);
    return(FAIL);
  }

  if(hi < lo)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "axis '%s': hi (%.6g) < lo (%.6g)", axis->name, hi, lo);
    return(FAIL);
  }

  hi_tol = hi + step * 0.5;

  for(v = lo; v <= hi_tol; v += step)
  {
    if(n >= WM_BT_SWEEP_MAX_VALUES)
    {
      if(err != NULL)
        snprintf(err, err_cap,
            "axis '%s' would expand to > %u values"
            " (lo=%.6g step=%.6g hi=%.6g)",
            axis->name, (unsigned)WM_BT_SWEEP_MAX_VALUES, lo, step, hi);
      return(FAIL);
    }

    axis->values[n++] = v;
  }

  if(n == 0)
  {
    if(err != NULL)
      snprintf(err, err_cap, "axis '%s' produced no values", axis->name);
    return(FAIL);
  }

  axis->n_values = n;
  return(SUCCESS);
}

bool
wm_bt_sweep_axis_add(wm_bt_sweep_plan_t *plan,
    const loaded_strategy_t *ls, const char *expr,
    char *err, size_t err_cap)
{
  const wm_strategy_param_t *p;
  wm_bt_sweep_axis_t        *axis;
  char                       avail[256];
  char                       buf[1024];
  char                      *eq;
  size_t                     name_len;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(plan == NULL || ls == NULL || expr == NULL)
    return(FAIL);

  if(plan->n_axes >= WM_BT_SWEEP_MAX_PARAMS)
  {
    if(err != NULL)
      snprintf(err, err_cap, "too many --sweep axes (max %u)",
          (unsigned)WM_BT_SWEEP_MAX_PARAMS);
    return(FAIL);
  }

  if(strlen(expr) >= sizeof(buf))
  {
    if(err != NULL)
      snprintf(err, err_cap, "--sweep expression too long");
    return(FAIL);
  }

  snprintf(buf, sizeof(buf), "%s", expr);

  eq = strchr(buf, '=');

  if(eq == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "--sweep needs <name>=<values>; got '%s'", expr);
    return(FAIL);
  }

  *eq      = '\0';
  name_len = strlen(buf);

  if(name_len == 0)
  {
    if(err != NULL)
      snprintf(err, err_cap, "--sweep: empty axis name");
    return(FAIL);
  }

  if(name_len >= WM_BT_SWEEP_NAME_SZ)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "--sweep: axis name too long (max %u chars)",
          (unsigned)(WM_BT_SWEEP_NAME_SZ - 1));
    return(FAIL);
  }

  // Validate against the strategy's declared schema.
  p = wm_bt_sweep_find_param(ls, buf, avail, sizeof(avail));

  if(p == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "unknown sweep param '%s' (available: %s)",
          buf, avail[0] != '\0' ? avail : "<none>");
    return(FAIL);
  }

  if(p->type == WM_PARAM_STR)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "axis '%s' has string type; only numeric params can sweep",
          buf);
    return(FAIL);
  }

  // Dedup: same axis named twice in a single run is operator error.
  {
    uint32_t i;

    for(i = 0; i < plan->n_axes; i++)
      if(strcmp(plan->axes[i].name, buf) == 0)
      {
        if(err != NULL)
          snprintf(err, err_cap,
              "axis '%s' already supplied", buf);
        return(FAIL);
      }
  }

  axis = &plan->axes[plan->n_axes];
  memset(axis, 0, sizeof(*axis));
  snprintf(axis->name, sizeof(axis->name), "%s", buf);
  axis->type = p->type;

  // Range form (lo:step:hi) when the values text contains ':' before
  // any ',' — colons are not legal inside discrete value lists.
  {
    const char *vals  = eq + 1;
    const char *colon = strchr(vals, ':');
    const char *comma = strchr(vals, ',');

    if(colon != NULL && (comma == NULL || colon < comma))
    {
      if(wm_bt_sweep_parse_range(vals, axis, err, err_cap) != SUCCESS)
        return(FAIL);
    }

    else
    {
      if(wm_bt_sweep_parse_value_list(vals, axis, err, err_cap)
             != SUCCESS)
        return(FAIL);
    }
  }

  plan->n_axes++;
  return(SUCCESS);
}

bool
wm_bt_sweep_plan_finalize(wm_bt_sweep_plan_t *plan,
    char *err, size_t err_cap)
{
  uint64_t total = 1;
  uint32_t i;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(plan == NULL)
    return(FAIL);

  for(i = 0; i < plan->n_axes; i++)
    total *= (uint64_t)plan->axes[i].n_values;

  if(total > (uint64_t)WM_BT_SWEEP_MAX_ITERS)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "sweep too large (got %" PRIu64 ", cap %u)",
          total, (unsigned)WM_BT_SWEEP_MAX_ITERS);
    return(FAIL);
  }

  plan->total_iters = (uint32_t)total;

  // Clamp top_k to total. Reduce noise on tiny sweeps.
  if(plan->top_k == 0 || plan->top_k > plan->total_iters)
    plan->top_k = plan->total_iters;

  if(plan->workers < WM_BT_WORKERS_MIN)
    plan->workers = WM_BT_WORKERS_MIN;

  if(plan->workers > WM_BT_WORKERS_MAX)
    plan->workers = WM_BT_WORKERS_MAX;

  return(SUCCESS);
}

void
wm_bt_sweep_iter_indices(const wm_bt_sweep_plan_t *plan,
    uint32_t iter, uint32_t *out_indices)
{
  uint32_t  i;
  uint32_t  acc = iter;

  if(plan == NULL || out_indices == NULL)
    return;

  // Last axis varies fastest; mirrors C row-major iteration.
  for(i = plan->n_axes; i > 0; i--)
  {
    uint32_t a   = i - 1;
    uint32_t n   = plan->axes[a].n_values;

    out_indices[a] = (n != 0) ? (acc % n) : 0;
    acc            = (n != 0) ? (acc / n) : acc;
  }
}

// ----------------------------------------------------------------------- //
// Per-iteration KV management                                             //
// ----------------------------------------------------------------------- //

static kv_type_t
wm_bt_sweep_kv_type(wm_param_type_t t)
{
  switch(t)
  {
    case WM_PARAM_INT:    return(KV_INT64);
    case WM_PARAM_UINT:   return(KV_UINT64);
    case WM_PARAM_DOUBLE: return(KV_DOUBLE);
    case WM_PARAM_STR:    return(KV_STR);
  }

  return(KV_DOUBLE);
}

// Format `v` as a string suitable for kv_register/kv_set under `t`.
// INT/UINT round; DOUBLE renders %.10g.
static void
wm_bt_sweep_format_value(wm_param_type_t t, double v,
    char *out, size_t cap)
{
  if(out == NULL || cap == 0)
    return;

  switch(t)
  {
    case WM_PARAM_INT:
      snprintf(out, cap, "%" PRId64, (int64_t)llround(v));
      break;

    case WM_PARAM_UINT:
      snprintf(out, cap, "%" PRIu64, (uint64_t)llround(v < 0.0 ? 0 : v));
      break;

    case WM_PARAM_DOUBLE:
      snprintf(out, cap, "%.10g", v);
      break;

    case WM_PARAM_STR:
    default:
      out[0] = '\0';
      break;
  }
}

// Build the per-iter KV path. Returns SUCCESS / FAIL on overflow.
static bool
wm_bt_sweep_kv_path(const char *synth_id, const char *strategy,
    const char *axis_name, char *out, size_t cap)
{
  int n;

  if(synth_id == NULL || strategy == NULL || axis_name == NULL)
    return(FAIL);

  n = snprintf(out, cap,
      "plugin.whenmoon.market.%s.strategy.%s.%s",
      synth_id, strategy, axis_name);

  return((n > 0 && (size_t)n < cap) ? SUCCESS : FAIL);
}

static bool
wm_bt_sweep_apply_iter_kv(const char *synth_id, const char *strategy,
    const wm_bt_sweep_plan_t *plan, const uint32_t *indices,
    char *err, size_t err_cap)
{
  uint32_t a;

  for(a = 0; a < plan->n_axes; a++)
  {
    const wm_bt_sweep_axis_t *axis = &plan->axes[a];
    char                      path[KV_KEY_SZ];
    char                      val[64];
    double                    v = axis->values[indices[a]];

    if(wm_bt_sweep_kv_path(synth_id, strategy, axis->name,
           path, sizeof(path)) != SUCCESS)
    {
      if(err != NULL)
        snprintf(err, err_cap, "kv path overflow on '%s'", axis->name);
      return(FAIL);
    }

    wm_bt_sweep_format_value(axis->type, v, val, sizeof(val));

    // First call for this synth_id registers the slot at the right type.
    // Later iterations for the same synth_id won't recur (each iter
    // gets a unique synth_id), but be defensive against unforeseen
    // re-use by checking kv_exists first.
    if(!kv_exists(path))
    {
      if(kv_register(path, wm_bt_sweep_kv_type(axis->type),
             val, NULL, NULL, "") != SUCCESS)
      {
        if(err != NULL)
          snprintf(err, err_cap, "kv_register failed for '%s'", path);
        return(FAIL);
      }
    }

    else
    {
      if(kv_set(path, val) != SUCCESS)
      {
        if(err != NULL)
          snprintf(err, err_cap, "kv_set failed for '%s'", path);
        return(FAIL);
      }
    }
  }

  return(SUCCESS);
}

// Drop everything we wrote for a single iteration.
static void
wm_bt_sweep_drop_iter_kv(const char *synth_id)
{
  char prefix[KV_KEY_SZ];
  int  n;

  if(synth_id == NULL)
    return;

  n = snprintf(prefix, sizeof(prefix),
      "plugin.whenmoon.market.%s.", synth_id);

  if(n <= 0 || (size_t)n >= sizeof(prefix))
    return;

  kv_delete_prefix(prefix);
}

void
wm_bt_sweep_cleanup_stale_kv(void)
{
  uint32_t n = kv_delete_prefix(WM_BT_KV_PREFIX);

  if(n > 0)
    clam(CLAM_INFO, WM_SWEEP_CTX,
        "cleanup: dropped %u stale per-iter KV slots under %s",
        n, WM_BT_KV_PREFIX);
}

// ----------------------------------------------------------------------- //
// JSON serialisers (params / metrics) — used by the worker on persist     //
// ----------------------------------------------------------------------- //

static void
wm_bt_sweep_render_params_json(const wm_bt_sweep_plan_t *plan,
    const uint32_t *indices, char *out, size_t cap)
{
  size_t   off = 0;
  uint32_t a;
  int      n;

  if(out == NULL || cap == 0)
    return;

  n = snprintf(out + off, cap - off, "{");
  if(n < 0 || (size_t)n >= cap - off) { out[cap - 1] = '\0'; return; }
  off += (size_t)n;

  for(a = 0; a < plan->n_axes; a++)
  {
    const wm_bt_sweep_axis_t *axis = &plan->axes[a];
    double                    v    = axis->values[indices[a]];

    if(axis->type == WM_PARAM_DOUBLE)
      n = snprintf(out + off, cap - off,
          "%s\"%s\":%.10g", a == 0 ? "" : ",", axis->name, v);
    else
      n = snprintf(out + off, cap - off,
          "%s\"%s\":%" PRId64,
          a == 0 ? "" : ",", axis->name, (int64_t)llround(v));

    if(n < 0 || (size_t)n >= cap - off)
    {
      out[cap - 1] = '\0';
      return;
    }

    off += (size_t)n;
  }

  if(off + 1 < cap)
  {
    out[off]     = '}';
    out[off + 1] = '\0';
  }

  else
    out[cap - 1] = '\0';
}

static void
wm_bt_sweep_render_metrics_json(const wm_trade_snapshot_t *snap,
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
// Worker pool                                                             //
// ----------------------------------------------------------------------- //

typedef struct
{
  // Atomic dispatch: each worker pulls the next iter index.
  atomic_uint_fast32_t        next_iter;

  // Shared inputs (read-only inside workers).
  whenmoon_state_t           *st;
  wm_backtest_snapshot_t     *snap;
  const char                 *strategy_name;
  int32_t                     market_id_db;
  const wm_bt_sweep_plan_t   *plan;
  const wm_backtest_params_t *base_params;

  // Shared outputs: workers write into out_results[iter] only, no
  // collisions.
  wm_bt_sweep_result_t       *results;
} wm_bt_pool_t;

static void
wm_bt_sweep_run_one(wm_bt_pool_t *pool, uint32_t iter,
    const uint32_t *indices)
{
  wm_bt_sweep_result_t *result = &pool->results[iter];
  wm_trade_registry_t  *reg;
  wm_backtest_result_t  bt_result;
  char                  synth_id[WM_MARKET_ID_STR_SZ];
  char                  err[160];
  bool                  iter_ok;

  memset(result, 0, sizeof(*result));
  memcpy(result->indices, indices,
      sizeof(result->indices[0]) * pool->plan->n_axes);
  result->score = WM_BT_RESULT_NOSCORE;

  // Allocate the synthetic id BEFORE any KV writes so the writer
  // already has its addressable scope.
  wm_backtest_alloc_synthetic_id(synth_id, sizeof(synth_id));

  // Per-iter KV layer.
  err[0] = '\0';

  if(wm_bt_sweep_apply_iter_kv(synth_id, pool->strategy_name,
         pool->plan, indices, err, sizeof(err)) != SUCCESS)
  {
    snprintf(result->err, sizeof(result->err),
        "%s", err[0] != '\0' ? err : "kv apply failed");
    wm_bt_sweep_drop_iter_kv(synth_id);
    return;
  }

  // Private trade registry for this iteration.
  reg = wm_trade_registry_create();

  if(reg == NULL)
  {
    snprintf(result->err, sizeof(result->err),
        "private registry alloc failed");
    wm_bt_sweep_drop_iter_kv(synth_id);
    return;
  }

  wm_trade_engine_use_registry(reg);

  err[0] = '\0';
  iter_ok = wm_backtest_run_iteration_with_id(pool->st, pool->snap,
      pool->strategy_name, synth_id, pool->base_params,
      &bt_result, err, sizeof(err)) == SUCCESS;

  // Unbind + destroy the registry regardless of outcome — workers
  // never share registries.
  wm_trade_engine_use_registry(NULL);
  wm_trade_registry_destroy(reg);

  if(!iter_ok)
  {
    snprintf(result->err, sizeof(result->err),
        "%s", err[0] != '\0' ? err : "iteration failed");
    wm_bt_sweep_drop_iter_kv(synth_id);
    return;
  }

  // Populate the row + persist.
  result->ok            = true;
  result->trade         = bt_result.trade;
  result->wallclock_ms  = bt_result.wallclock_ms;
  result->bars_replayed = bt_result.bars_replayed;
  result->score         = wm_bt_sweep_score_value(&bt_result.trade,
      pool->plan->score);

  {
    wm_backtest_record_t rec;
    char                 metrics_json[1024];
    char                 params_json[512];
    int64_t              new_run_id = 0;

    memset(&rec, 0, sizeof(rec));
    rec.market_id     = pool->market_id_db;
    snprintf(rec.strategy_name, sizeof(rec.strategy_name),
        "%s", pool->strategy_name);
    snprintf(rec.range_start, sizeof(rec.range_start),
        "%s", pool->snap->range_start);
    snprintf(rec.range_end,   sizeof(rec.range_end),
        "%s", pool->snap->range_end);
    rec.wallclock_ms  = (int64_t)bt_result.wallclock_ms;
    rec.bars_replayed = bt_result.bars_replayed;
    rec.n_trades      = bt_result.trade.metrics.n_trades;
    rec.realized_pnl  = bt_result.trade.metrics.realized_pnl;
    rec.max_drawdown  = bt_result.trade.metrics.max_drawdown;
    rec.sharpe        = bt_result.trade.metrics.sharpe;
    rec.sortino       = bt_result.trade.metrics.sortino;
    rec.final_equity  = bt_result.trade.equity;

    wm_bt_sweep_render_params_json(pool->plan, indices,
        params_json, sizeof(params_json));
    wm_bt_sweep_render_metrics_json(&bt_result.trade,
        metrics_json, sizeof(metrics_json));

    if(wm_backtest_persist_run(&rec, params_json, metrics_json,
           &new_run_id) == SUCCESS)
      result->run_id_db = new_run_id;
  }

  wm_bt_sweep_drop_iter_kv(synth_id);
}

static void *
wm_bt_sweep_worker_main(void *arg)
{
  wm_bt_pool_t *pool = arg;

  for(;;)
  {
    uint32_t iter = (uint32_t)atomic_fetch_add(&pool->next_iter, 1);
    uint32_t indices[WM_BT_SWEEP_MAX_PARAMS] = {0};

    if(iter >= pool->plan->total_iters)
      break;

    wm_bt_sweep_iter_indices(pool->plan, iter, indices);
    wm_bt_sweep_run_one(pool, iter, indices);
  }

  return(NULL);
}

bool
wm_bt_sweep_run(whenmoon_state_t *st,
    wm_backtest_snapshot_t *snap,
    const char *strategy_name,
    int32_t market_id_db,
    const wm_bt_sweep_plan_t *plan,
    const wm_backtest_params_t *base_params,
    wm_bt_sweep_result_t *out_results,
    char *err, size_t err_cap)
{
  wm_bt_pool_t      pool;
  pthread_t        *threads;
  uint32_t          spawned = 0;
  uint32_t          i;
  bool              ok = SUCCESS;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(st == NULL || snap == NULL || strategy_name == NULL ||
     plan == NULL || out_results == NULL || plan->total_iters == 0)
  {
    if(err != NULL)
      snprintf(err, err_cap, "bad sweep inputs");
    return(FAIL);
  }

  // Coordinate with /whenmoon backtest reload. Take the reload lock
  // briefly so reload cannot be mid-dlclose when we register
  // attachment-shaped KVs + start workers; bump the active counter
  // before releasing so reload's wait-for-zero loop sees us.
  pthread_mutex_lock(&g_bt_reload_lock);
  wm_bt_sweep_active_inc();
  pthread_mutex_unlock(&g_bt_reload_lock);

  memset(&pool, 0, sizeof(pool));
  atomic_init(&pool.next_iter, 0);
  pool.st            = st;
  pool.snap          = snap;
  pool.strategy_name = strategy_name;
  pool.market_id_db  = market_id_db;
  pool.plan          = plan;
  pool.base_params   = base_params;
  pool.results       = out_results;

  threads = mem_alloc("whenmoon.sweep", "threads",
      sizeof(*threads) * plan->workers);

  if(threads == NULL)
  {
    wm_bt_sweep_active_dec();

    if(err != NULL)
      snprintf(err, err_cap, "thread array alloc failed");
    return(FAIL);
  }

  for(i = 0; i < plan->workers; i++)
  {
    if(pthread_create(&threads[i], NULL,
           wm_bt_sweep_worker_main, &pool) != 0)
    {
      ok = FAIL;
      break;
    }

    spawned++;
  }

  if(spawned == 0)
  {
    wm_bt_sweep_active_dec();
    mem_free(threads);

    if(err != NULL)
      snprintf(err, err_cap, "could not spawn any worker thread");
    return(FAIL);
  }

  for(i = 0; i < spawned; i++)
    pthread_join(threads[i], NULL);

  wm_bt_sweep_active_dec();
  mem_free(threads);

  if(ok != SUCCESS && err != NULL && err[0] == '\0')
    snprintf(err, err_cap,
        "spawned only %u of %u workers; results may be partial",
        spawned, plan->workers);

  return(ok);
}

// ----------------------------------------------------------------------- //
// Render top-K                                                            //
// ----------------------------------------------------------------------- //

// Sort key context. qsort doesn't accept a context, but we only need
// score-based descending sort which is encoded directly in result->score.

static int
wm_bt_sweep_compare_desc(const void *a, const void *b)
{
  const wm_bt_sweep_result_t *ra = a;
  const wm_bt_sweep_result_t *rb = b;

  // Failed iterations sink (score = NOSCORE = -DBL_MAX).
  if(ra->score > rb->score) return(-1);
  if(ra->score < rb->score) return( 1);

  // Tie-break on wallclock_ms ascending (faster wins) so the rendered
  // order is deterministic across runs.
  if(ra->wallclock_ms < rb->wallclock_ms) return(-1);
  if(ra->wallclock_ms > rb->wallclock_ms) return( 1);

  return(0);
}

void
wm_bt_sweep_render_topk(const cmd_ctx_t *ctx,
    const wm_bt_sweep_plan_t *plan,
    const wm_bt_sweep_result_t *results, uint32_t n)
{
  wm_bt_sweep_result_t *sorted;
  char                  line[320];
  char                  param_buf[160];
  uint32_t              top_k;
  uint32_t              i;
  uint32_t              n_ok = 0;

  if(ctx == NULL || plan == NULL || results == NULL || n == 0)
    return;

  for(i = 0; i < n; i++)
    if(results[i].ok)
      n_ok++;

  sorted = mem_alloc("whenmoon.sweep", "sorted",
      sizeof(*sorted) * (size_t)n);

  if(sorted == NULL)
  {
    cmd_reply(ctx, "render: out of memory");
    return;
  }

  memcpy(sorted, results, sizeof(*sorted) * (size_t)n);
  qsort(sorted, n, sizeof(*sorted), wm_bt_sweep_compare_desc);

  top_k = plan->top_k > n ? n : plan->top_k;

  snprintf(line, sizeof(line),
      CLR_BOLD "top %u of %u (score=%s, ok=%u)" CLR_RESET,
      top_k, n, wm_bt_sweep_score_name(plan->score), n_ok);
  cmd_reply(ctx, line);

  for(i = 0; i < top_k; i++)
  {
    const wm_bt_sweep_result_t *r = &sorted[i];
    size_t                      off = 0;
    uint32_t                    a;
    int                         w;

    param_buf[0] = '\0';

    for(a = 0; a < plan->n_axes && off + 1 < sizeof(param_buf); a++)
    {
      const wm_bt_sweep_axis_t *axis = &plan->axes[a];
      double                    v    = axis->values[r->indices[a]];

      if(axis->type == WM_PARAM_DOUBLE)
        w = snprintf(param_buf + off, sizeof(param_buf) - off,
            "%s%s=%.6g", a == 0 ? "" : " ", axis->name, v);
      else
        w = snprintf(param_buf + off, sizeof(param_buf) - off,
            "%s%s=%" PRId64,
            a == 0 ? "" : " ", axis->name, (int64_t)llround(v));

      if(w < 0)
        break;

      off += (size_t)w;
    }

    if(!r->ok)
    {
      snprintf(line, sizeof(line),
          "  #%-3u %s  FAIL: %.120s",
          i + 1, param_buf, r->err);
      cmd_reply(ctx, line);
      continue;
    }

    snprintf(line, sizeof(line),
        "  #%-3u %s  %s=%+.4f"
        " realized=%+.4f trades=%-3u equity=%.2f"
        " ms=%-5" PRIu64 "%s%" PRId64,
        i + 1, param_buf,
        wm_bt_sweep_score_name(plan->score), r->score,
        r->trade.metrics.realized_pnl, r->trade.metrics.n_trades,
        r->trade.equity, r->wallclock_ms,
        r->run_id_db > 0 ? " run=" : "",
        r->run_id_db > 0 ? r->run_id_db : (int64_t)0);
    cmd_reply(ctx, line);
  }

  mem_free(sorted);
}

// ----------------------------------------------------------------------- //
// Reload gating                                                           //
// ----------------------------------------------------------------------- //

void
wm_bt_sweep_active_inc(void)
{
  atomic_fetch_add(&g_bt_active_sweeps, 1);
}

void
wm_bt_sweep_active_dec(void)
{
  atomic_fetch_sub(&g_bt_active_sweeps, 1);
}

uint32_t
wm_bt_sweep_active_count(void)
{
  return((uint32_t)atomic_load(&g_bt_active_sweeps));
}

bool
wm_bt_sweep_reload_strategy(whenmoon_state_t *st,
    const char *strategy_name, uint32_t *out_n_detached,
    char *err, size_t err_cap)
{
  bool ok;
  time_t   t_last_log;
  uint32_t active;

  if(out_n_detached != NULL)
    *out_n_detached = 0;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(st == NULL || strategy_name == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "bad reload inputs");
    return(FAIL);
  }

  pthread_mutex_lock(&g_bt_reload_lock);

  // Wait for in-flight sweeps to drain. A long sweep is the
  // operator's call to make — we don't cancel it. Periodic CLAM_INFO
  // tells the operator why reload is paused.
  t_last_log = 0;
  for(;;)
  {
    active = (uint32_t)atomic_load(&g_bt_active_sweeps);

    if(active == 0)
      break;

    {
      time_t now = time(NULL);

      if(now - t_last_log >= WM_BT_RELOAD_WAIT_LOG_SEC)
      {
        clam(CLAM_INFO, WM_SWEEP_CTX,
            "reload waiting on %u active sweep(s)", active);
        t_last_log = now;
      }
    }

    {
      struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };

      nanosleep(&ts, NULL);
    }
  }

  ok = wm_strategy_reload(st, strategy_name, out_n_detached,
      err, err_cap) == SUCCESS;

  pthread_mutex_unlock(&g_bt_reload_lock);

  return(ok ? SUCCESS : FAIL);
}
