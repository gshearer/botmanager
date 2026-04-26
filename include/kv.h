#ifndef BM_KV_H
#define BM_KV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nl.h"

#define KV_KEY_SZ  128
#define KV_STR_SZ  256

// Substituted for any secret KV value when read without an active admin
// context. Pointer-stable string literal — callers must not free.
extern const char *KV_REDACTED_VALUE;

// True iff any non-tail dot-separated segment of key equals "creds".
// "plugin.foo.creds.api_secret" is secret; "plugin.foo.creds" is not.
bool kv_is_secret_key(const char *key);

bool kv_set_secret(const char *key, const char *val);

// Like kv_get_str but for a secret key without admin context, returns
// KV_REDACTED_VALUE. Always non-NULL on a registered KV_STR key.
const char *kv_get_secret(const char *key);

// Thread-local admin-context flag. cmd dispatch sets this around the
// command callback so secret-tier KV reads return real values; all other
// readers see KV_REDACTED_VALUE.
void kv_admin_context_set(bool active);
bool kv_admin_context_active(void);

typedef enum
{
  KV_INT8,    KV_UINT8,
  KV_INT16,   KV_UINT16,
  KV_INT32,   KV_UINT32,
  KV_INT64,   KV_UINT64,
  KV_FLOAT,   KV_DOUBLE,   KV_LDOUBLE,
  KV_STR,
  KV_BOOL
} kv_type_t;

// Invoked when a value changes.
typedef void (*kv_cb_t)(const char *key, void *data);

// Fails if the key already exists or the default is bad for the type.
// help must be static storage; cb may be NULL.
bool kv_register(const char *key, kv_type_t type, const char *default_val,
    kv_cb_t cb, void *cb_data, const char *help);

// Returns 0 for missing or type-mismatched keys.
int64_t kv_get_int(const char *key);

// Returns 0 for missing or type-mismatched keys.
uint64_t kv_get_uint(const char *key);

// Returns 0.0 for missing or type-mismatched keys.
double kv_get_double(const char *key);

// Returns 0.0 for missing or type-mismatched keys.
long double kv_get_ldouble(const char *key);

// Returns pointer to internal storage (valid until value changes), or NULL.
const char *kv_get_str(const char *key);

bool kv_set(const char *key, const char *val);

// Fails on key not found, type mismatch, or out of range.
bool kv_set_int(const char *key, int64_t val);

// Fails on key not found, type mismatch, or out of range.
bool kv_set_uint(const char *key, uint64_t val);

// Fails on key not found or type mismatch.
bool kv_set_float(const char *key, long double val);

bool kv_set_str(const char *key, const char *val);

bool kv_exists(const char *key);

// Returns help text pointer (may be NULL), or NULL if key not found.
const char *kv_get_help(const char *key);

// Returns NULL if key not found.
const char *kv_get_type_name(const char *key);

// Serialize a KV entry's current value into buf as a string.
bool kv_get_val_str(const char *key, char *buf, size_t bufsz);

// Key must already be registered. cb NULL clears.
bool kv_set_cb(const char *key, kv_cb_t cb, void *cb_data);

// WARNING: invoked under the KV lock — do NOT call kv_* functions.
typedef void (*kv_iter_cb_t)(const char *key, kv_type_t type,
    const char *value_str, void *data);

// Returns the number of entries visited.
uint32_t kv_iterate_prefix(const char *prefix, kv_iter_cb_t cb, void *data);

// Also deletes matching rows from the database if available. Returns
// the number of entries deleted.
uint32_t kv_delete_prefix(const char *prefix);

const char *kv_type_name(kv_type_t type);

// Natural-language responder metadata attached to a KV. A KV becomes
// NL-bridge visible (/kv <suffix>) only when an nl_t is attached via
// kv_register_nl. All strings and arrays are static / caller-owned;
// the registry stores pointers only and never copies.
typedef struct
{
  const char         *when;              // REQUIRED — LLM cue
  const nl_example_t *examples;          // REQUIRED — >=1 entry
  uint8_t             example_count;

  // Response template with literal "$value" substitution for the KV's
  // serialized value. NULL = emit the value verbatim.
  const char         *response_template;
} kv_nl_t;

// Attach NL metadata to an already-registered KV. Fails (and logs) when
// the key has not been registered. nl storage must be static for the
// process lifetime — the registry retains the pointer.
bool kv_register_nl(const char *key, const kv_nl_t *nl);

// Return the NL metadata attached to a key, or NULL if the key has no
// NL responder attached (covers unregistered keys too).
const kv_nl_t *kv_get_nl(const char *key);

// Callback type for kv_iterate_nl. Invoked once per NL-capable KV.
// The callback is invoked UNDER NO LOCK — it may safely call other
// kv_* APIs (the registry is snapshotted before dispatch).
typedef void (*kv_nl_iter_cb_t)(const char *key, const kv_nl_t *nl,
    void *data);

// Iterate every KV that has an NL responder attached. See the callback
// type above for locking semantics.
void kv_iterate_nl(kv_nl_iter_cb_t cb, void *data);

bool kv_load(void);

// Register all remaining pending DB entries that were not claimed by
// explicit kv_register() calls. Call after bot restore to pick up
// dynamic keys (e.g., per-channel IRC configuration). Returns the
// number of entries claimed.
uint32_t kv_claim_pending(void);

bool kv_flush(void);

// Must be called after mem_init().
void kv_init(void);

// Flushes if DB is available, then frees all entries.
void kv_exit(void);

#ifdef KV_INTERNAL

#include "common.h"
#include "clam.h"
#include "db.h"
#include "alloc.h"

#include <limits.h>

#define KV_BUCKETS    64
#define KV_VAL_BUF    300    // serialization buffer

// str member makes this 256 bytes.
typedef union
{
  int8_t      i8;
  uint8_t     u8;
  int16_t     i16;
  uint16_t    u16;
  int32_t     i32;
  uint32_t    u32;
  int64_t     i64;
  uint64_t    u64;
  float       f;
  double      d;
  long double ld;
  char        str[KV_STR_SZ];
} kv_val_t;

typedef struct kv_entry
{
  char             key[KV_KEY_SZ];
  kv_type_t        type;
  kv_val_t         val;
  kv_cb_t          cb;
  void            *cb_data;
  const char      *help;     // human-readable help (static, may be NULL)
  bool             dirty;
  bool             secret;   // value redacted from non-admin readers
  struct kv_entry *next;     // hash chain
} kv_entry_t;

static kv_entry_t      *kv_table[KV_BUCKETS];
static pthread_mutex_t   kv_mutex;
static uint32_t          kv_count = 0;
static bool              kv_ready = false;

// Rows loaded by kv_load() before their key was registered. Consumed
// by kv_register() when a matching key appears.
typedef struct kv_pending
{
  char               key[KV_KEY_SZ];
  int                type;
  char               val_str[KV_STR_SZ];
  struct kv_pending  *next;
} kv_pending_t;

static kv_pending_t *kv_pending_list  = NULL;
static uint32_t      kv_pending_count = 0;

#endif // KV_INTERNAL

#endif // BM_KV_H
