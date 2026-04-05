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

// -----------------------------------------------------------------------
// Validate a namespace name: alphanumeric only, 1..USERNS_NAME_SZ-1.
// returns: SUCCESS or FAIL
// name: candidate namespace name
// -----------------------------------------------------------------------
static bool
validate_name(const char *name)
{
  if(name == NULL || name[0] == '\0')
    return(FAIL);

  size_t len = 0;

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

// -----------------------------------------------------------------------
// Find a namespace by name in the in-memory list.
// Must be called with userns_mutex held.
// returns: namespace pointer or NULL
// name: namespace name
// -----------------------------------------------------------------------
static userns_t *
find_locked(const char *name)
{
  for(userns_t *ns = userns_list; ns != NULL; ns = ns->next)
  {
    if(strcmp(ns->name, name) == 0)
      return(ns);
  }

  return(NULL);
}

// -----------------------------------------------------------------------
// Find a namespace by DB id in the in-memory list.
// Must be called with userns_mutex held.
// returns: namespace pointer or NULL
// id: database primary key
// -----------------------------------------------------------------------
static userns_t *
find_id_locked(uint32_t id)
{
  for(userns_t *ns = userns_list; ns != NULL; ns = ns->next)
  {
    if(ns->id == id)
      return(ns);
  }

  return(NULL);
}

// -----------------------------------------------------------------------
// Allocate a userns_t and prepend it to the list.
// Must be called with userns_mutex held.
// returns: new namespace pointer
// id: database primary key
// name: namespace name
// created: creation timestamp
// -----------------------------------------------------------------------
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

// -----------------------------------------------------------------------
// Remove a namespace from the in-memory list and free it.
// Must be called with userns_mutex held.
// returns: SUCCESS or FAIL (not found)
// name: namespace name
// -----------------------------------------------------------------------
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

// -----------------------------------------------------------------------
// Validate a username: alphanumeric only, 1..USERNS_USER_SZ-1.
// returns: SUCCESS or FAIL
// username: candidate username
// -----------------------------------------------------------------------
static bool
validate_username(const char *username)
{
  if(username == NULL || username[0] == '\0')
    return(FAIL);

  size_t len = 0;

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

// -----------------------------------------------------------------------
// Generate cryptographically random salt bytes.
// returns: SUCCESS or FAIL
// buf: destination buffer
// len: number of bytes to generate
// -----------------------------------------------------------------------
static bool
gen_salt(uint8_t *buf, size_t len)
{
  ssize_t ret = getrandom(buf, len, 0);

  if(ret < 0 || (size_t)ret != len)
    return(FAIL);

  return(SUCCESS);
}

// -----------------------------------------------------------------------
// Hash a password with argon2id.
// returns: SUCCESS or FAIL
// password: cleartext password
// encoded: output buffer for encoded hash string
// encoded_len: size of output buffer
// -----------------------------------------------------------------------
static bool
hash_password(const char *password, char *encoded, size_t encoded_len)
{
  uint8_t salt[ARGON2_SALT_LEN];

  if(gen_salt(salt, sizeof(salt)) != SUCCESS)
  {
    clam(CLAM_WARN, "userns", "failed to generate salt");
    return(FAIL);
  }

  int ret = argon2id_hash_encoded(
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

// -----------------------------------------------------------------------
// Verify a password against an encoded argon2id hash.
// returns: SUCCESS (password matches) or FAIL
// encoded: stored encoded hash string
// password: cleartext password to verify
// -----------------------------------------------------------------------
static bool
verify_password(const char *encoded, const char *password)
{
  int ret = argon2id_verify(encoded, password, strlen(password));

  return((ret == ARGON2_OK) ? SUCCESS : FAIL);
}

// -----------------------------------------------------------------------
// Run core DDL statements to create userns tables.
// returns: SUCCESS or FAIL
// -----------------------------------------------------------------------
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
      "passhash TEXT, "
      "created TIMESTAMPTZ NOT NULL DEFAULT NOW(), "
      "UNIQUE(ns_id, username))",

    "CREATE TABLE IF NOT EXISTS userns_group ("
      "id SERIAL PRIMARY KEY, "
      "ns_id INTEGER NOT NULL REFERENCES userns(id) ON DELETE CASCADE, "
      "name VARCHAR(64) NOT NULL, "
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

// -----------------------------------------------------------------------
// Try to add a column via ALTER TABLE. If the probe query succeeds, the
// column already exists and nothing is done.
// returns: SUCCESS or FAIL
// probe: SELECT query that tests for the column (e.g. "SELECT col FROM t LIMIT 0")
// alter: ALTER TABLE statement to add the column
// label: human-readable label for log messages
// -----------------------------------------------------------------------
static bool
migrate_add_column(const char *probe, const char *alter, const char *label)
{
  db_result_t *r = db_result_alloc();

  if(db_query(probe, r) == SUCCESS)
  {
    db_result_free(r);
    return(SUCCESS);
  }

  db_result_free(r);
  r = db_result_alloc();

  if(db_query(alter, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns", "migration: %s: %s", label, r->error);
    db_result_free(r);
    return(FAIL);
  }

  db_result_free(r);
  clam(CLAM_INFO, "userns", "migration: %s", label);
  return(SUCCESS);
}

// -----------------------------------------------------------------------
// Migration: add uuid column and backfill existing rows.
// returns: SUCCESS or FAIL
// -----------------------------------------------------------------------
static bool
migrate_uuid_column(void)
{
  db_result_t *r = db_result_alloc();

  if(db_query("SELECT uuid FROM userns_user LIMIT 0", r) == SUCCESS)
  {
    db_result_free(r);
    return(SUCCESS);
  }

  db_result_free(r);
  r = db_result_alloc();

  if(db_query("ALTER TABLE userns_user "
               "ADD COLUMN uuid VARCHAR(36) NOT NULL DEFAULT ''",
               r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns",
        "migration: cannot add uuid column: %s", r->error);
    db_result_free(r);
    return(FAIL);
  }

  clam(CLAM_INFO, "userns",
      "migration: added uuid column to userns_user");

  // Generate UUIDs for existing users that have empty uuid.
  db_result_free(r);
  r = db_result_alloc();

  if(db_query("SELECT id FROM userns_user WHERE uuid = ''",
               r) == SUCCESS)
  {
    for(uint32_t i = 0; i < r->rows; i++)
    {
      const char *id_str = db_result_get(r, i, 0);

      if(id_str == NULL)
        continue;

      uuid_t uu;
      char uuid_str[USERNS_UUID_SZ];

      uuid_generate(uu);
      uuid_unparse_lower(uu, uuid_str);

      char sql[256];

      snprintf(sql, sizeof(sql),
          "UPDATE userns_user SET uuid = '%s' WHERE id = %s",
          uuid_str, id_str);

      db_result_t *ur = db_result_alloc();

      db_query(sql, ur);
      db_result_free(ur);
    }

    clam(CLAM_INFO, "userns",
        "migration: generated UUIDs for %u existing user(s)", r->rows);
  }

  db_result_free(r);
  return(SUCCESS);
}

// -----------------------------------------------------------------------
// Migration: copy legacy mfa field values into user_mfa table.
// returns: SUCCESS or FAIL
// -----------------------------------------------------------------------
static bool
migrate_legacy_mfa(void)
{
  db_result_t *r = db_result_alloc();

  if(db_query("SELECT id, mfa FROM userns_user "
               "WHERE mfa != '' AND mfa IS NOT NULL "
               "AND id NOT IN (SELECT user_id FROM user_mfa)",
               r) != SUCCESS || r->rows == 0)
  {
    db_result_free(r);
    return(SUCCESS);
  }

  uint32_t migrated = 0;

  for(uint32_t i = 0; i < r->rows; i++)
  {
    const char *id_str  = db_result_get(r, i, 0);
    const char *mfa_str = db_result_get(r, i, 1);

    if(id_str == NULL || mfa_str == NULL || mfa_str[0] == '\0')
      continue;

    char *esc_mfa = db_escape(mfa_str);

    if(esc_mfa == NULL)
      continue;

    char sql[512];

    snprintf(sql, sizeof(sql),
        "INSERT INTO user_mfa (user_id, pattern) "
        "VALUES (%s, '%s') ON CONFLICT DO NOTHING",
        id_str, esc_mfa);

    mem_free(esc_mfa);

    db_result_t *mr = db_result_alloc();

    if(db_query(sql, mr) == SUCCESS)
      migrated++;

    db_result_free(mr);
  }

  if(migrated > 0)
    clam(CLAM_INFO, "userns",
        "migration: migrated %u legacy MFA value(s) to user_mfa",
        migrated);

  db_result_free(r);
  return(SUCCESS);
}

// -----------------------------------------------------------------------
// Create the userns tables if they do not exist, then run migrations.
// returns: SUCCESS or FAIL
// -----------------------------------------------------------------------
static bool
ensure_tables(void)
{
  if(create_core_tables() != SUCCESS)
    return(FAIL);

  if(migrate_add_column(
      "SELECT level FROM userns_member LIMIT 0",
      "ALTER TABLE userns_member "
        "ADD COLUMN level INTEGER NOT NULL DEFAULT 0",
      "added level column to userns_member") != SUCCESS)
    return(FAIL);

  if(migrate_add_column(
      "SELECT description FROM userns_group LIMIT 0",
      "ALTER TABLE userns_group "
        "ADD COLUMN description VARCHAR(101) NOT NULL DEFAULT ''",
      "added description column to userns_group") != SUCCESS)
    return(FAIL);

  if(migrate_add_column(
      "SELECT description FROM userns_user LIMIT 0",
      "ALTER TABLE userns_user "
        "ADD COLUMN description VARCHAR(101) NOT NULL DEFAULT '', "
        "ADD COLUMN mfa VARCHAR(101) NOT NULL DEFAULT '', "
        "ADD COLUMN passphrase VARCHAR(101) NOT NULL DEFAULT ''",
      "added description/mfa/passphrase to userns_user") != SUCCESS)
    return(FAIL);

  if(migrate_uuid_column() != SUCCESS)
    return(FAIL);

  if(migrate_legacy_mfa() != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

// -----------------------------------------------------------------------
// Seed built-in groups for a namespace. Idempotent — skips groups
// that already exist.
// ns: namespace to seed
// -----------------------------------------------------------------------
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

// -----------------------------------------------------------------------
// Seed the @owner user for a namespace. Inserts directly via SQL
// since the @ prefix fails alphanumeric validation. Idempotent.
// ns: namespace to seed
// -----------------------------------------------------------------------
static void
seed_owner_user(const userns_t *ns)
{
  // Check if @owner already exists.
  if(userns_user_exists(ns, USERNS_OWNER_USER))
    return;

  // Insert @owner with no password (cannot authenticate via methods).
  uuid_t uu;
  char uuid_str[USERNS_UUID_SZ];

  uuid_generate(uu);
  uuid_unparse_lower(uu, uuid_str);

  char sql[384];

  snprintf(sql, sizeof(sql),
      "INSERT INTO userns_user (ns_id, username, uuid) "
      "VALUES (%u, '%s', '%s') "
      "ON CONFLICT DO NOTHING",
      ns->id, USERNS_OWNER_USER, uuid_str);

  db_result_t *r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns", "cannot seed @owner in '%s': %s",
        ns->name, r->error);
    db_result_free(r);
    return;
  }

  db_result_free(r);

  // Add @owner to all built-in groups at maximum level.
  static const char *group_names[] = {
    USERNS_GROUP_OWNER, USERNS_GROUP_ADMIN,
    USERNS_GROUP_USER, USERNS_GROUP_EVERYONE,
  };

  for(size_t i = 0; i < sizeof(group_names) / sizeof(group_names[0]); i++)
    userns_member_add(ns, USERNS_OWNER_USER, group_names[i],
        USERNS_OWNER_LEVEL);

  clam(CLAM_INFO, "userns", "seeded @owner in namespace '%s'", ns->name);
}

// -----------------------------------------------------------------------
// Load all existing namespaces from the database into memory.
// returns: SUCCESS or FAIL
// -----------------------------------------------------------------------
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

    if(id_str == NULL || name_str == NULL || ts_str == NULL)
      continue;

    uint32_t id = (uint32_t)strtoul(id_str, NULL, 10);
    time_t created = (time_t)strtoll(ts_str, NULL, 10);

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

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

// Initialize the user namespace subsystem.
// returns: SUCCESS or FAIL
bool
userns_init(void)
{
  pthread_mutex_init(&userns_mutex, NULL);
  userns_list = NULL;
  userns_total = 0;

  if(ensure_tables() != SUCCESS)
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
  if(!userns_ready)
    return;

  userns_ready = false;

  pthread_mutex_lock(&userns_mutex);

  uint32_t freed = 0;
  userns_t *ns = userns_list;

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

// Find or create a namespace by name.
// returns: namespace pointer, or NULL on failure
// name: namespace name (alphanumeric, max USERNS_NAME_SZ-1 chars)
userns_t *
userns_get(const char *name)
{
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
  userns_t *ns = find_locked(name);

  if(ns != NULL)
  {
    pthread_mutex_unlock(&userns_mutex);
    return(ns);
  }

  pthread_mutex_unlock(&userns_mutex);

  // Not found — create in database.
  char *esc_name = db_escape(name);

  if(esc_name == NULL)
    return(NULL);

  char sql[256];

  snprintf(sql, sizeof(sql),
      "INSERT INTO userns (name) VALUES ('%s') "
      "RETURNING id, EXTRACT(EPOCH FROM created)::BIGINT",
      esc_name);

  mem_free(esc_name);

  db_result_t *r = db_result_alloc();

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

  const char *id_str = db_result_get(r, 0, 0);
  const char *ts_str = db_result_get(r, 0, 1);

  if(id_str == NULL || ts_str == NULL)
  {
    db_result_free(r);
    return(NULL);
  }

  uint32_t id = (uint32_t)strtoul(id_str, NULL, 10);
  time_t created = (time_t)strtoll(ts_str, NULL, 10);

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

// Find a namespace by name (does not create).
// returns: namespace pointer, or NULL if not found
// name: namespace name
userns_t *
userns_find(const char *name)
{
  if(!userns_ready || name == NULL)
    return(NULL);

  pthread_mutex_lock(&userns_mutex);
  userns_t *ns = find_locked(name);
  pthread_mutex_unlock(&userns_mutex);

  return(ns);
}

// Find a namespace by DB id.
// returns: namespace pointer, or NULL if not found
// id: database primary key
userns_t *
userns_find_id(uint32_t id)
{
  if(!userns_ready)
    return(NULL);

  pthread_mutex_lock(&userns_mutex);
  userns_t *ns = find_id_locked(id);
  pthread_mutex_unlock(&userns_mutex);

  return(ns);
}

// Delete a namespace and all its users, groups, and memberships.
// returns: SUCCESS or FAIL
// name: namespace name
bool
userns_delete(const char *name)
{
  if(!userns_ready || name == NULL)
    return(FAIL);

  pthread_mutex_lock(&userns_mutex);
  userns_t *ns = find_locked(name);

  if(ns == NULL)
  {
    pthread_mutex_unlock(&userns_mutex);
    clam(CLAM_WARN, "userns_delete", "namespace '%s' not found", name);
    return(FAIL);
  }

  uint32_t id = ns->id;

  pthread_mutex_unlock(&userns_mutex);

  // Delete from database (CASCADE removes users, groups, memberships).
  char sql[128];

  snprintf(sql, sizeof(sql), "DELETE FROM userns WHERE id = %u", id);

  db_result_t *r = db_result_alloc();

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

// returns: number of loaded namespaces
uint32_t
userns_count(void)
{
  pthread_mutex_lock(&userns_mutex);
  uint32_t n = userns_total;
  pthread_mutex_unlock(&userns_mutex);

  return(n);
}

// -----------------------------------------------------------------------
// Password policy
// -----------------------------------------------------------------------

// Validate a password against the password policy.
// Requirement: minimum length (core.userns.pass_min, default 8).
// returns: SUCCESS if password meets all requirements, FAIL otherwise
// password: candidate password to check
bool
userns_password_check(const char *password)
{
  if(password == NULL)
    return(FAIL);

  size_t len = strlen(password);

  if(len < userns_cfg.pass_min || len > userns_cfg.pass_max)
    return(FAIL);

  return(SUCCESS);
}

// -----------------------------------------------------------------------
// User operations
// -----------------------------------------------------------------------

// Create a user in a namespace.
// returns: SUCCESS or FAIL
// ns: target namespace
// username: alphanumeric, max USERNS_USER_SZ-1 chars
// password: must meet password policy requirements
bool
userns_user_create(const userns_t *ns, const char *username,
    const char *password)
{
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
  char encoded[ARGON2_ENC_LEN];

  if(hash_password(password, encoded, sizeof(encoded)) != SUCCESS)
    return(FAIL);

  // Generate UUID for the new user.
  uuid_t uu;
  char uuid_str[USERNS_UUID_SZ];

  uuid_generate(uu);
  uuid_unparse_lower(uu, uuid_str);

  // Escape inputs.
  char *esc_user = db_escape(username);
  char *esc_hash = db_escape(encoded);

  if(esc_user == NULL || esc_hash == NULL)
  {
    if(esc_user != NULL) mem_free(esc_user);
    if(esc_hash != NULL) mem_free(esc_hash);
    return(FAIL);
  }

  char sql[512];

  snprintf(sql, sizeof(sql),
      "INSERT INTO userns_user (ns_id, username, passhash, uuid) "
      "VALUES (%u, '%s', '%s', '%s')",
      ns->id, esc_user, esc_hash, uuid_str);

  mem_free(esc_user);
  mem_free(esc_hash);

  db_result_t *r = db_result_alloc();

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

// Create a user without a password (for user discovery).
// returns: SUCCESS or FAIL
bool
userns_user_create_nopass(const userns_t *ns, const char *username)
{
  if(!userns_ready || ns == NULL)
    return(FAIL);

  if(validate_username(username) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_user_create_nopass",
        "invalid username '%s'", username ? username : "(null)");
    return(FAIL);
  }

  // Generate UUID.
  uuid_t uu;
  char uuid_str[USERNS_UUID_SZ];

  uuid_generate(uu);
  uuid_unparse_lower(uu, uuid_str);

  // Escape username.
  char *esc_user = db_escape(username);

  if(esc_user == NULL)
    return(FAIL);

  char sql[512];

  snprintf(sql, sizeof(sql),
      "INSERT INTO userns_user (ns_id, username, passhash, uuid) "
      "VALUES (%u, '%s', '', '%s')",
      ns->id, esc_user, uuid_str);

  mem_free(esc_user);

  db_result_t *r = db_result_alloc();

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

// Delete a user from a namespace.
// returns: SUCCESS or FAIL
// ns: target namespace
// username: user to delete
bool
userns_user_delete(const userns_t *ns, const char *username)
{
  if(!userns_ready || ns == NULL || username == NULL)
    return(FAIL);

  char *esc_user = db_escape(username);

  if(esc_user == NULL)
    return(FAIL);

  char sql[256];

  snprintf(sql, sizeof(sql),
      "DELETE FROM userns_user WHERE ns_id = %u AND username = '%s'",
      ns->id, esc_user);

  mem_free(esc_user);

  db_result_t *r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_user_delete",
        "cannot delete user '%s' from '%s': %s",
        username, ns->name, r->error);
    db_result_free(r);
    return(FAIL);
  }

  bool deleted = (r->rows_affected > 0);

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

// Check if a user exists in a namespace.
// returns: true if user exists
// ns: target namespace
// username: user to check
bool
userns_user_exists(const userns_t *ns, const char *username)
{
  if(!userns_ready || ns == NULL || username == NULL)
    return(false);

  char *esc_user = db_escape(username);

  if(esc_user == NULL)
    return(false);

  char sql[256];

  snprintf(sql, sizeof(sql),
      "SELECT 1 FROM userns_user WHERE ns_id = %u AND username = '%s'",
      ns->id, esc_user);

  mem_free(esc_user);

  db_result_t *r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    db_result_free(r);
    return(false);
  }

  bool exists = (r->rows > 0);

  db_result_free(r);

  return(exists);
}

// Change a user's password.
// returns: SUCCESS or FAIL
// ns: target namespace
// username: user whose password to change
// old_password: current password (must verify)
// new_password: replacement password (must meet policy)
bool
userns_user_set_password(const userns_t *ns, const char *username,
    const char *old_password, const char *new_password)
{
  if(!userns_ready || ns == NULL || username == NULL)
    return(FAIL);

  // Verify old password first.
  userns_auth_t result = userns_auth(ns, username, old_password, NULL);

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
  char encoded[ARGON2_ENC_LEN];

  if(hash_password(new_password, encoded, sizeof(encoded)) != SUCCESS)
    return(FAIL);

  // Update in database.
  char *esc_user = db_escape(username);
  char *esc_hash = db_escape(encoded);

  if(esc_user == NULL || esc_hash == NULL)
  {
    if(esc_user != NULL) mem_free(esc_user);
    if(esc_hash != NULL) mem_free(esc_hash);
    return(FAIL);
  }

  char sql[512];

  snprintf(sql, sizeof(sql),
      "UPDATE userns_user SET passhash = '%s' "
      "WHERE ns_id = %u AND username = '%s'",
      esc_hash, ns->id, esc_user);

  mem_free(esc_user);
  mem_free(esc_hash);

  db_result_t *r = db_result_alloc();

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

// -----------------------------------------------------------------------
// Authentication
// -----------------------------------------------------------------------

// Authenticate a user against stored credentials.
// returns: USERNS_AUTH_OK on success, or an error code
// ns: target namespace
// username: user attempting to authenticate
// password: cleartext password to verify
// method_ctx: method-contributed context string, or NULL
userns_auth_t
userns_auth(const userns_t *ns, const char *username,
    const char *password, const char *method_ctx)
{
  if(!userns_ready || ns == NULL || username == NULL || password == NULL)
    return(USERNS_AUTH_ERR);

  // Fetch the stored hash from the database.
  char *esc_user = db_escape(username);

  if(esc_user == NULL)
    return(USERNS_AUTH_ERR);

  char sql[256];

  snprintf(sql, sizeof(sql),
      "SELECT passhash FROM userns_user "
      "WHERE ns_id = %u AND username = '%s'",
      ns->id, esc_user);

  mem_free(esc_user);

  db_result_t *r = db_result_alloc();

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
    clam(CLAM_DEBUG, "userns_auth",
        "user '%s' not found in '%s'", username, ns->name);
    return(USERNS_AUTH_BAD_USER);
  }

  const char *stored_hash = db_result_get(r, 0, 0);

  if(stored_hash == NULL || stored_hash[0] == '\0')
  {
    db_result_free(r);
    clam(CLAM_WARN, "userns_auth",
        "user '%s' in '%s' has no password set", username, ns->name);
    return(USERNS_AUTH_NO_HASH);
  }

  // Copy hash before freeing result (pointer invalidated by free).
  char *hash_copy = mem_strdup("userns", "hash", stored_hash);

  db_result_free(r);

  // Verify password against stored hash.
  bool match = verify_password(hash_copy, password);

  mem_free(hash_copy);

  if(match != SUCCESS)
  {
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

// -----------------------------------------------------------------------
// Group operations
// -----------------------------------------------------------------------

// Validate a group name: alphanumeric only, 1..USERNS_GROUP_SZ-1.
// returns: SUCCESS or FAIL
// name: candidate group name
static bool
validate_groupname(const char *name)
{
  if(name == NULL || name[0] == '\0')
    return(FAIL);

  size_t len = 0;

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

// Create a group in a namespace.
// returns: SUCCESS or FAIL
// ns: target namespace
// name: alphanumeric group name, max USERNS_GROUP_SZ-1 chars
bool
userns_group_create(const userns_t *ns, const char *name)
{
  if(!userns_ready || ns == NULL)
    return(FAIL);

  if(validate_groupname(name) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_group_create",
        "invalid group name '%s'", name ? name : "(null)");
    return(FAIL);
  }

  char *esc_name = db_escape(name);

  if(esc_name == NULL)
    return(FAIL);

  char sql[256];

  snprintf(sql, sizeof(sql),
      "INSERT INTO userns_group (ns_id, name) VALUES (%u, '%s')",
      ns->id, esc_name);

  mem_free(esc_name);

  db_result_t *r = db_result_alloc();

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

// Delete a group from a namespace.
// returns: SUCCESS or FAIL
// ns: target namespace
// name: group to delete
bool
userns_group_delete(const userns_t *ns, const char *name)
{
  if(!userns_ready || ns == NULL || name == NULL)
    return(FAIL);

  if(userns_group_is_builtin(name))
  {
    clam(CLAM_WARN, "userns_group_delete",
        "cannot delete built-in group '%s'", name);
    return(FAIL);
  }

  char *esc_name = db_escape(name);

  if(esc_name == NULL)
    return(FAIL);

  char sql[256];

  snprintf(sql, sizeof(sql),
      "DELETE FROM userns_group WHERE ns_id = %u AND name = '%s'",
      ns->id, esc_name);

  mem_free(esc_name);

  db_result_t *r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_group_delete",
        "cannot delete group '%s' from '%s': %s",
        name, ns->name, r->error);
    db_result_free(r);
    return(FAIL);
  }

  bool deleted = (r->rows_affected > 0);

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

// Check if a group exists in a namespace.
// returns: true if group exists
// ns: target namespace
// name: group to check
bool
userns_group_exists(const userns_t *ns, const char *name)
{
  if(!userns_ready || ns == NULL || name == NULL)
    return(false);

  char *esc_name = db_escape(name);

  if(esc_name == NULL)
    return(false);

  char sql[256];

  snprintf(sql, sizeof(sql),
      "SELECT 1 FROM userns_group WHERE ns_id = %u AND name = '%s'",
      ns->id, esc_name);

  mem_free(esc_name);

  db_result_t *r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    db_result_free(r);
    return(false);
  }

  bool exists = (r->rows > 0);

  db_result_free(r);

  return(exists);
}

// -----------------------------------------------------------------------
// Membership operations
// -----------------------------------------------------------------------

// Add a user to a group. Both must exist in the same namespace.
// returns: SUCCESS or FAIL
// ns: target namespace
// username: user to add
// group: group to add user to
bool
userns_member_add(const userns_t *ns, const char *username,
    const char *group, uint16_t level)
{
  if(!userns_ready || ns == NULL || username == NULL || group == NULL)
    return(FAIL);

  char *esc_user = db_escape(username);
  char *esc_group = db_escape(group);

  if(esc_user == NULL || esc_group == NULL)
  {
    if(esc_user != NULL) mem_free(esc_user);
    if(esc_group != NULL) mem_free(esc_group);
    return(FAIL);
  }

  char sql[512];

  snprintf(sql, sizeof(sql),
      "INSERT INTO userns_member (user_id, group_id, level) "
      "SELECT u.id, g.id, %u "
      "FROM userns_user u, userns_group g "
      "WHERE u.ns_id = %u AND u.username = '%s' "
      "AND g.ns_id = %u AND g.name = '%s'",
      (unsigned)level, ns->id, esc_user, ns->id, esc_group);

  mem_free(esc_user);
  mem_free(esc_group);

  db_result_t *r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_member_add",
        "cannot add '%s' to group '%s' in '%s': %s",
        username, group, ns->name, r->error);
    db_result_free(r);
    return(FAIL);
  }

  bool added = (r->rows_affected > 0);

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

// Remove a user from a group.
// returns: SUCCESS or FAIL
// ns: target namespace
// username: user to remove
// group: group to remove user from
bool
userns_member_remove(const userns_t *ns, const char *username,
    const char *group)
{
  if(!userns_ready || ns == NULL || username == NULL || group == NULL)
    return(FAIL);

  char *esc_user = db_escape(username);
  char *esc_group = db_escape(group);

  if(esc_user == NULL || esc_group == NULL)
  {
    if(esc_user != NULL) mem_free(esc_user);
    if(esc_group != NULL) mem_free(esc_group);
    return(FAIL);
  }

  char sql[512];

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

  db_result_t *r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_member_remove",
        "cannot remove '%s' from group '%s' in '%s': %s",
        username, group, ns->name, r->error);
    db_result_free(r);
    return(FAIL);
  }

  bool removed = (r->rows_affected > 0);

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

// Get a user's privilege level in a group.
// returns: level (>= 0) if member, -1 if not a member or error
// ns: target namespace
// username: user to check
// group: group to check membership in
int32_t
userns_member_level(const userns_t *ns, const char *username,
    const char *group)
{
  if(!userns_ready || ns == NULL || username == NULL || group == NULL)
    return(-1);

  char *esc_user = db_escape(username);
  char *esc_group = db_escape(group);

  if(esc_user == NULL || esc_group == NULL)
  {
    if(esc_user != NULL) mem_free(esc_user);
    if(esc_group != NULL) mem_free(esc_group);
    return(-1);
  }

  char sql[512];

  snprintf(sql, sizeof(sql),
      "SELECT m.level FROM userns_member m "
      "JOIN userns_user u ON m.user_id = u.id "
      "JOIN userns_group g ON m.group_id = g.id "
      "WHERE u.ns_id = %u AND u.username = '%s' "
      "AND g.ns_id = %u AND g.name = '%s'",
      ns->id, esc_user, ns->id, esc_group);

  mem_free(esc_user);
  mem_free(esc_group);

  db_result_t *r = db_result_alloc();

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

  const char *val = db_result_get(r, 0, 0);
  int32_t level = (val != NULL) ? (int32_t)atoi(val) : 0;

  db_result_free(r);

  return(level);
}

// Check if a user is a member of a group (convenience wrapper).
// returns: true if user is a member of the group
// ns: target namespace
// username: user to check
// group: group to check membership in
bool
userns_member_check(const userns_t *ns, const char *username,
    const char *group)
{
  return(userns_member_level(ns, username, group) >= 0);
}

// Create a group in a namespace with a description.
// returns: SUCCESS or FAIL
// ns: target namespace
// name: alphanumeric group name, max USERNS_GROUP_SZ-1 chars
// description: group description, max USERNS_DESC_SZ-1 chars (or NULL)
bool
userns_group_create_desc(const userns_t *ns, const char *name,
    const char *description)
{
  if(!userns_ready || ns == NULL)
    return(FAIL);

  if(validate_groupname(name) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_group_create_desc",
        "invalid group name '%s'", name ? name : "(null)");
    return(FAIL);
  }

  char *esc_name = db_escape(name);

  if(esc_name == NULL)
    return(FAIL);

  const char *desc = (description != NULL) ? description : "";
  char *esc_desc = db_escape(desc);

  if(esc_desc == NULL)
  {
    mem_free(esc_name);
    return(FAIL);
  }

  char sql[512];

  snprintf(sql, sizeof(sql),
      "INSERT INTO userns_group (ns_id, name, description) "
      "VALUES (%u, '%s', '%s')",
      ns->id, esc_name, esc_desc);

  mem_free(esc_name);
  mem_free(esc_desc);

  db_result_t *r = db_result_alloc();

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

// Check if a group name is one of the four built-in groups.
// returns: true if name is owner, admin, user, or everyone
// name: group name to check
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

// Update a user's privilege level in a group.
// returns: SUCCESS or FAIL (not a member, DB error)
// ns: target namespace
// username: user whose level to update
// group: group to update level in
// level: new privilege level
bool
userns_member_set_level(const userns_t *ns, const char *username,
    const char *group, uint16_t level)
{
  if(!userns_ready || ns == NULL || username == NULL || group == NULL)
    return(FAIL);

  char *esc_user = db_escape(username);
  char *esc_group = db_escape(group);

  if(esc_user == NULL || esc_group == NULL)
  {
    if(esc_user != NULL) mem_free(esc_user);
    if(esc_group != NULL) mem_free(esc_group);
    return(FAIL);
  }

  char sql[512];

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

  db_result_t *r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS)
  {
    clam(CLAM_WARN, "userns_member_set_level",
        "cannot update level for '%s' in group '%s': %s",
        username, group, r->error);
    db_result_free(r);
    return(FAIL);
  }

  bool updated = (r->rows_affected > 0);

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

// -----------------------------------------------------------------------
// Admin password reset
// -----------------------------------------------------------------------

// Reset a user's password without requiring the old password.
// Intended for administrative use only. New password must meet policy.
// returns: SUCCESS or FAIL
// ns: target namespace
// username: user whose password to reset
// new_password: replacement password (must meet policy)
bool
userns_user_reset_password(const userns_t *ns, const char *username,
    const char *new_password)
{
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

  char encoded[ARGON2_ENC_LEN];

  if(hash_password(new_password, encoded, sizeof(encoded)) != SUCCESS)
    return(FAIL);

  char *esc_user = db_escape(username);
  char *esc_hash = db_escape(encoded);

  if(esc_user == NULL || esc_hash == NULL)
  {
    if(esc_user != NULL) mem_free(esc_user);
    if(esc_hash != NULL) mem_free(esc_hash);
    return(FAIL);
  }

  char sql[512];

  snprintf(sql, sizeof(sql),
      "UPDATE userns_user SET passhash = '%s' "
      "WHERE ns_id = %u AND username = '%s'",
      esc_hash, ns->id, esc_user);

  mem_free(esc_user);
  mem_free(esc_hash);

  db_result_t *r = db_result_alloc();

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

// Get the DB user id for a user in a namespace.
// returns: user id, or 0 on failure
// ns: target namespace
// username: user to look up
uint32_t
userns_get_user_id(const userns_t *ns, const char *username)
{
  char *esc_user = db_escape(username);

  if(esc_user == NULL)
    return(0);

  char sql[256];

  snprintf(sql, sizeof(sql),
      "SELECT id FROM userns_user WHERE ns_id = %u AND username = '%s'",
      ns->id, esc_user);

  mem_free(esc_user);

  db_result_t *r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS || r->rows == 0)
  {
    db_result_free(r);
    return(0);
  }

  const char *id_str = db_result_get(r, 0, 0);
  uint32_t id = (id_str != NULL) ? (uint32_t)strtoul(id_str, NULL, 10) : 0;

  db_result_free(r);

  return(id);
}

// -----------------------------------------------------------------------
// KV configuration
// -----------------------------------------------------------------------

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

// KV change callback. Reloads configuration when any userns key changes.
// key: changed KV key (unused)
// data: opaque user data (unused)
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
  kv_register("core.userns.pass_min", KV_UINT32, "8",   userns_kv_changed, NULL);
  kv_register("core.userns.pass_max", KV_UINT32, "128", userns_kv_changed, NULL);
  kv_register("core.userns.max_mfa",  KV_UINT32, "10",  userns_kv_changed, NULL);
  userns_load_config();
}
