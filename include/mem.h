#ifndef BM_MEM_H
#define BM_MEM_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

// returns: valid pointer to sz bytes of zeroed memory (aborts on OOM)
// module: name of the requesting module/plugin
// name: label for tracking
// sz: number of bytes to allocate
void *mem_alloc(const char *module, const char *name, size_t sz);

// returns: valid pointer to resized memory (aborts on OOM or untracked)
// ptr: previously tracked allocation
// sz: new size in bytes
void *mem_realloc(void *ptr, size_t sz);

// returns: duplicated string via tracked memory
// module: name of the requesting module/plugin
// name: label for tracking
// s: source string to duplicate
char *mem_strdup(const char *module, const char *name, const char *s);

// Free a tracked allocation. Aborts on null, untracked, or corrupt free.
// ptr: heap pointer to free
void mem_free(void *ptr);

// Memory statistics (thread-safe snapshot).
typedef struct
{
  uint64_t active;        // tracked allocations
  uint64_t freelist;      // recycled journal entries
  size_t   heap_sz;       // total tracked heap bytes
  size_t   peak_heap_sz;  // high-water mark of heap_sz
  uint64_t total_allocs;  // lifetime allocation count including freed
} mem_stats_t;

// Get current memory statistics (thread-safe snapshot).
// out: destination for the snapshot
void mem_get_stats(mem_stats_t *out);

// Iteration callback for walking active allocations.
// module: requesting module name
// name: allocation label
// sz: allocation size in bytes
// timestamp: time of allocation
// data: opaque user data
typedef void (*mem_iterate_cb)(const char *module, const char *name,
    size_t sz, time_t timestamp, void *data);

// Walk all active allocations under mem_mutex.
// cb: callback invoked for each entry
// data: opaque user data passed to cb
void mem_iterate(mem_iterate_cb cb, void *data);

// Register /show memory subcommand. Must be called after admin_init().
void mem_register_commands(void);

// Initialize the memory subsystem. Must be called before any mem_alloc.
void mem_init(void);

// Shut down the memory subsystem. Reports any unfreed allocations.
void mem_exit(void);

#ifdef MEM_INTERNAL

#include "common.h"

// Journal entry: tracks one heap allocation.
#define MEM_NAME_SZ   30
#define MEM_MODULE_SZ 20

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

// Aggregated row: one per unique (module, name) pair.
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
