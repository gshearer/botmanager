// botmanager — MIT
// Coinbase Exchange: HMAC-SHA256 request signing + base URL selection.
//
// Both the REST dispatcher (CB2/CB3) and the WebSocket authenticated
// subscribe path (CB5) rely on this TU for the CB-ACCESS-SIGN
// construction and the sandbox-aware endpoint selection. No network
// I/O happens here.
#define CB_INTERNAL
#include "coinbase.h"

#include "util.h"  // util_b64_encode

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

// URL + credential helpers

bool
cb_sandbox_enabled(void)
{
  return((uint8_t)kv_get_uint("plugin.coinbase.sandbox") != 0);
}

// Latched at plugin init (cb_active_name_init). 16 bytes covers both
// "coinbase" and "coinbase-sb" with slack; cap stays well below the
// VARCHAR(32) of wm_market.exchange so qualifier strings round-trip
// through the registry cleanly.
static char cb_active_name[16] = "coinbase";

void
cb_active_name_init(void)
{
  if(cb_sandbox_enabled())
    snprintf(cb_active_name, sizeof(cb_active_name), "coinbase-sb");

  else
    snprintf(cb_active_name, sizeof(cb_active_name), "coinbase");
}

const char *
cb_active_exchange_name(void)
{
  return(cb_active_name);
}

bool
cb_rest_base_url(char *out, size_t cap)
{
  const char *src;

  if(out == NULL || cap == 0)
    return(FAIL);

  src = kv_get_str(cb_sandbox_enabled()
      ? "plugin.coinbase.rest_url_sandbox"
      : "plugin.coinbase.rest_url_prod");

  if(src == NULL || src[0] == '\0')
    return(FAIL);

  snprintf(out, cap, "%s", src);

  return(SUCCESS);
}

bool
cb_ws_base_url(char *out, size_t cap)
{
  const char *src;

  if(out == NULL || cap == 0)
    return(FAIL);

  src = kv_get_str(cb_sandbox_enabled()
      ? "plugin.coinbase.ws_url_sandbox"
      : "plugin.coinbase.ws_url_prod");

  if(src == NULL || src[0] == '\0')
    return(FAIL);

  snprintf(out, cap, "%s", src);

  return(SUCCESS);
}

bool
cb_apikey_configured(void)
{
  const char *k = kv_get_str("plugin.coinbase.apikey");
  const char *s = kv_get_str("plugin.coinbase.apisecret");
  const char *p = kv_get_str("plugin.coinbase.passphrase");

  return(k != NULL && k[0] != '\0'
      && s != NULL && s[0] != '\0'
      && p != NULL && p[0] != '\0');
}

size_t
cb_timestamp_str(char *out, size_t cap)
{
  int n;

  if(out == NULL || cap == 0)
    return(0);

  n = snprintf(out, cap, "%ld", (long)time(NULL));

  if(n < 0 || (size_t)n >= cap)
    return(0);

  return((size_t)n);
}

// Produce the base64(CB-ACCESS-SIGN) value over:
//   ts + METHOD + path + body
//
// `ts` must already be rendered by the caller (see cb_timestamp_str).
// Body is optional — a GET or empty-body POST passes NULL/0. On
// success the signature is written NUL-terminated to sig_out.
bool
cb_sign_request(const char *method, const char *path,
    const char *body, size_t body_len, const char *ts,
    char *sig_out, size_t sig_cap)
{
  const char    *b64secret;
  unsigned char  key[64];
  int            key_len;
  char           prehash[CB_PRESIGN_SZ];
  int            prehash_len;
  size_t         secret_len;
  unsigned char  mac[EVP_MAX_MD_SIZE];
  unsigned int   mac_len = 0;

  if(method == NULL || path == NULL || ts == NULL
      || sig_out == NULL || sig_cap < 1)
    return(FAIL);

  b64secret = kv_get_str("plugin.coinbase.apisecret");

  if(b64secret == NULL || b64secret[0] == '\0')
    return(FAIL);

  secret_len = strlen(b64secret);

  // EVP_DecodeBlock demands input length be a multiple of 4 and will
  // write up to secret_len * 3 / 4 bytes. Our `key` buffer holds 64
  // bytes, enough for a well-formed Coinbase secret (88 b64 chars →
  // 64 decoded bytes). Reject anything larger.
  if(secret_len > 88)
    return(FAIL);

  key_len = EVP_DecodeBlock(key, (const unsigned char *)b64secret,
      (int)secret_len);

  if(key_len <= 0)
    return(FAIL);

  // EVP_DecodeBlock rounds up; subtract '=' padding to get the true
  // length (matches standard base64 semantics).
  if(secret_len >= 1 && b64secret[secret_len - 1] == '=')
    key_len--;

  if(secret_len >= 2 && b64secret[secret_len - 2] == '=')
    key_len--;

  if(key_len <= 0)
    return(FAIL);

  prehash_len = snprintf(prehash, sizeof(prehash), "%s%s%s",
      ts, method, path);

  if(prehash_len < 0 || (size_t)prehash_len >= sizeof(prehash))
    return(FAIL);

  if(body != NULL && body_len > 0)
  {
    if((size_t)prehash_len + body_len >= sizeof(prehash))
      return(FAIL);

    memcpy(prehash + prehash_len, body, body_len);
    prehash_len += (int)body_len;
  }

  if(HMAC(EVP_sha256(), key, key_len,
        (const unsigned char *)prehash, (size_t)prehash_len,
        mac, &mac_len) == NULL)
    return(FAIL);

  if(util_b64_encode(mac, mac_len, sig_out, sig_cap) == 0)
    return(FAIL);

  return(SUCCESS);
}

// Public symbol exported via plugin_dlsym. Mirrors the
// coinmarketcap_apikey_configured shape so consumers see a stable,
// callback-free "is credential state ready" probe.
bool
coinbase_apikey_configured(void)
{
  return(cb_apikey_configured());
}

// Public probe for the sandbox flag — see coinbase_api.h for the
// contract. Whenmoon's wm_market_lookup_or_create reads this to
// decide between "coinbase" and "coinbase-sb" registry rows.
bool
coinbase_sandbox_active(void)
{
  return(cb_sandbox_enabled());
}
