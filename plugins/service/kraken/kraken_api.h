#ifndef BM_KRAKEN_API_H
#define BM_KRAKEN_API_H

// Public mechanism API for the kraken service plugin. Consumers
// include this header and resolve the symbols at runtime via
// plugin_dlsym("kraken", …) — the plugin is loaded RTLD_LOCAL.
//
// The shim shape mirrors plugins/service/coinbase/coinbase_api.h:
// per-symbol atomic cache guard, union to launder void*↔function-
// pointer conversion, FATAL + abort on dlsym miss.
//
// Inside the kraken plugin itself the static-inline shims below
// would collide with the real definitions, so kraken.c defines
// KR_INTERNAL before including this header to skip them.
//
// Scaffolding: no symbols are exported yet. KR-3 lands the plugin
// descriptor, KV schema, lifecycle hooks, HMAC-SHA512 signer, and
// vtable registration. KR-4 adds the REST surface (candles, orders,
// accounts, fills). KR-5 adds the WebSocket v2 transport + channels.
// Consumers go through feature_exchange (exchange_api.h), not this
// header directly — the kraken-specific shims are present for the
// rare consumer that needs Kraken-specific behaviour.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Fixed size limits for public result structs.
// Kraken's altname surface is short (`BTCUSD`); canonical/legacy
// surface is also short (`XXBTZUSD`); the wsname is `BTC/USD`. 24 B
// accommodates all three with NUL.
#define KRAKEN_PRODUCT_ID_SZ    24
#define KRAKEN_CURRENCY_SZ       8
#define KRAKEN_ORDER_ID_SZ      40   // Kraken txid is ~20 chars; pad for slack
#define KRAKEN_CLIENT_OID_SZ    40
#define KRAKEN_SIDE_SZ           8   // "buy" / "sell"
#define KRAKEN_STATUS_SZ        12
#define KRAKEN_TIF_SZ            8
#define KRAKEN_TYPE_SZ          16

// ------------------------------------------------------------------
// Real function declarations — visible only inside the kraken plugin
// (where KR_INTERNAL is defined). External consumers go through the
// static-inline dlsym shims defined further down. KR-3 ships one
// symbol: the credential-state probe.
// ------------------------------------------------------------------

#ifdef KR_INTERNAL

// Returns true iff both plugin.kraken.creds.api_key and
// plugin.kraken.creds.private_key are set (non-empty). Public probe
// consumers use to decide whether private REST endpoints and
// authenticated WebSocket channels are reachable.
bool kraken_apikey_configured(void);

#endif // KR_INTERNAL

// ------------------------------------------------------------------
// dlsym shim helpers
// ------------------------------------------------------------------

#ifndef KR_INTERNAL

#include "clam.h"
#include "plugin.h"

#include <stdlib.h>  // abort

static inline bool
kraken_apikey_configured(void)
{
  typedef bool (*fn_t)(void);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("kraken", "kraken_apikey_configured");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "kraken",
          "dlsym failed: kraken_apikey_configured");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn());
}

#endif // !KR_INTERNAL

#endif // BM_KRAKEN_API_H
