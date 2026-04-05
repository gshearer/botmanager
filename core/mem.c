#define MEM_INTERNAL
#include "mem.h"
#include "cmd.h"
#include "colors.h"
#include "util.h"

// -----------------------------------------------------------------------
// Get a journal entry from the freelist or malloc a new one.
// returns: zeroed journal entry (never NULL — aborts on OOM)
// -----------------------------------------------------------------------
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

// -----------------------------------------------------------------------
// Return a journal entry to the freelist.
// Caller must NOT hold mem_mutex.
// e: journal entry to recycle
// -----------------------------------------------------------------------
static void
journal_put(mem_entry_t *e)
{
  pthread_mutex_lock(&mem_mutex);
  e->next = mem_freelist;
  mem_freelist = e;
  mem_freelist_count++;
  pthread_mutex_unlock(&mem_mutex);
}

// -----------------------------------------------------------------------
// Find and unlink a journal entry by pointer.
// Caller must NOT hold mem_mutex.
// returns: the entry, or NULL if not found
// ptr: heap pointer to look up
// -----------------------------------------------------------------------
static mem_entry_t *
journal_remove(void *ptr)
{
  mem_entry_t *e, *prev = NULL;

  pthread_mutex_lock(&mem_mutex);

  for(e = mem_active; e != NULL; prev = e, e = e->next)
  {
    if(e->ptr == ptr)
    {
      if(prev != NULL)
        prev->next = e->next;

      else
        mem_active = e->next;

      mem_active_count--;
      pthread_mutex_unlock(&mem_mutex);
      return(e);
    }
  }

  pthread_mutex_unlock(&mem_mutex);
  return(NULL);
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

// returns: valid pointer to sz bytes of zeroed memory (aborts on OOM)
// module: name of the requesting module/plugin
// name: label for tracking
// sz: number of bytes to allocate
void *
mem_alloc(const char *module, const char *name, size_t sz)
{
  mem_entry_t *e;
  void *ptr;

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

  pthread_mutex_lock(&mem_mutex);
  e->next = mem_active;
  mem_active = e;
  mem_active_count++;
  mem_total_allocs++;
  mem_heap_sz += sz;

  if(mem_heap_sz > mem_peak_heap_sz)
    mem_peak_heap_sz = mem_heap_sz;

  pthread_mutex_unlock(&mem_mutex);

  return(ptr);
}

// returns: valid pointer to resized memory (aborts on OOM or untracked)
// ptr: previously tracked allocation
// sz: new size in bytes
void *
mem_realloc(void *ptr, size_t sz)
{
  mem_entry_t *e;
  void *newptr;

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

  pthread_mutex_lock(&mem_mutex);
  mem_heap_sz = mem_heap_sz - e->sz + sz;
  e->sz = sz;
  e->ptr = newptr;
  e->next = mem_active;
  mem_active = e;
  mem_active_count++;

  if(mem_heap_sz > mem_peak_heap_sz)
    mem_peak_heap_sz = mem_heap_sz;

  pthread_mutex_unlock(&mem_mutex);

  return(newptr);
}

// returns: duplicated string via tracked memory
// module: name of the requesting module/plugin
// name: label for tracking
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

// Free a tracked allocation. Aborts on null, untracked, or corrupt free.
// ptr: heap pointer to free
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
// out: destination struct for active count, freelist count, and heap size
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

// Walk all active allocations under mem_mutex, invoking cb for each.
// cb: callback receiving module, name, size, timestamp, and user data
// data: opaque pointer passed through to cb
void
mem_iterate(mem_iterate_cb cb, void *data)
{
  pthread_mutex_lock(&mem_mutex);

  for(mem_entry_t *e = mem_active; e != NULL; e = e->next)
    cb(e->module, e->name, e->sz, e->timestamp, data);

  pthread_mutex_unlock(&mem_mutex);
}

// -----------------------------------------------------------------------
// /show memory — memory utilization table
// -----------------------------------------------------------------------

// Iteration callback: aggregate by (module, name).
static void
mem_show_agg_cb(const char *module, const char *name, size_t sz,
    time_t timestamp, void *data)
{
  mem_agg_state_t *st = data;

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

  mem_agg_row_t *r = &st->rows[st->count++];

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


// Command handler for "/show memory". Displays an aggregated table of
// tracked heap allocations grouped by module and name.
// ctx: command context for sending reply lines
static void
mem_cmd_show(const cmd_ctx_t *ctx)
{
  mem_stats_t ms;

  mem_get_stats(&ms);

  char heap_str[16];

  util_fmt_bytes(ms.heap_sz, heap_str, sizeof(heap_str));

  char hdr[256];

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
  mem_agg_state_t st;

  memset(&st, 0, sizeof(st));
  mem_iterate(mem_show_agg_cb, &st);

  qsort(st.rows, st.count, sizeof(mem_agg_row_t), mem_agg_cmp);

  // Header line.
  char line[256];

  snprintf(line, sizeof(line),
      "  " CLR_BOLD "%-18s %-28s %6s %10s" CLR_RESET,
      "MODULE", "NAME", "COUNT", "BYTES");
  cmd_reply(ctx, line);

  // Data rows.
  uint32_t total_count = 0;
  size_t   total_bytes = 0;
  const char *prev_module = "";

  for(uint32_t i = 0; i < st.count; i++)
  {
    mem_agg_row_t *r = &st.rows[i];
    char sz_str[16];

    util_fmt_bytes(r->total_sz, sz_str, sizeof(sz_str));

    // Visual separator when module changes.
    const char *mod_col = CLR_CYAN;

    if(strcmp(r->module, prev_module) != 0)
      prev_module = r->module;
    else
    {
      mod_col = "";
    }

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
  char total_str[16];

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
  cmd_register_system("mem", "memory",
      "show memory",
      "Show memory utilization",
      "Displays a table of tracked heap allocations aggregated\n"
      "by module and allocation name, showing the count and total\n"
      "bytes for each category.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, mem_cmd_show, NULL, "show", "mem", NULL, 0);
}

// Initialize the memory subsystem. Must be called before any mem_alloc.
void
mem_init(void)
{
  pthread_mutex_init(&mem_mutex, NULL);
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

  // Report and free any remaining tracked allocations (leaks).
  e = mem_active;

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

  mem_active = NULL;
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
