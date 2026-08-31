// botmanager — MIT
// The knowledge store's ranking arithmetic: the cosine top-K heap, and
// the merge that interleaves it with the lexical channel.
//
// Split out of knowledge.c so it can be compiled into a test without
// the subsystem behind it — every function here is pure, touches no
// database, no allocator and no lock, and that is the whole reason the
// file exists. The two defects that produced it both aborted the
// daemon: a merge that deduplicated only one of its two inputs
// overflowed the array sized from the distinct rows behind it, and a
// made-no-progress guard broke the whole merge when the lexical side
// ran dry, throwing away the cosine hits behind it.
// Cases: tests/test_knowledge_rank.c.

#include "knowledge_priv.h"

// Keep the top-K (highest score) in a descending-sorted array.
void
knowledge_topk_insert(knowledge_hit_t *buf, size_t cap, size_t *n,
    int64_t id, float score)
{
  size_t pos;
  if(*n < cap)
  {
    size_t pos = *n;
    while(pos > 0 && buf[pos - 1].score < score)
    {
      buf[pos] = buf[pos - 1];
      pos--;
    }
    buf[pos].id = id;
    buf[pos].score = score;
    (*n)++;
    return;
  }

  if(score <= buf[cap - 1].score)
    return;

  pos = cap - 1;
  while(pos > 0 && buf[pos - 1].score < score)
  {
    buf[pos] = buf[pos - 1];
    pos--;
  }
  buf[pos].id = id;
  buf[pos].score = score;
}

// Order the lexical candidates: rarest terms first, and where two carry
// equally rare terms, whichever the embedder liked better. The list is
// KNOWLEDGE_LEX_CAND_MAX at its widest, so an insertion sort is both
// the fastest thing here and the shortest.
void
knowledge_lex_sort(knowledge_lex_t *lex, size_t n)
{
  for(size_t i = 1; i < n; i++)
  {
    knowledge_lex_t v = lex[i];
    size_t j = i;

    while(j > 0 && (lex[j - 1].idf < v.idf
        || (lex[j - 1].idf == v.idf && lex[j - 1].cos < v.cos)))
    {
      lex[j] = lex[j - 1];
      j--;
    }

    lex[j] = v;
  }
}

// Merge the two ranked lists into the delivery order: the best cosine
// hit, then the best lexical one, then alternating.
//
// The interleave is the point, not a tidiness. Position is priority all
// the way down to the model -- the KNOWLEDGE fence fills from the front
// and stops at its byte budget -- so appending the lexical channel
// after the cosine one would put its hits exactly where the budget cuts,
// and the channel would cost a query per reply to change nothing. The
// cosine channel keeps the first slot because it is right far more
// often; the lexical channel needs the second one to be worth having.
// Append one hit unless the list already carries that chunk. Both
// channels push through here, and BOTH have to: the two rank the same
// corpora, so a chunk the lexical channel likes is very often one the
// cosine channel is about to reach as well. Emitting it twice puts the
// same paragraph in the prompt twice AND makes the delivery array
// longer than the set of distinct rows behind it, which is a heap
// overflow one function down. Measured 2026-08-31 on "what is
// reflector": every lexical candidate and four of five cosine hits were
// chunks of the same page, and one-sided dedup aborted the daemon.
void
knowledge_hit_push(knowledge_hit_t *out, size_t *n, size_t cap,
    int64_t id, float score)
{
  if(*n >= cap)
    return;

  for(size_t i = 0; i < *n; i++)
  {
    if(out[i].id == id)
      return;
  }

  out[*n].id    = id;
  out[*n].score = score;
  (*n)++;
}

// Merge the two ranked lists into the delivery order: the best cosine
// hit, then the best lexical one, then alternating.
//
// The interleave is the point, not a tidiness. Position is priority all
// the way down to the model -- the KNOWLEDGE fence fills from the front
// and stops at its byte budget -- so appending the lexical channel
// after the cosine one would put its hits exactly where the budget cuts,
// and the channel would cost a query per reply to change nothing. The
// cosine channel keeps the first slot because it is right far more
// often; the lexical channel needs the second one to be worth having.
size_t
knowledge_interleave(const knowledge_rank_t *r, size_t lex_k,
    knowledge_hit_t *out, size_t out_cap)
{
  size_t n     = 0;
  size_t di    = 0;   // cursor into r->dense
  size_t li    = 0;   // cursor into r->lex
  size_t lex_n = 0;   // lexical hits actually emitted

  // Both cursors advance on every round the condition admits, so the
  // merge terminates on the cursors alone -- there is no need for a
  // made-no-progress guard, and an earlier one that broke the WHOLE
  // loop when the lexical side had nothing left also threw away the
  // cosine hits behind it.
  while(n < out_cap
      && (di < r->n_dense || (lex_n < lex_k && li < r->n_lex)))
  {
    if(di < r->n_dense)
    {
      knowledge_hit_push(out, &n, out_cap,
          r->dense[di].id, r->dense[di].score);
      di++;
    }

    while(lex_n < lex_k && li < r->n_lex && n < out_cap)
    {
      size_t was = n;

      // The score a consumer sees stays a cosine on both channels --
      // it is what `/show bot <b> knowledge` prints and the only
      // number the two lists have in common. A lexical hit the cosine
      // walk never reached carries 0.0, which is honest: the corpus
      // holds the chunk but not a vector for it.
      knowledge_hit_push(out, &n, out_cap, r->lex[li].id, r->lex[li].cos);
      li++;

      if(n != was)
      {
        lex_n++;
        break;
      }
    }
  }

  return(n);
}

