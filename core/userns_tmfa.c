// botmanager — MIT
// Temporary MFAs: the logins minted by !identify, bound to a method key.

#define USERNS_INTERNAL
#include "userns.h"
#include "kv.h"

#include <string.h>

// DB helpers — all called WITHOUT the cache lock held; postgres is
// remote and a query under a rwlock would stall every resolve. The
// `metadata` column predates keys and holds the key.

static void
tmfa_db_upsert(uint32_t user_id, const char *bot, const char *kind,
    const char *key)
{
  char uid[16];
  const char *params[4];
  db_result_t *r;

  snprintf(uid, sizeof(uid), "%u", user_id);
  params[0] = uid;
  params[1] = key;
  params[2] = bot;
  params[3] = kind;

  r = db_result_alloc();

  if(db_query_params(
      "INSERT INTO user_mfa_temp (user_id, metadata, bot_name, method_kind) "
      "VALUES ($1, $2, $3, $4) "
      "ON CONFLICT (user_id, metadata) "
      "DO UPDATE SET last_seen = NOW(), bot_name = EXCLUDED.bot_name, "
      "method_kind = EXCLUDED.method_kind",
      params, 4, r) != SUCCESS)
    clam(CLAM_WARN, "userns_tmfa", "upsert failed: %s", r->error);

  db_result_free(r);
}

static void
tmfa_db_delete(uint32_t user_id, const char *key)
{
  char *esc;
  char sql[512];
  db_result_t *r;

  esc = db_escape(key);
  if(esc == NULL)
    return;

  snprintf(sql, sizeof(sql),
      "DELETE FROM user_mfa_temp WHERE user_id = %u AND metadata = '%s'",
      user_id, esc);

  mem_free(esc);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
    clam(CLAM_WARN, "userns_tmfa", "delete failed: %s", r->error);

  db_result_free(r);
}

static void
tmfa_db_touch(uint32_t user_id, const char *key, time_t last_seen)
{
  char *esc;
  char sql[512];
  db_result_t *r;

  esc = db_escape(key);
  if(esc == NULL)
    return;

  snprintf(sql, sizeof(sql),
      "UPDATE user_mfa_temp SET last_seen = TO_TIMESTAMP(%lld) "
      "WHERE user_id = %u AND metadata = '%s'",
      (long long)last_seen, user_id, esc);

  mem_free(esc);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
    clam(CLAM_WARN, "userns_tmfa", "touch failed: %s", r->error);

  db_result_free(r);
}

// Entry policy — callers may hold the cache lock. The window is read
// live from KV (kv_mutex only, never a callout), so a set kv applies to
// the next line seen rather than the next restart.

static bool
tmfa_idled_out(const char *bot, const char *kind, time_t last_seen,
    time_t now)
{
  uint32_t timeout;

  timeout = (uint32_t)kv_get_bot_method_uint(bot, kind, "identtimeout");

  return(timeout != 0 && now - last_seen > (time_t)timeout);
}

static bool
tmfa_matches(const tmfa_entry_t *e, const char *kind, const char *key)
{
  return(strcasecmp(e->kind, kind) == 0 &&
      strncasecmp(e->key, key, USERNS_MFA_PATTERN_SZ) == 0);
}

// Stamp an entry and say whether the write-behind window has passed,
// in which case the caller owes tmfa_db_touch() for it once the lock
// is down — so it copies user_id and key out before unlocking.
static bool
tmfa_stamp(tmfa_entry_t *e, time_t now)
{
  e->last_seen = now;

  if(now - e->persisted_at < USERNS_TMFA_PERSIST_SEC)
    return(false);

  e->persisted_at = now;
  return(true);
}

// Cache lifecycle

void
userns_tmfa_ensure(userns_t *ns)
{
  tmfa_cache_t *c;
  char sql[512];
  db_result_t *r;

  if(ns == NULL || ns->tmfa_cache != NULL)
    return;

  c = mem_alloc("userns", "tmfa_cache", sizeof(*c));
  memset(c, 0, sizeof(*c));
  pthread_rwlock_init(&c->lock, NULL);

  snprintf(sql, sizeof(sql),
      "SELECT u.id, u.username, t.metadata, t.bot_name, t.method_kind, "
      "EXTRACT(EPOCH FROM t.created)::BIGINT, "
      "EXTRACT(EPOCH FROM t.last_seen)::BIGINT "
      "FROM user_mfa_temp t "
      "JOIN userns_user u ON u.id = t.user_id "
      "WHERE u.ns_id = %u",
      ns->id);

  r = db_result_alloc();

  if(db_query(sql, r) == SUCCESS)
  {
    for(uint32_t i = 0; i < r->rows; i++)
    {
      const char *uid  = db_result_get(r, i, 0);
      const char *user = db_result_get(r, i, 1);
      const char *key  = db_result_get(r, i, 2);
      const char *bot  = db_result_get(r, i, 3);
      const char *kind = db_result_get(r, i, 4);
      const char *cts  = db_result_get(r, i, 5);
      const char *lts  = db_result_get(r, i, 6);
      tmfa_entry_t *e;

      if(uid == NULL || user == NULL || key == NULL || bot == NULL ||
         kind == NULL || cts == NULL || lts == NULL)
        continue;

      e = mem_alloc("userns", "tmfa_entry", sizeof(*e));
      memset(e, 0, sizeof(*e));

      e->user_id = (uint32_t)strtoul(uid, NULL, 10);
      strlcpy(e->username, user, sizeof(e->username));
      strlcpy(e->key, key, sizeof(e->key));
      strlcpy(e->bot, bot, sizeof(e->bot));
      strlcpy(e->kind, kind, sizeof(e->kind));
      e->created      = (time_t)strtoll(cts, NULL, 10);
      e->last_seen    = (time_t)strtoll(lts, NULL, 10);
      e->persisted_at = e->last_seen;

      e->next = c->entries;
      c->entries = e;
      c->count++;
    }
  }

  else
    clam(CLAM_WARN, "userns_tmfa", "load failed for '%s': %s",
        ns->name, r->error);

  db_result_free(r);

  if(c->count > 0)
    clam(CLAM_INFO, "userns_tmfa",
        "'%s': loaded %u temporary MFA(s)", ns->name, c->count);

  ns->tmfa_cache = c;
}

void
userns_tmfa_destroy(userns_t *ns)
{
  tmfa_cache_t *c;
  tmfa_entry_t *e;

  if(ns == NULL || ns->tmfa_cache == NULL)
    return;

  c = (tmfa_cache_t *)ns->tmfa_cache;
  ns->tmfa_cache = NULL;

  // Flush stamps that outran the write-behind window, then free. No
  // lock discipline needed: teardown is single-threaded by contract.
  e = c->entries;

  while(e != NULL)
  {
    tmfa_entry_t *next = e->next;

    if(e->last_seen > e->persisted_at)
      tmfa_db_touch(e->user_id, e->key, e->last_seen);

    mem_free(e);
    e = next;
  }

  pthread_rwlock_destroy(&c->lock);
  mem_free(c);
}

// Public API

bool
userns_tmfa_add(userns_t *ns, const char *username, const char *bot,
    const char *kind, const char *key)
{
  tmfa_cache_t *c;
  tmfa_entry_t *e;
  tmfa_entry_t *stalest;
  uint32_t user_id;
  uint32_t held;
  time_t now;
  char evict_key[USERNS_MFA_PATTERN_SZ];
  uint32_t evict_id = 0;

  if(!userns_ready || ns == NULL || username == NULL || bot == NULL ||
     kind == NULL || key == NULL || key[0] == '\0')
    return(FAIL);

  if(strlen(key) >= USERNS_MFA_PATTERN_SZ ||
     strlen(bot) >= USERNS_TMFA_BOT_SZ ||
     strlen(kind) >= USERNS_TMFA_KIND_SZ)
  {
    clam(CLAM_WARN, "userns_tmfa",
        "key, bot or kind too long for '%s' in '%s'", username, ns->name);
    return(FAIL);
  }

  user_id = userns_get_user_id(ns, username);

  if(user_id == 0)
  {
    clam(CLAM_WARN, "userns_tmfa",
        "user '%s' not found in '%s'", username, ns->name);
    return(FAIL);
  }

  userns_tmfa_ensure(ns);
  c = (tmfa_cache_t *)ns->tmfa_cache;
  now = time(NULL);

  pthread_rwlock_wrlock(&c->lock);

  // Re-identify from a known key refreshes the stamp and takes the
  // newest minting bot, whose window then governs it.
  for(e = c->entries; e != NULL; e = e->next)
  {
    if(e->user_id == user_id && tmfa_matches(e, kind, key))
    {
      strlcpy(e->bot, bot, sizeof(e->bot));
      e->last_seen = now;
      e->persisted_at = now;
      pthread_rwlock_unlock(&c->lock);

      tmfa_db_upsert(user_id, bot, kind, key);
      return(SUCCESS);
    }
  }

  // Enforce the per-user cap: evict the stalest before inserting.
  held = 0;
  stalest = NULL;

  for(e = c->entries; e != NULL; e = e->next)
  {
    if(e->user_id != user_id)
      continue;

    held++;

    if(stalest == NULL || e->last_seen < stalest->last_seen)
      stalest = e;
  }

  if(held >= USERNS_TMFA_MAX && stalest != NULL)
  {
    tmfa_entry_t **pp = &c->entries;

    strlcpy(evict_key, stalest->key, sizeof(evict_key));
    evict_id = stalest->user_id;

    while(*pp != stalest)
      pp = &(*pp)->next;

    *pp = stalest->next;
    c->count--;
    mem_free(stalest);
  }

  e = mem_alloc("userns", "tmfa_entry", sizeof(*e));
  memset(e, 0, sizeof(*e));

  e->user_id = user_id;
  strlcpy(e->username, username, sizeof(e->username));
  strlcpy(e->key, key, sizeof(e->key));
  strlcpy(e->bot, bot, sizeof(e->bot));
  strlcpy(e->kind, kind, sizeof(e->kind));
  e->created      = now;
  e->last_seen    = now;
  e->persisted_at = now;

  e->next = c->entries;
  c->entries = e;
  c->count++;

  pthread_rwlock_unlock(&c->lock);

  if(evict_id != 0)
  {
    clam(CLAM_INFO, "userns_tmfa",
        "'%s': cap reached for '%s', evicted stalest '%s'",
        ns->name, username, evict_key);
    tmfa_db_delete(evict_id, evict_key);
  }

  tmfa_db_upsert(user_id, bot, kind, key);

  clam(CLAM_INFO, "userns_tmfa",
      "'%s': temporary MFA minted for '%s' via %s/%s (%s)",
      ns->name, username, bot, kind, key);

  return(SUCCESS);
}

tmfa_result_t
userns_tmfa_resolve(userns_t *ns, const char *kind, const char *key,
    char *user_out, size_t user_sz)
{
  tmfa_cache_t *c;
  tmfa_entry_t *e;
  tmfa_entry_t **pp;
  time_t now;
  bool touch;
  uint32_t id;
  char stored[USERNS_MFA_PATTERN_SZ];

  if(!userns_ready || ns == NULL || kind == NULL || key == NULL ||
     key[0] == '\0' || ns->tmfa_cache == NULL)
    return(TMFA_NONE);

  c = (tmfa_cache_t *)ns->tmfa_cache;
  now = time(NULL);

  pthread_rwlock_wrlock(&c->lock);

  for(pp = &c->entries; (e = *pp) != NULL; pp = &e->next)
  {
    if(!tmfa_matches(e, kind, key))
      continue;

    if(user_out != NULL && user_sz > 0)
      strlcpy(user_out, e->username, user_sz);

    id = e->user_id;
    strlcpy(stored, e->key, sizeof(stored));

    // Past the window: the entry dies at the moment we notice, and the
    // caller gets EXPIRED exactly once to carry the notice.
    if(tmfa_idled_out(e->bot, e->kind, e->last_seen, now))
    {
      long idle = (long)(now - e->last_seen);
      char dead_user[USERNS_USER_SZ];

      strlcpy(dead_user, e->username, sizeof(dead_user));

      *pp = e->next;
      c->count--;
      mem_free(e);

      pthread_rwlock_unlock(&c->lock);

      clam(CLAM_INFO, "userns_tmfa",
          "'%s': temporary MFA expired for '%s' (%s) (idle %ld sec)",
          ns->name, dead_user, stored, idle);

      tmfa_db_delete(id, stored);
      return(TMFA_EXPIRED);
    }

    touch = tmfa_stamp(e, now);

    pthread_rwlock_unlock(&c->lock);

    if(touch)
      tmfa_db_touch(id, stored, now);

    return(TMFA_OK);
  }

  pthread_rwlock_unlock(&c->lock);
  return(TMFA_NONE);
}

void
userns_tmfa_seen(userns_t *ns, const char *kind, const char *key)
{
  tmfa_cache_t *c;
  time_t now;
  uint32_t n_touch = 0;
  uint32_t touch_id[USERNS_TMFA_MAX];
  char touch_key[USERNS_TMFA_MAX][USERNS_MFA_PATTERN_SZ];

  if(!userns_ready || ns == NULL || kind == NULL || key == NULL ||
     key[0] == '\0' || ns->tmfa_cache == NULL)
    return;

  c = (tmfa_cache_t *)ns->tmfa_cache;
  now = time(NULL);

  pthread_rwlock_wrlock(&c->lock);

  for(tmfa_entry_t *e = c->entries; e != NULL; e = e->next)
  {
    if(!tmfa_matches(e, kind, key) ||
       tmfa_idled_out(e->bot, e->kind, e->last_seen, now))
      continue;

    // Past the scratch the stamp still moves in memory and only the DB
    // write waits: persisted_at stays behind, so a later line writes it.
    if(n_touch >= USERNS_TMFA_MAX)
    {
      e->last_seen = now;
      continue;
    }

    if(tmfa_stamp(e, now))
    {
      touch_id[n_touch] = e->user_id;
      strlcpy(touch_key[n_touch], e->key, USERNS_MFA_PATTERN_SZ);
      n_touch++;
    }
  }

  pthread_rwlock_unlock(&c->lock);

  for(uint32_t i = 0; i < n_touch; i++)
    tmfa_db_touch(touch_id[i], touch_key[i], now);
}

uint32_t
userns_tmfa_drop(userns_t *ns, const char *kind, const char *key)
{
  tmfa_cache_t *c;
  uint32_t removed = 0;

  if(!userns_ready || ns == NULL || kind == NULL || key == NULL ||
     key[0] == '\0' || ns->tmfa_cache == NULL)
    return(0);

  c = (tmfa_cache_t *)ns->tmfa_cache;

  // One entry per lock hold, so each DB delete and log line runs
  // unlocked and no scratch bounds how many one key can end.
  for(;;)
  {
    tmfa_entry_t **pp;
    tmfa_entry_t *e = NULL;
    uint32_t id;
    char user[USERNS_USER_SZ];
    char stored[USERNS_MFA_PATTERN_SZ];

    pthread_rwlock_wrlock(&c->lock);

    for(pp = &c->entries; *pp != NULL; pp = &(*pp)->next)
    {
      if(tmfa_matches(*pp, kind, key))
      {
        e = *pp;
        break;
      }
    }

    if(e == NULL)
    {
      pthread_rwlock_unlock(&c->lock);
      break;
    }

    id = e->user_id;
    strlcpy(user, e->username, sizeof(user));
    strlcpy(stored, e->key, sizeof(stored));

    *pp = e->next;
    c->count--;
    mem_free(e);

    pthread_rwlock_unlock(&c->lock);

    clam(CLAM_INFO, "userns_tmfa",
        "'%s': temporary MFA for '%s' (%s) ended: quit %s",
        ns->name, user, stored, kind);

    tmfa_db_delete(id, stored);
    removed++;
  }

  return(removed);
}

uint32_t
userns_tmfa_del(const userns_t *ns, const char *username, const char *key)
{
  tmfa_cache_t *c;
  tmfa_entry_t *e;
  tmfa_entry_t **pp;
  uint32_t user_id;
  uint32_t removed = 0;

  // Bounded scratch for post-unlock DB deletes; USERNS_TMFA_MAX is
  // the most one user can hold.
  char dead_key[USERNS_TMFA_MAX][USERNS_MFA_PATTERN_SZ];

  if(!userns_ready || ns == NULL || username == NULL ||
     ns->tmfa_cache == NULL)
    return(0);

  user_id = userns_get_user_id(ns, username);

  if(user_id == 0)
    return(0);

  c = (tmfa_cache_t *)ns->tmfa_cache;

  pthread_rwlock_wrlock(&c->lock);

  pp = &c->entries;

  while((e = *pp) != NULL)
  {
    if(e->user_id != user_id ||
       (key != NULL &&
        strncasecmp(e->key, key, USERNS_MFA_PATTERN_SZ) != 0))
    {
      pp = &e->next;
      continue;
    }

    if(removed < USERNS_TMFA_MAX)
      strlcpy(dead_key[removed], e->key, USERNS_MFA_PATTERN_SZ);

    *pp = e->next;
    c->count--;
    mem_free(e);
    removed++;
  }

  pthread_rwlock_unlock(&c->lock);

  for(uint32_t i = 0; i < removed && i < USERNS_TMFA_MAX; i++)
    tmfa_db_delete(user_id, dead_key[i]);

  if(removed > 0)
    clam(CLAM_INFO, "userns_tmfa",
        "'%s': removed %u temporary MFA(s) for '%s'",
        ns->name, removed, username);

  return(removed);
}

void
userns_tmfa_iterate(const userns_t *ns, userns_tmfa_iter_cb_t cb,
    void *data)
{
  tmfa_cache_t *c;

  if(!userns_ready || ns == NULL || cb == NULL || ns->tmfa_cache == NULL)
    return;

  c = (tmfa_cache_t *)ns->tmfa_cache;

  pthread_rwlock_rdlock(&c->lock);

  for(tmfa_entry_t *e = c->entries; e != NULL; e = e->next)
    cb(e->username, e->key, e->created, e->last_seen, data);

  pthread_rwlock_unlock(&c->lock);
}

bool
userns_tmfa_sweep_due(userns_t *ns)
{
  tmfa_cache_t *c;
  int64_t now;
  int64_t last;

  if(ns == NULL || ns->tmfa_cache == NULL)
    return(false);

  c = (tmfa_cache_t *)ns->tmfa_cache;
  now = (int64_t)time(NULL);
  last = __atomic_load_n(&c->last_sweep, __ATOMIC_ACQUIRE);

  if(now - last < USERNS_TMFA_SWEEP_SEC)
    return(false);

  // Many threads see activity at once; the compare-exchange is the test
  // and the write in one step, so exactly one of them wins the claim.
  return(__atomic_compare_exchange_n(&c->last_sweep, &last, now, false,
      __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
}

void
userns_tmfa_sweep(userns_t *ns, userns_tmfa_presence_cb_t presence,
    void *data)
{
  tmfa_cache_t *c;
  tmfa_entry_t *snap;
  uint32_t n = 0;
  uint32_t expired = 0;
  uint32_t refreshed = 0;

  if(!userns_ready || ns == NULL || presence == NULL ||
     ns->tmfa_cache == NULL)
    return;

  c = (tmfa_cache_t *)ns->tmfa_cache;

  // Snapshot, then ask the methods with no lock held: a presence check
  // takes the driver's own locks, and drivers report activity into
  // this cache.
  pthread_rwlock_rdlock(&c->lock);

  if(c->count == 0)
  {
    pthread_rwlock_unlock(&c->lock);
    return;
  }

  snap = mem_alloc("userns", "tmfa_sweep", c->count * sizeof(*snap));

  for(tmfa_entry_t *e = c->entries; e != NULL && n < c->count; e = e->next)
    snap[n++] = *e;

  pthread_rwlock_unlock(&c->lock);

  for(uint32_t i = 0; i < n; i++)
  {
    const tmfa_entry_t *s = &snap[i];
    tmfa_presence_t     p = TMFA_ABSENT;
    tmfa_entry_t      **pp;
    tmfa_entry_t       *e = NULL;
    time_t              now = time(NULL);
    bool                touch = false;
    bool                dead = false;

    // Presence is asked only of entries still inside their window: it
    // refreshes, it never revives.
    if(!tmfa_idled_out(s->bot, s->kind, s->last_seen, now))
    {
      p = presence(s->bot, s->kind, s->key, data);

      if(p == TMFA_ABSENT)
        continue;
    }

    pthread_rwlock_wrlock(&c->lock);

    // Re-read the live entry: a line may have refreshed it, or a QUIT
    // ended it, while the lock was down. The snapshot holds the stored
    // spelling, so the match is exact.
    for(pp = &c->entries; *pp != NULL; pp = &(*pp)->next)
    {
      if((*pp)->user_id == s->user_id && strcmp((*pp)->key, s->key) == 0)
      {
        e = *pp;
        break;
      }
    }

    if(e != NULL)
    {
      if(p == TMFA_ORPHAN ||
         tmfa_idled_out(e->bot, e->kind, e->last_seen, now))
      {
        *pp = e->next;
        c->count--;
        mem_free(e);
        dead = true;
      }

      else if(p == TMFA_PRESENT)
        touch = tmfa_stamp(e, now);
    }

    pthread_rwlock_unlock(&c->lock);

    if(dead)
    {
      clam(CLAM_INFO, "userns_tmfa",
          "'%s': temporary MFA for '%s' (%s) swept: %s",
          ns->name, s->username, s->key,
          p == TMFA_ORPHAN ? "its bot is gone" : "idled out");

      tmfa_db_delete(s->user_id, s->key);
      expired++;
    }

    else if(e != NULL && p == TMFA_PRESENT)
    {
      if(touch)
        tmfa_db_touch(s->user_id, s->key, now);

      refreshed++;
    }
  }

  mem_free(snap);

  clam(CLAM_DEBUG, "userns_tmfa",
      "'%s': swept %u temporary MFA(s): %u kept by presence, %u expired",
      ns->name, n, refreshed, expired);
}
