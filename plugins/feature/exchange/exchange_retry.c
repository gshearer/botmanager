// botmanager — MIT
// HTTP status classification + exponential backoff schedule used by the
// exchange dispatch loop. Generalised from plugins/service/coinbase/
// coinbase_rest.c::cb_classify_http (which was Coinbase-specific in
// wording only — the policy is the same: 429/5xx retry, 4xx-other surface).

#define EXCHANGE_INTERNAL
#include "exchange.h"

#include <stdio.h>

exchange_outcome_t
exchange_classify_status(int http_status, bool transport_err)
{
  // Negative http_status is a sentinel for "request cancelled" used by
  // protocol plugins to signal that the underlying transfer was aborted
  // (e.g. curl shutdown drain returning CURLE_ABORTED_BY_CALLBACK). Map
  // to FAIL: a re-submit would be rejected by the same gate that
  // cancelled it, so retrying just leaks an exchange_req_t and pins
  // upstream consumers waiting on their completion callback.
  if(http_status < 0)
    return(EXCHANGE_OUTCOME_FAIL);

  if(transport_err)
    return(EXCHANGE_OUTCOME_RETRY);

  if(http_status >= 200 && http_status < 300)
    return(EXCHANGE_OUTCOME_OK);

  if(http_status == 429)
    return(EXCHANGE_OUTCOME_RETRY);

  if(http_status >= 500 && http_status < 600)
    return(EXCHANGE_OUTCOME_RETRY);

  return(EXCHANGE_OUTCOME_FAIL);
}

uint32_t
exchange_backoff_ms(uint32_t attempt)
{
  // 250 → 500 → 1000 → 2000 → 4000 ms cap (5 attempts).
  uint32_t ms = EXCHANGE_RETRY_BASE_MS;
  uint32_t i;

  for(i = 0; i < attempt; i++)
  {
    ms *= 2;

    if(ms >= EXCHANGE_RETRY_CAP_MS)
    {
      ms = EXCHANGE_RETRY_CAP_MS;
      break;
    }
  }

  return(ms);
}

const char *
exchange_describe_status(int http_status, bool transport_err,
    const char *upstream_err, char *buf, size_t cap)
{
  if(transport_err)
  {
    snprintf(buf, cap, "exchange transport error: %s",
        upstream_err != NULL ? upstream_err : "unknown");
    return(buf);
  }

  switch(http_status)
  {
    case 200:
    case 201:
    case 202:
    case 204:
      return(NULL);

    case 400:
      return("exchange returned 400 (bad request)");

    case 401:
      return("exchange returned 401 (invalid credentials)");

    case 403:
      return("exchange returned 403 (permission denied)");

    case 404:
      return("exchange returned 404 (not found)");

    case 429:
      return("exchange returned 429 (rate limit, retry exhausted)");

    case 500:
    case 502:
    case 503:
    case 504:
      snprintf(buf, cap,
          "exchange backend error (HTTP %d, retry exhausted)",
          http_status);
      return(buf);

    default:
      snprintf(buf, cap, "exchange returned HTTP %d", http_status);
      return(buf);
  }
}
