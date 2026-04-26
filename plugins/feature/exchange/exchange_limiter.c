// botmanager — MIT
// Token-bucket + reserved-slot limiter for exchange dispatch.
//
// Bucket math is adapted from plugins/feature/whenmoon/dl_scheduler.c
// (the legacy per-bot limiter retired in EX-1). The reserved-slot
// policy is new: each priority tier holds N tokens hostage so a flood
// of low-priority backfill cannot starve a transactional buy/sell.

#define EXCHANGE_INTERNAL
#include "exchange.h"

uint32_t
exchange_prio_tier(uint8_t prio)
{
  if(prio <= EXCHANGE_PRIO_TRANSACTIONAL)    return(EXCHANGE_TIER_TXN);
  if(prio <= EXCHANGE_PRIO_MARKET_BACKFILL)  return(EXCHANGE_TIER_BACK);

  return(EXCHANGE_TIER_USER);
}

void
exchange_limiter_init(exchange_limiter_t *lim,
    uint32_t advertised_rps, uint32_t advertised_burst)
{
  uint32_t effective_rps;
  double   burst;

  if(lim == NULL)
    return;

  memset(lim, 0, sizeof(*lim));

  // Subtract a small headroom so non-abstraction traffic outside the
  // queue (admin polls, ws keepalives, etc.) cannot push us over the
  // exchange-published cap.
  effective_rps = advertised_rps > EXCHANGE_RPS_HEADROOM
                ? advertised_rps - EXCHANGE_RPS_HEADROOM
                : 1;

  burst = advertised_burst > 0 ? (double)advertised_burst
                               : (double)effective_rps * 1.5;

  lim->tokens_per_sec = (double)effective_rps;
  lim->tokens_cap     = burst;
  lim->tokens         = burst;       // start full

  // Default reservation: keep one slot for transactional traffic. Lower
  // tiers start with zero reservation; KV overrides land in
  // exchange.c when the per-exchange schema is registered.
  lim->reserved[EXCHANGE_TIER_TXN]  = 1;
  lim->reserved[EXCHANGE_TIER_BACK] = 0;
  lim->reserved[EXCHANGE_TIER_USER] = 0;

  clock_gettime(CLOCK_MONOTONIC, &lim->last_refill);
}

void
exchange_limiter_refill_locked(exchange_limiter_t *lim)
{
  struct timespec now;
  double          elapsed;

  if(lim == NULL)
    return;

  clock_gettime(CLOCK_MONOTONIC, &now);

  elapsed = (double)(now.tv_sec - lim->last_refill.tv_sec)
          + (double)(now.tv_nsec - lim->last_refill.tv_nsec) / 1e9;

  if(elapsed <= 0.0)
    return;

  // Pay back deficit before issuing new tokens.
  {
    double gained = elapsed * lim->tokens_per_sec;

    if(lim->deficit > 0.0)
    {
      if(gained >= lim->deficit)
      {
        gained      -= lim->deficit;
        lim->deficit = 0.0;
      }

      else
      {
        lim->deficit -= gained;
        gained        = 0.0;
      }
    }

    lim->tokens += gained;
  }

  if(lim->tokens > lim->tokens_cap)
    lim->tokens = lim->tokens_cap;

  lim->last_refill = now;
}

bool
exchange_limiter_take_locked(exchange_limiter_t *lim, uint8_t prio)
{
  uint32_t my_tier;
  double   reserved_above;
  uint32_t i;

  if(lim == NULL)
    return(false);

  exchange_limiter_refill_locked(lim);

  my_tier = exchange_prio_tier(prio);

  // Sum reservations for tiers strictly above this request's tier — i.e.
  // higher priority (lower numeric tier index). Those tokens are off-
  // limits to the current request.
  reserved_above = 0.0;

  for(i = 0; i < my_tier; i++)
    reserved_above += (double)lim->reserved[i];

  if(lim->tokens >= 1.0 + reserved_above)
  {
    lim->tokens -= 1.0;
    return(true);
  }

  return(false);
}

void
exchange_limiter_refund_locked(exchange_limiter_t *lim)
{
  if(lim == NULL)
    return;

  lim->tokens += 1.0;

  if(lim->tokens > lim->tokens_cap)
    lim->tokens = lim->tokens_cap;
}

void
exchange_limiter_penalty_locked(exchange_limiter_t *lim, double extra_tokens)
{
  if(lim == NULL || extra_tokens <= 0.0)
    return;

  // Drain the bucket up to `extra_tokens`; any unmet portion accrues to
  // the deficit so subsequent refills pay it back before issuing new
  // tokens. Cap deficit so a pathological 429 storm cannot stall
  // dispatch indefinitely.
  if(lim->tokens >= extra_tokens)
  {
    lim->tokens -= extra_tokens;
  }

  else
  {
    double remaining = extra_tokens - lim->tokens;

    lim->tokens   = 0.0;
    lim->deficit += remaining;

    if(lim->deficit > lim->tokens_cap)
      lim->deficit = lim->tokens_cap;
  }
}
