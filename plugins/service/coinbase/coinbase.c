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
  { "plugin.coinbase.rest_url", KV_STR,
    "https://api.coinbase.com", NULL, NULL, NULL },
  { "plugin.coinbase.ws_url",   KV_STR,
    "wss://advanced-trade-ws.coinbase.com", NULL, NULL, NULL },

  // Coinbase Developer Platform (CDP) creds. Per-request JWT/ES256
  // signed against `plugin.coinbase.creds.private_key_pem`,
  // identified by `plugin.coinbase.creds.key_name` (the
  // `organizations/<org>/apiKeys/<uuid>` opaque kid). The `creds`
  // segment makes both auto-secret-tier via kv_is_secret_key. PEM
  // may be a single line with literal `\n` escape sequences — the
  // signer translates them to real newlines before parsing. Empty
  // values keep the plugin in public-only mode.
  { "plugin.coinbase.creds.key_name",        KV_STR, "",
    NULL, NULL, NULL },
  { "plugin.coinbase.creds.private_key_pem", KV_STR, "",
    NULL, NULL, NULL },

  // Subsystem toggles.
  { "plugin.coinbase.rest_enabled", KV_BOOL, "true",  NULL, NULL, NULL },
  { "plugin.coinbase.ws_enabled",   KV_BOOL, "false", NULL, NULL, NULL },

  // Operational budgets.
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

  clam(CLAM_INFO, CB_CTX, "coinbase plugin initialized");

  return(SUCCESS);
}

static bool
cb_start(void)
{
  // EX-1: self-register with the feature_exchange abstraction so
  // candle traffic + private order/account traffic flows through the
  // priority queue + token bucket.
  if(cb_exchange_register_vtable() != SUCCESS)
  {
    clam(CLAM_WARN, CB_CTX,
        "exchange_register failed — candles/trades will not dispatch");
    return(FAIL);
  }

  clam(CLAM_INFO, CB_CTX, "coinbase plugin started");

  cb_ws_start();

  return(SUCCESS);
}

static bool
cb_stop(void)
{
  // The reader thread is the plugin's only Class-B holding. If it will
  // not come home, say so — an unload past this point unmaps the code
  // it is standing in.
  return(cb_ws_stop());
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
  cb_cdp_deinit();

  clam(CLAM_INFO, CB_CTX, "coinbase plugin deinitialized");
}

// Plugin descriptor

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "coinbase",
  .version         = "0.2-ex1",
  .type            = PLUGIN_SERVICE,
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
