#ifndef BM_KV_H
#define BM_KV_H

#include <stdbool.h>
#include <stdint.h>

#define KV_KEY_SZ  128
#define KV_STR_SZ  256

// Value types.
typedef enum
{
  KV_INT8,    KV_UINT8,
  KV_INT16,   KV_UINT16,
  KV_INT32,   KV_UINT32,
  KV_INT64,   KV_UINT64,
  KV_FLOAT,   KV_DOUBLE,   KV_LDOUBLE,
  KV_STR
} kv_type_t;

// Update callback: invoked when a value changes.
typedef void (*kv_cb_t)(const char *key, void *data);

// returns: SUCCESS or FAIL (bad default for type, or key already exists)
// key: configuration key
// type: value type
// default_val: default value as a string
// cb: optional callback invoked on value change (NULL for none)
// cb_data: opaque data passed to callback
bool kv_register(const char *key, kv_type_t type, const char *default_val,
    kv_cb_t cb, void *cb_data);

// returns: signed integer value (0 for missing or type-mismatched keys)
// key: configuration key
int64_t kv_get_int(const char *key);

// returns: unsigned integer value (0 for missing or type-mismatched keys)
// key: configuration key
uint64_t kv_get_uint(const char *key);

// returns: double value (0.0 for missing or type-mismatched keys)
// key: configuration key
double kv_get_double(const char *key);

// returns: long double value (0.0 for missing or type-mismatched keys)
// key: configuration key
long double kv_get_ldouble(const char *key);

// returns: pointer to internal storage (valid until value changes), or NULL
// key: configuration key
const char *kv_get_str(const char *key);

// returns: SUCCESS or FAIL
// key: configuration key
// val: new value as a string
bool kv_set(const char *key, const char *val);

// returns: SUCCESS or FAIL (key not found, type mismatch, or out of range)
// key: configuration key
// val: new signed integer value
bool kv_set_int(const char *key, int64_t val);

// returns: SUCCESS or FAIL (key not found, type mismatch, or out of range)
// key: configuration key
// val: new unsigned integer value
bool kv_set_uint(const char *key, uint64_t val);

// returns: SUCCESS or FAIL (key not found or type mismatch)
// key: configuration key
// val: new floating-point value
bool kv_set_float(const char *key, long double val);

// returns: SUCCESS or FAIL
// key: configuration key
// val: new string value
bool kv_set_str(const char *key, const char *val);

// returns: true if key is registered
// key: configuration key
bool kv_exists(const char *key);

// Callback type for kv_iterate_prefix(). Called once per matching entry.
// WARNING: invoked under the KV lock — do NOT call kv_* functions.
// key: entry key
// type: entry type
// value_str: serialized value string
// data: opaque user data
typedef void (*kv_iter_cb_t)(const char *key, kv_type_t type,
    const char *value_str, void *data);

// Iterate all entries whose key starts with prefix. Calls cb for each.
// returns: number of entries visited
// prefix: key prefix to match
// cb: callback function
// data: opaque data passed to callback
uint32_t kv_iterate_prefix(const char *prefix, kv_iter_cb_t cb, void *data);

// Delete all KV entries whose key starts with the given prefix.
// Also deletes matching rows from the database if available.
// returns: number of entries deleted
// prefix: key prefix to match (e.g., "bot.mybot.")
uint32_t kv_delete_prefix(const char *prefix);

// returns: human-readable name of a KV type
// type: KV type enum value
const char *kv_type_name(kv_type_t type);

// returns: SUCCESS or FAIL
bool kv_load(void);

// Register all remaining pending DB entries that were not claimed by
// explicit kv_register() calls. Call after bot restore to pick up
// dynamic keys (e.g., per-channel IRC configuration).
// returns: number of entries claimed
uint32_t kv_claim_pending(void);

// returns: SUCCESS or FAIL
bool kv_flush(void);

// Initialize the KV subsystem. Must be called after mem_init().
void kv_init(void);

// Shut down: flush if DB is available, free all entries.
void kv_exit(void);

#ifdef KV_INTERNAL

#include "common.h"
#include "clam.h"
#include "db.h"
#include "mem.h"

#include <limits.h>

#define KV_BUCKETS    64
#define KV_VAL_BUF    300    // serialization buffer

// Value union — str member makes this 256 bytes.
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
  bool             dirty;
  struct kv_entry *next;    // hash chain
} kv_entry_t;

static kv_entry_t      *kv_table[KV_BUCKETS];
static pthread_mutex_t   kv_mutex;
static uint32_t          kv_count = 0;
static bool              kv_ready = false;

// Pending DB values: rows loaded by kv_load() before their key was
// registered. Consumed by kv_register() when a matching key appears.
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
