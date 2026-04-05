// userns_cache.c — In-memory MFA cache management for user namespaces.
//
// Provides a per-namespace hash table that maps MFA pattern strings
// to user records (username, UUID) for fast lookups without DB
// round-trips.  Populated from the DB on first access and invalidated
// on any MFA mutation.

#define USERNS_INTERNAL
#include "userns.h"
#include "util.h"

#include <string.h>

// -----------------------------------------------------------------------
// MFA cache management
// -----------------------------------------------------------------------

// Allocate and initialize a cache for a namespace.
// returns: new cache pointer
userns_cache_t *
userns_cache_create(void)
{
  userns_cache_t *c = mem_alloc("userns", "mfa_cache", sizeof(*c));
  memset(c, 0, sizeof(*c));
  pthread_rwlock_init(&c->lock, NULL);
  return(c);
}

// Free all entries and the cache itself.
// c: cache to destroy (NULL is a no-op)
void
userns_cache_destroy(userns_cache_t *c)
{
  if(c == NULL)
    return;

  pthread_rwlock_wrlock(&c->lock);

  for(uint32_t i = 0; i < MFA_CACHE_BUCKETS; i++)
  {
    mfa_cache_entry_t *e = c->buckets[i];

    while(e != NULL)
    {
      mfa_cache_entry_t *next = e->next;
      mem_free(e);
      e = next;
    }

    c->buckets[i] = NULL;
  }

  c->count = 0;

  pthread_rwlock_unlock(&c->lock);
  pthread_rwlock_destroy(&c->lock);
  mem_free(c);
}

// Clear all entries without destroying the cache.
// c: cache to clear (NULL is a no-op)
void
userns_cache_clear(userns_cache_t *c)
{
  if(c == NULL)
    return;

  for(uint32_t i = 0; i < MFA_CACHE_BUCKETS; i++)
  {
    mfa_cache_entry_t *e = c->buckets[i];

    while(e != NULL)
    {
      mfa_cache_entry_t *next = e->next;
      mem_free(e);
      e = next;
    }

    c->buckets[i] = NULL;
  }

  c->count = 0;
}

// Populate the cache from the database. Caller must hold write lock.
// c: cache to populate
// ns_id: namespace DB primary key
void
userns_cache_populate(userns_cache_t *c, uint32_t ns_id)
{
  userns_cache_clear(c);

  char sql[256];

  snprintf(sql, sizeof(sql),
      "SELECT u.uuid, u.username, m.pattern "
      "FROM userns_user u "
      "JOIN user_mfa m ON u.id = m.user_id "
      "WHERE u.ns_id = %u ORDER BY u.id, m.id",
      ns_id);

  db_result_t *r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS || r->rows == 0)
  {
    db_result_free(r);
    return;
  }

  for(uint32_t i = 0; i < r->rows; i++)
  {
    const char *uuid     = db_result_get(r, i, 0);
    const char *username = db_result_get(r, i, 1);
    const char *pattern  = db_result_get(r, i, 2);

    if(uuid == NULL || username == NULL || pattern == NULL)
      continue;

    mfa_cache_entry_t *e = mem_alloc("userns", "mfa_cache_entry", sizeof(*e));

    strncpy(e->pattern, pattern, USERNS_MFA_PATTERN_SZ - 1);
    e->pattern[USERNS_MFA_PATTERN_SZ - 1] = '\0';

    strncpy(e->username, username, USERNS_USER_SZ - 1);
    e->username[USERNS_USER_SZ - 1] = '\0';

    strncpy(e->uuid, uuid, USERNS_UUID_SZ - 1);
    e->uuid[USERNS_UUID_SZ - 1] = '\0';

    uint32_t bucket = util_fnv1a_ci(pattern) % MFA_CACHE_BUCKETS;
    e->next = c->buckets[bucket];
    c->buckets[bucket] = e;
    c->count++;
  }

  db_result_free(r);
}

// Invalidate (repopulate) a namespace's cache.
// ns: namespace whose cache to invalidate (NULL is a no-op)
void
userns_cache_invalidate(const userns_t *ns)
{
  if(ns == NULL || ns->mfa_cache == NULL)
    return;

  userns_cache_t *c = (userns_cache_t *)ns->mfa_cache;

  pthread_rwlock_wrlock(&c->lock);
  userns_cache_populate(c, ns->id);
  pthread_rwlock_unlock(&c->lock);
}

// Ensure a namespace has a cache, creating and populating if needed.
// ns: namespace to ensure cache for
void
userns_cache_ensure(userns_t *ns)
{
  if(ns->mfa_cache != NULL)
    return;

  userns_cache_t *c = userns_cache_create();

  pthread_rwlock_wrlock(&c->lock);
  userns_cache_populate(c, ns->id);
  pthread_rwlock_unlock(&c->lock);

  ns->mfa_cache = c;
}
