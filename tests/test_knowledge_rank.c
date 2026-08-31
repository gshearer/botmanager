// botmanager — MIT
// Cases for the knowledge store's two-channel merge: the delivery order
// it produces, and the two invariants a consumer depends on.
//
// This suite exists because both of the defects it guards ABORTED THE
// DAEMON on 2026-08-31, and neither was visible by reading:
//
//   - `knowledge_interleave` deduplicated only its lexical input. The
//     two channels rank the same corpora, so a chunk both like is
//     ordinary — measured, every lexical candidate for "what is
//     reflector" and four of five cosine hits were chunks of one page.
//     The merged list then carried an id twice, and knowledge_deliver_
//     hits, which walks hits and writes one chunk per hit into an array
//     sized from the DISTINCT rows behind them, ran off the end.
//   - a made-no-progress guard in the same loop broke out of the WHOLE
//     merge when the lexical side had nothing left to give, discarding
//     the cosine hits queued behind it. Silent: it shipped four hits
//     where five were ranked, and nothing counted the loss.
//
// The whole unit is pure — no database, no allocator, no lock — which
// is why knowledge_rank.c is its own file. Rows assert on the two
// invariants rather than on a hand-copied expected ordering, because
// the ordering is a policy that may be retuned and the invariants are
// not.

#include "test.h"

#include "knowledge_priv.h"

#include <stdio.h>
#include <string.h>

#define SUITE "knowledge_rank"

// One merge scenario. `dense` and `lex` are id lists; the scores are
// synthesised descending so the arrays arrive already ranked, which is
// what both real producers hand over.
typedef struct
{
  const char *name;
  int64_t     dense[8];
  size_t      n_dense;
  int64_t     lex[8];
  size_t      n_lex;
  size_t      lex_k;
  size_t      want_n;      // hits the merge must deliver
  int64_t     want_head[4];  // first hits, in order; 0 ends the list
} merge_case_t;

static const merge_case_t merge_cases[] = {
  // The failing exchange: the answer is buried in the cosine ranking
  // and first in the lexical one, so it has to land SECOND — the
  // consumer's byte budget fills from the front and stops.
  { "disjoint channels interleave",
    { 11026, 32663, 32074, 32664, 11027 }, 5,
    { 11222, 11174, 115 }, 3,
    3, 8, { 11026, 11222, 32663, 11174 } },

  // "what is reflector": every lexical candidate is already a cosine
  // hit. The merge must emit five distinct hits, not eight, and must
  // not stop early just because the lexical side never contributes.
  { "total overlap loses no cosine hit",
    { 8464, 8462, 8463, 8465, 8461 }, 5,
    { 8461, 8462, 8463 }, 3,
    3, 5, { 8464, 8461, 8462, 8463 } },

  // The partial case, which is the one that overflowed: a lexical hit
  // is emitted at position 1 and the SAME id turns up later in the
  // cosine ranking. One-sided dedup admits it twice.
  { "lexical hit reappearing in the cosine tail is not emitted twice",
    { 700, 701, 8461, 702, 703 }, 5,
    { 8461, 900 }, 2,
    2, 6, { 700, 8461, 701, 900 } },

  { "lexical channel off delivers the cosine ranking alone",
    { 1, 2, 3 }, 3,
    { 40, 41 }, 2,
    0, 3, { 1, 2, 3 } },

  { "empty cosine channel still delivers the lexical slots",
    { 0 }, 0,
    { 40, 41, 42 }, 3,
    2, 2, { 40, 41 } },

  { "fewer lexical candidates than slots is not a stall",
    { 1, 2 }, 2,
    { 40 }, 1,
    3, 3, { 1, 40, 2 } },
};

static void
run_merge_case(const merge_case_t *c)
{
  knowledge_rank_t rank;
  knowledge_hit_t  out[KNOWLEDGE_HITS_MAX];
  char             label[128];
  size_t           n;

  memset(&rank, 0, sizeof(rank));

  for(size_t i = 0; i < c->n_dense; i++)
  {
    rank.dense[i].id    = c->dense[i];
    rank.dense[i].score = 0.9f - (float)i * 0.01f;
  }

  rank.n_dense = c->n_dense;

  for(size_t i = 0; i < c->n_lex; i++)
  {
    rank.lex[i].id  = c->lex[i];
    rank.lex[i].idf = 9.0f - (float)i;
    rank.lex[i].cos = 0.7f;
  }

  rank.n_lex = c->n_lex;

  n = knowledge_interleave(&rank, c->lex_k, out,
      sizeof(out) / sizeof(out[0]));

  snprintf(label, sizeof(label), "%s: count", c->name);
  test_check_sz(SUITE, label, c->want_n, n);

  // ⛔ The invariant knowledge_deliver_hits' allocation rests on. A
  // duplicate here is a heap overflow one function down, not a
  // cosmetic repeat.
  for(size_t i = 0; i < n; i++)
  {
    bool dup = false;

    for(size_t j = 0; j < i; j++)
    {
      if(out[j].id == out[i].id)
        dup = true;
    }

    snprintf(label, sizeof(label), "%s: hit %zu (id %lld) is not a repeat",
        c->name, i, (long long)out[i].id);
    test_check_bool(SUITE, label, false, dup);
  }

  for(size_t i = 0; i < 4 && c->want_head[i] != 0; i++)
  {
    long long got = i < n ? (long long)out[i].id : -1;

    snprintf(label, sizeof(label), "%s: position %zu", c->name, i);
    test_check_sz(SUITE, label, (size_t)c->want_head[i], (size_t)got);
  }
}

// The cosine heap. It is older than the merge and was not implicated in
// either abort, but it is the other half of what the merge consumes and
// it has never had a case.
static void
run_topk(void)
{
  knowledge_hit_t buf[4];
  size_t          n = 0;
  const float     scores[] = { 0.10f, 0.90f, 0.50f, 0.95f, 0.20f, 0.70f };

  for(size_t i = 0; i < sizeof(scores) / sizeof(scores[0]); i++)
    knowledge_topk_insert(buf, 4, &n, (int64_t)(i + 1), scores[i]);

  test_check_sz(SUITE, "topk: fills to cap and no further", 4, n);
  test_check_sz(SUITE, "topk: best first",  4, (size_t)buf[0].id);
  test_check_sz(SUITE, "topk: second",      2, (size_t)buf[1].id);
  test_check_sz(SUITE, "topk: third",       6, (size_t)buf[2].id);
  test_check_sz(SUITE, "topk: fourth",      3, (size_t)buf[3].id);
}

// The lexical tiebreak: rarest term first, and among equally rare terms
// whichever the embedder scored higher. Without the second key, "what
// is reflector" put a Latvian page above the English one.
static void
run_lex_sort(void)
{
  knowledge_lex_t lex[4] = {
    { .id = 1, .idf = 7.3f, .cos = 0.40f },
    { .id = 2, .idf = 9.1f, .cos = 0.30f },
    { .id = 3, .idf = 7.3f, .cos = 0.80f },
    { .id = 4, .idf = 7.3f, .cos = 0.60f },
  };

  knowledge_lex_sort(lex, 4);

  test_check_sz(SUITE, "lex_sort: rarest term first",       2, (size_t)lex[0].id);
  test_check_sz(SUITE, "lex_sort: tie broken by cosine",    3, (size_t)lex[1].id);
  test_check_sz(SUITE, "lex_sort: tie broken by cosine (2)", 4, (size_t)lex[2].id);
  test_check_sz(SUITE, "lex_sort: tie broken by cosine (3)", 1, (size_t)lex[3].id);
}

int
main(void)
{
  for(size_t i = 0; i < sizeof(merge_cases) / sizeof(merge_cases[0]); i++)
    run_merge_case(&merge_cases[i]);

  run_topk();
  run_lex_sort();

  return(test_report(SUITE));
}
