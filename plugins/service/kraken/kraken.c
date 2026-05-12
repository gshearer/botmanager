// botmanager — MIT
// Kraken Spot exchange service plugin (scaffolding).
//
// KR-3 ships the plugin descriptor, the KV schema, the lifecycle
// hooks, the HMAC-SHA512 signer + nonce minter, and an exchange-
// vtable registration with stubbed required slots. REST endpoints
// land in KR-4; the WebSocket v2 transport + channel multiplexer
// land in KR-5. See TODO.md §KR-3..KR-6 for the chunk roadmap and
// README.md for scope.
#define KR_INTERNAL
#include "kraken.h"
#include "exchange_api.h"

// KV schema
//
// All keys live under the `plugin.kraken.*` namespace per core
// convention. The `creds` segment auto-tiers `creds.api_key` and
// `creds.private_key` as secret (kv_is_secret_key matches any non-
// tail `creds` segment) so they redact for non-admin reads.
//
// `last_nonce` persists the most recently minted nonce so a daemon
// restart never re-uses one. Kraken rejects out-of-order nonces with
// `EAPI:Invalid nonce`; the cost of a torn write is one rejected
// request, which the operator clears by incrementing the value by
// hand.

static const plugin_kv_entry_t kr_kv_schema[] =
{
  { "plugin.kraken.rest_url",       KV_STR,
    "https://api.kraken.com",       NULL, NULL, NULL },
  { "plugin.kraken.ws_url_public",  KV_STR,
    "wss://ws.kraken.com/v2",       NULL, NULL, NULL },
  { "plugin.kraken.ws_url_private", KV_STR,
    "wss://ws-auth.kraken.com/v2",  NULL, NULL, NULL },

  // Kraken Spot REST credentials. The base64-encoded `private_key` is
  // decoded once and cached by the signer; the `api_key` is sent
  // verbatim in the `API-Key` header. The `creds` segment auto-tiers
  // both as secret.
  { "plugin.kraken.creds.api_key",     KV_STR, "", NULL, NULL, NULL },
  { "plugin.kraken.creds.private_key", KV_STR, "", NULL, NULL, NULL },

  // Subsystem toggles.
  { "plugin.kraken.rest_enabled", KV_BOOL, "true",  NULL, NULL, NULL },
  { "plugin.kraken.ws_enabled",   KV_BOOL, "false", NULL, NULL, NULL },

  // Operational budgets.
  { "plugin.kraken.ws_reconnect_ms",        KV_UINT32, "2000",
    NULL, NULL, NULL },
  { "plugin.kraken.request_timeout",        KV_UINT32, "15",
    NULL, NULL, NULL },
  { "plugin.kraken.assetpairs_refresh_sec", KV_UINT32, "86400",
    NULL, NULL, NULL },

  // Monotonic nonce — persisted per-request best-effort so a daemon
  // restart never re-uses a nonce.
  { "plugin.kraken.last_nonce",             KV_UINT64, "0",
    NULL, NULL, NULL },
};

// Plugin lifecycle

static bool
kr_init(void)
{
  kr_sign_init();
  kr_rest_init();
  kr_ws_init();
  kr_ws_channels_init();

  clam(CLAM_INFO, KR_CTX, "kraken plugin initialized");

  return(SUCCESS);
}

static bool
kr_start(void)
{
  // Self-register with the feature_exchange abstraction so candle
  // traffic + private order/account traffic flows through the
  // priority queue + token bucket. The required slots are stubbed
  // FAIL until KR-4 wires the REST surface; the capability hooks
  // stay NULL until KR-4 / KR-5 fill them.
  if(kr_exchange_register_vtable() != SUCCESS)
  {
    clam(CLAM_WARN, KR_CTX,
        "exchange_register failed — kraken traffic will not dispatch");
    return(FAIL);
  }

  clam(CLAM_INFO, KR_CTX, "kraken plugin started");

  // WS start deferred to KR-5 wiring; kr_ws_start is a no-op today.
  kr_ws_start();

  return(SUCCESS);
}

static bool
kr_stop(void)
{
  kr_ws_stop();

  return(SUCCESS);
}

static void
kr_deinit(void)
{
  // Drop our exchange registration first so any in-flight queue is
  // failed back to consumers before we tear down the curl pipeline
  // (when KR-4 wires it up).
  exchange_unregister("kraken");

  kr_ws_deinit();
  kr_ws_channels_deinit();
  kr_rest_deinit();
  kr_sign_deinit();

  clam(CLAM_INFO, KR_CTX, "kraken plugin deinitialized");
}

// Plugin descriptor

const plugin_desc_t bm_plugin_desc =
{
  .api_version     = PLUGIN_API_VERSION,
  .name            = "kraken",
  .version         = "0.1-kr3",
  .type            = PLUGIN_SERVICE,
  .kind            = "kraken",
  .provides        = { { .name = "exchange_kraken" } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_exchange" } },
  .requires_count  = 1,
  .kv_schema       = kr_kv_schema,
  .kv_schema_count = sizeof(kr_kv_schema) / sizeof(kr_kv_schema[0]),
  .init            = kr_init,
  .start           = kr_start,
  .stop            = kr_stop,
  .deinit          = kr_deinit,
  .ext             = NULL,
};
