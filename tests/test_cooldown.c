// botmanager — MIT
// Cases for cooldown.h, whose every failure is silent: a ring that
// misses a key it holds does not report anything, it just lets the bot
// speak again.
#include "test.h"
#include "cooldown.h"

#include <string.h>

// Small enough that eviction is reachable in four writes, which is what
// the LRU cases need.
#define RING_SLOTS  3

// A key filled to the last usable byte. The composed per-URL key in
// vision.c is the reason the size has margin over METHOD_CHANNEL_SZ,
// and a stamp truncated to fit could never be matched by the peek that
// followed it — so the boundary is a case, not a comment.
static char long_lower[COOLDOWN_KEY_SZ];
static char long_upper[COOLDOWN_KEY_SZ];
static char long_other[COOLDOWN_KEY_SZ];

// Reading: a ring seeded once, then asked the same question every way a
// caller asks it. `floor` is TEXT-COOLDOWN-1's reload floor.
static const struct
{
  const char *name;
  const char *key;
  time_t      floor;
  time_t      want;
} peek_cases[] = {
  { "exact hit",                    "#chan", 0,   100 },
  { "hit folds case",               "#CHAN", 0,   100 },
  { "hit folds case both ways",     "NiCk",  0,   200 },
  { "miss reads as the floor",      "#gone", 500, 500 },
  { "hit older than the floor",     "#chan", 500, 500 },
  { "hit newer than the floor",     "nick",  150, 200 },
  { "miss with no floor",           "#gone", 0,   0   },
  { "empty key is never floored",   "",      500, 0   },
  { "null key is never floored",    NULL,    500, 0   },
};

// Writing. One scenario drives every row: three keys fill an empty
// ring, the first is refreshed to the newest stamp, then a fourth
// arrives and must take the oldest slot. A FIFO write cursor — which is
// what two of these rings ran under a comment claiming LRU — would
// evict the refreshed key instead of the stale one.
static const struct
{
  const char *name;
  const char *key;
  time_t      want;
} evict_cases[] = {
  { "refreshed key kept its slot",   "a", 400 },
  { "oldest key was the one evicted","b", 0   },
  { "untouched key kept its slot",   "c", 300 },
  { "new key took the freed slot",   "d", 500 },
};

int
main(void)
{
  cooldown_slot_t peek_ring[RING_SLOTS];
  cooldown_slot_t evict_ring[RING_SLOTS];
  cooldown_slot_t long_ring[RING_SLOTS];

  memset(peek_ring, 0, sizeof(peek_ring));
  cooldown_ring_stamp(peek_ring, RING_SLOTS, "#chan", 100);
  cooldown_ring_stamp(peek_ring, RING_SLOTS, "nick",  200);

  for(size_t i = 0; i < sizeof(peek_cases) / sizeof(peek_cases[0]); i++)
    test_check_sz("peek", peek_cases[i].name,
        (size_t)peek_cases[i].want,
        (size_t)cooldown_ring_peek(peek_ring, RING_SLOTS,
            peek_cases[i].key, peek_cases[i].floor));

  memset(evict_ring, 0, sizeof(evict_ring));
  cooldown_ring_stamp(evict_ring, RING_SLOTS, "a", 100);
  cooldown_ring_stamp(evict_ring, RING_SLOTS, "b", 200);
  cooldown_ring_stamp(evict_ring, RING_SLOTS, "c", 300);
  cooldown_ring_stamp(evict_ring, RING_SLOTS, "a", 400);
  cooldown_ring_stamp(evict_ring, RING_SLOTS, "d", 500);

  for(size_t i = 0; i < sizeof(evict_cases) / sizeof(evict_cases[0]); i++)
    test_check_sz("evict", evict_cases[i].name,
        (size_t)evict_cases[i].want,
        (size_t)cooldown_ring_peek(evict_ring, RING_SLOTS,
            evict_cases[i].key, 0));

  memset(long_lower, 'a', sizeof(long_lower) - 1);
  memcpy(long_upper, long_lower, sizeof(long_upper));
  memcpy(long_other, long_lower, sizeof(long_other));
  long_upper[0] = 'A';
  long_other[sizeof(long_other) - 2] = 'b';

  memset(long_ring, 0, sizeof(long_ring));
  cooldown_ring_stamp(long_ring, RING_SLOTS, long_lower, 100);
  cooldown_ring_stamp(long_ring, RING_SLOTS, long_other, 200);

  test_check_sz("keylen", "a full-width key round-trips", 100,
      (size_t)cooldown_ring_peek(long_ring, RING_SLOTS, long_lower, 0));
  test_check_sz("keylen", "a full-width key still folds case", 100,
      (size_t)cooldown_ring_peek(long_ring, RING_SLOTS, long_upper, 0));
  test_check_sz("keylen", "the last byte still separates two keys", 200,
      (size_t)cooldown_ring_peek(long_ring, RING_SLOTS, long_other, 0));

  return(test_report("cooldown"));
}
