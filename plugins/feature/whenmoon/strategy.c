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
//   - Strategy admin commands take only the registry lock.
//   - Lock order: market_lock -> registry_lock. Strategy commands
//     never take a market lock; nothing inverts this order.

#define WHENMOON_INTERNAL
#include "strategy.h"

#include "market.h"
#include "order.h"
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

  if(kv_register(path, type, def, NULL, NULL,
         p->help != NULL ? p->help : "") != SUCCESS)
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

  if(kv_register(path, type, def, NULL, NULL,
         p->help != NULL ? p->help : "") != SUCCESS)
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

  // WM-LT-4: route the signal to the trade engine. The engine looks up
  // the (market, strategy) trade book; no-op when no book exists or
  // mode == OFF, so a strategy that emits without anyone having
  // configured a trade mode stays purely informational.
  wm_trade_engine_on_signal(ctx->market_id_str, ctx->strategy_name,
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
// function-pointer casts; matches the pattern in
// plugins/inference/inference.h's dlsym shims.
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

// Find a market by canonical id under the markets container. Caller
// must NOT hold mk->lock; the markets container has no lock of its
// own — wm_market_add / _remove serialize through the cmd worker
// thread, which is the only mutator. Safe to read the array length +
// pointers from the dispatch path.
static whenmoon_market_t *
wm_strategy_find_market(whenmoon_state_t *st, const char *market_id_str)
{
  whenmoon_markets_t *m;
  uint32_t            i;

  if(st == NULL || st->markets == NULL || market_id_str == NULL)
    return(NULL);

  m = st->markets;

  for(i = 0; i < m->n_markets; i++)
    if(strncmp(m->arr[i].market_id_str, market_id_str,
           WM_MARKET_ID_STR_SZ) == 0)
      return(&m->arr[i]);

  return(NULL);
}

// ----------------------------------------------------------------------- //
// Attach / detach                                                         //
// ----------------------------------------------------------------------- //

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

  mk = wm_strategy_find_market(st, market_id_str);

  if(mk == NULL)
  {
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

      if(err != NULL)
        snprintf(err, err_cap, "already attached");
      return(WM_ATTACH_DUPLICATE);
    }
  }

  att = mem_alloc("whenmoon", "attach", sizeof(*att));

  if(att == NULL)
  {
    pthread_mutex_unlock(&reg->lock);

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
    mem_free(att);

    if(err != NULL)
      snprintf(err, err_cap, "strategy init failed");
    return(WM_ATTACH_INIT_FAILED);
  }

  att->next        = ls->attachments;
  ls->attachments  = att;
  ls->n_attachments++;

  pthread_mutex_unlock(&reg->lock);

  // WM-PT-3: pre-warm the trade book so a re-attach after SIGTERM
  // hydrates from wm_trade_book_state immediately, and the strategy
  // sees the prior cash/position/fills/PnL/mode rather than waiting
  // for the operator to also re-issue /whenmoon trade mode. For a
  // brand-new attachment with no DB row, this creates a fresh book
  // in mode OFF (no DB write triggered until the first
  // mode-set/fill/reset). Backtest does NOT route through
  // wm_strategy_attach (backtest builds its own ctx + calls init_fn
  // directly in backtest.c) so this path runs only on the cmd-verb
  // /whenmoon strategy attach flow against the global registry.
  (void)wm_trade_book_get_or_create(market_id_str, strategy_name);

  // WM-SR-1: copy the persisted last_signal cursor from the (now-
  // hydrated) book into the per-attachment ctx. wm_strategy_dispatch_bar
  // uses this cursor to drop replay bars whose ts_close_ms is at or
  // before the last signal — without it, the REST candles backfill
  // (300 bars per market start) and the wm_aggregator_load_history_task
  // re-fire every prior signal as the warmup replays through dispatch.
  {
    wm_trade_snapshot_t snap;

    if(wm_trade_book_snapshot(market_id_str, strategy_name, &snap)
           == SUCCESS && snap.has_last_signal)
    {
      att->ctx.last_signal     = snap.last_signal;
      att->ctx.has_last_signal = true;
    }
  }

  clam(CLAM_INFO, WHENMOON_CTX,
      "strategy attach: %s -> %s", strategy_name, market_id_str);

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

  // WM-LT-4: drop the matching trade book (no-op if no /whenmoon trade
  // verb was ever issued for this attachment). Outside the registry
  // lock so the trade engine never inverts strategy_registry ->
  // trade_registry.
  wm_trade_book_remove(market_id_str, strategy_name);

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

  // WM-LT-4: drop every trade book bound to this market. Mirrors the
  // attachment auto-detach so a market stop tears down both layers
  // atomically (from the operator's perspective).
  wm_trade_books_remove_market(market_id_str);

  if(n_detached > 0)
    clam(CLAM_INFO, WHENMOON_CTX,
        "auto-detached %u strategy attachment(s) on market remove: %s",
        n_detached, market_id_str);

  return(n_detached);
}

// ----------------------------------------------------------------------- //
// Reload                                                                  //
// ----------------------------------------------------------------------- //

bool
wm_strategy_reload(whenmoon_state_t *st, const char *strategy_name,
    uint32_t *out_n_detached, char *err, size_t err_cap)
{
  wm_strategy_registry_t  *reg;
  loaded_strategy_t       *ls;
  loaded_strategy_t       *cur;
  loaded_strategy_t      **pp;
  char                     plugin_name[PLUGIN_NAME_SZ];
  char                     path[512];
  uint32_t                 n_detached = 0;
  bool                     have_path  = false;

  if(out_n_detached != NULL)
    *out_n_detached = 0;

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
  if(plugin_unload(plugin_name) != SUCCESS)
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
    plugin_unload(plugin_name);
    return(FAIL);
  }

  if(plugin_init_all() != SUCCESS)
  {
    if(err != NULL)
      snprintf(err, err_cap, "plugin_init_all failed");
    plugin_unload(plugin_name);
    return(FAIL);
  }

  if(plugin_start_all() != SUCCESS)
  {
    if(err != NULL)
      snprintf(err, err_cap, "plugin_start_all failed");
    plugin_unload(plugin_name);
    return(FAIL);
  }

  // Re-scan the registry to pick up the freshly-loaded strategy.
  wm_strategy_registry_scan(st);

  return(SUCCESS);
}

// ----------------------------------------------------------------------- //
// Bar-close fan-out                                                       //
// ----------------------------------------------------------------------- //

void
wm_strategy_dispatch_bar(whenmoon_state_t *st,
    whenmoon_market_t *mkt, wm_gran_t gran,
    const wm_candle_full_t *bar)
{
  wm_strategy_registry_t   *reg;
  loaded_strategy_t        *ls;
  wm_strategy_attachment_t *att;
  uint16_t                  bit;

  if(st == NULL || st->strategies == NULL || mkt == NULL || bar == NULL)
    return;

  if((unsigned)gran >= WM_GRAN_MAX)
    return;

  reg = st->strategies;
  bit = (uint16_t)(1u << gran);

  pthread_mutex_lock(&reg->lock);

  for(ls = reg->head; ls != NULL; ls = ls->next)
  {
    // Skip strategies that did not subscribe to this grain.
    if((ls->meta.grains_mask & bit) == 0)
      continue;

    if(ls->on_bar_fn == NULL)
      continue;

    for(att = ls->attachments; att != NULL; att = att->next)
    {
      // Match by market_id_str rather than pointer: market.c may
      // realloc the markets array (wm_market_grow), invalidating
      // `att->ctx.mkt`. The id string is stable across reallocs and
      // across stop/start cycles, so it is the resilient key.
      if(strncmp(att->ctx.market_id_str, mkt->market_id_str,
             sizeof(att->ctx.market_id_str)) != 0)
        continue;

      // WM-SR-1: drop replay bars that already produced a signal in a
      // prior session. The REST candles backfill + DB warmup task feed
      // historical bars through dispatch_bar with their original
      // ts_close_ms; without this gate every persisted attachment
      // re-emits its prior signals and the paper book accrues
      // duplicate-timestamp fills on every restart. The attach path
      // hydrates last_signal from the persisted book, so a fresh
      // attach with no prior signals still sees every bar.
      if(att->ctx.has_last_signal
          && bar->ts_close_ms <= att->ctx.last_signal.ts_ms)
        continue;

      // Refresh the cached pointer so the strategy callback sees the
      // current mkt for this attachment.
      att->ctx.mkt = mkt;

      att->ctx.bars_seen++;
      att->ctx.last_bar_ts_ms   = bar->ts_close_ms;

      // WM-LT-4: cache the mark for the trade engine. Bar close is
      // the natural mark for a bar-close-driven signal; the engine
      // reads this from the ctx if the strategy emits a signal.
      att->ctx.last_mark_px = bar->close;
      att->ctx.last_mark_ms = bar->ts_close_ms;

      // The callback may emit a signal via wm_strategy_emit_signal,
      // which writes to att->ctx.last_signal in place. The "be fast"
      // contract keeps this hold time bounded.
      ls->on_bar_fn(&att->ctx, mkt, gran, bar);
    }
  }

  pthread_mutex_unlock(&reg->lock);
}

void
wm_strategy_dispatch_trade(whenmoon_state_t *st,
    whenmoon_market_t *mkt, const wm_trade_t *trade)
{
  wm_strategy_registry_t   *reg;
  loaded_strategy_t        *ls;
  wm_strategy_attachment_t *att;

  if(st == NULL || st->strategies == NULL || mkt == NULL || trade == NULL)
    return;

  reg = st->strategies;

  pthread_mutex_lock(&reg->lock);

  for(ls = reg->head; ls != NULL; ls = ls->next)
  {
    if(!ls->meta.wants_trade_callback || ls->on_trade_fn == NULL)
      continue;

    for(att = ls->attachments; att != NULL; att = att->next)
    {
      if(strncmp(att->ctx.market_id_str, mkt->market_id_str,
             sizeof(att->ctx.market_id_str)) != 0)
        continue;

      att->ctx.mkt = mkt;

      // WM-LT-4: trade-tick mark for the trade engine.
      att->ctx.last_mark_px = trade->price;
      att->ctx.last_mark_ms = trade->ts_ms;

      ls->on_trade_fn(&att->ctx, mkt, trade);
    }
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
