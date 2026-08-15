// botmanager — MIT
// Coinbase Developer Platform (CDP) request signing — JWT/ES256.
//
// Coinbase's retail Advanced Trade API authenticates with a per-request
// JWT signed by an EC P-256 private key (the "CDP key"). This TU
// produces those JWTs:
//
//   header  = b64url({"alg":"ES256","typ":"JWT","kid":<key_name>,
//                     "nonce":<32-hex>})
//   payload = b64url({"iss":"cdp","sub":<key_name>,
//                     "nbf":<now>,"exp":<now+120>,
//                     "uri":"<METHOD> <host>/<path>"})
//   sig     = b64url(P1363(ECDSA-SHA256(header "." payload)))
//   jwt     = header "." payload "." sig
//
// OpenSSL emits ECDSA signatures in DER; the JWT spec requires P1363
// (fixed-width r||s). The DER → P1363 conversion lives here in
// cb_der_to_p1363 — it is the only easy-to-get-wrong piece.
//
// The PEM private key is parsed once and the resulting EVP_PKEY is
// cached. A snapshot of the source string lets us detect KV-side edits
// and rebuild the EVP_PKEY without subscribing to a kv_register
// callback (the KV registers in WM-LT-8-AUTH-2 own the lifecycle).
//
// No network I/O happens here. The caller wires the JWT into an
// `Authorization: Bearer <jwt>` header at request-build time.
#define CB_INTERNAL
#include "coinbase.h"

#include "alloc.h"
#include "util.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

// Sizes
//
// ES256 raw signature is 64 B (32 B r || 32 B s); base64url adds ~33%
// overhead with no padding. The JWT envelope (two dots + three b64url
// segments) tops out around 600-700 B in practice for our claim set;
// 1024 leaves slack for slightly larger key_name strings.
#define CB_JWT_SZ              1024

// JWT lifetime — Coinbase rejects exp - nbf > 120s.
#define CB_JWT_LIFETIME_SEC    120

// 16 random bytes → 32 hex chars, plus terminator.
#define CB_JWT_NONCE_BYTES     16
#define CB_JWT_NONCE_HEX       (CB_JWT_NONCE_BYTES * 2)

// ES256 (P-256) raw integer width.
#define CB_P256_INT_BYTES      32
#define CB_P256_SIG_BYTES      (CB_P256_INT_BYTES * 2)

// Module state — guarded by cb_cdp_mu.

static pthread_mutex_t  cb_cdp_mu        = PTHREAD_MUTEX_INITIALIZER;
static EVP_PKEY        *cb_cdp_pkey      = NULL;
static char            *cb_cdp_pem_snap  = NULL;  // last-parsed PEM source

// Translate any literal `\\n` / `\\r` escape sequences in `src` into
// real newlines / carriage returns. Operators frequently render the
// PEM into freshstart/.env as a single line with backslash-n
// separators; PEM_read_bio_PrivateKey only recognises actual newlines.
// Returns a freshly mem_alloc'd NUL-terminated string the caller owns;
// NULL only for a NULL `src`.
static char *
cb_pem_unescape(const char *src)
{
  size_t  in_len;
  size_t  i;
  size_t  o;
  char   *out;

  if(src == NULL)
    return(NULL);

  in_len = strlen(src);
  out    = mem_alloc(CB_CTX, "cdp pem", in_len + 1);

  for(i = 0, o = 0; i < in_len; i++)
  {
    if(src[i] == '\\' && i + 1 < in_len)
    {
      if(src[i + 1] == 'n')      { out[o++] = '\n'; i++; continue; }
      if(src[i + 1] == 'r')      { out[o++] = '\r'; i++; continue; }
      if(src[i + 1] == '\\')     { out[o++] = '\\'; i++; continue; }
    }

    out[o++] = src[i];
  }

  out[o] = '\0';

  return(out);
}

// Parse `pem_src` into a fresh EVP_PKEY. Caller owns the result and
// must EVP_PKEY_free it. Returns NULL on parse error.
static EVP_PKEY *
cb_pem_to_pkey(const char *pem_src)
{
  char     *unescaped;
  BIO      *bio;
  EVP_PKEY *pk;

  unescaped = cb_pem_unescape(pem_src);

  if(unescaped == NULL)
    return(NULL);

  bio = BIO_new_mem_buf(unescaped, -1);

  if(bio == NULL)
  {
    mem_free(unescaped);
    return(NULL);
  }

  pk = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);

  BIO_free(bio);
  mem_free(unescaped);

  return(pk);
}

// Refresh cb_cdp_pkey if the configured PEM differs from the last
// snapshot. Caller must hold cb_cdp_mu (the returned pointer is only
// valid while the lock is held — a parallel call could otherwise
// EVP_PKEY_free it from underneath us). Returns SUCCESS with
// `*out` set to a borrowed EVP_PKEY pointer on success; FAIL
// otherwise.
static bool
cb_load_cdp_key_locked(EVP_PKEY **out)
{
  const char *pem;
  EVP_PKEY   *pk;
  char       *snap;
  size_t      pem_len;

  if(out == NULL)
    return(FAIL);

  // kv_get_str returns the same pointer for repeat reads, but the
  // backing string mutates on kv_set; the snapshot compare is cheap
  // and decisive. Read under admin context — secret-tier KV would
  // otherwise hand back KV_REDACTED_VALUE.
  kv_admin_context_set(true);
  pem = kv_get_str("plugin.coinbase.creds.private_key_pem");
  kv_admin_context_set(false);

  if(pem == NULL || pem[0] == '\0')
    return(FAIL);

  if(cb_cdp_pkey != NULL
      && cb_cdp_pem_snap != NULL
      && strcmp(cb_cdp_pem_snap, pem) == 0)
  {
    *out = cb_cdp_pkey;
    return(SUCCESS);
  }

  pem_len = strlen(pem);
  pk      = cb_pem_to_pkey(pem);

  if(pk == NULL)
  {
    clam(CLAM_WARN, CB_CTX, "cdp pem parse failed");
    return(FAIL);
  }

  snap = mem_alloc(CB_CTX, "cdp pem snap", pem_len + 1);

  memcpy(snap, pem, pem_len + 1);

  if(cb_cdp_pkey != NULL)
    EVP_PKEY_free(cb_cdp_pkey);

  if(cb_cdp_pem_snap != NULL)
    mem_free(cb_cdp_pem_snap);

  cb_cdp_pkey     = pk;
  cb_cdp_pem_snap = snap;
  *out            = pk;

  return(SUCCESS);
}

// Convert a DER-encoded ECDSA signature (SEQUENCE { INTEGER r,
// INTEGER s }) into IEEE P1363 form: r and s each left-padded to
// CB_P256_INT_BYTES, concatenated. Returns SUCCESS on a clean
// conversion; FAIL on malformed DER or oversized integers.
static bool
cb_der_to_p1363(const unsigned char *der, size_t der_len,
    unsigned char out[CB_P256_SIG_BYTES])
{
  ECDSA_SIG     *sig;
  const BIGNUM  *r;
  const BIGNUM  *s;
  int            r_len;
  int            s_len;

  sig = d2i_ECDSA_SIG(NULL, &der, (long)der_len);

  if(sig == NULL)
    return(FAIL);

  ECDSA_SIG_get0(sig, &r, &s);

  r_len = BN_num_bytes(r);
  s_len = BN_num_bytes(s);

  if(r_len > CB_P256_INT_BYTES || s_len > CB_P256_INT_BYTES
      || r_len <= 0 || s_len <= 0)
  {
    ECDSA_SIG_free(sig);
    return(FAIL);
  }

  memset(out, 0, CB_P256_SIG_BYTES);

  BN_bn2bin(r, out + (CB_P256_INT_BYTES - r_len));
  BN_bn2bin(s, out + CB_P256_INT_BYTES + (CB_P256_INT_BYTES - s_len));

  ECDSA_SIG_free(sig);

  return(SUCCESS);
}

// Sign `msg` with `pk` (ES256), returning the IEEE P1363 64-byte
// signature in `sig_out`.
static bool
cb_sign_es256(EVP_PKEY *pk, const unsigned char *msg, size_t msg_len,
    unsigned char sig_out[CB_P256_SIG_BYTES])
{
  EVP_MD_CTX    *ctx;
  unsigned char  der[128];   // P-256 DER ECDSA sig is <= ~72 B
  size_t         der_len = sizeof(der);
  bool           ok;

  ctx = EVP_MD_CTX_new();

  if(ctx == NULL)
    return(FAIL);

  if(EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, pk) != 1
      || EVP_DigestSign(ctx, der, &der_len, msg, msg_len) != 1)
  {
    EVP_MD_CTX_free(ctx);
    return(FAIL);
  }

  EVP_MD_CTX_free(ctx);

  ok = cb_der_to_p1363(der, der_len, sig_out);

  return(ok);
}

// Render `len` random bytes as lowercase hex into `out`. `out` must
// hold at least len*2 + 1 chars.
static bool
cb_random_hex(char *out, size_t hex_chars)
{
  static const char    digits[] = "0123456789abcdef";
  unsigned char        buf[CB_JWT_NONCE_BYTES];
  size_t               i;
  ssize_t              n;

  if(hex_chars != CB_JWT_NONCE_HEX)
    return(FAIL);

  n = getrandom(buf, sizeof(buf), 0);

  if(n != (ssize_t)sizeof(buf))
    return(FAIL);

  for(i = 0; i < sizeof(buf); i++)
  {
    out[i * 2]     = digits[(buf[i] >> 4) & 0xf];
    out[i * 2 + 1] = digits[buf[i] & 0xf];
  }

  out[hex_chars] = '\0';

  return(SUCCESS);
}

// Strip a leading "https://" / "http://" / "wss://" / "ws://" scheme
// from `url` and copy the host (no path, no port) into `out`. Returns
// FAIL on malformed input.
static bool
cb_url_host(const char *url, char *out, size_t cap)
{
  const char *p;
  const char *end;
  size_t      n;

  if(url == NULL || out == NULL || cap == 0)
    return(FAIL);

  p = strstr(url, "://");
  p = (p != NULL) ? p + 3 : url;

  end = strchr(p, '/');

  if(end == NULL)
    end = p + strlen(p);

  n = (size_t)(end - p);

  if(n == 0 || n + 1 > cap)
    return(FAIL);

  memcpy(out, p, n);
  out[n] = '\0';

  return(SUCCESS);
}

// Inner JWT builder. When `uri_claim` is non-NULL it is embedded
// verbatim as the payload's "uri" claim (REST minting passes
// "<METHOD> <host><path>"); when NULL the claim is omitted (Advanced
// Trade WebSocket auth).
static bool
cb_sign_jwt_inner(const char *uri_claim, char *out, size_t cap)
{
  EVP_PKEY      *pk;
  const char    *key_name;
  char           nonce[CB_JWT_NONCE_HEX + 1];
  char           header_json[256];
  char           payload_json[512];
  char           header_b64[384];
  char           payload_b64[768];
  char           sig_b64[128];
  unsigned char  sig_raw[CB_P256_SIG_BYTES];
  char           signing_input[1280];
  int            n;
  int            si_len;
  size_t         hb_len;
  size_t         pb_len;
  size_t         sb_len;
  time_t         now;

  if(out == NULL || cap == 0)
    return(FAIL);

  kv_admin_context_set(true);
  key_name = kv_get_str("plugin.coinbase.creds.key_name");
  kv_admin_context_set(false);

  if(key_name == NULL || key_name[0] == '\0')
    return(FAIL);

  if(cb_random_hex(nonce, CB_JWT_NONCE_HEX) != SUCCESS)
    return(FAIL);

  n = snprintf(header_json, sizeof(header_json),
      "{\"alg\":\"ES256\",\"typ\":\"JWT\",\"kid\":\"%s\",\"nonce\":\"%s\"}",
      key_name, nonce);

  if(n < 0 || (size_t)n >= sizeof(header_json))
    return(FAIL);

  hb_len = util_b64url_encode(header_json, (size_t)n,
      header_b64, sizeof(header_b64));

  if(hb_len == 0)
    return(FAIL);

  now = time(NULL);

  if(uri_claim != NULL)
  {
    n = snprintf(payload_json, sizeof(payload_json),
        "{\"iss\":\"cdp\",\"sub\":\"%s\","
        "\"nbf\":%lld,\"exp\":%lld,\"uri\":\"%s\"}",
        key_name,
        (long long)now,
        (long long)now + CB_JWT_LIFETIME_SEC,
        uri_claim);
  }
  else
  {
    n = snprintf(payload_json, sizeof(payload_json),
        "{\"iss\":\"cdp\",\"sub\":\"%s\","
        "\"nbf\":%lld,\"exp\":%lld}",
        key_name,
        (long long)now,
        (long long)now + CB_JWT_LIFETIME_SEC);
  }

  if(n < 0 || (size_t)n >= sizeof(payload_json))
    return(FAIL);

  pb_len = util_b64url_encode(payload_json, (size_t)n,
      payload_b64, sizeof(payload_b64));

  if(pb_len == 0)
    return(FAIL);

  // Signing input: header_b64 "." payload_b64
  si_len = snprintf(signing_input, sizeof(signing_input), "%s.%s",
      header_b64, payload_b64);

  if(si_len < 0 || (size_t)si_len >= sizeof(signing_input))
    return(FAIL);

  // Hold cb_cdp_mu across both the key (re-)load and the sign so
  // another thread can't EVP_PKEY_free the cached pointer between
  // the two calls.
  pthread_mutex_lock(&cb_cdp_mu);

  if(cb_load_cdp_key_locked(&pk) != SUCCESS)
  {
    pthread_mutex_unlock(&cb_cdp_mu);
    return(FAIL);
  }

  if(cb_sign_es256(pk, (const unsigned char *)signing_input,
        (size_t)si_len, sig_raw) != SUCCESS)
  {
    pthread_mutex_unlock(&cb_cdp_mu);
    return(FAIL);
  }

  pthread_mutex_unlock(&cb_cdp_mu);

  sb_len = util_b64url_encode(sig_raw, sizeof(sig_raw),
      sig_b64, sizeof(sig_b64));

  if(sb_len == 0)
    return(FAIL);

  n = snprintf(out, cap, "%s.%s", signing_input, sig_b64);

  if(n < 0 || (size_t)n >= cap)
    return(FAIL);

  return(SUCCESS);
}

// Build a fresh CDP JWT for a REST request. `method` is uppercase
// ("GET" / "POST" / "DELETE"); `path` is the absolute request path and
// MAY carry a query string. The "uri" claim binds the JWT to the
// request, so callers must mint per-request.
//
// CDP computes the claim from METHOD + host + path with the query string
// EXCLUDED. Including "?..." in the claim makes Coinbase reject the
// request 401 — which is why query-less endpoints (/accounts) signed fine
// while every GET with query params (/orders/historical/batch?...,
// /orders/historical/fills?...) failed. Strip the query here so the URL
// curl sends keeps it but the signed claim does not.
bool
cb_sign_jwt(const char *method, const char *path, char *out, size_t cap)
{
  char        rest_url[CB_URL_SZ];
  char        host[256];
  char        path_noquery[CB_URL_SZ];
  char        uri[1024];
  const char *q;
  size_t      plen;
  int         n;

  if(method == NULL || path == NULL)
    return(FAIL);

  if(cb_rest_base_url(rest_url, sizeof(rest_url)) != SUCCESS)
    return(FAIL);

  if(cb_url_host(rest_url, host, sizeof(host)) != SUCCESS)
    return(FAIL);

  // Copy the path up to (but not including) any '?'.
  q    = strchr(path, '?');
  plen = (q != NULL) ? (size_t)(q - path) : strlen(path);

  if(plen >= sizeof(path_noquery))
    plen = sizeof(path_noquery) - 1;

  memcpy(path_noquery, path, plen);
  path_noquery[plen] = '\0';

  n = snprintf(uri, sizeof(uri), "%s %s%s", method, host, path_noquery);

  if(n < 0 || (size_t)n >= sizeof(uri))
    return(FAIL);

  return(cb_sign_jwt_inner(uri, out, cap));
}

// Build a fresh CDP JWT for a WebSocket subscribe payload. Advanced
// Trade WS auth omits the "uri" claim — the same JWT is valid against
// any subscribe within its lifetime. Mint fresh per subscribe (cheap;
// avoids re-auth races on long-lived sessions).
bool
cb_sign_jwt_ws(char *out, size_t cap)
{
  return(cb_sign_jwt_inner(NULL, out, cap));
}

bool
cb_cdp_configured(void)
{
  const char *k;
  const char *p;

  kv_admin_context_set(true);
  k = kv_get_str("plugin.coinbase.creds.key_name");
  p = kv_get_str("plugin.coinbase.creds.private_key_pem");
  kv_admin_context_set(false);

  return(k != NULL && k[0] != '\0' && p != NULL && p[0] != '\0');
}

void
cb_cdp_deinit(void)
{
  pthread_mutex_lock(&cb_cdp_mu);

  if(cb_cdp_pkey != NULL)
  {
    EVP_PKEY_free(cb_cdp_pkey);
    cb_cdp_pkey = NULL;
  }

  if(cb_cdp_pem_snap != NULL)
  {
    mem_free(cb_cdp_pem_snap);
    cb_cdp_pem_snap = NULL;
  }

  pthread_mutex_unlock(&cb_cdp_mu);
}
