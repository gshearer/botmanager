// botmanager — MIT
// Kraken Spot exchange service plugin (scaffolding).
//
// KR-3 ships the plugin descriptor, the KV schema, the lifecycle
// hooks, the HMAC-SHA512 signer + nonce minter, and an exchange-
// vtable registration with stubbed required slots. KR-4 lands the
// REST endpoints (candles, orders, fills, balances, assetpairs); the
// WebSocket v2 transport + channel multiplexer land in KR-5. See
// TODO.md §KR-3..KR-6 for the chunk roadmap and README.md for scope.
#define KR_INTERNAL
#include "kraken.h"

#include "kraken_persist.h"

#include "exchange_api.h"
#include "task.h"

// KV schema
//
// All keys live under the `plugin.kraken.*` namespace per core
// convention. The `creds` segment auto-tiers `creds.apikey` and
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
  { "plugin.kraken.creds.apikey",     KV_STR, "", NULL, NULL, NULL },
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

// Handle for the periodic assetpairs refresh task. TASK_HANDLE_NONE
// until kr_start; cancelled in kr_deinit so the daemon shuts down
// cleanly even if a refresh tick was pending.
static task_handle_t kr_assetpairs_task = TASK_HANDLE_NONE;

static void
kr_assetpairs_periodic_cb(task_t *t)
{
  // EXCH-PRIME-1: route through the staleness-aware load path rather
  // than forcing an unconditional network refresh. task_add_periodic
  // runs the first tick immediately on submit, so an unconditional
  // refresh here would hit the network on every (warm) restart; the
  // load path instead re-checks the persisted snapshot's age and only
  // refreshes when it is missing or older than the staleness window —
  // preserving the daily refresh cadence while keeping a warm restart
  // net-zero.
  kr_assetpairs_load_or_refresh_async();
  t->state = TASK_ENDED;
}

static bool
kr_init(void)
{
  kr_sign_init();
  kr_pairs_init();
  kr_rest_init();
  kr_ws_init();
  kr_ws_channels_init();

  // EXCH-PRIME-1: ensure the persisted assetpairs cache table exists.
  // The DB pool is up by plugin_init_all; this is the only synchronous
  // DB touch on the startup path.
  (void)kr_assetpairs_ensure_table();

  clam(CLAM_INFO, KR_CTX, "kraken plugin initialized");

  return(SUCCESS);
}

static bool
kr_start(void)
{
  uint32_t refresh_sec;

  // Self-register with the feature_exchange abstraction so candle
  // traffic + private order/account traffic flows through the
  // priority queue + token bucket.
  if(kr_exchange_register_vtable() != SUCCESS)
  {
    clam(CLAM_WARN, KR_CTX,
        "exchange_register failed — kraken traffic will not dispatch");
    return(FAIL);
  }

  // EXCH-PRIME-1: prime the assetpairs cache from the persisted snapshot
  // and only refresh from the network when it is missing or stale. No
  // synchronous network I/O blocks start(), so the operator control
  // socket comes up without waiting on GET /0/public/AssetPairs.
  //
  // Downstream consumers (e.g. wm_market_restore in whenmoon_start) may
  // dispatch OHLC / WS-subscribe requests before the async load lands,
  // routing abstraction-side symbols (e.g. `BTC-USD`) through
  // kr_pair_lookup_rest / _ws. On a warm restart the DB snapshot
  // repopulates the cache within the same boot, so the prior
  // EQuery:Unknown race is avoided in the common case; a cold/stale
  // cache falls back to the network refresh and pass-through lookups
  // until it lands, matching the periodic-refresh behaviour.
  //
  // The initial prime runs exactly once: when the periodic task is
  // created its first tick fires immediately on submit (task_add_periodic
  // runs the cb at submit time, not after the first interval) and that
  // tick calls kr_assetpairs_load_or_refresh_async, so priming it here
  // too would start a second, concurrent refresh racing on the shared
  // kr_pairs cache. When the periodic task is disabled (refresh_sec == 0)
  // or its submit fails, prime directly.
  refresh_sec = (uint32_t)kv_get_uint("plugin.kraken.assetpairs_refresh_sec");

  if(refresh_sec > 0)
  {
    kr_assetpairs_task = task_add_periodic("kr.assetpairs",
        TASK_THREAD, 50, refresh_sec * 1000u,
        kr_assetpairs_periodic_cb, NULL);

    if(kr_assetpairs_task == TASK_HANDLE_NONE)
    {
      clam(CLAM_WARN, KR_CTX,
          "assetpairs periodic task submit failed; priming directly");
      kr_assetpairs_load_or_refresh_async();
    }
  }

  else
    kr_assetpairs_load_or_refresh_async();

  clam(CLAM_INFO, KR_CTX, "kraken plugin started");

  // KR-5: spawns the persist reader if plugin.kraken.ws_enabled is set.
  // The reader idles in DISCONNECTED until the toggle flips on; flipping
  // the KV at runtime triggers a config-reload via kr_ws_kv_cb.
  kr_ws_start();

  return(SUCCESS);
}

static bool
kr_stop(void)
{
  if(kr_assetpairs_task != TASK_HANDLE_NONE)
  {
    task_cancel(kr_assetpairs_task);
    kr_assetpairs_task = TASK_HANDLE_NONE;
  }

  kr_ws_stop();

  return(SUCCESS);
}

static void
kr_deinit(void)
{
  // Drop our exchange registration first so any in-flight queue is
  // failed back to consumers before we tear down the curl pipeline.
  exchange_unregister("kraken");

  kr_ws_deinit();
  kr_ws_channels_deinit();
  kr_rest_deinit();
  kr_pairs_deinit();
  kr_sign_deinit();

  clam(CLAM_INFO, KR_CTX, "kraken plugin deinitialized");
}

// Plugin descriptor

const plugin_desc_t bm_plugin_desc =
{
  .api_version     = PLUGIN_API_VERSION,
  .name            = "kraken",
  .version         = "0.3-kr5",
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
