// botmanager — MIT
// Gemini Spot exchange service plugin (scaffolding).
//
// GEM-1 ships the plugin descriptor, the KV schema, the lifecycle
// hooks, the HMAC-SHA384 signer + nonce minter, the REST mechanism
// (gem_submit_public / gem_submit_private), the symbols cache, and
// the skeletal exchange-vtable registration. GEM-2 lands the typed
// REST wrappers; GEM-3 wires the WebSocket transport + channel
// multiplexer. See TODO.md §GEM-1..GEM-4 for the chunk roadmap.
#define GEM_INTERNAL
#include "gemini.h"

#include "gemini_persist.h"

#include "exchange_api.h"
#include "task.h"

// ------------------------------------------------------------------
// KV schema
//
// All keys live under the `plugin.gemini.*` namespace per core
// convention. The `creds` segment auto-tiers `creds.api_key` and
// `creds.private_key` as secret (kv_is_secret_key matches any non-
// tail `creds` segment) so they redact for non-admin reads.
//
// `last_nonce` persists the most recently minted nonce so a daemon
// restart never re-uses one. Gemini rejects out-of-order nonces with
// a hard error; the cost of a torn write is one rejected request,
// which the operator clears by bumping the value by hand.
// ------------------------------------------------------------------

static const plugin_kv_entry_t gem_kv_schema[] =
{
  { "plugin.gemini.rest_url",            KV_STR,
    "https://api.gemini.com",            NULL, NULL, NULL },
  { "plugin.gemini.ws_url_marketdata",   KV_STR,
    "wss://api.gemini.com/v2/marketdata",NULL, NULL, NULL },
  { "plugin.gemini.ws_url_order_events", KV_STR,
    "wss://api.gemini.com/v1/order/events", NULL, NULL, NULL },

  // creds segment auto-tiers both keys as secret
  // (kv_is_secret_key matches any non-tail `creds` segment).
  { "plugin.gemini.creds.api_key",       KV_STR, "", NULL, NULL, NULL },
  { "plugin.gemini.creds.private_key",   KV_STR, "", NULL, NULL, NULL },

  // Subsystem toggles.
  { "plugin.gemini.rest_enabled",        KV_BOOL,   "true",  NULL, NULL, NULL },
  { "plugin.gemini.ws_enabled",          KV_BOOL,   "false", NULL, NULL, NULL },

  // Operational budgets.
  { "plugin.gemini.ws_reconnect_ms",     KV_UINT32, "2000",  NULL, NULL, NULL },
  { "plugin.gemini.request_timeout",     KV_UINT32, "15",    NULL, NULL, NULL },
  { "plugin.gemini.symbols_refresh_sec", KV_UINT32, "86400", NULL, NULL, NULL },

  // Monotonic nonce — persisted per-request best-effort so a daemon
  // restart never re-uses one.
  { "plugin.gemini.last_nonce",          KV_UINT64, "0",     NULL, NULL, NULL },
};

// ------------------------------------------------------------------
// Plugin lifecycle
// ------------------------------------------------------------------

// Handle for the periodic symbols refresh task. TASK_HANDLE_NONE
// until gem_start; cancelled in gem_stop so the daemon shuts down
// cleanly even if a refresh tick was pending.
static task_handle_t gem_symbols_task = TASK_HANDLE_NONE;

static void
gem_symbols_periodic_cb(task_t *t)
{
  // EXCH-PRIME-1: route through the staleness-aware load path rather
  // than forcing an unconditional network refresh. task_add_periodic
  // runs the first tick immediately on submit, so an unconditional
  // refresh here would hit the network on every (warm) restart; the
  // load path instead re-checks the persisted snapshot's age and only
  // refreshes when it is missing or older than the staleness window —
  // preserving the daily refresh cadence while keeping a warm restart
  // net-zero.
  gem_symbols_load_or_refresh_async();
  t->state = TASK_ENDED;
}

static bool
gem_init(void)
{
  gem_sign_init();
  gem_pairs_init();
  gem_rest_init();
  gem_ws_init();           // stub in GEM-1; real body in GEM-3
  gem_ws_channels_init();  // stub in GEM-1; real body in GEM-3

  // EXCH-PRIME-1: ensure the persisted symbol cache table exists. The
  // DB pool is up by plugin_init_all; this is the only synchronous DB
  // touch on the startup path.
  (void)gem_symbols_ensure_table();

  clam(CLAM_INFO, GEM_CTX, "gemini plugin initialized");

  return(SUCCESS);
}

static bool
gem_start(void)
{
  uint32_t refresh_sec;

  // Self-register with the feature_exchange abstraction. In GEM-1 the
  // vtable is skeletal (every capability hook NULL except
  // is_authenticated); GEM-2 fills in REST hooks, GEM-3 wires the WS
  // slots. The registration call itself must work in GEM-1 so the
  // daemon sees `gemini` in `/show whenmoon exchange`.
  if(gem_exchange_register_vtable() != SUCCESS)
  {
    clam(CLAM_WARN, GEM_CTX,
        "exchange_register failed — gemini traffic will not dispatch");
    return(FAIL);
  }

  // EXCH-PRIME-1: prime the symbols cache from the persisted snapshot
  // and only refresh from the network when it is missing or stale. No
  // synchronous network I/O blocks start(), so the operator control
  // socket comes up without waiting on Gemini's N+1 /v1/symbols fan-out.
  //
  // The initial prime runs exactly once: when the periodic task is
  // created its first tick fires immediately on submit (task_add_periodic
  // runs the cb at submit time, not after the first interval) and that
  // tick calls gem_symbols_load_or_refresh_async, so priming it here too
  // would start a second, concurrent refresh fan-out racing on the shared
  // gem_pairs cache. When the periodic task is disabled (refresh_sec == 0)
  // or its submit fails, prime directly.
  refresh_sec = (uint32_t)kv_get_uint("plugin.gemini.symbols_refresh_sec");

  if(refresh_sec > 0)
  {
    gem_symbols_task = task_add_periodic("gem.symbols",
        TASK_THREAD, 50, refresh_sec * 1000u,
        gem_symbols_periodic_cb, NULL);

    if(gem_symbols_task == TASK_HANDLE_NONE)
    {
      clam(CLAM_WARN, GEM_CTX,
          "symbols periodic task submit failed; priming directly");
      gem_symbols_load_or_refresh_async();
    }
  }

  else
    gem_symbols_load_or_refresh_async();

  clam(CLAM_INFO, GEM_CTX, "gemini plugin started");

  // GEM-1: no-op stub; GEM-3 spawns the persist reader if
  // plugin.gemini.ws_enabled is set.
  gem_ws_start();

  return(SUCCESS);
}

static bool
gem_stop(void)
{
  if(gem_symbols_task != TASK_HANDLE_NONE)
  {
    task_cancel(gem_symbols_task);
    gem_symbols_task = TASK_HANDLE_NONE;
  }

  gem_ws_stop();

  return(SUCCESS);
}

static void
gem_deinit(void)
{
  // Drop our exchange registration first so any in-flight queue is
  // failed back to consumers before we tear down the curl pipeline.
  exchange_unregister("gemini");

  gem_ws_deinit();
  gem_ws_channels_deinit();
  gem_rest_deinit();
  gem_pairs_deinit();
  gem_sign_deinit();

  clam(CLAM_INFO, GEM_CTX, "gemini plugin deinitialized");
}

// ------------------------------------------------------------------
// Plugin descriptor
// ------------------------------------------------------------------

const plugin_desc_t bm_plugin_desc =
{
  .api_version     = PLUGIN_API_VERSION,
  .name            = "gemini",
  .version         = "1.0-gem4",
  .type            = PLUGIN_SERVICE,
  .kind            = "gemini",
  .provides        = { { .name = "exchange_gemini" } },
  .provides_count  = 1,
  .requires        = { { .name = "feature_exchange" } },
  .requires_count  = 1,
  .kv_schema       = gem_kv_schema,
  .kv_schema_count = sizeof(gem_kv_schema) / sizeof(gem_kv_schema[0]),
  .init            = gem_init,
  .start           = gem_start,
  .stop            = gem_stop,
  .deinit          = gem_deinit,
  .ext             = NULL,
};
