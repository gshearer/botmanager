// botmanager — MIT
// Coinbase Exchange service plugin (scaffolding).
//
// This file currently exposes only the plugin descriptor, KV schema
// knobs, and stub lifecycle hooks. REST signing, the REST mechanism
// API, and the WebSocket subsystem land in later chunks; see TODO.md
// §CB1–CB6 for the chunk roadmap and README.md for the scope.
#define CB_INTERNAL
#include "coinbase.h"
#include "exchange_api.h"

// KV schema
//
// Kept intentionally minimal in scaffold. As subsystems come online
// each chunk appends its own rows in a single place — no per-chunk
// renames. All keys live under the `plugin.coinbase.*` namespace per
// the core convention.

static const plugin_kv_entry_t cb_kv_schema[] = {
  // Endpoint selection.
  { "plugin.coinbase.sandbox", KV_BOOL, "false", NULL, NULL, NULL },
  { "plugin.coinbase.rest_url_prod", KV_STR,
    "https://api.exchange.coinbase.com", NULL, NULL, NULL },
  { "plugin.coinbase.rest_url_sandbox", KV_STR,
    "https://api-public.sandbox.exchange.coinbase.com", NULL, NULL, NULL },
  { "plugin.coinbase.ws_url_prod", KV_STR,
    "wss://ws-feed.exchange.coinbase.com", NULL, NULL, NULL },
  { "plugin.coinbase.ws_url_sandbox", KV_STR,
    "wss://ws-feed-public.sandbox.exchange.coinbase.com",
    NULL, NULL, NULL },

  // Authentication. apisecret is the base64 secret exactly as issued
  // by Coinbase; base64-decode happens at signing time. Empty values
  // keep the plugin in public-only mode (market data is unauthenticated).
  { "plugin.coinbase.apikey",     KV_STR, "", NULL, NULL, NULL },
  { "plugin.coinbase.apisecret",  KV_STR, "", NULL, NULL, NULL },
  { "plugin.coinbase.passphrase", KV_STR, "", NULL, NULL, NULL },

  // Subsystem toggles.
  { "plugin.coinbase.rest_enabled", KV_BOOL, "true",  NULL, NULL, NULL },
  { "plugin.coinbase.ws_enabled",   KV_BOOL, "false", NULL, NULL, NULL },

  // Operational budgets.
  { "plugin.coinbase.cache_ttl",       KV_UINT32, "5",  NULL, NULL, NULL },
  { "plugin.coinbase.ws_reconnect_ms", KV_UINT32, "2000", NULL, NULL, NULL },
  { "plugin.coinbase.request_timeout", KV_UINT32, "15", NULL, NULL, NULL },
};

// Plugin lifecycle

static bool
cb_init(void)
{
  cb_rest_init();
  cb_ws_init();
  cb_ws_channels_init();

  // EX-1: self-register with the feature_exchange abstraction so
  // candle + trade traffic gets the priority queue + token bucket.
  // Other coinbase APIs (products, ticker, orders, accounts) keep the
  // legacy direct-curl path for now.
  if(cb_exchange_register_vtable() != SUCCESS)
  {
    clam(CLAM_WARN, CB_CTX,
        "exchange_register failed — candles/trades will not dispatch");
    return(FAIL);
  }

  clam(CLAM_INFO, CB_CTX, "coinbase plugin initialized");

  return(SUCCESS);
}

static bool
cb_start(void)
{
  cb_ws_start();

  return(SUCCESS);
}

static bool
cb_stop(void)
{
  cb_ws_stop();

  return(SUCCESS);
}

static void
cb_deinit(void)
{
  // Drop our exchange registration first so any in-flight queue is
  // failed back to consumers before we tear down the curl pipeline.
  exchange_unregister("coinbase");

  cb_ws_deinit();            // stops reader, frees transport state
  cb_ws_channels_deinit();   // drops every sub handle + slot state
  cb_rest_deinit();

  clam(CLAM_INFO, CB_CTX, "coinbase plugin deinitialized");
}

// Plugin descriptor

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "coinbase",
  .version         = "0.2-ex1",
  .type            = PLUGIN_EXCHANGE,
  .kind            = "coinbase",
  .provides        = { { .name = "exchange_coinbase" } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_exchange" } },
  .requires_count  = 1,
  .kv_schema       = cb_kv_schema,
  .kv_schema_count = sizeof(cb_kv_schema) / sizeof(cb_kv_schema[0]),
  .init            = cb_init,
  .start           = cb_start,
  .stop            = cb_stop,
  .deinit          = cb_deinit,
  .ext             = NULL,
};
