// botmanager — MIT
// In-memory hierarchical key-value store with persistence hooks.
#define KV_INTERNAL
#include "kv.h"

#include "util.h"

// NL responder registry. Kept separate from the main KV table so the
// per-entry struct can stay unchanged and so the NL iterator can
// release its lock before dispatching callbacks. The registry is a
// singly-linked list — NL-capable KVs number in the dozens, not
// thousands, so a hash table would be overkill.
typedef struct kv_nl_reg
{
  char              key[KV_KEY_SZ];
  const kv_nl_t    *nl;
  struct kv_nl_reg *next;
} kv_nl_reg_t;

static kv_nl_reg_t     *kv_nl_head  = NULL;
static pthread_mutex_t  kv_nl_mutex = PTHREAD_MUTEX_INITIALIZER;

const char *KV_REDACTED_VALUE = "[CENSORED]";

static __thread bool kv_admin_active = false;

void
kv_admin_context_set(bool active)
{
  kv_admin_active = active;
}

bool
kv_admin_context_active(void)
{
  return(kv_admin_active);
}

bool
kv_is_secret_key(const char *key)
{
  const char *p;
  const char *seg;

  if(key == NULL || key[0] == '\0')
    return(false);

  // Walk dot-separated segments. A segment is "secret-bearing" iff it
  // equals "creds" AND is not the trailing segment (i.e., is followed
  // by another '.').
  seg = key;

  for(p = key; *p != '\0'; p++)
  {
    if(*p == '.')
    {
      size_t len = (size_t)(p - seg);

      if(len == 5 && memcmp(seg, "creds", 5) == 0)
        return(true);

      seg = p + 1;
    }
  }

  // Tail segment intentionally not checked — a key whose final segment
  // is "creds" is NOT secret per the rule.
  return(false);
}

static inline uint32_t
hash_key(const char *key)
{
  return(util_djb2(key) % KV_BUCKETS);
}

// Find an entry by key. Must be called with kv_mutex held.
static kv_entry_t *
find_locked(const char *key)
{
  uint32_t bucket = hash_key(key);

  for(kv_entry_t *e = kv_table[bucket]; e != NULL; e = e->next)
    if(strcmp(e->key, key) == 0)
      return(e);

  return(NULL);
}

// Return the one immortal copy of the first `len` bytes of s, creating
// it if this is the first time those bytes have been stored. Callers
// choose the length because the two things interned here answer to
// different bounds: a value truncates at KV_STR_SZ-1, help does not.
//
// Never returns NULL: mem_alloc aborts rather than fail.
static const char *
intern_bytes(const char *s, size_t len)
{
  kv_str_node_t *n;
  uint32_t       h      = 5381;
  uint32_t       bucket;

  for(size_t i = 0; i < len; i++)
    h = ((h << 5) + h) + (uint8_t)s[i];

  bucket = h % KV_INTERN_BUCKETS;

  pthread_mutex_lock(&kv_intern_mutex);

  for(n = kv_intern_table[bucket]; n != NULL; n = n->next)
    if(n->len == len && memcmp(n->s, s, len) == 0)
    {
      pthread_mutex_unlock(&kv_intern_mutex);
      return(n->s);
    }

  n = mem_alloc("kv", "string", sizeof(*n) + len + 1);

  n->len = len;
  memcpy(n->s, s, len);
  n->s[len] = '\0';

  n->next                 = kv_intern_table[bucket];
  kv_intern_table[bucket] = n;
  kv_intern_count++;

  pthread_mutex_unlock(&kv_intern_mutex);

  return(n->s);
}

// A KV value. The bound is the one the entry's inline buffer used to
// impose, so an over-long value truncates exactly as it always did.
static const char *
kv_intern(const char *s)
{
  return(intern_bytes(s, strnlen(s, KV_STR_SZ - 1)));
}

// A KV help string (OBS-14). "Static caller-owned storage" meant static
// in the *owning plugin's* mapping, so the pointer the registry retained
// outlived the object it pointed into and the one reader in core
// (cmd.c's `/help kv`) read an unmapped page. Interning gives help the
// same lifetime kv_get_str's answer has: immortal, and nobody's to free.
// No bound applies — help is prose, and truncating it would trade a rare
// crash for a permanent silent one.
static const char *
intern_help(const char *s)
{
  return(intern_bytes(s, strlen(s)));
}

static bool
str_to_val(kv_type_t type, const char *str, kv_val_t *val)
{
  char *end;
  long long ll;
  unsigned long long ull;

  switch(type)
  {
    case KV_INT8:
      ll = strtoll(str, &end, 10);
      if(end == str || *end != '\0') return(FAIL);
      if(ll < INT8_MIN || ll > INT8_MAX) return(FAIL);
      val->i8 = (int8_t)ll;
      return(SUCCESS);

    case KV_UINT8:
      ull = strtoull(str, &end, 10);
      if(end == str || *end != '\0') return(FAIL);
      if(ull > UINT8_MAX) return(FAIL);
      val->u8 = (uint8_t)ull;
      return(SUCCESS);

    case KV_INT16:
      ll = strtoll(str, &end, 10);
      if(end == str || *end != '\0') return(FAIL);
      if(ll < INT16_MIN || ll > INT16_MAX) return(FAIL);
      val->i16 = (int16_t)ll;
      return(SUCCESS);

    case KV_UINT16:
      ull = strtoull(str, &end, 10);
      if(end == str || *end != '\0') return(FAIL);
      if(ull > UINT16_MAX) return(FAIL);
      val->u16 = (uint16_t)ull;
      return(SUCCESS);

    case KV_INT32:
      ll = strtoll(str, &end, 10);
      if(end == str || *end != '\0') return(FAIL);
      if(ll < INT32_MIN || ll > INT32_MAX) return(FAIL);
      val->i32 = (int32_t)ll;
      return(SUCCESS);

    case KV_UINT32:
      ull = strtoull(str, &end, 10);
      if(end == str || *end != '\0') return(FAIL);
      if(ull > UINT32_MAX) return(FAIL);
      val->u32 = (uint32_t)ull;
      return(SUCCESS);

    case KV_INT64:
      ll = strtoll(str, &end, 10);
      if(end == str || *end != '\0') return(FAIL);
      val->i64 = (int64_t)ll;
      return(SUCCESS);

    case KV_UINT64:
      ull = strtoull(str, &end, 10);
      if(end == str || *end != '\0') return(FAIL);
      val->u64 = (uint64_t)ull;
      return(SUCCESS);

    case KV_FLOAT:
      val->f = strtof(str, &end);
      if(end == str || *end != '\0') return(FAIL);
      return(SUCCESS);

    case KV_DOUBLE:
      val->d = strtod(str, &end);
      if(end == str || *end != '\0') return(FAIL);
      return(SUCCESS);

    case KV_LDOUBLE:
      val->ld = strtold(str, &end);
      if(end == str || *end != '\0') return(FAIL);
      return(SUCCESS);

    case KV_STR:
      val->str = kv_intern(str);
      return(SUCCESS);

    case KV_BOOL:
      if(strcmp(str, "1") == 0 || strcasecmp(str, "true") == 0)
        val->u8 = 1;
      else if(strcmp(str, "0") == 0 || strcasecmp(str, "false") == 0)
        val->u8 = 0;
      else
        return(FAIL);
      return(SUCCESS);

    default:
      return(FAIL);
  }
}

static void
val_to_str(kv_type_t type, const kv_val_t *val, char *buf, size_t bufsz)
{
  switch(type)
  {
    case KV_INT8:    snprintf(buf, bufsz, "%d",   val->i8);   break;
    case KV_UINT8:   snprintf(buf, bufsz, "%u",   val->u8);   break;
    case KV_INT16:   snprintf(buf, bufsz, "%d",   val->i16);  break;
    case KV_UINT16:  snprintf(buf, bufsz, "%u",   val->u16);  break;
    case KV_INT32:   snprintf(buf, bufsz, "%d",   val->i32);  break;
    case KV_UINT32:  snprintf(buf, bufsz, "%u",   val->u32);  break;
    case KV_INT64:
      snprintf(buf, bufsz, "%lld", (long long)val->i64);
      break;
    case KV_UINT64:
      snprintf(buf, bufsz, "%llu", (unsigned long long)val->u64);
      break;
    case KV_FLOAT:   snprintf(buf, bufsz, "%.9g",   (double)val->f); break;
    case KV_DOUBLE:  snprintf(buf, bufsz, "%.17g",  val->d);  break;
    case KV_LDOUBLE: snprintf(buf, bufsz, "%.21Lg", val->ld);  break;
    case KV_STR:
      strlcpy(buf, val->str, bufsz);
      break;
    case KV_BOOL:
      snprintf(buf, bufsz, "%s", val->u8 ? "true" : "false");
      break;
    default:
      buf[0] = '\0';
      break;
  }
}

static bool
val_changed(kv_type_t type, const kv_val_t *old, const kv_val_t *new)
{
  char a[KV_VAL_BUF], b[KV_VAL_BUF];

  // Interning makes pointer identity value identity, so the string case
  // answers without serializing 600 bytes to compare them.
  if(type == KV_STR)
    return(old->str != new->str);

  val_to_str(type, old, a, sizeof(a));
  val_to_str(type, new, b, sizeof(b));
  return(strcmp(a, b) != 0);
}

// Set a typed value into an entry from a new parsed value.
// Fires callback outside lock if value changed.
// Must be called with kv_mutex held. Unlocks before callback.
static bool
apply_val(kv_entry_t *e, const kv_val_t *new_val)
{
  char     key[KV_KEY_SZ];
  bool     changed;
  kv_cb_t  cb;
  void    *cb_data;

  changed = val_changed(e->type, &e->val, new_val);

  if(changed)
  {
    e->val = *new_val;
    e->dirty = true;
  }

  cb      = changed ? e->cb : NULL;
  cb_data = e->cb_data;

  // The callback runs with the lock released, so it cannot be handed
  // e->key: a concurrent kv_unregister or a plugin unload's
  // kv_reclaim_owned frees the entry out from under it. The value it
  // will read back needs no such care — see kv_get_str.
  strlcpy(key, e->key, sizeof(key));

  pthread_mutex_unlock(&kv_mutex);

  if(cb != NULL)
    cb(key, cb_data);

  return(SUCCESS);
}

static bool
persist_entry(kv_entry_t *e)
{
  char         val_str[KV_VAL_BUF];
  char        *esc_key;
  char        *esc_val;
  char         sql[512 + KV_KEY_SZ + KV_VAL_BUF];
  db_result_t *r;
  bool         ret;

  val_to_str(e->type, &e->val, val_str, sizeof(val_str));

  esc_key = db_escape(e->key);
  esc_val = db_escape(val_str);

  if(esc_key == NULL || esc_val == NULL)
  {
    if(esc_key != NULL) mem_free(esc_key);
    if(esc_val != NULL) mem_free(esc_val);
    return(FAIL);
  }

  snprintf(sql, sizeof(sql),
      "INSERT INTO kv (key, type, value) VALUES ('%s', %d, '%s') "
      "ON CONFLICT (key) DO UPDATE SET type = EXCLUDED.type, "
      "value = EXCLUDED.value",
      esc_key, (int)e->type, esc_val);

  mem_free(esc_key);
  mem_free(esc_val);

  r = db_result_alloc();
  ret = db_query(sql, r);

  db_result_free(r);

  return(ret);
}

// Public API

const char *
kv_type_name(kv_type_t type)
{
  switch(type)
  {
    case KV_INT8:    return("INT8");
    case KV_UINT8:   return("UINT8");
    case KV_INT16:   return("INT16");
    case KV_UINT16:  return("UINT16");
    case KV_INT32:   return("INT32");
    case KV_UINT32:  return("UINT32");
    case KV_INT64:   return("INT64");
    case KV_UINT64:  return("UINT64");
    case KV_FLOAT:   return("FLOAT");
    case KV_DOUBLE:  return("DOUBLE");
    case KV_LDOUBLE: return("LDOUBLE");
    case KV_STR:     return("STR");
    case KV_BOOL:    return("BOOL");
    default:         return("UNKNOWN");
  }
}

// Fetch one persisted row by exact key. Caller must NOT hold kv_mutex
// (this does DB I/O). On a hit, writes the raw type id to *out_type and
// copies the value string into out_val[out_sz]. Returns true iff a row
// exists and both columns are non-NULL.
static bool
kv_db_lookup_one(const char *key, int *out_type, char *out_val, size_t out_sz)
{
  char        *esc;
  char         sql[256 + KV_KEY_SZ];
  db_result_t *r;
  bool         hit = false;

  esc = db_escape(key);

  if(esc == NULL)
    return(false);

  snprintf(sql, sizeof(sql),
      "SELECT type, value FROM kv WHERE key = '%s'", esc);
  mem_free(esc);

  r = db_result_alloc();

  if(db_query(sql, r) == SUCCESS && r->rows > 0)
  {
    const char *db_type = db_result_get(r, 0, 0);
    const char *db_val  = db_result_get(r, 0, 1);

    if(db_type != NULL && db_val != NULL)
    {
      *out_type = atoi(db_type);
      strlcpy(out_val, db_val, out_sz);
      hit = true;
    }
  }

  db_result_free(r);
  return(hit);
}

bool
kv_register(const char *key, kv_type_t type, const char *default_val,
    kv_cb_t cb, void *cb_data, const char *help)
{
  // Core is link_whole'd into botman with export_dynamic, so a plugin
  // reaches this symbol through the PLT and the return address lands in
  // the plugin's own mapping.
  return(kv_register_owned(key, type, default_val, cb, cb_data, help,
      __builtin_return_address(0)));
}

// Which part of a re-registration the standing entry disagrees with, or
// NULL when the two declarations say the same thing. Re-registering a key
// is the design here — a method plugin's per-bot instance KV is replayed
// on every rebind, and releasing it would be wrong (PLUGIN.md §Class A).
// Caller holds kv_mutex; help is interned, so identical prose is one
// pointer.
static const char *
reg_differs(const kv_entry_t *e, kv_type_t type, const kv_val_t *def,
    kv_cb_t cb, void *cb_data, const char *help)
{
  if(e->type != type)
    return("type");

  // Only past the type check do the two defaults name the same union member.
  if(val_changed(type, &e->def, def))
    return("default");

  if(e->cb != cb || e->cb_data != cb_data)
    return("callback");

  if(e->help != help)
    return("help text");

  return(NULL);
}

bool
kv_register_owned(const char *key, kv_type_t type, const char *default_val,
    kv_cb_t cb, void *cb_data, const char *help, const void *owner_pc)
{
  kv_val_t       val;
  kv_entry_t    *e;
  const char    *help_i;
  uint32_t       bucket;

  if(key == NULL || default_val == NULL)
    return(FAIL);

  // Parse the default value to validate it.
  memset(&val, 0, sizeof(val));

  if(str_to_val(type, default_val, &val) != SUCCESS)
  {
    clam(CLAM_WARN, "kv_register",
        "invalid default '%s' for '%s' (type: %s)",
        default_val, key, kv_type_name(type));
    return(FAIL);
  }

  // Interned above the lock so the duplicate comparison below is a pointer
  // test, and so kv_intern_mutex is not taken under kv_mutex to make it.
  help_i = (help != NULL) ? intern_help(help) : NULL;

  pthread_mutex_lock(&kv_mutex);

  // Check for duplicate. The first declaration wins and nothing it holds is
  // touched, so a replay of that same declaration is correct behaviour and
  // says so quietly — the alternative made a WARN out of the design, 736 of
  // them in one log, in a tree where the identical words from cmd_register
  // mean the command does not exist (AGENTS.md §Two Different "Memory"
  // Concepts). Same ruling, same reason, as plugin.c's reclaim count. Only a
  // declaration that actually differs has lost something, and that one is
  // still loud.
  e = find_locked(key);

  if(e != NULL)
  {
    const char *differs = reg_differs(e, type, &val, cb, cb_data, help_i);

    pthread_mutex_unlock(&kv_mutex);

    if(differs == NULL)
      clam(CLAM_DEBUG, "kv_register",
          "'%s' already registered; keeping the existing declaration", key);

    else
      clam(CLAM_WARN, "kv_register",
          "'%s' already registered with a different %s; the existing "
          "declaration stands and this one is discarded", key, differs);

    return(FAIL);
  }

  // Allocate and populate entry.
  e = mem_alloc("kv", "entry", sizeof(kv_entry_t));

  strlcpy(e->key, key, KV_KEY_SZ);
  e->type     = type;
  e->val      = val;
  e->def      = val;   // what the declaration said, kept for kv_get_uint_or_default
  e->cb       = cb;
  e->cb_data  = cb_data;
  e->help     = help_i;
  e->owner_pc = owner_pc;
  e->dirty    = true;   // new entries need DB persistence
  e->secret   = kv_is_secret_key(key);

  // Insert into hash table.
  bucket = hash_key(key);

  e->next = kv_table[bucket];
  kv_table[bucket] = e;
  kv_count++;

  // The entry is inserted at its schema default. Before kv_load() has run
  // (cold-start registration) the kv table may not exist yet and the bulk
  // load pass will apply any persisted value momentarily — keep the
  // default. After kv_load(), the DB is authoritative: a re-register after
  // a plugin hot-reload, or a dynamic key registered post-restore, must
  // take the persisted value. Read it with the lock released.
  if(kv_loaded)
  {
    int  db_type;
    char db_str[KV_STR_SZ];

    pthread_mutex_unlock(&kv_mutex);

    if(kv_db_lookup_one(key, &db_type, db_str, sizeof(db_str)))
    {
      kv_val_t db_val;
      bool     live;

      memset(&db_val, 0, sizeof(db_val));

      pthread_mutex_lock(&kv_mutex);

      // Re-find: a concurrent kv_unregister may have dropped the entry.
      e    = find_locked(key);
      live = (e != NULL);

      // Type-mismatched rows are ignored (keep the default), mirroring the
      // old pending-restore guard.
      if(live && db_type == (int)type &&
          str_to_val(type, db_str, &db_val) == SUCCESS)
      {
        e->val   = db_val;
        e->dirty = false;
        pthread_mutex_unlock(&kv_mutex);

        clam(CLAM_DEBUG, "kv_register",
            "'%s' (%s) = %s [rehydrated from db]",
            key, kv_type_name(type), db_str);
        return(SUCCESS);
      }

      // A row exists that this registration cannot take: the declaration
      // changed type, or the stored text no longer parses as one. Keeping
      // the default is the correct outcome — but the entry was born dirty,
      // and a dirty default is exactly what kv_flush() would write over the
      // operator's tuned row at shutdown (the kv_flush_failed_boot_clobber
      // class). Mark it clean: nothing persists until someone deliberately
      // sets the key, so reverting the declaration restores the value.
      if(live)
        e->dirty = false;

      pthread_mutex_unlock(&kv_mutex);

      // Doing this at DEBUG silently resets whatever was tuned — say it out
      // loud (root TODO.md §PLIFE-4, the type-changed case).
      if(live)
        clam(CLAM_WARN, "kv_register",
            "'%s': persisted row is %s '%s' but the schema declares %s — "
            "using default '%s'; the row is untouched",
            key, kv_type_name((kv_type_t)db_type), db_str,
            kv_type_name(type), default_val);
    }

    clam(CLAM_DEBUG, "kv_register", "'%s' (%s) = %s",
        key, kv_type_name(type), default_val);
    return(SUCCESS);
  }

  pthread_mutex_unlock(&kv_mutex);
  clam(CLAM_DEBUG, "kv_register", "'%s' (%s) = %s",
      key, kv_type_name(type), default_val);
  return(SUCCESS);
}

int64_t
kv_get_int(const char *key)
{
  int64_t     result = 0;
  kv_entry_t *e;

  pthread_mutex_lock(&kv_mutex);
  e = find_locked(key);

  if(e != NULL)
  {
    switch(e->type)
    {
      case KV_INT8:   result = e->val.i8;              break;
      case KV_INT16:  result = e->val.i16;             break;
      case KV_INT32:  result = e->val.i32;             break;
      case KV_INT64:  result = e->val.i64;             break;
      case KV_UINT8:  result = (int64_t)e->val.u8;    break;
      case KV_UINT16: result = (int64_t)e->val.u16;   break;
      case KV_UINT32: result = (int64_t)e->val.u32;   break;
      case KV_UINT64: result = (int64_t)e->val.u64;   break;
      case KV_BOOL:   result = (int64_t)e->val.u8;    break;
      default: break;
    }
  }

  pthread_mutex_unlock(&kv_mutex);

  return(result);
}

// Widen whichever member of `v` the type names. A type that holds no
// integer reads as 0, which is kv_get_uint's answer for it either way.
static uint64_t
val_as_uint(kv_type_t type, const kv_val_t *v)
{
  switch(type)
  {
    case KV_UINT8:  return(v->u8);
    case KV_UINT16: return(v->u16);
    case KV_UINT32: return(v->u32);
    case KV_UINT64: return(v->u64);
    case KV_INT8:   return((uint64_t)v->i8);
    case KV_INT16:  return((uint64_t)v->i16);
    case KV_INT32:  return((uint64_t)v->i32);
    case KV_INT64:  return((uint64_t)v->i64);
    case KV_BOOL:   return((uint64_t)v->u8);
    default:        return(0);
  }
}

uint64_t
kv_get_uint(const char *key)
{
  uint64_t    result = 0;
  kv_entry_t *e;

  pthread_mutex_lock(&kv_mutex);
  e = find_locked(key);

  if(e != NULL)
    result = val_as_uint(e->type, &e->val);

  pthread_mutex_unlock(&kv_mutex);

  return(result);
}

uint64_t
kv_get_uint_or_default(const char *key)
{
  uint64_t    result = 0;
  kv_entry_t *e;

  pthread_mutex_lock(&kv_mutex);
  e = find_locked(key);

  // One lookup answers both: the default travels with the entry, so the
  // substitution costs nothing a plain read did not already pay.
  if(e != NULL)
  {
    result = val_as_uint(e->type, &e->val);

    if(result == 0)
      result = val_as_uint(e->type, &e->def);
  }

  pthread_mutex_unlock(&kv_mutex);

  return(result);
}

double
kv_get_double(const char *key)
{
  double      result = 0.0;
  kv_entry_t *e;

  pthread_mutex_lock(&kv_mutex);
  e = find_locked(key);

  if(e != NULL)
  {
    switch(e->type)
    {
      case KV_FLOAT:   result = (double)e->val.f;  break;
      case KV_DOUBLE:  result = e->val.d;           break;
      case KV_LDOUBLE: result = (double)e->val.ld;  break;
      default: break;
    }
  }

  pthread_mutex_unlock(&kv_mutex);

  return(result);
}

long double
kv_get_ldouble(const char *key)
{
  long double  result = 0.0L;
  kv_entry_t  *e;

  pthread_mutex_lock(&kv_mutex);
  e = find_locked(key);

  if(e != NULL)
  {
    switch(e->type)
    {
      case KV_FLOAT:   result = (long double)e->val.f;  break;
      case KV_DOUBLE:  result = (long double)e->val.d;  break;
      case KV_LDOUBLE: result = e->val.ld;              break;
      default: break;
    }
  }

  pthread_mutex_unlock(&kv_mutex);

  return(result);
}

const char *
kv_get_str(const char *key)
{
  const char *result = NULL;
  kv_entry_t *e;
  bool        redact = false;

  pthread_mutex_lock(&kv_mutex);
  e = find_locked(key);

  if(e != NULL && e->type == KV_STR)
  {
    if(e->secret && !kv_admin_active)
      redact = true;
    else
      result = e->val.str;
  }

  pthread_mutex_unlock(&kv_mutex);

  if(redact)
    return(KV_REDACTED_VALUE);

  return(result);
}

const char *
kv_get_secret(const char *key)
{
  const char *s = kv_get_str(key);

  return(s != NULL ? s : KV_REDACTED_VALUE);
}

// Arms the thread-local admin context for the duration of one read so a
// plugin can retrieve its own credential regardless of the caller's
// transport. Restores the prior state — safe to nest inside an already
// de-redacted context.
const char *
kv_get_creds(const char *key)
{
  const char *result;
  bool        prev = kv_admin_active;

  kv_admin_active = true;
  result          = kv_get_str(key);
  kv_admin_active = prev;

  return(result);
}

bool
kv_set_secret(const char *key, const char *val)
{
  kv_entry_t *e;

  pthread_mutex_lock(&kv_mutex);

  e = find_locked(key);

  if(e != NULL)
    e->secret = true;

  pthread_mutex_unlock(&kv_mutex);

  return(kv_set(key, val));
}

bool
kv_set(const char *key, const char *val)
{
  kv_val_t    new_val;
  kv_entry_t *e;

  if(key == NULL || val == NULL)
    return(FAIL);

  memset(&new_val, 0, sizeof(new_val));

  pthread_mutex_lock(&kv_mutex);

  e = find_locked(key);

  if(e == NULL)
  {
    pthread_mutex_unlock(&kv_mutex);
    return(FAIL);
  }

  if(str_to_val(e->type, val, &new_val) != SUCCESS)
  {
    pthread_mutex_unlock(&kv_mutex);
    return(FAIL);
  }

  // apply_val unlocks the mutex and fires callback if changed.
  return(apply_val(e, &new_val));
}

bool
kv_set_int(const char *key, int64_t val)
{
  kv_entry_t *e;
  kv_val_t    new_val;

  if(key == NULL)
    return(FAIL);

  pthread_mutex_lock(&kv_mutex);

  e = find_locked(key);

  if(e == NULL)
  {
    pthread_mutex_unlock(&kv_mutex);
    return(FAIL);
  }

  memset(&new_val, 0, sizeof(new_val));

  switch(e->type)
  {
    case KV_INT8:
      if(val < INT8_MIN || val > INT8_MAX) goto fail;
      new_val.i8 = (int8_t)val;
      break;
    case KV_UINT8:
      if(val < 0 || val > UINT8_MAX) goto fail;
      new_val.u8 = (uint8_t)val;
      break;
    case KV_INT16:
      if(val < INT16_MIN || val > INT16_MAX) goto fail;
      new_val.i16 = (int16_t)val;
      break;
    case KV_UINT16:
      if(val < 0 || val > UINT16_MAX) goto fail;
      new_val.u16 = (uint16_t)val;
      break;
    case KV_INT32:
      if(val < INT32_MIN || val > INT32_MAX) goto fail;
      new_val.i32 = (int32_t)val;
      break;
    case KV_UINT32:
      if(val < 0 || val > UINT32_MAX) goto fail;
      new_val.u32 = (uint32_t)val;
      break;
    case KV_INT64:
      new_val.i64 = val;
      break;
    case KV_UINT64:
      if(val < 0) goto fail;
      new_val.u64 = (uint64_t)val;
      break;
    case KV_BOOL:
      if(val < 0 || val > 1) goto fail;
      new_val.u8 = (uint8_t)val;
      break;
    default:
      goto fail;
  }

  return(apply_val(e, &new_val));

fail:
  pthread_mutex_unlock(&kv_mutex);
  return(FAIL);
}

bool
kv_set_uint(const char *key, uint64_t val)
{
  kv_entry_t *e;
  kv_val_t    new_val;

  if(key == NULL)
    return(FAIL);

  pthread_mutex_lock(&kv_mutex);

  e = find_locked(key);

  if(e == NULL)
  {
    pthread_mutex_unlock(&kv_mutex);
    return(FAIL);
  }

  memset(&new_val, 0, sizeof(new_val));

  switch(e->type)
  {
    case KV_INT8:
      if(val > (uint64_t)INT8_MAX) goto fail;
      new_val.i8 = (int8_t)val;
      break;
    case KV_UINT8:
      if(val > UINT8_MAX) goto fail;
      new_val.u8 = (uint8_t)val;
      break;
    case KV_INT16:
      if(val > (uint64_t)INT16_MAX) goto fail;
      new_val.i16 = (int16_t)val;
      break;
    case KV_UINT16:
      if(val > UINT16_MAX) goto fail;
      new_val.u16 = (uint16_t)val;
      break;
    case KV_INT32:
      if(val > (uint64_t)INT32_MAX) goto fail;
      new_val.i32 = (int32_t)val;
      break;
    case KV_UINT32:
      if(val > UINT32_MAX) goto fail;
      new_val.u32 = (uint32_t)val;
      break;
    case KV_INT64:
      if(val > (uint64_t)INT64_MAX) goto fail;
      new_val.i64 = (int64_t)val;
      break;
    case KV_UINT64:
      new_val.u64 = val;
      break;
    case KV_BOOL:
      if(val > 1) goto fail;
      new_val.u8 = (uint8_t)val;
      break;
    default:
      goto fail;
  }

  return(apply_val(e, &new_val));

fail:
  pthread_mutex_unlock(&kv_mutex);
  return(FAIL);
}

bool
kv_set_float(const char *key, long double val)
{
  kv_entry_t *e;
  kv_val_t    new_val;

  if(key == NULL)
    return(FAIL);

  pthread_mutex_lock(&kv_mutex);

  e = find_locked(key);

  if(e == NULL)
  {
    pthread_mutex_unlock(&kv_mutex);
    return(FAIL);
  }

  memset(&new_val, 0, sizeof(new_val));

  switch(e->type)
  {
    case KV_FLOAT:   new_val.f  = (float)val;  break;
    case KV_DOUBLE:  new_val.d  = (double)val;  break;
    case KV_LDOUBLE: new_val.ld = val;           break;
    default:
      pthread_mutex_unlock(&kv_mutex);
      return(FAIL);
  }

  return(apply_val(e, &new_val));
}

bool
kv_set_str(const char *key, const char *val)
{
  return(kv_set(key, val));
}

bool
kv_exists(const char *key)
{
  bool found;

  pthread_mutex_lock(&kv_mutex);
  found = (find_locked(key) != NULL);
  pthread_mutex_unlock(&kv_mutex);
  return(found);
}

const char *
kv_get_help(const char *key)
{
  kv_entry_t *e;
  const char *help;

  if(key == NULL)
    return(NULL);

  pthread_mutex_lock(&kv_mutex);

  e = find_locked(key);
  help = (e != NULL) ? e->help : NULL;

  pthread_mutex_unlock(&kv_mutex);
  return(help);
}

bool
kv_get_val_str(const char *key, char *buf, size_t bufsz)
{
  kv_entry_t *e;

  if(key == NULL || buf == NULL || bufsz == 0)
    return(FAIL);

  pthread_mutex_lock(&kv_mutex);

  e = find_locked(key);

  if(e == NULL)
  {
    pthread_mutex_unlock(&kv_mutex);
    return(FAIL);
  }

  if(e->secret && !kv_admin_active)
    strlcpy(buf, KV_REDACTED_VALUE, bufsz);

  else
    val_to_str(e->type, &e->val, buf, bufsz);

  pthread_mutex_unlock(&kv_mutex);
  return(SUCCESS);
}

const char *
kv_get_type_name(const char *key)
{
  kv_entry_t *e;
  const char *name;

  if(key == NULL)
    return(NULL);

  pthread_mutex_lock(&kv_mutex);

  e = find_locked(key);
  name = (e != NULL) ? kv_type_name(e->type) : NULL;

  pthread_mutex_unlock(&kv_mutex);
  return(name);
}

// Set or replace the change callback on an existing KV entry.
// returns: SUCCESS or FAIL (key not found)
// key: configuration key (must already be registered)
bool
kv_set_cb(const char *key, kv_cb_t cb, void *cb_data)
{
  kv_entry_t *e;

  if(key == NULL)
    return(FAIL);

  pthread_mutex_lock(&kv_mutex);

  e = find_locked(key);

  if(e == NULL)
  {
    pthread_mutex_unlock(&kv_mutex);
    return(FAIL);
  }

  e->cb      = cb;
  e->cb_data = cb_data;

  pthread_mutex_unlock(&kv_mutex);
  return(SUCCESS);
}

// Per-bot keys — the one place the "bot.<name>[.<kind>].<suffix>"
// grammar is written for reading. Its write side is
// bot_register_driver_kv() / bot_register_method_kv() in core/bot.c,
// which lay the same two forms down from a plugin's instance schema.
//
// Composing here rather than at ~150 call sites buys one thing those
// copies could not have: a truncated key is refused and logged instead
// of quietly addressing the shorter key it collapsed onto.

static bool
kv_bot_key(char *buf, size_t sz, const char *name, const char *kind,
    const char *suffix)
{
  int n;

  if(name == NULL || name[0] == '\0' || suffix == NULL || suffix[0] == '\0')
    return(FAIL);

  if(kind != NULL && kind[0] != '\0')
    n = snprintf(buf, sz, "bot.%s.%s.%s", name, kind, suffix);

  else
    n = snprintf(buf, sz, "bot.%s.%s", name, suffix);

  if(n < 0 || (size_t)n >= sz)
  {
    clam(CLAM_WARN, "kv_bot_key",
        "key too long for bot '%s'%s%s suffix '%s' — refusing rather than"
        " addressing the key it truncates onto", name,
        (kind != NULL) ? " method " : "", (kind != NULL) ? kind : "", suffix);
    return(FAIL);
  }

  return(SUCCESS);
}

uint64_t
kv_get_bot_uint(const char *name, const char *suffix)
{
  return(kv_get_bot_method_uint(name, NULL, suffix));
}

uint64_t
kv_get_bot_method_uint(const char *name, const char *kind, const char *suffix)
{
  char key[KV_KEY_SZ];

  if(kv_bot_key(key, sizeof(key), name, kind, suffix) != SUCCESS)
    return(0);

  return(kv_get_uint(key));
}

uint64_t
kv_get_bot_uint_or_default(const char *name, const char *suffix)
{
  char key[KV_KEY_SZ];

  if(kv_bot_key(key, sizeof(key), name, NULL, suffix) != SUCCESS)
    return(0);

  return(kv_get_uint_or_default(key));
}

const char *
kv_get_bot_str(const char *name, const char *suffix)
{
  return(kv_get_bot_method_str(name, NULL, suffix));
}

const char *
kv_get_bot_method_str(const char *name, const char *kind, const char *suffix)
{
  char key[KV_KEY_SZ];

  if(kv_bot_key(key, sizeof(key), name, kind, suffix) != SUCCESS)
    return(NULL);

  return(kv_get_str(key));
}

bool
kv_set_bot_uint(const char *name, const char *suffix, uint64_t val)
{
  char key[KV_KEY_SZ];

  if(kv_bot_key(key, sizeof(key), name, NULL, suffix) != SUCCESS)
    return(FAIL);

  return(kv_set_uint(key, val));
}

// NL responder registry — attach / lookup / iterate.
//
// The underlying KV must already be registered: an nl_register_nl call
// on an unknown key is a programmer error and logs a warning. Duplicate
// attachments replace the previous pointer in place so a plugin can
// legitimately re-register its schema at runtime.

bool
kv_register_nl(const char *key, const kv_nl_t *nl)
{
  kv_nl_reg_t *r;
  bool         exists;

  if(key == NULL || key[0] == '\0' || nl == NULL)
    return(FAIL);

  // Refuse attachment when the key is not registered. Prevents silently
  // exposing NL metadata for a typo'd or stale key.
  pthread_mutex_lock(&kv_mutex);
  exists = (find_locked(key) != NULL);
  pthread_mutex_unlock(&kv_mutex);

  if(!exists)
  {
    clam(CLAM_WARN, "kv_register_nl",
        "refusing NL attach on unknown key '%s'", key);
    return(FAIL);
  }

  pthread_mutex_lock(&kv_nl_mutex);

  for(r = kv_nl_head; r != NULL; r = r->next)
  {
    if(strcmp(r->key, key) == 0)
    {
      r->nl = nl;
      pthread_mutex_unlock(&kv_nl_mutex);
      return(SUCCESS);
    }
  }

  r = mem_alloc("kv", "nl_reg", sizeof(kv_nl_reg_t));
  strlcpy(r->key, key, KV_KEY_SZ);
  r->nl   = nl;
  r->next = kv_nl_head;
  kv_nl_head = r;

  pthread_mutex_unlock(&kv_nl_mutex);

  clam(CLAM_DEBUG, "kv_register_nl", "'%s' NL responder attached", key);
  return(SUCCESS);
}

const kv_nl_t *
kv_get_nl(const char *key)
{
  const kv_nl_t *result = NULL;
  kv_nl_reg_t   *r;

  if(key == NULL)
    return(NULL);

  pthread_mutex_lock(&kv_nl_mutex);

  for(r = kv_nl_head; r != NULL; r = r->next)
  {
    if(strcmp(r->key, key) == 0)
    {
      result = r->nl;
      break;
    }
  }

  pthread_mutex_unlock(&kv_nl_mutex);
  return(result);
}

// Snapshot the registry under the lock, then dispatch unlocked so the
// callback is free to call other kv_* APIs (kv_get_val_str in
// particular) without risking self-deadlock.
void
kv_iterate_nl(kv_nl_iter_cb_t cb, void *data)
{
  kv_nl_reg_t  *r;
  size_t        n;
  size_t        cap;
  size_t        i;
  char         *keys;
  const kv_nl_t **nls;

  if(cb == NULL)
    return;

  pthread_mutex_lock(&kv_nl_mutex);

  cap = 0;
  for(r = kv_nl_head; r != NULL; r = r->next)
    cap++;

  if(cap == 0)
  {
    pthread_mutex_unlock(&kv_nl_mutex);
    return;
  }

  keys = mem_alloc("kv", "nl_iter_keys", cap * KV_KEY_SZ);
  nls  = mem_alloc("kv", "nl_iter_nls",  cap * sizeof(*nls));

  n = 0;
  for(r = kv_nl_head; r != NULL && n < cap; r = r->next)
  {
    memcpy(keys + n * KV_KEY_SZ, r->key, KV_KEY_SZ);
    nls[n] = r->nl;
    n++;
  }

  pthread_mutex_unlock(&kv_nl_mutex);

  for(i = 0; i < n; i++)
    cb(keys + i * KV_KEY_SZ, nls[i], data);

  mem_free(keys);
  mem_free(nls);
}

void
kv_audit_iterate(kv_audit_cb_t cb, void *data)
{
  if(cb == NULL)
    return;

  pthread_mutex_lock(&kv_mutex);

  for(uint32_t b = 0; b < KV_BUCKETS; b++)
    for(kv_entry_t *e = kv_table[b]; e != NULL; e = e->next)
    {
      cb(e->key, "cb",      fn_addr(&e->cb), data);
      cb(e->key, "cb_data", e->cb_data,      data);
      cb(e->key, "help",    e->help,         data);
    }

  pthread_mutex_unlock(&kv_mutex);

  // NL responders live in their own registry; sweep it separately
  // rather than nesting the two locks.
  pthread_mutex_lock(&kv_nl_mutex);

  for(kv_nl_reg_t *r = kv_nl_head; r != NULL; r = r->next)
    cb(r->key, "nl", r->nl, data);

  pthread_mutex_unlock(&kv_nl_mutex);
}

// Iterate all entries whose key starts with prefix. Calls cb for each.
// Callback is invoked under the KV lock — must NOT call kv_* functions.
uint32_t
kv_iterate_prefix(const char *prefix, kv_iter_cb_t cb, void *data)
{
  size_t   plen;
  uint32_t count = 0;

  if(prefix == NULL || cb == NULL)
    return(0);

  plen = strlen(prefix);

  pthread_mutex_lock(&kv_mutex);

  for(uint32_t b = 0; b < KV_BUCKETS; b++)
  {
    for(kv_entry_t *e = kv_table[b]; e != NULL; e = e->next)
    {
      if(strncmp(e->key, prefix, plen) == 0)
      {
        char val_str[KV_VAL_BUF];

        if(e->secret && !kv_admin_active)
          strlcpy(val_str, KV_REDACTED_VALUE, sizeof(val_str));

        else
          val_to_str(e->type, &e->val, val_str, sizeof(val_str));

        cb(e->key, e->type, val_str, data);
        count++;
      }
    }
  }

  pthread_mutex_unlock(&kv_mutex);
  return(count);
}

uint32_t
kv_delete_prefix(const char *prefix)
{
  size_t   plen;
  uint32_t deleted = 0;

  if(prefix == NULL || prefix[0] == '\0')
    return(0);

  plen = strlen(prefix);

  pthread_mutex_lock(&kv_mutex);

  for(uint32_t b = 0; b < KV_BUCKETS; b++)
  {
    kv_entry_t *e = kv_table[b];
    kv_entry_t *prev = NULL;

    while(e != NULL)
    {
      kv_entry_t *next = e->next;

      if(strncmp(e->key, prefix, plen) == 0)
      {
        if(prev != NULL)
          prev->next = next;
        else
          kv_table[b] = next;

        mem_free(e);
        kv_count--;
        deleted++;
      }

      else
        prev = e;

      e = next;
    }
  }

  pthread_mutex_unlock(&kv_mutex);

  // Delete matching rows from DB if available.
  if(deleted > 0)
  {
    char *esc = db_escape(prefix);

    if(esc != NULL)
    {
      char         sql[512];
      db_result_t *r;

      snprintf(sql, sizeof(sql),
          "DELETE FROM kv WHERE key LIKE '%s%%'", esc);
      mem_free(esc);

      r = db_result_alloc();
      db_query(sql, r);
      db_result_free(r);
    }

    clam(CLAM_DEBUG, "kv_delete_prefix",
        "deleted %u entries with prefix '%s'", deleted, prefix);
  }

  return(deleted);
}

// Drop one key's persisted row. Issued unconditionally by both callers
// below — a 0-row DELETE is harmless when the key was never persisted,
// and it is what lets a DB-only orphan be swept with nothing in memory.
// Caller must NOT hold kv_mutex (this does DB I/O).
static void
kv_row_delete(const char *key)
{
  char *esc = db_escape(key);

  if(esc != NULL)
  {
    char         sql[KV_KEY_SZ + 64];
    db_result_t *r;

    snprintf(sql, sizeof(sql), "DELETE FROM kv WHERE key = '%s'", esc);
    mem_free(esc);

    r = db_result_alloc();
    db_query(sql, r);
    db_result_free(r);
  }
}

// Delete exactly ONE key from the live registry and its persisted row.
// Unlike kv_delete_prefix this matches the whole key, so a janitor can
// drop a single orphan without nuking a namespace. The DB DELETE is issued
// unconditionally (harmless 0-row when the key was never persisted), which
// also lets a DB-only orphan be swept even if it isn't in memory. Returns
// true iff a live in-memory entry was removed.
bool
kv_delete(const char *key)
{
  uint32_t    bucket;
  kv_entry_t *e;
  kv_entry_t *prev = NULL;
  bool        found = false;

  if(key == NULL || key[0] == '\0')
    return(false);

  pthread_mutex_lock(&kv_mutex);

  bucket = hash_key(key);

  for(e = kv_table[bucket]; e != NULL; prev = e, e = e->next)
  {
    if(strcmp(e->key, key) == 0)
    {
      if(prev != NULL)
        prev->next = e->next;
      else
        kv_table[bucket] = e->next;

      mem_free(e);
      kv_count--;
      found = true;
      break;
    }
  }

  pthread_mutex_unlock(&kv_mutex);

  // Drop the persisted row (exact match), whether or not it was live.
  kv_row_delete(key);

  if(found)
    clam(CLAM_INFO, "kv_delete", "deleted key '%s'", key);

  return(found);
}

// Revert a registered key to the default its declaration named, and drop
// its persisted row so the next boot reads that declaration too.
//
// ⚠⚠ The invariant is a NEGATIVE and it is the whole point: `dirty` must
// be FALSE when this returns. cmd_set_kv calls kv_flush() on success and
// kv_flush() persists every dirty entry, so a reset routed through
// apply_val — whose job is to raise that flag — would re-INSERT the row
// it had just deleted, inside the same command. The flag is cleared
// rather than merely left alone, because another writer may have raised
// it earlier and never flushed; that pending write is exactly what a
// revert discards. Nothing reports the failure: the operator sees the
// old value again after a restart and cannot tell why (OBS-50 T4).
//
// The cb-under-released-lock pattern is copied from apply_val for its
// stated reason — a concurrent kv_unregister or a plugin unload's
// kv_reclaim_owned frees the entry — and not called, for the one above.
//
// Returns true iff a registered entry was found, whether or not its
// value differed from the default. An unregistered key is kv_delete's
// subject, not this one: there is no declaration to revert to.
bool
kv_reset(const char *key)
{
  kv_entry_t *e;
  char        k[KV_KEY_SZ];
  bool        changed;
  kv_cb_t     cb;
  void       *cb_data;

  if(key == NULL || key[0] == '\0')
    return(false);

  pthread_mutex_lock(&kv_mutex);

  e = find_locked(key);

  if(e == NULL)
  {
    pthread_mutex_unlock(&kv_mutex);
    return(false);
  }

  changed = val_changed(e->type, &e->val, &e->def);

  if(changed)
    e->val = e->def;

  e->dirty = false;

  cb      = changed ? e->cb : NULL;
  cb_data = e->cb_data;

  strlcpy(k, e->key, sizeof(k));

  pthread_mutex_unlock(&kv_mutex);

  if(cb != NULL)
    cb(k, cb_data);

  kv_row_delete(k);

  clam(CLAM_INFO, "kv_reset", "reset key '%s' to its declared default", k);

  return(true);
}

// Drop the NL responder attached to `key`, if any. Internal helper —
// callers must NOT hold kv_nl_mutex. Returns true if one was removed.
static bool
kv_nl_unregister_locked_key(const char *key)
{
  kv_nl_reg_t  *r;
  kv_nl_reg_t **pp;
  bool          dropped = false;

  pthread_mutex_lock(&kv_nl_mutex);

  pp = &kv_nl_head;

  while(*pp != NULL)
  {
    r = *pp;

    if(strcmp(r->key, key) == 0)
    {
      *pp = r->next;
      mem_free(r);
      dropped = true;
      break;
    }

    pp = &r->next;
  }

  pthread_mutex_unlock(&kv_nl_mutex);
  return(dropped);
}

bool
kv_unregister(const char *key)
{
  kv_entry_t  *e;
  kv_entry_t **pp;
  uint32_t     bucket;
  bool         found = false;

  if(key == NULL || key[0] == '\0')
    return(FAIL);

  bucket = hash_key(key);

  pthread_mutex_lock(&kv_mutex);

  pp = &kv_table[bucket];

  while(*pp != NULL)
  {
    e = *pp;

    if(strcmp(e->key, key) == 0)
    {
      *pp = e->next;
      mem_free(e);
      kv_count--;
      found = true;
      break;
    }

    pp = &e->next;
  }

  pthread_mutex_unlock(&kv_mutex);

  if(found)
  {
    kv_nl_unregister_locked_key(key);
    clam(CLAM_DEBUG, "kv_unregister", "'%s' removed", key);
  }

  return(found ? SUCCESS : FAIL);
}

uint32_t
kv_reclaim_owned(uintptr_t lo, uintptr_t hi)
{
  kv_nl_reg_t **rp;
  kv_entry_t   *doomed  = NULL;   // unlinked, awaiting persist + free
  uint32_t      removed = 0;
  uint32_t      saved   = 0;
  uint32_t      lost    = 0;

  if(lo >= hi)
    return(0);

  pthread_mutex_lock(&kv_mutex);

  for(uint32_t b = 0; b < KV_BUCKETS; b++)
  {
    kv_entry_t *e    = kv_table[b];
    kv_entry_t *prev = NULL;

    while(e != NULL)
    {
      kv_entry_t *next = e->next;
      uintptr_t   pc   = (uintptr_t)e->owner_pc;

      if(pc >= lo && pc < hi)
      {
        if(prev != NULL)
          prev->next = next;
        else
          kv_table[b] = next;

        // OBS-59: unlinked, NOT freed. A dirty entry still owes the
        // database a write, and persist_entry issues a remote query —
        // which must not run under kv_mutex. Reaped below, unlocked.
        e->next = doomed;
        doomed  = e;

        kv_count--;
        removed++;
      }

      else
        prev = e;

      e = next;
    }
  }

  pthread_mutex_unlock(&kv_mutex);

  // OBS-59: this is kv_exit()'s flush, scoped to one mapping and run at
  // the last moment its entries still exist. Until it was added, a value
  // set through kv_set() died here silently: apply_val only marks the
  // entry dirty, kv_flush() is the sole writer, and nothing outside the
  // `set kv` and IRC command surfaces calls it — so a plugin's own
  // kv_set never reached the DB, and this loop freed the evidence. The
  // measured case was whenmoon's strategy binding: `/plugin reload
  // whenmoon` forgot which strategy a market ran, and — because
  // kv_register then adopts the older row — a market the operator had
  // DETACHED came back attached.
  //
  // ⛔ It persists every dirty entry, exactly as kv_flush() does,
  // including a registration default nobody ever set. That is
  // deliberate: this path is a flush, and a second persistence policy
  // that disagreed with kv_flush() about what a dirty entry means would
  // be worse than the write it saves. Measured live on a whenmoon
  // reload: 18 values written, inside the same log second as the rest of
  // the cascade.
  //
  // The entries are off the table, so nothing can reach them and the
  // query below is unsynchronised by construction; the value strings a
  // reader may still hold live in the intern table, never here.
  //
  // A graceful shutdown does not come through here at all — plugin_exit
  // dlcloses without plugin_reclaim, and kv_exit() has already flushed
  // by then (main.c orders kv_exit before plugin_exit). This runs only
  // on an unload, where the database is still open.
  while(doomed != NULL)
  {
    kv_entry_t *dead = doomed;

    doomed = doomed->next;

    if(dead->dirty)
    {
      if(persist_entry(dead) == SUCCESS)
        saved++;

      else
        lost++;
    }

    mem_free(dead);
  }

  // Both lines name a value that a reader would otherwise never learn
  // the fate of: silence here is what the row was filed for.
  if(saved > 0)
    clam(CLAM_INFO, "kv_reclaim",
        "persisted %u unsaved value(s) before unmapping their owner",
        saved);

  if(lost > 0)
    clam(CLAM_WARN, "kv_reclaim",
        "%u unsaved value(s) could NOT be written and are gone with the"
        " mapping", lost);

  // NL responders are keyed by hint pointer rather than by owner: the
  // hint is the only thing the registry retains, and it is exactly what
  // dies at dlclose. Walked outside kv_mutex, per lock ordering.
  pthread_mutex_lock(&kv_nl_mutex);

  rp = &kv_nl_head;

  while(*rp != NULL)
  {
    kv_nl_reg_t *r  = *rp;
    uintptr_t    nl = (uintptr_t)r->nl;

    if(nl >= lo && nl < hi)
    {
      *rp = r->next;
      mem_free(r);
    }

    else
      rp = &r->next;
  }

  pthread_mutex_unlock(&kv_nl_mutex);

  if(removed > 0)
    clam(CLAM_DEBUG, "kv_reclaim", "reclaimed %u entry(s)", removed);

  return(removed);
}

static bool
load_ensure_table(void)
{
  db_result_t *r = db_result_alloc();

  if(db_query("CREATE TABLE IF NOT EXISTS kv ("
               "key TEXT PRIMARY KEY, "
               "type SMALLINT NOT NULL, "
               "value TEXT NOT NULL)", r) != SUCCESS)
  {
    clam(CLAM_WARN, "kv_load", "cannot create kv table: %s", r->error);
    db_result_free(r);
    return(FAIL);
  }

  db_result_free(r);
  return(SUCCESS);
}

// Apply a loaded DB row to an existing registered entry.
// Must be called with kv_mutex held. Always unlocks before returning.
// e: registered entry (must not be NULL)
static bool
load_apply_row(kv_entry_t *e, const char *db_key,
    const char *db_type, const char *db_val)
{
  int      type_id;
  kv_val_t new_val;
  bool     changed;

  type_id = atoi(db_type);

  if(type_id != (int)e->type)
  {
    // Keep the default, but clean — see the matching reasoning in
    // kv_register(): a dirty default would flush over this very row.
    e->dirty = false;
    pthread_mutex_unlock(&kv_mutex);
    // Same case as the kv_register rehydrate path, seen at boot instead of
    // at re-registration: name the row we are declining so the reset is
    // never silent. The row stays put.
    clam(CLAM_WARN, "kv_load",
        "'%s': persisted row is %s '%s' but the schema declares %s — "
        "using default; the row is untouched",
        db_key, kv_type_name((kv_type_t)type_id), db_val,
        kv_type_name(e->type));
    return(false);
  }

  memset(&new_val, 0, sizeof(new_val));

  if(str_to_val(e->type, db_val, &new_val) != SUCCESS)
  {
    e->dirty = false;
    pthread_mutex_unlock(&kv_mutex);
    clam(CLAM_WARN, "kv_load",
        "invalid value for '%s': '%s' — using default; the row is untouched",
        db_key, db_val);
    return(false);
  }

  changed = val_changed(e->type, &e->val, &new_val);

  if(changed)
  {
    kv_cb_t     cb;
    void       *cb_data;
    const char *ekey;

    e->val = new_val;
    e->dirty = false;

    cb      = e->cb;
    cb_data = e->cb_data;
    ekey    = e->key;

    pthread_mutex_unlock(&kv_mutex);

    if(cb != NULL)
      cb(ekey, cb_data);
  }

  else
  {
    e->dirty = false;
    pthread_mutex_unlock(&kv_mutex);
  }

  return(true);
}

bool
kv_load(void)
{
  db_result_t *r;
  uint32_t     loaded = 0;
  uint32_t     skipped = 0;

  if(load_ensure_table() != SUCCESS)
    return(FAIL);

  // Load all rows.
  r = db_result_alloc();

  if(db_query("SELECT key, type, value FROM kv", r) != SUCCESS)
  {
    clam(CLAM_WARN, "kv_load", "cannot load kv entries: %s", r->error);
    db_result_free(r);
    return(FAIL);
  }

  for(uint32_t i = 0; i < r->rows; i++)
  {
    const char *db_key = db_result_get(r, i, 0);
    const char *db_type = db_result_get(r, i, 1);
    const char *db_val = db_result_get(r, i, 2);
    kv_entry_t *e;

    if(db_key == NULL || db_type == NULL || db_val == NULL)
      continue;

    pthread_mutex_lock(&kv_mutex);
    e = find_locked(db_key);

    if(e == NULL)
    {
      // No live entry yet — leave the row in the DB. A later
      // kv_register() rehydrates it on demand, and kv_claim_orphans()
      // materializes whatever stays schema-less after bot restore.
      pthread_mutex_unlock(&kv_mutex);
      skipped++;
      continue;
    }

    // load_apply_row unlocks kv_mutex before returning.
    if(load_apply_row(e, db_key, db_type, db_val))
      loaded++;
    else
      skipped++;
  }

  db_result_free(r);

  clam(CLAM_INFO, "kv_load", "loaded %u entries (%u skipped)", loaded, skipped);
  kv_loaded = true;
  return(SUCCESS);
}

bool
kv_flush(void)
{
  uint32_t flushed = 0;
  uint32_t failed = 0;

  pthread_mutex_lock(&kv_mutex);

  // Refuse to persist before kv_load() has run. Until restore, the table
  // holds only un-rehydrated schema defaults (plus bootstrap values that
  // reload from botman.conf regardless), so flushing would overwrite the
  // authoritative DB rows with those defaults. This is exactly the clobber
  // a failed startup inflicts: when plugin_init_all() aborts before
  // kv_load() (e.g. a plugin init error), the shutdown path still reaches
  // kv_exit() -> kv_flush(). Nothing here is safe to save; skip it.
  if(!kv_loaded)
  {
    pthread_mutex_unlock(&kv_mutex);

    clam(CLAM_INFO, "kv_flush",
        "skipped: kv not loaded (no persist before restore)");
    return(SUCCESS);
  }

  for(uint32_t b = 0; b < KV_BUCKETS; b++)
  {
    for(kv_entry_t *e = kv_table[b]; e != NULL; e = e->next)
    {
      if(!e->dirty)
        continue;

      // Unlock during DB I/O to avoid holding the lock for extended time.
      pthread_mutex_unlock(&kv_mutex);

      if(persist_entry(e) == SUCCESS)
      {
        flushed++;
        pthread_mutex_lock(&kv_mutex);
        e->dirty = false;
      }

      else
      {
        failed++;
        pthread_mutex_lock(&kv_mutex);
      }
    }
  }

  pthread_mutex_unlock(&kv_mutex);

  if(flushed > 0 || failed > 0)
    clam(CLAM_INFO, "kv_flush", "persisted %u entries (%u failed)",
        flushed, failed);

  return((failed == 0) ? SUCCESS : FAIL);
}

// Report the persisted rows that no live entry claims, without touching
// one of them. This is the operator's view of "keys a plugin used to
// have": after an unload, core's Class-A sweep drops the binding and
// deliberately leaves the row, because an unloaded plugin and a retired
// key look identical from here (root TODO.md §PLIFE-4). Pruning therefore
// stays a deliberate act — /db delete kv <key>.
uint32_t
kv_iterate_orphans(kv_orphan_cb_t cb, void *data)
{
  db_result_t *r;
  uint32_t     found = 0;

  if(cb == NULL)
    return(0);

  r = db_result_alloc();

  if(db_query("SELECT key, type, value FROM kv ORDER BY key", r) != SUCCESS)
  {
    clam(CLAM_WARN, "kv_orphans", "db scan failed: %s", r->error);
    db_result_free(r);
    return(0);
  }

  for(uint32_t i = 0; i < r->rows; i++)
  {
    const char *db_key  = db_result_get(r, i, 0);
    const char *db_type = db_result_get(r, i, 1);
    const char *db_val  = db_result_get(r, i, 2);
    bool        live;

    if(db_key == NULL || db_type == NULL || db_val == NULL)
      continue;

    pthread_mutex_lock(&kv_mutex);
    live = (find_locked(db_key) != NULL);
    pthread_mutex_unlock(&kv_mutex);

    if(live)
      continue;

    // An unparseable type id reaches the callback as KV_UNKNOWN's name
    // rather than being hidden — a row nothing can ever claim is the most
    // orphaned row there is.
    cb(db_key, (kv_type_t)atoi(db_type), db_val, data);
    found++;
  }

  db_result_free(r);
  return(found);
}

// Materialize persisted DB rows that no live entry claims — dynamic keys
// with no static schema (per-channel IRC config, etc.). Runs after bot
// restore; schema keys and any key a plugin already re-registered are
// live and skipped. Returns the count materialized.
uint32_t
kv_claim_orphans(void)
{
  db_result_t *r;
  uint32_t     claimed = 0;

  r = db_result_alloc();

  if(db_query("SELECT key, type, value FROM kv", r) != SUCCESS)
  {
    clam(CLAM_WARN, "kv_claim_orphans", "db scan failed: %s", r->error);
    db_result_free(r);
    return(0);
  }

  for(uint32_t i = 0; i < r->rows; i++)
  {
    const char *db_key  = db_result_get(r, i, 0);
    const char *db_type = db_result_get(r, i, 1);
    const char *db_val  = db_result_get(r, i, 2);
    int         type_id;
    bool        live;

    if(db_key == NULL || db_type == NULL || db_val == NULL)
      continue;

    type_id = atoi(db_type);

    if(type_id < 0 || type_id > KV_BOOL)
      continue;  // unknown type id — leave the row untouched in the DB

    pthread_mutex_lock(&kv_mutex);
    live = (find_locked(db_key) != NULL);
    pthread_mutex_unlock(&kv_mutex);

    if(live)
      continue;

    // Register at the persisted value; kv_register re-confirms it from the
    // DB (kv_loaded is true here) and clears the dirty flag.
    if(kv_register(db_key, (kv_type_t)type_id, db_val, NULL, NULL, NULL)
        == SUCCESS)
      claimed++;
  }

  db_result_free(r);

  if(claimed > 0)
    clam(CLAM_DEBUG, "kv_claim_orphans", "claimed %u orphan entries",
        claimed);

  return(claimed);
}

// Initialize the KV configuration subsystem.
// Sets up the mutex, clears the hash table, and marks the system ready.
void
kv_init(void)
{
  pthread_mutex_init(&kv_mutex, NULL);
  memset(kv_table, 0, sizeof(kv_table));
  kv_count = 0;
  kv_ready = true;

  clam(CLAM_INFO, "kv_init", "configuration system initialized");
}

// Shut down the KV configuration subsystem.
// Flushes dirty entries to DB, frees all entries and pending entries,
// and destroys the mutex.
void
kv_exit(void)
{
  uint32_t freed    = 0;
  uint32_t interned = 0;

  if(!kv_ready)
    return;

  // Attempt to flush dirty entries.
  kv_flush();

  // Free all entries.
  pthread_mutex_lock(&kv_mutex);

  for(uint32_t b = 0; b < KV_BUCKETS; b++)
  {
    kv_entry_t *e = kv_table[b];

    while(e != NULL)
    {
      kv_entry_t *next = e->next;

      mem_free(e);
      freed++;
      e = next;
    }

    kv_table[b] = NULL;
  }

  kv_count = 0;

  pthread_mutex_unlock(&kv_mutex);
  pthread_mutex_destroy(&kv_mutex);

  // Free the NL responder registry. NL metadata itself is static /
  // caller-owned (we only free the registry nodes).
  pthread_mutex_lock(&kv_nl_mutex);

  while(kv_nl_head != NULL)
  {
    kv_nl_reg_t *r = kv_nl_head;

    kv_nl_head = r->next;
    mem_free(r);
  }

  pthread_mutex_unlock(&kv_nl_mutex);

  // The intern table outlives every entry by design; shutdown is the one
  // point at which no reader can still hold one of its strings.
  pthread_mutex_lock(&kv_intern_mutex);

  for(uint32_t b = 0; b < KV_INTERN_BUCKETS; b++)
  {
    kv_str_node_t *n = kv_intern_table[b];

    while(n != NULL)
    {
      kv_str_node_t *next = n->next;

      mem_free(n);
      n = next;
    }

    kv_intern_table[b] = NULL;
  }

  interned        = kv_intern_count;
  kv_intern_count = 0;

  pthread_mutex_unlock(&kv_intern_mutex);

  kv_ready = false;

  clam(CLAM_INFO, "kv_exit",
      "configuration shut down (%u entries, %u interned strings freed)",
      freed, interned);
}
