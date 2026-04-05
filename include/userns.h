#ifndef BM_USERNS_H
#define BM_USERNS_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define USERNS_NAME_SZ  64
#define USERNS_USER_SZ  31        // 30-char alphanumeric username + null
#define USERNS_GROUP_SZ 31        // 30-char alphanumeric group name + null
#define USERNS_DESC_SZ  101       // 100-char description + null
#define USERNS_MFA_SZ   101       // 100-char legacy MFA field + null
#define USERNS_MFA_PATTERN_SZ 201 // 200-char MFA pattern + null
#define USERNS_UUID_SZ  37        // 36-char UUID + null
#define USERNS_PASS_SZ  101       // 100-char passphrase + null

// Password policy default constraints (effective values loaded from KV).
#define USERNS_PASS_MIN_DEFAULT  10
#define USERNS_PASS_MAX_DEFAULT  128

// Method context size for second-factor data (e.g., hostname/IP).
#define USERNS_MCTX_SZ     256

// Built-in group names. These are created automatically when a
// namespace is first loaded or created, and cannot be deleted.
#define USERNS_GROUP_OWNER    "owner"
#define USERNS_GROUP_ADMIN    "admin"
#define USERNS_GROUP_USER     "user"
#define USERNS_GROUP_EVERYONE "everyone"

// Hard-coded owner user. The '@' prefix prevents collision with
// normal alphanumeric usernames. This user bypasses all permission
// checks and is a member of all built-in groups with level 65535.
#define USERNS_OWNER_USER   "@owner"
#define USERNS_OWNER_LEVEL  65535

// Auth result codes.
typedef enum
{
  USERNS_AUTH_OK,             // authentication succeeded
  USERNS_AUTH_BAD_USER,       // username not found
  USERNS_AUTH_BAD_PASS,       // password mismatch
  USERNS_AUTH_NO_HASH,        // user has no password set
  USERNS_AUTH_ERR             // internal error (DB, hashing, etc.)
} userns_auth_t;

// A loaded user namespace.
typedef struct userns
{
  uint32_t      id;                   // DB primary key
  char          name[USERNS_NAME_SZ]; // unique namespace name
  time_t        created;
  void         *mfa_cache;            // opaque MFA cache (userns_cache_t *)
  struct userns *next;                // linked list chain
} userns_t;

// Initialize the user namespace subsystem. Must be called after db_init().
// Loads all existing namespaces from the database.
// returns: SUCCESS or FAIL
bool userns_init(void);

// Shut down the user namespace subsystem. Frees all in-memory records.
void userns_exit(void);

// Register KV configuration keys and load values. Must be called
// after kv_init() and kv_load().
void userns_register_config(void);

// -----------------------------------------------------------------------
// Namespace operations
// -----------------------------------------------------------------------

// Find or create a namespace by name.
// If the namespace does not exist, it is created in the database and loaded.
// returns: namespace pointer, or NULL on failure
// name: namespace name (alphanumeric, max USERNS_NAME_SZ-1 chars)
userns_t *userns_get(const char *name);

// Find a namespace by name (does not create).
// returns: namespace pointer, or NULL if not found
// name: namespace name
userns_t *userns_find(const char *name);

// Find a namespace by DB id.
// returns: namespace pointer, or NULL if not found
// id: database primary key
userns_t *userns_find_id(uint32_t id);

// Delete a namespace and all its users, groups, and memberships.
// The namespace is removed from the database (CASCADE) and freed.
// returns: SUCCESS or FAIL
// name: namespace name
bool userns_delete(const char *name);

// returns: number of loaded namespaces
uint32_t userns_count(void);

// -----------------------------------------------------------------------
// User operations
// -----------------------------------------------------------------------

// Create a user in a namespace. Password is validated against policy
// and hashed with argon2id before storage.
// returns: SUCCESS or FAIL (invalid username, bad password, duplicate, DB error)
// ns: target namespace
// username: alphanumeric, max USERNS_USER_SZ-1 chars
// password: must meet password policy requirements
bool userns_user_create(const userns_t *ns, const char *username,
    const char *password);

// Create a user in a namespace without a password. The user must later
// set a password via userns_user_set_password() or admin reset.
// Intended for user discovery: auto-created accounts from MFA patterns.
// returns: SUCCESS or FAIL (invalid username, duplicate, DB error)
// ns: target namespace
// username: alphanumeric, max USERNS_USER_SZ-1 chars
bool userns_user_create_nopass(const userns_t *ns, const char *username);

// Delete a user from a namespace. Also removes group memberships (CASCADE).
// returns: SUCCESS or FAIL
// ns: target namespace
// username: user to delete
bool userns_user_delete(const userns_t *ns, const char *username);

// Check if a user exists in a namespace.
// returns: true if user exists
// ns: target namespace
// username: user to check
bool userns_user_exists(const userns_t *ns, const char *username);

// Change a user's password. Old password must verify first.
// New password is validated against policy and re-hashed.
// returns: SUCCESS or FAIL
// ns: target namespace
// username: user whose password to change
// old_password: current password (must verify)
// new_password: replacement password (must meet policy)
bool userns_user_set_password(const userns_t *ns, const char *username,
    const char *old_password, const char *new_password);

// -----------------------------------------------------------------------
// Authentication
// -----------------------------------------------------------------------

// Authenticate a user. Verifies username/password against stored hash.
// method_ctx is optional context contributed by the method plugin
// (e.g., hostname/IP from IRC) for logging and future MFA use.
// returns: USERNS_AUTH_OK on success, or an error code
// ns: target namespace
// username: user attempting to authenticate
// password: cleartext password to verify
// method_ctx: method-contributed context string, or NULL
userns_auth_t userns_auth(const userns_t *ns, const char *username,
    const char *password, const char *method_ctx);

// -----------------------------------------------------------------------
// Password policy
// -----------------------------------------------------------------------

// Validate a password against the password policy.
// returns: SUCCESS if password meets all requirements, FAIL otherwise
// password: candidate password to check
bool userns_password_check(const char *password);

// -----------------------------------------------------------------------
// Group operations
// -----------------------------------------------------------------------

// Create a group in a namespace.
// returns: SUCCESS or FAIL (invalid name, duplicate, DB error)
// ns: target namespace
// name: alphanumeric group name, max USERNS_GROUP_SZ-1 chars
bool userns_group_create(const userns_t *ns, const char *name);

// Create a group in a namespace with a description.
// returns: SUCCESS or FAIL (invalid name, duplicate, DB error)
// ns: target namespace
// name: alphanumeric group name, max USERNS_GROUP_SZ-1 chars
// description: group description, max USERNS_DESC_SZ-1 chars (or NULL)
bool userns_group_create_desc(const userns_t *ns, const char *name,
    const char *description);

// Check if a group name is one of the four built-in groups.
// returns: true if name is owner, admin, user, or everyone
// name: group name to check
bool userns_group_is_builtin(const char *name);

// Delete a group from a namespace. Also removes all memberships (CASCADE).
// Built-in groups (owner, admin, user, everyone) cannot be deleted.
// returns: SUCCESS or FAIL
// ns: target namespace
// name: group to delete
bool userns_group_delete(const userns_t *ns, const char *name);

// Check if a group exists in a namespace.
// returns: true if group exists
// ns: target namespace
// name: group to check
bool userns_group_exists(const userns_t *ns, const char *name);

// -----------------------------------------------------------------------
// Membership operations
// -----------------------------------------------------------------------

// Add a user to a group with a privilege level. Both must exist in the
// same namespace. Higher levels grant access to more commands.
// returns: SUCCESS or FAIL (user/group not found, already member, DB error)
// ns: target namespace
// username: user to add
// group: group to add user to
// level: privilege level (0-65535, higher = more access)
bool userns_member_add(const userns_t *ns, const char *username,
    const char *group, uint16_t level);

// Remove a user from a group.
// returns: SUCCESS or FAIL (not a member, DB error)
// ns: target namespace
// username: user to remove
// group: group to remove user from
bool userns_member_remove(const userns_t *ns, const char *username,
    const char *group);

// Get a user's privilege level in a group. This is the primary
// authorization primitive used by the command system.
// returns: level (>= 0) if member, -1 if not a member or error
// ns: target namespace
// username: user to check
// group: group to check membership in
int32_t userns_member_level(const userns_t *ns, const char *username,
    const char *group);

// Check if a user is a member of a group (convenience wrapper).
// Equivalent to: userns_member_level(ns, username, group) >= 0
// returns: true if user is a member of the group
// ns: target namespace
// username: user to check
// group: group to check membership in
bool userns_member_check(const userns_t *ns, const char *username,
    const char *group);

// Update a user's privilege level in a group. The user must already
// be a member.
// returns: SUCCESS or FAIL (not a member, DB error)
// ns: target namespace
// username: user whose level to update
// group: group to update level in
// level: new privilege level
bool userns_member_set_level(const userns_t *ns, const char *username,
    const char *group, uint16_t level);

// -----------------------------------------------------------------------
// User profile fields
// -----------------------------------------------------------------------

// Set a user's description.
// returns: SUCCESS or FAIL
// ns: target namespace
// username: user to update
// description: new description (max USERNS_DESC_SZ-1 chars)
bool userns_user_set_description(const userns_t *ns, const char *username,
    const char *description);

// Set a user's legacy MFA field (single value, deprecated).
// Use userns_user_add_mfa() for the new multi-MFA system.
// returns: SUCCESS or FAIL
// ns: target namespace
// username: user to update
// mfa: new MFA value (max USERNS_MFA_SZ-1 chars)
bool userns_user_set_mfa(const userns_t *ns, const char *username,
    const char *mfa);

// -----------------------------------------------------------------------
// Multi-MFA pattern management
// -----------------------------------------------------------------------

// Add an MFA pattern for a user. The pattern must be in the format
// handle!username@hostname where glob characters (* and ?) are
// allowed in the handle and hostname portions. Security constraints:
// - handle must contain at least 3 non-glob characters
// - hostname must contain at least 6 non-glob characters
// - all-glob patterns are rejected
// Maximum patterns per user is configurable (core.userns.max_mfa).
// returns: SUCCESS or FAIL
// ns: target namespace
// username: user to add the pattern to
// pattern: MFA pattern string (max USERNS_MFA_PATTERN_SZ-1 chars)
bool userns_user_add_mfa(const userns_t *ns, const char *username,
    const char *pattern);

// Remove an MFA pattern from a user.
// returns: SUCCESS or FAIL (pattern not found)
// ns: target namespace
// username: user to remove the pattern from
// pattern: exact MFA pattern string to remove
bool userns_user_remove_mfa(const userns_t *ns, const char *username,
    const char *pattern);

// Callback for MFA pattern iteration.
// pattern: MFA pattern string
// data: opaque user data
typedef void (*userns_mfa_iter_cb_t)(const char *pattern, void *data);

// Iterate all MFA patterns for a user.
// ns: target namespace
// username: user whose patterns to list
// cb: callback invoked for each pattern
// data: opaque user data passed to cb
void userns_user_list_mfa(const userns_t *ns, const char *username,
    userns_mfa_iter_cb_t cb, void *data);

// Match an MFA string against all patterns in a namespace.
// The input is a raw MFA string in the format handle!username@hostname
// (e.g., "doc!docdrow@laptop.theshearerfamily.com"). Each user's MFA
// patterns are checked. The username component of the pattern must
// match exactly; handle and hostname use glob matching (* and ?).
// First match wins.
// returns: matching username (static buffer, valid until next call), or NULL
// ns: target namespace
// mfa_string: raw MFA string to match
const char *userns_mfa_match(const userns_t *ns, const char *mfa_string);

// Set a user's passphrase.
// returns: SUCCESS or FAIL
// ns: target namespace
// username: user to update
// passphrase: new passphrase (max USERNS_PASS_SZ-1 chars)
bool userns_user_set_passphrase(const userns_t *ns, const char *username,
    const char *passphrase);

// -----------------------------------------------------------------------
// Listing / iteration
// -----------------------------------------------------------------------

// Callback for user iteration.
// username: user's name
// uuid: user's UUID string (36 chars, never NULL)
// description: user's description (may be empty, never NULL)
// data: opaque user data
typedef void (*userns_user_iter_cb_t)(const char *username,
    const char *uuid, const char *description, void *data);

// Iterate all users in a namespace.
// ns: target namespace
// cb: callback invoked for each user
// data: opaque user data passed to cb
void userns_user_iterate(const userns_t *ns,
    userns_user_iter_cb_t cb, void *data);

// Callback for group iteration.
// name: group name
// description: group description (may be empty, never NULL)
// data: opaque user data
typedef void (*userns_group_iter_cb_t)(const char *name,
    const char *description, void *data);

// Iterate all groups in a namespace.
// ns: target namespace
// cb: callback invoked for each group
// data: opaque user data passed to cb
void userns_group_iterate(const userns_t *ns,
    userns_group_iter_cb_t cb, void *data);

// Callback for membership iteration.
// group: group name
// level: privilege level in the group
// data: opaque user data
typedef void (*userns_membership_iter_cb_t)(const char *group,
    uint16_t level, void *data);

// Iterate all group memberships for a user in a namespace.
// ns: target namespace
// username: user whose memberships to list
// cb: callback invoked for each membership
// data: opaque user data passed to cb
void userns_membership_iterate(const userns_t *ns, const char *username,
    userns_membership_iter_cb_t cb, void *data);

// -----------------------------------------------------------------------
// Admin password reset
// -----------------------------------------------------------------------

// Reset a user's password without requiring the old password.
// Intended for administrative use only. New password must meet policy.
// returns: SUCCESS or FAIL
// ns: target namespace
// username: user whose password to reset
// new_password: replacement password (must meet policy)
bool userns_user_reset_password(const userns_t *ns, const char *username,
    const char *new_password);

// -----------------------------------------------------------------------
// Namespace iteration
// -----------------------------------------------------------------------

// Callback for namespace name iteration.
// name: namespace name string
// data: opaque user data
typedef void (*userns_iter_cb_t)(const char *name, void *data);

// Iterate all loaded namespace names.
// cb: callback invoked for each namespace
// data: opaque user data passed to cb
void userns_iterate(userns_iter_cb_t cb, void *data);

// -----------------------------------------------------------------------
// Owner identity
// -----------------------------------------------------------------------

// Check if a username is the system owner.
// returns: true if username matches USERNS_OWNER_USER
// username: username to check (NULL returns false)
bool userns_is_owner(const char *username);

#ifdef USERNS_INTERNAL

#include "common.h"
#include "clam.h"
#include "db.h"
#include "kv.h"
#include "mem.h"

#include <argon2.h>
#include <uuid/uuid.h>

// Shared internal state — defined in userns.c, used across split files.
extern userns_t        *userns_list;
extern pthread_mutex_t  userns_mutex;
extern uint32_t         userns_total;
extern bool             userns_ready;

// Argon2id parameters.
#define ARGON2_T_COST    3          // iterations
#define ARGON2_M_COST    65536      // 64 MiB memory
#define ARGON2_P_COST    1          // parallelism
#define ARGON2_SALT_LEN  16         // salt bytes
#define ARGON2_HASH_LEN  32         // hash output bytes
#define ARGON2_ENC_LEN   256        // encoded string buffer

// Cached configuration (loaded from KV).
typedef struct
{
  uint32_t pass_min;
  uint32_t pass_max;
  uint32_t max_mfa;
} userns_cfg_t;

// Default max MFA patterns per user.
#define USERNS_MAX_MFA_DEFAULT  10

// Shared configuration — defined in userns.c.
extern userns_cfg_t userns_cfg;

// -----------------------------------------------------------------------
// In-memory MFA cache structures
// -----------------------------------------------------------------------

#define MFA_CACHE_BUCKETS  64

// A single cached MFA pattern entry.
typedef struct mfa_cache_entry
{
  char                     pattern[USERNS_MFA_PATTERN_SZ];
  char                     username[USERNS_USER_SZ];
  char                     uuid[USERNS_UUID_SZ];
  struct mfa_cache_entry  *next;     // hash chain
} mfa_cache_entry_t;

// Per-namespace MFA cache.
typedef struct
{
  mfa_cache_entry_t  *buckets[MFA_CACHE_BUCKETS];
  uint32_t            count;         // total entries
  pthread_rwlock_t    lock;
} userns_cache_t;

// -----------------------------------------------------------------------
// Internal functions shared across userns_*.c files
// -----------------------------------------------------------------------

// Cache lifecycle (userns_cache.c).
userns_cache_t *userns_cache_create(void);
void userns_cache_destroy(userns_cache_t *c);
void userns_cache_clear(userns_cache_t *c);
void userns_cache_populate(userns_cache_t *c, uint32_t ns_id);
void userns_cache_invalidate(const userns_t *ns);
void userns_cache_ensure(userns_t *ns);

// DB user id lookup (userns.c).
uint32_t userns_get_user_id(const userns_t *ns, const char *username);

#endif // USERNS_INTERNAL

#endif // BM_USERNS_H
