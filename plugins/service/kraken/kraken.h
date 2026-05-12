// kraken.h — Kraken exchange service plugin (kind: kraken)
//
// Internal-only declarations. Public mechanism lives in kraken_api.h,
// consumed by external callers via the dlsym-shim block gated with
// !KR_INTERNAL. Subsystems land per TODO.md chunks KR-3..KR-6.

#ifndef BM_KRAKEN_H
#define BM_KRAKEN_H

#ifdef KR_INTERNAL

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "curl.h"
#include "kv.h"
#include "plugin.h"

#include "kraken_api.h"
#include "kraken_pairs.h"
#include "kraken_rest.h"
#include "kraken_sign.h"
#include "kraken_ws.h"
#include "kraken_ws_channels.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KR_CTX "kraken"

// Size limits.
// REST / WebSocket base URL buffer. Matches the convention used by
// the coinbase service plugin.
#define KR_URL_SZ        512
// Upper bound on a signed REST request body (form-urlencoded). Kraken
// AddOrder bodies sit well under 1 KiB; 4 KiB gives headroom for
// validate-only echoes that may carry larger debug payloads.
#define KR_BODY_SZ       4096
// Error message buffer for transient classifier output.
#define KR_ERR_SZ        128

// kraken_exchange.c — vtable registration with the feature_exchange
// abstraction. Called from kr_start.
bool    kr_exchange_register_vtable(void);

#endif // KR_INTERNAL

#endif // BM_KRAKEN_H
