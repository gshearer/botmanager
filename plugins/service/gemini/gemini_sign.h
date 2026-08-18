// gemini_sign.h — Gemini HMAC-SHA384 signer + nonce minter.
//
// Gemini's REST private endpoints authenticate with three HTTP headers
// (HTTP body is intentionally empty — every parameter rides in the
// base64-encoded payload):
//
//   X-GEMINI-APIKEY:    <plugin.gemini.creds.apikey verbatim>
//   X-GEMINI-PAYLOAD:   base64(payload_json)
//   X-GEMINI-SIGNATURE: hex_lower(HMAC-SHA384(
//                           plugin.gemini.creds.private_key,
//                           base64(payload_json)))
//
// The private_key is a base64-encoded secret from Gemini's portal; we
// decode it once and cache the binary form. The decoded secret + KV
// snapshot are mutex-protected so KV-edits at runtime force a re-decode
// on the next request without racing in-flight signs.
//
// Two Gemini-specific divergences from the Kraken signer:
//
//   * HMAC is SHA-384, not SHA-512.
//   * Signature output is lowercase hex, not base64.
//
// Nonces are monotonic 64-bit counters. Gemini rejects out-of-order
// nonces with a hard error.
//
// ⭑ What guarantees monotonicity across a restart is the SEED, not the
// persisted row: every start takes `max(plugin.gemini.last_nonce,
// time(NULL) * 1e6)`, so the microsecond clock floor is already ahead
// of any nonce minted in a previous second and a lost row costs
// nothing. The row is an optimisation, and its write is best-effort in
// the strict sense (OBS-59): gem_next_nonce only marks the KV entry
// dirty, and the value reaches the database when something flushes, at
// kv_exit(), or when this plugin is unloaded.

#ifndef BM_GEMINI_SIGN_H
#define BM_GEMINI_SIGN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Lifecycle. Paired with gem_init / gem_deinit in gemini.c.
void    gem_sign_init(void);
void    gem_sign_deinit(void);

// True iff both plugin.gemini.creds.apikey and
// plugin.gemini.creds.private_key are non-empty AND the private_key
// base64-decoded cleanly. Safe to call at any time — refreshes the
// cached snapshot transparently when the KVs change.
bool    gem_apikey_configured(void);

// Mint the next monotonic nonce. `*out` receives the decimal value
// directly (callers compose it into the payload JSON via snprintf).
// Returns SUCCESS on a clean mint; FAIL when the counter would
// overflow (impractical in practice — UINT64_MAX of microseconds is
// ~584 millennia past epoch). Persists the new value to
// plugin.gemini.last_nonce best-effort.
bool    gem_next_nonce(uint64_t *out);

// Standard-alphabet base64 encoder. Returns SUCCESS with `*out_len`
// holding the byte count (excluding NUL) written into `out`; FAIL on
// buffer overflow or NULL inputs.
bool    gem_b64_encode(const uint8_t *in, size_t in_len,
            char *out, size_t out_cap, size_t *out_len);

// Lowercase-hex encoder. Each input byte → two hex chars + NUL.
// `out_cap` must hold at least `2 * in_len + 1`. Returns SUCCESS or
// FAIL on overflow / NULL inputs.
bool    gem_hex_encode(const uint8_t *in, size_t in_len,
            char *out, size_t out_cap);

// Sign one Gemini private request. `payload_json` is the raw JSON
// envelope (e.g. `{"request":"/v1/balances","nonce":12345}`).
//
//   1. base64-encode payload_json → `out_b64` (suitable for the
//      X-GEMINI-PAYLOAD header).
//   2. HMAC-SHA384 keyed by the cached binary secret, with `out_b64`
//      as the message body.
//   3. Lowercase-hex encode the HMAC → `out_sig_hex` (suitable for
//      the X-GEMINI-SIGNATURE header).
//
// On SUCCESS both output buffers are NUL-terminated. Returns FAIL
// when the cached secret is missing or malformed — callers should not
// have invoked this path without gem_apikey_configured() returning
// true first.
//
// Buffer sizing: `out_b64_cap` >= 4 * ((payload_len + 2) / 3) + 1;
// `out_sig_hex_cap` >= 2 * 48 + 1 = 97 (SHA-384 produces 48 bytes).
bool    gem_sign_request(const char *payload_json, size_t payload_len,
            char *out_b64, size_t out_b64_cap,
            char *out_sig_hex, size_t out_sig_hex_cap);

#endif // BM_GEMINI_SIGN_H
