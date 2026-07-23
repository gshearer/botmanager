// botmanager — MIT
// Kraken Spot REST signing — HMAC-SHA512 over uripath || SHA256(nonce
// || postdata), keyed by the base64-decoded `creds.private_key`.
//
// The wire shape is documented at
// docs.kraken.com/api/docs/rest-api/get-server-time §Authentication.
// Result base64 lands in the `API-Sign` request header alongside
// `API-Key: <plugin.kraken.creds.apikey>`.
//
// Nonces are a monotonic 64-bit counter. The latest value is persisted
// best-effort to `plugin.kraken.last_nonce` after every mint so a
// daemon restart never re-uses one — Kraken rejects out-of-order
// nonces with `EAPI:Invalid nonce`. Seed on init is
// max(last_nonce, time(NULL) * 1e6) so a fresh install lands ahead of
// any historical nonce on the key.
//
// No network I/O happens here.
#define KR_INTERNAL
#include "kraken.h"

#include "util.h"

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
// Base64-decoded Kraken private keys are 64 bytes in practice; the
// cached buffer caps at 1 KiB to absorb any future key-format
// expansion without dynamic resize. The KV snapshot buffers
// (`api_key_snap`, `private_key_snap`) hold the source string we last
// decoded so a KV-edit forces a re-decode on the next request.
#define KR_SECRET_BIN_CAP    1024
#define KR_API_KEY_SNAP_SZ   256
#define KR_PRIV_KEY_SNAP_SZ  1024

// Module state — guarded by kr_sign.lock.

typedef struct
{
  pthread_mutex_t lock;
  uint8_t         secret_bin[KR_SECRET_BIN_CAP];
  size_t          secret_bin_len;
  bool            secret_valid;
  char            api_key_snap[KR_API_KEY_SNAP_SZ];
  char            private_key_snap[KR_PRIV_KEY_SNAP_SZ];
  uint64_t        next_nonce;
} kr_sign_t;

static kr_sign_t kr_sign;

// Forward declarations for static helpers.

static bool   kr_b64_decode(const char *in, size_t in_len,
                  uint8_t *out, size_t out_cap, size_t *out_len);
static bool   kr_refresh_secret_locked(void);
static bool   kr_hmac_sha512(const uint8_t *key, size_t key_len,
                  const uint8_t *msg, size_t msg_len,
                  uint8_t out[64], size_t *out_len);
static bool   kr_sha256(const uint8_t *msg, size_t msg_len,
                  uint8_t out[32], size_t *out_len);

// Lifecycle

void
kr_sign_init(void)
{
  uint64_t persisted;
  uint64_t seed_us;
  time_t   now;

  memset(&kr_sign, 0, sizeof(kr_sign));

  pthread_mutex_init(&kr_sign.lock, NULL);

  // Seed the counter. KV layer returns 0 on missing / unparseable, so
  // the max() against time(NULL) * 1e6 is the floor that keeps us
  // ahead of any historical nonce on the key.
  persisted = kv_get_uint("plugin.kraken.last_nonce");
  now       = time(NULL);
  seed_us   = (now > 0) ? ((uint64_t)now) * 1000000ULL : 0;

  kr_sign.next_nonce = (persisted > seed_us) ? persisted : seed_us;

  // Best-effort prime of the cached secret; on FAIL the plugin runs
  // in public-only mode until creds are populated and the next sign
  // attempt re-decodes.
  pthread_mutex_lock(&kr_sign.lock);
  (void)kr_refresh_secret_locked();
  pthread_mutex_unlock(&kr_sign.lock);
}

void
kr_sign_deinit(void)
{
  pthread_mutex_lock(&kr_sign.lock);
  OPENSSL_cleanse(kr_sign.secret_bin, sizeof(kr_sign.secret_bin));
  kr_sign.secret_bin_len = 0;
  kr_sign.secret_valid   = false;
  pthread_mutex_unlock(&kr_sign.lock);
  pthread_mutex_destroy(&kr_sign.lock);
}

bool
kr_apikey_configured(void)
{
  bool ok;

  pthread_mutex_lock(&kr_sign.lock);
  (void)kr_refresh_secret_locked();
  ok = kr_sign.secret_valid;
  pthread_mutex_unlock(&kr_sign.lock);

  return(ok);
}

bool
kr_next_nonce(char *out, size_t cap)
{
  uint64_t n;
  int      written;

  if(out == NULL || cap < 21)
    return(FAIL);

  pthread_mutex_lock(&kr_sign.lock);
  n = ++kr_sign.next_nonce;
  pthread_mutex_unlock(&kr_sign.lock);

  // Persist for restart-safety. A torn write loses at most one nonce,
  // which Kraken rejects with EAPI:Invalid nonce; the operator clears
  // by bumping plugin.kraken.last_nonce by hand.
  (void)kv_set_uint("plugin.kraken.last_nonce", n);

  written = snprintf(out, cap, "%" PRIu64, n);

  if(written < 0 || (size_t)written >= cap)
    return(FAIL);

  return(SUCCESS);
}

bool
kr_sign_request(const char *uripath, size_t uripath_len,
    const char *nonce_str, size_t nonce_len,
    const char *postdata, size_t postdata_len,
    char *out_sig_b64, size_t out_sig_cap)
{
  uint8_t  sha256_buf[32];
  size_t   sha256_len;
  uint8_t  hmac_buf[64];
  size_t   hmac_len;
  uint8_t  hmac_input[KR_BODY_SZ + 32 + KR_URL_SZ];
  size_t   hmac_input_len;
  size_t   encoded;
  bool     ok;

  if(uripath == NULL || nonce_str == NULL || out_sig_b64 == NULL)
    return(FAIL);

  if(uripath_len == 0 || nonce_len == 0 || out_sig_cap < 128)
    return(FAIL);

  if(uripath_len > KR_URL_SZ)
    return(FAIL);

  // SHA256(nonce || postdata)
  {
    uint8_t  sha_input[KR_BODY_SZ + 32];
    size_t   need = nonce_len + postdata_len;

    if(need > sizeof(sha_input))
      return(FAIL);

    memcpy(sha_input, nonce_str, nonce_len);

    if(postdata_len > 0 && postdata != NULL)
      memcpy(sha_input + nonce_len, postdata, postdata_len);

    if(kr_sha256(sha_input, need, sha256_buf, &sha256_len) != SUCCESS)
      return(FAIL);
  }

  // HMAC-SHA512(secret_bin, uripath || sha256_buf)
  hmac_input_len = uripath_len + sha256_len;

  if(hmac_input_len > sizeof(hmac_input))
    return(FAIL);

  memcpy(hmac_input, uripath, uripath_len);
  memcpy(hmac_input + uripath_len, sha256_buf, sha256_len);

  pthread_mutex_lock(&kr_sign.lock);

  if(kr_refresh_secret_locked() != SUCCESS || !kr_sign.secret_valid)
  {
    pthread_mutex_unlock(&kr_sign.lock);
    return(FAIL);
  }

  ok = (kr_hmac_sha512(kr_sign.secret_bin, kr_sign.secret_bin_len,
              hmac_input, hmac_input_len, hmac_buf, &hmac_len)
        == SUCCESS);

  pthread_mutex_unlock(&kr_sign.lock);

  if(!ok)
    return(FAIL);

  encoded = util_b64_encode(hmac_buf, hmac_len, out_sig_b64, out_sig_cap);

  OPENSSL_cleanse(hmac_buf, sizeof(hmac_buf));

  if(encoded == 0)
    return(FAIL);

  return(SUCCESS);
}

// Public exported symbol — see kraken_api.h.
bool
kraken_apikey_configured(void)
{
  return(kr_apikey_configured());
}

// ---- static helpers ----

// Re-decode the cached secret when the source KV strings have drifted
// from the last snapshot. Caller must hold kr_sign.lock. Returns
// SUCCESS when the cache is either already current or has been
// refreshed cleanly; FAIL when the KV is empty / malformed. On FAIL
// `secret_valid` is left false so callers can short-circuit.
static bool
kr_refresh_secret_locked(void)
{
  const char *api_key;
  const char *priv_key;
  size_t      api_len;
  size_t      priv_len;
  size_t      decoded_len;
  uint8_t     decoded[KR_SECRET_BIN_CAP];

  kv_admin_context_set(true);
  api_key  = kv_get_str("plugin.kraken.creds.apikey");
  priv_key = kv_get_str("plugin.kraken.creds.private_key");
  kv_admin_context_set(false);

  if(api_key == NULL || api_key[0] == '\0'
      || priv_key == NULL || priv_key[0] == '\0')
  {
    kr_sign.secret_valid = false;
    return(FAIL);
  }

  if(strcmp(kr_sign.api_key_snap, api_key) == 0
      && strcmp(kr_sign.private_key_snap, priv_key) == 0
      && kr_sign.secret_valid)
    return(SUCCESS);

  api_len  = strlen(api_key);
  priv_len = strlen(priv_key);

  if(api_len >= sizeof(kr_sign.api_key_snap)
      || priv_len >= sizeof(kr_sign.private_key_snap))
  {
    clam(CLAM_WARN, KR_CTX,
        "creds snapshot overflow (api=%zu priv=%zu)", api_len, priv_len);
    kr_sign.secret_valid = false;
    return(FAIL);
  }

  if(kr_b64_decode(priv_key, priv_len, decoded, sizeof(decoded),
          &decoded_len) != SUCCESS)
  {
    clam(CLAM_WARN, KR_CTX,
        "creds.private_key base64 decode failed");
    kr_sign.secret_valid = false;
    return(FAIL);
  }

  memcpy(kr_sign.api_key_snap, api_key, api_len);
  kr_sign.api_key_snap[api_len] = '\0';

  memcpy(kr_sign.private_key_snap, priv_key, priv_len);
  kr_sign.private_key_snap[priv_len] = '\0';

  OPENSSL_cleanse(kr_sign.secret_bin, sizeof(kr_sign.secret_bin));
  memcpy(kr_sign.secret_bin, decoded, decoded_len);
  kr_sign.secret_bin_len = decoded_len;
  kr_sign.secret_valid   = true;

  OPENSSL_cleanse(decoded, sizeof(decoded));

  return(SUCCESS);
}

// Standard-alphabet base64 decode. Tolerates trailing `=` padding +
// embedded whitespace. Returns SUCCESS with `*out_len` populated; FAIL
// on malformed input or buffer overrun.
static bool
kr_b64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap,
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
  // Treat every non-alphabet byte as -1; the table above leaves them
  // as the implicit 0 fill. Use a separate "valid" mask: track via
  // checking the source char.
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

static bool
kr_sha256(const uint8_t *msg, size_t msg_len, uint8_t out[32],
    size_t *out_len)
{
  EVP_MD_CTX  *ctx;
  unsigned int olen = 32;
  bool         ok   = FAIL;

  ctx = EVP_MD_CTX_new();

  if(ctx == NULL)
    return(FAIL);

  if(EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1
      && EVP_DigestUpdate(ctx, msg, msg_len) == 1
      && EVP_DigestFinal_ex(ctx, out, &olen) == 1
      && olen == 32)
  {
    *out_len = olen;
    ok       = SUCCESS;
  }

  EVP_MD_CTX_free(ctx);

  return(ok);
}

static bool
kr_hmac_sha512(const uint8_t *key, size_t key_len, const uint8_t *msg,
    size_t msg_len, uint8_t out[64], size_t *out_len)
{
  EVP_MAC     *mac;
  EVP_MAC_CTX *ctx;
  OSSL_PARAM   params[2];
  size_t       olen = 64;
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

  params[0] = OSSL_PARAM_construct_utf8_string("digest", "SHA512", 0);
  params[1] = OSSL_PARAM_construct_end();

  if(EVP_MAC_init(ctx, key, key_len, params) == 1
      && EVP_MAC_update(ctx, msg, msg_len) == 1
      && EVP_MAC_final(ctx, out, &olen, 64) == 1
      && olen == 64)
  {
    *out_len = olen;
    ok       = SUCCESS;
  }

  EVP_MAC_CTX_free(ctx);
  EVP_MAC_free(mac);

  return(ok);
}
