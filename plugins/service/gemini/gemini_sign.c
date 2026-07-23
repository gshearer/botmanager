// botmanager — MIT
// Gemini Spot REST signing — HMAC-SHA384 over the base64-encoded JSON
// payload, keyed by the base64-decoded `creds.private_key`.
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
// Base64-decoded Gemini private keys are 48 bytes in practice; the
// cached buffer caps at 1 KiB to absorb any future key-format
// expansion without dynamic resize. The KV snapshot buffers hold the
// source string we last decoded so a KV-edit forces a re-decode on the
// next request.
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

static bool   gem_b64_decode(const char *in, size_t in_len,
                  uint8_t *out, size_t out_cap, size_t *out_len);
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
  // attempt re-decodes.
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

  // Persist for restart-safety. A torn write loses at most one nonce,
  // which Gemini rejects with a hard error; the operator clears by
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
  size_t      decoded_len;
  uint8_t     decoded[GEM_SECRET_BIN_CAP];

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

  if(gem_b64_decode(priv_key, priv_len, decoded, sizeof(decoded),
          &decoded_len) != SUCCESS)
  {
    clam(CLAM_WARN, GEM_CTX,
        "creds.private_key base64 decode failed");
    gem_sign.secret_valid = false;
    return(FAIL);
  }

  memcpy(gem_sign.api_key_snap, api_key, api_len);
  gem_sign.api_key_snap[api_len] = '\0';

  memcpy(gem_sign.private_key_snap, priv_key, priv_len);
  gem_sign.private_key_snap[priv_len] = '\0';

  OPENSSL_cleanse(gem_sign.secret_bin, sizeof(gem_sign.secret_bin));
  memcpy(gem_sign.secret_bin, decoded, decoded_len);
  gem_sign.secret_bin_len = decoded_len;
  gem_sign.secret_valid   = true;

  OPENSSL_cleanse(decoded, sizeof(decoded));

  return(SUCCESS);
}

// Standard-alphabet base64 decode. Tolerates trailing `=` padding +
// embedded whitespace. Returns SUCCESS with `*out_len` populated; FAIL
// on malformed input or buffer overrun.
static bool
gem_b64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap,
    size_t *out_len)
{
  static const int8_t map[256] =
  {
    ['A'] =  0, ['B'] =  1, ['C'] =  2, ['D'] =  3,
    ['E'] =  4, ['F'] =  5, ['G'] =  6, ['H'] =  7,
    ['I'] =  8, ['J'] =  9, ['K'] = 10, ['L'] = 11,
    ['M'] = 12, ['N'] = 13, ['O'] = 14, ['P'] = 15,
    ['Q'] = 16, ['R'] = 17, ['S'] = 18, ['T'] = 19,
    ['U'] = 20, ['V'] = 21, ['W'] = 22, ['X'] = 23,
    ['Y'] = 24, ['Z'] = 25,
    ['a'] = 26, ['b'] = 27, ['c'] = 28, ['d'] = 29,
    ['e'] = 30, ['f'] = 31, ['g'] = 32, ['h'] = 33,
    ['i'] = 34, ['j'] = 35, ['k'] = 36, ['l'] = 37,
    ['m'] = 38, ['n'] = 39, ['o'] = 40, ['p'] = 41,
    ['q'] = 42, ['r'] = 43, ['s'] = 44, ['t'] = 45,
    ['u'] = 46, ['v'] = 47, ['w'] = 48, ['x'] = 49,
    ['y'] = 50, ['z'] = 51,
    ['0'] = 52, ['1'] = 53, ['2'] = 54, ['3'] = 55,
    ['4'] = 56, ['5'] = 57, ['6'] = 58, ['7'] = 59,
    ['8'] = 60, ['9'] = 61,
    ['+'] = 62, ['/'] = 63
  };
  static const bool valid_b64[256] =
  {
    ['A'] = 1, ['B'] = 1, ['C'] = 1, ['D'] = 1, ['E'] = 1, ['F'] = 1,
    ['G'] = 1, ['H'] = 1, ['I'] = 1, ['J'] = 1, ['K'] = 1, ['L'] = 1,
    ['M'] = 1, ['N'] = 1, ['O'] = 1, ['P'] = 1, ['Q'] = 1, ['R'] = 1,
    ['S'] = 1, ['T'] = 1, ['U'] = 1, ['V'] = 1, ['W'] = 1, ['X'] = 1,
    ['Y'] = 1, ['Z'] = 1,
    ['a'] = 1, ['b'] = 1, ['c'] = 1, ['d'] = 1, ['e'] = 1, ['f'] = 1,
    ['g'] = 1, ['h'] = 1, ['i'] = 1, ['j'] = 1, ['k'] = 1, ['l'] = 1,
    ['m'] = 1, ['n'] = 1, ['o'] = 1, ['p'] = 1, ['q'] = 1, ['r'] = 1,
    ['s'] = 1, ['t'] = 1, ['u'] = 1, ['v'] = 1, ['w'] = 1, ['x'] = 1,
    ['y'] = 1, ['z'] = 1,
    ['0'] = 1, ['1'] = 1, ['2'] = 1, ['3'] = 1, ['4'] = 1, ['5'] = 1,
    ['6'] = 1, ['7'] = 1, ['8'] = 1, ['9'] = 1,
    ['+'] = 1, ['/'] = 1
  };
  uint32_t  acc = 0;
  int       bits = 0;
  size_t    written = 0;
  size_t    i;

  if(in == NULL || out == NULL || out_len == NULL)
    return(FAIL);

  for(i = 0; i < in_len; i++)
  {
    unsigned char c = (unsigned char)in[i];

    if(c == '=' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
      continue;

    if(!valid_b64[c])
      return(FAIL);

    acc   = (acc << 6) | (uint32_t)map[c];
    bits += 6;

    if(bits >= 8)
    {
      bits -= 8;

      if(written >= out_cap)
        return(FAIL);

      out[written++] = (uint8_t)((acc >> bits) & 0xFFu);
    }
  }

  *out_len = written;

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
