// botmanager — MIT
// Gemini Spot REST signing — HMAC-SHA384 over the base64-encoded JSON
// payload, keyed by `creds.private_key` VERBATIM (its ASCII bytes;
// nothing is decoded out of it — OBS-67).
//
// The wire shape is documented at
// docs.gemini.com/rest-api/#private-api-invocation.
//
//   Headers:
//     X-GEMINI-APIKEY:    <api_key verbatim>
//     X-GEMINI-PAYLOAD:   base64(payload_json)
//     X-GEMINI-SIGNATURE: hex_lower(HMAC-SHA384(secret, b64(payload)))
//
// The signature lands in the X-GEMINI-SIGNATURE header; the encoded
// payload doubles as both the HMAC message body AND the
// X-GEMINI-PAYLOAD header value so the caller does not redo the
// encode.
//
// Nonces are a monotonic 64-bit counter. The latest value is persisted
// best-effort to `plugin.gemini.last_nonce` after every mint so a
// daemon restart never re-uses one. Seed on init is
// max(last_nonce, time(NULL) * 1e6) so a fresh install lands ahead of
// any historical nonce on the key.
//
// No network I/O happens here.
#define GEM_INTERNAL
#include "gemini.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/params.h>

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// Sizes
//
// A Gemini secret is 28 ASCII characters in practice and is used as
// the HMAC key verbatim; the cached buffer caps at 1 KiB to absorb any
// future key-format expansion without dynamic resize, and is the same
// size as the snapshot so the copy needs no bound of its own. The KV
// snapshot buffers hold the source strings so a KV edit forces a
// refresh on the next request.
#define GEM_SECRET_BIN_CAP    1024
#define GEM_API_KEY_SNAP_SZ   256
#define GEM_PRIV_KEY_SNAP_SZ  1024

// Module state — guarded by gem_sign.lock.

typedef struct
{
  pthread_mutex_t lock;
  uint8_t         secret_bin[GEM_SECRET_BIN_CAP];
  size_t          secret_bin_len;
  bool            secret_valid;
  char            api_key_snap[GEM_API_KEY_SNAP_SZ];
  char            private_key_snap[GEM_PRIV_KEY_SNAP_SZ];
  uint64_t        next_nonce;
} gem_sign_t;

static gem_sign_t gem_sign;

// Forward declarations for static helpers.

static bool   gem_refresh_secret_locked(void);
static bool   gem_hmac_sha384(const uint8_t *key, size_t key_len,
                  const uint8_t *msg, size_t msg_len,
                  uint8_t out[48], size_t *out_len);

// ------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------

void
gem_sign_init(void)
{
  uint64_t persisted;
  uint64_t seed_us;
  time_t   now;

  memset(&gem_sign, 0, sizeof(gem_sign));

  pthread_mutex_init(&gem_sign.lock, NULL);

  // Seed the counter. KV layer returns 0 on missing / unparseable, so
  // the max() against time(NULL) * 1e6 is the floor that keeps us
  // ahead of any historical nonce on the key.
  persisted = kv_get_uint("plugin.gemini.last_nonce");
  now       = time(NULL);
  seed_us   = (now > 0) ? ((uint64_t)now) * 1000000ULL : 0;

  gem_sign.next_nonce = (persisted > seed_us) ? persisted : seed_us;

  // Best-effort prime of the cached secret; on FAIL the plugin runs
  // in public-only mode until creds are populated and the next sign
  // attempt refreshes.
  pthread_mutex_lock(&gem_sign.lock);
  (void)gem_refresh_secret_locked();
  pthread_mutex_unlock(&gem_sign.lock);
}

void
gem_sign_deinit(void)
{
  pthread_mutex_lock(&gem_sign.lock);
  OPENSSL_cleanse(gem_sign.secret_bin, sizeof(gem_sign.secret_bin));
  gem_sign.secret_bin_len = 0;
  gem_sign.secret_valid   = false;
  pthread_mutex_unlock(&gem_sign.lock);
  pthread_mutex_destroy(&gem_sign.lock);
}

bool
gem_apikey_configured(void)
{
  bool ok;

  pthread_mutex_lock(&gem_sign.lock);
  (void)gem_refresh_secret_locked();
  ok = gem_sign.secret_valid;
  pthread_mutex_unlock(&gem_sign.lock);

  return(ok);
}

bool
gem_next_nonce(uint64_t *out)
{
  uint64_t n;

  if(out == NULL)
    return(FAIL);

  pthread_mutex_lock(&gem_sign.lock);

  if(gem_sign.next_nonce == UINT64_MAX)
  {
    pthread_mutex_unlock(&gem_sign.lock);
    return(FAIL);
  }

  n = ++gem_sign.next_nonce;
  pthread_mutex_unlock(&gem_sign.lock);

  // Best-effort only, and nothing here depends on it: kv_set marks the
  // entry dirty and kv_flush() is the sole writer, so this value reaches
  // the database when something else flushes, at kv_exit(), or when this
  // plugin is unloaded (OBS-59) — never on this line. What actually
  // keeps a nonce ahead is the seed above, max(persisted, now_us): a
  // lost row costs nothing because the clock floor outruns it. Losing
  // one anyway is harmless — Gemini rejects with a hard error; the operator clears by
  // bumping plugin.gemini.last_nonce by hand.
  (void)kv_set_uint("plugin.gemini.last_nonce", n);

  *out = n;

  return(SUCCESS);
}

// ------------------------------------------------------------------
// Base64 encoder + Hex encoder
// ------------------------------------------------------------------

bool
gem_b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap,
    size_t *out_len)
{
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t need;
  size_t i;
  size_t o = 0;

  if(out == NULL || out_len == NULL)
    return(FAIL);

  if(in == NULL && in_len > 0)
    return(FAIL);

  need = ((in_len + 2) / 3) * 4 + 1;

  if(out_cap < need)
    return(FAIL);

  for(i = 0; i + 3 <= in_len; i += 3)
  {
    uint32_t v = ((uint32_t)in[i] << 16)
               | ((uint32_t)in[i + 1] << 8)
               |  (uint32_t)in[i + 2];

    out[o++] = alphabet[(v >> 18) & 0x3F];
    out[o++] = alphabet[(v >> 12) & 0x3F];
    out[o++] = alphabet[(v >>  6) & 0x3F];
    out[o++] = alphabet[ v        & 0x3F];
  }

  if(i < in_len)
  {
    uint32_t v   = (uint32_t)in[i] << 16;
    size_t   rem = in_len - i;

    if(rem == 2)
      v |= (uint32_t)in[i + 1] << 8;

    out[o++] = alphabet[(v >> 18) & 0x3F];
    out[o++] = alphabet[(v >> 12) & 0x3F];
    out[o++] = (rem == 2) ? alphabet[(v >> 6) & 0x3F] : '=';
    out[o++] = '=';
  }

  out[o]   = '\0';
  *out_len = o;

  return(SUCCESS);
}

bool
gem_hex_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap)
{
  static const char hex[] = "0123456789abcdef";
  size_t i;

  if(out == NULL)
    return(FAIL);

  if(in == NULL && in_len > 0)
    return(FAIL);

  if(out_cap < 2 * in_len + 1)
    return(FAIL);

  for(i = 0; i < in_len; i++)
  {
    out[2 * i]     = hex[(in[i] >> 4) & 0x0F];
    out[2 * i + 1] = hex[ in[i]       & 0x0F];
  }

  out[2 * in_len] = '\0';

  return(SUCCESS);
}

// ------------------------------------------------------------------
// Sign request
// ------------------------------------------------------------------

bool
gem_sign_request(const char *payload_json, size_t payload_len,
    char *out_b64, size_t out_b64_cap,
    char *out_sig_hex, size_t out_sig_hex_cap)
{
  uint8_t  hmac_buf[48];
  size_t   hmac_len = 0;
  size_t   b64_len  = 0;
  bool     ok;

  if(payload_json == NULL || payload_len == 0
      || out_b64 == NULL || out_sig_hex == NULL)
    return(FAIL);

  if(out_b64_cap < 4 * ((payload_len + 2) / 3) + 1
      || out_sig_hex_cap < 2 * 48 + 1)
    return(FAIL);

  // Base64-encode the raw JSON payload first; the encoded string is
  // both the X-GEMINI-PAYLOAD header value AND the input to HMAC.
  if(gem_b64_encode((const uint8_t *)payload_json, payload_len,
        out_b64, out_b64_cap, &b64_len) != SUCCESS)
    return(FAIL);

  // Snapshot the decoded secret under the signer's mutex. The secret
  // is base64-decoded once and re-decoded on KV change.
  pthread_mutex_lock(&gem_sign.lock);

  if(gem_refresh_secret_locked() != SUCCESS || !gem_sign.secret_valid)
  {
    pthread_mutex_unlock(&gem_sign.lock);
    return(FAIL);
  }

  ok = (gem_hmac_sha384(gem_sign.secret_bin, gem_sign.secret_bin_len,
            (const uint8_t *)out_b64, b64_len, hmac_buf, &hmac_len)
        == SUCCESS);

  pthread_mutex_unlock(&gem_sign.lock);

  if(!ok)
    return(FAIL);

  // Lowercase hex output. Gemini's contract: signature MUST be hex,
  // NOT base64 (this is where the Kraken signer's API-Sign output
  // shape would be wrong).
  if(gem_hex_encode(hmac_buf, hmac_len, out_sig_hex, out_sig_hex_cap)
      != SUCCESS)
  {
    OPENSSL_cleanse(hmac_buf, sizeof(hmac_buf));
    return(FAIL);
  }

  OPENSSL_cleanse(hmac_buf, sizeof(hmac_buf));

  return(SUCCESS);
}

// ------------------------------------------------------------------
// Public exported wrapper — see gemini_api.h.
// ------------------------------------------------------------------

bool
gemini_apikey_configured(void)
{
  return(gem_apikey_configured());
}

// ------------------------------------------------------------------
// Static helpers
// ------------------------------------------------------------------

// Re-decode the cached secret when the source KV strings have drifted
// from the last snapshot. Caller must hold gem_sign.lock. Returns
// SUCCESS when the cache is either already current or has been
// refreshed cleanly; FAIL when the KV is empty / malformed. On FAIL
// `secret_valid` is left false so callers can short-circuit.
static bool
gem_refresh_secret_locked(void)
{
  const char *api_key;
  const char *priv_key;
  size_t      api_len;
  size_t      priv_len;

  kv_admin_context_set(true);
  api_key  = kv_get_str("plugin.gemini.creds.apikey");
  priv_key = kv_get_str("plugin.gemini.creds.private_key");
  kv_admin_context_set(false);

  if(api_key == NULL || api_key[0] == '\0'
      || priv_key == NULL || priv_key[0] == '\0')
  {
    gem_sign.secret_valid = false;
    return(FAIL);
  }

  if(strcmp(gem_sign.api_key_snap, api_key) == 0
      && strcmp(gem_sign.private_key_snap, priv_key) == 0
      && gem_sign.secret_valid)
    return(SUCCESS);

  api_len  = strlen(api_key);
  priv_len = strlen(priv_key);

  if(api_len >= sizeof(gem_sign.api_key_snap)
      || priv_len >= sizeof(gem_sign.private_key_snap))
  {
    clam(CLAM_WARN, GEM_CTX,
        "creds snapshot overflow (api=%zu priv=%zu)", api_len, priv_len);
    gem_sign.secret_valid = false;
    return(FAIL);
  }

  memcpy(gem_sign.api_key_snap, api_key, api_len);
  gem_sign.api_key_snap[api_len] = '\0';

  memcpy(gem_sign.private_key_snap, priv_key, priv_len);
  gem_sign.private_key_snap[priv_len] = '\0';

  // ⛔ The secret is the HMAC key AS IT IS WRITTEN — Gemini keys the
  // MAC with the ASCII bytes of the secret, and nothing is decoded out
  // of them. This used to base64-decode it, copied from kraken, whose
  // secret genuinely is base64. A Gemini secret is 28 characters drawn
  // from the base64 alphabet, so the decode SUCCEEDED and handed the
  // MAC 21 bytes of garbage: every private request this plugin ever
  // signed came back `InvalidSignature`, and on the Order Events
  // upgrade that is an HTTP 400 (OBS-67).
  //
  // `priv_len` is bounded by the snapshot-size check above, and
  // GEM_SECRET_BIN_CAP is that same size.
  OPENSSL_cleanse(gem_sign.secret_bin, sizeof(gem_sign.secret_bin));
  memcpy(gem_sign.secret_bin, priv_key, priv_len);
  gem_sign.secret_bin_len = priv_len;
  gem_sign.secret_valid   = true;

  return(SUCCESS);
}

// HMAC-SHA384 via the OpenSSL 3 EVP_MAC interface. SHA-384 produces a
// 48-byte digest.
static bool
gem_hmac_sha384(const uint8_t *key, size_t key_len, const uint8_t *msg,
    size_t msg_len, uint8_t out[48], size_t *out_len)
{
  EVP_MAC     *mac;
  EVP_MAC_CTX *ctx;
  OSSL_PARAM   params[2];
  size_t       olen = 48;
  bool         ok   = FAIL;

  mac = EVP_MAC_fetch(NULL, "HMAC", NULL);

  if(mac == NULL)
    return(FAIL);

  ctx = EVP_MAC_CTX_new(mac);

  if(ctx == NULL)
  {
    EVP_MAC_free(mac);
    return(FAIL);
  }

  params[0] = OSSL_PARAM_construct_utf8_string("digest", "SHA384", 0);
  params[1] = OSSL_PARAM_construct_end();

  if(EVP_MAC_init(ctx, key, key_len, params) == 1
      && EVP_MAC_update(ctx, msg, msg_len) == 1
      && EVP_MAC_final(ctx, out, &olen, 48) == 1
      && olen == 48)
  {
    *out_len = olen;
    ok       = SUCCESS;
  }

  EVP_MAC_CTX_free(ctx);
  EVP_MAC_free(mac);

  return(ok);
}
