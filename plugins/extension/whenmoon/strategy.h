// strategy.h — internal whenmoon strategy registry + KV resolver.
// Internal; WHENMOON_INTERNAL-gated.
//
// Layered:
//   - whenmoon_strategy.h is the cross-plugin public ABI (dlsym shim).
//   - strategy.h (this file) is the whenmoon-internal registry: load
//     bookkeeping, attachment table, dispatch fan-out from the bar-
//     close path. Strategy plugins never include this header.

#ifndef BM_WHENMOON_STRATEGY_H
#define BM_WHENMOON_STRATEGY_H

#ifdef WHENMOON_INTERNAL

#define WHENMOON_STRATEGY_INTERNAL
#include "whenmoon_strategy.h"
#undef WHENMOON_STRATEGY_INTERNAL

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward declarations.
struct whenmoon_state;
struct whenmoon_market;
typedef struct loaded_strategy loaded_strategy_t;
typedef struct wm_strategy_attachment wm_strategy_attachment_t;

// -----------------------------------------------------------------------
// KV resolver (existing, retained from WM-G1 stub)
// -----------------------------------------------------------------------
//
// Two-tier strategy KV lookup. Resolution order:
//   1. plugin.whenmoon.market.<market_id>.strategy.<strategy>.<key>
//   2. plugin.whenmoon.strategy.<strategy>.<key>
//   3. dflt
//
// market_id is the canonical dash form ("coinbase-btc-usd"). NULL/empty
// market_id skips tier 1 and falls through to the global slot.

uint64_t wm_strategy_kv_get_uint(const char *market_id,
    const char *strategy, const char *key, uint64_t dflt);

int64_t wm_strategy_kv_get_int(const char *market_id,
    const char *strategy, const char *key, int64_t dflt);

double wm_strategy_kv_get_dbl(const char *market_id,
    const char *strategy, const char *key, double dflt);

const char *wm_strategy_kv_get_str(const char *market_id,
    const char *strategy, const char *key, const char *dflt);

// -----------------------------------------------------------------------
// Per-market strategy binding (WM-WARMUP-2 / WM-MI-3)
// -----------------------------------------------------------------------
//
// `plugin.whenmoon.market.<id>.strategy` names the ONE strategy a
// market auto-attaches and warms at start; "" is the registered
// default and means feed-only. wm_market_add registers it and the
// attach/detach verbs keep it in sync.
//
// ⚠ It survives a daemon RESTART and not a plugin RELOAD, which is the
// wrong way round: kv_set marks the entry dirty and only kv_flush()
// writes, nothing on the attach path calls it, and a plugin unload
// reclaims the entry — so the value dies with the mapping (OBS-59).
// That does NOT affect the OBS-45 reads below: whenmoon's KV is still
// registered throughout the cascade window, which is the only moment
// they consult it.
//
// OBS-45: this binding is the only thing that separates "this market
// has no strategy" from "this market's strategy .so is temporarily
// gone". The registry cannot tell them apart and never will — a
// departing strategy's deinit() calls wm_strategy_detach_self, so an
// iterate over it truthfully reports zero attachments for the whole
// length of a reload cascade, which unloads every strategy .so before
// whenmoon's own stop(). A warmup that reads that zero as feed-only
// sizes the market by a roster that is merely invisible.
//
// A .so leaving never touches the binding; only the operator's detach
// verb clears it. That is what makes it the answer.

#define WM_STRATEGY_BINDING_KEY_SZ  160

void wm_strategy_binding_key(const char *market_id_str, char *out,
    size_t cap);

// Copies the declared binding into `out` ("" = feed-only). Reads a
// KV-owned pointer and copies it, so the answer outlives any
// concurrent kv_set.
void wm_strategy_binding_get(const char *market_id_str, char *out,
    size_t cap);

// -----------------------------------------------------------------------
// Strategy ctx (opaque to strategy plugins)
// -----------------------------------------------------------------------
//
// Allocated at attach time, freed at detach. Carries everything the
// strategy callbacks need plus internal bookkeeping rendered by
// /show whenmoon strategy <name>. The shim helpers in
// whenmoon_strategy.h reach into the user_data slot via
// wm_strategy_ctx_set_user / wm_strategy_ctx_get_user.

struct wm_strategy_ctx
{
  // Identity (filled by attach).
  char                  market_id_str[64];
  char                  strategy_name[WM_STRATEGY_NAME_SZ];
  struct whenmoon_market *mkt;     // weak ref; lifetime managed elsewhere

  // Strategy private state slot. Written by wm_strategy_ctx_set_user;
  // whenmoon does not interpret.
  void                 *user;

  // Internal per-attachment counters.
  uint64_t              bars_seen;
  uint64_t              signals_emitted;
  int64_t               last_bar_ts_ms;
  wm_strategy_signal_t  last_signal;
  bool                  has_last_signal;

  // WM-LT-4: cached mark for the trade engine. Populated on every
  // bar-close (bar->close / bar->ts_close_ms) and trade-tick
  // (trade->price / trade->ts_ms) dispatch right before the
  // strategy callback runs. wm_strategy_emit_signal_impl reads
  // these so the signal handler does not need its own market lock
  // to find the mark.
  double                last_mark_px;
  int64_t               last_mark_ms;
};

// -----------------------------------------------------------------------
// Registry types
// -----------------------------------------------------------------------

struct wm_strategy_attachment
{
  // Owning loaded strategy.
  loaded_strategy_t        *owner;

  // Per-attachment context (passed to strategy callbacks).
  wm_strategy_ctx_t         ctx;

  // Linkage in the per-strategy attachment list (owner->attachments).
  wm_strategy_attachment_t *next;
};

struct loaded_strategy
{
  // User-facing name (= meta.name = the plugin's `kind`). KV paths,
  // command output, attach lookups all use this.
  char                      name[WM_STRATEGY_NAME_SZ];

  // Plugin name as the core loader sees it (= bm_plugin_desc.name).
  // Used for plugin_unload during reload — distinct from `name`
  // because strategies follow the naming convention "strategy_<kind>"
  // for the plugin name to keep the loader's name table unambiguous
  // and avoid clashing with non-strategy plugins of the same kind.
  char                      plugin_name[WM_STRATEGY_NAME_SZ];

  char                      version[WM_STRATEGY_VERSION_SZ];
  char                      plugin_path[512];   // captured at first load

  // dlsym-resolved entry points. Cached per-strategy at load time so
  // the dispatch path does not pay the lookup cost per bar.
  void                    (*describe_fn)(wm_strategy_meta_t *);
  int                     (*init_fn)(wm_strategy_ctx_t *);
  void                    (*finalize_fn)(wm_strategy_ctx_t *);
  void                    (*on_bar_fn)(wm_strategy_ctx_t *,
                              const struct whenmoon_market *,
                              wm_gran_t,
                              const wm_candle_full_t *);
  void                    (*on_trade_fn)(wm_strategy_ctx_t *,
                              const struct whenmoon_market *,
                              const wm_trade_t *);

  // Cached meta (param schema kept by reference into the strategy .so).
  wm_strategy_meta_t        meta;

  // Attachment list (per (market, strategy) pair).
  wm_strategy_attachment_t *attachments;
  uint32_t                  n_attachments;

  // Linkage in registry list.
  loaded_strategy_t        *next;
};

struct wm_strategy_registry
{
  pthread_mutex_t      lock;
  loaded_strategy_t   *head;
  uint32_t             n_loaded;
};

// -----------------------------------------------------------------------
// Registry lifecycle (called from whenmoon_init / _deinit)
// -----------------------------------------------------------------------

bool wm_strategy_registry_init(struct whenmoon_state *st);
void wm_strategy_registry_destroy(struct whenmoon_state *st);

// Idempotent: load every PLUGIN_STRATEGY plugin that core's loader has
// already discovered. For each, dlsym wm_strategy_describe, validate
// abi_version, register the param-default KVs at
// plugin.whenmoon.strategy.<name>.<key>, and add a loaded_strategy_t
// to the registry. Called once after plugin_init_all has run.
//
// Returns the number of strategies registered.
uint32_t wm_strategy_registry_scan(struct whenmoon_state *st);

// -----------------------------------------------------------------------
// Attach / detach (called from the strategy admin commands)
// -----------------------------------------------------------------------

typedef enum
{
  WM_ATTACH_OK,
  WM_ATTACH_NO_REGISTRY,
  WM_ATTACH_NO_STRATEGY,
  WM_ATTACH_NO_MARKET,
  WM_ATTACH_DUPLICATE,
  WM_ATTACH_INIT_FAILED,
  WM_ATTACH_OOM,
  WM_ATTACH_OCCUPIED,
} wm_attach_result_t;

// WM-MI-3: a market instance holds exactly ONE strategy. Attaching to
// a market that already has any attachment fails with
// WM_ATTACH_OCCUPIED — detach first; there is no auto-detach.
wm_attach_result_t wm_strategy_attach(struct whenmoon_state *st,
    const char *market_id_str, const char *strategy_name,
    char *err, size_t err_cap);

typedef enum
{
  WM_DETACH_OK,
  WM_DETACH_NO_REGISTRY,
  WM_DETACH_NOT_FOUND,
} wm_detach_result_t;

wm_detach_result_t wm_strategy_detach(struct whenmoon_state *st,
    const char *market_id_str, const char *strategy_name);

// WM-RELOAD-1: cap on the number of MARKETS one strategy's attachments
// can span in a reload snapshot (a strategy may be attached to many
// markets; each market holds at most one attachment per WM-MI-3).
#define WM_RELOAD_MAX_MARKETS  32

// WM-RELOAD-1: one market id captured across a reload so the live
// attachments can be replayed after the dlclose+dlopen cycle.
// Sized to match wm_strategy_ctx.market_id_str.
typedef struct
{
  char  market_id_str[64];
} wm_reattach_snap_t;

// Reload: snapshot every attachment's market, detach them all, drop
// the registry entry, plugin_unload + plugin_load + plugin_resolve +
// plugin_init_all + plugin_start_all, re-scan the registry, then
// replay the snapshot through wm_strategy_attach (WM-RELOAD-1).
// Attach-time WM-SR-1/2 cursor seeding keeps the replayed attachments
// from double-acting on bars the live session already saw.
//
// Returns SUCCESS + writes the detached / re-attached counts to
// `out_n_detached` / `out_n_reattached` (either may be NULL); FAIL on
// lookup miss or a load error (with err populated — the attachments
// are already gone in that case and there is nothing to re-attach
// into). A re-attach miss on a single market (removed mid-reload) is
// CLAM_WARNed and skipped, never a reload failure: compare the two
// counts.
bool wm_strategy_reload(struct whenmoon_state *st,
    const char *strategy_name, uint32_t *out_n_detached,
    uint32_t *out_n_reattached, char *err, size_t err_cap);

// Detach every attachment whose market_id_str matches. Called from
// wm_market_remove so an in-flight stop does not leave attachments
// pointing at freed market state. Returns the number detached;
// no-op when the registry is null or no attachments match.
uint32_t wm_strategy_detach_market(struct whenmoon_state *st,
    const char *market_id_str);

// WM-SR-2: re-seed the WM-SR-1 replay cursor on every attachment of `mk`
// from `mk->session`. Needed because wm_market_restore (which attaches the
// configured strategies) runs BEFORE wm_market_persist_restore_all (which
// hydrates the session), so the seed taken at attach time is always empty
// on the daemon-restart path. Call once per market at the tail of the
// hydration, before warmup replay can feed a bar.
//
// Takes mk->lock and reg->lock, never simultaneously. Returns the number
// of attachments seeded. A market whose session has no last-acted signal
// leaves its attachments' cursors zeroed.
uint32_t wm_strategy_seed_replay_cursors(struct whenmoon_state *st,
    struct whenmoon_market *mk);

// -----------------------------------------------------------------------
// Bar-close fan-out (called from aggregator.c)
// -----------------------------------------------------------------------
//
// Iterates every attachment whose owner subscribes to `gran`, and
// invokes on_bar_fn under the registry lock. Caller MUST hold
// mk->lock; lock order is market_lock -> registry_lock. The bar-close
// callback is "be fast" — strategies that need to do heavier work
// must enqueue a task.

void wm_strategy_dispatch_bar(struct whenmoon_state *st,
    struct whenmoon_market *mkt, wm_gran_t gran,
    const wm_candle_full_t *bar);

// Optional trade-tick dispatch. Same locking discipline as
// dispatch_bar; only invokes on_trade_fn for strategies that opted
// in via meta.wants_trade_callback.
void wm_strategy_dispatch_trade(struct whenmoon_state *st,
    struct whenmoon_market *mkt, const wm_trade_t *trade);

// -----------------------------------------------------------------------
// Iteration helpers (used by /show whenmoon strategy)
// -----------------------------------------------------------------------

typedef void (*wm_strategy_loaded_iter_cb_t)(
    const loaded_strategy_t *ls, void *user);

// Walks a snapshot of the loaded list under the registry lock.
void wm_strategy_loaded_iterate(struct whenmoon_state *st,
    wm_strategy_loaded_iter_cb_t cb, void *user);

// Lookup helper. Returns NULL if not loaded. Caller must NOT free.
// Safe to read fields only while holding the registry lock — for the
// admin views, copy under lock + render after release.
loaded_strategy_t *wm_strategy_find_loaded(struct whenmoon_state *st,
    const char *strategy_name);

// -----------------------------------------------------------------------
// Public dlsym targets (whenmoon_strategy.h shim resolves these)
// -----------------------------------------------------------------------
//
// Defined in strategy.c with default visibility; the dlsym shim in
// whenmoon_strategy.h calls these by symbol name.

void wm_strategy_emit_signal_impl(wm_strategy_ctx_t *ctx,
    const wm_strategy_signal_t *sig);

// OBS-26: called from a strategy plugin's own deinit(), so it takes no
// state handle — it resolves the singleton itself, exactly as the shim
// caller cannot. Finalizes and frees every attachment the named
// strategy holds and returns how many; leaves the registry row for the
// unmap listener to drop. Idempotent, and a no-op for a strategy that
// is not loaded or holds nothing.
uint32_t wm_strategy_detach_self_impl(const char *strategy_name);

void wm_strategy_ctx_set_user_impl(wm_strategy_ctx_t *ctx, void *user);
void *wm_strategy_ctx_get_user_impl(wm_strategy_ctx_t *ctx);
const char *wm_strategy_ctx_market_id_impl(wm_strategy_ctx_t *ctx);
const char *wm_strategy_ctx_strategy_name_impl(wm_strategy_ctx_t *ctx);

// Verb registration (called from whenmoon_init).
bool wm_strategy_register_verbs(void);

// Snapshot helpers used by /show whenmoon strategy <name>.
typedef struct
{
  char                  market_id_str[64];
  uint64_t              bars_seen;
  uint64_t              signals_emitted;
  int64_t               last_bar_ts_ms;
  wm_strategy_signal_t  last_signal;
  bool                  has_last_signal;
} wm_attach_snapshot_t;

// Copy up to `cap` attachment snapshots for the named strategy under
// the registry lock; returns the number written. Returns 0 if the
// strategy is not loaded.
uint32_t wm_strategy_snapshot_attachments(struct whenmoon_state *st,
    const char *strategy_name,
    wm_attach_snapshot_t *out, uint32_t cap);

// Market-centric snapshot: every strategy attached to ONE market, in
// poll order, with its live counters and its *resolved* per-market param
// values. Rendered by /show whenmoon market <id> — the companion to the
// strategy-centric /show whenmoon strategy <name>. `source` records which
// resolver tier supplied each value: 'm' per-market override, 'g' global
// default, 'd' compiled-in schema default.
#define WM_MARKET_SNAP_MAX_PARAMS  16

typedef struct
{
  char  name[WM_STRATEGY_NAME_SZ];
  char  value[48];
  char  source;
} wm_market_strat_param_t;

typedef struct
{
  char                     strategy_name[WM_STRATEGY_NAME_SZ];
  uint64_t                 bars_seen;
  uint64_t                 signals_emitted;
  int64_t                  last_bar_ts_ms;
  wm_strategy_signal_t     last_signal;
  bool                     has_last_signal;
  wm_market_strat_param_t  params[WM_MARKET_SNAP_MAX_PARAMS];
  uint32_t                 n_params;
} wm_market_attach_snapshot_t;

// Copy up to `cap` attached-strategy snapshots for the named market
// under the registry lock. A market holds at most one attachment
// (WM-MI-3), so this returns 0 or 1; 0 if no strategy is attached.
// Each strategy's params are resolved against the live KV at snapshot
// time via the two-tier resolver.
uint32_t wm_strategy_snapshot_market(struct whenmoon_state *st,
    const char *market_id_str,
    wm_market_attach_snapshot_t *out, uint32_t cap);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_STRATEGY_H
