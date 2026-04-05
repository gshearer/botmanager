// userns_util.c — User profile field setters, iteration, and utilities.
//
// Self-contained user namespace operations that depend only on the
// public DB API and shared internal state (userns_ready).

#define USERNS_INTERNAL
#include "userns.h"

#include <string.h>

// -----------------------------------------------------------------------
// User profile field setters
// -----------------------------------------------------------------------

// Internal helper: update a single VARCHAR column on userns_user.
// returns: SUCCESS or FAIL
// ns: target namespace
// username: user to update
// column: column name (trusted, not escaped)
// value: new value (will be escaped)
// max_len: maximum allowed string length
static bool
user_set_field(const userns_t *ns, const char *username,
    const char *column, const char *value, size_t max_len)
{
  if(!userns_ready || ns == NULL || username == NULL || value == NULL)
    return(FAIL);

  if(strlen(value) > max_len)
  {
    clam(CLAM_WARN, "userns", "value too long for %s (max %zu)",
        column, max_len);
    return(FAIL);
  }

  char *esc_user = db_escape(username);
  char *esc_val = db_escape(value);

  if(esc_user == NULL || esc_val == NULL)
  {
    if(esc_user != NULL) mem_free(esc_user);
    if(esc_val != NULL) mem_free(esc_val);
    return(FAIL);
  }

  char sql[512];

  snprintf(sql, sizeof(sql),
      "UPDATE userns_user SET %s = '%s' "
      "WHERE ns_id = %u AND username = '%s'",
      column, esc_val, ns->id, esc_user);

  mem_free(esc_user);
  mem_free(esc_val);

  db_result_t *r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns", "cannot update %s for '%s': %s",
        column, username, r->error);
    db_result_free(r);
    return(FAIL);
  }

  bool updated = (r->rows_affected > 0);

  db_result_free(r);

  return(updated ? SUCCESS : FAIL);
}

// Set a user's description field.
// returns: SUCCESS or FAIL
// ns: target namespace
// username: user to update
// description: new description (max USERNS_DESC_SZ-1 chars)
bool
userns_user_set_description(const userns_t *ns, const char *username,
    const char *description)
{
  return(user_set_field(ns, username, "description",
      description, USERNS_DESC_SZ - 1));
}

// Set a user's legacy MFA field (single value, deprecated).
// returns: SUCCESS or FAIL
// ns: target namespace
// username: user to update
// mfa: new MFA value (max USERNS_MFA_SZ-1 chars)
bool
userns_user_set_mfa(const userns_t *ns, const char *username,
    const char *mfa)
{
  return(user_set_field(ns, username, "mfa", mfa, USERNS_MFA_SZ - 1));
}

// Set a user's passphrase field.
// returns: SUCCESS or FAIL
// ns: target namespace
// username: user to update
// passphrase: new passphrase (max USERNS_PASS_SZ-1 chars)
bool
userns_user_set_passphrase(const userns_t *ns, const char *username,
    const char *passphrase)
{
  return(user_set_field(ns, username, "passphrase",
      passphrase, USERNS_PASS_SZ - 1));
}

// -----------------------------------------------------------------------
// Listing / iteration
// -----------------------------------------------------------------------

// Iterate all users in a namespace, invoking cb for each.
// ns: target namespace
// cb: callback invoked with username, uuid, and description
// data: opaque user data passed to cb
void
userns_user_iterate(const userns_t *ns,
    userns_user_iter_cb_t cb, void *data)
{
  if(!userns_ready || ns == NULL || cb == NULL)
    return;

  char sql[256];

  snprintf(sql, sizeof(sql),
      "SELECT username, COALESCE(uuid, ''), COALESCE(description, '') "
      "FROM userns_user WHERE ns_id = %u ORDER BY username",
      ns->id);

  db_result_t *r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    db_result_free(r);
    return;
  }

  for(uint32_t i = 0; i < r->rows; i++)
  {
    const char *user = db_result_get(r, i, 0);
    const char *uuid = db_result_get(r, i, 1);
    const char *desc = db_result_get(r, i, 2);

    if(user != NULL)
      cb(user, uuid ? uuid : "", desc ? desc : "", data);
  }

  db_result_free(r);
}

// Iterate all groups in a namespace, invoking cb for each.
// ns: target namespace
// cb: callback invoked with group name and description
// data: opaque user data passed to cb
void
userns_group_iterate(const userns_t *ns,
    userns_group_iter_cb_t cb, void *data)
{
  if(!userns_ready || ns == NULL || cb == NULL)
    return;

  char sql[256];

  snprintf(sql, sizeof(sql),
      "SELECT name, COALESCE(description, '') "
      "FROM userns_group WHERE ns_id = %u ORDER BY name",
      ns->id);

  db_result_t *r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    db_result_free(r);
    return;
  }

  for(uint32_t i = 0; i < r->rows; i++)
  {
    const char *name = db_result_get(r, i, 0);
    const char *desc = db_result_get(r, i, 1);

    if(name != NULL)
      cb(name, desc ? desc : "", data);
  }

  db_result_free(r);
}

// Iterate all group memberships for a user in a namespace.
// ns: target namespace
// username: user whose memberships to list
// cb: callback invoked with group name and privilege level
// data: opaque user data passed to cb
void
userns_membership_iterate(const userns_t *ns, const char *username,
    userns_membership_iter_cb_t cb, void *data)
{
  if(!userns_ready || ns == NULL || username == NULL || cb == NULL)
    return;

  char *esc_user = db_escape(username);

  if(esc_user == NULL)
    return;

  char sql[512];

  snprintf(sql, sizeof(sql),
      "SELECT g.name, m.level FROM userns_member m "
      "JOIN userns_user u ON m.user_id = u.id "
      "JOIN userns_group g ON m.group_id = g.id "
      "WHERE u.ns_id = %u AND u.username = '%s' "
      "ORDER BY g.name",
      ns->id, esc_user);

  mem_free(esc_user);

  db_result_t *r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    db_result_free(r);
    return;
  }

  for(uint32_t i = 0; i < r->rows; i++)
  {
    const char *group = db_result_get(r, i, 0);
    const char *lvl   = db_result_get(r, i, 1);

    if(group != NULL)
      cb(group, (uint16_t)(lvl ? atoi(lvl) : 0), data);
  }

  db_result_free(r);
}

// -----------------------------------------------------------------------
// Owner identity
// -----------------------------------------------------------------------

// Iterate all loaded namespace names.
// cb: callback invoked for each namespace
// data: opaque user data passed to cb
void
userns_iterate(userns_iter_cb_t cb, void *data)
{
  if(cb == NULL)
    return;

  pthread_mutex_lock(&userns_mutex);

  for(userns_t *ns = userns_list; ns != NULL; ns = ns->next)
  {
    pthread_mutex_unlock(&userns_mutex);
    cb(ns->name, data);
    pthread_mutex_lock(&userns_mutex);
  }

  pthread_mutex_unlock(&userns_mutex);
}

// Check if a username is the system owner (@owner).
// returns: true if username matches USERNS_OWNER_USER
// username: username to check (NULL returns false)
bool
userns_is_owner(const char *username)
{
  if(username == NULL)
    return(false);

  return(strcmp(username, USERNS_OWNER_USER) == 0);
}
