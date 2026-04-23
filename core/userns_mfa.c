// botmanager — MIT
// Multi-MFA pattern management and glob matching over handle!user@host.

#define USERNS_INTERNAL
#include "userns.h"

#include <fnmatch.h>
#include <string.h>

// Multi-MFA pattern management

// Validate an MFA pattern: must be handle!username@hostname format.
// Security constraints:
// - handle must contain at least 3 non-glob characters
// - hostname must contain at least 6 non-glob characters
// - all-glob patterns are rejected
static bool
validate_mfa_pattern(const char *pattern)
{
  size_t len;
  const char *bang;
  const char *at;
  uint32_t handle_non_glob = 0;
  uint32_t host_non_glob = 0;

  if(pattern == NULL || pattern[0] == '\0')
    return(FAIL);

  len = strlen(pattern);
  if(len >= USERNS_MFA_PATTERN_SZ)
    return(FAIL);

  // Find the '!' separator between handle and username.
  bang = strchr(pattern, '!');
  if(bang == NULL || bang == pattern)
    return(FAIL);

  // Find the '@' separator between username and hostname.
  at = strchr(bang + 1, '@');
  if(at == NULL || at == bang + 1 || at[1] == '\0')
    return(FAIL);

  // Count non-glob characters in handle (before '!').
  for(const char *p = pattern; p < bang; p++)
    if(*p != '*' && *p != '?')
      handle_non_glob++;

  if(handle_non_glob < 3)
    return(FAIL);

  // Count non-glob characters in hostname (after '@').
  for(const char *p = at + 1; *p != '\0'; p++)
    if(*p != '*' && *p != '?')
      host_non_glob++;

  if(host_non_glob < 6)
    return(FAIL);

  return(SUCCESS);
}

// Add an MFA pattern for a user. Pattern must be in handle!user@host
// format and pass security constraints. Limited by core.userns.max_mfa.
bool
userns_user_add_mfa(const userns_t *ns, const char *username,
    const char *pattern)
{
  uint32_t user_id;
  char *esc_pattern;
  char sql[512];
  db_result_t *r;

  if(!userns_ready || ns == NULL || username == NULL || pattern == NULL)
    return(FAIL);

  if(validate_mfa_pattern(pattern) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_user_add_mfa",
        "invalid MFA pattern '%s' for '%s' in '%s'",
        pattern, username, ns->name);
    return(FAIL);
  }

  user_id = userns_get_user_id(ns, username);
  if(user_id == 0)
  {
    clam(CLAM_WARN, "userns_user_add_mfa",
        "user '%s' not found in '%s'", username, ns->name);
    return(FAIL);
  }

  // Check current MFA count against limit.
  {
    char csql[128];
    db_result_t *cr;

    snprintf(csql, sizeof(csql),
        "SELECT COUNT(*) FROM user_mfa WHERE user_id = %u", user_id);

    cr = db_result_alloc();

    if(db_query(csql, cr) == SUCCESS && cr->rows > 0)
    {
      const char *cnt = db_result_get(cr, 0, 0);
      uint32_t count = (cnt != NULL) ? (uint32_t)strtoul(cnt, NULL, 10) : 0;

      if(count >= userns_cfg.max_mfa)
      {
        clam(CLAM_WARN, "userns_user_add_mfa",
            "MFA limit (%u) reached for '%s' in '%s'",
            userns_cfg.max_mfa, username, ns->name);
        db_result_free(cr);
        return(FAIL);
      }
    }

    db_result_free(cr);
  }

  // Insert the pattern.
  esc_pattern = db_escape(pattern);
  if(esc_pattern == NULL)
    return(FAIL);

  snprintf(sql, sizeof(sql),
      "INSERT INTO user_mfa (user_id, pattern) VALUES (%u, '%s')",
      user_id, esc_pattern);

  mem_free(esc_pattern);

  r = db_result_alloc();
  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_user_add_mfa",
        "cannot add MFA pattern for '%s' in '%s': %s",
        username, ns->name, r->error);
    db_result_free(r);
    return(FAIL);
  }

  db_result_free(r);

  clam(CLAM_INFO, "userns_user_add_mfa",
      "added MFA pattern '%s' for '%s' in '%s'",
      pattern, username, ns->name);

  userns_cache_invalidate(ns);
  return(SUCCESS);
}

bool
userns_user_remove_mfa(const userns_t *ns, const char *username,
    const char *pattern)
{
  uint32_t user_id;
  char *esc_pattern;
  char sql[512];
  db_result_t *r;
  bool removed;

  if(!userns_ready || ns == NULL || username == NULL || pattern == NULL)
    return(FAIL);

  user_id = userns_get_user_id(ns, username);
  if(user_id == 0)
  {
    clam(CLAM_WARN, "userns_user_remove_mfa",
        "user '%s' not found in '%s'", username, ns->name);
    return(FAIL);
  }

  esc_pattern = db_escape(pattern);
  if(esc_pattern == NULL)
    return(FAIL);

  snprintf(sql, sizeof(sql),
      "DELETE FROM user_mfa WHERE user_id = %u AND pattern = '%s'",
      user_id, esc_pattern);

  mem_free(esc_pattern);

  r = db_result_alloc();
  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_user_remove_mfa",
        "cannot remove MFA pattern for '%s' in '%s': %s",
        username, ns->name, r->error);
    db_result_free(r);
    return(FAIL);
  }

  removed = (r->rows_affected > 0);
  db_result_free(r);

  if(!removed)
  {
    clam(CLAM_WARN, "userns_user_remove_mfa",
        "MFA pattern '%s' not found for '%s' in '%s'",
        pattern, username, ns->name);
    return(FAIL);
  }

  clam(CLAM_INFO, "userns_user_remove_mfa",
      "removed MFA pattern '%s' for '%s' in '%s'",
      pattern, username, ns->name);

  userns_cache_invalidate(ns);
  return(SUCCESS);
}

void
userns_user_list_mfa(const userns_t *ns, const char *username,
    userns_mfa_iter_cb_t cb, void *data)
{
  uint32_t user_id;
  char sql[128];
  db_result_t *r;

  if(!userns_ready || ns == NULL || username == NULL || cb == NULL)
    return;

  user_id = userns_get_user_id(ns, username);
  if(user_id == 0)
    return;

  snprintf(sql, sizeof(sql),
      "SELECT pattern FROM user_mfa WHERE user_id = %u ORDER BY id",
      user_id);

  r = db_result_alloc();
  if(db_query(sql, r) != SUCCESS)
  {
    db_result_free(r);
    return;
  }

  for(uint32_t i = 0; i < r->rows; i++)
  {
    const char *pat = db_result_get(r, i, 0);

    if(pat != NULL)
      cb(pat, data);
  }

  db_result_free(r);
}

// MFA pattern matching

// Decomposed MFA triple: handle, user, host.
typedef struct
{
  char handle[USERNS_MFA_PATTERN_SZ];
  char user[USERNS_MFA_PATTERN_SZ];
  char host[USERNS_MFA_PATTERN_SZ];
} mfa_parts_t;

static bool
parse_mfa_string(const char *str, mfa_parts_t *out)
{
  const char *bang;
  const char *at;
  size_t handle_len, user_len, host_len;

  bang = strchr(str, '!');
  if(bang == NULL || bang == str)
    return(FAIL);

  at = strchr(bang + 1, '@');
  if(at == NULL || at == bang + 1 || at[1] == '\0')
    return(FAIL);

  handle_len = (size_t)(bang - str);
  user_len   = (size_t)(at - (bang + 1));
  host_len   = strlen(at + 1);

  if(handle_len >= USERNS_MFA_PATTERN_SZ ||
     user_len   >= USERNS_MFA_PATTERN_SZ ||
     host_len   >= USERNS_MFA_PATTERN_SZ)
    return(FAIL);

  memcpy(out->handle, str, handle_len);
  out->handle[handle_len] = '\0';

  memcpy(out->user, bang + 1, user_len);
  out->user[user_len] = '\0';

  memcpy(out->host, at + 1, host_len);
  out->host[host_len] = '\0';

  return(SUCCESS);
}

static bool
match_entry(const mfa_cache_entry_t *e, const mfa_parts_t *in)
{
  mfa_parts_t pat;

  if(parse_mfa_string(e->pattern, &pat) != SUCCESS)
    return(FAIL);

  // All three components use case-insensitive glob matching.
  if(fnmatch(pat.handle, in->handle, FNM_CASEFOLD) != 0)
    return(FAIL);

  if(fnmatch(pat.user, in->user, FNM_CASEFOLD) != 0)
    return(FAIL);

  if(fnmatch(pat.host, in->host, FNM_CASEFOLD) != 0)
    return(FAIL);

  return(SUCCESS);
}

// Match an MFA string against all cached patterns in a namespace.
// Input format is handle!username@hostname. First match wins.
// returns: matching username (static buffer, valid until next call), or NULL
const char *
userns_mfa_match(const userns_t *ns, const char *mfa_string)
{
  userns_cache_t *c;
  mfa_parts_t in;
  static char matched_user[USERNS_USER_SZ];

  if(!userns_ready || ns == NULL || mfa_string == NULL || mfa_string[0] == '\0')
    return(NULL);

  c = (userns_cache_t *)ns->mfa_cache;
  if(c == NULL)
    return(NULL);

  if(parse_mfa_string(mfa_string, &in) != SUCCESS)
    return(NULL);

  // Scan the cache under read lock.
  pthread_rwlock_rdlock(&c->lock);

  for(uint32_t i = 0; i < MFA_CACHE_BUCKETS; i++)
  {
    for(mfa_cache_entry_t *e = c->buckets[i]; e != NULL; e = e->next)
    {
      if(match_entry(e, &in) != SUCCESS)
        continue;

      // Match found.
      strncpy(matched_user, e->username, USERNS_USER_SZ - 1);
      matched_user[USERNS_USER_SZ - 1] = '\0';

      pthread_rwlock_unlock(&c->lock);
      __atomic_add_fetch(&userns_stat_mfa_matches, 1, __ATOMIC_RELAXED);
      return(matched_user);
    }
  }

  pthread_rwlock_unlock(&c->lock);
  return(NULL);
}

bool
userns_user_mfa_match(const userns_t *ns, const char *username,
    const char *mfa_string)
{
  userns_cache_t *c;
  mfa_parts_t in;
  bool found = false;

  if(!userns_ready || ns == NULL || username == NULL ||
     mfa_string == NULL || mfa_string[0] == '\0')
    return(false);

  c = (userns_cache_t *)ns->mfa_cache;
  if(c == NULL)
    return(false);

  if(parse_mfa_string(mfa_string, &in) != SUCCESS)
    return(false);

  pthread_rwlock_rdlock(&c->lock);

  for(uint32_t i = 0; i < MFA_CACHE_BUCKETS && !found; i++)
  {
    for(mfa_cache_entry_t *e = c->buckets[i]; e != NULL; e = e->next)
    {
      if(strcasecmp(e->username, username) != 0)
        continue;

      if(match_entry(e, &in) == SUCCESS)
      {
        found = true;
        break;
      }
    }
  }

  pthread_rwlock_unlock(&c->lock);
  return(found);
}

bool
userns_user_has_mfa(const userns_t *ns, const char *username)
{
  uint32_t user_id;
  char sql[128];
  db_result_t *r;
  const char *cnt;
  uint32_t count;

  if(!userns_ready || ns == NULL || username == NULL || username[0] == '\0')
    return(false);

  user_id = userns_get_user_id(ns, username);
  if(user_id == 0)
    return(false);

  snprintf(sql, sizeof(sql),
      "SELECT COUNT(*) FROM user_mfa WHERE user_id = %u", user_id);

  r = db_result_alloc();
  if(db_query(sql, r) != SUCCESS || r->rows == 0)
  {
    db_result_free(r);
    return(false);
  }

  cnt = db_result_get(r, 0, 0);
  count = (cnt != NULL) ? (uint32_t)strtoul(cnt, NULL, 10) : 0;

  db_result_free(r);
  return(count > 0);
}
