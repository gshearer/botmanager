// gemini.h — Gemini Spot exchange service plugin (kind: gemini)
//
// Internal-only declarations. Public mechanism lives in gemini_api.h,
// consumed by external callers via the dlsym-shim block gated with
// !GEM_INTERNAL. Subsystems land per TODO.md chunks GEM-1..GEM-4.
//
// GEM-1 ships the scaffold + HMAC-SHA384 signer + REST mechanism +
// symbols cache. GEM-2 adds typed REST wrappers + vtable REST hooks.
// GEM-3 lands the WebSocket transport + channel multiplexer.

#ifndef BM_GEMINI_H
#define BM_GEMINI_H

#ifdef GEM_INTERNAL

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "curl.h"
#include "kv.h"
#include "plugin.h"

#include "gemini_api.h"
#include "gemini_pairs.h"
#include "gemini_rest.h"
#include "gemini_sign.h"
#include "gemini_ws.h"
#include "gemini_ws_channels.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GEM_CTX "gemini"

// Size limits.
// REST / WebSocket base URL buffer. Matches the convention used by the
// coinbase + kraken service plugins.
#define GEM_URL_SZ        512
// Upper bound on a signed REST request payload (JSON envelope). Gemini
// AddOrder payloads sit well under 1 KiB; 4 KiB gives headroom for
// validate-only echoes and large session token fields.
#define GEM_BODY_SZ       4096
// Error message buffer for transient classifier output.
#define GEM_ERR_SZ        128
// Symbols cache capacity. Gemini's `/v1/symbols` returns 419 entries
// as of 2026-05 (spot + perpetuals + multi-stablecoin pairs); 1024
// leaves comfortable headroom for growth + new quote-currency
// dialects. Overflow logs + drops trailing rows.
#define GEM_SYMS_CAP      1024

// WS sizing knobs — shared by all TUs so the multiplexer + transport
// agree on caps. Real bodies arrive in GEM-3; declared here so GEM-1's
// empty stubs do not introduce stale macros to grep over.
#define GEM_WS_IDLE_TIMEOUT_MS   45000
#define GEM_WS_PING_INTERVAL_MS  20000
#define GEM_WS_MAX_BACKOFF_MS    60000
#define GEM_WS_STOP_WAIT_MS       5000
#define GEM_WS_MAX_CONSEC_FAIL      10
#define GEM_SUBS_MAX                64
#define GEM_PRODS_PER_SUB_MAX       16
#define GEM_SLOTS_MAX              128
#define GEM_REQ_ID_RING            256

// gemini_exchange.c — vtable registration with the feature_exchange
// abstraction. Called from gem_start.
bool    gem_exchange_register_vtable(void);

#endif // GEM_INTERNAL

#endif // BM_GEMINI_H
