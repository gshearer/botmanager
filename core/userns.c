// botmanager — MIT
// User-namespace subsystem: accounts, groups, permissions.
#define USERNS_INTERNAL
#include "userns.h"

#include <ctype.h>
#include <string.h>
#include <sys/random.h>

// Shared state (declared extern in userns.h USERNS_INTERNAL).
userns_cfg_t userns_cfg = {
  .pass_min = USERNS_PASS_MIN_DEFAULT,
  .pass_max = USERNS_PASS_MAX_DEFAULT,
  .max_mfa  = USERNS_MAX_MFA_DEFAULT,
};

userns_t        *userns_list  = NULL;
pthread_mutex_t  userns_mutex;
uint32_t         userns_total = 0;
bool             userns_ready = false;

// Lifetime counters for statistics (atomic, no lock needed).
uint64_t userns_stat_auth_attempts = 0;
uint64_t userns_stat_auth_failures = 0;
uint64_t userns_stat_mfa_matches   = 0;
uint64_t userns_stat_discoveries   = 0;

static bool
validate_name(const char *name)
{
  size_t len;
  if(name == NULL || name[0] == '\0')
    return(FAIL);

  len = 0;

  for(const char *p = name; *p != '\0'; p++)
  {
    if(!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
          || (*p >= '0' && *p <= '9')))
      return(FAIL);

    len++;

    if(len >= USERNS_NAME_SZ)
      return(FAIL);
  }

  return(SUCCESS);
}

// Find a namespace by name in the in-memory list.
// Must be called with userns_mutex held.
static userns_t *
find_locked(const char *name)
{
  for(userns_t *ns = userns_list; ns != NULL; ns = ns->next)
    if(strcmp(ns->name, name) == 0)
      return(ns);

  return(NULL);
}

// Find a namespace by DB id in the in-memory list.
// Must be called with userns_mutex held.
static userns_t *
find_id_locked(uint32_t id)
{
  for(userns_t *ns = userns_list; ns != NULL; ns = ns->next)
    if(ns->id == id)
      return(ns);

  return(NULL);
}

// Allocate a userns_t and prepend it to the list.
// Must be called with userns_mutex held.
static userns_t *
list_add(uint32_t id, const char *name, time_t created)
{
  userns_t *ns = mem_alloc("userns", "namespace", sizeof(userns_t));

  ns->id = id;
  strncpy(ns->name, name, USERNS_NAME_SZ - 1);
  ns->name[USERNS_NAME_SZ - 1] = '\0';
  ns->created = created;
  ns->mfa_cache = NULL;
  ns->next = userns_list;
  userns_list = ns;
  userns_total++;

  return(ns);
}

// Remove a namespace from the in-memory list and free it.
// Must be called with userns_mutex held.
// returns: SUCCESS or FAIL (not found)
static bool
list_remove(const char *name)
{
  userns_t **prev = &userns_list;

  for(userns_t *ns = userns_list; ns != NULL; ns = ns->next)
  {
    if(strcmp(ns->name, name) == 0)
    {
      *prev = ns->next;
      userns_cache_destroy((userns_cache_t *)ns->mfa_cache);
      mem_free(ns);
      userns_total--;
      return(SUCCESS);
    }

    prev = &ns->next;
  }

  return(FAIL);
}

static bool
validate_username(const char *username)
{
  size_t len;
  if(username == NULL || username[0] == '\0')
    return(FAIL);

  len = 0;

  for(const char *p = username; *p != '\0'; p++)
  {
    if(!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
          || (*p >= '0' && *p <= '9')))
      return(FAIL);

    len++;

    if(len >= USERNS_USER_SZ)
      return(FAIL);
  }

  return(SUCCESS);
}

static bool
gen_salt(uint8_t *buf, size_t len)
{
  ssize_t ret = getrandom(buf, len, 0);

  if(ret < 0 || (size_t)ret != len)
    return(FAIL);

  return(SUCCESS);
}

static bool
hash_password(const char *password, char *encoded, size_t encoded_len)
{
  uint8_t salt[ARGON2_SALT_LEN];

  int ret;
  if(gen_salt(salt, sizeof(salt)) != SUCCESS)
  {
    clam(CLAM_WARN, "userns", "failed to generate salt");
    return(FAIL);
  }

  ret = argon2id_hash_encoded(
      ARGON2_T_COST, ARGON2_M_COST, ARGON2_P_COST,
      password, strlen(password),
      salt, sizeof(salt),
      ARGON2_HASH_LEN, encoded, encoded_len);

  if(ret != ARGON2_OK)
  {
    clam(CLAM_WARN, "userns", "argon2id hash failed: %s",
        argon2_error_message(ret));
    return(FAIL);
  }

  return(SUCCESS);
}

static bool
verify_password(const char *encoded, const char *password)
{
  int ret = argon2id_verify(encoded, password, strlen(password));

  return((ret == ARGON2_OK) ? SUCCESS : FAIL);
}

static bool
create_core_tables(void)
{
  static const char *ddl[] =
  {
    "CREATE TABLE IF NOT EXISTS userns ("
      "id SERIAL PRIMARY KEY, "
      "name VARCHAR(64) NOT NULL UNIQUE, "
      "created TIMESTAMPTZ NOT NULL DEFAULT NOW())",

    "CREATE TABLE IF NOT EXISTS userns_user ("
      "id SERIAL PRIMARY KEY, "
      "ns_id INTEGER NOT NULL REFERENCES userns(id) ON DELETE CASCADE, "
      "username VARCHAR(64) NOT NULL, "
      "uuid VARCHAR(36) NOT NULL DEFAULT '', "
      "passhash TEXT, "
      "description VARCHAR(101) NOT NULL DEFAULT '', "
      "passphrase VARCHAR(101) NOT NULL DEFAULT '', "
      "autoidentify BOOLEAN NOT NULL DEFAULT FALSE, "
      "lastseen TIMESTAMPTZ, "
      "lastseen_method VARCHAR(64) NOT NULL DEFAULT '', "
      "lastseen_mfa VARCHAR(200) NOT NULL DEFAULT '', "
      "created TIMESTAMPTZ NOT NULL DEFAULT NOW(), "
      "UNIQUE(ns_id, username))",

    "CREATE TABLE IF NOT EXISTS userns_group ("
      "id SERIAL PRIMARY KEY, "
      "ns_id INTEGER NOT NULL REFERENCES userns(id) ON DELETE CASCADE, "
      "name VARCHAR(64) NOT NULL, "
      "description VARCHAR(101) NOT NULL DEFAULT '', "
      "created TIMESTAMPTZ NOT NULL DEFAULT NOW(), "
      "UNIQUE(ns_id, name))",

    "CREATE TABLE IF NOT EXISTS userns_member ("
      "user_id INTEGER NOT NULL REFERENCES userns_user(id) ON DELETE CASCADE, "
      "group_id INTEGER NOT NULL REFERENCES userns_group(id) ON DELETE CASCADE, "
      "level INTEGER NOT NULL DEFAULT 0, "
      "PRIMARY KEY(user_id, group_id))",

    "CREATE TABLE IF NOT EXISTS user_mfa ("
      "id SERIAL PRIMARY KEY, "
      "user_id INTEGER NOT NULL REFERENCES userns_user(id) ON DELETE CASCADE, "
      "pattern VARCHAR(200) NOT NULL, "
      "UNIQUE(user_id, pattern))",

    NULL
  };

  for(const char **sql = ddl; *sql != NULL; sql++)
  {
    db_result_t *r = db_result_alloc();

    if(db_query(*sql, r) != SUCCESS)
    {
      clam(CLAM_WARN, "userns", "DDL failed: %s", r->error);
      db_result_free(r);
      return(FAIL);
    }

    db_result_free(r);
  }

  return(SUCCESS);
}

static void
seed_builtin_groups(const userns_t *ns)
{
  static const struct { const char *name; const char *desc; } groups[] = {
    { USERNS_GROUP_OWNER,    "System owner group" },
    { USERNS_GROUP_ADMIN,    "Administrative group" },
    { USERNS_GROUP_USER,     "Authenticated users" },
    { USERNS_GROUP_EVERYONE, "All users including unauthenticated" },
  };

  for(size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++)
  {
    if(userns_group_exists(ns, groups[i].name))
      continue;

    userns_group_create_desc(ns, groups[i].name, groups[i].desc);
  }
}

static void
seed_owner_user(const userns_t *ns)
{
  // Check if @owner already exists.
  uuid_t uu;
  char sql[384];
  db_result_t *r;
  static const char *group_names[] = {
    USERNS_GROUP_OWNER, USERNS_GROUP_ADMIN,
    USERNS_GROUP_USER, USERNS_GROUP_EVERYONE,
  };
  char uuid_str[USERNS_UUID_SZ];
  if(userns_user_exists(ns, USERNS_OWNER_USER))
    return;

  // Insert @owner with no password (cannot authenticate via methods).

  uuid_generate(uu);
  uuid_unparse_lower(uu, uuid_str);


  snprintf(sql, sizeof(sql),
      "INSERT INTO userns_user (ns_id, username, uuid) "
      "VALUES (%u, '%s', '%s') "
      "ON CONFLICT DO NOTHING",
      ns->id, USERNS_OWNER_USER, uuid_str);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns", "cannot seed @owner in '%s': %s",
        ns->name, r->error);
    db_result_free(r);
    return;
  }

  db_result_free(r);

  // Add @owner to all built-in groups at maximum level.

  for(size_t i = 0; i < sizeof(group_names) / sizeof(group_names[0]); i++)
    userns_member_add(ns, USERNS_OWNER_USER, group_names[i],
        USERNS_OWNER_LEVEL);

  clam(CLAM_INFO, "userns", "seeded @owner in namespace '%s'", ns->name);
}

static bool
load_all(void)
{
  db_result_t *r = db_result_alloc();

  if(db_query("SELECT id, name, EXTRACT(EPOCH FROM created)::BIGINT "
               "FROM userns ORDER BY id", r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns", "cannot load namespaces: %s", r->error);
    db_result_free(r);
    return(FAIL);
  }

  pthread_mutex_lock(&userns_mutex);

  for(uint32_t i = 0; i < r->rows; i++)
  {
    const char *id_str   = db_result_get(r, i, 0);
    const char *name_str = db_result_get(r, i, 1);
    const char *ts_str   = db_result_get(r, i, 2);

    uint32_t id;
    time_t created;
    if(id_str == NULL || name_str == NULL || ts_str == NULL)
      continue;

    id = (uint32_t)strtoul(id_str, NULL, 10);
    created = (time_t)strtoll(ts_str, NULL, 10);

    list_add(id, name_str, created);
  }

  pthread_mutex_unlock(&userns_mutex);

  db_result_free(r);

  clam(CLAM_INFO, "userns", "loaded %u namespace(s)", userns_total);

  // Seed built-in groups and @owner user for all loaded namespaces,
  // then populate MFA caches.
  for(userns_t *ns = userns_list; ns != NULL; ns = ns->next)
  {
    seed_builtin_groups(ns);
    seed_owner_user(ns);
    userns_cache_ensure(ns);
  }

  return(SUCCESS);
}

// Public API

// Get user namespace subsystem statistics (thread-safe snapshot).
void
userns_get_stats(userns_stats_t *out)
{
  db_result_t *r;
  if(out == NULL)
    return;

  pthread_mutex_lock(&userns_mutex);
  out->namespaces = userns_total;
  pthread_mutex_unlock(&userns_mutex);

  // Count total users via DB (lightweight aggregate query).
  out->users = 0;
  r = db_result_alloc();

  if(db_query("SELECT COUNT(*) FROM userns_user", r) == SUCCESS
      && r->rows > 0)
  {
    const char *val = db_result_get(r, 0, 0);

    if(val != NULL)
      out->users = (uint32_t)strtoul(val, NULL, 10);
  }

  db_result_free(r);

  out->auth_attempts = __atomic_load_n(&userns_stat_auth_attempts,
      __ATOMIC_RELAXED);
  out->auth_failures = __atomic_load_n(&userns_stat_auth_failures,
      __ATOMIC_RELAXED);
  out->mfa_matches   = __atomic_load_n(&userns_stat_mfa_matches,
      __ATOMIC_RELAXED);
  out->discoveries   = __atomic_load_n(&userns_stat_discoveries,
      __ATOMIC_RELAXED);
}

bool
userns_init(void)
{
  pthread_mutex_init(&userns_mutex, NULL);
  userns_list  = NULL;
  userns_total = 0;

  userns_stat_auth_attempts = 0;
  userns_stat_auth_failures = 0;
  userns_stat_mfa_matches   = 0;
  userns_stat_discoveries   = 0;

  if(create_core_tables() != SUCCESS)
  {
    clam(CLAM_FATAL, "userns_init", "cannot create schema");
    pthread_mutex_destroy(&userns_mutex);
    return(FAIL);
  }

  if(load_all() != SUCCESS)
  {
    clam(CLAM_FATAL, "userns_init", "cannot load namespaces");
    pthread_mutex_destroy(&userns_mutex);
    return(FAIL);
  }

  userns_ready = true;

  clam(CLAM_INFO, "userns_init", "user namespace subsystem initialized");
  return(SUCCESS);
}

// Shut down the user namespace subsystem. Frees all in-memory
// namespace records and destroys MFA caches.
void
userns_exit(void)
{
  uint32_t freed;
  userns_t *ns;
  if(!userns_ready)
    return;

  userns_ready = false;

  pthread_mutex_lock(&userns_mutex);

  freed = 0;
  ns = userns_list;

  while(ns != NULL)
  {
    userns_t *next = ns->next;

    userns_cache_destroy((userns_cache_t *)ns->mfa_cache);
    ns->mfa_cache = NULL;

    mem_free(ns);
    freed++;
    ns = next;
  }

  userns_list = NULL;
  userns_total = 0;

  pthread_mutex_unlock(&userns_mutex);
  pthread_mutex_destroy(&userns_mutex);

  clam(CLAM_INFO, "userns_exit",
      "user namespace subsystem shut down (%u freed)", freed);
}

userns_t *
userns_get(const char *name)
{
  userns_t *ns;
  char *esc_name;
  char sql[256];
  db_result_t *r;
  const char *id_str;
  uint32_t id;
  const char *ts_str;
  time_t created;
  if(!userns_ready)
    return(NULL);

  if(validate_name(name) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_get",
        "invalid namespace name '%s'", name ? name : "(null)");
    return(NULL);
  }

  // Check if already loaded.
  pthread_mutex_lock(&userns_mutex);
  ns = find_locked(name);

  if(ns != NULL)
  {
    pthread_mutex_unlock(&userns_mutex);
    return(ns);
  }

  pthread_mutex_unlock(&userns_mutex);

  // Not found — create in database.
  esc_name = db_escape(name);

  if(esc_name == NULL)
    return(NULL);


  snprintf(sql, sizeof(sql),
      "INSERT INTO userns (name) VALUES ('%s') "
      "RETURNING id, EXTRACT(EPOCH FROM created)::BIGINT",
      esc_name);

  mem_free(esc_name);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_get",
        "cannot create namespace '%s': %s", name, r->error);
    db_result_free(r);
    return(NULL);
  }

  if(r->rows < 1)
  {
    clam(CLAM_WARN, "userns_get",
        "no rows returned for namespace '%s'", name);
    db_result_free(r);
    return(NULL);
  }

  id_str = db_result_get(r, 0, 0);
  ts_str = db_result_get(r, 0, 1);

  if(id_str == NULL || ts_str == NULL)
  {
    db_result_free(r);
    return(NULL);
  }

  id = (uint32_t)strtoul(id_str, NULL, 10);
  created = (time_t)strtoll(ts_str, NULL, 10);

  db_result_free(r);

  pthread_mutex_lock(&userns_mutex);
  ns = list_add(id, name, created);
  pthread_mutex_unlock(&userns_mutex);

  clam(CLAM_INFO, "userns_get", "created namespace '%s' (id: %u)",
      name, id);

  // Seed built-in groups and @owner user for the new namespace.
  seed_builtin_groups(ns);
  seed_owner_user(ns);
  userns_cache_ensure(ns);

  return(ns);
}

userns_t *
userns_find(const char *name)
{
  userns_t *ns;
  if(!userns_ready || name == NULL)
    return(NULL);

  pthread_mutex_lock(&userns_mutex);
  ns = find_locked(name);
  pthread_mutex_unlock(&userns_mutex);

  return(ns);
}

userns_t *
userns_find_id(uint32_t id)
{
  userns_t *ns;
  if(!userns_ready)
    return(NULL);

  pthread_mutex_lock(&userns_mutex);
  ns = find_id_locked(id);
  pthread_mutex_unlock(&userns_mutex);

  return(ns);
}

bool
userns_delete(const char *name)
{
  userns_t *ns;
  uint32_t id;
  char sql[128];
  db_result_t *r;
  if(!userns_ready || name == NULL)
    return(FAIL);

  pthread_mutex_lock(&userns_mutex);
  ns = find_locked(name);

  if(ns == NULL)
  {
    pthread_mutex_unlock(&userns_mutex);
    clam(CLAM_WARN, "userns_delete", "namespace '%s' not found", name);
    return(FAIL);
  }

  id = ns->id;

  pthread_mutex_unlock(&userns_mutex);

  // Delete from database (CASCADE removes users, groups, memberships).

  snprintf(sql, sizeof(sql), "DELETE FROM userns WHERE id = %u", id);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_delete",
        "cannot delete namespace '%s': %s", name, r->error);
    db_result_free(r);
    return(FAIL);
  }

  db_result_free(r);

  // Remove from in-memory list.
  pthread_mutex_lock(&userns_mutex);
  list_remove(name);
  pthread_mutex_unlock(&userns_mutex);

  clam(CLAM_INFO, "userns_delete", "deleted namespace '%s' (id: %u)",
      name, id);

  return(SUCCESS);
}

uint32_t
userns_count(void)
{
  uint32_t n;
  pthread_mutex_lock(&userns_mutex);
  n = userns_total;
  pthread_mutex_unlock(&userns_mutex);

  return(n);
}

// Return the first loaded namespace, or NULL if none exist. Used by
// system-level dispatchers (e.g., botmanctl) that don't bind to a
// specific bot but still need a userns to resolve identity against.
userns_t *
userns_first(void)
{
  userns_t *ns;
  pthread_mutex_lock(&userns_mutex);
  ns = userns_list;
  pthread_mutex_unlock(&userns_mutex);
  return(ns);
}

// Password policy

bool
userns_password_check(const char *password)
{
  size_t len;
  if(password == NULL)
    return(FAIL);

  len = strlen(password);

  if(len < userns_cfg.pass_min || len > userns_cfg.pass_max)
    return(FAIL);

  return(SUCCESS);
}

// User operations

bool
userns_user_create(const userns_t *ns, const char *username,
    const char *password)
{
  char encoded[ARGON2_ENC_LEN];
  uuid_t uu;
  char *esc_user;
  char sql[512];
  db_result_t *r;
  char uuid_str[USERNS_UUID_SZ];
  char *esc_hash;
  if(!userns_ready || ns == NULL)
    return(FAIL);

  if(validate_username(username) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_user_create",
        "invalid username '%s'", username ? username : "(null)");
    return(FAIL);
  }

  if(userns_password_check(password) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_user_create",
        "password does not meet policy for user '%s'", username);
    return(FAIL);
  }

  // Hash the password.

  if(hash_password(password, encoded, sizeof(encoded)) != SUCCESS)
    return(FAIL);

  // Generate UUID for the new user.

  uuid_generate(uu);
  uuid_unparse_lower(uu, uuid_str);

  // Escape inputs.
  esc_user = db_escape(username);
  esc_hash = db_escape(encoded);

  if(esc_user == NULL || esc_hash == NULL)
  {
    if(esc_user != NULL) mem_free(esc_user);
    if(esc_hash != NULL) mem_free(esc_hash);
    return(FAIL);
  }


  snprintf(sql, sizeof(sql),
      "INSERT INTO userns_user (ns_id, username, passhash, uuid) "
      "VALUES (%u, '%s', '%s', '%s')",
      ns->id, esc_user, esc_hash, uuid_str);

  mem_free(esc_user);
  mem_free(esc_hash);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_user_create",
        "cannot create user '%s' in '%s': %s",
        username, ns->name, r->error);
    db_result_free(r);
    return(FAIL);
  }

  db_result_free(r);

  clam(CLAM_INFO, "userns_user_create",
      "created user '%s' in namespace '%s'", username, ns->name);

  // Auto-add to built-in groups "everyone" and "user" at level 0.
  userns_member_add(ns, username, USERNS_GROUP_EVERYONE, 0);
  userns_member_add(ns, username, USERNS_GROUP_USER, 0);

  userns_cache_invalidate(ns);
  return(SUCCESS);
}

bool
userns_user_create_nopass(const userns_t *ns, const char *username)
{
  uuid_t uu;
  char *esc_user;
  char sql[512];
  db_result_t *r;
  char uuid_str[USERNS_UUID_SZ];
  if(!userns_ready || ns == NULL)
    return(FAIL);

  if(validate_username(username) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_user_create_nopass",
        "invalid username '%s'", username ? username : "(null)");
    return(FAIL);
  }

  // Generate UUID.

  uuid_generate(uu);
  uuid_unparse_lower(uu, uuid_str);

  // Escape username.
  esc_user = db_escape(username);

  if(esc_user == NULL)
    return(FAIL);


  snprintf(sql, sizeof(sql),
      "INSERT INTO userns_user (ns_id, username, passhash, uuid) "
      "VALUES (%u, '%s', '', '%s')",
      ns->id, esc_user, uuid_str);

  mem_free(esc_user);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_user_create_nopass",
        "cannot create user '%s' in '%s': %s",
        username, ns->name, r->error);
    db_result_free(r);
    return(FAIL);
  }

  db_result_free(r);

  clam(CLAM_INFO, "userns_user_create_nopass",
      "created password-less user '%s' in namespace '%s'",
      username, ns->name);

  // Auto-add to built-in groups.
  userns_member_add(ns, username, USERNS_GROUP_EVERYONE, 0);
  userns_member_add(ns, username, USERNS_GROUP_USER, 0);

  userns_cache_invalidate(ns);
  return(SUCCESS);
}

bool
userns_user_delete(const userns_t *ns, const char *username)
{
  char *esc_user;
  char sql[256];
  db_result_t *r;
  bool deleted;
  if(!userns_ready || ns == NULL || username == NULL)
    return(FAIL);

  esc_user = db_escape(username);

  if(esc_user == NULL)
    return(FAIL);


  snprintf(sql, sizeof(sql),
      "DELETE FROM userns_user WHERE ns_id = %u AND username = '%s'",
      ns->id, esc_user);

  mem_free(esc_user);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_user_delete",
        "cannot delete user '%s' from '%s': %s",
        username, ns->name, r->error);
    db_result_free(r);
    return(FAIL);
  }

  deleted = (r->rows_affected > 0);

  db_result_free(r);

  if(!deleted)
  {
    clam(CLAM_WARN, "userns_user_delete",
        "user '%s' not found in '%s'", username, ns->name);
    return(FAIL);
  }

  clam(CLAM_INFO, "userns_user_delete",
      "deleted user '%s' from namespace '%s'", username, ns->name);

  userns_cache_invalidate(ns);
  return(SUCCESS);
}

bool
userns_user_exists(const userns_t *ns, const char *username)
{
  char *esc_user;
  char sql[256];
  db_result_t *r;
  bool exists;
  if(!userns_ready || ns == NULL || username == NULL)
    return(false);

  esc_user = db_escape(username);

  if(esc_user == NULL)
    return(false);


  snprintf(sql, sizeof(sql),
      "SELECT 1 FROM userns_user WHERE ns_id = %u AND username = '%s'",
      ns->id, esc_user);

  mem_free(esc_user);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    db_result_free(r);
    return(false);
  }

  exists = (r->rows > 0);

  db_result_free(r);

  return(exists);
}

bool
userns_user_set_password(const userns_t *ns, const char *username,
    const char *old_password, const char *new_password)
{
  userns_auth_t result;
  char encoded[ARGON2_ENC_LEN];
  char *esc_user;
  char sql[512];
  db_result_t *r;
  char *esc_hash;
  if(!userns_ready || ns == NULL || username == NULL)
    return(FAIL);

  // Verify old password first.
  result = userns_auth(ns, username, old_password, NULL);

  if(result != USERNS_AUTH_OK)
  {
    clam(CLAM_WARN, "userns_user_set_password",
        "old password verification failed for '%s' in '%s'",
        username, ns->name);
    return(FAIL);
  }

  // Validate new password.
  if(userns_password_check(new_password) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_user_set_password",
        "new password does not meet policy for '%s'", username);
    return(FAIL);
  }

  // Hash the new password.

  if(hash_password(new_password, encoded, sizeof(encoded)) != SUCCESS)
    return(FAIL);

  // Update in database.
  esc_user = db_escape(username);
  esc_hash = db_escape(encoded);

  if(esc_user == NULL || esc_hash == NULL)
  {
    if(esc_user != NULL) mem_free(esc_user);
    if(esc_hash != NULL) mem_free(esc_hash);
    return(FAIL);
  }


  snprintf(sql, sizeof(sql),
      "UPDATE userns_user SET passhash = '%s' "
      "WHERE ns_id = %u AND username = '%s'",
      esc_hash, ns->id, esc_user);

  mem_free(esc_user);
  mem_free(esc_hash);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_user_set_password",
        "cannot update password for '%s': %s", username, r->error);
    db_result_free(r);
    return(FAIL);
  }

  db_result_free(r);

  clam(CLAM_INFO, "userns_user_set_password",
      "password changed for '%s' in '%s'", username, ns->name);

  return(SUCCESS);
}

// Authentication

userns_auth_t
userns_auth(const userns_t *ns, const char *username,
    const char *password, const char *method_ctx)
{
  char *esc_user;
  char sql[256];
  db_result_t *r;
  const char *stored_hash;
  char *hash_copy;
  bool match;
  __atomic_add_fetch(&userns_stat_auth_attempts, 1, __ATOMIC_RELAXED);

  if(!userns_ready || ns == NULL || username == NULL || password == NULL)
    return(USERNS_AUTH_ERR);

  // Fetch the stored hash from the database.
  esc_user = db_escape(username);

  if(esc_user == NULL)
    return(USERNS_AUTH_ERR);


  snprintf(sql, sizeof(sql),
      "SELECT passhash FROM userns_user "
      "WHERE ns_id = %u AND username = '%s'",
      ns->id, esc_user);

  mem_free(esc_user);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_auth", "DB error for '%s': %s",
        username, r->error);
    db_result_free(r);
    return(USERNS_AUTH_ERR);
  }

  if(r->rows == 0)
  {
    db_result_free(r);
    __atomic_add_fetch(&userns_stat_auth_failures, 1, __ATOMIC_RELAXED);
    clam(CLAM_DEBUG, "userns_auth",
        "user '%s' not found in '%s'", username, ns->name);
    return(USERNS_AUTH_BAD_USER);
  }

  stored_hash = db_result_get(r, 0, 0);

  if(stored_hash == NULL || stored_hash[0] == '\0')
  {
    db_result_free(r);
    clam(CLAM_WARN, "userns_auth",
        "user '%s' in '%s' has no password set", username, ns->name);
    return(USERNS_AUTH_NO_HASH);
  }

  // Copy hash before freeing result (pointer invalidated by free).
  hash_copy = mem_strdup("userns", "hash", stored_hash);

  db_result_free(r);

  // Verify password against stored hash.
  match = verify_password(hash_copy, password);

  mem_free(hash_copy);

  if(match != SUCCESS)
  {
    __atomic_add_fetch(&userns_stat_auth_failures, 1, __ATOMIC_RELAXED);
    clam(CLAM_DEBUG, "userns_auth",
        "password mismatch for '%s' in '%s'", username, ns->name);
    return(USERNS_AUTH_BAD_PASS);
  }

  // Log successful authentication with method context if provided.
  if(method_ctx != NULL && method_ctx[0] != '\0')
    clam(CLAM_INFO, "userns_auth",
        "'%s' authenticated in '%s' (method: %s)",
        username, ns->name, method_ctx);

  else
    clam(CLAM_INFO, "userns_auth",
        "'%s' authenticated in '%s'", username, ns->name);

  return(USERNS_AUTH_OK);
}

// Group operations

static bool
validate_groupname(const char *name)
{
  size_t len;
  if(name == NULL || name[0] == '\0')
    return(FAIL);

  len = 0;

  for(const char *p = name; *p != '\0'; p++)
  {
    if(!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
          || (*p >= '0' && *p <= '9')))
      return(FAIL);

    len++;

    if(len >= USERNS_GROUP_SZ)
      return(FAIL);
  }

  return(SUCCESS);
}

bool
userns_group_create(const userns_t *ns, const char *name)
{
  char *esc_name;
  char sql[256];
  db_result_t *r;
  if(!userns_ready || ns == NULL)
    return(FAIL);

  if(validate_groupname(name) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_group_create",
        "invalid group name '%s'", name ? name : "(null)");
    return(FAIL);
  }

  esc_name = db_escape(name);

  if(esc_name == NULL)
    return(FAIL);


  snprintf(sql, sizeof(sql),
      "INSERT INTO userns_group (ns_id, name) VALUES (%u, '%s')",
      ns->id, esc_name);

  mem_free(esc_name);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_group_create",
        "cannot create group '%s' in '%s': %s",
        name, ns->name, r->error);
    db_result_free(r);
    return(FAIL);
  }

  db_result_free(r);

  clam(CLAM_INFO, "userns_group_create",
      "created group '%s' in namespace '%s'", name, ns->name);

  return(SUCCESS);
}

bool
userns_group_delete(const userns_t *ns, const char *name)
{
  char *esc_name;
  char sql[256];
  db_result_t *r;
  bool deleted;
  if(!userns_ready || ns == NULL || name == NULL)
    return(FAIL);

  if(userns_group_is_builtin(name))
  {
    clam(CLAM_WARN, "userns_group_delete",
        "cannot delete built-in group '%s'", name);
    return(FAIL);
  }

  esc_name = db_escape(name);

  if(esc_name == NULL)
    return(FAIL);


  snprintf(sql, sizeof(sql),
      "DELETE FROM userns_group WHERE ns_id = %u AND name = '%s'",
      ns->id, esc_name);

  mem_free(esc_name);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_group_delete",
        "cannot delete group '%s' from '%s': %s",
        name, ns->name, r->error);
    db_result_free(r);
    return(FAIL);
  }

  deleted = (r->rows_affected > 0);

  db_result_free(r);

  if(!deleted)
  {
    clam(CLAM_WARN, "userns_group_delete",
        "group '%s' not found in '%s'", name, ns->name);
    return(FAIL);
  }

  clam(CLAM_INFO, "userns_group_delete",
      "deleted group '%s' from namespace '%s'", name, ns->name);

  return(SUCCESS);
}

bool
userns_group_exists(const userns_t *ns, const char *name)
{
  char *esc_name;
  char sql[256];
  db_result_t *r;
  bool exists;
  if(!userns_ready || ns == NULL || name == NULL)
    return(false);

  esc_name = db_escape(name);

  if(esc_name == NULL)
    return(false);


  snprintf(sql, sizeof(sql),
      "SELECT 1 FROM userns_group WHERE ns_id = %u AND name = '%s'",
      ns->id, esc_name);

  mem_free(esc_name);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    db_result_free(r);
    return(false);
  }

  exists = (r->rows > 0);

  db_result_free(r);

  return(exists);
}

// Membership operations

bool
userns_member_add(const userns_t *ns, const char *username,
    const char *group, uint16_t level)
{
  char *esc_user;
  char sql[512];
  db_result_t *r;
  bool added;
  char *esc_group;
  if(!userns_ready || ns == NULL || username == NULL || group == NULL)
    return(FAIL);

  esc_user = db_escape(username);
  esc_group = db_escape(group);

  if(esc_user == NULL || esc_group == NULL)
  {
    if(esc_user != NULL) mem_free(esc_user);
    if(esc_group != NULL) mem_free(esc_group);
    return(FAIL);
  }


  snprintf(sql, sizeof(sql),
      "INSERT INTO userns_member (user_id, group_id, level) "
      "SELECT u.id, g.id, %u "
      "FROM userns_user u, userns_group g "
      "WHERE u.ns_id = %u AND u.username = '%s' "
      "AND g.ns_id = %u AND g.name = '%s'",
      (unsigned)level, ns->id, esc_user, ns->id, esc_group);

  mem_free(esc_user);
  mem_free(esc_group);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_member_add",
        "cannot add '%s' to group '%s' in '%s': %s",
        username, group, ns->name, r->error);
    db_result_free(r);
    return(FAIL);
  }

  added = (r->rows_affected > 0);

  db_result_free(r);

  if(!added)
  {
    clam(CLAM_WARN, "userns_member_add",
        "user '%s' or group '%s' not found in '%s'",
        username, group, ns->name);
    return(FAIL);
  }

  clam(CLAM_INFO, "userns_member_add",
      "added '%s' to group '%s' in '%s' (level %u)",
      username, group, ns->name, (unsigned)level);

  return(SUCCESS);
}

bool
userns_member_remove(const userns_t *ns, const char *username,
    const char *group)
{
  char *esc_user;
  char sql[512];
  db_result_t *r;
  bool removed;
  char *esc_group;
  if(!userns_ready || ns == NULL || username == NULL || group == NULL)
    return(FAIL);

  esc_user = db_escape(username);
  esc_group = db_escape(group);

  if(esc_user == NULL || esc_group == NULL)
  {
    if(esc_user != NULL) mem_free(esc_user);
    if(esc_group != NULL) mem_free(esc_group);
    return(FAIL);
  }


  snprintf(sql, sizeof(sql),
      "DELETE FROM userns_member "
      "WHERE user_id = ("
        "SELECT id FROM userns_user "
        "WHERE ns_id = %u AND username = '%s') "
      "AND group_id = ("
        "SELECT id FROM userns_group "
        "WHERE ns_id = %u AND name = '%s')",
      ns->id, esc_user, ns->id, esc_group);

  mem_free(esc_user);
  mem_free(esc_group);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_member_remove",
        "cannot remove '%s' from group '%s' in '%s': %s",
        username, group, ns->name, r->error);
    db_result_free(r);
    return(FAIL);
  }

  removed = (r->rows_affected > 0);

  db_result_free(r);

  if(!removed)
  {
    clam(CLAM_WARN, "userns_member_remove",
        "'%s' is not a member of '%s' in '%s'",
        username, group, ns->name);
    return(FAIL);
  }

  clam(CLAM_INFO, "userns_member_remove",
      "removed '%s' from group '%s' in '%s'",
      username, group, ns->name);

  return(SUCCESS);
}

int32_t
userns_member_level(const userns_t *ns, const char *username,
    const char *group)
{
  char *esc_user;
  char sql[512];
  db_result_t *r;
  const char *val;
  char *esc_group;
  int32_t level;
  if(!userns_ready || ns == NULL || username == NULL || group == NULL)
    return(-1);

  esc_user = db_escape(username);
  esc_group = db_escape(group);

  if(esc_user == NULL || esc_group == NULL)
  {
    if(esc_user != NULL) mem_free(esc_user);
    if(esc_group != NULL) mem_free(esc_group);
    return(-1);
  }


  snprintf(sql, sizeof(sql),
      "SELECT m.level FROM userns_member m "
      "JOIN userns_user u ON m.user_id = u.id "
      "JOIN userns_group g ON m.group_id = g.id "
      "WHERE u.ns_id = %u AND u.username = '%s' "
      "AND g.ns_id = %u AND g.name = '%s'",
      ns->id, esc_user, ns->id, esc_group);

  mem_free(esc_user);
  mem_free(esc_group);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    db_result_free(r);
    return(-1);
  }

  if(r->rows == 0)
  {
    db_result_free(r);
    return(-1);
  }

  val = db_result_get(r, 0, 0);
  level = (val != NULL) ? (int32_t)atoi(val) : 0;

  db_result_free(r);

  return(level);
}

bool
userns_member_check(const userns_t *ns, const char *username,
    const char *group)
{
  return(userns_member_level(ns, username, group) >= 0);
}

bool
userns_group_create_desc(const userns_t *ns, const char *name,
    const char *description)
{
  char *esc_name;
  const char *desc;
  char sql[512];
  db_result_t *r;
  char *esc_desc;
  if(!userns_ready || ns == NULL)
    return(FAIL);

  if(validate_groupname(name) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_group_create_desc",
        "invalid group name '%s'", name ? name : "(null)");
    return(FAIL);
  }

  esc_name = db_escape(name);

  if(esc_name == NULL)
    return(FAIL);

  desc = (description != NULL) ? description : "";
  esc_desc = db_escape(desc);

  if(esc_desc == NULL)
  {
    mem_free(esc_name);
    return(FAIL);
  }


  snprintf(sql, sizeof(sql),
      "INSERT INTO userns_group (ns_id, name, description) "
      "VALUES (%u, '%s', '%s')",
      ns->id, esc_name, esc_desc);

  mem_free(esc_name);
  mem_free(esc_desc);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_group_create_desc",
        "cannot create group '%s' in '%s': %s",
        name, ns->name, r->error);
    db_result_free(r);
    return(FAIL);
  }

  db_result_free(r);

  clam(CLAM_INFO, "userns_group_create_desc",
      "created group '%s' in namespace '%s'", name, ns->name);

  return(SUCCESS);
}

bool
userns_group_is_builtin(const char *name)
{
  if(name == NULL)
    return(false);

  return(strcmp(name, USERNS_GROUP_OWNER) == 0
      || strcmp(name, USERNS_GROUP_ADMIN) == 0
      || strcmp(name, USERNS_GROUP_USER) == 0
      || strcmp(name, USERNS_GROUP_EVERYONE) == 0);
}

bool
userns_group_set_description(const userns_t *ns, const char *name,
    const char *description)
{
  char *esc_name;
  char *esc_desc;
  char sql[512];
  db_result_t *r;
  if(!userns_ready || ns == NULL || name == NULL || description == NULL)
    return(FAIL);

  if(!userns_group_exists(ns, name))
  {
    clam(CLAM_WARN, "userns_group_set_desc",
        "group '%s' not found in '%s'", name, ns->name);
    return(FAIL);
  }

  esc_name = db_escape(name);

  if(esc_name == NULL)
    return(FAIL);

  esc_desc = db_escape(description);

  if(esc_desc == NULL)
  {
    mem_free(esc_name);
    return(FAIL);
  }


  snprintf(sql, sizeof(sql),
      "UPDATE userns_group SET description = '%s' "
      "WHERE ns_id = %u AND name = '%s'",
      esc_desc, ns->id, esc_name);

  mem_free(esc_name);
  mem_free(esc_desc);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_group_set_desc",
        "cannot update group '%s' in '%s': %s",
        name, ns->name, r->error);
    db_result_free(r);
    return(FAIL);
  }

  db_result_free(r);

  clam(CLAM_INFO, "userns_group_set_desc",
      "updated description for group '%s' in '%s'", name, ns->name);

  return(SUCCESS);
}

bool
userns_member_set_level(const userns_t *ns, const char *username,
    const char *group, uint16_t level)
{
  char *esc_user;
  char sql[512];
  db_result_t *r;
  bool updated;
  char *esc_group;
  if(!userns_ready || ns == NULL || username == NULL || group == NULL)
    return(FAIL);

  esc_user = db_escape(username);
  esc_group = db_escape(group);

  if(esc_user == NULL || esc_group == NULL)
  {
    if(esc_user != NULL) mem_free(esc_user);
    if(esc_group != NULL) mem_free(esc_group);
    return(FAIL);
  }


  snprintf(sql, sizeof(sql),
      "UPDATE userns_member SET level = %u "
      "WHERE user_id = ("
        "SELECT id FROM userns_user "
        "WHERE ns_id = %u AND username = '%s') "
      "AND group_id = ("
        "SELECT id FROM userns_group "
        "WHERE ns_id = %u AND name = '%s')",
      (unsigned)level, ns->id, esc_user, ns->id, esc_group);

  mem_free(esc_user);
  mem_free(esc_group);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_member_set_level",
        "cannot update level for '%s' in group '%s': %s",
        username, group, r->error);
    db_result_free(r);
    return(FAIL);
  }

  updated = (r->rows_affected > 0);

  db_result_free(r);

  if(!updated)
  {
    clam(CLAM_WARN, "userns_member_set_level",
        "'%s' is not a member of '%s' in '%s'",
        username, group, ns->name);
    return(FAIL);
  }

  clam(CLAM_INFO, "userns_member_set_level",
      "set level %u for '%s' in group '%s' of '%s'",
      (unsigned)level, username, group, ns->name);

  return(SUCCESS);
}

// Admin password reset

// Reset a user's password without requiring the old password.
// Intended for administrative use only. New password must meet policy.
// new_password: replacement password (must meet policy)
bool
userns_user_reset_password(const userns_t *ns, const char *username,
    const char *new_password)
{
  char encoded[ARGON2_ENC_LEN];
  char *esc_user;
  char sql[512];
  db_result_t *r;
  char *esc_hash;
  if(!userns_ready || ns == NULL || username == NULL
      || new_password == NULL)
    return(FAIL);

  if(!userns_user_exists(ns, username))
  {
    clam(CLAM_WARN, "userns_user_reset_password",
        "user '%s' not found in '%s'", username, ns->name);
    return(FAIL);
  }

  if(userns_password_check(new_password) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_user_reset_password",
        "new password does not meet policy for '%s'", username);
    return(FAIL);
  }


  if(hash_password(new_password, encoded, sizeof(encoded)) != SUCCESS)
    return(FAIL);

  esc_user = db_escape(username);
  esc_hash = db_escape(encoded);

  if(esc_user == NULL || esc_hash == NULL)
  {
    if(esc_user != NULL) mem_free(esc_user);
    if(esc_hash != NULL) mem_free(esc_hash);
    return(FAIL);
  }


  snprintf(sql, sizeof(sql),
      "UPDATE userns_user SET passhash = '%s' "
      "WHERE ns_id = %u AND username = '%s'",
      esc_hash, ns->id, esc_user);

  mem_free(esc_user);
  mem_free(esc_hash);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_user_reset_password",
        "cannot reset password for '%s': %s", username, r->error);
    db_result_free(r);
    return(FAIL);
  }

  db_result_free(r);

  clam(CLAM_INFO, "userns_user_reset_password",
      "password reset for '%s' in '%s'", username, ns->name);

  return(SUCCESS);
}

uint32_t
userns_user_id(const userns_t *ns, const char *username)
{
  if(ns == NULL || username == NULL || username[0] == '\0') return(0);
  return(userns_get_user_id(ns, username));
}

uint32_t
userns_get_user_id(const userns_t *ns, const char *username)
{
  char *esc_user = db_escape(username);

  char sql[256];
  db_result_t *r;
  const char *id_str;
  uint32_t id;
  if(esc_user == NULL)
    return(0);


  snprintf(sql, sizeof(sql),
      "SELECT id FROM userns_user WHERE ns_id = %u AND username = '%s'",
      ns->id, esc_user);

  mem_free(esc_user);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS || r->rows == 0)
  {
    db_result_free(r);
    return(0);
  }

  id_str = db_result_get(r, 0, 0);
  id = (id_str != NULL) ? (uint32_t)strtoul(id_str, NULL, 10) : 0;

  db_result_free(r);

  return(id);
}

// KV configuration

// Load password policy and MFA configuration from the KV store.
// Clamps values to sane bounds (pass_min >= 4, max_mfa 1..100).
static void
userns_load_config(void)
{
  userns_cfg.pass_min = (uint32_t)kv_get_uint("core.userns.pass_min");
  userns_cfg.pass_max = (uint32_t)kv_get_uint("core.userns.pass_max");
  userns_cfg.max_mfa  = (uint32_t)kv_get_uint("core.userns.max_mfa");

  if(userns_cfg.pass_min < 4)   userns_cfg.pass_min = 4;
  if(userns_cfg.pass_max < userns_cfg.pass_min)
    userns_cfg.pass_max = userns_cfg.pass_min;
  if(userns_cfg.pass_max > 512) userns_cfg.pass_max = 512;

  if(userns_cfg.max_mfa < 1)    userns_cfg.max_mfa = 1;
  if(userns_cfg.max_mfa > 100)  userns_cfg.max_mfa = 100;
}

static void
userns_kv_changed(const char *key, void *data)
{
  (void)key;
  (void)data;
  userns_load_config();
}

// Register userns KV configuration keys (pass_min, pass_max, max_mfa)
// with defaults and change callbacks, then load initial values.
void
userns_register_config(void)
{
  kv_register("core.userns.pass_min", KV_UINT32, "8",   userns_kv_changed, NULL,
      "Minimum password length");
  kv_register("core.userns.pass_max", KV_UINT32, "128", userns_kv_changed, NULL,
      "Maximum password length");
  kv_register("core.userns.max_mfa",  KV_UINT32, "10",  userns_kv_changed, NULL,
      "Maximum MFA tokens per user");
  userns_load_config();
}
