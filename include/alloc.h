#ifndef BM_MEM_H
#define BM_MEM_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Returns a pointer to sz bytes of zeroed memory. Aborts on OOM.
void *mem_alloc(const char *module, const char *name, size_t sz);

// Aborts on OOM or untracked pointer.
void *mem_realloc(void *ptr, size_t sz);

char *mem_strdup(const char *module, const char *name, const char *s);

// Aborts on null, untracked, or corrupt free.
void mem_free(void *ptr);

typedef struct
{
  uint64_t active;        // tracked allocations
  uint64_t freelist;      // recycled journal entries
  size_t   heap_sz;       // total tracked heap bytes
  size_t   peak_heap_sz;  // high-water mark of heap_sz
  uint64_t total_allocs;  // lifetime allocation count including freed
} mem_stats_t;

void mem_get_stats(mem_stats_t *out);

typedef void (*mem_iterate_cb)(const char *module, const char *name,
    size_t sz, time_t timestamp, void *data);

// Walks under mem_mutex.
void mem_iterate(mem_iterate_cb cb, void *data);

// Must be called after admin_init().
void mem_register_commands(void);

// Must be called before any mem_alloc.
void mem_init(void);

// Reports any unfreed allocations.
void mem_exit(void);

#ifdef MEM_INTERNAL

#include "common.h"

#define MEM_NAME_SZ   30
#define MEM_MODULE_SZ 20

// Tracks one heap allocation.
typedef struct mem_entry
{
  time_t           timestamp;
  size_t           sz;
  char             module[MEM_MODULE_SZ];
  char             name[MEM_NAME_SZ];
  void            *ptr;
  struct mem_entry *next;
} mem_entry_t;

static pthread_mutex_t mem_mutex;
static mem_entry_t    *mem_active   = NULL;  // tracked allocations
static mem_entry_t    *mem_freelist = NULL;  // recycled journal entries
static uint64_t        mem_active_count   = 0;
static uint64_t        mem_freelist_count = 0;
static size_t          mem_heap_sz        = 0;
static size_t          mem_peak_heap_sz   = 0;
static uint64_t        mem_total_allocs   = 0;
static bool            mem_ready    = false;

// One per unique (module, name) pair.
#define MEM_SHOW_MAX 256

typedef struct
{
  char     module[MEM_MODULE_SZ];
  char     name[MEM_NAME_SZ];
  uint32_t count;
  size_t   total_sz;
} mem_agg_row_t;

typedef struct
{
  mem_agg_row_t rows[MEM_SHOW_MAX];
  uint32_t      count;
} mem_agg_state_t;

#endif // MEM_INTERNAL

#endif // BM_MEM_H
