// botmanager — MIT
// User profile field setters, iteration, and utility helpers.

#define USERNS_INTERNAL
#include "userns.h"

#include <string.h>

// User profile field setters

static bool
user_set_field(const userns_t *ns, const char *username,
    const char *column, const char *value, size_t max_len)
{
  char *esc_user;
  char *esc_val;
  char sql[512];
  db_result_t *r;
  bool updated;

  if(!userns_ready || ns == NULL || username == NULL || value == NULL)
    return(FAIL);

  if(strlen(value) > max_len)
  {
    clam(CLAM_WARN, "userns", "value too long for %s (max %zu)",
        column, max_len);
    return(FAIL);
  }

  esc_user = db_escape(username);
  esc_val = db_escape(value);
  if(esc_user == NULL || esc_val == NULL)
  {
    if(esc_user != NULL) mem_free(esc_user);
    if(esc_val != NULL) mem_free(esc_val);
    return(FAIL);
  }

  snprintf(sql, sizeof(sql),
      "UPDATE userns_user SET %s = '%s' "
      "WHERE ns_id = %u AND username = '%s'",
      column, esc_val, ns->id, esc_user);

  mem_free(esc_user);
  mem_free(esc_val);

  r = db_result_alloc();
  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns", "cannot update %s for '%s': %s",
        column, username, r->error);
    db_result_free(r);
    return(FAIL);
  }

  updated = (r->rows_affected > 0);
  db_result_free(r);

  return(updated ? SUCCESS : FAIL);
}

bool
userns_user_set_description(const userns_t *ns, const char *username,
    const char *description)
{
  return(user_set_field(ns, username, "description",
      description, USERNS_DESC_SZ - 1));
}

bool
userns_user_set_passphrase(const userns_t *ns, const char *username,
    const char *passphrase)
{
  return(user_set_field(ns, username, "passphrase",
      passphrase, USERNS_PASS_SZ - 1));
}

// Last-seen tracking

bool
userns_user_touch_lastseen(const userns_t *ns, const char *username,
    const char *method, const char *mfa_string)
{
  char *esc_user;
  char *esc_method;
  char *esc_mfa;
  char sql[768];
  db_result_t *r;
  bool updated;

  if(!userns_ready || ns == NULL || username == NULL)
    return(FAIL);

  esc_user = db_escape(username);
  esc_method = (method != NULL) ? db_escape(method) : NULL;
  esc_mfa = (mfa_string != NULL) ? db_escape(mfa_string) : NULL;

  if(esc_user == NULL)
  {
    if(esc_method != NULL) mem_free(esc_method);
    if(esc_mfa != NULL) mem_free(esc_mfa);
    return(FAIL);
  }

  snprintf(sql, sizeof(sql),
      "UPDATE userns_user SET lastseen = NOW(), "
      "lastseen_method = '%s', lastseen_mfa = '%s' "
      "WHERE ns_id = %u AND username = '%s'",
      esc_method ? esc_method : "",
      esc_mfa ? esc_mfa : "",
      ns->id, esc_user);

  mem_free(esc_user);
  if(esc_method != NULL) mem_free(esc_method);
  if(esc_mfa != NULL) mem_free(esc_mfa);

  r = db_result_alloc();
  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns", "cannot update lastseen for '%s': %s",
        username, r->error);
    db_result_free(r);
    return(FAIL);
  }

  updated = (r->rows_affected > 0);
  db_result_free(r);
  return(updated ? SUCCESS : FAIL);
}

bool
userns_user_get_lastseen(const userns_t *ns, const char *username,
    time_t *ts_out, char *method_out, size_t method_sz,
    char *mfa_out, size_t mfa_sz)
{
  char *esc_user;
  char sql[256];
  db_result_t *r;
  const char *ts_str;
  const char *meth;
  const char *mfa;

  if(!userns_ready || ns == NULL || username == NULL)
    return(FAIL);

  esc_user = db_escape(username);
  if(esc_user == NULL)
    return(FAIL);

  snprintf(sql, sizeof(sql),
      "SELECT EXTRACT(EPOCH FROM lastseen)::BIGINT, "
      "COALESCE(lastseen_method, ''), COALESCE(lastseen_mfa, '') "
      "FROM userns_user WHERE ns_id = %u AND username = '%s'",
      ns->id, esc_user);

  mem_free(esc_user);

  r = db_result_alloc();
  if(db_query(sql, r) != SUCCESS || r->rows == 0)
  {
    db_result_free(r);
    return(FAIL);
  }

  ts_str = db_result_get(r, 0, 0);
  meth   = db_result_get(r, 0, 1);
  mfa    = db_result_get(r, 0, 2);

  if(ts_out != NULL)
    *ts_out = (ts_str != NULL) ? (time_t)strtoll(ts_str, NULL, 10) : 0;

  if(method_out != NULL && method_sz > 0)
  {
    strncpy(method_out, meth ? meth : "", method_sz - 1);
    method_out[method_sz - 1] = '\0';
  }

  if(mfa_out != NULL && mfa_sz > 0)
  {
    strncpy(mfa_out, mfa ? mfa : "", mfa_sz - 1);
    mfa_out[mfa_sz - 1] = '\0';
  }

  db_result_free(r);
  return(SUCCESS);
}

// Listing / iteration

void
userns_user_iterate(const userns_t *ns,
    userns_user_iter_cb_t cb, void *data)
{
  char sql[256];
  db_result_t *r;

  if(!userns_ready || ns == NULL || cb == NULL)
    return;

  snprintf(sql, sizeof(sql),
      "SELECT username, COALESCE(uuid, ''), COALESCE(description, '') "
      "FROM userns_user WHERE ns_id = %u ORDER BY username",
      ns->id);

  r = db_result_alloc();
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

void
userns_group_iterate(const userns_t *ns,
    userns_group_iter_cb_t cb, void *data)
{
  char sql[256];
  db_result_t *r;

  if(!userns_ready || ns == NULL || cb == NULL)
    return;

  snprintf(sql, sizeof(sql),
      "SELECT name, COALESCE(description, '') "
      "FROM userns_group WHERE ns_id = %u ORDER BY name",
      ns->id);

  r = db_result_alloc();
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

void
userns_membership_iterate(const userns_t *ns, const char *username,
    userns_membership_iter_cb_t cb, void *data)
{
  char *esc_user;
  char sql[512];
  db_result_t *r;

  if(!userns_ready || ns == NULL || username == NULL || cb == NULL)
    return;

  esc_user = db_escape(username);
  if(esc_user == NULL)
    return;

  snprintf(sql, sizeof(sql),
      "SELECT g.name, m.level FROM userns_member m "
      "JOIN userns_user u ON m.user_id = u.id "
      "JOIN userns_group g ON m.group_id = g.id "
      "WHERE u.ns_id = %u AND u.username = '%s' "
      "ORDER BY g.name",
      ns->id, esc_user);

  mem_free(esc_user);

  r = db_result_alloc();
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

// Group members iteration

void
userns_group_members_iterate(const userns_t *ns, const char *groupname,
    userns_group_members_iter_cb_t cb, void *data)
{
  char *esc_group;
  char sql[512];
  db_result_t *r;

  if(!userns_ready || ns == NULL || groupname == NULL || cb == NULL)
    return;

  esc_group = db_escape(groupname);
  if(esc_group == NULL)
    return;

  snprintf(sql, sizeof(sql),
      "SELECT u.username, m.level FROM userns_member m "
      "JOIN userns_user u ON m.user_id = u.id "
      "JOIN userns_group g ON m.group_id = g.id "
      "WHERE u.ns_id = %u AND g.name = '%s' "
      "ORDER BY m.level DESC, u.username",
      ns->id, esc_group);

  mem_free(esc_group);

  r = db_result_alloc();
  if(db_query(sql, r) != SUCCESS)
  {
    db_result_free(r);
    return;
  }

  for(uint32_t i = 0; i < r->rows; i++)
  {
    const char *user = db_result_get(r, i, 0);
    const char *lvl  = db_result_get(r, i, 1);

    if(user != NULL)
      cb(user, (uint16_t)(lvl ? atoi(lvl) : 0), data);
  }

  db_result_free(r);
}

// User info lookup

bool
userns_user_get_info(const userns_t *ns, const char *username,
    char *uuid_out, size_t uuid_sz,
    char *desc_out, size_t desc_sz)
{
  char *esc_user;
  char sql[256];
  db_result_t *r;
  const char *uuid;
  const char *desc;

  if(!userns_ready || ns == NULL || username == NULL)
    return(FAIL);

  esc_user = db_escape(username);
  if(esc_user == NULL)
    return(FAIL);

  snprintf(sql, sizeof(sql),
      "SELECT COALESCE(uuid, ''), COALESCE(description, '') "
      "FROM userns_user WHERE ns_id = %u AND username = '%s'",
      ns->id, esc_user);

  mem_free(esc_user);

  r = db_result_alloc();
  if(db_query(sql, r) != SUCCESS)
  {
    db_result_free(r);
    return(FAIL);
  }

  if(r->rows == 0)
  {
    db_result_free(r);
    return(FAIL);
  }

  uuid = db_result_get(r, 0, 0);
  desc = db_result_get(r, 0, 1);

  if(uuid_out != NULL && uuid_sz > 0)
  {
    strncpy(uuid_out, uuid ? uuid : "", uuid_sz - 1);
    uuid_out[uuid_sz - 1] = '\0';
  }

  if(desc_out != NULL && desc_sz > 0)
  {
    strncpy(desc_out, desc ? desc : "", desc_sz - 1);
    desc_out[desc_sz - 1] = '\0';
  }

  db_result_free(r);
  return(SUCCESS);
}

// Autoidentify flag

bool
userns_user_get_autoidentify(const userns_t *ns, const char *username)
{
  char *esc_user;
  char sql[256];
  db_result_t *r;
  const char *val;
  bool result;

  if(!userns_ready || ns == NULL || username == NULL)
    return(false);

  esc_user = db_escape(username);
  if(esc_user == NULL)
    return(false);

  snprintf(sql, sizeof(sql),
      "SELECT autoidentify FROM userns_user "
      "WHERE ns_id = %u AND username = '%s'",
      ns->id, esc_user);

  mem_free(esc_user);

  r = db_result_alloc();
  if(db_query(sql, r) != SUCCESS || r->rows == 0)
  {
    db_result_free(r);
    return(false);
  }

  val = db_result_get(r, 0, 0);
  result = (val != NULL && (val[0] == 't' || val[0] == '1'));

  db_result_free(r);
  return(result);
}

bool
userns_user_set_autoidentify(const userns_t *ns, const char *username,
    bool value)
{
  char *esc_user;
  char sql[256];
  db_result_t *r;
  bool updated;

  if(!userns_ready || ns == NULL || username == NULL)
    return(FAIL);

  esc_user = db_escape(username);
  if(esc_user == NULL)
    return(FAIL);

  snprintf(sql, sizeof(sql),
      "UPDATE userns_user SET autoidentify = %s "
      "WHERE ns_id = %u AND username = '%s'",
      value ? "TRUE" : "FALSE", ns->id, esc_user);

  mem_free(esc_user);

  r = db_result_alloc();
  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns", "cannot set autoidentify for '%s': %s",
        username, r->error);
    db_result_free(r);
    return(FAIL);
  }

  updated = (r->rows_affected > 0);
  db_result_free(r);
  return(updated ? SUCCESS : FAIL);
}

// Owner identity

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

bool
userns_is_owner(const char *username)
{
  if(username == NULL)
    return(false);

  return(strcmp(username, USERNS_OWNER_USER) == 0);
}
