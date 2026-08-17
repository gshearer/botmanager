// botmanager — MIT
// Kraken assetpairs-cache persistence (EXCH-PRIME-1).
//
// Mirrors the in-memory kr_pairs cache to the `kraken_assetpairs`
// Postgres table and primes it back on startup. This replaces the
// synchronous network prime that kr_start used to run inside the plugin
// start() path — the operator control socket no longer waits on
// GET /0/public/AssetPairs. The network refresh fires only when the
// persisted snapshot is missing or older than the staleness window
// (plugin.kraken.assetpairs_refresh_sec).
#define KR_INTERNAL
#include "kraken.h"

#include "kraken_persist.h"

#include "db.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------
// Schema
// ------------------------------------------------------------------

// Canonical DDL lives in scripts/schema.sql; this mirror keeps the
// runtime self-bootstrapping (CREATE TABLE IF NOT EXISTS). `wsname` is
// stored with the leading XBT/ → BTC/ rewrite already applied, so load
// is a straight re-add (kr_pairs_add only rewrites a leading XBT/).
static const char kr_assetpairs_ddl[] =
  "CREATE TABLE IF NOT EXISTS kraken_assetpairs ("
  " altname    VARCHAR(16)  PRIMARY KEY,"
  " canonical  VARCHAR(16)  NOT NULL,"
  " wsname     VARCHAR(32)  NOT NULL,"
  " fetched_at TIMESTAMPTZ  NOT NULL DEFAULT NOW()"
  ")";

// Per-daemon DDL guard. CREATE TABLE IF NOT EXISTS is idempotent at the
// Postgres level, so the latch only suppresses log noise when many bots
// race a DDL batch at boot (mirrors wm_dl_schema_ensured / _lock).
static bool             kr_assetpairs_ensured     = false;
static pthread_mutex_t  kr_assetpairs_ensure_lock = PTHREAD_MUTEX_INITIALIZER;

// Initial SQL builder capacity. KRAKEN_PAIRS_CAP (2048) rows at ~40
// bytes each fits comfortably; the grow-buffer doubles if a larger
// listing arrives.
#define KR_PERSIST_SQL_CAP  16384

// ------------------------------------------------------------------
// Table bootstrap
// ------------------------------------------------------------------

bool
kr_assetpairs_ensure_table(void)
{
  db_result_t *res;
  bool         ok = SUCCESS;

  pthread_mutex_lock(&kr_assetpairs_ensure_lock);

  if(kr_assetpairs_ensured)
  {
    pthread_mutex_unlock(&kr_assetpairs_ensure_lock);
    return(SUCCESS);
  }

  res = db_result_alloc();

  if(res == NULL)
  {
    pthread_mutex_unlock(&kr_assetpairs_ensure_lock);
    return(FAIL);
  }

  if(db_query(kr_assetpairs_ddl, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, KR_CTX, "assetpairs: ensure table failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");
    ok = FAIL;
  }

  else
    kr_assetpairs_ensured = true;

  db_result_free(res);
  pthread_mutex_unlock(&kr_assetpairs_ensure_lock);

  return(ok);
}

// ------------------------------------------------------------------
// Persist (cache → DB)
// ------------------------------------------------------------------

bool
kr_assetpairs_persist(void)
{
  kraken_pair_t *snap;
  db_result_t   *dbres;
  char          *sql;
  size_t         cap = KR_PERSIST_SQL_CAP;
  size_t         len = 0;
  uint32_t       n;
  uint32_t       i;
  int            n_w;
  bool           ok = FAIL;

  snap = mem_alloc(KR_CTX, "persist.snap",
      (size_t)KRAKEN_PAIRS_CAP * sizeof(*snap));

  n = kr_pairs_snapshot(snap, KRAKEN_PAIRS_CAP);

  sql = mem_alloc(KR_CTX, "persist.sql", cap);

  // Single transaction: clear the table, then bulk-insert the snapshot.
  // An empty cache still emits BEGIN; DELETE; COMMIT; to clear stale
  // rows.
  len += (size_t)snprintf(sql + len, cap - len,
      "BEGIN; DELETE FROM kraken_assetpairs;");

  if(n > 0)
    len += (size_t)snprintf(sql + len, cap - len,
        " INSERT INTO kraken_assetpairs (altname, canonical, wsname)"
        " VALUES ");

  for(i = 0; i < n; i++)
  {
    if(cap - len < 128)
    {
      cap *= 2;
      sql  = mem_realloc(sql, cap);
    }

    // altname/canonical/wsname are exchange-issued pair identifiers
    // (uppercase alnum + a single '/' in wsname) — no quote characters
    // are possible, so a direct single-quoted embed is injection-safe.
    n_w = snprintf(sql + len, cap - len, "%s('%s','%s','%s')",
        i > 0 ? "," : "", snap[i].altname, snap[i].canonical,
        snap[i].wsname);

    if(n_w < 0)
    {
      mem_free(sql);
      mem_free(snap);
      return(FAIL);
    }

    len += (size_t)n_w;
  }

  mem_free(snap);

  if(cap - len < 64)
  {
    cap += 64;
    sql  = mem_realloc(sql, cap);
  }

  // Terminate the INSERT (only emitted when rows were appended), then
  // close the transaction. ON CONFLICT DO NOTHING makes the batch
  // robust to a transient duplicate `altname` in the snapshot (e.g. two
  // refresh fan-outs racing on the shared cache) — Postgres skips the
  // dup rather than aborting the whole persist; first occurrence wins.
  len += (size_t)snprintf(sql + len, cap - len, "%s COMMIT;",
      n > 0 ? " ON CONFLICT (altname) DO NOTHING;" : "");

  dbres = db_result_alloc();

  if(dbres == NULL)
  {
    mem_free(sql);
    return(FAIL);
  }

  if(db_query(sql, dbres) == SUCCESS && dbres->ok)
  {
    ok = SUCCESS;
    clam(CLAM_INFO, KR_CTX, "assetpairs: persisted %u rows", n);
  }

  else
    clam(CLAM_WARN, KR_CTX, "assetpairs: persist failed: %s",
        dbres->error[0] != '\0' ? dbres->error : "(no driver error)");

  db_result_free(dbres);
  mem_free(sql);

  return(ok);
}

// ------------------------------------------------------------------
// Prime (DB → cache, refresh-on-stale)
// ------------------------------------------------------------------

// The snapshot SELECT, shared by the async load and the synchronous
// prime. Cols: 0=altname 1=canonical 2=wsname 3=age_sec.
static const char kr_assetpairs_snapshot_sql[] =
  "SELECT altname, canonical, wsname,"
  " EXTRACT(EPOCH FROM (NOW() - fetched_at))::bigint AS age_sec"
  " FROM kraken_assetpairs";

// The oldest row's age, in seconds — the snapshot's age.
static int64_t
kr_assetpairs_snapshot_age(const db_result_t *res)
{
  int64_t  max_age = 0;
  uint32_t i;

  for(i = 0; i < res->rows; i++)
  {
    const char *age_s = db_result_get(res, i, 3);

    if(age_s != NULL)
    {
      int64_t age = (int64_t)strtoll(age_s, NULL, 10);

      if(age > max_age)
        max_age = age;
    }
  }

  return(max_age);
}

// Replace the in-memory cache with the snapshot's rows. Returns how many
// were applied.
static uint32_t
kr_assetpairs_apply(const db_result_t *res)
{
  uint32_t n = 0;
  uint32_t i;

  kr_pairs_clear();

  for(i = 0; i < res->rows; i++)
  {
    const char *altname   = db_result_get(res, i, 0);
    const char *canonical = db_result_get(res, i, 1);
    const char *wsname    = db_result_get(res, i, 2);

    if(altname != NULL && altname[0] != '\0')
    {
      kr_pairs_add(altname, canonical, wsname);
      n++;
    }
  }

  return(n);
}

// Async load result handler. Takes ownership of `res` (db_cb_t
// contract). Repopulates the in-memory cache from a fresh snapshot;
// fires a background network refresh when the snapshot is missing,
// failed, or older than the staleness window.
static void
kr_assetpairs_load_cb(db_result_t *res, void *data)
{
  uint32_t refresh_sec;
  int64_t  max_age;
  uint32_t n;

  (void)data;

  if(res == NULL || !res->ok || res->rows == 0)
  {
    db_result_free(res);
    (void)kraken_assetpairs_refresh_async(
        kr_assetpairs_refresh_persist_cb, NULL);
    return;
  }

  max_age     = kr_assetpairs_snapshot_age(res);
  refresh_sec = (uint32_t)kv_get_uint("plugin.kraken.assetpairs_refresh_sec");

  // refresh_sec == 0 disables the periodic refresh; treat any existing
  // snapshot as fresh regardless of age (only an empty table refreshes).
  if(refresh_sec > 0 && max_age >= (int64_t)refresh_sec)
  {
    db_result_free(res);
    clam(CLAM_INFO, KR_CTX,
        "assetpairs: cache stale (age %llds >= %u s); refreshing",
        (long long)max_age, refresh_sec);
    (void)kraken_assetpairs_refresh_async(
        kr_assetpairs_refresh_persist_cb, NULL);
    return;
  }

  n = kr_assetpairs_apply(res);

  db_result_free(res);

  clam(CLAM_INFO, KR_CTX,
      "assetpairs: loaded %u from cache (age %llds)", n, (long long)max_age);
}

// OBS-23: the pair cache must be populated BEFORE this plugin registers
// with feature_exchange, because registration is what tells a consumer
// its subscriptions are stale — and whenmoon rebuilds them synchronously
// inside that call. kr_ws_subscribe resolves each product to its wsname
// at slot-creation time (kraken_ws_channels.c), so a rebuild that runs
// against an empty cache binds the pass-through symbol (`ETH-USD`) and
// Kraken rejects the subscription outright: "Currency pair not in ISO
// 4217-A3 format". Measured on 2026-08-16 — the feed stayed dead until
// an operator market op re-subscribed against a warm cache.
//
// So this is the second synchronous DB touch on the startup path, and it
// is deliberate: one indexed SELECT of a ~1.4k-row table against the
// pool, no network. Staleness is NOT judged here — a stale name map
// still resolves every pair a fresh one does, and the async load that
// follows re-judges it properly and refreshes from the network when it
// must. An empty or unreachable snapshot leaves the cache as it was and
// the async path handles it, which is exactly today's behaviour.
void
kr_assetpairs_prime_sync(void)
{
  db_result_t *res = db_result_alloc();
  uint32_t     n;

  if(db_query(kr_assetpairs_snapshot_sql, res) != SUCCESS || !res->ok
      || res->rows == 0)
  {
    db_result_free(res);
    clam(CLAM_INFO, KR_CTX,
        "assetpairs: no persisted snapshot to prime; async load will refresh");
    return;
  }

  n = kr_assetpairs_apply(res);

  clam(CLAM_INFO, KR_CTX,
      "assetpairs: primed %u from cache (age %llds) before registration",
      n, (long long)kr_assetpairs_snapshot_age(res));

  db_result_free(res);
}

void
kr_assetpairs_load_or_refresh_async(void)
{
  if(db_query_async(kr_assetpairs_snapshot_sql,
        kr_assetpairs_load_cb, NULL) == ASYNC_FAILED_UNDELIVERED)
  {
    // DB unreachable / submit failed → never leave the cache empty;
    // fall through to a direct network refresh.
    clam(CLAM_WARN, KR_CTX,
        "assetpairs: cache load submit failed; refreshing from network");
    (void)kraken_assetpairs_refresh_async(
        kr_assetpairs_refresh_persist_cb, NULL);
  }
}

void
kr_assetpairs_refresh_persist_cb(const kraken_assetpairs_result_t *res,
    void *user)
{
  (void)user;

  if(res != NULL && res->err[0] == '\0')
  {
    (void)kr_assetpairs_persist();
    return;
  }

  // Refresh failed — leave any prior cache (in-memory and DB) intact.
  clam(CLAM_WARN, KR_CTX, "assetpairs: refresh failed: %s",
      (res != NULL && res->err[0] != '\0') ? res->err : "(no error string)");
}
