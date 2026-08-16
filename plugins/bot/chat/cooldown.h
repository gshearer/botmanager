#ifndef CHATBOT_COOLDOWN_H
#define CHATBOT_COOLDOWN_H

#include "method.h"

#include <stddef.h>
#include <time.h>

// A cooldown ring answers one question — "when did this last happen
// here?" — over a fixed table of (key, stamp) slots scanned linearly.
// Five of them live in this plugin: the reply cooldown and the
// witness-interject budget on chatbot_state_t, vision's channel
// throttle and per-URL dedup, and the volunteer subject ring.
//
// Three properties are the reason this is one module and not five
// hand-rolled copies:
//
// - NOTHING HERE LOCKS. The owner picks the lock, because the five
//   sit under five different ones: a shared rwlock (st->lock, read for
//   peek and write for stamp), three dedicated mutexes, and one mutex
//   shared with three sibling rings that are NOT cooldown rings. A ring
//   that owned a mutex would fit three of the five.
//
// - MATCHING FOLDS CASE. Every key here is an IRC channel or nick, and
//   both are case-insensitive per RFC 2812, so `#BotMan` and `#botman`
//   are one target and must be one slot. Three of the five rings used
//   strcmp before this module and gave a differently-cased spelling of
//   the same target its own untouched cooldown.
//
// - EVICTION IS LRU. Refresh the matching slot, else take the first
//   free one, else take the oldest stamp. Two of the five advertised
//   LRU in their comments and ran a FIFO write cursor, which evicts
//   the slot stamped a second ago while keeping one stamped an hour
//   ago — the eviction path is live for any bot whose distinct DM
//   senders plus channels outnumber its slots.
//
// A key longer than COOLDOWN_KEY_SZ truncates, and a truncated stamp
// can never be matched by the peek that follows it — the ring goes
// permanently cold and churns a slot per call. So the size is one
// number in one place and every producer's buffer is declared with it;
// the widest key in the tree (vision's "<target>:<fnv16-of-url>") is
// what sets the margin over METHOD_CHANNEL_SZ.
#define COOLDOWN_KEY_SZ (METHOD_CHANNEL_SZ + 32)

typedef struct
{
  char    key[COOLDOWN_KEY_SZ];   // empty = slot never used
  time_t  stamp;
} cooldown_slot_t;

// Returns the key's stamp, or `floor_at` when the ring has never seen
// it. TEXT-COOLDOWN-1: `floor_at` is the reload floor — a ring lives in
// the bot handle's mapping, so a /plugin reload empties it and every
// caller's `now - last > cooldown` test would read the emptiness as
// "never spoke here" and speak at once. Pass st->created_at to serve
// out one window of quiet first, or 0 for a ring that is a dedup rather
// than a throttle (vision's per-URL ring is the one such case, and its
// call site says why). An empty or NULL key returns 0 unfloored: it
// names no target, so there is nothing to be quiet towards.
time_t cooldown_ring_peek(const cooldown_slot_t *slots, size_t n_slots,
    const char *key, time_t floor_at);

void cooldown_ring_stamp(cooldown_slot_t *slots, size_t n_slots,
    const char *key, time_t now);

#ifdef COOLDOWN_INTERNAL

static size_t cooldown_ring_find(const cooldown_slot_t *, size_t, const char *);
static size_t cooldown_ring_evict(const cooldown_slot_t *, size_t);

#endif // COOLDOWN_INTERNAL

#endif // CHATBOT_COOLDOWN_H
