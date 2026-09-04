// botmanager — MIT
// Cases for the corpus binding a bot carries in `bot.<n>.corpus`:
// `name[:weight];name[:weight];…`, parsed into the multipliers both
// retrieval channels scale their scores by.
//
// The bytes are a KV value a human typed, and every reading here fails
// SILENTLY when it is wrong — a misread weight does not error, it just
// answers the channel out of the wrong corpus for as long as nobody
// looks. Two pairs of readings are outright opposite: a weight that
// could not be read at all against one written as zero, and a name
// carrying a colon against a name plus a weight.
//
// The unit is pure — no database, no allocator, no lock — so
// knowledge_rank.c compiles straight into this binary.

#include "test.h"

#include "knowledge_priv.h"

#include <stdio.h>
#include <string.h>

#define SUITE "knowledge_corpus"

// One binding string and what it must parse to. `want` lists the
// entries in order; an empty name ends the list.
typedef struct
{
  const char *name;
  const char *list;
  size_t      want_n;
  struct
  {
    const char *corpus;
    float       weight;
  } want[4];
} parse_case_t;

static const parse_case_t parse_cases[] = {
  { "bare names take the default weight",
    "archwiki;hedgehogg_acquired", 2,
    { { "archwiki", 1.0f }, { "hedgehogg_acquired", 1.0f } } },

  { "a written weight applies to its own name only",
    "archwiki:0.4;hedgehogg_acquired", 2,
    { { "archwiki", 0.4f }, { "hedgehogg_acquired", 1.0f } } },

  // The two readings are opposite and only one of them is recoverable:
  // a weight nobody wrote must not silently disable the corpus. No
  // corpus name ends in a colon, so the name loses it.
  { "a trailing colon is an unwritten weight, not a zero",
    "archwiki:", 1, { { "archwiki", 1.0f } } },

  { "a colon with only whitespace behind it is the same",
    "archwiki: ", 1, { { "archwiki", 1.0f } } },

  // Here the colon could have been part of the name, and that reading
  // is the safe one: a corpus that exists stays reachable.
  { "an unreadable weight is an unwritten one",
    "archwiki:abc", 1, { { "archwiki:abc", 1.0f } } },

  // Splitting on the LAST colon is what keeps a corpus somebody already
  // created with one in its name reachable.
  { "the split is on the last colon",
    "ns:archwiki:2", 1, { { "ns:archwiki", 2.0f } } },

  { "a negative weight clamps to nothing retrieved",
    "archwiki:-1", 1, { { "archwiki", 0.0f } } },

  { "an oversized weight clamps to the ceiling",
    "archwiki:9.5", 1, { { "archwiki", 4.0f } } },

  // strtof reads "nan" happily, and a NaN multiplier would poison every
  // score the corpus produced rather than failing anywhere visible.
  { "a NaN weight clamps to nothing retrieved",
    "archwiki:nan", 1, { { "archwiki", 0.0f } } },

  { "whitespace around the name and the colon is trimmed",
    " archwiki : 0.4 ; hedgehogg_acquired ", 2,
    { { "archwiki", 0.4f }, { "hedgehogg_acquired", 1.0f } } },

  { "empty entries are skipped",
    ";;archwiki;;", 1, { { "archwiki", 1.0f } } },

  { "an empty binding parses to nothing",
    "", 0, { { "", 0.0f } } },

  // 64 'a's — one past the field, which is also one past anything the
  // corpus column can hold, so truncating it could only ever match the
  // wrong corpus.
  { "a name too long for the field is dropped, not truncated",
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    ";archwiki", 1, { { "archwiki", 1.0f } } },
};

static void
check_weight(const char *label, float want, float got)
{
  char w[32];
  char g[32];

  snprintf(w, sizeof(w), "%.3f", (double)want);
  snprintf(g, sizeof(g), "%.3f", (double)got);

  test_check_str(SUITE, label, w, g);
}

static void
run_parse_case(const parse_case_t *c)
{
  knowledge_corpus_weight_t out[KNOWLEDGE_CORPUS_LIST_MAX];
  char                      label[192];
  size_t                    n;

  memset(out, 0, sizeof(out));
  n = knowledge_parse_corpus_list(c->list, out, KNOWLEDGE_CORPUS_LIST_MAX);

  snprintf(label, sizeof(label), "%s: entries", c->name);
  test_check_sz(SUITE, label, c->want_n, n);

  for(size_t i = 0; i < c->want_n && i < n; i++)
  {
    snprintf(label, sizeof(label), "%s: name %zu", c->name, i);
    test_check_str(SUITE, label, c->want[i].corpus, out[i].name);

    snprintf(label, sizeof(label), "%s: weight %zu", c->name, i);
    check_weight(label, c->want[i].weight, out[i].weight);
  }
}

// The array is what bounds the clause built from it, so the cap is an
// invariant the SQL buffer depends on rather than a nicety.
static void
run_parse_cap(void)
{
  knowledge_corpus_weight_t out[KNOWLEDGE_CORPUS_LIST_MAX];
  char                      list[512];
  size_t                    pos = 0;

  for(size_t i = 0; i < KNOWLEDGE_CORPUS_LIST_MAX + 4; i++)
    pos += (size_t)snprintf(list + pos, sizeof(list) - pos, "c%zu;", i);

  test_check_sz(SUITE, "the list cap bounds the array",
      KNOWLEDGE_CORPUS_LIST_MAX,
      knowledge_parse_corpus_list(list, out, KNOWLEDGE_CORPUS_LIST_MAX));
}

// A NULL binding reaches here from a bot whose KV row was never set.
static void
run_parse_null(void)
{
  knowledge_corpus_weight_t out[KNOWLEDGE_CORPUS_LIST_MAX];

  test_check_sz(SUITE, "a NULL binding parses to nothing", 0,
      knowledge_parse_corpus_list(NULL, out, KNOWLEDGE_CORPUS_LIST_MAX));
}

// Zero is bound-but-never-retrieved, and retrieval spells that as an
// entry that never reaches the IN clause. Scoring its rows to 0.0
// instead would still let them fill a top-K nothing else reached.
static void
run_compact(void)
{
  knowledge_corpus_weight_t w[4] = {
    { "zeroed",   0.0f },
    { "kept",     0.4f },
    { "alsozero", 0.0f },
    { "kepttoo",  1.0f },
  };
  size_t n = knowledge_corpus_weight_compact(w, 4);

  test_check_sz(SUITE, "compact: survivors", 2, n);
  test_check_str(SUITE, "compact: order is preserved", "kept", w[0].name);
  test_check_str(SUITE, "compact: order is preserved (2)", "kepttoo",
      w[1].name);
}

static void
run_weight_of(void)
{
  const knowledge_corpus_weight_t w[2] = {
    { "archwiki", 0.4f },
    { "acquired", 2.0f },
  };

  check_weight("weight_of: a bound name",
      0.4f, knowledge_corpus_weight_of(w, 2, "archwiki"));
  check_weight("weight_of: another bound name",
      2.0f, knowledge_corpus_weight_of(w, 2, "acquired"));

  // Every row a scan walks came out of a corpus in the clause, so this
  // is the empty-binding path rather than a miss.
  check_weight("weight_of: a name the list did not carry",
      1.0f, knowledge_corpus_weight_of(w, 2, "somethingelse"));
  check_weight("weight_of: an empty list",
      1.0f, knowledge_corpus_weight_of(w, 0, "archwiki"));
}

int
main(void)
{
  for(size_t i = 0; i < sizeof(parse_cases) / sizeof(parse_cases[0]); i++)
    run_parse_case(&parse_cases[i]);

  run_parse_cap();
  run_parse_null();
  run_compact();
  run_weight_of();

  return(test_report(SUITE));
}
