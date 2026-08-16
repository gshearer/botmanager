// botmanager — MIT
// Fixed-slot cooldown rings: when did this last happen here?

#define COOLDOWN_INTERNAL
#include "cooldown.h"

#include <string.h>
#include <strings.h>

// Both helpers speak in indices rather than pointers so one
// implementation serves the const peek and the mutating stamp alike.
// `n_slots` is the not-found index.
static size_t
cooldown_ring_find(const cooldown_slot_t *slots, size_t n_slots,
    const char *key)
{
  for(size_t i = 0; i < n_slots; i++)
    if(slots[i].key[0] != '\0' && strcasecmp(slots[i].key, key) == 0)
      return(i);

  return(n_slots);
}

// The slot the next stamp takes when the key is new: the first free
// one, else the oldest. Always a real index — a fixed table with no
// free slot is still evictable, which is what lets a caller treat a
// full ring and an empty one alike.
static size_t
cooldown_ring_evict(const cooldown_slot_t *slots, size_t n_slots)
{
  size_t lru = 0;

  for(size_t i = 0; i < n_slots; i++)
  {
    if(slots[i].key[0] == '\0')
      return(i);

    if(slots[i].stamp < slots[lru].stamp)
      lru = i;
  }

  return(lru);
}

time_t
cooldown_ring_peek(const cooldown_slot_t *slots, size_t n_slots,
    const char *key, time_t floor_at)
{
  size_t hit;

  if(key == NULL || key[0] == '\0') return(0);

  hit = cooldown_ring_find(slots, n_slots, key);

  if(hit == n_slots || slots[hit].stamp < floor_at)
    return(floor_at);

  return(slots[hit].stamp);
}

void
cooldown_ring_stamp(cooldown_slot_t *slots, size_t n_slots,
    const char *key, time_t now)
{
  size_t slot;

  if(n_slots == 0 || key == NULL || key[0] == '\0') return;

  slot = cooldown_ring_find(slots, n_slots, key);

  if(slot != n_slots)
  {
    slots[slot].stamp = now;
    return;
  }

  slot = cooldown_ring_evict(slots, n_slots);

  strlcpy(slots[slot].key, key, sizeof(slots[slot].key));
  slots[slot].stamp = now;
}
