// botmanager — MIT
// Tracked allocator wrappers (mem_alloc / mem_free) with diagnostics.
#define MEM_INTERNAL
#include "alloc.h"
#include "cmd.h"
#include "colors.h"
#include "util.h"

// Fibonacci hash on the pointer. Shifts off the 16-byte malloc alignment
// before spreading with the golden-ratio multiplier; takes the top
// MEM_HASH_BITS of the 64-bit product.
static inline size_t
mem_hash(const void *ptr)
{
  uintptr_t v = (uintptr_t)ptr >> 4;
  return((size_t)((v * 0x9E3779B97F4A7C15ull) >> (64 - MEM_HASH_BITS)));
}

static mem_entry_t *
journal_get(void)
{
  mem_entry_t *e;

  pthread_mutex_lock(&mem_mutex);

  if(mem_freelist != NULL)
  {
    e = mem_freelist;
    mem_freelist = e->next;
    mem_freelist_count--;
    pthread_mutex_unlock(&mem_mutex);
    memset(e, 0, sizeof(*e));
    return(e);
  }

  pthread_mutex_unlock(&mem_mutex);

  e = malloc(sizeof(*e));

  if(e == NULL)
  {
    fprintf(stderr, "[FATAL] mem: OOM allocating journal entry (%zu bytes)\n",
        sizeof(*e));
    abort();
  }

  memset(e, 0, sizeof(*e));
  return(e);
}

// Return a journal entry to the freelist.
// Caller must NOT hold mem_mutex.
static void
journal_put(mem_entry_t *e)
{
  pthread_mutex_lock(&mem_mutex);
  e->next = mem_freelist;
  mem_freelist = e;
  mem_freelist_count++;
  pthread_mutex_unlock(&mem_mutex);
}

// Find and unlink a journal entry by pointer.
// Caller must NOT hold mem_mutex.
// returns: the entry, or NULL if not found
static mem_entry_t *
journal_remove(void *ptr)
{
  size_t       bucket = mem_hash(ptr);
  mem_entry_t *e, *prev = NULL;

  pthread_mutex_lock(&mem_mutex);

  for(e = mem_buckets[bucket]; e != NULL; prev = e, e = e->next)
  {
    if(e->ptr == ptr)
    {
      if(prev != NULL)
        prev->next = e->next;

      else
        mem_buckets[bucket] = e->next;

      mem_active_count--;
      pthread_mutex_unlock(&mem_mutex);
      return(e);
    }
  }

  pthread_mutex_unlock(&mem_mutex);
  return(NULL);
}

// Public API

void *
mem_alloc(const char *module, const char *name, size_t sz)
{
  mem_entry_t *e;
  void        *ptr;
  size_t       bucket;

  if(!mem_ready)
  {
    fprintf(stderr, "[FATAL] mem_alloc: memory journal not initialized\n");
    abort();
  }

  if(sz == 0)
  {
    fprintf(stderr, "[FATAL] mem_alloc: zero-size request by '%s/%s'\n",
        module, name);
    abort();
  }

  ptr = malloc(sz);

  if(ptr == NULL)
  {
    fprintf(stderr, "[FATAL] mem_alloc: OOM: '%s/%s' requested %zu bytes\n",
        module, name, sz);
    abort();
  }

  memset(ptr, 0, sz);

  e = journal_get();
  e->sz = sz;
  e->ptr = ptr;
  e->timestamp = time(NULL);
  strncpy(e->module, module, MEM_MODULE_SZ - 1);
  strncpy(e->name, name, MEM_NAME_SZ - 1);

  bucket = mem_hash(ptr);

  pthread_mutex_lock(&mem_mutex);
  e->next = mem_buckets[bucket];
  mem_buckets[bucket] = e;
  mem_active_count++;
  mem_total_allocs++;
  mem_heap_sz += sz;

  if(mem_heap_sz > mem_peak_heap_sz)
    mem_peak_heap_sz = mem_heap_sz;

  pthread_mutex_unlock(&mem_mutex);

  return(ptr);
}

void *
mem_realloc(void *ptr, size_t sz)
{
  mem_entry_t *e;
  void        *newptr;
  size_t       bucket;

  if(!mem_ready)
  {
    fprintf(stderr, "[FATAL] mem_realloc: memory journal not initialized\n");
    abort();
  }

  if(ptr == NULL)
  {
    fprintf(stderr, "[FATAL] mem_realloc: null pointer\n");
    abort();
  }

  if(sz == 0)
  {
    fprintf(stderr, "[FATAL] mem_realloc: zero-size realloc\n");
    abort();
  }

  e = journal_remove(ptr);

  if(e == NULL)
  {
    fprintf(stderr, "[FATAL] mem_realloc: untracked pointer %p\n", ptr);
    abort();
  }

  newptr = realloc(ptr, sz);

  if(newptr == NULL)
  {
    fprintf(stderr, "[FATAL] mem_realloc: OOM: '%s/%s' requested %zu bytes\n",
        e->module, e->name, sz);
    abort();
  }

  bucket = mem_hash(newptr);

  pthread_mutex_lock(&mem_mutex);
  mem_heap_sz = mem_heap_sz - e->sz + sz;
  e->sz = sz;
  e->ptr = newptr;
  e->next = mem_buckets[bucket];
  mem_buckets[bucket] = e;
  mem_active_count++;

  if(mem_heap_sz > mem_peak_heap_sz)
    mem_peak_heap_sz = mem_heap_sz;

  pthread_mutex_unlock(&mem_mutex);

  return(newptr);
}

// s: source string to duplicate (must not be NULL)
char *
mem_strdup(const char *module, const char *name, const char *s)
{
  size_t len;
  char *dup;

  if(s == NULL)
  {
    fprintf(stderr, "[FATAL] mem_strdup: null string by '%s/%s'\n",
        module, name);
    abort();
  }

  len = strlen(s) + 1;
  dup = mem_alloc(module, name, len);
  memcpy(dup, s, len);
  return(dup);
}

void
mem_free(void *ptr)
{
  mem_entry_t *e;

  if(ptr == NULL)
  {
    fprintf(stderr, "[FATAL] mem_free: attempt to free null pointer\n");
    abort();
  }

  e = journal_remove(ptr);

  if(e == NULL)
  {
    fprintf(stderr, "[FATAL] mem_free: attempt to free untracked pointer %p\n",
        ptr);
    abort();
  }

  pthread_mutex_lock(&mem_mutex);

  if(e->sz > mem_heap_sz)
  {
    pthread_mutex_unlock(&mem_mutex);
    fprintf(stderr,
        "[FATAL] mem_free: heap corruption: '%s/%s' sz=%zu heap=%zu\n",
        e->module, e->name, e->sz, mem_heap_sz);
    abort();
  }

  mem_heap_sz -= e->sz;
  pthread_mutex_unlock(&mem_mutex);

  free(ptr);
  journal_put(e);
}

// Populate a thread-safe snapshot of current memory statistics.
void
mem_get_stats(mem_stats_t *out)
{
  pthread_mutex_lock(&mem_mutex);
  out->active       = mem_active_count;
  out->freelist     = mem_freelist_count;
  out->heap_sz      = mem_heap_sz;
  out->peak_heap_sz = mem_peak_heap_sz;
  out->total_allocs = mem_total_allocs;
  pthread_mutex_unlock(&mem_mutex);
}

void
mem_iterate(mem_iterate_cb cb, void *data)
{
  pthread_mutex_lock(&mem_mutex);

  for(size_t i = 0; i < MEM_HASH_BUCKETS; i++)
  {
    for(mem_entry_t *e = mem_buckets[i]; e != NULL; e = e->next)
      cb(e->module, e->name, e->sz, e->timestamp, data);
  }

  pthread_mutex_unlock(&mem_mutex);
}

// /show memory — memory utilization table

// Iteration callback: aggregate by (module, name).
static void
mem_show_agg_cb(const char *module, const char *name, size_t sz,
    time_t timestamp, void *data)
{
  mem_agg_state_t *st = data;
  mem_agg_row_t   *r;

  (void)timestamp;

  for(uint32_t i = 0; i < st->count; i++)
  {
    if(strcmp(st->rows[i].module, module) == 0 &&
       strcmp(st->rows[i].name, name) == 0)
    {
      st->rows[i].count++;
      st->rows[i].total_sz += sz;
      return;
    }
  }

  if(st->count >= MEM_SHOW_MAX)
    return;

  r = &st->rows[st->count++];

  strncpy(r->module, module, MEM_MODULE_SZ - 1);
  strncpy(r->name, name, MEM_NAME_SZ - 1);
  r->count = 1;
  r->total_sz = sz;
}

// qsort comparator: sort by module, then name.
static int
mem_agg_cmp(const void *a, const void *b)
{
  const mem_agg_row_t *ra = a;
  const mem_agg_row_t *rb = b;
  int rc = strcmp(ra->module, rb->module);

  if(rc != 0)
    return(rc);

  return(strcmp(ra->name, rb->name));
}

static void
mem_cmd_show(const cmd_ctx_t *ctx)
{
  mem_stats_t     ms;
  char            heap_str[16];
  char            hdr[256];
  mem_agg_state_t st;
  char            line[256];
  uint32_t        total_count = 0;
  size_t          total_bytes = 0;
  const char     *prev_module = "";
  char            total_str[16];

  mem_get_stats(&ms);
  util_fmt_bytes(ms.heap_sz, heap_str, sizeof(heap_str));

  snprintf(hdr, sizeof(hdr),
      CLR_BOLD "memory:" CLR_RESET
      " %s total, %lu allocs, %lu freelist entries",
      heap_str, (unsigned long)ms.active, (unsigned long)ms.freelist);

  cmd_reply(ctx, hdr);

  if(ms.active == 0)
  {
    cmd_reply(ctx, "  (no active allocations)");
    return;
  }

  // Aggregate entries by (module, name).
  memset(&st, 0, sizeof(st));
  mem_iterate(mem_show_agg_cb, &st);

  qsort(st.rows, st.count, sizeof(mem_agg_row_t), mem_agg_cmp);

  // Header line.
  snprintf(line, sizeof(line),
      "  " CLR_BOLD "%-18s %-28s %6s %10s" CLR_RESET,
      "MODULE", "NAME", "COUNT", "BYTES");
  cmd_reply(ctx, line);

  // Data rows.
  for(uint32_t i = 0; i < st.count; i++)
  {
    mem_agg_row_t *r = &st.rows[i];
    char            sz_str[16];
    const char     *mod_col = CLR_CYAN;

    util_fmt_bytes(r->total_sz, sz_str, sizeof(sz_str));

    // Visual separator when module changes.
    if(strcmp(r->module, prev_module) != 0)
      prev_module = r->module;
    else
      mod_col = "";

    snprintf(line, sizeof(line),
        "  %s%-18s" CLR_RESET " %-28s %6u %10s",
        mod_col,
        (mod_col[0] != '\0') ? r->module : "",
        r->name, r->count, sz_str);

    cmd_reply(ctx, line);

    total_count += r->count;
    total_bytes += r->total_sz;
  }

  // Total row.
  util_fmt_bytes(total_bytes, total_str, sizeof(total_str));

  snprintf(line, sizeof(line),
      "  " CLR_BOLD "%-18s %-28s %6u %10s" CLR_RESET,
      "TOTAL", "", total_count, total_str);

  cmd_reply(ctx, line);
}

// Register the "/show memory" command. Must be called after cmd_init().
void
mem_register_commands(void)
{
  cmd_register("mem", "memory",
      "show memory",
      "Show memory utilization",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      mem_cmd_show, NULL, "show", "mem", NULL, 0, NULL, NULL);
}

// Initialize the memory subsystem. Must be called before any mem_alloc.
void
mem_init(void)
{
  pthread_mutex_init(&mem_mutex, NULL);

  mem_buckets = calloc(MEM_HASH_BUCKETS, sizeof(*mem_buckets));

  if(mem_buckets == NULL)
  {
    fprintf(stderr,
        "[FATAL] mem_init: OOM allocating hash table (%zu bytes)\n",
        (size_t)(MEM_HASH_BUCKETS * sizeof(*mem_buckets)));
    abort();
  }

  mem_ready = true;
  fprintf(stdout, "[INFO] mem_init: memory manager initialized\n");
}

// Shut down the memory subsystem. Reports and frees any leaked allocations,
// then frees all freelist entries and destroys the mutex.
void
mem_exit(void)
{
  mem_entry_t *e, *next;
  uint64_t leaked = 0;

  if(!mem_ready)
    return;

  mem_ready = false;

  // Walk every bucket; report and free any remaining tracked allocations.
  for(size_t i = 0; i < MEM_HASH_BUCKETS; i++)
  {
    e = mem_buckets[i];

    while(e != NULL)
    {
      next = e->next;
      leaked++;
      fprintf(stderr,
          "[WARN] mem_exit: LEAK: '%s/%s' %zu bytes (allocated at %ld)\n",
          e->module, e->name, e->sz, (long)e->timestamp);
      mem_heap_sz -= e->sz;
      free(e->ptr);
      free(e);
      e = next;
    }

    mem_buckets[i] = NULL;
  }

  free(mem_buckets);
  mem_buckets = NULL;
  mem_active_count = 0;

  // Free the freelist entries themselves.
  e = mem_freelist;

  while(e != NULL)
  {
    next = e->next;
    free(e);
    e = next;
  }

  mem_freelist = NULL;
  mem_freelist_count = 0;

  if(leaked > 0)
    fprintf(stderr, "[WARN] mem_exit: %lu allocation(s) leaked\n",
        (unsigned long)leaked);

  fprintf(stdout, "[INFO] mem_exit: memory manager shut down (heap: %zu)\n",
      mem_heap_sz);

  pthread_mutex_destroy(&mem_mutex);
}
