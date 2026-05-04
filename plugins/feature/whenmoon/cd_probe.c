// cd_probe.c — bisection probe of Coinbase candle history depth.
// WM-CD-1 Phase 1.

#define WHENMOON_INTERNAL
#include "cd_probe.h"

#include "coinbase_api.h"
#include "dl_schema.h"     // WM_DL_CTX
#include "exchange_api.h"  // EXCHANGE_PRIO_USER_DOWNLOAD

#include "alloc.h"
#include "clam.h"
#include "common.h"        // SUCCESS / FAIL
#include "kv.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// Bisection bounds: lo starts at 0 (no depth confirmed) and grows on
// every reachable probe; hi starts at CD_PROBE_MAX_DAYS (5 years) and
// shrinks on every empty/error probe. The terminal `lo` is published
// to KV as the discovered depth cap.
#define CD_PROBE_MIN_DAYS    0
#define CD_PROBE_MAX_DAYS    1825   // 5 years
#define CD_PROBE_MAX_ITERS   16     // ceil(log2(1825)) = 11; safety cap

static const int32_t CD_PROBE_GRANS[] = {
  60, 300, 900, 3600, 21600, 86400
};

#define CD_PROBE_GRAN_N \
  ((int32_t)(sizeof(CD_PROBE_GRANS) / sizeof(CD_PROBE_GRANS[0])))

typedef struct
{
  char    symbol[32];
  int32_t gran;
  int32_t lo_days;     // largest depth confirmed reachable
  int32_t hi_days;     // smallest depth confirmed unreachable
  int32_t mid_days;    // depth currently in flight
  int32_t iter;
} cd_probe_state_t;

static void cd_probe_fire(cd_probe_state_t *s);

static void
cd_probe_publish(const cd_probe_state_t *s)
{
  char key[80];

  snprintf(key, sizeof(key),
      "plugin.whenmoon.candles.%" PRId32 ".max_lookback_days",
      s->gran);

  // SUCCESS/FAIL convention is inverted (SUCCESS=false): test against
  // SUCCESS, not `true`. The earlier `!= true` check fired the WARN on
  // every successful set.
  if(kv_set_uint(key, (uint64_t)s->lo_days) != SUCCESS)
    clam(CLAM_WARN, WM_DL_CTX,
        "cd_probe: kv_set_uint failed for %s val=%" PRId32,
        key, s->lo_days);

  clam(CLAM_INFO, WM_DL_CTX,
      "cd_probe: gran=%" PRId32 "s symbol=%s"
      " max_lookback_days=%" PRId32 " iters=%" PRId32,
      s->gran, s->symbol, s->lo_days, s->iter);
}

static void
cd_probe_done(const coinbase_candles_result_t *res, void *user)
{
  cd_probe_state_t *s = user;
  bool              reachable;

  if(s == NULL)
  {
    clam(CLAM_WARN, WM_DL_CTX,
        "cd_probe: done user=NULL res=%p", (const void *)res);
    return;
  }

  clam(CLAM_DEBUG, WM_DL_CTX,
      "cd_probe: done gran=%" PRId32 "s symbol=%s iter=%" PRId32
      " res=%s err='%s' count=%u",
      s->gran, s->symbol, s->iter,
      res == NULL ? "NULL" : "ok",
      res == NULL ? "" : res->err,
      res == NULL ? 0u : res->count);

  if(res == NULL)
  {
    clam(CLAM_WARN, WM_DL_CTX,
        "cd_probe: NULL result; gran=%" PRId32 "s symbol=%s",
        s->gran, s->symbol);
    cd_probe_publish(s);
    mem_free(s);
    return;
  }

  if(res->err[0] != '\0')
  {
    // Treat any error (HTTP 4xx/5xx, parse failure, submit fail) as
    // "depth unreachable" — Coinbase rejects out-of-range windows with
    // 400, which is exactly the signal we are bisecting on. Continue
    // the bisection so we still narrow the cap even if the deepest
    // probe returns an error rather than an empty body.
    reachable = false;
  }
  else
  {
    reachable = (res->count > 0);
  }

  if(reachable)
    s->lo_days = s->mid_days;
  else
    s->hi_days = s->mid_days;

  s->iter++;

  if(s->hi_days - s->lo_days <= 1 || s->iter >= CD_PROBE_MAX_ITERS)
  {
    cd_probe_publish(s);
    mem_free(s);
    return;
  }

  cd_probe_fire(s);
}

static void
cd_probe_fire(cd_probe_state_t *s)
{
  int64_t now_s;
  int64_t end_s;
  int64_t start_s;

  s->mid_days = (s->lo_days + s->hi_days) / 2;

  // Defensive: bisection should never invoke this with a degenerate
  // interval, but if it ever does, fall through to publish without
  // submitting a request.
  if(s->mid_days <= s->lo_days)
  {
    cd_probe_publish(s);
    mem_free(s);
    return;
  }

  now_s   = (int64_t)time(NULL);
  end_s   = now_s - (int64_t)s->mid_days * 86400;
  start_s = end_s - (int64_t)300 * (int64_t)s->gran;

  if(start_s < 0)
    start_s = 0;

  clam(CLAM_DEBUG, WM_DL_CTX,
      "cd_probe: fire gran=%" PRId32 "s symbol=%s iter=%" PRId32
      " lo=%" PRId32 " hi=%" PRId32 " mid=%" PRId32
      " start=%" PRId64 " end=%" PRId64,
      s->gran, s->symbol, s->iter,
      s->lo_days, s->hi_days, s->mid_days, start_s, end_s);

  if(end_s <= start_s)
  {
    // Probe window collapsed (mid pushed end_s back past epoch+window).
    // Treat as unreachable; advance the bisection rather than firing.
    s->hi_days = s->mid_days;
    s->iter++;

    clam(CLAM_INFO, WM_DL_CTX,
        "cd_probe: window collapsed; gran=%" PRId32 "s iter=%" PRId32
        " lo=%" PRId32 " hi=%" PRId32,
        s->gran, s->iter, s->lo_days, s->hi_days);

    if(s->hi_days - s->lo_days <= 1 || s->iter >= CD_PROBE_MAX_ITERS)
    {
      cd_probe_publish(s);
      mem_free(s);
      return;
    }

    cd_probe_fire(s);
    return;
  }

  // Coinbase treats `end` as inclusive; subtract one bucket so the
  // boundary doesn't double-count (mirrors wm_dl_candles_dispatch_one).
  if(coinbase_fetch_candles_async(s->symbol, s->gran,
        start_s, end_s - 1,
        EXCHANGE_PRIO_USER_DOWNLOAD,
        cd_probe_done, s) != SUCCESS)
  {
    // The shim contract: on submit failure the completion callback
    // fires synchronously with res->err set, so ownership has already
    // transferred. Do NOT free here.
    clam(CLAM_INFO, WM_DL_CTX,
        "cd_probe: submit returned FAIL; gran=%" PRId32 "s iter=%"
        PRId32 " (cd_probe_done is firing synchronously per shim contract)",
        s->gran, s->iter);
  }
}

bool
wm_cd_probe_run(const char *symbol)
{
  int32_t i;
  int32_t fired = 0;

  if(symbol == NULL || symbol[0] == '\0')
    return(FAIL);

  for(i = 0; i < CD_PROBE_GRAN_N; i++)
  {
    cd_probe_state_t *s;

    s = mem_alloc(WM_DL_CTX, "cd_probe", sizeof(*s));

    if(s == NULL)
    {
      clam(CLAM_WARN, WM_DL_CTX,
          "cd_probe: alloc failed for gran=%" PRId32 "s",
          CD_PROBE_GRANS[i]);
      continue;
    }

    snprintf(s->symbol, sizeof(s->symbol), "%s", symbol);
    s->gran     = CD_PROBE_GRANS[i];
    s->lo_days  = CD_PROBE_MIN_DAYS;
    s->hi_days  = CD_PROBE_MAX_DAYS;
    s->mid_days = 0;
    s->iter     = 0;

    clam(CLAM_DEBUG, WM_DL_CTX,
        "cd_probe: launching gran=%" PRId32 "s symbol=%s",
        s->gran, s->symbol);

    fired++;
    cd_probe_fire(s);
  }

  return(fired > 0 ? SUCCESS : FAIL);
}
