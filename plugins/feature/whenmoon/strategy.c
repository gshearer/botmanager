// botmanager — MIT
// Two-tier KV resolver for whenmoon strategies. Per-market override,
// global default, compiled fallback.

#define WHENMOON_INTERNAL
#include "strategy.h"

#include "kv.h"

#include <stdio.h>

// Build the per-market path and test for existence. Returns true iff
// the per-market override key is registered (kv_exists).
static bool
wm_strategy_per_market_path(const char *market_id, const char *strategy,
    const char *key, char *out, size_t out_sz)
{
  int n;

  if(market_id == NULL || market_id[0] == '\0' ||
     strategy == NULL || key == NULL)
    return(false);

  n = snprintf(out, out_sz,
      "plugin.whenmoon.market.%s.strategy.%s.%s",
      market_id, strategy, key);

  if(n < 0 || (size_t)n >= out_sz)
    return(false);

  return(kv_exists(out));
}

static bool
wm_strategy_global_path(const char *strategy, const char *key,
    char *out, size_t out_sz)
{
  int n;

  if(strategy == NULL || key == NULL)
    return(false);

  n = snprintf(out, out_sz,
      "plugin.whenmoon.strategy.%s.%s", strategy, key);

  if(n < 0 || (size_t)n >= out_sz)
    return(false);

  return(kv_exists(out));
}

uint64_t
wm_strategy_kv_get_uint(const char *market_id, const char *strategy,
    const char *key, uint64_t dflt)
{
  char path[KV_KEY_SZ];

  if(wm_strategy_per_market_path(market_id, strategy, key,
         path, sizeof(path)))
    return(kv_get_uint(path));

  if(wm_strategy_global_path(strategy, key, path, sizeof(path)))
    return(kv_get_uint(path));

  return(dflt);
}

int64_t
wm_strategy_kv_get_int(const char *market_id, const char *strategy,
    const char *key, int64_t dflt)
{
  char path[KV_KEY_SZ];

  if(wm_strategy_per_market_path(market_id, strategy, key,
         path, sizeof(path)))
    return(kv_get_int(path));

  if(wm_strategy_global_path(strategy, key, path, sizeof(path)))
    return(kv_get_int(path));

  return(dflt);
}

const char *
wm_strategy_kv_get_str(const char *market_id, const char *strategy,
    const char *key, const char *dflt)
{
  char        path[KV_KEY_SZ];
  const char *v;

  if(wm_strategy_per_market_path(market_id, strategy, key,
         path, sizeof(path)))
  {
    v = kv_get_str(path);

    if(v != NULL)
      return(v);
  }

  if(wm_strategy_global_path(strategy, key, path, sizeof(path)))
  {
    v = kv_get_str(path);

    if(v != NULL)
      return(v);
  }

  return(dflt);
}
