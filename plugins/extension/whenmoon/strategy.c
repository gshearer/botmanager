// botmanager — MIT
// Whenmoon strategy registry + two-tier KV resolver.
//
// WM-G1 stubbed the resolver (per-market override, global default,
// compiled fallback). WM-LT-3 layers in:
//   - the loaded-strategy table (one row per PLUGIN_STRATEGY plugin),
//   - per-strategy KV registration of param defaults at load time,
//   - per-attachment override slots at attach time,
//   - bar-close fan-out from aggregator.c into the attached strategies'
//     wm_strategy_on_bar callback.
//
// Locking discipline:
//   - The registry has its own mutex (registry->lock).
//   - Bar-close dispatch is called by aggregator.c with the per-market
//     lock held. The dispatch path takes the registry lock briefly to
//     find matching attachments, holds it for the iteration (the
//     "be fast" rule keeps the hold time tiny), then releases.
//   - WM-MK-3-B: dispatch_bar / dispatch_trade DROP mkt->lock around
//     the strategy on_bar_fn / on_trade_fn callback so the strategy's
//     wm_strategy_emit_signal can re-enter wm_market_engine_on_signal
//     (which takes mkt->lock itself) without deadlocking.
//     reg->lock stays held to keep `att` valid — wm_strategy_detach_market
//     takes reg->lock so a concurrent market remove cannot tear down
//     the attachment under us.
//   - Strategy admin commands take only the registry lock.
//   - Lock order: market_lock -> registry_lock. Strategy commands
//     never take a market lock; nothing inverts this order. The
//     dispatch path's mkt->lock re-acquire (after drop) does NOT
//     deadlock because no other code path holds mkt->lock and then
//     tries to take reg->lock.

#define WHENMOON_INTERNAL
#include "strategy.h"

#include "market.h"
#include "market_engine.h"
#include "whenmoon.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "kv.h"
#include "plugin.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ----------------------------------------------------------------------- //
// Two-tier KV resolver                                                    //
// ----------------------------------------------------------------------- //

// Build the per-market path and test for existence. Returns true iff
// the per-market override key is registered (kv_exists).
static bool
wm_strategy_per_market_path(const char *market_id, const char *strategy,
    const char *key, char *out, size_t out_sz)
{
  int n;

  if(market_id == NULL || market_id[0] == '\0' ||
     strategy == NULL || key == NULL)
    return(false);

  n = snprintf(out, out_sz,
      "plugin.whenmoon.market.%s.strategy.%s.%s",
      market_id, strategy, key);

  if(n < 0 || (size_t)n >= out_sz)
    return(false);

  return(kv_exists(out));
}

static bool
wm_strategy_global_path(const char *strategy, const char *key,
    char *out, size_t out_sz)
{
  int n;

  if(strategy == NULL || key == NULL)
    return(false);

  n = snprintf(out, out_sz,
      "plugin.whenmoon.strategy.%s.%s", strategy, key);

  if(n < 0 || (size_t)n >= out_sz)
    return(false);

  return(kv_exists(out));
}

uint64_t
wm_strategy_kv_get_uint(const char *market_id, const char *strategy,
    const char *key, uint64_t dflt)
{
  char path[KV_KEY_SZ];

  if(wm_strategy_per_market_path(market_id, strategy, key,
         path, sizeof(path)))
    return(kv_get_uint(path));

  if(wm_strategy_global_path(strategy, key, path, sizeof(path)))
    return(kv_get_uint(path));

  return(dflt);
}

int64_t
wm_strategy_kv_get_int(const char *market_id, const char *strategy,
    const char *key, int64_t dflt)
{
  char path[KV_KEY_SZ];

  if(wm_strategy_per_market_path(market_id, strategy, key,
         path, sizeof(path)))
    return(kv_get_int(path));

  if(wm_strategy_global_path(strategy, key, path, sizeof(path)))
    return(kv_get_int(path));

  return(dflt);
}

double
wm_strategy_kv_get_dbl(const char *market_id, const char *strategy,
    const char *key, double dflt)
{
  char path[KV_KEY_SZ];

  if(wm_strategy_per_market_path(market_id, strategy, key,
         path, sizeof(path)))
    return(kv_get_double(path));

  if(wm_strategy_global_path(strategy, key, path, sizeof(path)))
    return(kv_get_double(path));

  return(dflt);
}

const char *
wm_strategy_kv_get_str(const char *market_id, const char *strategy,
    const char *key, const char *dflt)
{
  char        path[KV_KEY_SZ];
  const char *v;

  if(wm_strategy_per_market_path(market_id, strategy, key,
         path, sizeof(path)))
  {
    v = kv_get_str(path);

    if(v != NULL)
      return(v);
  }

  if(wm_strategy_global_path(strategy, key, path, sizeof(path)))
  {
    v = kv_get_str(path);

    if(v != NULL)
      return(v);
  }

  return(dflt);
}

// ----------------------------------------------------------------------- //
// Param-default formatting helper                                         //
// ----------------------------------------------------------------------- //
//
// Renders a wm_strategy_param_t default into a string suitable for
// kv_register. KV_INT64 is the most permissive numeric type; we use
// it for both INT and UINT params at registration so range checking
// at set time uses the int64 path. Strategies that need narrower
// types should validate themselves on read.

static void
wm_strategy_format_default(const wm_strategy_param_t *p,
    char *out, size_t out_sz)
{
  if(out == NULL || out_sz == 0)
    return;

  switch(p->type)
  {
    case WM_PARAM_INT:
      snprintf(out, out_sz, "%" PRId64, p->default_int);
      break;

    case WM_PARAM_UINT:
      snprintf(out, out_sz, "%" PRIu64, (uint64_t)p->default_int);
      break;

    case WM_PARAM_DOUBLE:
      snprintf(out, out_sz, "%.10g", p->default_dbl);
      break;

    case WM_PARAM_STR:
      if(p->default_str != NULL)
        snprintf(out, out_sz, "%s", p->default_str);
      else
        out[0] = '\0';
      break;

    default:
      out[0] = '\0';
      break;
  }
}

static kv_type_t
wm_strategy_param_kv_type(wm_param_type_t t)
{
  switch(t)
  {
    case WM_PARAM_INT:    return(KV_INT64);
    case WM_PARAM_UINT:   return(KV_UINT64);
    case WM_PARAM_DOUBLE: return(KV_DOUBLE);
    case WM_PARAM_STR:    return(KV_STR);
  }

  return(KV_STR);
}

// Register the per-strategy global slot. Idempotent: kv_register
// fails if the key already exists (e.g. on reload), which is fine —
// we ignore that condition and keep the existing entry. We can't
// distinguish "already exists" from "failed to register" via the
// return value alone, so we kv_exists first.
static void
wm_strategy_register_global_param(const char *strategy_name,
    const wm_strategy_param_t *p)
{
  char       path[KV_KEY_SZ];
  char       def[64];
  kv_type_t  type;
  int        n;

  n = snprintf(path, sizeof(path),
      "plugin.whenmoon.strategy.%s.%s", strategy_name, p->name);

  if(n < 0 || (size_t)n >= sizeof(path))
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "strategy %s: param '%s' key too long, skipped",
        strategy_name, p->name);
    return;
  }

  if(kv_exists(path))
    return;

  wm_strategy_format_default(p, def, sizeof(def));
  type = wm_strategy_param_kv_type(p->type);

  // Owned by the strategy, not by us: `p` and its help string live in
  // the strategy plugin's .rodata, so the entry has to name that
  // mapping or it survives the strategy's unload with a dangling help
  // pointer -- which is exactly what an unload audit reports.
  if(kv_register_owned(path, type, def, NULL, NULL,
         p->help != NULL ? p->help : "", p) != SUCCESS)
    clam(CLAM_WARN, WHENMOON_CTX,
        "strategy %s: kv_register failed: %s", strategy_name, path);
}

static void
wm_strategy_register_attach_param(const char *market_id_str,
    const char *strategy_name, const wm_strategy_param_t *p)
{
  char       path[KV_KEY_SZ];
  char       def[64];
  kv_type_t  type;
  int        n;

  n = snprintf(path, sizeof(path),
      "plugin.whenmoon.market.%s.strategy.%s.%s",
      market_id_str, strategy_name, p->name);

  if(n < 0 || (size_t)n >= sizeof(path))
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "attach %s/%s: param '%s' key too long, skipped",
        market_id_str, strategy_name, p->name);
    return;
  }

  if(kv_exists(path))
    return;

  wm_strategy_format_default(p, def, sizeof(def));
  type = wm_strategy_param_kv_type(p->type);

  // Same ownership rule as the global slot above: the strategy owns
  // what its own .rodata supplied, however many markets clone it.
  if(kv_register_owned(path, type, def, NULL, NULL,
         p->help != NULL ? p->help : "", p) != SUCCESS)
    clam(CLAM_WARN, WHENMOON_CTX,
        "attach %s/%s: kv_register failed: %s",
        market_id_str, strategy_name, path);
}

// ----------------------------------------------------------------------- //
// Public dlsym targets                                                    //
// ----------------------------------------------------------------------- //

void
wm_strategy_emit_signal_impl(wm_strategy_ctx_t *ctx,
    const wm_strategy_signal_t *sig)
{
  if(ctx == NULL || sig == NULL)
    return;

  ctx->last_signal       = *sig;
  ctx->has_last_signal   = true;
  ctx->signals_emitted++;

  // ctx->mkt is populated by the dispatcher (production: live market
  // under mk->lock; backtest: heap-owned synth market). The direct-
  // pointer engine entry skips the lookup-by-id prelude.
  if(ctx->mkt != NULL)
    wm_market_engine_on_signal_with_mk(ctx->mkt,
        ctx->last_mark_px, ctx->last_mark_ms, sig);
}

void
wm_strategy_ctx_set_user_impl(wm_strategy_ctx_t *ctx, void *user)
{
  if(ctx == NULL)
    return;

  ctx->user = user;
}

void *
wm_strategy_ctx_get_user_impl(wm_strategy_ctx_t *ctx)
{
  if(ctx == NULL)
    return(NULL);

  return(ctx->user);
}

const char *
wm_strategy_ctx_market_id_impl(wm_strategy_ctx_t *ctx)
{
  if(ctx == NULL)
    return("");

  return(ctx->market_id_str);
}

const char *
wm_strategy_ctx_strategy_name_impl(wm_strategy_ctx_t *ctx)
{
  if(ctx == NULL)
    return("");

  return(ctx->strategy_name);
}

// ----------------------------------------------------------------------- //
// Registry lifecycle                                                      //
// ----------------------------------------------------------------------- //

bool
wm_strategy_registry_init(whenmoon_state_t *st)
{
  wm_strategy_registry_t *reg;

  if(st == NULL)
    return(FAIL);

  reg = mem_alloc("whenmoon", "strategy_reg", sizeof(*reg));

  if(reg == NULL)
    return(FAIL);

  memset(reg, 0, sizeof(*reg));

  if(pthread_mutex_init(&reg->lock, NULL) != 0)
  {
    mem_free(reg);
    return(FAIL);
  }

  st->strategies = reg;

  clam(CLAM_INFO, WHENMOON_CTX, "strategy registry initialized");
  return(SUCCESS);
}

// Detach + free a single attachment (no lock; caller holds it).
static void
wm_strategy_free_attachment_locked(loaded_strategy_t *ls,
    wm_strategy_attachment_t *att)
{
  if(att == NULL)
    return;

  if(ls != NULL && ls->finalize_fn != NULL)
    ls->finalize_fn(&att->ctx);

  mem_free(att);
}

// Free a loaded_strategy_t and its attachment list (caller holds lock).
static void
wm_strategy_free_loaded_locked(loaded_strategy_t *ls)
{
  wm_strategy_attachment_t *a;
  wm_strategy_attachment_t *next;

  if(ls == NULL)
    return;

  a = ls->attachments;

  while(a != NULL)
  {
    next = a->next;
    wm_strategy_free_attachment_locked(ls, a);
    a    = next;
  }

  mem_free(ls);
}

void
wm_strategy_registry_destroy(whenmoon_state_t *st)
{
  wm_strategy_registry_t *reg;
  loaded_strategy_t      *ls;
  loaded_strategy_t      *next;

  if(st == NULL || st->strategies == NULL)
    return;

  reg = st->strategies;

  pthread_mutex_lock(&reg->lock);

  ls = reg->head;

  while(ls != NULL)
  {
    next = ls->next;
    wm_strategy_free_loaded_locked(ls);
    ls   = next;
  }

  reg->head     = NULL;
  reg->n_loaded = 0;

  pthread_mutex_unlock(&reg->lock);
  pthread_mutex_destroy(&reg->lock);

  mem_free(reg);
  st->strategies = NULL;

  clam(CLAM_INFO, WHENMOON_CTX, "strategy registry destroyed");
}

// ----------------------------------------------------------------------- //
// Strategy plugin discovery                                               //
// ----------------------------------------------------------------------- //
//
// On boot, iterate every loaded plugin, picking out PLUGIN_STRATEGY
// kinds. Resolve their entry points via plugin_dlsym, validate
// abi_version, register the param-default KVs, and add to the
// registry list.

typedef struct
{
  whenmoon_state_t *st;
  uint32_t          added;
} wm_strategy_scan_ctx_t;

// Resolve the four required entry points + the optional on_trade.
// Returns SUCCESS only when describe/init/finalize/on_bar are all
// non-NULL; on_trade is optional. Logs a CLAM_WARN on miss.
//
// The union dance launders ISO-C-forbidden object-pointer-to-
// function-pointer casts; matches the pattern in the inference
// engine's dlsym shims (plugins/extension/inference/engine/inference.h).
static bool
wm_strategy_resolve_fns(const char *plugin_name, loaded_strategy_t *ls)
{
  void *p;

  p = plugin_dlsym(plugin_name, "wm_strategy_describe");

  if(p == NULL)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "strategy %s: missing wm_strategy_describe", plugin_name);
    return(FAIL);
  }

  {
    union { void *obj; void (*fn)(wm_strategy_meta_t *); } u;

    u.obj = p;
    ls->describe_fn = u.fn;
  }

  p = plugin_dlsym(plugin_name, "wm_strategy_init");

  if(p == NULL)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "strategy %s: missing wm_strategy_init", plugin_name);
    return(FAIL);
  }

  {
    union { void *obj; int (*fn)(wm_strategy_ctx_t *); } u;

    u.obj = p;
    ls->init_fn = u.fn;
  }

  p = plugin_dlsym(plugin_name, "wm_strategy_finalize");

  if(p == NULL)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "strategy %s: missing wm_strategy_finalize", plugin_name);
    return(FAIL);
  }

  {
    union { void *obj; void (*fn)(wm_strategy_ctx_t *); } u;

    u.obj = p;
    ls->finalize_fn = u.fn;
  }

  p = plugin_dlsym(plugin_name, "wm_strategy_on_bar");

  if(p == NULL)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "strategy %s: missing wm_strategy_on_bar", plugin_name);
    return(FAIL);
  }

  {
    union {
      void *obj;
      void (*fn)(wm_strategy_ctx_t *, const struct whenmoon_market *,
          wm_gran_t, const wm_candle_full_t *);
    } u;

    u.obj = p;
    ls->on_bar_fn = u.fn;
  }

  // Optional.
  p = plugin_dlsym(plugin_name, "wm_strategy_on_trade");

  if(p != NULL)
  {
    union {
      void *obj;
      void (*fn)(wm_strategy_ctx_t *, const struct whenmoon_market *,
          const wm_trade_t *);
    } u;

    u.obj = p;
    ls->on_trade_fn = u.fn;
  }

  else
    ls->on_trade_fn = NULL;

  return(SUCCESS);
}

// Build a loaded_strategy_t from a PLUGIN_STRATEGY descriptor + the
// plugin's .so path (passed in directly from the outer plugin_iterate
// callback so we do not have to re-iterate from inside the registry
// lock). Caller takes ownership of the returned pointer; returns NULL
// on failure. Does NOT touch the registry list.
static loaded_strategy_t *
wm_strategy_make_loaded(const plugin_desc_t *pd, const char *plugin_path)
{
  loaded_strategy_t  *ls;
  wm_strategy_meta_t  meta;
  uint32_t            i;

  ls = mem_alloc("whenmoon", "strategy", sizeof(*ls));

  if(ls == NULL)
    return(NULL);

  memset(ls, 0, sizeof(*ls));
  snprintf(ls->name, sizeof(ls->name), "%s", pd->kind);
  snprintf(ls->plugin_name, sizeof(ls->plugin_name), "%s", pd->name);

  if(wm_strategy_resolve_fns(pd->name, ls) != SUCCESS)
  {
    mem_free(ls);
    return(NULL);
  }

  memset(&meta, 0, sizeof(meta));
  ls->describe_fn(&meta);

  if(meta.abi_version != WM_STRATEGY_ABI_VERSION)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "strategy %s: abi_version %u != expected %u, skipping",
        pd->name, meta.abi_version, WM_STRATEGY_ABI_VERSION);
    mem_free(ls);
    return(NULL);
  }

  if(meta.name[0] == '\0')
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "strategy %s: meta.name empty, skipping", pd->name);
    mem_free(ls);
    return(NULL);
  }

  // The strategy's reported name should match its plugin kind. If they
  // differ we trust the meta (it is what /show whenmoon strategy
  // displays and what KV paths use).
  snprintf(ls->name, sizeof(ls->name), "%s", meta.name);
  snprintf(ls->version, sizeof(ls->version), "%s", meta.version);

  ls->meta = meta;

  // Path is only required for /whenmoon strategy reload; not having
  // it is non-fatal.
  if(plugin_path != NULL && plugin_path[0] != '\0')
    snprintf(ls->plugin_path, sizeof(ls->plugin_path), "%s", plugin_path);
  else
  {
    ls->plugin_path[0] = '\0';
    clam(CLAM_INFO, WHENMOON_CTX,
        "strategy %s: plugin path not captured (reload disabled)",
        pd->name);
  }

  // Register global param-default KVs once per strategy.
  for(i = 0; i < meta.n_params; i++)
  {
    if(meta.params == NULL)
      break;

    wm_strategy_register_global_param(ls->name, &meta.params[i]);
  }

  return(ls);
}

// plugin_iterate callback — collect PLUGIN_STRATEGY plugins and add
// each to the registry if not already present.
static void
wm_strategy_scan_iter_cb(const char *name, const char *version,
    const char *path, plugin_type_t type, const char *kind,
    plugin_state_t state, void *data)
{
  wm_strategy_scan_ctx_t  *ctx = data;
  wm_strategy_registry_t  *reg = ctx->st->strategies;
  const plugin_desc_t     *pd;
  loaded_strategy_t       *existing;
  loaded_strategy_t       *ls;

  (void)version;
  (void)path;
  (void)kind;
  (void)state;

  if(type != PLUGIN_STRATEGY)
    return;

  pd = plugin_find(name);

  if(pd == NULL)
    return;

  // Already registered? wm_strategy_registry_scan is idempotent so we
  // can be called more than once across a daemon's life.
  for(existing = reg->head; existing != NULL; existing = existing->next)
    if(strcmp(existing->name, pd->kind) == 0)
      return;

  ls = wm_strategy_make_loaded(pd, path);

  if(ls == NULL)
    return;

  ls->next   = reg->head;
  reg->head  = ls;
  reg->n_loaded++;
  ctx->added++;

  clam(CLAM_INFO, WHENMOON_CTX,
      "strategy registered: %s v%s (grains_mask=0x%04x"
      " params=%u trade_cb=%d path=%s)",
      ls->name, ls->version,
      (unsigned)ls->meta.grains_mask, ls->meta.n_params,
      (int)ls->meta.wants_trade_callback,
      ls->plugin_path[0] != '\0' ? ls->plugin_path : "(unknown)");
}

uint32_t
wm_strategy_registry_scan(whenmoon_state_t *st)
{
  wm_strategy_registry_t *reg;
  wm_strategy_scan_ctx_t  ctx;

  if(st == NULL || st->strategies == NULL)
    return(0);

  reg = st->strategies;

  memset(&ctx, 0, sizeof(ctx));
  ctx.st = st;

  pthread_mutex_lock(&reg->lock);
  plugin_iterate(wm_strategy_scan_iter_cb, &ctx);
  pthread_mutex_unlock(&reg->lock);

  if(ctx.added > 0)
    clam(CLAM_INFO, WHENMOON_CTX,
        "strategy registry scan: %u strategies registered", ctx.added);

  return(ctx.added);
}

// ----------------------------------------------------------------------- //
// Lookup helpers                                                          //
// ----------------------------------------------------------------------- //

loaded_strategy_t *
wm_strategy_find_loaded(whenmoon_state_t *st, const char *strategy_name)
{
  wm_strategy_registry_t *reg;
  loaded_strategy_t      *ls;

  if(st == NULL || st->strategies == NULL || strategy_name == NULL)
    return(NULL);

  reg = st->strategies;

  for(ls = reg->head; ls != NULL; ls = ls->next)
    if(strcmp(ls->name, strategy_name) == 0)
      return(ls);

  return(NULL);
}

// Find a market by canonical id under the markets container.
// WM-MKT-ARR-UAF-1: LOCK-FREE. The caller MUST hold
// `st->markets->arr_lock` (read or write) across this call AND all use of
// the returned pointer — the rwlock keeps arr stable and the returned
// session alive for that span.
static whenmoon_market_t *
wm_strategy_find_market(whenmoon_state_t *st, const char *market_id_str)
{
  whenmoon_markets_t *m;
  uint32_t            i;

  if(st == NULL || st->markets == NULL || market_id_str == NULL)
    return(NULL);

  m = st->markets;

  for(i = 0; i < m->n_markets; i++)
    if(strncmp(m->arr[i]->market_id_str, market_id_str,
           WM_MARKET_ID_STR_SZ) == 0)
      return(m->arr[i]);

  return(NULL);
}

// WM-SR-1/WM-SR-2: seed one attachment's replay cursor from the market's
// restored session. Sole definition of the cursor's meaning — both the
// attach path and the post-hydration re-seed call it so the two cannot
// drift.
//
// The `false` branch is not merely a formality: a market whose persisted
// row carries has_last_signal=f must be left with a zeroed cursor rather
// than inheriting whatever the attachment happened to hold.
//
// Caller holds mk->lock (or otherwise guarantees `sess` is stable).
static void
wm_strategy_seed_cursor_from_session(wm_strategy_ctx_t *ctx,
    const wm_market_session_t *sess)
{
  if(sess->has_last_acted_signal)
  {
    ctx->last_signal     = sess->last_acted_signal;
    ctx->has_last_signal = true;
  }
  else
  {
    memset(&ctx->last_signal, 0, sizeof(ctx->last_signal));
    ctx->has_last_signal = false;
  }
}

// ----------------------------------------------------------------------- //
// Attach / detach                                                         //
// ----------------------------------------------------------------------- //

// WM-MI-3: true iff ANY loaded strategy already has an attachment on
// `market_id_str`. The walk spans every loaded strategy's attachments,
// not just one — the one-per-market invariant is per market, across
// strategies. Caller holds reg->lock.
static bool
wm_strategy_market_has_attachment_locked(wm_strategy_registry_t *reg,
    const char *market_id_str)
{
  loaded_strategy_t        *ls;
  wm_strategy_attachment_t *cur;

  for(ls = reg->head; ls != NULL; ls = ls->next)
  {
    for(cur = ls->attachments; cur != NULL; cur = cur->next)
    {
      if(strncmp(cur->ctx.market_id_str, market_id_str,
             sizeof(cur->ctx.market_id_str)) == 0)
        return(true);
    }
  }

  return(false);
}

wm_attach_result_t
wm_strategy_attach(whenmoon_state_t *st,
    const char *market_id_str, const char *strategy_name,
    char *err, size_t err_cap)
{
  wm_strategy_registry_t   *reg;
  loaded_strategy_t        *ls;
  whenmoon_market_t        *mk;
  wm_strategy_attachment_t *att;
  wm_strategy_attachment_t *cur;
  uint32_t                  i;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(st == NULL || st->strategies == NULL)
    return(WM_ATTACH_NO_REGISTRY);

  reg = st->strategies;

  if(st->markets == NULL)
    return(WM_ATTACH_NO_MARKET);

  // WM-MKT-ARR-UAF-1: hold rdlock across find AND all subsequent use of mk
  // (it is stored into att->ctx.mkt and dereferenced by the strategy init +
  // the WM-SR-1 cursor hydration at the tail). Every exit below releases it.
  // Ordering: arr_lock is taken BEFORE reg->lock; release in reverse.
  pthread_rwlock_rdlock(&st->markets->arr_lock);

  mk = wm_strategy_find_market(st, market_id_str);

  if(mk == NULL)
  {
    pthread_rwlock_unlock(&st->markets->arr_lock);
    if(err != NULL)
      snprintf(err, err_cap, "market %s not running",
          market_id_str != NULL ? market_id_str : "(null)");
    return(WM_ATTACH_NO_MARKET);
  }

  pthread_mutex_lock(&reg->lock);

  ls = wm_strategy_find_loaded(st, strategy_name);

  if(ls == NULL)
  {
    pthread_mutex_unlock(&reg->lock);
    pthread_rwlock_unlock(&st->markets->arr_lock);

    if(err != NULL)
      snprintf(err, err_cap, "strategy %s not loaded",
          strategy_name != NULL ? strategy_name : "(null)");
    return(WM_ATTACH_NO_STRATEGY);
  }

  // Dedup: the same (market, strategy) pair can attach only once.
  for(cur = ls->attachments; cur != NULL; cur = cur->next)
  {
    if(strncmp(cur->ctx.market_id_str, market_id_str,
           sizeof(cur->ctx.market_id_str)) == 0)
    {
      pthread_mutex_unlock(&reg->lock);
      pthread_rwlock_unlock(&st->markets->arr_lock);

      if(err != NULL)
        snprintf(err, err_cap, "already attached");
      return(WM_ATTACH_DUPLICATE);
    }
  }

  // WM-MI-3: one strategy per market instance, enforced here. This is
  // the invariant's sole enforcement point — there is no auto-detach.
  if(wm_strategy_market_has_attachment_locked(reg, market_id_str))
  {
    pthread_mutex_unlock(&reg->lock);
    pthread_rwlock_unlock(&st->markets->arr_lock);

    if(err != NULL)
      snprintf(err, err_cap,
          "market %s already has a strategy attached; detach it first",
          market_id_str);
    return(WM_ATTACH_OCCUPIED);
  }

  att = mem_alloc("whenmoon", "attach", sizeof(*att));

  if(att == NULL)
  {
    pthread_mutex_unlock(&reg->lock);
    pthread_rwlock_unlock(&st->markets->arr_lock);

    if(err != NULL)
      snprintf(err, err_cap, "out of memory");
    return(WM_ATTACH_OOM);
  }

  memset(att, 0, sizeof(*att));
  att->owner = ls;
  snprintf(att->ctx.market_id_str, sizeof(att->ctx.market_id_str),
      "%s", market_id_str);
  snprintf(att->ctx.strategy_name, sizeof(att->ctx.strategy_name),
      "%s", strategy_name);
  att->ctx.mkt = mk;

  // Register the per-attachment override slots BEFORE init runs, so
  // the strategy's init can read them via wm_strategy_kv_get_*.
  for(i = 0; i < ls->meta.n_params; i++)
  {
    if(ls->meta.params == NULL)
      break;

    wm_strategy_register_attach_param(market_id_str, ls->name,
        &ls->meta.params[i]);
  }

  // wm_strategy_init returns 0 on success (POSIX-style) per the
  // public ABI in whenmoon_strategy.h.
  if(ls->init_fn(&att->ctx) != 0)
  {
    pthread_mutex_unlock(&reg->lock);
    pthread_rwlock_unlock(&st->markets->arr_lock);
    mem_free(att);

    if(err != NULL)
      snprintf(err, err_cap, "strategy init failed");
    return(WM_ATTACH_INIT_FAILED);
  }

  att->next        = ls->attachments;
  ls->attachments  = att;
  ls->n_attachments++;

  pthread_mutex_unlock(&reg->lock);

  // WM-SR-1 cursor hydration: seed the per-attachment ctx with the
  // market's last-acted signal (if any) so wm_strategy_dispatch_bar
  // drops replay bars whose ts_close_ms is at or before that cursor.
  // Without this seed the REST candles backfill (300 bars per market
  // start) and the wm_aggregator_load_history_task re-fire every
  // prior signal as the warmup replays through dispatch.
  //
  // WM-SR-2: on the daemon-restart path the session is still zeroed here
  // (wm_market_restore runs before wm_market_persist_restore_all), so this
  // seed is a no-op and wm_strategy_seed_replay_cursors re-runs it once the
  // session has been hydrated. On a runtime attach the session is already
  // live and this is the only seed that happens.
  if(mk != NULL)
  {
    pthread_mutex_lock(&mk->lock);
    wm_strategy_seed_cursor_from_session(&att->ctx, &mk->session);
    pthread_mutex_unlock(&mk->lock);
  }

  // WM-WARMUP-2: warmup is no longer driven from attach. The market
  // lifecycle (wm_market_warmup_begin) owns it now — DB-first gap-fill
  // sized to the attached strategy, replayed via the 1m cascade.
  // The binding auto-attach at market start and the /whenmoon strategy
  // attach command handler invoke wm_market_warmup_begin after the
  // attachment lands, so a runtime attach re-warms to the new depth.

  clam(CLAM_INFO, WHENMOON_CTX,
      "strategy attach: %s -> %s",
      strategy_name, market_id_str);

  pthread_rwlock_unlock(&st->markets->arr_lock);
  return(WM_ATTACH_OK);
}

wm_detach_result_t
wm_strategy_detach(whenmoon_state_t *st,
    const char *market_id_str, const char *strategy_name)
{
  wm_strategy_registry_t    *reg;
  loaded_strategy_t         *ls;
  wm_strategy_attachment_t **pp;
  wm_strategy_attachment_t  *target = NULL;

  if(st == NULL || st->strategies == NULL)
    return(WM_DETACH_NO_REGISTRY);

  reg = st->strategies;

  pthread_mutex_lock(&reg->lock);

  ls = wm_strategy_find_loaded(st, strategy_name);

  if(ls == NULL)
  {
    pthread_mutex_unlock(&reg->lock);
    return(WM_DETACH_NOT_FOUND);
  }

  pp = &ls->attachments;

  while(*pp != NULL)
  {
    if(strncmp((*pp)->ctx.market_id_str, market_id_str,
           sizeof((*pp)->ctx.market_id_str)) == 0)
    {
      target = *pp;
      *pp    = target->next;
      ls->n_attachments--;
      break;
    }

    pp = &(*pp)->next;
  }

  if(target == NULL)
  {
    pthread_mutex_unlock(&reg->lock);
    return(WM_DETACH_NOT_FOUND);
  }

  // Run finalize while still holding the registry lock so the
  // dispatch path cannot race against a half-freed attachment.
  wm_strategy_free_attachment_locked(ls, target);

  pthread_mutex_unlock(&reg->lock);

  clam(CLAM_INFO, WHENMOON_CTX,
      "strategy detach: %s -> %s", strategy_name, market_id_str);

  return(WM_DETACH_OK);
}

// ----------------------------------------------------------------------- //
// Detach-by-market (called from wm_market_remove)                         //
// ----------------------------------------------------------------------- //

uint32_t
wm_strategy_detach_market(whenmoon_state_t *st,
    const char *market_id_str)
{
  wm_strategy_registry_t    *reg;
  loaded_strategy_t         *ls;
  wm_strategy_attachment_t **pp;
  uint32_t                   n_detached = 0;

  if(st == NULL || st->strategies == NULL || market_id_str == NULL)
    return(0);

  reg = st->strategies;

  pthread_mutex_lock(&reg->lock);

  for(ls = reg->head; ls != NULL; ls = ls->next)
  {
    pp = &ls->attachments;

    while(*pp != NULL)
    {
      wm_strategy_attachment_t *cur = *pp;

      if(strncmp(cur->ctx.market_id_str, market_id_str,
             sizeof(cur->ctx.market_id_str)) == 0)
      {
        *pp = cur->next;
        ls->n_attachments--;
        wm_strategy_free_attachment_locked(ls, cur);
        n_detached++;
        continue;
      }

      pp = &cur->next;
    }
  }

  pthread_mutex_unlock(&reg->lock);

  if(n_detached > 0)
    clam(CLAM_INFO, WHENMOON_CTX,
        "auto-detached %u strategy attachment(s) on market remove: %s",
        n_detached, market_id_str);

  return(n_detached);
}

// ----------------------------------------------------------------------- //
// Replay-cursor re-seed (called from wm_market_persist_restore_all)       //
// ----------------------------------------------------------------------- //

uint32_t
wm_strategy_seed_replay_cursors(whenmoon_state_t *st,
    whenmoon_market_t *mk)
{
  wm_strategy_registry_t   *reg;
  loaded_strategy_t        *ls;
  wm_strategy_attachment_t *att;
  wm_market_session_t       snap;
  uint32_t                  n_seeded = 0;

  if(st == NULL || st->strategies == NULL || mk == NULL)
    return(0);

  reg = st->strategies;

  // Snapshot the session under mk->lock and release it BEFORE taking
  // reg->lock. Holding both would invert the dispatch path's ordering
  // (reg->lock held across a mkt->lock re-acquire — see the locking
  // discipline note at the head of this file), so the two locks are
  // deliberately never held together here.
  pthread_mutex_lock(&mk->lock);
  snap = mk->session;
  pthread_mutex_unlock(&mk->lock);

  pthread_mutex_lock(&reg->lock);

  for(ls = reg->head; ls != NULL; ls = ls->next)
  {
    for(att = ls->attachments; att != NULL; att = att->next)
    {
      if(strncmp(att->ctx.market_id_str, mk->market_id_str,
             sizeof(att->ctx.market_id_str)) != 0)
        continue;

      wm_strategy_seed_cursor_from_session(&att->ctx, &snap);
      n_seeded++;
    }
  }

  pthread_mutex_unlock(&reg->lock);

  if(n_seeded > 0 && snap.has_last_acted_signal)
    clam(CLAM_INFO, WHENMOON_CTX,
        "replay cursor seeded on %u attachment(s) of %s (ts=%" PRId64 ")",
        n_seeded, mk->market_id_str, snap.last_acted_signal.ts_ms);

  return(n_seeded);
}

// ----------------------------------------------------------------------- //
// Reload                                                                  //
// ----------------------------------------------------------------------- //

bool
wm_strategy_reload(whenmoon_state_t *st, const char *strategy_name,
    uint32_t *out_n_detached, uint32_t *out_n_reattached,
    char *err, size_t err_cap)
{
  wm_strategy_registry_t   *reg;
  loaded_strategy_t        *ls;
  loaded_strategy_t        *cur;
  loaded_strategy_t       **pp;
  wm_strategy_attachment_t *walk;
  wm_reattach_snap_t        snap[WM_RELOAD_MAX_MARKETS];
  char                      plugin_name[PLUGIN_NAME_SZ];
  char                      path[512];
  uint32_t                  n_detached    = 0;
  uint32_t                  n_snap        = 0;
  uint32_t                  n_trunc       = 0;
  uint32_t                  n_reattached  = 0;
  uint32_t                  i;
  bool                      have_path     = false;

  if(out_n_detached != NULL)
    *out_n_detached = 0;

  if(out_n_reattached != NULL)
    *out_n_reattached = 0;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(st == NULL || st->strategies == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "registry not ready");
    return(FAIL);
  }

  reg = st->strategies;

  pthread_mutex_lock(&reg->lock);

  ls = wm_strategy_find_loaded(st, strategy_name);

  if(ls == NULL)
  {
    pthread_mutex_unlock(&reg->lock);

    if(err != NULL)
      snprintf(err, err_cap, "strategy %s not loaded",
          strategy_name != NULL ? strategy_name : "(null)");
    return(FAIL);
  }

  // Capture the loader-side plugin name and .so path while we still
  // hold a reference. plugin_unload uses the loader's plugin name
  // (= pd->name = "strategy_<kind>"), not the strategy's user-facing
  // name (= meta.name = pd->kind).
  snprintf(plugin_name, sizeof(plugin_name), "%s", ls->plugin_name);

  if(ls->plugin_path[0] != '\0')
  {
    snprintf(path, sizeof(path), "%s", ls->plugin_path);
    have_path = true;
  }

  // WM-RELOAD-1: capture the market of every attachment so the reload
  // can replay them once the fresh .so is registered. Overflow is
  // counted and warned after the lock drops.
  for(walk = ls->attachments; walk != NULL; walk = walk->next)
  {
    if(n_snap >= WM_RELOAD_MAX_MARKETS)
    {
      n_trunc++;
      continue;
    }

    snprintf(snap[n_snap].market_id_str,
        sizeof(snap[n_snap].market_id_str), "%s",
        walk->ctx.market_id_str);
    n_snap++;
  }

  // Detach every attachment. Each finalize fires under the lock —
  // matches the normal detach path.
  while(ls->attachments != NULL)
  {
    wm_strategy_attachment_t *a = ls->attachments;

    ls->attachments = a->next;
    ls->n_attachments--;

    wm_strategy_free_attachment_locked(ls, a);
    n_detached++;
  }

  // Drop the loaded_strategy_t from the registry list.
  pp = &reg->head;

  while(*pp != NULL)
  {
    cur = *pp;

    if(cur == ls)
    {
      *pp = cur->next;
      reg->n_loaded--;
      break;
    }

    pp = &cur->next;
  }

  // Free the registry entry itself. The strategy plugin's .so will
  // be closed by plugin_unload below; clearing our cached function
  // pointers first protects against any stale dispatch.
  ls->describe_fn = NULL;
  ls->init_fn     = NULL;
  ls->finalize_fn = NULL;
  ls->on_bar_fn   = NULL;
  ls->on_trade_fn = NULL;
  mem_free(ls);

  pthread_mutex_unlock(&reg->lock);

  // plugin_unload + plugin_load happen outside the registry lock so
  // we don't invert with any locks the loader takes internally.
  if(plugin_unload(plugin_name, NULL) != SUCCESS)
  {
    if(err != NULL)
      snprintf(err, err_cap, "plugin_unload(%s) failed", plugin_name);
    return(FAIL);
  }

  if(out_n_detached != NULL)
    *out_n_detached = n_detached;

  if(!have_path)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "unloaded %s but no path captured; restart daemon to reload",
          plugin_name);
    return(FAIL);
  }

  if(plugin_load(path) != SUCCESS)
  {
    if(err != NULL)
      snprintf(err, err_cap, "plugin_load(%s) failed", path);
    return(FAIL);
  }

  if(plugin_resolve() != SUCCESS)
  {
    if(err != NULL)
      snprintf(err, err_cap, "plugin_resolve failed");
    plugin_unload(plugin_name, NULL);
    return(FAIL);
  }

  if(plugin_init_all() != SUCCESS)
  {
    if(err != NULL)
      snprintf(err, err_cap, "plugin_init_all failed");
    plugin_unload(plugin_name, NULL);
    return(FAIL);
  }

  if(plugin_start_all() != SUCCESS)
  {
    if(err != NULL)
      snprintf(err, err_cap, "plugin_start_all failed");
    plugin_unload(plugin_name, NULL);
    return(FAIL);
  }

  // Re-scan the registry to pick up the freshly-loaded strategy.
  wm_strategy_registry_scan(st);

  if(n_trunc > 0)
    clam(CLAM_WARN, WHENMOON_CTX,
        "reload %s: %u attachment(s) beyond the %u snapshot cap were"
        " dropped — re-attach them manually", strategy_name, n_trunc,
        (uint32_t)WM_RELOAD_MAX_MARKETS);

  // WM-RELOAD-1: replay the snapshot. wm_strategy_attach takes the
  // registry lock itself, so this must run unlocked. A single-market
  // miss (removed mid-reload) is logged and skipped — the reload
  // itself stands; callers compare the two counts.
  for(i = 0; i < n_snap; i++)
  {
    char aerr[128];

    aerr[0] = '\0';

    if(wm_strategy_attach(st, snap[i].market_id_str, strategy_name,
           aerr, sizeof(aerr)) == WM_ATTACH_OK)
    {
      n_reattached++;
      continue;
    }

    clam(CLAM_WARN, WHENMOON_CTX,
        "reload %s: re-attach %s failed: %s",
        strategy_name, snap[i].market_id_str,
        aerr[0] != '\0' ? aerr : "unknown");
  }

  if(n_reattached > 0)
    clam(CLAM_INFO, WHENMOON_CTX,
        "reload %s: re-attached %u/%u live attachment(s)",
        strategy_name, n_reattached, n_detached);

  if(out_n_reattached != NULL)
    *out_n_reattached = n_reattached;

  return(SUCCESS);
}

// ----------------------------------------------------------------------- //
// Bar-close dispatch (one attachment per market, WM-MI-3)                 //
// ----------------------------------------------------------------------- //

// Render the per-attachment audit line. `sig` may be NULL — that means
// the attachment was polled but did not emit a fresh signal (HOLD).
static void
wm_strategy_log_advice(const whenmoon_market_t *mkt,
    const wm_strategy_attachment_t *att, int64_t tick_ts_ms,
    const wm_strategy_signal_t *sig)
{
  const char *advice;
  double      score;
  double      conf;
  const char *reason;

  if(sig == NULL || sig->score == 0.0)
  {
    advice = "HOLD";
    score  = 0.0;
    conf   = 0.0;
    reason = "";
  }

  else
  {
    advice = sig->score > 0.0 ? "BUY" : "SELL";
    score  = sig->score;
    conf   = sig->confidence;
    reason = sig->reason;
  }

  clam(CLAM_INFO, WHENMOON_CTX,
      "whenmoon.advice: market %s strategy=%s tick_ts=%" PRId64
      " advice=%s score=%.4f conf=%.2f reason=\"%s\"",
      mkt->market_id_str, att->owner->name,
      tick_ts_ms, advice, score, conf, reason);
}

// Map a wm_gran_t to its grains_mask bit. Caller already validated
// `gran < WM_GRAN_MAX`.
static inline uint16_t
wm_strategy_gran_bit(wm_gran_t gran)
{
  return((uint16_t)(1u << (unsigned)gran));
}

// Caller holds reg->lock. A market has at most one attachment
// (WM-MI-3), so this is a find, not a collect. `gran_bit == 0` means
// "no grain filter, caller wants the trade-tick variant"; that path
// filters on `wants_trade_callback` instead.
static wm_strategy_attachment_t *
wm_strategy_find_market_attachment_locked(wm_strategy_registry_t *reg,
    whenmoon_market_t *mkt, uint16_t gran_bit, bool trade_path)
{
  loaded_strategy_t        *ls;
  wm_strategy_attachment_t *att;

  for(ls = reg->head; ls != NULL; ls = ls->next)
  {
    if(trade_path)
    {
      if(!ls->meta.wants_trade_callback || ls->on_trade_fn == NULL)
        continue;
    }

    else
    {
      if((ls->meta.grains_mask & gran_bit) == 0)
        continue;

      if(ls->on_bar_fn == NULL)
        continue;
    }

    for(att = ls->attachments; att != NULL; att = att->next)
    {
      if(strncmp(att->ctx.market_id_str, mkt->market_id_str,
             sizeof(att->ctx.market_id_str)) == 0)
        return(att);
    }
  }

  return(NULL);
}

void
wm_strategy_dispatch_bar(whenmoon_state_t *st,
    whenmoon_market_t *mkt, wm_gran_t gran,
    const wm_candle_full_t *bar)
{
  wm_strategy_registry_t   *reg;
  wm_strategy_attachment_t *att;
  int64_t                   pre_emit_ts;
  bool                      emitted_this_tick;

  if(st == NULL || st->strategies == NULL || mkt == NULL || bar == NULL)
    return;

  if((unsigned)gran >= WM_GRAN_MAX)
    return;

  reg = st->strategies;

  pthread_mutex_lock(&reg->lock);

  att = wm_strategy_find_market_attachment_locked(reg, mkt,
      wm_strategy_gran_bit(gran), false);

  if(att == NULL)
  {
    pthread_mutex_unlock(&reg->lock);
    return;
  }

  // WM-SR-1 replay gate: drop bars that already produced a signal in
  // a prior session.
  if(att->ctx.has_last_signal
      && bar->ts_close_ms <= att->ctx.last_signal.ts_ms)
  {
    pthread_mutex_unlock(&reg->lock);
    return;
  }

  // Match by id, not pointer: market.c may realloc the markets array
  // (wm_market_grow). Refresh the cached pointer so the callback
  // sees the current mkt for this attachment.
  att->ctx.mkt            = mkt;
  att->ctx.bars_seen++;
  att->ctx.last_bar_ts_ms = bar->ts_close_ms;
  att->ctx.last_mark_px   = bar->close;
  att->ctx.last_mark_ms   = bar->ts_close_ms;

  pre_emit_ts = att->ctx.has_last_signal
      ? att->ctx.last_signal.ts_ms : 0;

  // WM-MK-3-B: copy the bar onto the stack so the strategy callback
  // sees a stable view, then drop mkt->lock around the callback. The
  // callback may emit a signal that re-enters
  // wm_market_engine_on_signal, which takes mkt->lock itself; without
  // the drop we'd deadlock. reg->lock stays held to keep `att` valid
  // (wm_market_remove takes reg->lock via wm_strategy_detach_market
  // before tearing the slot down, so dropping mkt->lock here cannot
  // race a market remove). The original `bar` pointer (into mkt's
  // grain ring) may be invalidated by a concurrent push during the
  // drop window; post-callback reads use the cached ts_close_ms
  // snapshot below.
  {
    wm_candle_full_t bar_copy   = *bar;
    int64_t          ts_close_ms = bar->ts_close_ms;

    pthread_mutex_unlock(&mkt->lock);
    att->owner->on_bar_fn(&att->ctx, mkt, gran, &bar_copy);
    pthread_mutex_lock(&mkt->lock);

    emitted_this_tick = att->ctx.has_last_signal
        && att->ctx.last_signal.ts_ms != pre_emit_ts
        && att->ctx.last_signal.ts_ms == ts_close_ms;

    wm_strategy_log_advice(mkt, att, ts_close_ms,
        emitted_this_tick ? &att->ctx.last_signal : NULL);
  }

  pthread_mutex_unlock(&reg->lock);
}

void
wm_strategy_dispatch_trade(whenmoon_state_t *st,
    whenmoon_market_t *mkt, const wm_trade_t *trade)
{
  wm_strategy_registry_t   *reg;
  wm_strategy_attachment_t *att;
  int64_t                   pre_emit_ts;
  bool                      emitted_this_tick;

  if(st == NULL || st->strategies == NULL || mkt == NULL || trade == NULL)
    return;

  reg = st->strategies;

  pthread_mutex_lock(&reg->lock);

  att = wm_strategy_find_market_attachment_locked(reg, mkt, 0, true);

  if(att == NULL)
  {
    pthread_mutex_unlock(&reg->lock);
    return;
  }

  att->ctx.mkt          = mkt;
  att->ctx.last_mark_px = trade->price;
  att->ctx.last_mark_ms = trade->ts_ms;

  pre_emit_ts = att->ctx.has_last_signal
      ? att->ctx.last_signal.ts_ms : 0;

  // WM-MK-3-B: copy trade onto stack and drop mkt->lock around the
  // callback (same shape as dispatch_bar). The trade pointer's
  // backing storage is the WS event payload — it is not part of
  // mkt's mutable state — but matching the bar dispatch's
  // copy-then-drop discipline keeps the locking pattern uniform.
  {
    wm_trade_t trade_copy = *trade;
    int64_t    ts_ms      = trade->ts_ms;

    pthread_mutex_unlock(&mkt->lock);
    att->owner->on_trade_fn(&att->ctx, mkt, &trade_copy);
    pthread_mutex_lock(&mkt->lock);

    emitted_this_tick = att->ctx.has_last_signal
        && att->ctx.last_signal.ts_ms != pre_emit_ts;

    wm_strategy_log_advice(mkt, att, ts_ms,
        emitted_this_tick ? &att->ctx.last_signal : NULL);
  }

  pthread_mutex_unlock(&reg->lock);
}

// ----------------------------------------------------------------------- //
// Iteration / snapshot helpers                                            //
// ----------------------------------------------------------------------- //

void
wm_strategy_loaded_iterate(whenmoon_state_t *st,
    wm_strategy_loaded_iter_cb_t cb, void *user)
{
  wm_strategy_registry_t *reg;
  loaded_strategy_t      *ls;

  if(st == NULL || st->strategies == NULL || cb == NULL)
    return;

  reg = st->strategies;

  pthread_mutex_lock(&reg->lock);

  for(ls = reg->head; ls != NULL; ls = ls->next)
    cb(ls, user);

  pthread_mutex_unlock(&reg->lock);
}

uint32_t
wm_strategy_snapshot_attachments(whenmoon_state_t *st,
    const char *strategy_name,
    wm_attach_snapshot_t *out, uint32_t cap)
{
  wm_strategy_registry_t   *reg;
  loaded_strategy_t        *ls;
  wm_strategy_attachment_t *att;
  uint32_t                  n = 0;

  if(st == NULL || st->strategies == NULL || out == NULL || cap == 0)
    return(0);

  reg = st->strategies;

  pthread_mutex_lock(&reg->lock);

  ls = wm_strategy_find_loaded(st, strategy_name);

  if(ls == NULL)
  {
    pthread_mutex_unlock(&reg->lock);
    return(0);
  }

  for(att = ls->attachments; att != NULL && n < cap; att = att->next)
  {
    wm_attach_snapshot_t *snap = &out[n];

    snprintf(snap->market_id_str, sizeof(snap->market_id_str), "%s",
        att->ctx.market_id_str);
    snap->bars_seen        = att->ctx.bars_seen;
    snap->signals_emitted  = att->ctx.signals_emitted;
    snap->last_bar_ts_ms   = att->ctx.last_bar_ts_ms;
    snap->last_signal      = att->ctx.last_signal;
    snap->has_last_signal  = att->ctx.has_last_signal;

    n++;
  }

  pthread_mutex_unlock(&reg->lock);

  return(n);
}

// Resolve one param's effective value for a market and record which
// tier supplied it. Mirrors the wm_strategy_kv_get_* resolution order
// (per-market override -> global default -> compiled default) so the
// rendered value matches what the strategy's init() actually reads.
static void
wm_strategy_resolve_param(const char *market_id, const char *strategy,
    const wm_strategy_param_t *p, wm_market_strat_param_t *out)
{
  char path[KV_KEY_SZ];
  bool from_market;
  bool from_global = false;

  snprintf(out->name, sizeof(out->name), "%s", p->name);

  from_market = wm_strategy_per_market_path(market_id, strategy, p->name,
      path, sizeof(path));

  if(!from_market)
    from_global = wm_strategy_global_path(strategy, p->name,
        path, sizeof(path));

  out->source = from_market ? 'm' : (from_global ? 'g' : 'd');

  switch(p->type)
  {
    case WM_PARAM_INT:
      if(out->source == 'd')
        snprintf(out->value, sizeof(out->value), "%" PRId64,
            p->default_int);
      else
        snprintf(out->value, sizeof(out->value), "%" PRId64,
            kv_get_int(path));
      break;

    case WM_PARAM_UINT:
      if(out->source == 'd')
        snprintf(out->value, sizeof(out->value), "%" PRIu64,
            (uint64_t)p->default_int);
      else
        snprintf(out->value, sizeof(out->value), "%" PRIu64,
            kv_get_uint(path));
      break;

    case WM_PARAM_DOUBLE:
      if(out->source == 'd')
        snprintf(out->value, sizeof(out->value), "%.6g", p->default_dbl);
      else
        snprintf(out->value, sizeof(out->value), "%.6g",
            kv_get_double(path));
      break;

    case WM_PARAM_STR:
      if(out->source == 'd')
        snprintf(out->value, sizeof(out->value), "%s",
            p->default_str != NULL ? p->default_str : "");
      else
      {
        const char *v = kv_get_str(path);

        snprintf(out->value, sizeof(out->value), "%s",
            v != NULL ? v : "");
      }
      break;

    default:
      out->value[0] = '\0';
      break;
  }
}

uint32_t
wm_strategy_snapshot_market(whenmoon_state_t *st,
    const char *market_id_str,
    wm_market_attach_snapshot_t *out, uint32_t cap)
{
  wm_strategy_registry_t   *reg;
  loaded_strategy_t        *ls;
  wm_strategy_attachment_t *att;
  uint32_t                  n = 0;

  if(st == NULL || st->strategies == NULL || market_id_str == NULL ||
     out == NULL || cap == 0)
    return(0);

  reg = st->strategies;

  pthread_mutex_lock(&reg->lock);

  for(ls = reg->head; ls != NULL && n < cap; ls = ls->next)
  {
    for(att = ls->attachments; att != NULL && n < cap; att = att->next)
    {
      wm_market_attach_snapshot_t *snap;
      uint32_t                     np;
      uint32_t                     j;

      if(strcmp(att->ctx.market_id_str, market_id_str) != 0)
        continue;

      snap = &out[n];

      snprintf(snap->strategy_name, sizeof(snap->strategy_name), "%s",
          ls->name);
      snap->bars_seen        = att->ctx.bars_seen;
      snap->signals_emitted  = att->ctx.signals_emitted;
      snap->last_bar_ts_ms   = att->ctx.last_bar_ts_ms;
      snap->last_signal      = att->ctx.last_signal;
      snap->has_last_signal  = att->ctx.has_last_signal;

      np = ls->meta.n_params;

      if(np > WM_MARKET_SNAP_MAX_PARAMS)
        np = WM_MARKET_SNAP_MAX_PARAMS;

      if(ls->meta.params == NULL)
        np = 0;

      for(j = 0; j < np; j++)
        wm_strategy_resolve_param(market_id_str, ls->name,
            &ls->meta.params[j], &snap->params[j]);

      snap->n_params = np;

      n++;
    }
  }

  pthread_mutex_unlock(&reg->lock);

  return(n);
}
