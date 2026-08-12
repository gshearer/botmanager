#ifndef BM_FACT_VOCAB_H
#define BM_FACT_VOCAB_H

#include "memory.h"

#include <stdbool.h>
#include <stddef.h>

// The canonical fact vocabulary — the single authority on which fact
// keys mean the same thing every time they are written.
//
// Why it exists: the extractor mints keys freely, so the "same" fact
// landed under a new (kind, fact_key) almost every time somebody
// restated it — one dossier carried six rows about the same children
// under four keys and two kinds. A correction can only land on a row it
// can FIND, so key drift quietly disables every merge rule the store
// has. Both writers read this table: the extractor's system prompt is
// rendered from it, and the /remember verbs validate against it.
//
// Free-form keys stay legal for everything else — that is where a
// persona's charm lives — steered to one to three lowercase words
// joined by underscores. Deliberately NOT canonicalized: political,
// religious and worldview content (that stays free-form opinion, not a
// field), and the three prefix families that already have owners
// (`city_of_interest:*`, `upcoming_event:*`, `toward:*`).
//
// `hint` is the value shape, written to be read by a model in the
// prompt AND by a human in a `/remember` refusal — keep it one short
// phrase in that voice.

typedef struct
{
  const char      *key;
  mem_fact_kind_t  kind;
  const char      *hint;
} fact_vocab_t;

// Case-insensitive exact match. NULL when the key is not canonical,
// which is not an error anywhere except the /remember verbs.
const fact_vocab_t *fact_vocab_lookup(const char *key);

size_t              fact_vocab_count(void);
const fact_vocab_t *fact_vocab_at(size_t i);

#endif // BM_FACT_VOCAB_H
