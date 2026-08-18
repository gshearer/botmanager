// botmanager — MIT
// Gemini symbol-cache persistence (EXCH-PRIME-1).
//
// Mirrors the in-memory gemini_pairs cache to the `gemini_symbols`
// Postgres table and primes it back on startup. This replaces the
// synchronous network prime that gem_start used to run inside the
// plugin start() path — the operator control socket no longer waits on
// Gemini's N+1 /v1/symbols fan-out. The network refresh fires only when
// the persisted snapshot is missing or older than the staleness window
// (plugin.gemini.symbols_refresh_sec).
#define GEM_INTERNAL
#include "gemini.h"

#include "gemini_persist.h"

#include "db.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------
// Schema
// ------------------------------------------------------------------

// Canonical DDL lives in scripts/schema.sql; this mirror keeps the
// runtime self-bootstrapping (CREATE TABLE IF NOT EXISTS). `abstr` is
// not stored — gem_pairs_add re-derives it from base/quote on load.
static const char gem_symbols_ddl[] =
  "CREATE TABLE IF NOT EXISTS gemini_symbols ("
  " native     VARCHAR(16)  PRIMARY KEY,"
  " base       VARCHAR(8)   NOT NULL,"
  " quote      VARCHAR(8)   NOT NULL,"
  " fetched_at TIMESTAMPTZ  NOT NULL DEFAULT NOW()"
  ")";

// Per-daemon DDL guard. CREATE TABLE IF NOT EXISTS is idempotent at the
// Postgres level, so the latch only suppresses log noise when many bots
// race a DDL batch at boot (mirrors wm_dl_schema_ensured / _lock).
static bool             gem_symbols_ensured     = false;
static pthread_mutex_t  gem_symbols_ensure_lock = PTHREAD_MUTEX_INITIALIZER;

// Initial SQL builder capacity. GEM_SYMS_CAP (1024) rows at ~30 bytes
// each fits comfortably; the grow-buffer doubles if a larger listing
// arrives.
#define GEM_PERSIST_SQL_CAP  8192

// ------------------------------------------------------------------
// Table bootstrap
// ------------------------------------------------------------------

bool
gem_symbols_ensure_table(void)
{
  db_result_t *res;
  bool         ok = SUCCESS;

  pthread_mutex_lock(&gem_symbols_ensure_lock);

  if(gem_symbols_ensured)
  {
    pthread_mutex_unlock(&gem_symbols_ensure_lock);
    return(SUCCESS);
  }

  res = db_result_alloc();

  if(db_query(gem_symbols_ddl, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, GEM_CTX, "symbols: ensure table failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");
    ok = FAIL;
  }

  else
    gem_symbols_ensured = true;

  db_result_free(res);
  pthread_mutex_unlock(&gem_symbols_ensure_lock);

  return(ok);
}

// ------------------------------------------------------------------
// Persist (cache → DB)
// ------------------------------------------------------------------

bool
gem_symbols_persist(void)
{
  gemini_pair_t *snap;
  db_result_t   *dbres;
  char          *sql;
  size_t         cap = GEM_PERSIST_SQL_CAP;
  size_t         len = 0;
  uint32_t       n;
  uint32_t       i;
  int            n_w;
  bool           ok = FAIL;

  snap = mem_alloc(GEM_CTX, "persist.snap",
      (size_t)GEM_SYMS_CAP * sizeof(*snap));

  n = gem_pairs_snapshot(snap, GEM_SYMS_CAP);

  sql = mem_alloc(GEM_CTX, "persist.sql", cap);

  // Single transaction: clear the table, then bulk-insert the snapshot.
  // An empty cache still emits BEGIN; DELETE; COMMIT; to clear stale
  // rows.
  len += (size_t)snprintf(sql + len, cap - len,
      "BEGIN; DELETE FROM gemini_symbols;");

  if(n > 0)
    len += (size_t)snprintf(sql + len, cap - len,
        " INSERT INTO gemini_symbols (native, base, quote) VALUES ");

  for(i = 0; i < n; i++)
  {
    if(cap - len < 128)
    {
      cap *= 2;
      sql  = mem_realloc(sql, cap);
    }

    // native/base/quote are constrained tokens — lowercase alnum and
    // uppercase ISO codes written by gem_pairs_add — so no quote
    // characters are possible and a direct single-quoted embed is
    // injection-safe.
    n_w = snprintf(sql + len, cap - len, "%s('%s','%s','%s')",
        i > 0 ? "," : "", snap[i].native, snap[i].base, snap[i].quote);

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
  // robust to a transient duplicate `native` in the snapshot (e.g. two
  // refresh fan-outs racing on the shared cache) — Postgres skips the
  // dup rather than aborting the whole persist; first occurrence wins.
  len += (size_t)snprintf(sql + len, cap - len, "%s COMMIT;",
      n > 0 ? " ON CONFLICT (native) DO NOTHING;" : "");

  dbres = db_result_alloc();

  if(db_query(sql, dbres) == SUCCESS && dbres->ok)
  {
    ok = SUCCESS;
    clam(CLAM_INFO, GEM_CTX, "symbols: persisted %u rows", n);
  }

  else
    clam(CLAM_WARN, GEM_CTX, "symbols: persist failed: %s",
        dbres->error[0] != '\0' ? dbres->error : "(no driver error)");

  db_result_free(dbres);
  mem_free(sql);

  return(ok);
}

// ------------------------------------------------------------------
// Prime (DB → cache, refresh-on-stale)
// ------------------------------------------------------------------

// The snapshot SELECT, shared by the async load and the synchronous
// prime. Cols: 0=native 1=base 2=quote 3=age_sec.
static const char gem_symbols_snapshot_sql[] =
  "SELECT native, base, quote,"
  " EXTRACT(EPOCH FROM (NOW() - fetched_at))::bigint AS age_sec"
  " FROM gemini_symbols";

// The oldest row's age, in seconds — the snapshot's age.
static int64_t
gem_symbols_snapshot_age(const db_result_t *res)
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
// were applied. Borrows `res` — freeing it belongs to the caller, whose
// two call sites hold it under different ownership conventions (the
// async cb is handed `res` by db_cb_t; the sync prime allocates it).
static uint32_t
gem_symbols_apply(const db_result_t *res)
{
  uint32_t n = 0;
  uint32_t i;

  gem_pairs_clear();

  for(i = 0; i < res->rows; i++)
  {
    const char *native = db_result_get(res, i, 0);
    const char *base   = db_result_get(res, i, 1);
    const char *quote  = db_result_get(res, i, 2);

    if(native != NULL && base != NULL && quote != NULL
        && gem_pairs_add(native, base, quote) == SUCCESS)
      n++;
  }

  return(n);
}

// Async load result handler. Takes ownership of `res` (db_cb_t
// contract). Repopulates the in-memory cache from the snapshot; fires a
// background network refresh when the snapshot is missing, failed, or
// older than the staleness window.
static void
gem_symbols_load_cb(db_result_t *res, void *data)
{
  uint32_t refresh_sec;
  int64_t  max_age;
  uint32_t n;

  (void)data;

  if(res == NULL || !res->ok || res->rows == 0)
  {
    db_result_free(res);
    (void)gemini_symbols_refresh_async(gem_symbols_refresh_persist_cb, NULL);
    return;
  }

  max_age     = gem_symbols_snapshot_age(res);
  refresh_sec = (uint32_t)kv_get_uint("plugin.gemini.symbols_refresh_sec");
  n           = gem_symbols_apply(res);

  db_result_free(res);

  // OBS-47: a stale snapshot is applied and THEN refreshed. It is the
  // same data the fan-out will mostly return, and the refresh overwrites
  // it when it lands — whereas declining to apply leaves the cache empty
  // for the whole duration of an N+1 fan-out over ~347 symbols, and
  // every translation in that window takes the pass-through arm. Only
  // the missing/failed arm above cannot apply.
  //
  // refresh_sec == 0 disables the periodic refresh; treat any existing
  // snapshot as fresh regardless of age (only an empty table refreshes).
  if(refresh_sec > 0 && max_age >= (int64_t)refresh_sec)
  {
    clam(CLAM_INFO, GEM_CTX,
        "symbols: loaded %u from cache but stale (age %llds >= %u s); "
        "refreshing", n, (long long)max_age, refresh_sec);
    (void)gemini_symbols_refresh_async(gem_symbols_refresh_persist_cb, NULL);
    return;
  }

  clam(CLAM_INFO, GEM_CTX,
      "symbols: loaded %u from cache (age %llds)", n, (long long)max_age);
}

// OBS-47: the symbol cache must be populated BEFORE this plugin
// registers with feature_exchange. Registration fires feature_exchange's
// registration watch, and a consumer that rebuilds its subscriptions
// inside that call resolves each product against the cache at
// slot-creation time — against an empty one it binds the pass-through
// symbol instead of Gemini's native name. gem_symbols_load_or_refresh_async
// cannot serve that: it is deliberately async so no network I/O blocks
// start().
//
// So this is the second synchronous DB touch on the startup path, and it
// is deliberate: one SELECT of a ~350-row table against the pool, no
// network. Staleness is NOT judged here — a stale name map still
// resolves every pair a fresh one does, and the async load that follows
// re-judges it properly and refreshes when it must. An empty or
// unreachable snapshot leaves the cache as it was (gem_symbols_apply,
// which clears, is not reached) and the async path handles it.
void
gem_symbols_prime_sync(void)
{
  db_result_t *res = db_result_alloc();
  uint32_t     n;

  if(db_query(gem_symbols_snapshot_sql, res) != SUCCESS || !res->ok
      || res->rows == 0)
  {
    db_result_free(res);
    clam(CLAM_INFO, GEM_CTX,
        "symbols: no persisted snapshot to prime; async load will refresh");
    return;
  }

  n = gem_symbols_apply(res);

  clam(CLAM_INFO, GEM_CTX,
      "symbols: primed %u from cache (age %llds) before registration",
      n, (long long)gem_symbols_snapshot_age(res));

  db_result_free(res);
}

void
gem_symbols_load_or_refresh_async(void)
{
  if(db_query_async(gem_symbols_snapshot_sql,
        gem_symbols_load_cb, NULL) == ASYNC_FAILED_UNDELIVERED)
  {
    // DB unreachable / submit failed → never leave the cache empty;
    // fall through to a direct network refresh.
    clam(CLAM_WARN, GEM_CTX,
        "symbols: cache load submit failed; refreshing from network");
    (void)gemini_symbols_refresh_async(gem_symbols_refresh_persist_cb, NULL);
  }
}

void
gem_symbols_refresh_persist_cb(const gemini_symbols_result_t *res, void *user)
{
  (void)user;

  if(res != NULL && res->err[0] == '\0')
  {
    (void)gem_symbols_persist();
    return;
  }

  // Refresh failed — leave any prior cache (in-memory and DB) intact.
  clam(CLAM_WARN, GEM_CTX, "symbols: refresh failed: %s",
      (res != NULL && res->err[0] != '\0') ? res->err : "(no error string)");
}
