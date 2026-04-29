#ifndef BM_WHENMOON_STRATEGY_PUBLIC_H
#define BM_WHENMOON_STRATEGY_PUBLIC_H

// Public cross-plugin surface of the whenmoon strategy ABI. Strategy
// plugins (PLUGIN_STRATEGY, kind = "<strategy_name>") include this
// header to declare the four required exports + (optionally) the
// trade callback. The dlsym shim inlines below resolve whenmoon-side
// helpers (signal emission, ctx user-slot, KV lookups) on first use
// and cache the resolved function pointer.
//
// Shim shape mirrors plugins/inference/inference.h: atomic-guarded
// static cache, union-laundered void* -> fn_t conversion, FATAL +
// abort on lookup miss (which implies a broken plugin-dependency
// graph: the strategy is loaded but feature_whenmoon is not).
//
// Inside the whenmoon plugin itself, the shims must NOT activate
// (they would collide with the real definitions). Translation units
// inside whenmoon define WHENMOON_STRATEGY_INTERNAL before including
// this header to skip the inline block.
//
// This header also publishes the bar-/grain-/indicator-slot types
// strategies index into. aggregator.h (whenmoon-internal) includes
// this header to share the same typedefs, so the type identity is
// guaranteed across the dlsym boundary.
//
// "Be fast" rule: wm_strategy_on_bar runs under the per-market lock
// in the WS / aggregator path. Heavy work (DB, network) belongs on a
// task or worker; the bar-close callback should compute, emit, and
// return.

#include "clam.h"
#include "plugin.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>  // abort

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------
// ABI versioning
// -----------------------------------------------------------------------
//
// Bump on ANY change to the structs / function signatures below.
// whenmoon's loader reads the strategy's wm_strategy_describe and
// rejects strategies whose abi_version does not match.
#define WM_STRATEGY_ABI_VERSION  1

// -----------------------------------------------------------------------
// Bar / grain / indicator types
// -----------------------------------------------------------------------
//
// These are the same types aggregator.h uses. WM-LT-3 promoted them
// here so strategy plugins do not need an include path into the
// internal whenmoon directory.

#define WM_INDICATOR_SCHEMA_VERSION  1

typedef enum
{
  WM_GRAN_1M  = 0,
  WM_GRAN_5M,
  WM_GRAN_15M,
  WM_GRAN_1H,
  WM_GRAN_6H,
  WM_GRAN_1D,
  WM_GRAN_MAX
} wm_gran_t;

// OHLCV + indicator block. Hot path: read by strategies on every
// closed bar. Layout is column-friendly — OHLCV first, indicator
// block as a fixed-size float array — so a strategy can index by
// slot enum rather than chase pointers. ~280 B total.
typedef struct
{
  int64_t  ts_close_ms;
  double   open;
  double   high;
  double   low;
  double   close;
  double   volume;

  float    ind[50];
} wm_candle_full_t;

// Indicator slot enum. Strategies index `ind[]` via these names so
// adding a slot at the end does not break existing strategies. New
// slots go at or after WM_IND_RESERVED_BASE; existing ids never
// shift. Bump WM_INDICATOR_SCHEMA_VERSION regardless.
enum
{
  // moving averages (8 slots)
  WM_IND_SMA_20      = 0,
  WM_IND_SMA_50,
  WM_IND_SMA_200,
  WM_IND_EMA_9,
  WM_IND_EMA_12,
  WM_IND_EMA_20,
  WM_IND_EMA_26,
  WM_IND_EMA_50,

  // MACD (3 slots)
  WM_IND_MACD,
  WM_IND_MACD_SIGNAL,
  WM_IND_MACD_HIST,

  // oscillators (4 slots)
  WM_IND_RSI_14,
  WM_IND_STOCH_K,
  WM_IND_STOCH_D,
  WM_IND_CCI_20,

  // bands + bands-derived (4 slots)
  WM_IND_BB_UPPER,
  WM_IND_BB_MIDDLE,
  WM_IND_BB_LOWER,
  WM_IND_BB_PCTB,

  // volume-weighted (4 slots)
  WM_IND_VWAP,
  WM_IND_OBV,
  WM_IND_MFI_14,
  WM_IND_VPT,

  // volatility / range (4 slots)
  WM_IND_ATR_14,
  WM_IND_TR,
  WM_IND_NATR_14,
  WM_IND_ADX_14,

  // trend / momentum (4 slots)
  WM_IND_ROC_10,
  WM_IND_MOM_10,
  WM_IND_WILLR_14,
  WM_IND_PSAR,

  // microstructure approximations (4 slots)
  WM_IND_BAR_RANGE_PCT,
  WM_IND_BODY_PCT,
  WM_IND_UPPER_WICK_PCT,
  WM_IND_LOWER_WICK_PCT,

  // reserved (15 slots) — append new indicators here so existing slot
  // ids stay stable. Bump WM_INDICATOR_SCHEMA_VERSION on any change.
  WM_IND_RESERVED_BASE,

  WM_IND_COUNT = 50
};

// -----------------------------------------------------------------------
// Param schema
// -----------------------------------------------------------------------

typedef enum
{
  WM_PARAM_INT,
  WM_PARAM_UINT,
  WM_PARAM_DOUBLE,
  WM_PARAM_STR
} wm_param_type_t;

// One declared parameter. Strategies declare their schema as a static
// const array; whenmoon registers a runtime KV under
// `plugin.whenmoon.strategy.<name>.<param>` per row at load time, and
// a per-attachment override slot under
// `plugin.whenmoon.market.<id>.strategy.<name>.<param>` at attach time.
//
// For STR params, only `default_str` is consulted; numeric fields are
// ignored. For INT/UINT, default_int / min_int / max_int apply.
// For DOUBLE, default_dbl / min_dbl / max_dbl / step_dbl apply.
typedef struct
{
  const char       *name;
  wm_param_type_t   type;

  int64_t           default_int;
  int64_t           min_int;
  int64_t           max_int;

  double            default_dbl;
  double            min_dbl;
  double            max_dbl;
  double            step_dbl;        // sweep step; 0 = continuous

  const char       *default_str;
  const char       *help;
} wm_strategy_param_t;

// -----------------------------------------------------------------------
// Strategy metadata
// -----------------------------------------------------------------------

#define WM_STRATEGY_NAME_SZ      64
#define WM_STRATEGY_VERSION_SZ   32
#define WM_STRATEGY_REASON_SZ    64

typedef struct
{
  uint32_t                    abi_version;       // = WM_STRATEGY_ABI_VERSION
  char                        name[WM_STRATEGY_NAME_SZ];     // matches plugin kind
  char                        version[WM_STRATEGY_VERSION_SZ];

  // Bitmask of subscribed grains. Bit (1u << g) for each wm_gran_t g.
  // wm_strategy_on_bar fires only on grains whose bit is set.
  uint16_t                    grains_mask;

  // Per-grain min history (bars). Indexed by wm_gran_t. The aggregator
  // already keeps generous history; this is informational so future
  // chunks can pre-flight backtests against insufficient history.
  uint32_t                    min_history[WM_GRAN_MAX];

  // Param schema. Storage must be static / process-lifetime; the
  // whenmoon registry retains the pointer.
  const wm_strategy_param_t  *params;
  uint32_t                    n_params;

  bool                        wants_trade_callback;
} wm_strategy_meta_t;

// -----------------------------------------------------------------------
// Signal + trade types
// -----------------------------------------------------------------------

typedef struct
{
  int64_t  ts_ms;                          // bar close ts (ms epoch)
  double   score;                          // -1.0 .. +1.0
  double   confidence;                     // 0.0 .. 1.0
  char     reason[WM_STRATEGY_REASON_SZ];
} wm_strategy_signal_t;

typedef struct
{
  int64_t  ts_ms;
  double   price;
  double   size;
  char     side;                           // 'b' or 's'
} wm_trade_t;

// -----------------------------------------------------------------------
// Opaque per-attachment context
// -----------------------------------------------------------------------
//
// Allocated by whenmoon at attach time, freed at detach. Strategies
// stash their private state via wm_strategy_ctx_set_user; whenmoon
// keeps the slot but does not interpret the pointer. The ctx persists
// across reloads only when the user explicitly re-attaches; reload
// detaches all attachments first.

typedef struct wm_strategy_ctx wm_strategy_ctx_t;

// Forward decl for callbacks. The full struct is whenmoon-internal;
// strategies see it only as an opaque pointer.
struct whenmoon_market;

// -----------------------------------------------------------------------
// Required strategy exports
// -----------------------------------------------------------------------
//
// Every PLUGIN_STRATEGY .so must export wm_strategy_describe; the
// other three are required when the strategy is attached via
// /whenmoon strategy attach. Lookup is by name via dlsym at attach
// time, so signatures must match exactly.

void wm_strategy_describe(wm_strategy_meta_t *out);

// Called once per attachment, after whenmoon has registered the
// per-attachment KV override slots. Return 0 on success, non-zero on
// failure — POSIX-style. The whenmoon attach path treats any
// non-zero return as a failed attach and tears down the new ctx.
//
// Note: this is INTENTIONALLY the natural POSIX convention (0 = ok),
// not the project-internal SUCCESS=false / FAIL=true bool convention,
// because strategy plugins are an external authoring surface and 0-on-
// success is the lower-surprise default for that audience.
//
// Strategies typically allocate their private state here and stash it
// via wm_strategy_ctx_set_user.
int wm_strategy_init(wm_strategy_ctx_t *ctx);

// Symmetric to init. Strategies free anything they allocated and
// clear the user slot if they care.
void wm_strategy_finalize(wm_strategy_ctx_t *ctx);

// Bar-close callback. Fires under the per-market lock; keep work
// minimal and signal-only. Strategies that need to act on the signal
// outside the locked region must enqueue a task.
void wm_strategy_on_bar(wm_strategy_ctx_t *ctx,
    const struct whenmoon_market *mkt,
    wm_gran_t grain,
    const wm_candle_full_t *bar);

// Optional, gated by meta.wants_trade_callback. Fires per trade tick
// on the WS reader thread under the per-market lock.
void wm_strategy_on_trade(wm_strategy_ctx_t *ctx,
    const struct whenmoon_market *mkt,
    const wm_trade_t *trade);

// -----------------------------------------------------------------------
// Whenmoon-side helpers (resolved via dlsym shim)
// -----------------------------------------------------------------------

#ifndef WHENMOON_STRATEGY_INTERNAL

// -------- ctx introspection --------

// Strategy's private-state slot. Whenmoon does not interpret the
// pointer. user_data lifetime is the strategy's responsibility.
static inline void
wm_strategy_ctx_set_user(wm_strategy_ctx_t *ctx, void *user)
{
  typedef void (*fn_t)(wm_strategy_ctx_t *, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("whenmoon", "wm_strategy_ctx_set_user_impl");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "whenmoon",
          "dlsym failed: wm_strategy_ctx_set_user_impl");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  fn(ctx, user);
}

static inline void *
wm_strategy_ctx_get_user(wm_strategy_ctx_t *ctx)
{
  typedef void *(*fn_t)(wm_strategy_ctx_t *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("whenmoon", "wm_strategy_ctx_get_user_impl");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "whenmoon",
          "dlsym failed: wm_strategy_ctx_get_user_impl");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(ctx));
}

// Returns the canonical "<exch>-<base>-<quote>" market id this
// attachment is bound to. Pointer is valid for the lifetime of the
// attachment.
static inline const char *
wm_strategy_ctx_market_id(wm_strategy_ctx_t *ctx)
{
  typedef const char *(*fn_t)(wm_strategy_ctx_t *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("whenmoon", "wm_strategy_ctx_market_id_impl");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "whenmoon",
          "dlsym failed: wm_strategy_ctx_market_id_impl");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(ctx));
}

static inline const char *
wm_strategy_ctx_strategy_name(wm_strategy_ctx_t *ctx)
{
  typedef const char *(*fn_t)(wm_strategy_ctx_t *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("whenmoon",
        "wm_strategy_ctx_strategy_name_impl");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "whenmoon",
          "dlsym failed: wm_strategy_ctx_strategy_name_impl");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(ctx));
}

// -------- signal emission --------

// Records the signal on the attachment's internal state (visible via
// /show whenmoon strategy <name>). WM-LT-4 wires this into the trade
// engine.
static inline void
wm_strategy_emit_signal(wm_strategy_ctx_t *ctx,
    const wm_strategy_signal_t *sig)
{
  typedef void (*fn_t)(wm_strategy_ctx_t *, const wm_strategy_signal_t *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("whenmoon", "wm_strategy_emit_signal_impl");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "whenmoon",
          "dlsym failed: wm_strategy_emit_signal_impl");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  fn(ctx, sig);
}

// -------- KV resolvers (two-tier: per-market override -> global -> dflt) --------

static inline uint64_t
wm_strategy_kv_get_uint(const char *market_id, const char *strategy,
    const char *key, uint64_t dflt)
{
  typedef uint64_t (*fn_t)(const char *, const char *, const char *,
      uint64_t);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("whenmoon", "wm_strategy_kv_get_uint");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "whenmoon",
          "dlsym failed: wm_strategy_kv_get_uint");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(market_id, strategy, key, dflt));
}

static inline int64_t
wm_strategy_kv_get_int(const char *market_id, const char *strategy,
    const char *key, int64_t dflt)
{
  typedef int64_t (*fn_t)(const char *, const char *, const char *,
      int64_t);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("whenmoon", "wm_strategy_kv_get_int");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "whenmoon",
          "dlsym failed: wm_strategy_kv_get_int");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(market_id, strategy, key, dflt));
}

static inline double
wm_strategy_kv_get_dbl(const char *market_id, const char *strategy,
    const char *key, double dflt)
{
  typedef double (*fn_t)(const char *, const char *, const char *, double);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("whenmoon", "wm_strategy_kv_get_dbl");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "whenmoon",
          "dlsym failed: wm_strategy_kv_get_dbl");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(market_id, strategy, key, dflt));
}

static inline const char *
wm_strategy_kv_get_str(const char *market_id, const char *strategy,
    const char *key, const char *dflt)
{
  typedef const char *(*fn_t)(const char *, const char *, const char *,
      const char *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("whenmoon", "wm_strategy_kv_get_str");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "whenmoon",
          "dlsym failed: wm_strategy_kv_get_str");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(market_id, strategy, key, dflt));
}

#endif // WHENMOON_STRATEGY_INTERNAL

#ifdef __cplusplus
}
#endif

#endif // BM_WHENMOON_STRATEGY_PUBLIC_H
