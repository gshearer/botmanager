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
      strncpy(val->str, str, KV_STR_SZ - 1);
      val->str[KV_STR_SZ - 1] = '\0';
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
      strncpy(buf, val->str, bufsz - 1);
      buf[bufsz - 1] = '\0';
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
  bool        changed;
  kv_cb_t     cb;
  void       *cb_data;
  const char *key;

  changed = val_changed(e->type, &e->val, new_val);

  if(changed)
  {
    e->val = *new_val;
    e->dirty = true;
  }

  cb      = changed ? e->cb : NULL;
  cb_data = e->cb_data;
  key     = e->key;

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

bool
kv_register(const char *key, kv_type_t type, const char *default_val,
    kv_cb_t cb, void *cb_data, const char *help)
{
  kv_val_t       val;
  kv_entry_t    *e;
  uint32_t       bucket;
  kv_pending_t **pp;

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

  pthread_mutex_lock(&kv_mutex);

  // Check for duplicate.
  if(find_locked(key) != NULL)
  {
    pthread_mutex_unlock(&kv_mutex);
    clam(CLAM_WARN, "kv_register", "duplicate key '%s'", key);
    return(FAIL);
  }

  // Allocate and populate entry.
  e = mem_alloc("kv", "entry", sizeof(kv_entry_t));

  strncpy(e->key, key, KV_KEY_SZ - 1);
  e->key[KV_KEY_SZ - 1] = '\0';
  e->type    = type;
  e->val     = val;
  e->cb      = cb;
  e->cb_data = cb_data;
  e->help    = help;
  e->dirty   = true;   // new entries need DB persistence

  // Insert into hash table.
  bucket = hash_key(key);

  e->next = kv_table[bucket];
  kv_table[bucket] = e;
  kv_count++;

  // Check pending list for a DB value loaded before this key existed.
  pp = &kv_pending_list;

  while(*pp != NULL)
  {
    kv_pending_t *p = *pp;

    if(strcmp(p->key, key) == 0)
    {
      if(p->type == (int)type)
      {
        kv_val_t db_val;

        memset(&db_val, 0, sizeof(db_val));

        if(str_to_val(type, p->val_str, &db_val) == SUCCESS)
        {
          e->val   = db_val;
          e->dirty = false;

          clam(CLAM_DEBUG, "kv_register",
              "'%s' (%s) = %s [restored from db]",
              key, kv_type_name(type), p->val_str);
        }
      }

      // Remove from pending list.
      *pp = p->next;
      mem_free(p);
      kv_pending_count--;
      goto done;
    }

    pp = &(*pp)->next;
  }

  clam(CLAM_DEBUG, "kv_register", "'%s' (%s) = %s",
      key, kv_type_name(type), default_val);

done:
  pthread_mutex_unlock(&kv_mutex);
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

uint64_t
kv_get_uint(const char *key)
{
  uint64_t    result = 0;
  kv_entry_t *e;

  pthread_mutex_lock(&kv_mutex);
  e = find_locked(key);

  if(e != NULL)
  {
    switch(e->type)
    {
      case KV_UINT8:  result = e->val.u8;               break;
      case KV_UINT16: result = e->val.u16;              break;
      case KV_UINT32: result = e->val.u32;              break;
      case KV_UINT64: result = e->val.u64;              break;
      case KV_INT8:   result = (uint64_t)e->val.i8;    break;
      case KV_INT16:  result = (uint64_t)e->val.i16;   break;
      case KV_INT32:  result = (uint64_t)e->val.i32;   break;
      case KV_INT64:  result = (uint64_t)e->val.i64;   break;
      case KV_BOOL:   result = (uint64_t)e->val.u8;    break;
      default: break;
    }
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

  pthread_mutex_lock(&kv_mutex);
  e = find_locked(key);

  if(e != NULL && e->type == KV_STR)
    result = e->val.str;

  pthread_mutex_unlock(&kv_mutex);

  return(result);
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
  strncpy(r->key, key, KV_KEY_SZ - 1);
  r->key[KV_KEY_SZ - 1] = '\0';
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

// Cache a DB row into the pending list for later kv_register() pickup.
// Must be called without kv_mutex held.
static void
load_cache_pending(const char *db_key, const char *db_type, const char *db_val)
{
  kv_pending_t *p = mem_alloc("kv", "pending", sizeof(kv_pending_t));

  strncpy(p->key, db_key, KV_KEY_SZ - 1);
  p->key[KV_KEY_SZ - 1] = '\0';
  p->type = atoi(db_type);
  strncpy(p->val_str, db_val, KV_STR_SZ - 1);
  p->val_str[KV_STR_SZ - 1] = '\0';
  p->next = kv_pending_list;
  kv_pending_list = p;
  kv_pending_count++;
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
    pthread_mutex_unlock(&kv_mutex);
    clam(CLAM_WARN, "kv_load",
        "type mismatch for '%s': registered %s, db %d",
        db_key, kv_type_name(e->type), type_id);
    return(false);
  }

  memset(&new_val, 0, sizeof(new_val));

  if(str_to_val(e->type, db_val, &new_val) != SUCCESS)
  {
    pthread_mutex_unlock(&kv_mutex);
    clam(CLAM_WARN, "kv_load",
        "invalid value for '%s': '%s'", db_key, db_val);
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
      pthread_mutex_unlock(&kv_mutex);
      load_cache_pending(db_key, db_type, db_val);
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
  return(SUCCESS);
}

bool
kv_flush(void)
{
  uint32_t flushed = 0;
  uint32_t failed = 0;

  pthread_mutex_lock(&kv_mutex);

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

// Register all remaining pending DB entries into the KV hash table.
// Called after bot restore to claim dynamic keys (e.g., channel config)
// that have no static schema registration.
uint32_t
kv_claim_pending(void)
{
  uint32_t claimed = 0;

  // Walk the pending list, registering each entry.
  // kv_register will consume matching entries from the list.
  while(kv_pending_list != NULL)
  {
    kv_pending_t *p = kv_pending_list;

    if(p->type >= 0 && p->type <= KV_BOOL)
    {
      kv_register(p->key, (kv_type_t)p->type, p->val_str, NULL, NULL,
          NULL);
      claimed++;
    }

    else
    {
      // kv_register didn't consume it (unknown type); remove manually.
      kv_pending_list = p->next;
      mem_free(p);
      kv_pending_count--;
    }
  }

  if(claimed > 0)
    clam(CLAM_DEBUG, "kv_claim_pending",
        "claimed %u pending entries", claimed);

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
  uint32_t freed = 0;

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

  // Free any remaining pending entries (all should be claimed by now).
  if(kv_pending_count > 0)
    clam(CLAM_DEBUG, "kv_exit",
        "%u unclaimed pending entries", kv_pending_count);

  // Free unclaimed pending entries.
  while(kv_pending_list != NULL)
  {
    kv_pending_t *p = kv_pending_list;

    kv_pending_list = p->next;
    mem_free(p);
  }

  kv_pending_count = 0;

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

  kv_ready = false;

  clam(CLAM_INFO, "kv_exit", "configuration shut down (%u entries freed)",
      freed);
}
