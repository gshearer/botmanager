// botmanager — MIT
// Coinbase Advanced Trade: URL/credential helpers shared between the
// REST dispatcher and the WebSocket subsystem. Per-request JWT/ES256
// signing lives in coinbase_sign_cdp.c. No network I/O happens here.
#define CB_INTERNAL
#include "coinbase.h"

#include <stdio.h>
#include <string.h>

bool
cb_rest_base_url(char *out, size_t cap)
{
  const char *src;

  if(out == NULL || cap == 0)
    return(FAIL);

  src = kv_get_str("plugin.coinbase.rest_url");

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

  src = kv_get_str("plugin.coinbase.ws_url");

  if(src == NULL || src[0] == '\0')
    return(FAIL);

  snprintf(out, cap, "%s", src);

  return(SUCCESS);
}

bool
cb_apikey_configured(void)
{
  return(cb_cdp_configured());
}

// Public symbol exported via plugin_dlsym. Mirrors the
// coinmarketcap_apikey_configured shape so consumers see a stable,
// callback-free "is credential state ready" probe.
bool
coinbase_apikey_configured(void)
{
  return(cb_apikey_configured());
}
