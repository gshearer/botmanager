#ifndef BM_USERNS_H
#define BM_USERNS_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define USERNS_NAME_SZ  64
#define USERNS_USER_SZ  31        // 30-char alphanumeric username + null
#define USERNS_GROUP_SZ 31        // 30-char alphanumeric group name + null
#define USERNS_DESC_SZ  101       // 100-char description + null
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

typedef enum
{
  USERNS_AUTH_OK,             // authentication succeeded
  USERNS_AUTH_BAD_USER,       // username not found
  USERNS_AUTH_BAD_PASS,       // password mismatch
  USERNS_AUTH_NO_HASH,        // user has no password set
  USERNS_AUTH_ERR             // internal error (DB, hashing, etc.)
} userns_auth_t;

typedef struct userns
{
  uint32_t      id;                   // DB primary key
  char          name[USERNS_NAME_SZ]; // unique namespace name
  time_t        created;
  void         *mfa_cache;            // opaque MFA cache (userns_cache_t *)
  struct userns *next;                // linked list chain
} userns_t;

typedef struct
{
  uint32_t namespaces;        // loaded namespaces
  uint32_t users;             // total users across namespaces (from DB)
  uint64_t auth_attempts;     // lifetime identify attempts
  uint64_t auth_failures;     // lifetime failed attempts
  uint64_t mfa_matches;       // lifetime successful MFA auto-matches
  uint64_t discoveries;       // lifetime user discovery events
} userns_stats_t;

void userns_get_stats(userns_stats_t *out);

// Must be called after db_init(). Loads all existing namespaces from
// the database.
bool userns_init(void);

// Frees all in-memory records.
void userns_exit(void);

// Must be called after cmd_init().
void userns_register_commands(void);

// Must be called after kv_init() and kv_load().
void userns_register_config(void);

// Find or create a namespace by name. If the namespace does not exist,
// it is created in the database and loaded.
userns_t *userns_get(const char *name);

// Does not create.
userns_t *userns_find(const char *name);

userns_t *userns_find_id(uint32_t id);

// Deletes namespace and all its users, groups, and memberships
// (CASCADE) and frees it.
bool userns_delete(const char *name);

uint32_t userns_count(void);

// Return the first loaded namespace, or NULL if none. For system-level
// dispatchers (e.g., botmanctl) that lack a bot-bound userns but need
// to resolve identity against a real namespace.
userns_t *userns_first(void);

// Password is validated against policy and hashed with argon2id before
// storage.
bool userns_user_create(const userns_t *ns, const char *username,
    const char *password);

// Create a user without a password. The user must later set a password
// via userns_user_set_password() or admin reset. Intended for user
// discovery: auto-created accounts from MFA patterns.
bool userns_user_create_nopass(const userns_t *ns, const char *username);

// Also removes group memberships (CASCADE).
bool userns_user_delete(const userns_t *ns, const char *username);

bool userns_user_exists(const userns_t *ns, const char *username);

// Old password must verify first. New password is validated against
// policy and re-hashed.
bool userns_user_set_password(const userns_t *ns, const char *username,
    const char *old_password, const char *new_password);

// Verifies username/password against stored hash. method_ctx is
// optional context contributed by the method plugin (e.g., hostname/IP
// from IRC) for logging and future MFA use.
userns_auth_t userns_auth(const userns_t *ns, const char *username,
    const char *password, const char *method_ctx);

bool userns_password_check(const char *password);

bool userns_group_create(const userns_t *ns, const char *name);

bool userns_group_create_desc(const userns_t *ns, const char *name,
    const char *description);

// Returns true if name is owner, admin, user, or everyone.
bool userns_group_is_builtin(const char *name);

// Also removes all memberships (CASCADE). Built-in groups (owner,
// admin, user, everyone) cannot be deleted.
bool userns_group_delete(const userns_t *ns, const char *name);

bool userns_group_set_description(const userns_t *ns, const char *name,
    const char *description);

bool userns_group_exists(const userns_t *ns, const char *name);

// Both user and group must exist in the same namespace. Higher levels
// grant access to more commands. level is 0-65535.
bool userns_member_add(const userns_t *ns, const char *username,
    const char *group, uint16_t level);

bool userns_member_remove(const userns_t *ns, const char *username,
    const char *group);

// Primary authorization primitive used by the command system. Returns
// level (>= 0) if member, -1 if not a member or error.
int32_t userns_member_level(const userns_t *ns, const char *username,
    const char *group);

// Equivalent to: userns_member_level(ns, username, group) >= 0.
bool userns_member_check(const userns_t *ns, const char *username,
    const char *group);

// User must already be a member.
bool userns_member_set_level(const userns_t *ns, const char *username,
    const char *group, uint16_t level);

bool userns_user_set_description(const userns_t *ns, const char *username,
    const char *description);

// Add an MFA pattern for a user. The pattern must be in the format
// handle!username@hostname where glob characters (* and ?) are
// allowed in the handle and hostname portions. Security constraints:
// - handle must contain at least 3 non-glob characters
// - hostname must contain at least 6 non-glob characters
// - all-glob patterns are rejected
// Maximum patterns per user is configurable (core.userns.max_mfa).
bool userns_user_add_mfa(const userns_t *ns, const char *username,
    const char *pattern);

bool userns_user_remove_mfa(const userns_t *ns, const char *username,
    const char *pattern);

typedef void (*userns_mfa_iter_cb_t)(const char *pattern, void *data);

void userns_user_list_mfa(const userns_t *ns, const char *username,
    userns_mfa_iter_cb_t cb, void *data);

bool userns_user_has_mfa(const userns_t *ns, const char *username);

// Match an MFA string against all patterns in a namespace.
// The input is a raw MFA string in the format handle!username@hostname
// (e.g., "doc!docdrow@laptop.theshearerfamily.com"). Each user's MFA
// patterns are checked. The username component of the pattern must
// match exactly; handle and hostname use glob matching (* and ?).
// First match wins. Returns username (static buffer, valid until next
// call), or NULL.
const char *userns_mfa_match(const userns_t *ns, const char *mfa_string);

// Resolve the working namespace for a command context, mirroring the
// /user cd resolution chain: bot session cd → bot binding for in-bot
// dispatch, or per-client botmanctl session for console dispatch.
// Returns NULL after sending a "no namespace set" error to the caller.
struct cmd_ctx;
userns_t *userns_session_resolve(const struct cmd_ctx *ctx);

bool userns_user_set_passphrase(const userns_t *ns, const char *username,
    const char *passphrase);

// Used by subsystems that need to attach per-user rows (e.g.,
// memory.conversation_log). Returns user id, or 0 if not found.
uint32_t userns_user_id(const userns_t *ns, const char *username);

// Called when any user event is witnessed that matches an MFA pattern.
bool userns_user_touch_lastseen(const userns_t *ns, const char *username,
    const char *method, const char *mfa_string);

// ts_out is set to 0 if never seen.
bool userns_user_get_lastseen(const userns_t *ns, const char *username,
    time_t *ts_out, char *method_out, size_t method_sz,
    char *mfa_out, size_t mfa_sz);

// When enabled, the bot will automatically create a session for a user
// whose MFA pattern matches.
bool userns_user_get_autoidentify(const userns_t *ns,
    const char *username);

bool userns_user_set_autoidentify(const userns_t *ns,
    const char *username, bool value);

// Unlike userns_mfa_match() which searches all users, this checks only
// the patterns belonging to the specified user.
bool userns_user_mfa_match(const userns_t *ns, const char *username,
    const char *mfa_string);

typedef void (*userns_user_iter_cb_t)(const char *username,
    const char *uuid, const char *description, void *data);

void userns_user_iterate(const userns_t *ns,
    userns_user_iter_cb_t cb, void *data);

typedef void (*userns_group_iter_cb_t)(const char *name,
    const char *description, void *data);

void userns_group_iterate(const userns_t *ns,
    userns_group_iter_cb_t cb, void *data);

typedef void (*userns_membership_iter_cb_t)(const char *group,
    uint16_t level, void *data);

void userns_membership_iterate(const userns_t *ns, const char *username,
    userns_membership_iter_cb_t cb, void *data);

typedef void (*userns_group_members_iter_cb_t)(const char *username,
    uint16_t level, void *data);

void userns_group_members_iterate(const userns_t *ns,
    const char *groupname,
    userns_group_members_iter_cb_t cb, void *data);

bool userns_user_get_info(const userns_t *ns, const char *username,
    char *uuid_out, size_t uuid_sz,
    char *desc_out, size_t desc_sz);

// Intended for administrative use only. New password must meet policy.
bool userns_user_reset_password(const userns_t *ns, const char *username,
    const char *new_password);

typedef void (*userns_iter_cb_t)(const char *name, void *data);

void userns_iterate(userns_iter_cb_t cb, void *data);

// Returns true if username matches USERNS_OWNER_USER. NULL returns false.
bool userns_is_owner(const char *username);

#ifdef USERNS_INTERNAL

#include "common.h"
#include "clam.h"
#include "db.h"
#include "kv.h"
#include "alloc.h"

#include <argon2.h>
#include <uuid/uuid.h>

// Shared internal state — defined in userns.c, used across split files.
extern userns_t        *userns_list;
extern pthread_mutex_t  userns_mutex;
extern uint32_t         userns_total;
extern bool             userns_ready;

// Atomic, no lock needed.
extern uint64_t         userns_stat_auth_attempts;
extern uint64_t         userns_stat_auth_failures;
extern uint64_t         userns_stat_mfa_matches;
extern uint64_t         userns_stat_discoveries;

// Argon2id parameters.
#define ARGON2_T_COST    3          // iterations
#define ARGON2_M_COST    65536      // 64 MiB memory
#define ARGON2_P_COST    1          // parallelism
#define ARGON2_SALT_LEN  16         // salt bytes
#define ARGON2_HASH_LEN  32         // hash output bytes
#define ARGON2_ENC_LEN   256        // encoded string buffer

typedef struct
{
  uint32_t pass_min;
  uint32_t pass_max;
  uint32_t max_mfa;
} userns_cfg_t;

#define USERNS_MAX_MFA_DEFAULT  10

extern userns_cfg_t userns_cfg;

#define MFA_CACHE_BUCKETS  64

typedef struct mfa_cache_entry
{
  char                     pattern[USERNS_MFA_PATTERN_SZ];
  char                     username[USERNS_USER_SZ];
  char                     uuid[USERNS_UUID_SZ];
  struct mfa_cache_entry  *next;     // hash chain
} mfa_cache_entry_t;

typedef struct
{
  mfa_cache_entry_t  *buckets[MFA_CACHE_BUCKETS];
  uint32_t            count;         // total entries
  pthread_rwlock_t    lock;
} userns_cache_t;

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

#ifdef USERNS_CMD_INTERNAL

#include "common.h"
#include "bot.h"
#include "cmd.h"
#include "colors.h"
#include "db.h"
#include "kv.h"
#include "alloc.h"
#include "validate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         count;
} userns_cmd_list_state_t;

typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         count;
} userns_cmd_group_state_t;

typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         count;
} userns_cmd_mfa_state_t;

#endif // USERNS_CMD_INTERNAL

#endif // BM_USERNS_H
