// botmanager — MIT
// Temporary MFAs: exact-match identities minted by !identify, lazily expired.

#define USERNS_INTERNAL
#include "userns.h"

#include <string.h>

// DB helpers — all called WITHOUT the cache lock held; postgres is
// remote and a query under a rwlock would stall every resolve.

static void
tmfa_db_upsert(uint32_t user_id, const char *metadata)
{
  char *esc;
  char sql[512];
  db_result_t *r;

  esc = db_escape(metadata);
  if(esc == NULL)
    return;

  snprintf(sql, sizeof(sql),
      "INSERT INTO user_mfa_temp (user_id, metadata) "
      "VALUES (%u, '%s') "
      "ON CONFLICT (user_id, metadata) "
      "DO UPDATE SET last_seen = NOW()",
      user_id, esc);

  mem_free(esc);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
    clam(CLAM_WARN, "userns_tmfa", "upsert failed: %s", r->error);

  db_result_free(r);
}

static void
tmfa_db_delete(uint32_t user_id, const char *metadata)
{
  char *esc;
  char sql[512];
  db_result_t *r;

  esc = db_escape(metadata);
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
tmfa_db_touch(uint32_t user_id, const char *metadata, time_t last_seen)
{
  char *esc;
  char sql[512];
  db_result_t *r;

  esc = db_escape(metadata);
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
      "SELECT u.id, u.username, t.metadata, "
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
      const char *meta = db_result_get(r, i, 2);
      const char *cts  = db_result_get(r, i, 3);
      const char *lts  = db_result_get(r, i, 4);
      tmfa_entry_t *e;

      if(uid == NULL || user == NULL || meta == NULL ||
         cts == NULL || lts == NULL)
        continue;

      e = mem_alloc("userns", "tmfa_entry", sizeof(*e));
      memset(e, 0, sizeof(*e));

      e->user_id = (uint32_t)strtoul(uid, NULL, 10);
      snprintf(e->username, sizeof(e->username), "%s", user);
      snprintf(e->metadata, sizeof(e->metadata), "%s", meta);
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
      tmfa_db_touch(e->user_id, e->metadata, e->last_seen);

    mem_free(e);
    e = next;
  }

  pthread_rwlock_destroy(&c->lock);
  mem_free(c);
}

// Public API

bool
userns_tmfa_add(userns_t *ns, const char *username, const char *metadata)
{
  tmfa_cache_t *c;
  tmfa_entry_t *e;
  tmfa_entry_t *stalest;
  uint32_t user_id;
  uint32_t held;
  time_t now;
  char evict_meta[USERNS_MFA_PATTERN_SZ];
  uint32_t evict_id = 0;

  if(!userns_ready || ns == NULL || username == NULL ||
     metadata == NULL || metadata[0] == '\0')
    return(FAIL);

  if(strlen(metadata) >= USERNS_MFA_PATTERN_SZ)
  {
    clam(CLAM_WARN, "userns_tmfa",
        "metadata too long for '%s' in '%s'", username, ns->name);
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

  // Re-identify from a known hostmask just refreshes the stamp.
  for(e = c->entries; e != NULL; e = e->next)
  {
    if(e->user_id == user_id &&
       strncasecmp(e->metadata, metadata, USERNS_MFA_PATTERN_SZ) == 0)
    {
      e->last_seen = now;
      e->persisted_at = now;
      pthread_rwlock_unlock(&c->lock);

      tmfa_db_upsert(user_id, metadata);
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

    snprintf(evict_meta, sizeof(evict_meta), "%s", stalest->metadata);
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
  snprintf(e->username, sizeof(e->username), "%s", username);
  snprintf(e->metadata, sizeof(e->metadata), "%s", metadata);
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
        ns->name, username, evict_meta);
    tmfa_db_delete(evict_id, evict_meta);
  }

  tmfa_db_upsert(user_id, metadata);

  clam(CLAM_INFO, "userns_tmfa",
      "'%s': temporary MFA minted for '%s' (%s)",
      ns->name, username, metadata);

  return(SUCCESS);
}

tmfa_result_t
userns_tmfa_resolve(userns_t *ns, const char *metadata,
    uint32_t timeout, char *user_out, size_t user_sz)
{
  tmfa_cache_t *c;
  tmfa_entry_t *e;
  tmfa_entry_t **pp;
  time_t now;
  bool touch;
  uint32_t touch_id = 0;
  char touch_meta[USERNS_MFA_PATTERN_SZ];
  time_t touch_seen = 0;

  if(!userns_ready || ns == NULL || metadata == NULL ||
     metadata[0] == '\0' || ns->tmfa_cache == NULL)
    return(TMFA_NONE);

  c = (tmfa_cache_t *)ns->tmfa_cache;
  now = time(NULL);
  touch = false;

  pthread_rwlock_wrlock(&c->lock);

  for(pp = &c->entries; (e = *pp) != NULL; pp = &e->next)
  {
    if(strncasecmp(e->metadata, metadata, USERNS_MFA_PATTERN_SZ) != 0)
      continue;

    if(user_out != NULL && user_sz > 0)
      snprintf(user_out, user_sz, "%s", e->username);

    // Lazy expiry: the entry dies at the moment we notice, and the
    // caller gets EXPIRED exactly once to carry the notice.
    if(timeout != 0 && (now - e->last_seen) > (time_t)timeout)
    {
      char dead_meta[USERNS_MFA_PATTERN_SZ];
      char dead_user[USERNS_USER_SZ];
      uint32_t dead_id = e->user_id;
      long idle = (long)(now - e->last_seen);

      snprintf(dead_meta, sizeof(dead_meta), "%s", e->metadata);
      snprintf(dead_user, sizeof(dead_user), "%s", e->username);

      *pp = e->next;
      c->count--;
      mem_free(e);

      pthread_rwlock_unlock(&c->lock);

      clam(CLAM_INFO, "userns_tmfa",
          "'%s': temporary MFA expired for '%s' (%s) "
          "(idle %ld sec, limit %u sec)",
          ns->name, dead_user, dead_meta, idle, timeout);

      tmfa_db_delete(dead_id, dead_meta);
      return(TMFA_EXPIRED);
    }

    e->last_seen = now;

    // Write-behind: persist the stamp at most once per window so
    // witnessed chatter doesn't become a DB write per line.
    if((now - e->persisted_at) >= USERNS_TMFA_PERSIST_SEC)
    {
      e->persisted_at = now;
      touch = true;
      touch_id = e->user_id;
      snprintf(touch_meta, sizeof(touch_meta), "%s", e->metadata);
      touch_seen = now;
    }

    pthread_rwlock_unlock(&c->lock);

    if(touch)
      tmfa_db_touch(touch_id, touch_meta, touch_seen);

    return(TMFA_OK);
  }

  pthread_rwlock_unlock(&c->lock);
  return(TMFA_NONE);
}

uint32_t
userns_tmfa_del(const userns_t *ns, const char *username,
    const char *metadata)
{
  tmfa_cache_t *c;
  tmfa_entry_t *e;
  tmfa_entry_t **pp;
  uint32_t user_id;
  uint32_t removed = 0;

  // Bounded scratch for post-unlock DB deletes; USERNS_TMFA_MAX is
  // the most one user can hold.
  char dead_meta[USERNS_TMFA_MAX][USERNS_MFA_PATTERN_SZ];

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
       (metadata != NULL &&
        strncasecmp(e->metadata, metadata, USERNS_MFA_PATTERN_SZ) != 0))
    {
      pp = &e->next;
      continue;
    }

    if(removed < USERNS_TMFA_MAX)
      snprintf(dead_meta[removed], USERNS_MFA_PATTERN_SZ, "%s",
          e->metadata);

    *pp = e->next;
    c->count--;
    mem_free(e);
    removed++;
  }

  pthread_rwlock_unlock(&c->lock);

  for(uint32_t i = 0; i < removed && i < USERNS_TMFA_MAX; i++)
    tmfa_db_delete(user_id, dead_meta[i]);

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
    cb(e->username, e->metadata, e->created, e->last_seen, data);

  pthread_rwlock_unlock(&c->lock);
}
