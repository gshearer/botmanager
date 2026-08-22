// botmanager — MIT
// Knowledge ingest: slurp, UTF-8-safe chunkers, file walker, text entry.

#include "knowledge_priv.h"

#include "db.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

// Ingest pipeline — file slurp, section-aware markdown chunker, plain
// paragraph chunker, file/directory walker.

// Cap on a single slurped file. Arch wiki pages rarely exceed 200 KB;
// SEP entries can reach ~400 KB. Half a meg is plenty for K2 and
// keeps us firmly on the stack for bookkeeping allocations.
#define KNOWLEDGE_SLURP_MAX   (512 * 1024)

// Minimum useful chunk length. Below this we skip a chunk rather than
// embed it — short fragments dilute RAG relevance with near-noise.
#define KNOWLEDGE_CHUNK_MIN   80

// Overlap between adjacent chunks inside the same section. Enough to
// catch a sentence that straddles a boundary without wasting embedding
// budget on large repeats. Zero overlap across section boundaries.
#define KNOWLEDGE_CHUNK_OVERLAP  200

static char *
kw_slurp(const char *path, size_t *out_len)
{
  FILE *fp = fopen(path, "rb");
  long sz;
  char *buf;
  size_t n;
  if(fp == NULL) return(NULL);

  if(fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return(NULL); }
  sz = ftell(fp);
  if(sz < 0 || sz > KNOWLEDGE_SLURP_MAX)
  {
    fclose(fp);
    return(NULL);
  }
  rewind(fp);

  buf = mem_alloc("knowledge", "slurp", (size_t)sz + 1);

  n = fread(buf, 1, (size_t)sz, fp);
  buf[n] = '\0';
  fclose(fp);

  if(out_len != NULL) *out_len = n;
  return(buf);
}

// Recognise an ATX-style markdown heading: "^#{1,6} text". Returns the
// pointer past the '#' run and leading spaces, or NULL if not a heading.
static const char *
kw_md_heading_text(const char *line)
{
  const char *p;
  int n;
  if(line == NULL || line[0] != '#') return(NULL);

  p = line;
  n = 0;
  while(*p == '#' && n < 6) { p++; n++; }
  if(n == 0 || *p != ' ') return(NULL);

  while(*p == ' ') p++;
  return(p);
}

// Copy src into dst, trimming trailing whitespace and bounded by dst_sz.
static void
kw_copy_trimmed(char *dst, size_t dst_sz, const char *src, size_t src_len)
{
  size_t n;
  if(dst == NULL || dst_sz == 0) return;

  while(src_len > 0
      && (src[src_len - 1] == ' ' || src[src_len - 1] == '\t'
          || src[src_len - 1] == '\r' || src[src_len - 1] == '\n'))
    src_len--;

  n = (src_len < dst_sz - 1) ? src_len : dst_sz - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

// Trim `text` in-place to the longest valid-UTF-8 prefix, dropping
// any leading orphan continuation bytes and any trailing incomplete
// multi-byte sequence. Returns the new length (writes a NUL at that
// position).
//
// Why this exists: the chunker splits on byte offsets (for speed and
// simplicity). When a chunk boundary lands inside a multi-byte UTF-8
// codepoint — common with box-drawing characters, CJK, extended
// Latin — one chunk ends mid-sequence and the next begins with
// orphan continuation bytes. Either form is invalid UTF-8 and gets
// rejected by downstream JSON encoders. vLLM's `/v1/embeddings`
// responds with `{"detail":"There was an error parsing the body"}`
// and the chunk's embedding is lost. At 1200-byte chunks on the
// English Arch wiki this happened for ~0.1% of chunks.
//
// Mechanics:
//   UTF-8 lead bytes:  0xxxxxxx            (1-byte, ASCII)
//                      110xxxxx 10...      (2-byte)
//                      1110xxxx 10... 10...(3-byte)
//                      11110xxx 10... 10... 10... (4-byte)
//   Continuation:      10xxxxxx
// Scan forward codepoint-by-codepoint; on any malformed byte or
// incomplete tail, truncate there.
//
// Cost: O(n); ~1-2 microseconds on a 1200-byte chunk. ASCII input
// is a no-op.
static size_t
kw_trim_utf8(char *text, size_t len)
{
  size_t head;
  size_t i;
  size_t last_valid_end;
  if(text == NULL || len == 0)
  {
    if(text != NULL) text[0] = '\0';
    return(0);
  }

  // 1. Leading trim: orphan continuation bytes at the start cannot
  //    belong to any codepoint since nothing precedes them.
  head = 0;
  while(head < len
      && ((unsigned char)text[head] & 0xC0) == 0x80)
    head++;

  if(head > 0)
  {
    memmove(text, text + head, len - head);
    len -= head;
  }

  if(len == 0)
  {
    text[0] = '\0';
    return(0);
  }

  // 2. Validate forward, codepoint at a time. Last complete codepoint
  //    end is the new length; anything after is trimmed.
  i = 0;
  last_valid_end = 0;

  while(i < len)
  {
    unsigned char c = (unsigned char)text[i];
    int expected;

    bool ok;
    if(c < 0x80)                      expected = 1;
    else if((c & 0xE0) == 0xC0)       expected = 2;
    else if((c & 0xF0) == 0xE0)       expected = 3;
    else if((c & 0xF8) == 0xF0)       expected = 4;
    else                              break;   // invalid lead byte

    if(i + (size_t)expected > len)    break;   // truncated tail

    ok = true;
    for(int j = 1; j < expected; j++)
    {
      if(((unsigned char)text[i + j] & 0xC0) != 0x80)
      { ok = false; break; }
    }
    if(!ok) break;

    i += (size_t)expected;
    last_valid_end = i;
  }

  text[last_valid_end] = '\0';
  return(last_valid_end);
}

typedef struct
{
  const char         *corpus;
  const char         *source_url;
  char                heading[KNOWLEDGE_SECTION_SZ];
  uint32_t            chunk_max;
  size_t              emitted;   // inserted chunk count (DB rows)
  size_t              skipped;   // chunks skipped as too short
  size_t              duplicates; // already stored AND already embedded
  size_t              reembedded; // already stored, vector-less — resumed
  knowledge_batch_t  *batch;     // embed accumulator, shared across files
} kw_ingest_t;

// Emit one chunk [start, end) under the current heading. Inserts the
// chunk row synchronously, then hands the new id + text to the batch
// accumulator. The batch flushes itself when it fills; a final flush
// happens in knowledge_ingest_path's cleanup.
static void
kw_emit_chunk(kw_ingest_t *ing, const char *start, const char *end)
{
  size_t len;
  char text[KNOWLEDGE_CHUNK_TEXT_SZ];
  size_t clean_len;
  knowledge_chunk_rc_t rc;
  int64_t id;
  if(start == NULL || end <= start) return;

  // The batch gave up on the engine. The INSERT below would still
  // succeed — which is the trap: it would leave a chunk row no
  // retrieval can ever reach. Since OBS-16 a re-ingest *does* repair
  // such a row rather than duplicating it, and that is exactly why the
  // walk must still stop here: emitting more vector-less rows makes
  // more work for the re-run, not less. Stop emitting instead.
  if(ing->batch != NULL && ing->batch->aborted) return;

  len = (size_t)(end - start);

  // Strip leading whitespace; enforce a minimum length so we don't
  // embed near-empty fragments.
  while(len > 0
      && (*start == ' ' || *start == '\t' || *start == '\r'
          || *start == '\n'))
  {
    start++;
    len--;
  }

  if(len < KNOWLEDGE_CHUNK_MIN)
  {
    ing->skipped++;
    return;
  }

  // No cut of our own before the copy: chunk_max's ceiling is
  // KNOWLEDGE_CHUNK_TEXT_SZ - 1, so `len` fits. The clamp that stood
  // here read as the place over-long chunks were handled, and that was
  // the whole trouble — it silently ate the tail of every chunk the
  // splitters produced above 4295 bytes while the knob advertised
  // 8192.
  kw_copy_trimmed(text, sizeof(text), start, len);

  // Repair UTF-8 at the chunk boundaries. The byte-oriented chunker
  // may have sliced through a multi-byte codepoint; without this, the
  // embed endpoint rejects the chunk as invalid JSON. See kw_trim_utf8.
  clean_len = kw_trim_utf8(text, strlen(text));

  // Re-check the minimum after trimming — a boundary trim can shrink
  // an already-short chunk below the useful threshold.
  if(clean_len < KNOWLEDGE_CHUNK_MIN)
  {
    ing->skipped++;
    return;
  }

  id = 0;
  rc = knowledge_insert_chunk_raw(ing->corpus, ing->source_url,
      ing->heading, text, &id);

  if(rc == KNOWLEDGE_CHUNK_FAILED)
    return;

  // Already stored and already retrievable. Counting it as a chunk
  // would make the command's headline number a lie about what the walk
  // added, which is the number the operator is deciding on.
  if(rc == KNOWLEDGE_CHUNK_PRESENT)
  {
    ing->duplicates++;
    return;
  }

  if(rc == KNOWLEDGE_CHUNK_UNEMBEDDED)
    ing->reembedded++;

  else
    ing->emitted++;

  // The row is already in the table, so a batch that will not take it
  // is a chunk with no embedding — count it as one. This is the chunk
  // whose own add is what discovered the abort: without this the
  // command's "failed" line is short by exactly that one, and the row
  // it does not mention is as orphaned as the four it does.
  if(ing->batch != NULL
      && knowledge_batch_add(ing->batch, id, text) != SUCCESS)
    ing->batch->chunks_embedded_fail++;
}

// Size-only splitter for a run of bytes with no separator left in it.
// Breaks on the size budget, backing up to the last space so a chunk
// does not end mid-word, and carries the same overlap the other two
// carry.
//
// It is the URL path's whole chunker — acq_strip_html collapses every
// whitespace run, newlines included, so a fetched page arrives as one
// line and neither separator-driven splitter can divide it — and it is
// also where those two send a single paragraph or line that is larger
// than their accumulator. Their alternative was dropping it.
//
// The heading belongs to the caller in all three uses, so unlike its
// two siblings this one does not reset it.
static void
kw_chunk_flow(kw_ingest_t *ing, const char *body, size_t len)
{
  size_t pos = 0;

  if(body == NULL || len == 0) return;

  while(pos < len)
  {
    size_t take = len - pos;
    size_t back;

    if(take <= ing->chunk_max)
    {
      kw_emit_chunk(ing, body + pos, body + len);
      return;
    }

    take = ing->chunk_max;
    back = take;

    // Back up to the last space in the window. The floor keeps `take`
    // above the overlap, which is what makes the advance below strictly
    // positive: a window of unbroken bytes — a base64 blob, CJK with no
    // spaces — splits mid-token rather than looping forever. chunk_max is
    // clamped at 256 and the floor is 201, so the window always has room.
    while(back > KNOWLEDGE_CHUNK_OVERLAP + 1 && body[pos + back] != ' ')
      back--;

    if(back > KNOWLEDGE_CHUNK_OVERLAP + 1)
      take = back;

    kw_emit_chunk(ing, body + pos, body + pos + take);

    if(ing->batch != NULL && ing->batch->aborted)
      return;

    pos += take - KNOWLEDGE_CHUNK_OVERLAP;
  }
}

// Section-aware markdown chunker. Heading runs define section bounds
// and become each chunk's `section_heading` column. Inside a section,
// content is greedy-split at paragraph boundaries (double newline) up
// to `chunk_max` bytes, with `KNOWLEDGE_CHUNK_OVERLAP` bytes of trailing
// context carried into the next chunk. Overlap is reset at section
// boundaries — a new section means a new context.
static void
kw_chunk_markdown(kw_ingest_t *ing, const char *body)
{
  char *section;
  size_t sec_len;
  size_t sec_cap;
  const char *p;
  if(body == NULL) return;

  ing->heading[0] = '\0';

  // Walk lines; when we hit a heading, flush the current section's
  // buffer. Otherwise, append to the section buffer and split when the
  // buffer exceeds chunk_max.
  section = mem_alloc("knowledge", "md_section",
      ing->chunk_max + KNOWLEDGE_CHUNK_TEXT_SZ + 1);

  sec_len = 0;
  sec_cap = ing->chunk_max + KNOWLEDGE_CHUNK_TEXT_SZ;

  p = body;
  while(*p != '\0')
  {
    const char *eol = strchr(p, '\n');
    size_t llen = (eol != NULL) ? (size_t)(eol - p) : strlen(p);

    // Probe the heading without mutating p.
    char lineprobe[KNOWLEDGE_SECTION_SZ];
    size_t plen = (llen < sizeof(lineprobe) - 1) ? llen
        : sizeof(lineprobe) - 1;
    const char *htext;
    memcpy(lineprobe, p, plen);
    lineprobe[plen] = '\0';

    htext = kw_md_heading_text(lineprobe);
    if(htext != NULL)
    {
      // Flush whatever sits in the buffer under the previous heading.
      size_t hlen;
      if(sec_len > 0)
      {
        kw_emit_chunk(ing, section, section + sec_len);
        sec_len = 0;
      }

      // Update current heading. Trim trailing "#".
      hlen = strlen(htext);
      while(hlen > 0 && htext[hlen - 1] == '#') hlen--;
      while(hlen > 0 && (htext[hlen - 1] == ' ' || htext[hlen - 1] == '\t'))
        hlen--;
      kw_copy_trimmed(ing->heading, sizeof(ing->heading), htext, hlen);

      p = (eol != NULL) ? eol + 1 : p + llen;
      continue;
    }

    // Append line to the section buffer. A line that will not fit
    // beside what is already buffered flushes it first; one that will
    // not fit an empty buffer either goes to the size splitter,
    // because the accumulator can never hold it. Skipping the append
    // instead — which is what this did — drops the line's whole
    // content and moves no counter, so an .md of unwrapped paragraphs
    // ingests short and reports success.
    if(sec_len + llen + 2 >= sec_cap)
    {
      if(sec_len > 0)
      {
        kw_emit_chunk(ing, section, section + sec_len);
        sec_len = 0;
      }

      if(llen + 2 >= sec_cap)
      {
        kw_chunk_flow(ing, p, llen);
        p = (eol != NULL) ? eol + 1 : p + llen;
        continue;
      }
    }

    memcpy(section + sec_len, p, llen);
    sec_len += llen;
    section[sec_len++] = '\n';

    // Once the buffer exceeds chunk_max, emit the first chunk_max bytes
    // and retain the last KNOWLEDGE_CHUNK_OVERLAP bytes as the opening
    // context for the next chunk in the same section. Drain until the
    // buffer is back under the budget: one append can carry several
    // chunks' worth, and a single pass leaves the excess to be cut off
    // by kw_emit_chunk's KNOWLEDGE_CHUNK_TEXT_SZ clamp later. Each
    // round drops sec_len by chunk_max - KNOWLEDGE_CHUNK_OVERLAP, and
    // chunk_max is clamped at 256 against an overlap of 200, so the
    // walk is strictly downward.
    while(sec_len >= ing->chunk_max)
    {
      size_t keep;
      size_t src;
      size_t tail;
      kw_emit_chunk(ing, section, section + ing->chunk_max);

      keep = (ing->chunk_max > KNOWLEDGE_CHUNK_OVERLAP)
          ? KNOWLEDGE_CHUNK_OVERLAP : ing->chunk_max;
      src = ing->chunk_max - keep;
      tail = sec_len - ing->chunk_max;

      memmove(section, section + src, keep + tail);
      sec_len = keep + tail;
    }

    p = (eol != NULL) ? eol + 1 : p + llen;
  }

  // Flush residual content.
  if(sec_len > 0)
    kw_emit_chunk(ing, section, section + sec_len);

  mem_free(section);
}

// Plain-text fallback: split greedy at paragraph boundaries, same size
// budget, no section heading, same overlap.
static void
kw_chunk_plaintext(kw_ingest_t *ing, const char *body)
{
  char *buf;
  size_t len;
  size_t cap;
  const char *p;
  if(body == NULL) return;

  ing->heading[0] = '\0';

  buf = mem_alloc("knowledge", "txt_buf",
      ing->chunk_max + KNOWLEDGE_CHUNK_TEXT_SZ + 1);

  len = 0;
  cap = ing->chunk_max + KNOWLEDGE_CHUNK_TEXT_SZ;

  p = body;
  while(*p != '\0')
  {
    // Append up to the next blank line (paragraph boundary).
    const char *para_end = strstr(p, "\n\n");
    size_t pl = (para_end != NULL) ? (size_t)(para_end - p) + 1
        : strlen(p);

    // Same rule as the markdown chunker's, over paragraphs rather than
    // lines: flush to make room, and hand a paragraph too large for an
    // empty accumulator to the size splitter. A .txt whose paragraphs
    // all exceed the buffer used to ingest as zero chunks.
    if(len + pl + 2 >= cap)
    {
      if(len > 0)
      {
        kw_emit_chunk(ing, buf, buf + len);
        len = 0;
      }

      if(pl + 2 >= cap)
      {
        kw_chunk_flow(ing, p, pl);
        p = (para_end != NULL) ? para_end + 2 : p + pl;
        continue;
      }
    }

    memcpy(buf + len, p, pl);
    len += pl;
    buf[len++] = '\n';

    // Drain to under the budget rather than cutting one chunk off the
    // front — see the markdown chunker's copy of this block.
    while(len >= ing->chunk_max)
    {
      size_t keep;
      size_t src;
      size_t tail;
      kw_emit_chunk(ing, buf, buf + ing->chunk_max);

      keep = (ing->chunk_max > KNOWLEDGE_CHUNK_OVERLAP)
          ? KNOWLEDGE_CHUNK_OVERLAP : ing->chunk_max;
      src = ing->chunk_max - keep;
      tail = len - ing->chunk_max;

      memmove(buf, buf + src, keep + tail);
      len = keep + tail;
    }

    p = (para_end != NULL) ? para_end + 2 : p + pl;
  }

  if(len > 0)
    kw_emit_chunk(ing, buf, buf + len);

  mem_free(buf);
}

static bool
kw_path_has_ext(const char *path, const char *ext)
{
  size_t pl = strlen(path);
  size_t el = strlen(ext);
  if(pl < el) return(false);
  return(strcasecmp(path + pl - el, ext) == 0);
}

// Compose a canonical source_url for a single file. When base_url is
// empty the ingest path is used verbatim (back-compat — matches the
// pre-base-url behaviour). Otherwise the file's basename is appended
// to base_url with its trailing .md / .markdown / .txt extension
// stripped, yielding "<base-url>/<stem>". Filenames are trusted to be
// URL-safe by the corpus owner (arch-wiki-docs pages already satisfy
// this); callers needing richer encoding should pre-encode.
static void
kw_derive_source_url(char *out, size_t sz, const char *base_url,
    const char *path)
{
  const char *slash = strrchr(path, '/');
  const char *base  = (slash != NULL) ? slash + 1 : path;

  size_t baselen = strlen(base);
  const char *exts[] = { ".markdown", ".md", ".txt" };

  for(size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
  {
    size_t el = strlen(exts[i]);

    if(baselen >= el && strcasecmp(base + baselen - el, exts[i]) == 0)
    {
      baselen -= el;
      break;
    }
  }

  snprintf(out, sz, "%s/%.*s", base_url, (int)baselen, base);
}

// Four counters is where separate out-params stop being readable, so
// this accumulates straight into the walk's stats block.
static void
kw_ingest_file(const char *corpus, const char *path,
    const char *base_url_or_NULL,
    uint32_t chunk_max, knowledge_batch_t *batch,
    knowledge_ingest_stats_t *acc)
{
  size_t len = 0;
  char *body = kw_slurp(path, &len);
  char derived[KNOWLEDGE_SOURCE_URL_SZ];
  kw_ingest_t ing;
  bool is_md;
  const char *source_url;
  if(body == NULL)
  {
    clam(CLAM_WARN, "knowledge", "ingest: cannot read '%s'", path);
    return;
  }

  source_url = path;

  if(base_url_or_NULL != NULL && base_url_or_NULL[0] != '\0')
  {
    kw_derive_source_url(derived, sizeof(derived), base_url_or_NULL, path);
    source_url = derived;
  }

  ing = (kw_ingest_t){0};
  ing.corpus     = corpus;
  ing.source_url = source_url;
  ing.chunk_max  = chunk_max;
  ing.heading[0] = '\0';
  ing.batch      = batch;

  is_md = kw_path_has_ext(path, ".md")
      || kw_path_has_ext(path, ".markdown");

  if(is_md)
    kw_chunk_markdown(&ing, body);
  else
    kw_chunk_plaintext(&ing, body);

  // A file with bytes in it that produced nothing at all — not one
  // chunk, not one skip, not one duplicate. Say so: that is the shape
  // the dropped-oversized-unit defect wore for as long as it lived,
  // and the walk's own total cannot show it, because a directory of
  // good files hides one silent file inside a healthy number. Still
  // reachable above the fix — an .md of nothing but headings appends
  // to no buffer and emits nothing. An aborted batch is excluded: it
  // zeroes every file after it and is reported as itself.
  if(len > 0
      && ing.emitted == 0 && ing.skipped == 0
      && ing.duplicates == 0 && ing.reembedded == 0
      && (batch == NULL || !batch->aborted))
    clam(CLAM_WARN, "knowledge",
        "ingest: '%s' (%zu bytes) yielded no chunks", path, len);

  mem_free(body);

  acc->chunks     += ing.emitted;
  acc->skipped    += ing.skipped;
  acc->duplicates += ing.duplicates;
  acc->reembedded += ing.reembedded;
}

// Open the embed accumulator an ingest walk runs against, and hand back
// the chunk size it should split at. Both entry points below need the
// identical lines: a config snapshot and one embed-model resolve —
// resolved once here rather than per chunk, because a model swap
// mid-walk mixes vector dimensions into the same corpus. The caller
// owns `out_batch` and must knowledge_batch_free it; that closing flush
// is where the final stats appear.
//
// chunk_max arrives already clamped — knowledge_load_config owns that
// range, so what the operator is shown and what the splitters cut at
// cannot drift apart.
static void
kw_batch_open(const char *corpus, uint32_t *out_chunk_max,
    knowledge_batch_t *out_batch)
{
  knowledge_cfg_t cfg;
  uint32_t chunk_max;
  uint32_t batch_size;
  char embed_model[KNOWLEDGE_EMBED_MODEL_SZ];

  knowledge_cfg_snapshot(&cfg);
  chunk_max = cfg.chunk_max_chars;

  batch_size = cfg.embed_batch_size;
  if(batch_size == 0)
    batch_size = KNOWLEDGE_DEF_EMBED_BATCH_SIZE;
  if(batch_size > KNOWLEDGE_EMBED_BATCH_MAX)
    batch_size = KNOWLEDGE_EMBED_BATCH_MAX;

  knowledge_effective_embed_model(embed_model, sizeof(embed_model));

  knowledge_batch_init(out_batch, corpus, embed_model, batch_size);
  *out_chunk_max = chunk_max;
}

// Ingest a file or every .md/.txt/.markdown at the top of a directory.
// Directory walk is non-recursive by design — most corpus layouts are
// flat (one file per wiki page). Recursive ingest can land as a follow
// -up flag if a hierarchical corpus shows up.
//
// Runs on the admin-command worker thread. The embed batch flushes
// block that thread whenever the curl queue saturates
// (llm_embed_submit_wait → curl_request_submit_wait). The rest of the
// daemon (IRC dispatch, chat replies, etc.) continues on other pool
// workers; ingest backpressure does not stall the whole process.
//
// That wait is bounded by llm.embed_submit_wait_ms, and the bound is
// what makes this function returnable: without it a queue behind a
// silent endpoint parks this thread for as long as that endpoint
// likes, and neither the operator nor a plugin reload can call it
// back. When the bound fires the walk stops where it is and says so
// in `out->aborted` — the counts are then a prefix, not a total.
bool
knowledge_ingest_path(const char *corpus, const char *path,
    const char *base_url_or_NULL, knowledge_ingest_stats_t *out)
{
  struct stat st;
  uint32_t chunk_max;
  knowledge_batch_t batch;
  if(out == NULL)
    return(FAIL);

  memset(out, 0, sizeof(*out));

  if(stat(path, &st) != 0)
    return(FAIL);

  kw_batch_open(corpus, &chunk_max, &batch);

  // The memset above already zeroed every counter, including the two
  // OBS-16 added.
  if(S_ISREG(st.st_mode))
  {
    kw_ingest_file(corpus, path, base_url_or_NULL, chunk_max, &batch, out);
    out->files = 1;
  }

  else if(S_ISDIR(st.st_mode))
  {
    DIR *d = opendir(path);
    struct dirent *de;
    char entry[1024];
    if(d == NULL)
    {
      knowledge_batch_free(&batch);
      return(FAIL);
    }


    while((de = readdir(d)) != NULL)
    {
      struct stat est;
      if(batch.aborted) break;

      if(de->d_name[0] == '.') continue;

      snprintf(entry, sizeof(entry), "%s/%s", path, de->d_name);

      if(stat(entry, &est) != 0 || !S_ISREG(est.st_mode))
        continue;

      if(!kw_path_has_ext(entry, ".md")
          && !kw_path_has_ext(entry, ".markdown")
          && !kw_path_has_ext(entry, ".txt"))
        continue;

      kw_ingest_file(corpus, entry, base_url_or_NULL, chunk_max, &batch,
          out);
      out->files++;
    }

    closedir(d);
  }

  else
  {
    knowledge_batch_free(&batch);
    return(FAIL);
  }

  // Drain the final partial batch and tear down. The closing flush can
  // itself abort, so the stats are read after it, never before.
  knowledge_batch_free(&batch);

  out->embed_ok   = batch.chunks_embedded_ok;
  out->embed_fail = batch.chunks_embedded_fail;
  out->aborted    = batch.aborted;

  return(SUCCESS);
}

// The in-memory twin of knowledge_ingest_path: same emitter, same batch,
// same stats — the bytes arrive from a fetch instead of a file, with no
// structure left for the section-aware splitter to use, and the heading
// names the page rather than a section inside it.
//
// ⚠ Runs on a task worker, never on the curl thread: the batch flush
// inside blocks on llm_embed_submit_wait, which needs the curl worker to
// drain the queue it is waiting on.
bool
knowledge_ingest_text(const char *corpus, const char *source_url,
    const char *section_heading, const char *body, size_t len,
    knowledge_ingest_stats_t *out)
{
  uint32_t chunk_max;
  knowledge_batch_t batch;
  kw_ingest_t ing;

  if(out == NULL)
    return(FAIL);

  memset(out, 0, sizeof(*out));

  if(corpus == NULL || body == NULL || len == 0)
    return(FAIL);

  kw_batch_open(corpus, &chunk_max, &batch);

  ing = (kw_ingest_t){0};
  ing.corpus     = corpus;
  ing.source_url = source_url;
  ing.chunk_max  = chunk_max;
  ing.batch      = &batch;

  if(section_heading != NULL)
    kw_copy_trimmed(ing.heading, sizeof(ing.heading), section_heading,
        strlen(section_heading));

  kw_chunk_flow(&ing, body, len);

  // The closing flush can abort, so every counter is read after it.
  knowledge_batch_free(&batch);

  out->files      = 1;
  out->chunks     = ing.emitted;
  out->skipped    = ing.skipped;
  out->duplicates = ing.duplicates;
  out->reembedded = ing.reembedded;
  out->embed_ok   = batch.chunks_embedded_ok;
  out->embed_fail = batch.chunks_embedded_fail;
  out->aborted    = batch.aborted;

  return(SUCCESS);
}
