// botmanager — MIT
// Kraken exchange-vtable: scaffolding registration with
// feature_exchange. KR-3 ships the three required vtable slots
// (build_request / submit / free_request) as FAIL stubs so the
// abstraction surfaces a clean error for any traffic that lands here
// before KR-4 wires the REST surface. Capability hooks
// (is_authenticated, place_order_async, fetch_candles_async, …) all
// stay NULL — the abstraction reports them as unsupported until the
// later chunks fill them in.
//
// `advertised_rps` / `advertised_burst` reflect Kraken's spot tier-2
// public-API budget; the abstraction's token bucket sizes from these
// at registration time.
#define KR_INTERNAL
#include "kraken.h"
#include "exchange_api.h"

static bool
kr_exchange_build_request(exchange_op_kind_t kind, const char *path,
    const char *body_json, void **out_handle)
{
  (void)kind;
  (void)path;
  (void)body_json;

  if(out_handle != NULL)
    *out_handle = NULL;

  clam(CLAM_WARN, KR_CTX,
      "exchange build: kraken REST surface not implemented yet (KR-4)");

  return(FAIL);
}

static bool
kr_exchange_submit(void *handle, uint8_t prio,
    exchange_response_cb_t cb, void *user)
{
  (void)handle;
  (void)prio;
  (void)cb;
  (void)user;

  return(FAIL);
}

static void
kr_exchange_free_request(void *handle)
{
  (void)handle;
}

// File-scope vtable. Static storage so the abstraction can keep the
// pointer; advertised_rps reflects Kraken Spot's tier-2 public-API
// budget (~1 req/s sustained with a 15-call burst).
static const exchange_protocol_vtable_t kr_vtable =
{
  .build_request    = kr_exchange_build_request,
  .submit           = kr_exchange_submit,
  .free_request     = kr_exchange_free_request,
  .advertised_rps   = 1,
  .advertised_burst = 15,

  // Capability hooks all NULL until KR-4 wires the REST surface.
  // exchange_get_capabilities reports has_credentials=true when the
  // is_authenticated hook is NULL (the abstraction treats public-only
  // exchanges as authed-by-absence — auth-gated verbs FAIL on the
  // hook layer instead). KR-4 will populate is_authenticated +
  // is_sandbox + every typed verb.
};

bool
kr_exchange_register_vtable(void)
{
  return(exchange_register("kraken", &kr_vtable));
}
