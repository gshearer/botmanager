// botmanager — MIT
// Marketwatch substrate (MW-2) + price-move detectors (MW-3): per-
// exchange bulk-ticker polling, plus a 2-state machine per pair that
// fires CLAM events on entry to / exit from a "hot" condition.
//
// One periodic task per enabled exchange fires
// `exchange_fetch_all_tickers_async` at the cadence read from KV. The
// typed response callback walks the snapshot array under the per-
// exchange lock, finds-or-inserts each pair in a fixed-size table,
// pushes the row into the pair's ring, runs the MW-3 detectors, and
// records pending CLAM emissions in a per-tick heap buffer that is
// drained AFTER the lock is released (so a slow subscriber cb can't
// extend lock hold time).
//
// Threading: one lock per exchange (`ex->lock`) guards pairs[] + ring
// memory + counters; the global `mw_g.mtx` guards only the exchange
// roster + global enabled flag. The two are never held simultaneously.
// Per-exchange tasks run on TASK_THREAD workers; response callbacks
// fire on the curl worker thread of the protocol plugin.
//
// Storage is eager: at mw_start every registered exchange gets one
// pairs[] allocation (MW_PAIRS_CAP rows) and every pair gets one ring
// of `mw_g.ring_n` rows. ~17 MB across three exchanges with defaults
// — within budget and simpler than lazy allocation when ticks fire
// across all exchanges.

#define WHENMOON_INTERNAL
#include "mw.h"
#include "whenmoon.h"
#include "dl_jobtable.h"   // wm_dl_now_ms
#include "exchange_api.h"
#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "kv.h"
#include "method.h"
#include "task.h"

#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------------ //
// Constants                                                           //
// ------------------------------------------------------------------ //

#define MW_PAIRS_CAP            2048
#define MW_RING_N_DEFAULT         60
#define MW_POLL_SEC_DEFAULT       60
#define MW_MAX_EXCH                8
#define MW_INFLIGHT_GUARD_MS    5000
#define MW_EXCH_LIST_CAP          16

#define MW_KV_GLOBAL_ENABLED   "plugin.whenmoon.mw.enabled"
#define MW_KV_RING_N           "plugin.whenmoon.mw.ring_n"

// MW-3 detector defaults + tunables.
// Thresholds are stored as `% × 100` (a uint that survives the KV layer);
// `500` means 5.00%. Windows/cooldowns are in their named units.
#define MW_PCT_24H_THRESH_DEFAULT      500       // 5.00 %
#define MW_VEL_PCT_THRESH_DEFAULT      200       // 2.00 %
#define MW_VEL_WINDOW_MIN_DEFAULT       10       // minutes
#define MW_VOL_Z_THRESH_DEFAULT        300       // 3.00 sigma
#define MW_VOL_Z_MIN_SAMPLES            10       // history floor
#define MW_MIN_VOL_USD_DEFAULT     1000000ull    // $1M 24h quote-vol
#define MW_COOLDOWN_SEC_DEFAULT        300
#define MW_UPD_THROTTLE_SEC_DEFAULT    300

// 50% hysteresis: a HOT pair must drop below thresh * NUM/DEN on all
// signals for cooldown duration before COOL fires.
#define MW_HOT_HYST_NUM                  1
#define MW_HOT_HYST_DEN                  2

// Per-tick stack-bounded emission cap. 256 transitions per exchange
// per tick is comically high — overflow under defaults only on a
// pump that converts the entire roster in one breath. Beyond the cap
// we drop further emissions, log one WARN, and let next tick re-emit
// (pair state is still updated under the lock).
#define MW_EMIT_BUF_CAP                256

// Per-signal trigger bitset. Stored on mw_pair_t.last_trigger so the
// UPD throttle can detect "trigger set changed even though still HOT".
#define MW_TRIG_PCT_24H        (1u << 0)
#define MW_TRIG_VELOCITY       (1u << 1)
#define MW_TRIG_BRK_HI         (1u << 2)
#define MW_TRIG_BRK_LO         (1u << 3)
#define MW_TRIG_VOL_Z          (1u << 4)

// Per-exchange KV key buffer size. Longest tail is ".enabled" (8
// bytes) below "plugin.whenmoon.mw." (19) + EXCHANGE_NAME_SZ (32) +
// NUL — comfortably fits 96 bytes.
#define MW_KV_KEY_SZ              96

// Output line widths.
#define MW_LINE_SZ                256

// Per-render top-N cutoffs.
#define MW_TOPN                    10

// ------------------------------------------------------------------ //
// State                                                               //
// ------------------------------------------------------------------ //

typedef enum
{
  MW_PSTATE_COLD = 0,
  MW_PSTATE_HOT  = 1
} mw_pair_state_t;

typedef struct
{
  char                          product_id[EXCHANGE_PRODUCT_ID_SZ];
  int64_t                       last_seen_ms;
  uint32_t                      snap_count;
  uint32_t                      ring_head;
  exchange_ticker_snapshot_t   *ring;

  // MW-3: per-slot wall-clock timestamps, indexed in lockstep with
  // pp->ring. Allocated once at mw_start, freed at mw_deinit.
  int64_t                      *ring_ts;

  // MW-3: detector state.
  uint8_t                       state;            // mw_pair_state_t
  uint8_t                       last_trigger;     // MW_TRIG_* bitset
  int64_t                       state_changed_ms; // last COLD<->HOT flip
  int64_t                       last_emit_ms;     // last hot|upd emission
  double                        prev_hi_24h;      // for BRK_HI gate
  double                        prev_lo_24h;      // for BRK_LO gate
} mw_pair_t;

typedef struct
{
  char                  name[EXCHANGE_NAME_SZ];
  bool                  enabled;
  uint32_t              poll_sec;
  task_handle_t         task;
  pthread_mutex_t       lock;
  uint32_t              pair_count;
  uint32_t              pair_cap;
  mw_pair_t            *pairs;
  int64_t               last_poll_ms;
  int64_t               last_dispatch_ms;
  uint64_t              total_polls;
  uint64_t              total_pairs_seen;
  uint64_t              total_drops_full;

  // MW-3: per-exchange detector thresholds. Read at mw_start +
  // mw_enable_exch from KV. Counters are bumped under ex->lock.
  uint32_t              pct_24h_thresh_x100;  // signal × 100
  uint32_t              vel_pct_thresh_x100;
  uint32_t              vel_window_ms;
  uint32_t              vol_z_thresh_x100;    // sigma × 100
  uint64_t              min_vol_usd;
  uint32_t              cooldown_ms;
  uint32_t              upd_throttle_ms;
  uint64_t              total_emits_hot;
  uint64_t              total_emits_cool;
  uint64_t              total_emits_upd;
} mw_exch_t;

// MW-3: one queued CLAM emission. Built under ex->lock, drained
// outside it.
typedef struct
{
  char  topic[CLAM_CTX_SZ];
  char  body[CLAM_MSG_SZ];
} mw_emit_t;

typedef struct
{
  bool             ready;          // true once mw_init has fully run
  pthread_mutex_t  mtx;
  bool             global_enabled;
  uint32_t         ring_n;
  uint32_t         n_exch;
  mw_exch_t        exch[MW_MAX_EXCH];
} mw_state_t;

static mw_state_t mw_g;

// ------------------------------------------------------------------ //
// Static helpers                                                      //
// ------------------------------------------------------------------ //

static void
mw_kv_key(char *buf, size_t cap, const char *exch, const char *tail)
{
  // Bounded copy so the const-pointer length is provably <
  // EXCHANGE_NAME_SZ to silence -Wformat-truncation on
  // %s-into-fixed-buffer concatenations.
  char ename[EXCHANGE_NAME_SZ];
  size_t n = strnlen(exch, EXCHANGE_NAME_SZ - 1);

  memcpy(ename, exch, n);
  ename[n] = '\0';

  snprintf(buf, cap, "plugin.whenmoon.mw.%s.%s", ename, tail);
}

// MW-3: read all six per-exchange detector thresholds from KV, applying
// the documented `0 → default` clamp. Caller must already have filled
// ex->name. Used from mw_start (initial load) and from mw_enable_exch
// (operator may have set kv * between mw_start and the enable verb,
// same rationale as poll_sec).
static void
mw_load_detector_thresholds(mw_exch_t *ex)
{
  char     key[MW_KV_KEY_SZ];
  uint64_t v;

  mw_kv_key(key, sizeof(key), ex->name, "pct_24h_thresh_x100");
  v = kv_get_uint(key);
  ex->pct_24h_thresh_x100 =
      (uint32_t)(v == 0 ? MW_PCT_24H_THRESH_DEFAULT : v);

  mw_kv_key(key, sizeof(key), ex->name, "vel_pct_thresh_x100");
  v = kv_get_uint(key);
  ex->vel_pct_thresh_x100 =
      (uint32_t)(v == 0 ? MW_VEL_PCT_THRESH_DEFAULT : v);

  mw_kv_key(key, sizeof(key), ex->name, "vol_z_thresh_x100");
  v = kv_get_uint(key);
  ex->vol_z_thresh_x100 =
      (uint32_t)(v == 0 ? MW_VOL_Z_THRESH_DEFAULT : v);

  mw_kv_key(key, sizeof(key), ex->name, "vel_window_min");
  v = kv_get_uint(key);
  ex->vel_window_ms =
      (uint32_t)((v == 0 ? MW_VEL_WINDOW_MIN_DEFAULT : v) * 60000ull);

  // min_vol_usd: zero is a legitimate operator choice ("don't gate"),
  // so we map the KV-absent case to MW_MIN_VOL_USD_DEFAULT but allow
  // an explicit /set kv ... 0 to disable the floor. Distinguishing
  // "unset" from "set to 0" requires kv_exists.
  mw_kv_key(key, sizeof(key), ex->name, "min_vol_usd");
  if(kv_exists(key))
    ex->min_vol_usd = kv_get_uint(key);
  else
    ex->min_vol_usd = MW_MIN_VOL_USD_DEFAULT;

  mw_kv_key(key, sizeof(key), ex->name, "cooldown_sec");
  v = kv_get_uint(key);
  ex->cooldown_ms =
      (uint32_t)((v == 0 ? MW_COOLDOWN_SEC_DEFAULT : v) * 1000u);

  mw_kv_key(key, sizeof(key), ex->name, "upd_throttle_sec");
  v = kv_get_uint(key);
  ex->upd_throttle_ms =
      (uint32_t)((v == 0 ? MW_UPD_THROTTLE_SEC_DEFAULT : v) * 1000u);
}

static mw_exch_t *
mw_find_exch_by_name(const char *name)
{
  uint32_t i;

  if(name == NULL || name[0] == '\0')
    return(NULL);

  for(i = 0; i < mw_g.n_exch; i++)
  {
    if(strncmp(mw_g.exch[i].name, name, EXCHANGE_NAME_SZ) == 0)
      return(&mw_g.exch[i]);
  }

  return(NULL);
}

static uint32_t
mw_pair_find_or_insert(mw_exch_t *ex, const char *product_id)
{
  uint32_t i;

  for(i = 0; i < ex->pair_count; i++)
  {
    if(strncmp(ex->pairs[i].product_id, product_id,
          EXCHANGE_PRODUCT_ID_SZ) == 0)
      return(i);
  }

  if(ex->pair_count >= ex->pair_cap)
    return(UINT32_MAX);

  i = ex->pair_count++;
  snprintf(ex->pairs[i].product_id, sizeof(ex->pairs[i].product_id),
      "%s", product_id);
  ex->pairs[i].snap_count       = 0;
  ex->pairs[i].ring_head        = 0;
  ex->pairs[i].state            = MW_PSTATE_COLD;
  ex->pairs[i].last_trigger     = 0;
  ex->pairs[i].state_changed_ms = 0;
  ex->pairs[i].last_emit_ms     = 0;
  ex->pairs[i].prev_hi_24h      = 0.0;
  ex->pairs[i].prev_lo_24h      = 0.0;
  return(i);
}

static void
mw_ring_push(mw_pair_t *pp, const exchange_ticker_snapshot_t *snap,
    uint32_t ring_n, int64_t now_ms)
{
  if(ring_n == 0 || pp->ring == NULL)
    return;

  memcpy(&pp->ring[pp->ring_head], snap, sizeof(*snap));

  // MW-3: parallel ts[] stays in lockstep with ring[]. ring_ts may be
  // NULL on the (single) tick that races a freshly-rolled allocation
  // failure; tolerate that — detectors that need it short-circuit on
  // pp->snap_count and that gate covers the missing-ts case too.
  if(pp->ring_ts != NULL)
    pp->ring_ts[pp->ring_head] = now_ms;

  pp->ring_head = (pp->ring_head + 1) % ring_n;
  pp->last_seen_ms = now_ms;

  if(pp->snap_count < ring_n)
    pp->snap_count++;
}

// Read latest snapshot from a pair's ring without copying. Caller
// holds ex->lock. Returns NULL when snap_count == 0.
static const exchange_ticker_snapshot_t *
mw_pair_latest(const mw_pair_t *pp, uint32_t ring_n)
{
  uint32_t idx;

  if(pp->snap_count == 0 || pp->ring == NULL || ring_n == 0)
    return(NULL);

  // ring_head points to the next write slot — the latest sample is
  // one slot behind, modulo ring_n.
  idx = (pp->ring_head + ring_n - 1) % ring_n;
  return(&pp->ring[idx]);
}

// ------------------------------------------------------------------ //
// MW-3: detectors                                                     //
// ------------------------------------------------------------------ //

// Walks pp->ring newest-to-oldest from (ring_head - 1), looking for
// the first snapshot whose stored ts is <= (now_ms - window_ms). Returns
// (now_price - then_price)/then_price * 100, or NAN when the ring is
// shallower than the window or the older sample is malformed.
//
// Caller holds ex->lock.
static double
mw_compute_velocity_pct(const mw_pair_t *pp, uint32_t window_ms,
    int64_t now_ms)
{
  uint32_t                              ring_n = mw_g.ring_n;
  int64_t                               target_ms = now_ms - (int64_t)window_ms;
  const exchange_ticker_snapshot_t     *now_snap;
  const exchange_ticker_snapshot_t     *then_snap;
  uint32_t                              idx;
  uint32_t                              hops;

  if(pp->snap_count < 2 || pp->ring == NULL || pp->ring_ts == NULL
      || ring_n == 0)
    return(NAN);

  // newest entry was just pushed at (ring_head - 1) mod ring_n
  idx      = (pp->ring_head + ring_n - 1) % ring_n;
  now_snap = &pp->ring[idx];

  for(hops = 1; hops < pp->snap_count; hops++)
  {
    idx = (idx + ring_n - 1) % ring_n;

    if(pp->ring_ts[idx] <= target_ms)
    {
      then_snap = &pp->ring[idx];

      if(then_snap->price <= 0.0 || isnan(then_snap->price))
        return(NAN);

      return((now_snap->price - then_snap->price)
          / then_snap->price * 100.0);
    }
  }

  return(NAN);   // ring shallower than window
}

// Volume z-score over pp->ring, EXCLUDING the just-pushed entry (it
// lives at (ring_head - 1) mod ring_n after mw_ring_push). Two-pass
// mean+stdev — N is small enough (default 60) that a single-pass
// numerically-stable variant is unnecessary; clarity wins. Sample
// stdev (N-1 denominator) is conservative for the ring sizes in play.
//
// Returns NAN when:
//   * the just-pushed vol is itself NaN (exchange doesn't ship vol);
//   * pp->snap_count is below MW_VOL_Z_MIN_SAMPLES + 1 (cold pair);
//   * fewer than MW_VOL_Z_MIN_SAMPLES finite-vol historical entries
//     exist (intermittent drops in the ring);
//   * sample stdev is effectively zero (degenerate constant-vol
//     history — z-score undefined).
//
// Caller holds ex->lock.
static double
mw_compute_vol_z(const mw_pair_t *pp, uint32_t ring_n,
    const exchange_ticker_snapshot_t *snap)
{
  uint32_t newest_idx;
  uint32_t hops;
  uint32_t idx;
  uint32_t n_finite = 0;
  double   sum      = 0.0;
  double   mean;
  double   ss       = 0.0;
  double   var;
  double   stdev;
  double   d;

  if(pp->ring == NULL || ring_n == 0)            return(NAN);
  if(isnan(snap->vol_24h_quote))                 return(NAN);
  if(pp->snap_count < MW_VOL_Z_MIN_SAMPLES + 1)  return(NAN);

  newest_idx = (pp->ring_head + ring_n - 1) % ring_n;

  // Pass 1: mean of historical (non-newest) finite vol_24h_quote.
  for(hops = 1; hops < pp->snap_count; hops++)
  {
    idx = (newest_idx + ring_n - hops) % ring_n;

    if(!isnan(pp->ring[idx].vol_24h_quote))
    {
      sum += pp->ring[idx].vol_24h_quote;
      n_finite++;
    }
  }

  if(n_finite < MW_VOL_Z_MIN_SAMPLES)
    return(NAN);

  mean = sum / (double)n_finite;

  // Pass 2: sample stdev over the same set.
  for(hops = 1; hops < pp->snap_count; hops++)
  {
    idx = (newest_idx + ring_n - hops) % ring_n;

    if(!isnan(pp->ring[idx].vol_24h_quote))
    {
      d   = pp->ring[idx].vol_24h_quote - mean;
      ss += d * d;
    }
  }

  var   = ss / (double)(n_finite - 1);
  stdev = sqrt(var);

  if(stdev < 1e-9)
    return(NAN);

  return((snap->vol_24h_quote - mean) / stdev);
}

// Render `mw.<exch>.<event>.<id>` into out (cap = CLAM_CTX_SZ). On
// truncation, the rendered string is still NUL-terminated and matches
// subscribers' prefix regexes — log one WARN per (exch, event) so the
// operator can choose to lengthen CLAM_CTX_SZ if real-world product
// ids start blowing the budget.
static void
mw_format_topic(char *out, size_t sz, const char *exch,
    const char *event, const char *id)
{
  static bool warned[4];   // pct_24h doesn't apply — events: hot/cool/upd/add ish
  int   n;
  int   event_idx;

  n = snprintf(out, sz, "mw.%s.%s.%s", exch, event, id);

  if(n < 0 || (size_t)n >= sz)
  {
    // WARN-once per event token. We don't strictly need to scope it
    // per exchange — the per-event guard is enough to prevent log
    // floods; operators rarely add new exchanges.
    if(strcmp(event, "hot") == 0)        event_idx = 0;
    else if(strcmp(event, "cool") == 0)  event_idx = 1;
    else if(strcmp(event, "upd") == 0)   event_idx = 2;
    else                                  event_idx = 3;

    if(!warned[event_idx])
    {
      warned[event_idx] = true;
      clam(CLAM_WARN, MW_CTX,
          "topic truncated: mw.%s.%s.%s (cap=%zu)",
          exch, event, id, sz);
    }
  }
}

// Tiny helper: append `frag` to `buf` at position *off, advancing *off.
// On truncation, *off is clamped to cap-1 so subsequent appends still
// see a valid NUL. Returns false when the append did not fully fit
// (caller may abort).
static bool
mw_body_append(char *buf, size_t cap, size_t *off, const char *frag)
{
  size_t free_cap;
  size_t fl;

  if(*off >= cap)
    return(false);

  free_cap = cap - *off;
  fl       = strnlen(frag, free_cap);

  memcpy(buf + *off, frag, fl);
  *off += fl;

  if(*off < cap)
    buf[*off] = '\0';
  else
  {
    buf[cap - 1] = '\0';
    return(false);
  }

  return(fl < strlen(frag) ? false : true);
}

// Render `%g` / `%f` field or the literal `null` if NaN. Writes into
// a tiny stack buffer then appends via mw_body_append.
static bool
mw_body_append_dbl(char *buf, size_t cap, size_t *off, const char *key,
    const char *fmt, double v)
{
  char  scratch[64];

  if(isnan(v))
    snprintf(scratch, sizeof(scratch), "\"%s\":null", key);
  else
  {
    char  numbuf[40];

    snprintf(numbuf, sizeof(numbuf), fmt, v);
    snprintf(scratch, sizeof(scratch), "\"%s\":%s", key, numbuf);
  }

  return(mw_body_append(buf, cap, off, scratch));
}

// Build the trigger string ("pct_24h+velocity+brk_hi"...). For an empty
// bitset (COOL emissions), writes "".
static void
mw_format_triggers(char *out, size_t sz, uint8_t bits)
{
  size_t off = 0;
  bool   first = true;

  out[0] = '\0';

  if(bits & MW_TRIG_PCT_24H)
  {
    off += snprintf(out + off, sz - off, "%spct_24h",
        first ? "" : "+");
    first = false;
  }

  if(bits & MW_TRIG_VELOCITY && off < sz)
  {
    off += snprintf(out + off, sz - off, "%svelocity",
        first ? "" : "+");
    first = false;
  }

  if(bits & MW_TRIG_BRK_HI && off < sz)
  {
    off += snprintf(out + off, sz - off, "%sbrk_hi",
        first ? "" : "+");
    first = false;
  }

  if(bits & MW_TRIG_BRK_LO && off < sz)
  {
    off += snprintf(out + off, sz - off, "%sbrk_lo",
        first ? "" : "+");
    first = false;
  }

  if(bits & MW_TRIG_VOL_Z && off < sz)
    snprintf(out + off, sz - off, "%svol_z", first ? "" : "+");
}

// Render the single-line JSON body. Returns true on full render,
// false on truncation (caller should refuse the emit).
static bool
mw_format_body(char *out, size_t sz, const mw_exch_t *ex,
    const exchange_ticker_snapshot_t *snap, int64_t now_ms,
    double vel_pct, double vol_z, uint8_t triggers, const char *state)
{
  size_t  off = 0;
  char    scratch[160];
  char    trig_buf[64];

  out[0] = '\0';
  mw_format_triggers(trig_buf, sizeof(trig_buf), triggers);

  // The function-level `now_ms` is monotonic — used by detector
  // bookkeeping. JSON consumers (IRC bridges, log forwarders) need
  // wall-clock epoch ms or they can't render a real date.
  (void)now_ms;
  int64_t ts_wall_ms = wm_now_ms();

  // Open brace + ts + exch + id + price (all required).
  snprintf(scratch, sizeof(scratch),
      "{\"ts\":%" PRId64 ",\"exch\":\"%s\",\"id\":\"%s\",",
      ts_wall_ms, ex->name, snap->product_id);
  if(!mw_body_append(out, sz, &off, scratch)) return(false);

  // price: emit verbatim if finite (no exchange ships NaN here, but
  // guard anyway so a malformed snap can't drop the trailing }).
  if(isnan(snap->price))
    snprintf(scratch, sizeof(scratch), "\"price\":null,");
  else
    snprintf(scratch, sizeof(scratch), "\"price\":%.8g,", snap->price);
  if(!mw_body_append(out, sz, &off, scratch)) return(false);

  // Numeric optional fields.
  if(!mw_body_append_dbl(out, sz, &off, "pct_24h", "%.2f",
        snap->pct_24h)) return(false);
  if(!mw_body_append(out, sz, &off, ",")) return(false);

  if(!mw_body_append_dbl(out, sz, &off, "vel_pct", "%.2f", vel_pct))
    return(false);
  if(!mw_body_append(out, sz, &off, ",")) return(false);

  snprintf(scratch, sizeof(scratch), "\"vel_window_min\":%u,",
      (unsigned)(ex->vel_window_ms / 60000u));
  if(!mw_body_append(out, sz, &off, scratch)) return(false);

  if(!mw_body_append_dbl(out, sz, &off, "vol_z", "%.2f", vol_z))
    return(false);
  if(!mw_body_append(out, sz, &off, ",")) return(false);

  if(!mw_body_append_dbl(out, sz, &off, "hi_24h", "%.8g",
        snap->hi_24h)) return(false);
  if(!mw_body_append(out, sz, &off, ",")) return(false);

  if(!mw_body_append_dbl(out, sz, &off, "lo_24h", "%.8g",
        snap->lo_24h)) return(false);
  if(!mw_body_append(out, sz, &off, ",")) return(false);

  if(!mw_body_append_dbl(out, sz, &off, "vol_24h_q", "%.2f",
        snap->vol_24h_quote)) return(false);
  if(!mw_body_append(out, sz, &off, ",")) return(false);

  // trigger + state + close brace.
  snprintf(scratch, sizeof(scratch),
      "\"trigger\":\"%s\",\"state\":\"%s\"}",
      trig_buf, state);
  if(!mw_body_append(out, sz, &off, scratch)) return(false);

  return(true);
}

// Compute the four-bit trigger set against the given threshold scale.
// `scale_num/scale_den` allow the hysteresis pass to use thresh×NUM/DEN
// without touching the real ex->* fields. Caller passes the just-
// pushed snap; pp is the same pair the snap was pushed into.
static uint8_t
mw_compute_triggers(const mw_exch_t *ex, const mw_pair_t *pp,
    const exchange_ticker_snapshot_t *snap, double vel_pct,
    double vol_z, uint32_t scale_num, uint32_t scale_den)
{
  uint8_t  bits = 0;
  uint64_t pct_thresh;
  uint64_t vel_thresh;
  uint64_t vol_z_thresh;

  // Avoid 0-division if a caller passes a bad scale.
  if(scale_den == 0)
    scale_den = 1;

  pct_thresh   = (uint64_t)ex->pct_24h_thresh_x100 * scale_num / scale_den;
  vel_thresh   = (uint64_t)ex->vel_pct_thresh_x100 * scale_num / scale_den;
  vol_z_thresh = (uint64_t)ex->vol_z_thresh_x100   * scale_num / scale_den;

  if(!isnan(snap->pct_24h)
      && (uint64_t)(fabs(snap->pct_24h) * 100.0) >= pct_thresh)
    bits |= MW_TRIG_PCT_24H;

  if(pp->snap_count >= 2 && !isnan(vel_pct)
      && (uint64_t)(fabs(vel_pct) * 100.0) >= vel_thresh)
    bits |= MW_TRIG_VELOCITY;

  if(pp->snap_count >= 2
      && !isnan(snap->hi_24h) && !isnan(pp->prev_hi_24h)
      && pp->prev_hi_24h > 0.0
      && snap->hi_24h > pp->prev_hi_24h
      && snap->price >= pp->prev_hi_24h)
    bits |= MW_TRIG_BRK_HI;

  if(pp->snap_count >= 2
      && !isnan(snap->lo_24h) && !isnan(pp->prev_lo_24h)
      && pp->prev_lo_24h > 0.0
      && snap->lo_24h < pp->prev_lo_24h
      && snap->price <= pp->prev_lo_24h)
    bits |= MW_TRIG_BRK_LO;

  // One-sided: positive z (high-volume spike) only. Negative z is the
  // quiet-pair signal, not in scope for this initiative.
  if(pp->snap_count >= MW_VOL_Z_MIN_SAMPLES + 1
      && !isnan(vol_z)
      && vol_z > 0.0
      && (uint64_t)(vol_z * 100.0) >= vol_z_thresh)
    bits |= MW_TRIG_VOL_Z;

  return(bits);
}

// Detector entry point. Runs the three signals over the just-pushed
// snap, drives the per-pair state machine, and fills `out_emit` when a
// transition needs to be communicated. Returns true iff an emit was
// queued. Caller holds ex->lock.
static bool
mw_detect_pair(mw_exch_t *ex, mw_pair_t *pp,
    const exchange_ticker_snapshot_t *snap, int64_t now_ms,
    mw_emit_t *out_emit)
{
  double   vel_pct;
  double   vol_z;
  uint8_t  triggers;
  uint8_t  hyst_triggers;
  bool     queued = false;
  const char *event_str = NULL;
  const char *state_str = NULL;

  vel_pct = mw_compute_velocity_pct(pp, ex->vel_window_ms, now_ms);
  vol_z   = mw_compute_vol_z(pp, mw_g.ring_n, snap);
  triggers = mw_compute_triggers(ex, pp, snap, vel_pct, vol_z, 1, 1);

  // Min-volume gate on HOT entry only. Bypassed when the exchange
  // didn't publish a quote volume (Gemini), to avoid suppressing every
  // pair on that backend.
  if(pp->state == MW_PSTATE_COLD
      && !isnan(snap->vol_24h_quote)
      && snap->vol_24h_quote < (double)ex->min_vol_usd)
    triggers = 0;

  hyst_triggers = mw_compute_triggers(ex, pp, snap, vel_pct, vol_z,
      MW_HOT_HYST_NUM, MW_HOT_HYST_DEN);

  // State machine.
  if(pp->state == MW_PSTATE_COLD)
  {
    if(triggers != 0)
    {
      pp->state            = MW_PSTATE_HOT;
      pp->state_changed_ms = now_ms;
      pp->last_trigger     = triggers;
      pp->last_emit_ms     = now_ms;
      event_str = "hot";
      state_str = "hot";
      ex->total_emits_hot++;
      queued = true;
    }
  }

  else  // HOT
  {
    if((now_ms - pp->state_changed_ms) >= (int64_t)ex->cooldown_ms
        && hyst_triggers == 0)
    {
      pp->state            = MW_PSTATE_COLD;
      pp->state_changed_ms = now_ms;
      pp->last_trigger     = 0;
      event_str = "cool";
      state_str = "cool";
      ex->total_emits_cool++;
      // For COOL we emit with empty trigger set; that gives subscribers
      // a clear "all clear" signal.
      triggers = 0;
      queued = true;
    }

    else
    {
      // Still HOT — throttled UPD. Throttle alone is the gate; we
      // also refresh last_trigger so the caller can see which signals
      // are currently active without scraping the body JSON.
      if((now_ms - pp->last_emit_ms) >= (int64_t)ex->upd_throttle_ms)
      {
        pp->last_emit_ms = now_ms;
        pp->last_trigger = triggers;
        event_str = "upd";
        state_str = "upd";
        ex->total_emits_upd++;
        queued = true;
      }
    }
  }

  // Always-update high/low watermarks so the next tick has the
  // freshest comparison point. Preserve previous value when the
  // exchange dropped the field this tick (NaN).
  if(!isnan(snap->hi_24h))
    pp->prev_hi_24h = snap->hi_24h;
  if(!isnan(snap->lo_24h))
    pp->prev_lo_24h = snap->lo_24h;

  if(!queued)
    return(false);

  mw_format_topic(out_emit->topic, sizeof(out_emit->topic),
      ex->name, event_str, snap->product_id);

  if(!mw_format_body(out_emit->body, sizeof(out_emit->body),
        ex, snap, now_ms, vel_pct, vol_z, triggers, state_str))
  {
    // Body overflow is unreachable in practice with single-line JSON
    // of ~10 fields, but be loud + drop quietly if it ever fires.
    clam(CLAM_DEBUG3, MW_CTX, "%s: body render overflow (%s %s)",
        ex->name, event_str, snap->product_id);
    return(false);
  }

  return(true);
}

// Drain `n` queued emissions via clam(). Called AFTER pthread_mutex_
// unlock(&ex->lock) so subscriber callbacks (which fire inline in
// clam()) cannot lengthen lock-hold time.
static void
mw_drain_emits(const mw_emit_t *emits, uint32_t n)
{
  uint32_t i;

  for(i = 0; i < n; i++)
    clam(CLAM_INFO, emits[i].topic, "%s", emits[i].body);
}

// ------------------------------------------------------------------ //
// Task callbacks                                                      //
// ------------------------------------------------------------------ //

static void
mw_tickers_done_cb(bool success, const char *err,
    const exchange_ticker_snapshot_t *snaps, size_t n, void *user)
{
  mw_exch_t *ex = user;
  mw_emit_t *pending;
  uint32_t   n_pending = 0;
  uint32_t   n_dropped_emits = 0;
  size_t     i;
  uint32_t   slot;
  uint32_t   drops_this_tick = 0;
  uint32_t   ring_n;
  int64_t    now_ms;

  if(ex == NULL)
    return;

  if(!success)
  {
    clam(CLAM_WARN, MW_CTX, "%s: tick FAIL: %s",
        ex->name, err != NULL ? err : "?");
    return;
  }

  // MW-3: heap-allocate the per-tick emit buffer. 256 × ~1060 bytes
  // ≈ 272 KiB — too large for the curl-worker thread stack on some
  // distros' default 256 KiB pthread default. On alloc FAIL we skip
  // detection for this tick (pair state stays untouched so next tick
  // can still transition) but still update the ring + counters, so
  // operators see polling continuing in /show whenmoon mw.
  pending = mem_alloc(WHENMOON_CTX, "mw.emits",
      (size_t)MW_EMIT_BUF_CAP * sizeof(*pending));

  if(pending == NULL)
    clam(CLAM_WARN, MW_CTX, "%s: emit buf alloc FAIL — skipping"
        " detection this tick", ex->name);

  pthread_mutex_lock(&ex->lock);

  // Disabled between dispatch + completion — drop result.
  if(!ex->enabled)
  {
    pthread_mutex_unlock(&ex->lock);
    if(pending != NULL)
      mem_free(pending);
    return;
  }

  ring_n = mw_g.ring_n;
  now_ms = wm_dl_now_ms();

  for(i = 0; i < n; i++)
  {
    slot = mw_pair_find_or_insert(ex, snaps[i].product_id);

    if(slot == UINT32_MAX)
    {
      drops_this_tick++;
      continue;
    }

    mw_ring_push(&ex->pairs[slot], &snaps[i], ring_n, now_ms);

    // MW-3: detector hook. Caller (this loop) holds ex->lock; the
    // returned `pending[]` slot is consumed AFTER lock release.
    if(pending == NULL)
      continue;

    if(n_pending < MW_EMIT_BUF_CAP)
    {
      if(mw_detect_pair(ex, &ex->pairs[slot], &snaps[i], now_ms,
            &pending[n_pending]))
        n_pending++;
    }
    else
      n_dropped_emits++;
  }

  ex->last_poll_ms       = now_ms;
  ex->total_polls++;
  ex->total_pairs_seen   = (uint64_t)n;
  ex->total_drops_full  += drops_this_tick;

  pthread_mutex_unlock(&ex->lock);

  // MW-3: drain outside the lock so a slow CLAM subscriber cb cannot
  // extend ex->lock hold time.
  if(pending != NULL)
  {
    mw_drain_emits(pending, n_pending);
    mem_free(pending);
  }

  if(n_dropped_emits > 0)
    clam(CLAM_WARN, MW_CTX,
        "%s: emit buf overflow %u (cap=%u)",
        ex->name, n_dropped_emits, (unsigned)MW_EMIT_BUF_CAP);

  if(drops_this_tick > 0)
    clam(CLAM_WARN, MW_CTX, "%s: %u pairs dropped (cap=%u)",
        ex->name, drops_this_tick, (unsigned)MW_PAIRS_CAP);
  else
    clam(CLAM_DEBUG3, MW_CTX, "%s: tick ok n=%zu emits=%u",
        ex->name, n, n_pending);
}

static void
mw_periodic_cb(task_t *t)
{
  mw_exch_t *ex;
  int64_t    now_ms;
  bool       dispatch;

  if(t == NULL)
    return;

  ex = t->data;
  dispatch = true;

  if(ex == NULL)
  {
    t->state = TASK_ENDED;
    return;
  }

  pthread_mutex_lock(&ex->lock);

  now_ms = wm_dl_now_ms();

  // Inflight guard. last_dispatch_ms > last_poll_ms means a previous
  // dispatch has not yet been observed. Skip a tick if the response
  // hasn't shown up within the guard window — prevents stacking 60
  // simultaneous requests on an outage at a 60s cadence.
  if(ex->last_dispatch_ms > ex->last_poll_ms
      && (now_ms - ex->last_dispatch_ms) < MW_INFLIGHT_GUARD_MS)
  {
    dispatch = false;
    clam(CLAM_DEBUG3, MW_CTX,
        "%s: skip tick (inflight %" PRId64 "ms)",
        ex->name, now_ms - ex->last_dispatch_ms);
  }

  else
    ex->last_dispatch_ms = now_ms;

  pthread_mutex_unlock(&ex->lock);

  if(dispatch
      && exchange_fetch_all_tickers_async(ex->name,
            mw_tickers_done_cb, ex) != SUCCESS)
    clam(CLAM_WARN, MW_CTX, "%s: dispatch FAIL", ex->name);

  t->state = TASK_ENDED;   // periodic — "iteration done, reschedule"
}

// ------------------------------------------------------------------ //
// Internal lifecycle helpers                                          //
// ------------------------------------------------------------------ //

// Allocates pairs[] + per-pair rings for one exchange slot. Returns
// SUCCESS on success; on failure any partial allocation is rolled back
// and the slot is left in its zeroed state.
static bool
mw_exch_alloc_pairs(mw_exch_t *ex, uint32_t ring_n)
{
  uint32_t i;

  ex->pairs = mem_alloc(WHENMOON_CTX, "mw.pairs",
      (size_t)MW_PAIRS_CAP * sizeof(*ex->pairs));

  if(ex->pairs == NULL)
    return(FAIL);

  memset(ex->pairs, 0, (size_t)MW_PAIRS_CAP * sizeof(*ex->pairs));
  ex->pair_cap   = MW_PAIRS_CAP;
  ex->pair_count = 0;

  for(i = 0; i < MW_PAIRS_CAP; i++)
  {
    ex->pairs[i].ring = mem_alloc(WHENMOON_CTX, "mw.ring",
        (size_t)ring_n * sizeof(*ex->pairs[i].ring));

    if(ex->pairs[i].ring == NULL)
    {
      // Roll back: free already-allocated rings + ts arrays, then pairs[].
      uint32_t j;

      for(j = 0; j < i; j++)
      {
        mem_free(ex->pairs[j].ring);
        if(ex->pairs[j].ring_ts != NULL)
          mem_free(ex->pairs[j].ring_ts);
      }

      mem_free(ex->pairs);
      ex->pairs    = NULL;
      ex->pair_cap = 0;
      return(FAIL);
    }

    // MW-3: parallel ts[] for velocity walk-back. Allocated lockstep
    // with ring[]; freed in the same loop in mw_exch_free_pairs.
    ex->pairs[i].ring_ts = mem_alloc(WHENMOON_CTX, "mw.ring_ts",
        (size_t)ring_n * sizeof(*ex->pairs[i].ring_ts));

    if(ex->pairs[i].ring_ts == NULL)
    {
      uint32_t j;

      mem_free(ex->pairs[i].ring);
      ex->pairs[i].ring = NULL;

      for(j = 0; j < i; j++)
      {
        mem_free(ex->pairs[j].ring);
        mem_free(ex->pairs[j].ring_ts);
      }

      mem_free(ex->pairs);
      ex->pairs    = NULL;
      ex->pair_cap = 0;
      return(FAIL);
    }
  }

  return(SUCCESS);
}

static void
mw_exch_free_pairs(mw_exch_t *ex)
{
  uint32_t i;

  if(ex->pairs == NULL)
    return;

  for(i = 0; i < ex->pair_cap; i++)
  {
    if(ex->pairs[i].ring != NULL)
    {
      mem_free(ex->pairs[i].ring);
      ex->pairs[i].ring = NULL;
    }

    if(ex->pairs[i].ring_ts != NULL)
    {
      mem_free(ex->pairs[i].ring_ts);
      ex->pairs[i].ring_ts = NULL;
    }
  }

  mem_free(ex->pairs);
  ex->pairs      = NULL;
  ex->pair_cap   = 0;
  ex->pair_count = 0;
}

// Spawn the per-exchange periodic task. Caller holds mw_g.mtx; ex
// must already have pairs[] allocated and enabled=true.
static void
mw_exch_kick_task(mw_exch_t *ex)
{
  char tname[TASK_NAME_SZ];

  if(ex->task != TASK_HANDLE_NONE)
    return;

  snprintf(tname, sizeof(tname), "mw.%s", ex->name);

  ex->task = task_add_periodic(tname, TASK_THREAD, 100,
      ex->poll_sec * 1000u, mw_periodic_cb, ex);

  if(ex->task == TASK_HANDLE_NONE)
    clam(CLAM_WARN, MW_CTX,
        "%s: task_add_periodic FAIL — operator can retry"
        " with /whenmoon mw disable/enable", ex->name);
  else
    clam(CLAM_INFO, MW_CTX, "%s: polling every %us",
        ex->name, ex->poll_sec);
}

static void
mw_exch_cancel_task(mw_exch_t *ex)
{
  if(ex->task == TASK_HANDLE_NONE)
    return;

  task_cancel(ex->task);
  ex->task = TASK_HANDLE_NONE;
}

// ------------------------------------------------------------------ //
// Lifecycle                                                           //
// ------------------------------------------------------------------ //

bool
mw_init(void)
{
  memset(&mw_g, 0, sizeof(mw_g));

  if(pthread_mutex_init(&mw_g.mtx, NULL) != 0)
  {
    clam(CLAM_INFO, MW_CTX, "pthread_mutex_init FAIL");
    return(FAIL);
  }

  // Register KV knobs up front so kv_load picks up persisted values
  // before mw_start consults them. Per-exchange KVs are registered
  // lazily at mw_start (one set per registered exchange).
  if(kv_register(MW_KV_GLOBAL_ENABLED, KV_UINT32, "0", NULL, NULL,
        "Global enable for marketwatch (0/1).") != SUCCESS)
  {
    clam(CLAM_INFO, MW_CTX, "kv_register " MW_KV_GLOBAL_ENABLED " FAIL");
    pthread_mutex_destroy(&mw_g.mtx);
    return(FAIL);
  }

  if(kv_register(MW_KV_RING_N, KV_UINT32, "0", NULL, NULL,
        "Per-pair snapshot ring depth (0 = MW_RING_N_DEFAULT).") != SUCCESS)
  {
    clam(CLAM_INFO, MW_CTX, "kv_register " MW_KV_RING_N " FAIL");
    pthread_mutex_destroy(&mw_g.mtx);
    return(FAIL);
  }

  mw_g.ready = true;
  return(SUCCESS);
}

bool
mw_start(void)
{
  char     names[MW_EXCH_LIST_CAP][EXCHANGE_NAME_SZ];
  uint32_t n_reg = 0;
  uint32_t i;
  uint64_t v;

  mw_g.global_enabled = kv_get_uint(MW_KV_GLOBAL_ENABLED) != 0;

  v = kv_get_uint(MW_KV_RING_N);

  if(v == 0)
    mw_g.ring_n = MW_RING_N_DEFAULT;
  else
    mw_g.ring_n = (uint32_t)v;

  if(exchange_name_list(names, MW_EXCH_LIST_CAP, &n_reg) != SUCCESS)
  {
    clam(CLAM_INFO, MW_CTX, "exchange_name_list FAIL");
    return(SUCCESS);   // empty roster is not fatal
  }

  if(n_reg > MW_EXCH_LIST_CAP)
    n_reg = MW_EXCH_LIST_CAP;

  if(n_reg > MW_MAX_EXCH)
    n_reg = MW_MAX_EXCH;

  pthread_mutex_lock(&mw_g.mtx);

  mw_g.n_exch = 0;

  for(i = 0; i < n_reg; i++)
  {
    mw_exch_t *ex = &mw_g.exch[mw_g.n_exch];
    char       key[MW_KV_KEY_SZ];
    char       help[160];

    memset(ex, 0, sizeof(*ex));
    snprintf(ex->name, sizeof(ex->name), "%s", names[i]);

    if(pthread_mutex_init(&ex->lock, NULL) != 0)
    {
      clam(CLAM_WARN, MW_CTX,
          "%s: pthread_mutex_init FAIL — skipping", ex->name);
      continue;
    }

    // Per-exchange KV registration. Keys are stable strings owned by
    // a single mw_kv_key fill; kv_register copies them internally.
    // A non-SUCCESS return here means the key was already registered
    // (e.g. on a re-entry); the persisted value still applies, so we
    // just continue past with a DBG note.
    mw_kv_key(key, sizeof(key), ex->name, "enabled");
    snprintf(help, sizeof(help),
        "Per-exchange marketwatch enable for %s (0/1).", ex->name);

    if(kv_register(key, KV_UINT32, "0", NULL, NULL, help) != SUCCESS)
      clam(CLAM_DEBUG, MW_CTX,
          "%s: kv_register .enabled already present", ex->name);

    mw_kv_key(key, sizeof(key), ex->name, "poll_sec");
    snprintf(help, sizeof(help),
        "Per-exchange marketwatch poll cadence (s) for %s"
        " (0 = MW_POLL_SEC_DEFAULT).", ex->name);

    if(kv_register(key, KV_UINT32, "0", NULL, NULL, help) != SUCCESS)
      clam(CLAM_DEBUG, MW_CTX,
          "%s: kv_register .poll_sec already present", ex->name);

    // MW-3: per-exchange detector thresholds. Same `0 → default`
    // convention as poll_sec, except min_vol_usd where 0 is a valid
    // operator-set value meaning "no floor" (see
    // mw_load_detector_thresholds for the distinction).
    mw_kv_key(key, sizeof(key), ex->name, "pct_24h_thresh_x100");
    snprintf(help, sizeof(help),
        "MW: 24h-pct threshold for %s, encoded %% × 100"
        " (0 = MW_PCT_24H_THRESH_DEFAULT).", ex->name);
    if(kv_register(key, KV_UINT32, "0", NULL, NULL, help) != SUCCESS)
      clam(CLAM_DEBUG, MW_CTX,
          "%s: kv_register .pct_24h_thresh_x100 already present",
          ex->name);

    mw_kv_key(key, sizeof(key), ex->name, "vel_pct_thresh_x100");
    snprintf(help, sizeof(help),
        "MW: velocity threshold for %s, encoded %% × 100"
        " (0 = MW_VEL_PCT_THRESH_DEFAULT).", ex->name);
    if(kv_register(key, KV_UINT32, "0", NULL, NULL, help) != SUCCESS)
      clam(CLAM_DEBUG, MW_CTX,
          "%s: kv_register .vel_pct_thresh_x100 already present",
          ex->name);

    mw_kv_key(key, sizeof(key), ex->name, "vol_z_thresh_x100");
    snprintf(help, sizeof(help),
        "MW: volume z-score threshold for %s, sigma × 100"
        " (300 = 3.00 sigma). One-sided (positive z only)."
        " 0 = MW_VOL_Z_THRESH_DEFAULT.", ex->name);
    if(kv_register(key, KV_UINT32, "0", NULL, NULL, help) != SUCCESS)
      clam(CLAM_DEBUG, MW_CTX,
          "%s: kv_register .vol_z_thresh_x100 already present",
          ex->name);

    mw_kv_key(key, sizeof(key), ex->name, "vel_window_min");
    snprintf(help, sizeof(help),
        "MW: velocity walk-back window (minutes) for %s"
        " (0 = MW_VEL_WINDOW_MIN_DEFAULT).", ex->name);
    if(kv_register(key, KV_UINT32, "0", NULL, NULL, help) != SUCCESS)
      clam(CLAM_DEBUG, MW_CTX,
          "%s: kv_register .vel_window_min already present", ex->name);

    mw_kv_key(key, sizeof(key), ex->name, "min_vol_usd");
    snprintf(help, sizeof(help),
        "MW: 24h quote-vol floor (USD) for HOT entry on %s"
        " (unset = MW_MIN_VOL_USD_DEFAULT; 0 disables the gate).",
        ex->name);
    if(kv_register(key, KV_UINT64, "0", NULL, NULL, help) != SUCCESS)
      clam(CLAM_DEBUG, MW_CTX,
          "%s: kv_register .min_vol_usd already present", ex->name);

    mw_kv_key(key, sizeof(key), ex->name, "cooldown_sec");
    snprintf(help, sizeof(help),
        "MW: minimum HOT dwell + COOL latch (s) for %s"
        " (0 = MW_COOLDOWN_SEC_DEFAULT).", ex->name);
    if(kv_register(key, KV_UINT32, "0", NULL, NULL, help) != SUCCESS)
      clam(CLAM_DEBUG, MW_CTX,
          "%s: kv_register .cooldown_sec already present", ex->name);

    mw_kv_key(key, sizeof(key), ex->name, "upd_throttle_sec");
    snprintf(help, sizeof(help),
        "MW: minimum interval (s) between UPD re-emits on still-HOT"
        " pairs for %s (0 = MW_UPD_THROTTLE_SEC_DEFAULT).", ex->name);
    if(kv_register(key, KV_UINT32, "0", NULL, NULL, help) != SUCCESS)
      clam(CLAM_DEBUG, MW_CTX,
          "%s: kv_register .upd_throttle_sec already present",
          ex->name);

    // Read persisted values back.
    mw_kv_key(key, sizeof(key), ex->name, "enabled");
    ex->enabled = kv_get_uint(key) != 0;

    mw_kv_key(key, sizeof(key), ex->name, "poll_sec");
    v = kv_get_uint(key);
    ex->poll_sec = v == 0 ? MW_POLL_SEC_DEFAULT : (uint32_t)v;

    mw_load_detector_thresholds(ex);

    ex->task = TASK_HANDLE_NONE;

    if(mw_exch_alloc_pairs(ex, mw_g.ring_n) != SUCCESS)
    {
      clam(CLAM_WARN, MW_CTX,
          "%s: pairs allocation FAIL — skipping", ex->name);
      pthread_mutex_destroy(&ex->lock);
      continue;
    }

    mw_g.n_exch++;

    if(mw_g.global_enabled && ex->enabled)
      mw_exch_kick_task(ex);
  }

  pthread_mutex_unlock(&mw_g.mtx);

  clam(CLAM_INFO, MW_CTX, "started: global=%s ring_n=%u tracked=%u",
      mw_g.global_enabled ? "enabled" : "disabled",
      mw_g.ring_n, mw_g.n_exch);

  return(SUCCESS);
}

void
mw_stop(void)
{
  uint32_t i;

  if(!mw_g.ready)
    return;

  pthread_mutex_lock(&mw_g.mtx);

  for(i = 0; i < mw_g.n_exch; i++)
    mw_exch_cancel_task(&mw_g.exch[i]);

  pthread_mutex_unlock(&mw_g.mtx);
}

void
mw_deinit(void)
{
  uint32_t i;

  if(!mw_g.ready)
    return;

  pthread_mutex_lock(&mw_g.mtx);

  for(i = 0; i < mw_g.n_exch; i++)
  {
    mw_exch_t *ex = &mw_g.exch[i];

    // Tasks were cancelled at mw_stop. task_cancel does not block on a
    // running callback — but the per-callback ex->lock acquisition
    // serialises us against any tail iteration that snuck through.
    pthread_mutex_lock(&ex->lock);
    mw_exch_free_pairs(ex);
    pthread_mutex_unlock(&ex->lock);
    pthread_mutex_destroy(&ex->lock);
  }

  mw_g.n_exch = 0;

  pthread_mutex_unlock(&mw_g.mtx);
  pthread_mutex_destroy(&mw_g.mtx);
  mw_g.ready = false;
}

// ------------------------------------------------------------------ //
// Runtime ops                                                         //
// ------------------------------------------------------------------ //

bool
mw_enable_exch(const char *name)
{
  mw_exch_t *ex;
  char       key[MW_KV_KEY_SZ];
  uint64_t   v;
  bool       kick;

  // Re-read poll_sec at enable time so an operator's `set kv
  // plugin.whenmoon.mw.<exch>.poll_sec <n>` between mw_start and the
  // enable verb actually takes effect on the spawned task.
  mw_kv_key(key, sizeof(key), name, "poll_sec");
  v = kv_get_uint(key);

  pthread_mutex_lock(&mw_g.mtx);

  ex = mw_find_exch_by_name(name);

  if(ex == NULL)
  {
    pthread_mutex_unlock(&mw_g.mtx);
    return(FAIL);
  }

  ex->enabled = true;
  if(v != 0)
    ex->poll_sec = (uint32_t)v;

  // MW-3: pick up any threshold edits made between mw_start and this
  // enable verb. Same rationale as poll_sec — operator hits /set kv,
  // then /whenmoon mw enable; the new values must take effect on the
  // task that's about to be kicked.
  mw_load_detector_thresholds(ex);

  kick = mw_g.global_enabled;

  if(kick)
    mw_exch_kick_task(ex);

  pthread_mutex_unlock(&mw_g.mtx);

  mw_kv_key(key, sizeof(key), name, "enabled");
  (void)kv_set_uint(key, 1);

  return(SUCCESS);
}

bool
mw_disable_exch(const char *name)
{
  mw_exch_t *ex;
  char       key[MW_KV_KEY_SZ];
  uint32_t   i;

  pthread_mutex_lock(&mw_g.mtx);

  ex = mw_find_exch_by_name(name);

  if(ex == NULL)
  {
    pthread_mutex_unlock(&mw_g.mtx);
    return(FAIL);
  }

  ex->enabled = false;
  mw_exch_cancel_task(ex);

  // Clear pair table (slot 0 byte → empty). Ring memory stays
  // allocated for re-enable. Done under ex->lock so an in-flight
  // response cb (which checks ex->enabled at the top under the same
  // lock) cannot observe a half-cleared table.
  pthread_mutex_lock(&ex->lock);

  for(i = 0; i < ex->pair_count; i++)
    ex->pairs[i].product_id[0] = '\0';

  ex->pair_count = 0;

  pthread_mutex_unlock(&ex->lock);
  pthread_mutex_unlock(&mw_g.mtx);

  mw_kv_key(key, sizeof(key), name, "enabled");
  (void)kv_set_uint(key, 0);

  return(SUCCESS);
}

bool
mw_set_global_enabled(bool on)
{
  uint32_t i;

  pthread_mutex_lock(&mw_g.mtx);

  mw_g.global_enabled = on;

  for(i = 0; i < mw_g.n_exch; i++)
  {
    mw_exch_t *ex = &mw_g.exch[i];

    if(!ex->enabled)
      continue;

    if(on)
      mw_exch_kick_task(ex);
    else
      mw_exch_cancel_task(ex);
  }

  pthread_mutex_unlock(&mw_g.mtx);

  (void)kv_set_uint(MW_KV_GLOBAL_ENABLED, on ? 1 : 0);

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Render helpers                                                      //
// ------------------------------------------------------------------ //

static void
mw_fmt_age(char *buf, size_t cap, int64_t now_ms, int64_t then_ms)
{
  int64_t age_s;

  if(then_ms <= 0)
  {
    snprintf(buf, cap, "-");
    return;
  }

  age_s = (now_ms - then_ms) / 1000;

  if(age_s < 0)
    age_s = 0;

  if(age_s < 120)
    snprintf(buf, cap, "%llds ago", (long long)age_s);
  else if(age_s < 7200)
    snprintf(buf, cap, "%lldm ago", (long long)(age_s / 60));
  else
    snprintf(buf, cap, "%lldh ago", (long long)(age_s / 3600));
}

static void
mw_send(method_inst_t *inst, const char *target, const char *text)
{
  if(inst == NULL || target == NULL || target[0] == '\0' || text == NULL)
    return;

  method_send(inst, target, text);
}

void
mw_render_status(method_inst_t *inst, const char *target)
{
  char     line[MW_LINE_SZ];
  uint32_t i;
  int64_t  now_ms;

  pthread_mutex_lock(&mw_g.mtx);

  now_ms = wm_dl_now_ms();

  snprintf(line, sizeof(line),
      "marketwatch global=%s ring_n=%u",
      mw_g.global_enabled ? "enabled" : "disabled",
      mw_g.ring_n);
  mw_send(inst, target, line);

  snprintf(line, sizeof(line),
      "%-12s %-7s %-9s %-15s %6s %6s %6s",
      "exchange", "enabled", "poll_sec", "last_poll",
      "pairs", "polls", "drops");
  mw_send(inst, target, line);

  for(i = 0; i < mw_g.n_exch; i++)
  {
    mw_exch_t *ex = &mw_g.exch[i];
    char       age[32];
    uint32_t   pair_count;
    uint64_t   polls;
    uint64_t   drops;
    int64_t    last_poll_ms;

    pthread_mutex_lock(&ex->lock);
    pair_count   = ex->pair_count;
    polls        = ex->total_polls;
    drops        = ex->total_drops_full;
    last_poll_ms = ex->last_poll_ms;
    pthread_mutex_unlock(&ex->lock);

    mw_fmt_age(age, sizeof(age), now_ms, last_poll_ms);

    snprintf(line, sizeof(line),
        "%-12s %-7s %-9u %-15s %6u %6" PRIu64 " %6" PRIu64,
        ex->name,
        ex->enabled ? "yes" : "no",
        ex->poll_sec,
        age,
        pair_count,
        polls,
        drops);
    mw_send(inst, target, line);
  }

  if(mw_g.n_exch == 0)
    mw_send(inst, target, "(no exchanges registered)");

  pthread_mutex_unlock(&mw_g.mtx);
}

// One per-pair render row, with a pre-computed sort key (|pct_24h| or
// vol_24h_quote). Built per-render under ex->lock; only the latest
// snapshot is read per pair.
typedef struct
{
  char    product_id[EXCHANGE_PRODUCT_ID_SZ];
  double  key;
  double  price;
  double  pct_24h;
  double  vol_24h_quote;
} mw_topn_row_t;

static int
mw_topn_cmp_desc(const void *a, const void *b)
{
  const mw_topn_row_t *ra = a;
  const mw_topn_row_t *rb = b;

  // NaNs sink to the bottom — they never win a top-N spot.
  if(isnan(ra->key) && isnan(rb->key)) return(0);
  if(isnan(ra->key)) return( 1);
  if(isnan(rb->key)) return(-1);
  if(ra->key < rb->key) return( 1);
  if(ra->key > rb->key) return(-1);
  return(0);
}

// Build a top-N table sorted by the given key extractor. `out` is a
// caller-owned array of capacity MW_TOPN; returns the count actually
// populated (<= MW_TOPN). Caller holds ex->lock for the snapshot read.
static uint32_t
mw_build_topn(mw_topn_row_t *out, const mw_exch_t *ex, uint32_t ring_n,
    bool by_vol)
{
  // Heap-allocate the working set so very wide exchanges (Kraken
  // ~1500 pairs) don't blow the thread stack at ~24 bytes/row.
  mw_topn_row_t *work;
  uint32_t       i;
  uint32_t       w = 0;
  uint32_t       n_out;

  if(ex->pair_count == 0)
    return(0);

  work = mem_alloc(WHENMOON_CTX, "mw.topn",
      (size_t)ex->pair_count * sizeof(*work));

  if(work == NULL)
    return(0);

  for(i = 0; i < ex->pair_count; i++)
  {
    const exchange_ticker_snapshot_t *s =
        mw_pair_latest(&ex->pairs[i], ring_n);

    if(s == NULL || ex->pairs[i].product_id[0] == '\0')
      continue;

    snprintf(work[w].product_id, sizeof(work[w].product_id),
        "%s", ex->pairs[i].product_id);
    work[w].price         = s->price;
    work[w].pct_24h       = s->pct_24h;
    work[w].vol_24h_quote = s->vol_24h_quote;

    if(by_vol)
      work[w].key = s->vol_24h_quote;
    else
      work[w].key = isnan(s->pct_24h) ? NAN : fabs(s->pct_24h);

    w++;
  }

  if(w == 0)
  {
    mem_free(work);
    return(0);
  }

  qsort(work, w, sizeof(*work), mw_topn_cmp_desc);

  n_out = w < MW_TOPN ? w : MW_TOPN;

  // Filter out rows whose sort key is NaN — they convey no signal.
  while(n_out > 0 && isnan(work[n_out - 1].key))
    n_out--;

  memcpy(out, work, (size_t)n_out * sizeof(*out));

  mem_free(work);
  return(n_out);
}

static void
mw_fmt_dbl(char *buf, size_t cap, const char *fmt, double v)
{
  if(isnan(v))
    snprintf(buf, cap, "-");
  else
    snprintf(buf, cap, fmt, v);
}

static void
mw_render_topn(method_inst_t *inst, const char *target,
    const mw_topn_row_t *rows, uint32_t n)
{
  char line[MW_LINE_SZ];
  uint32_t i;

  snprintf(line, sizeof(line),
      "  %-14s %-12s %8s %16s",
      "product_id", "price", "pct_24h", "vol_24h_q");
  mw_send(inst, target, line);

  for(i = 0; i < n; i++)
  {
    char price_s[32];
    char pct_s[16];
    char vol_s[32];

    mw_fmt_dbl(price_s, sizeof(price_s), "%.4g", rows[i].price);
    mw_fmt_dbl(pct_s,   sizeof(pct_s),   "%.2f", rows[i].pct_24h);
    mw_fmt_dbl(vol_s,   sizeof(vol_s),   "%.2f", rows[i].vol_24h_quote);

    snprintf(line, sizeof(line),
        "  %-14s %-12s %8s %16s",
        rows[i].product_id, price_s, pct_s, vol_s);
    mw_send(inst, target, line);
  }
}

bool
mw_render_status_exch(method_inst_t *inst, const char *target,
    const char *exch)
{
  mw_exch_t *ex;
  char       line[MW_LINE_SZ];
  char       age[32];
  uint32_t   pair_count;
  uint64_t   polls;
  uint64_t   drops;
  int64_t    last_poll_ms;
  int64_t    now_ms;
  uint32_t   ring_n;
  bool       enabled;
  uint32_t   poll_sec;
  uint32_t   pct_24h_thresh_x100;
  uint32_t   vel_pct_thresh_x100;
  uint32_t   vel_window_ms;
  uint32_t   vol_z_thresh_x100;
  uint64_t   min_vol_usd;
  uint32_t   cooldown_ms;
  uint32_t   upd_throttle_ms;
  uint64_t   emits_hot;
  uint64_t   emits_cool;
  uint64_t   emits_upd;
  mw_topn_row_t *rows_pct;
  mw_topn_row_t *rows_vol;
  uint32_t       n_pct;
  uint32_t       n_vol;
  bool           any_vol;

  pthread_mutex_lock(&mw_g.mtx);

  ex = mw_find_exch_by_name(exch);

  if(ex == NULL)
  {
    pthread_mutex_unlock(&mw_g.mtx);
    return(FAIL);
  }

  ring_n   = mw_g.ring_n;
  enabled  = ex->enabled;
  poll_sec = ex->poll_sec;
  now_ms   = wm_dl_now_ms();

  // Allocate render buffers outside the inner lock so the qsort runs
  // hold ex->lock briefly (snapshot copy only).
  rows_pct = mem_alloc(WHENMOON_CTX, "mw.topn.pct",
      (size_t)MW_TOPN * sizeof(*rows_pct));
  rows_vol = mem_alloc(WHENMOON_CTX, "mw.topn.vol",
      (size_t)MW_TOPN * sizeof(*rows_vol));

  if(rows_pct == NULL || rows_vol == NULL)
  {
    pthread_mutex_unlock(&mw_g.mtx);
    if(rows_pct != NULL) mem_free(rows_pct);
    if(rows_vol != NULL) mem_free(rows_vol);
    return(FAIL);
  }

  pthread_mutex_lock(&ex->lock);

  pair_count           = ex->pair_count;
  polls                = ex->total_polls;
  drops                = ex->total_drops_full;
  last_poll_ms         = ex->last_poll_ms;
  pct_24h_thresh_x100  = ex->pct_24h_thresh_x100;
  vel_pct_thresh_x100  = ex->vel_pct_thresh_x100;
  vel_window_ms        = ex->vel_window_ms;
  vol_z_thresh_x100    = ex->vol_z_thresh_x100;
  min_vol_usd          = ex->min_vol_usd;
  cooldown_ms          = ex->cooldown_ms;
  upd_throttle_ms      = ex->upd_throttle_ms;
  emits_hot            = ex->total_emits_hot;
  emits_cool           = ex->total_emits_cool;
  emits_upd            = ex->total_emits_upd;

  n_pct = mw_build_topn(rows_pct, ex, ring_n, false);
  n_vol = mw_build_topn(rows_vol, ex, ring_n, true);

  // Decide whether the vol section is worth printing — Gemini reports
  // NaN vol_24h_quote, so n_vol ends up zero after NaN filtering.
  any_vol = false;
  {
    uint32_t i;

    for(i = 0; i < n_vol; i++)
    {
      if(!isnan(rows_vol[i].vol_24h_quote)
          && rows_vol[i].vol_24h_quote > 0.0)
      {
        any_vol = true;
        break;
      }
    }
  }

  pthread_mutex_unlock(&ex->lock);
  pthread_mutex_unlock(&mw_g.mtx);

  mw_fmt_age(age, sizeof(age), now_ms, last_poll_ms);

  snprintf(line, sizeof(line),
      "marketwatch exchange=%s enabled=%s poll_sec=%u"
      " last_poll=%s pairs=%u polls=%" PRIu64 " drops=%" PRIu64,
      exch, enabled ? "yes" : "no", poll_sec, age, pair_count,
      polls, drops);
  mw_send(inst, target, line);

  // MW-3: detector thresholds + emit counters. One line apiece so the
  // botmanctl/IRC paths don't word-wrap on narrower terminals.
  snprintf(line, sizeof(line),
      "  thresholds: pct_24h>=%.2f%% velocity>=%.2f%% window=%us"
      " vol_z>=%.2fsig min_vol=%" PRIu64 " cooldown=%us upd=%us",
      (double)pct_24h_thresh_x100 / 100.0,
      (double)vel_pct_thresh_x100 / 100.0,
      (unsigned)(vel_window_ms / 1000u),
      (double)vol_z_thresh_x100 / 100.0,
      min_vol_usd,
      (unsigned)(cooldown_ms / 1000u),
      (unsigned)(upd_throttle_ms / 1000u));
  mw_send(inst, target, line);

  snprintf(line, sizeof(line),
      "  emits: hot=%" PRIu64 " cool=%" PRIu64 " upd=%" PRIu64,
      emits_hot, emits_cool, emits_upd);
  mw_send(inst, target, line);

  if(pair_count == 0)
  {
    mw_send(inst, target, "(no pairs collected)");
    mem_free(rows_pct);
    mem_free(rows_vol);
    return(SUCCESS);
  }

  if(n_pct > 0)
  {
    mw_send(inst, target, "top-10 by |pct_24h|:");
    mw_render_topn(inst, target, rows_pct, n_pct);
  }

  else
    mw_send(inst, target, "top-10 by |pct_24h|: (no data)");

  if(any_vol)
  {
    mw_send(inst, target, "top-10 by vol_24h_quote:");
    mw_render_topn(inst, target, rows_vol, n_vol);
  }

  mem_free(rows_pct);
  mem_free(rows_vol);
  return(SUCCESS);
}
