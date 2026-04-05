// userns_mfa.c — Multi-MFA pattern management and matching.
//
// Handles adding, removing, and listing MFA patterns for users,
// as well as matching incoming MFA strings (handle!user@host)
// against cached patterns using glob matching.

#define USERNS_INTERNAL
#include "userns.h"

#include <ctype.h>
#include <string.h>

// -----------------------------------------------------------------------
// Multi-MFA pattern management
// -----------------------------------------------------------------------

// Validate an MFA pattern: must be handle!username@hostname format.
// Security constraints:
// - handle must contain at least 3 non-glob characters
// - hostname must contain at least 6 non-glob characters
// - all-glob patterns are rejected
// returns: SUCCESS or FAIL
// pattern: candidate MFA pattern
static bool
validate_mfa_pattern(const char *pattern)
{
  if(pattern == NULL || pattern[0] == '\0')
    return(FAIL);

  size_t len = strlen(pattern);

  if(len >= USERNS_MFA_PATTERN_SZ)
    return(FAIL);

  // Find the '!' separator between handle and username.
  const char *bang = strchr(pattern, '!');

  if(bang == NULL || bang == pattern)
    return(FAIL);

  // Find the '@' separator between username and hostname.
  const char *at = strchr(bang + 1, '@');

  if(at == NULL || at == bang + 1 || at[1] == '\0')
    return(FAIL);

  // Count non-glob characters in handle (before '!').
  uint32_t handle_non_glob = 0;

  for(const char *p = pattern; p < bang; p++)
  {
    if(*p != '*' && *p != '?')
      handle_non_glob++;
  }

  if(handle_non_glob < 3)
    return(FAIL);

  // Count non-glob characters in hostname (after '@').
  uint32_t host_non_glob = 0;

  for(const char *p = at + 1; *p != '\0'; p++)
  {
    if(*p != '*' && *p != '?')
      host_non_glob++;
  }

  if(host_non_glob < 6)
    return(FAIL);

  return(SUCCESS);
}

// Add an MFA pattern for a user. Pattern must be in handle!user@host
// format and pass security constraints. Limited by core.userns.max_mfa.
// returns: SUCCESS or FAIL
// ns: target namespace
// username: user to add the pattern to
// pattern: MFA pattern string (max USERNS_MFA_PATTERN_SZ-1 chars)
bool
userns_user_add_mfa(const userns_t *ns, const char *username,
    const char *pattern)
{
  if(!userns_ready || ns == NULL || username == NULL || pattern == NULL)
    return(FAIL);

  if(validate_mfa_pattern(pattern) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_user_add_mfa",
        "invalid MFA pattern '%s' for '%s' in '%s'",
        pattern, username, ns->name);
    return(FAIL);
  }

  uint32_t user_id = userns_get_user_id(ns, username);

  if(user_id == 0)
  {
    clam(CLAM_WARN, "userns_user_add_mfa",
        "user '%s' not found in '%s'", username, ns->name);
    return(FAIL);
  }

  // Check current MFA count against limit.
  {
    char sql[128];

    snprintf(sql, sizeof(sql),
        "SELECT COUNT(*) FROM user_mfa WHERE user_id = %u", user_id);

    db_result_t *r = db_result_alloc();

    if(db_query(sql, r) == SUCCESS && r->rows > 0)
    {
      const char *cnt = db_result_get(r, 0, 0);
      uint32_t count = (cnt != NULL) ? (uint32_t)strtoul(cnt, NULL, 10) : 0;

      if(count >= userns_cfg.max_mfa)
      {
        clam(CLAM_WARN, "userns_user_add_mfa",
            "MFA limit (%u) reached for '%s' in '%s'",
            userns_cfg.max_mfa, username, ns->name);
        db_result_free(r);
        return(FAIL);
      }
    }

    db_result_free(r);
  }

  // Insert the pattern.
  char *esc_pattern = db_escape(pattern);

  if(esc_pattern == NULL)
    return(FAIL);

  char sql[512];

  snprintf(sql, sizeof(sql),
      "INSERT INTO user_mfa (user_id, pattern) VALUES (%u, '%s')",
      user_id, esc_pattern);

  mem_free(esc_pattern);

  db_result_t *r = db_result_alloc();

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

// Remove an MFA pattern from a user.
// returns: SUCCESS or FAIL (pattern not found)
// ns: target namespace
// username: user to remove the pattern from
// pattern: exact MFA pattern string to remove
bool
userns_user_remove_mfa(const userns_t *ns, const char *username,
    const char *pattern)
{
  if(!userns_ready || ns == NULL || username == NULL || pattern == NULL)
    return(FAIL);

  uint32_t user_id = userns_get_user_id(ns, username);

  if(user_id == 0)
  {
    clam(CLAM_WARN, "userns_user_remove_mfa",
        "user '%s' not found in '%s'", username, ns->name);
    return(FAIL);
  }

  char *esc_pattern = db_escape(pattern);

  if(esc_pattern == NULL)
    return(FAIL);

  char sql[512];

  snprintf(sql, sizeof(sql),
      "DELETE FROM user_mfa WHERE user_id = %u AND pattern = '%s'",
      user_id, esc_pattern);

  mem_free(esc_pattern);

  db_result_t *r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_user_remove_mfa",
        "cannot remove MFA pattern for '%s' in '%s': %s",
        username, ns->name, r->error);
    db_result_free(r);
    return(FAIL);
  }

  bool removed = (r->rows_affected > 0);

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

// Iterate all MFA patterns for a user, invoking cb for each.
// ns: target namespace
// username: user whose patterns to list
// cb: callback invoked with each pattern string
// data: opaque user data passed to cb
void
userns_user_list_mfa(const userns_t *ns, const char *username,
    userns_mfa_iter_cb_t cb, void *data)
{
  if(!userns_ready || ns == NULL || username == NULL || cb == NULL)
    return;

  uint32_t user_id = userns_get_user_id(ns, username);

  if(user_id == 0)
    return;

  char sql[128];

  snprintf(sql, sizeof(sql),
      "SELECT pattern FROM user_mfa WHERE user_id = %u ORDER BY id",
      user_id);

  db_result_t *r = db_result_alloc();

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

// -----------------------------------------------------------------------
// Glob matching
// -----------------------------------------------------------------------

// Simple glob matcher. Supports '*' (zero or more characters) and
// '?' (exactly one character). Case-insensitive.
// returns: SUCCESS if string matches pattern, FAIL otherwise
static bool
glob_match(const char *pattern, const char *string)
{
  const char *p = pattern;
  const char *s = string;
  const char *star_p = NULL;   // position after last '*' in pattern
  const char *star_s = NULL;   // position in string when '*' was matched

  while(*s != '\0')
  {
    if(*p == '?' ||
       (*p != '*' && tolower((unsigned char)*p) == tolower((unsigned char)*s)))
    {
      p++;
      s++;
    }
    else if(*p == '*')
    {
      star_p = ++p;
      star_s = s;
    }
    else if(star_p != NULL)
    {
      // Backtrack: advance the star match by one character.
      p = star_p;
      s = ++star_s;
    }
    else
    {
      return(FAIL);
    }
  }

  // Consume trailing '*' in pattern.
  while(*p == '*')
    p++;

  return(*p == '\0' ? SUCCESS : FAIL);
}

// -----------------------------------------------------------------------
// MFA pattern matching
// -----------------------------------------------------------------------

// Decomposed MFA triple: handle, user, host.
typedef struct
{
  char handle[USERNS_MFA_PATTERN_SZ];
  char user[USERNS_MFA_PATTERN_SZ];
  char host[USERNS_MFA_PATTERN_SZ];
} mfa_parts_t;

// Parse a handle!user@host string into its three components.
// returns: SUCCESS or FAIL
// str: input string
// out: destination parts structure
static bool
parse_mfa_string(const char *str, mfa_parts_t *out)
{
  const char *bang = strchr(str, '!');

  if(bang == NULL || bang == str)
    return(FAIL);

  const char *at = strchr(bang + 1, '@');

  if(at == NULL || at == bang + 1 || at[1] == '\0')
    return(FAIL);

  size_t handle_len = (size_t)(bang - str);
  size_t user_len   = (size_t)(at - (bang + 1));
  size_t host_len   = strlen(at + 1);

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

// Test whether a cached MFA entry matches the given input parts.
// returns: SUCCESS on match, FAIL otherwise
// e: cache entry to test
// in: parsed input MFA triple
static bool
match_entry(const mfa_cache_entry_t *e, const mfa_parts_t *in)
{
  mfa_parts_t pat;

  if(parse_mfa_string(e->pattern, &pat) != SUCCESS)
    return(FAIL);

  // Username portion must match exactly (case-insensitive).
  if(strcasecmp(pat.user, in->user) != 0)
    return(FAIL);

  // Handle and hostname use glob matching.
  if(glob_match(pat.handle, in->handle) != SUCCESS)
    return(FAIL);

  if(glob_match(pat.host, in->host) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

// Match an MFA string against all cached patterns in a namespace.
// Input format is handle!username@hostname. First match wins.
// returns: matching username (static buffer, valid until next call), or NULL
// ns: target namespace
// mfa_string: raw MFA string to match
const char *
userns_mfa_match(const userns_t *ns, const char *mfa_string)
{
  if(!userns_ready || ns == NULL || mfa_string == NULL || mfa_string[0] == '\0')
    return(NULL);

  userns_cache_t *c = (userns_cache_t *)ns->mfa_cache;

  if(c == NULL)
    return(NULL);

  mfa_parts_t in;

  if(parse_mfa_string(mfa_string, &in) != SUCCESS)
    return(NULL);

  // Scan the cache under read lock.
  static char matched_user[USERNS_USER_SZ];

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
      return(matched_user);
    }
  }

  pthread_rwlock_unlock(&c->lock);
  return(NULL);
}
