// botmanager — MIT
// Knowledge file ingest: slurp, UTF-8-safe chunker, markdown/plain walker.

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

// Forward decl: knowledge.c owns the raw-insert; knowledge_file.c calls
// it for each emitted chunk. Exposed inside the module via the internal
// header block below.
bool knowledge_insert_chunk_raw(const char *corpus, const char *source_url,
    const char *section_heading, const char *text, int64_t *out_id);

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
  if(buf == NULL) { fclose(fp); return(NULL); }

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
  int64_t id;
  if(start == NULL || end <= start) return;

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

  if(len >= KNOWLEDGE_CHUNK_TEXT_SZ)
    len = KNOWLEDGE_CHUNK_TEXT_SZ - 1;

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
  if(knowledge_insert_chunk_raw(ing->corpus, ing->source_url,
        ing->heading, text, &id) != SUCCESS)
    return;

  ing->emitted++;

  if(ing->batch != NULL)
    knowledge_batch_add(ing->batch, id, text);
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
  if(section == NULL) return;

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

    // Append line to the section buffer.
    if(sec_len + llen + 2 < sec_cap)
    {
      memcpy(section + sec_len, p, llen);
      sec_len += llen;
      section[sec_len++] = '\n';
    }

    // Once the buffer exceeds chunk_max, emit the first chunk_max bytes
    // and retain the last KNOWLEDGE_CHUNK_OVERLAP bytes as the opening
    // context for the next chunk in the same section.
    if(sec_len >= ing->chunk_max)
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
  if(buf == NULL) return;

  len = 0;
  cap = ing->chunk_max + KNOWLEDGE_CHUNK_TEXT_SZ;

  p = body;
  while(*p != '\0')
  {
    // Append up to the next blank line (paragraph boundary).
    const char *para_end = strstr(p, "\n\n");
    size_t pl = (para_end != NULL) ? (size_t)(para_end - p) + 1
        : strlen(p);

    if(len + pl + 2 < cap)
    {
      memcpy(buf + len, p, pl);
      len += pl;
      buf[len++] = '\n';
    }

    if(len >= ing->chunk_max)
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

static size_t
kw_ingest_file(const char *corpus, const char *path,
    const char *base_url_or_NULL,
    uint32_t chunk_max, knowledge_batch_t *batch, size_t *out_skipped)
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
    return(0);
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

  mem_free(body);

  if(out_skipped != NULL) *out_skipped = ing.skipped;
  return(ing.emitted);
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
bool
knowledge_ingest_path(const char *corpus, const char *path,
    const char *base_url_or_NULL,
    size_t *out_files, size_t *out_chunks, size_t *out_skipped,
    uint64_t *out_embed_ok, uint64_t *out_embed_fail)
{
  struct stat st;
  knowledge_cfg_t cfg;
  uint32_t chunk_max;
  uint32_t batch_size;
  char embed_model[KNOWLEDGE_EMBED_MODEL_SZ];
  knowledge_batch_t batch;
  size_t files, chunks, skipped;
  if(stat(path, &st) != 0)
    return(FAIL);

  knowledge_cfg_snapshot(&cfg);
  chunk_max = cfg.chunk_max_chars;
  if(chunk_max < 256) chunk_max = 256;
  if(chunk_max > 8192) chunk_max = 8192;

  batch_size = cfg.embed_batch_size;
  if(batch_size == 0)
    batch_size = KNOWLEDGE_DEF_EMBED_BATCH_SIZE;
  if(batch_size > KNOWLEDGE_EMBED_BATCH_MAX)
    batch_size = KNOWLEDGE_EMBED_BATCH_MAX;

  // Resolve the effective embed model once at the top of the ingest so
  // the batch uses a consistent model for every flush. A model swap
  // mid-ingest is rare and undesirable (mixed dims corrupt the corpus);
  // we snapshot instead of re-reading per chunk.
  knowledge_effective_embed_model(embed_model, sizeof(embed_model));

  knowledge_batch_init(&batch, corpus, embed_model, batch_size);

  files = 0;
  chunks = 0;
  skipped = 0;

  if(S_ISREG(st.st_mode))
  {
    size_t sk = 0;
    chunks += kw_ingest_file(corpus, path, base_url_or_NULL,
        chunk_max, &batch, &sk);
    skipped += sk;
    files = 1;
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
      size_t sk;
      if(de->d_name[0] == '.') continue;

      snprintf(entry, sizeof(entry), "%s/%s", path, de->d_name);

      if(stat(entry, &est) != 0 || !S_ISREG(est.st_mode))
        continue;

      if(!kw_path_has_ext(entry, ".md")
          && !kw_path_has_ext(entry, ".markdown")
          && !kw_path_has_ext(entry, ".txt"))
        continue;

      sk = 0;
      chunks += kw_ingest_file(corpus, entry, base_url_or_NULL,
          chunk_max, &batch, &sk);
      skipped += sk;
      files++;
    }

    closedir(d);
  }

  else
  {
    knowledge_batch_free(&batch);
    return(FAIL);
  }

  // Drain the final partial batch and tear down.
  knowledge_batch_free(&batch);

  if(out_files)      *out_files      = files;
  if(out_chunks)     *out_chunks     = chunks;
  if(out_skipped)    *out_skipped    = skipped;
  if(out_embed_ok)   *out_embed_ok   = batch.chunks_embedded_ok;
  if(out_embed_fail) *out_embed_fail = batch.chunks_embedded_fail;

  return(SUCCESS);
}
