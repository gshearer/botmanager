// kraken_sign.h — Kraken HMAC-SHA512 signer + nonce minter.
//
// Kraken's REST private endpoints authenticate with two HTTP headers:
//
//   API-Key:  <plugin.kraken.creds.apikey verbatim>
//   API-Sign: base64(HMAC-SHA512(
//                base64-decode(plugin.kraken.creds.private_key),
//                uripath || SHA256(nonce_str || postdata)))
//
// The private_key is base64 from Kraken; we decode it once + cache.
// The base64-decoded secret + key snapshot are mutex-protected so
// KV-edits at runtime force a re-decode on the next request without
// racing with in-flight signs.
//
// Nonces are monotonic 64-bit counters. Kraken rejects out-of-order
// nonces with `EAPI:Invalid nonce`.
//
// ⭑ What guarantees monotonicity across a restart is the SEED, not the
// persisted row: every start takes `max(plugin.kraken.last_nonce,
// time(NULL) * 1e6)`, so the microsecond clock floor is already ahead
// of any nonce minted in a previous second and a lost row costs
// nothing. The row is an optimisation, and its write is best-effort in
// the strict sense (OBS-59): kr_next_nonce only marks the KV entry
// dirty, and the value reaches the database when something flushes, at
// kv_exit(), or when this plugin is unloaded.

#ifndef BM_KRAKEN_SIGN_H
#define BM_KRAKEN_SIGN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Lifecycle. Paired with kr_init / kr_deinit in kraken.c.
void    kr_sign_init(void);
void    kr_sign_deinit(void);

// True iff both plugin.kraken.creds.apikey and
// plugin.kraken.creds.private_key are non-empty AND the private_key
// base64-decoded cleanly. Safe to call at any time — refreshes the
// cached snapshot transparently when the KVs change.
bool    kr_apikey_configured(void);

// Mint the next monotonic nonce as a decimal string. `out` receives
// a NUL-terminated value; `cap` must be at least 21 chars (20 digits
// for UINT64_MAX + NUL). Returns SUCCESS on a clean format; FAIL when
// `out` is NULL/short. Persists the new value to
// plugin.kraken.last_nonce best-effort.
bool    kr_next_nonce(char *out, size_t cap);

// Compute `API-Sign` for one request. `uripath` is the full path
// portion of the URL (e.g. "/0/private/AddOrder"); `nonce_str` is the
// decimal nonce produced by kr_next_nonce; `postdata` is the form-
// urlencoded body sent on the wire. `out_sig_b64` receives a NUL-
// terminated base64 signature on SUCCESS; contents unspecified on
// FAIL. `out_sig_cap` should be at least 128 bytes (base64 of 64 B
// HMAC-SHA512 is 88 chars + NUL).
//
// Returns FAIL when the cached secret is missing or malformed —
// callers should not have invoked this path without
// kr_apikey_configured() returning true first.
bool    kr_sign_request(const char *uripath, size_t uripath_len,
            const char *nonce_str, size_t nonce_len,
            const char *postdata, size_t postdata_len,
            char *out_sig_b64, size_t out_sig_cap);

#endif // BM_KRAKEN_SIGN_H
