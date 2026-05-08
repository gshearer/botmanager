// botmanager — MIT
// Coinbase Advanced Trade: URL/credential helpers shared between the
// REST dispatcher and the WebSocket subsystem. Per-request JWT/ES256
// signing lives in coinbase_sign_cdp.c. No network I/O happens here.
#define CB_INTERNAL
#include "coinbase.h"

#include <stdio.h>
#include <string.h>

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

// Public probe for the sandbox flag — see coinbase_api.h for the
// contract. Whenmoon's wm_market_lookup_or_create reads this to
// decide between "coinbase" and "coinbase-sb" registry rows.
bool
coinbase_sandbox_active(void)
{
  return(cb_sandbox_enabled());
}
