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

#include "exchange_api.h"
#include "task.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>

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
  // Fire and forget — the response handler updates the cache and
  // logs any failure. cb=NULL routes to the silent logger in
  // gemini_orders.c.
  (void)gemini_symbols_refresh_async(NULL, NULL);
  t->state = TASK_ENDED;
}

// Synchronous first prime of the symbols cache. Runs from gem_start
// before any consumer that depends on symbol translation has a chance
// to dispatch. Bounded by a 10 s timeout so a Gemini outage or DNS
// hiccup never wedges plugin loading — on timeout / failure, lookups
// fall through with a heuristic translation and the periodic task
// re-primes on its interval.

// Heap-allocated + refcounted because the async refresh batch can
// outlive the prime barrier's timeout. With Gemini's N+1 fan-out
// (listing + per-symbol detail GETs) the batch routinely takes 30+ s
// at the 5 rps default rate limit, while the prime barrier expires
// at 10 s. A stack-allocated sync struct (Kraken's pattern, safe
// there because Kraken's refresh is one fast REST call) would be
// reclaimed under the still-running batch's feet — the eventual
// completion callback would lock a destroyed mutex and write to
// stale stack memory, crashing the daemon with no coredump because
// the standard launch path runs under `ulimit -c 0`. Two refs:
// one for the prime function, one for the callback; whichever side
// finishes last frees.
typedef struct
{
  pthread_mutex_t lock;
  pthread_cond_t  cond;
  bool            done;
  bool            ok;
  unsigned        refs;
  char            err[GEMINI_ERR_SZ];
} gem_prime_sync_t;

// Drop a ref under the lock; destroy + free when the last holder
// releases. The mutex/cond must survive every legitimate access, so
// teardown is the responsibility of the final releaser only.
static void
gem_prime_sync_release(gem_prime_sync_t *s)
{
  bool last;

  if(s == NULL)
    return;

  pthread_mutex_lock(&s->lock);
  s->refs--;
  last = (s->refs == 0);
  pthread_mutex_unlock(&s->lock);

  if(last)
  {
    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->lock);
    mem_free(s);
  }
}

static void
gem_prime_done_cb(const gemini_symbols_result_t *res, void *user)
{
  gem_prime_sync_t *s = user;

  if(s == NULL)
    return;

  pthread_mutex_lock(&s->lock);

  s->done = true;
  s->ok   = (res != NULL && res->err[0] == '\0');

  if(res != NULL)
    snprintf(s->err, sizeof(s->err), "%s", res->err);

  pthread_cond_signal(&s->cond);
  pthread_mutex_unlock(&s->lock);

  gem_prime_sync_release(s);
}

static bool
gem_prime_symbols_sync(unsigned timeout_ms)
{
  gem_prime_sync_t *s;
  struct timespec   deadline;
  int               rc = 0;
  bool              ok;

  s = mem_alloc(GEM_CTX, "prime.sync", sizeof(*s));

  if(s == NULL)
    return(FAIL);

  memset(s, 0, sizeof(*s));
  pthread_mutex_init(&s->lock, NULL);
  pthread_cond_init(&s->cond, NULL);
  s->refs = 2;          // one for us, one for the callback

  if(gemini_symbols_refresh_async(gem_prime_done_cb, s) != SUCCESS)
  {
    // Submit failure already invoked the cb synchronously inside the
    // refresh function; s->done is true, s->ok is false, and the cb
    // has already released its ref (refs now == 1, owned by us).
  }

  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec  += (time_t)(timeout_ms / 1000u);
  deadline.tv_nsec += (long)((timeout_ms % 1000u) * 1000000UL);

  if(deadline.tv_nsec >= 1000000000L)
  {
    deadline.tv_sec  += 1;
    deadline.tv_nsec -= 1000000000L;
  }

  pthread_mutex_lock(&s->lock);

  while(!s->done && rc != ETIMEDOUT)
    rc = pthread_cond_timedwait(&s->cond, &s->lock, &deadline);

  ok = s->done && s->ok;

  if(!ok)
  {
    if(rc == ETIMEDOUT)
      clam(CLAM_WARN, GEM_CTX,
          "symbols prime timed out after %u ms; lookups will pass"
          " through until the periodic refresh succeeds", timeout_ms);
    else
      clam(CLAM_WARN, GEM_CTX,
          "symbols prime failed: %s; lookups will pass through"
          " until the periodic refresh succeeds",
          s->err[0] != '\0' ? s->err : "(no error string)");
  }

  pthread_mutex_unlock(&s->lock);

  gem_prime_sync_release(s);    // drop our ref; cb may still hold one

  return(ok ? SUCCESS : FAIL);
}

static bool
gem_init(void)
{
  gem_sign_init();
  gem_pairs_init();
  gem_rest_init();
  gem_ws_init();           // stub in GEM-1; real body in GEM-3
  gem_ws_channels_init();  // stub in GEM-1; real body in GEM-3

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

  // Block until the symbols cache is primed (or the prime times out
  // / fails). Bounded by 10 s — same rationale as
  // kr_prime_assetpairs_sync (plugins/service/kraken/kraken.c:207).
  (void)gem_prime_symbols_sync(10000);

  // Periodic refresh. The cadence KV defaults to 86400 s (one day);
  // operators tune via plugin.gemini.symbols_refresh_sec.
  refresh_sec = (uint32_t)kv_get_uint("plugin.gemini.symbols_refresh_sec");

  if(refresh_sec > 0)
  {
    gem_symbols_task = task_add_periodic("gem.symbols",
        TASK_THREAD, 50, refresh_sec * 1000u,
        gem_symbols_periodic_cb, NULL);

    if(gem_symbols_task == TASK_HANDLE_NONE)
      clam(CLAM_WARN, GEM_CTX,
          "symbols periodic task submit failed");
  }

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
  .version         = "0.3-gem3",
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
