// botmanager — MIT
// feature_exchange plugin: cross-exchange dispatch abstraction.
//
// Owns the priority queue, token bucket, and 429/5xx retry policy for
// every service plugin that provides an exchange interface. Each such
// plugin (e.g. plugins/service/coinbase/) self-registers via
// exchange_register("<name>", &vtable) at its own init time. The
// abstraction's only job at init is to stand up the registry + KV
// schema; provider plugins do their own bookkeeping.

#define EXCHANGE_INTERNAL
#include "exchange.h"

// ------------------------------------------------------------------ //
// KV schema                                                           //
// ------------------------------------------------------------------ //
//
// Per-exchange knobs live under plugin.exchange.<name>.*. Coinbase's
// rate_limit_rps and reserved-slot defaults are baked in here so freshly
// installed daemons get sensible behaviour without a runtime
// kv_register dance. Future exchanges (kraken, ...) extend this table or
// register their knobs at exchange_register() time.

static const plugin_kv_entry_t exchange_kv_schema[] = {
  // Coinbase: 8 rps default leaves headroom under the public 10 rps cap
  // for non-abstraction traffic. Range clamped per-exchange in the
  // limiter (advertised cap minus EXCHANGE_RPS_HEADROOM).
  { "plugin.exchange.coinbase.rate_limit_rps", KV_UINT32, "8",
    "Sustained requests/sec budget for the coinbase exchange dispatch"
    " queue. Range 1..10 (Coinbase public cap). Bucket depth follows"
    " the protocol-advertised burst.",
    NULL, NULL },

  // Reserved tier knobs. P0 keeps one slot by default; P50 backfill has
  // none reserved (token-bucket + queue ordering is enough). Operators
  // can raise reserved.0 if their strategy submits bursty buy/sell
  // pairs.
  { "plugin.exchange.coinbase.reserved_slots.0", KV_UINT32, "1",
    "Tokens reserved for EXCHANGE_PRIO_TRANSACTIONAL traffic. Lower"
    " priorities cannot dispatch unless the bucket has at least"
    " (1 + reserved.0) tokens available.",
    NULL, NULL },

  { "plugin.exchange.coinbase.reserved_slots.50", KV_UINT32, "0",
    "Tokens reserved for EXCHANGE_PRIO_MARKET_BACKFILL. P254 user-"
    "download traffic is gated on (1 + reserved.0 + reserved.50).",
    NULL, NULL },
};

// Apply the per-exchange KV settings to a freshly registered exchange.
// Called from exchange_apply_known_kvs() once at start; protocol plugins
// register before our start runs but the limiter starts with vtable
// defaults — this routine overlays operator knobs if set.

static void
exchange_apply_overrides_for(const char *name, exchange_t *e)
{
  char     key[160];
  uint64_t rps;
  uint64_t r0;
  uint64_t r50;

  if(name == NULL || e == NULL)
    return;

  snprintf(key, sizeof(key), "plugin.exchange.%s.rate_limit_rps", name);

  if(kv_exists(key))
  {
    rps = kv_get_uint(key);

    if(rps > 0)
    {
      pthread_mutex_lock(&e->lock);

      // Recompute tokens_per_sec/cap using the same headroom math as
      // exchange_limiter_init (single source of truth — no copy-paste).
      if(rps > EXCHANGE_RPS_HEADROOM)
        e->limiter.tokens_per_sec = (double)rps - EXCHANGE_RPS_HEADROOM;

      else
        e->limiter.tokens_per_sec = 1.0;

      // Keep the bucket cap proportional (1.5x rps); operators tune via
      // rate_limit_rps directly. burst is rarely tuned independently.
      e->limiter.tokens_cap = e->limiter.tokens_per_sec * 1.5;

      if(e->limiter.tokens > e->limiter.tokens_cap)
        e->limiter.tokens = e->limiter.tokens_cap;

      pthread_mutex_unlock(&e->lock);
    }
  }

  snprintf(key, sizeof(key),
      "plugin.exchange.%s.reserved_slots.0", name);

  if(kv_exists(key))
  {
    r0 = kv_get_uint(key);
    pthread_mutex_lock(&e->lock);
    e->limiter.reserved[EXCHANGE_TIER_TXN] = (uint32_t)r0;
    pthread_mutex_unlock(&e->lock);
  }

  snprintf(key, sizeof(key),
      "plugin.exchange.%s.reserved_slots.50", name);

  if(kv_exists(key))
  {
    r50 = kv_get_uint(key);
    pthread_mutex_lock(&e->lock);
    e->limiter.reserved[EXCHANGE_TIER_BACK] = (uint32_t)r50;
    pthread_mutex_unlock(&e->lock);
  }
}

static void
exchange_apply_overrides(exchange_t *e, void *user)
{
  (void)user;
  exchange_apply_overrides_for(e->name, e);
}

// ------------------------------------------------------------------ //
// Plugin lifecycle                                                    //
// ------------------------------------------------------------------ //

static bool
exchange_init(void)
{
  exchange_registry_init();
  clam(CLAM_INFO, EXCHANGE_CTX, "exchange abstraction initialized");
  return(SUCCESS);
}

static bool
exchange_start(void)
{
  // Protocol plugins (coinbase, ...) self-register from their own
  // init() callback before ours runs (we depend on nothing; they
  // require feature_exchange). Apply the static KV overrides over each
  // already-registered exchange's limiter.
  exchange_registry_iterate(exchange_apply_overrides, NULL);
  return(SUCCESS);
}

static bool
exchange_stop(void)
{
  return(SUCCESS);
}

static void
exchange_deinit(void)
{
  exchange_registry_destroy();
  clam(CLAM_INFO, EXCHANGE_CTX, "exchange abstraction deinitialized");
}

// ------------------------------------------------------------------ //
// Plugin descriptor                                                   //
// ------------------------------------------------------------------ //

const plugin_desc_t bm_plugin_desc = {
  .api_version          = PLUGIN_API_VERSION,
  .name                 = "exchange",
  .version              = "1.0",
  .type                 = PLUGIN_FEATURE,
  .kind                 = "exchange",
  .provides             = { { .name = "feature_exchange" } },
  .provides_count       = 1,
  .requires_count       = 0,
  .kv_schema            = exchange_kv_schema,
  .kv_schema_count      =
      sizeof(exchange_kv_schema) / sizeof(exchange_kv_schema[0]),
  .kv_inst_schema       = NULL,
  .kv_inst_schema_count = 0,
  .init                 = exchange_init,
  .start                = exchange_start,
  .stop                 = exchange_stop,
  .deinit               = exchange_deinit,
  .ext                  = NULL,
};
