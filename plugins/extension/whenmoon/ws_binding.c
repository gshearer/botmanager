// botmanager — MIT
// ws_binding.c — OBS-41: what a WS binding subscribed with, and whether it still matches.

#include "ws_binding.h"

#include <string.h>

static bool wm_ws_recorded_contains(const wm_ws_product_set_t *, const char *);
static bool wm_ws_list_contains(const char *const *, uint32_t, const char *);

static bool
wm_ws_recorded_contains(const wm_ws_product_set_t *set, const char *product)
{
  uint32_t i;

  for(i = 0; i < set->n_products; i++)
  {
    if(strncmp(set->products[i], product, WM_WS_PRODUCT_ID_SZ) == 0)
      return(true);
  }

  return(false);
}

static bool
wm_ws_list_contains(const char *const *products, uint32_t n_products,
    const char *product)
{
  uint32_t i;

  for(i = 0; i < n_products; i++)
  {
    if(products[i] == NULL || products[i][0] == '\0')
      continue;

    if(strncmp(products[i], product, WM_WS_PRODUCT_ID_SZ) == 0)
      return(true);
  }

  return(false);
}

bool
wm_ws_product_set_record(wm_ws_product_set_t *set,
    const char *const *products, uint32_t n_products)
{
  uint32_t i;
  bool     fit = true;

  if(set == NULL)
    return(false);

  wm_ws_product_set_clear(set);

  if(products == NULL)
    return(true);

  for(i = 0; i < n_products; i++)
  {
    if(products[i] == NULL || products[i][0] == '\0')
      continue;

    if(wm_ws_recorded_contains(set, products[i]))
      continue;

    if(set->n_products >= WM_WS_BINDING_MAX_PRODUCTS)
    {
      fit = false;
      break;
    }

    strlcpy(set->products[set->n_products], products[i],
        sizeof(set->products[set->n_products]));
    set->n_products++;
  }

  set->truncated = !fit;
  return(fit);
}

bool
wm_ws_product_set_differs(const wm_ws_product_set_t *set,
    const char *const *products, uint32_t n_products)
{
  uint32_t i;

  if(set == NULL || set->truncated)
    return(true);

  if(products == NULL)
    n_products = 0;

  for(i = 0; i < n_products; i++)
  {
    if(products[i] == NULL || products[i][0] == '\0')
      continue;

    if(!wm_ws_recorded_contains(set, products[i]))
      return(true);
  }

  for(i = 0; i < set->n_products; i++)
  {
    if(!wm_ws_list_contains(products, n_products, set->products[i]))
      return(true);
  }

  return(false);
}

void
wm_ws_product_set_clear(wm_ws_product_set_t *set)
{
  if(set == NULL)
    return;

  memset(set, 0, sizeof(*set));
}
