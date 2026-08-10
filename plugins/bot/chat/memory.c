// botmanager — MIT
// Chat bot memory store: conversation-log append and recall.
#define MEMORY_INTERNAL
#include "memory.h"

#include "bot.h"
#include "botmanctl.h"
#include "cmd.h"
#include "db.h"
#include "inference.h"
#include "method.h"
#include "task.h"
#include "userns.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Module state

bool                     memory_ready      = false;
mem_cfg_t                memory_cfg;
pthread_mutex_t          memory_cfg_mutex;

// Stats.
pthread_mutex_t          memory_stat_mutex;
uint64_t                 memory_stat_facts   = 0;
uint64_t                 memory_stat_logs    = 0;
uint64_t                 memory_stat_sweeps  = 0;
uint64_t                 memory_stat_forgets = 0;

// Decay sweep periodic task (if scheduled).
static task_handle_t     memory_sweep_task  = TASK_HANDLE_NONE;
time_t                   memory_last_sweep  = 0;

// The compiled embed-exclusion pattern. Compiled once per change under
// the write lock and read under the read lock by every logged line, so a
// live /set kv can never be seen half-applied. The pattern string is
// kept beside it purely so a recompile can be skipped when some other
// memory.* key changes.
static regex_t           memory_exclude_rx;
static bool              memory_exclude_rx_valid = false;
static char              memory_exclude_rx_pattern[MEM_EXCLUDE_REGEX_SZ] = "";
static pthread_rwlock_t  memory_exclude_rx_lock;

// Small utilities

static void
memory_stat_bump_facts(void)
{
  pthread_mutex_lock(&memory_stat_mutex);
  memory_stat_facts++;
  pthread_mutex_unlock(&memory_stat_mutex);
}

static void
memory_stat_bump_logs(void)
{
  pthread_mutex_lock(&memory_stat_mutex);
  memory_stat_logs++;
  pthread_mutex_unlock(&memory_stat_mutex);
}

static void
memory_stat_bump_sweeps(void)
{
  pthread_mutex_lock(&memory_stat_mutex);
  memory_stat_sweeps++;
  pthread_mutex_unlock(&memory_stat_mutex);
}

static void
memory_stat_bump_forgets(void)
{
  pthread_mutex_lock(&memory_stat_mutex);
  memory_stat_forgets++;
  pthread_mutex_unlock(&memory_stat_mutex);
}

// Snapshot config under lock.
void
memory_cfg_snapshot(mem_cfg_t *out)
{
  pthread_mutex_lock(&memory_cfg_mutex);
  *out = memory_cfg;
  pthread_mutex_unlock(&memory_cfg_mutex);
}

// KV configuration

bool
memory_kv_str_disabled(const char *s)
{
  return(s == NULL || s[0] == '\0'
      || strcasecmp(s, "off")  == 0
      || strcasecmp(s, "none") == 0
      || strcmp(s, "\"\"")     == 0);
}

// Compile the exclusion pattern. Called for every memory.* change, so it
// no-ops unless the pattern itself moved.
//
// ⚠ A pattern that fails to compile DISABLES the gate — it must never
// reject everything. A typo in a KV would otherwise silently empty the
// corpus, one unembedded line at a time, with nothing to see afterwards.
static void
memory_exclude_regex_compile(const char *pattern)
{
  char err[MEM_ERR_SZ];
  int  rc;

  pthread_rwlock_wrlock(&memory_exclude_rx_lock);

  if(strcmp(pattern, memory_exclude_rx_pattern) == 0)
  {
    pthread_rwlock_unlock(&memory_exclude_rx_lock);
    return;
  }

  if(memory_exclude_rx_valid)
  {
    regfree(&memory_exclude_rx);
    memory_exclude_rx_valid = false;
  }

  snprintf(memory_exclude_rx_pattern, sizeof(memory_exclude_rx_pattern),
      "%s", pattern);

  if(memory_kv_str_disabled(pattern))
  {
    pthread_rwlock_unlock(&memory_exclude_rx_lock);
    clam(CLAM_INFO, "memory", "embed exclude regex disabled");
    return;
  }

  rc = regcomp(&memory_exclude_rx, pattern, REG_EXTENDED | REG_NOSUB);

  if(rc != 0)
  {
    // Do not regfree() a preg regcomp() rejected — its contents are
    // undefined. regerror() is the one call that stays valid.
    regerror(rc, &memory_exclude_rx, err, sizeof(err));
    pthread_rwlock_unlock(&memory_exclude_rx_lock);
    clam(CLAM_WARN, "memory",
        "embed exclude regex '%s' does not compile (%s); gate disabled",
        pattern, err);
    return;
  }

  memory_exclude_rx_valid = true;
  pthread_rwlock_unlock(&memory_exclude_rx_lock);

  clam(CLAM_DEBUG, "memory", "embed exclude regex compiled: %s", pattern);
}

bool
memory_exclude_regex_active(void)
{
  bool on;

  pthread_rwlock_rdlock(&memory_exclude_rx_lock);
  on = memory_exclude_rx_valid;
  pthread_rwlock_unlock(&memory_exclude_rx_lock);

  return(on);
}

static void
memory_load_config(void)
{
  mem_cfg_t c;
  const char *em;
  const char *ri;
  const char *xr;

  c.enabled                   = kv_get_uint("memory.enabled") != 0;
  c.witness_embeds            = kv_get_uint("memory.witness_embeds") != 0;
  c.log_retention_days        = (uint32_t)kv_get_uint("memory.log_retention_days");
  c.fact_decay_half_life_days = (uint32_t)kv_get_uint("memory.fact_decay_half_life_days");
  c.min_fact_confidence       = (float)kv_get_double("memory.min_fact_confidence");
  c.rag_top_k                 = (uint32_t)kv_get_uint("memory.rag_top_k");
  c.recall_top_k              = (uint32_t)kv_get_uint("memory.recall_top_k");
  c.recall_min_cosine_x100    = (uint32_t)kv_get_uint("memory.recall_min_cosine");
  c.rag_max_context_chars     = (uint32_t)kv_get_uint("memory.rag_max_context_chars");
  c.embed_min_chars           = (uint32_t)kv_get_uint("memory.embed_min_chars");
  c.embed_batch_size          = (uint32_t)kv_get_uint("memory.embed_batch_size");
  c.embed_burst_max           = (uint32_t)kv_get_uint("memory.embed_burst_max");
  c.embed_burst_secs          = (uint32_t)kv_get_uint("memory.embed_burst_secs");
  c.embed_own_replies         = kv_get_uint("memory.embed_own_replies") != 0;
  c.decay_sweep_interval_secs = (uint32_t)kv_get_uint("memory.decay_sweep_interval_secs");

  em = kv_get_str("memory.embed_model");
  snprintf(c.embed_model, sizeof(c.embed_model), "%s", em ? em : "");

  ri = kv_get_str("memory.recall_instruct");
  snprintf(c.recall_instruct, sizeof(c.recall_instruct), "%s", ri ? ri : "");

  xr = kv_get_str("memory.embed_exclude_regex");
  snprintf(c.embed_exclude_regex, sizeof(c.embed_exclude_regex), "%s",
      xr ? xr : "");

  if(c.log_retention_days == 0)
    c.log_retention_days = MEM_DEF_LOG_RETENTION_DAYS;

  if(c.fact_decay_half_life_days == 0)
    c.fact_decay_half_life_days = MEM_DEF_FACT_DECAY_HALF_LIFE;

  if(c.min_fact_confidence <= 0.0f || c.min_fact_confidence > 1.0f)
    c.min_fact_confidence = 0.6f;

  if(c.rag_top_k == 0)
    c.rag_top_k = MEM_DEF_RAG_TOP_K;

  if(c.recall_top_k == 0)
    c.recall_top_k = MEM_DEF_RECALL_TOP_K;

  if(c.rag_max_context_chars == 0)
    c.rag_max_context_chars = MEM_DEF_RAG_MAX_CONTEXT_CHARS;

  if(c.decay_sweep_interval_secs == 0)
    c.decay_sweep_interval_secs = MEM_DEF_DECAY_SWEEP_INTERVAL_SEC;

  if(c.embed_batch_size == 0)
    c.embed_batch_size = MEM_DEF_EMBED_BATCH_SIZE;

  if(c.embed_batch_size > MEM_EMBED_BATCH_MAX)
    c.embed_batch_size = MEM_EMBED_BATCH_MAX;

  // embed_burst_max is NOT clamped up either — 0 disables the burst
  // gate. The window is, since a zero-second window would count only
  // rows sharing the current instant and never trip.
  if(c.embed_burst_max > 0 && c.embed_burst_secs == 0)
    c.embed_burst_secs = MEM_DEF_EMBED_BURST_SECS;

  // embed_min_chars is deliberately NOT clamped up to its default the
  // way every knob above is: 0 is a meaningful value here, meaning
  // "disable the content filter", the same escape hatch
  // memory.recall_min_cosine uses. Clamping would remove it.
  // recall_instruct is left alone for the same reason: an empty or
  // sentinel value means "submit the recall query raw", and it is
  // memory_recall_render_query() that interprets it.

  pthread_mutex_lock(&memory_cfg_mutex);
  memory_cfg = c;
  pthread_mutex_unlock(&memory_cfg_mutex);

  // Outside the cfg lock: the pattern lives in its own compiled form
  // under its own rwlock, and holding two locks to publish one knob buys
  // nothing.
  memory_exclude_regex_compile(c.embed_exclude_regex);
}

static void
memory_kv_changed(const char *key, void *data)
{
  (void)key;
  (void)data;
  memory_load_config();
}

static void
memory_register_kv(void)
{
  kv_register("memory.enabled", KV_BOOL, "true",
      memory_kv_changed, NULL, "Enable memory subsystem");
  kv_register("memory.witness_embeds", KV_BOOL, "true",
      memory_kv_changed, NULL,
      "Embed overheard (WITNESS) lines, not just lines addressed to the"
      " bot. On by default: with it off the conversation corpus is only"
      " the handful of directed lines, which is too small for semantic"
      " recall to return anything but noise. ⚠ It widens what the bot"
      " can recall about people who never spoke to it — a privacy"
      " posture, not just a tuning knob.");
  kv_register("memory.log_retention_days", KV_UINT32, "30",
      memory_kv_changed, NULL, "Days to retain conversation_log rows");
  kv_register("memory.fact_decay_half_life_days", KV_UINT32, "30",
      memory_kv_changed, NULL, "Fact-confidence half-life in days");
  kv_register("memory.min_fact_confidence", KV_FLOAT, "0.6",
      memory_kv_changed, NULL, "Minimum decayed confidence to keep a fact");
  kv_register("memory.rag_top_k", KV_UINT32, "8",
      memory_kv_changed, NULL, "Top-K facts+messages returned by retrieve");
  kv_register("memory.recall_top_k", KV_UINT32, "4",
      memory_kv_changed, NULL,
      "Top-K semantic-recall hits from conversation_embeddings"
      " (dossier-scoped); 0 uses default");
  kv_register("memory.recall_min_cosine", KV_UINT32, "0",
      memory_kv_changed, NULL,
      "Cosine floor for semantic recall, stored x100"
      " (e.g. 30 = 0.30); 0 disables the floor");
  kv_register("memory.rag_max_context_chars", KV_UINT32, "2048",
      memory_kv_changed, NULL, "Max characters of RAG context injected");
  kv_register("memory.embed_own_replies", KV_BOOL, "false",
      memory_kv_changed, NULL,
      "Include bot's own replies in RAG (embed + mention recall)."
      " Default off to avoid echo-chamber tics where the bot copies its"
      " own prior phrasing verbatim.");
  kv_register("memory.decay_sweep_interval_secs", KV_UINT32, "3600",
      memory_kv_changed, NULL, "Seconds between decay sweeps");
  kv_register("memory.embed_model", KV_STR, "",
      memory_kv_changed, NULL,
      "LLM embed model for memory RAG (empty = embeddings disabled)");
  kv_register("memory.embed_min_chars", KV_UINT32, "24",
      memory_kv_changed, NULL,
      "Minimum trimmed length in bytes a conversation line needs before"
      " it is embedded for semantic recall. Short strings ('!', 'heh',"
      " '....') embed to hub vectors roughly equidistant from every"
      " query, so they outrank genuine matches and must be kept out of"
      " the corpus — a read-time cosine floor cannot substitute, since"
      " the hubs are what score high. 0 disables the filter.");
  kv_register("memory.embed_batch_size", KV_UINT32, "32",
      memory_kv_changed, NULL,
      "Rows per embed request during a /bot <name> embedbackfill run."
      " Higher means fewer round trips and better GPU utilisation on a"
      " batching-aware endpoint; bounded at compile time by"
      " MEM_EMBED_BATCH_MAX. Does not affect the live embed path, which"
      " submits one line at a time as it is logged.");
  kv_register("memory.recall_instruct", KV_STR, MEM_DEF_RECALL_INSTRUCT,
      memory_kv_changed, NULL,
      "Task sentence prefixed to the semantic-recall query for"
      " /show bot <name> memories. Qwen3 embedders are asymmetric —"
      " the query is instructed, stored documents never are, so this"
      " needs no re-embed. Without it a bare one-word query embeds to"
      " a hub vector and returns the corpus's blandest line."
      " 'off' or 'none' disables it. Does NOT affect live bot replies"
      " or knowledge RAG.");
  kv_register("memory.embed_burst_max", KV_UINT32, "4",
      memory_kv_changed, NULL,
      "Messages from one person within memory.embed_burst_secs before"
      " the burst is treated as a paste and none of the window is"
      " embedded — including the lines already embedded before the"
      " threshold tripped, whose vectors are deleted. A human does not"
      " post four lines in five seconds; a paste, or a bot dumping a"
      " table, does. 0 disables the gate.");
  kv_register("memory.embed_burst_secs", KV_UINT32, "5",
      memory_kv_changed, NULL,
      "Window in seconds for memory.embed_burst_max. Ignored when that"
      " is 0.");
  kv_register("memory.embed_exclude_regex", KV_STR,
      MEM_DEF_EMBED_EXCLUDE_REGEX,
      memory_kv_changed, NULL,
      "POSIX extended regex; a conversation line matching it earns no"
      " vector. Catches what a burst cannot — code, diffs and tables a"
      " human pasted at ordinary typing speed — which reach live reply"
      " prompts and displace real content. Matched against the line with"
      " leading whitespace stripped, so anchor alternatives with ^."
      " ⛔ Never add a whitespace-run alternative: it reads as column"
      " output and eats the speech of anyone who double-spaces after a"
      " sentence. 'off' or 'none' disables it; a pattern that does not"
      " compile logs a WARN and disables it too.");
}

// BYTEA helpers (float32 LE packing + hex serialization for Postgres).
// The pg text protocol returns BYTEA as '\x<hex>'; we emit the same form
// on insert. No pgvector, no libpq binary binding — simple and portable.

static const char memory_hex_digits[] = "0123456789abcdef";

// Allocate "'\xAABB...'::bytea" literal from a float32 vector. Caller frees.
static char *
memory_vec_to_bytea_literal(const float *vec, uint32_t dim)
{
  size_t n_bytes = (size_t)dim * sizeof(float);
  size_t cap = n_bytes * 2 + 16;
  char *out = mem_alloc("memory", "bytea_lit", cap);
  const unsigned char *bytes;
  size_t w;

  if(out == NULL)
    return(NULL);

  bytes = (const unsigned char *)vec;
  w = 0;
  out[w++] = '\'';
  out[w++] = '\\';
  out[w++] = 'x';

  for(size_t i = 0; i < n_bytes; i++)
  {
    out[w++] = memory_hex_digits[(bytes[i] >> 4) & 0xF];
    out[w++] = memory_hex_digits[bytes[i] & 0xF];
  }

  out[w++] = '\'';
  out[w] = '\0';
  return(out);
}

// Parse a BYTEA cell value ("\\x<hex>...") into a heap float32 buffer of
// length *dim_out. Returns NULL on malformed input. Caller frees with
// mem_free.
float *
memory_bytea_to_vec(const char *cell, uint32_t expected_dim)
{
  unsigned char *bytes;
  const char *h;
  float *out;
  size_t n_bytes;
  size_t hex_len;

  if(cell == NULL)
    return(NULL);

  // pg text format: leading "\x" followed by hex pairs.
  if(cell[0] != '\\' || cell[1] != 'x')
    return(NULL);

  hex_len = strlen(cell + 2);

  if((hex_len & 1) != 0)
    return(NULL);

  n_bytes = hex_len / 2;

  if(n_bytes != (size_t)expected_dim * sizeof(float))
    return(NULL);

  out = mem_alloc("memory", "vec", n_bytes);

  if(out == NULL)
    return(NULL);

  bytes = (unsigned char *)out;
  h = cell + 2;

  for(size_t i = 0; i < n_bytes; i++)
  {
    unsigned char hi, lo;
    char c = h[i * 2];
    if(c >= '0' && c <= '9')      hi = (unsigned char)(c - '0');
    else if(c >= 'a' && c <= 'f') hi = (unsigned char)(c - 'a' + 10);
    else if(c >= 'A' && c <= 'F') hi = (unsigned char)(c - 'A' + 10);
    else { mem_free(out); return(NULL); }

    c = h[i * 2 + 1];
    if(c >= '0' && c <= '9')      lo = (unsigned char)(c - '0');
    else if(c >= 'a' && c <= 'f') lo = (unsigned char)(c - 'a' + 10);
    else if(c >= 'A' && c <= 'F') lo = (unsigned char)(c - 'A' + 10);
    else { mem_free(out); return(NULL); }

    bytes[i] = (unsigned char)((hi << 4) | lo);
  }

  return(out);
}

// Cosine similarity. Returns 0 if either norm is zero.
float
memory_cosine(const float *a, const float *b, uint32_t dim)
{
  double dot = 0.0, na = 0.0, nb = 0.0;

  for(uint32_t i = 0; i < dim; i++)
  {
    dot += (double)a[i] * (double)b[i];
    na  += (double)a[i] * (double)a[i];
    nb  += (double)b[i] * (double)b[i];
  }

  if(na == 0.0 || nb == 0.0)
    return(0.0f);

  return((float)(dot / (sqrt(na) * sqrt(nb))));
}

// Persist an embedding row. Returns SUCCESS on success.
bool
memory_write_embedding(const char *table, const char *id_col, int64_t id,
    const char *model, uint32_t dim, const float *vec)
{
  char *hex = memory_vec_to_bytea_literal(vec, dim);
  char *e_model = db_escape(model);
  db_result_t *res;
  bool ok;
  size_t cap;
  char *sql;

  if(hex == NULL || e_model == NULL)
  {
    if(hex)     mem_free(hex);
    if(e_model) mem_free(e_model);
    return(FAIL);
  }

  cap = strlen(hex) + strlen(e_model) + 256;
  sql = mem_alloc("memory", "embed_sql", cap);

  if(sql == NULL)
  {
    mem_free(hex); mem_free(e_model);
    return(FAIL);
  }

  snprintf(sql, cap,
      "INSERT INTO %s (%s, model, dim, vec)"
      " VALUES (%" PRId64 ", '%s', %u, %s::bytea)"
      " ON CONFLICT (%s) DO UPDATE"
      " SET model = EXCLUDED.model,"
      "     dim   = EXCLUDED.dim,"
      "     vec   = EXCLUDED.vec",
      table, id_col, id, e_model, dim, hex, id_col);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok;

  if(!ok && res->error[0] != '\0')
    clam(CLAM_WARN, "memory", "write_embedding: %s", res->error);

  db_result_free(res);
  mem_free(sql);
  mem_free(hex);
  mem_free(e_model);
  return(ok ? SUCCESS : FAIL);
}

// Async embed completion: writes the vector row.

typedef struct
{
  int64_t id;
  char    model[MEM_EMBED_MODEL_SZ];
} memory_embed_ctx_t;

static void
memory_embed_done(const llm_embed_response_t *resp)
{
  memory_embed_ctx_t *c = resp->user_data;

  if(!resp->ok || resp->n_vectors < 1 || resp->dim == 0)
  {
    clam(CLAM_WARN, "memory", "embed failed: %s",
        resp->error ? resp->error : "(no detail)");
    mem_free(c);
    return;
  }

  memory_write_embedding("conversation_embeddings", "msg_id",
      c->id, c->model, resp->dim, resp->vectors[0]);

  mem_free(c);
}

// True when a line carries enough substance to be worth a vector.
//
// Short degenerate strings embed to *hub* vectors that sit roughly
// equidistant from everything, so they outrank real matches on every
// query: measured, query "coffee" scores "!" at 0.976 and "I drink
// black coffee every morning" at 0.509. Keeping them out of the corpus
// is the only lever that works — a read-time cosine floor cannot
// substitute, because the hubs are precisely what score high, so a
// floor discards the genuine matches first.
//
// Three gates, all of which must pass:
//
//   1. trimmed byte length >= min_chars (0 disables gates 1 and 2)
//   2. at least MEM_EMBED_MIN_TOKENS content-bearing tokens
//   3. no match against memory.embed_exclude_regex
//
// Gate 3 is independent and carries its own escape hatch, which is why
// min_chars == 0 does not disable it: length says nothing about whether
// a line is a pasted diff, and a pasted diff is long.
//
// Rule 2 is not a knob on purpose. A single-token line is the hub case
// itself and no threshold makes "heh" worth recalling, and it is also
// what rejects "....", "!!!" and ":)" however long they run.
//
// A byte >= 0x80 counts as content, so non-ASCII scripts clear rule 2
// (isalnum() would reject every UTF-8 continuation byte in the C
// locale). Rule 1 counting bytes then works in their favour — a CJK
// sentence reaches 24 bytes at roughly 8 characters.
//
// Two deliberate consequences: a bare URL is long but single-token and
// so is rejected, which is intended (a link has no text to recall on,
// and urlgrabber already owns links); and the IRC action prefix
// "* <nick> " is NOT stripped here, unlike extract_prompt_build() —
// an action is content and counts toward both gates.

// Gate 3. `text` must already have had its leading whitespace stripped:
// the shipped alternatives are ^-anchored, and " karen  20  12  0" would
// slip past every one of them. Trailing whitespace is harmless, so the
// caller passes a pointer into its own string rather than a trimmed copy.
static bool
memory_text_excluded(const char *text)
{
  bool hit;

  pthread_rwlock_rdlock(&memory_exclude_rx_lock);
  hit = memory_exclude_rx_valid
      && regexec(&memory_exclude_rx, text, 0, NULL, 0) == 0;
  pthread_rwlock_unlock(&memory_exclude_rx_lock);

  return(hit);
}

bool
memory_text_is_embeddable(const char *text, uint32_t min_chars)
{
  const char *start;
  const char *end;
  const char *p;
  size_t      n_tokens;
  bool        in_token;
  bool        tok_content;

  if(text == NULL)
    return(false);

  start = text;

  while(*start != '\0' && isspace((unsigned char)*start))
    start++;

  if(memory_text_excluded(start))
    return(false);

  if(min_chars == 0)
    return(true);

  end = start + strlen(start);

  while(end > start && isspace((unsigned char)end[-1]))
    end--;

  if((size_t)(end - start) < (size_t)min_chars)
    return(false);

  n_tokens    = 0;
  in_token    = false;
  tok_content = false;

  for(p = start; p < end; p++)
  {
    if(isspace((unsigned char)*p))
    {
      if(in_token && tok_content)
      {
        n_tokens++;

        if(n_tokens >= MEM_EMBED_MIN_TOKENS)
          return(true);
      }

      in_token    = false;
      tok_content = false;
      continue;
    }

    in_token = true;

    if(isalnum((unsigned char)*p) || (unsigned char)*p >= 0x80)
      tok_content = true;
  }

  if(in_token && tok_content)
    n_tokens++;

  return(n_tokens >= MEM_EMBED_MIN_TOKENS);
}

// Submit an embed job if the embed model is configured. The caller owns
// the *kind* decision (WITNESS / EXCHANGE_IN / EXCHANGE_OUT); the
// *content* gate is applied here so that every write path — this one
// and any batch backfill — shares one rule and the two cannot drift.
//
// Only conversation_log rows are embedded. Facts are not: the dossier
// store has no embedding table and its facts are retrieved by dossier
// id, not by cosine.
static void
memory_submit_embed(int64_t id, const char *model, const char *text,
    uint32_t min_chars)
{
  const char *inputs[1] = { text };
  memory_embed_ctx_t *c;

  if(model[0] == '\0' || text == NULL || text[0] == '\0')
    return;

  if(!memory_text_is_embeddable(text, min_chars))
    return;

  c = mem_alloc("memory", "embed_ctx", sizeof(*c));

  if(c == NULL)
    return;

  c->id = id;
  snprintf(c->model, sizeof(c->model), "%s", model);

  if(llm_embed_submit(model, inputs, 1, memory_embed_done, c) != SUCCESS)
  {
    // Model missing / unregistered / queue full -- log once.
    static bool warned = false;
    if(!warned)
    {
      clam(CLAM_WARN, "memory",
          "embed submit failed (model '%s' unavailable); skipping", model);
      warned = true;
    }
    mem_free(c);
  }
}

// Schema

static void
memory_run_ddl(const char *sql)
{
  db_result_t *res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    if(res->error[0] != '\0')
      clam(CLAM_WARN, "memory", "ensure_tables: %s", res->error);
  }

  db_result_free(res);
}

static void
memory_ensure_tables(void)
{
  memory_run_ddl(
      "CREATE TABLE IF NOT EXISTS conversation_log ("
      " id         BIGSERIAL    PRIMARY KEY,"
      " user_id    INTEGER      REFERENCES userns_user(id) ON DELETE SET NULL,"
      " ns_id      INTEGER      NOT NULL REFERENCES userns(id) ON DELETE CASCADE,"
      " bot_name   VARCHAR(64)  NOT NULL,"
      " method     VARCHAR(64)  NOT NULL,"
      " channel    VARCHAR(128) NOT NULL DEFAULT '',"
      " kind       SMALLINT     NOT NULL,"
      " text       TEXT         NOT NULL,"
      " dossier_id BIGINT,"   // FK to dossier(id) attached by dossier subsystem
      " referenced_dossiers JSONB,"   // array of dossier ids mentioned in text
      " ts         TIMESTAMPTZ  NOT NULL DEFAULT NOW()"
      ")");

  // Idempotent add-column for upgraded installs (pre-release but the
  // column might be missing if conversation_log was created by an
  // earlier build before dossier landed).
  memory_run_ddl(
      "ALTER TABLE conversation_log"
      " ADD COLUMN IF NOT EXISTS dossier_id BIGINT");
  memory_run_ddl(
      "ALTER TABLE conversation_log"
      " ADD COLUMN IF NOT EXISTS referenced_dossiers JSONB");

  memory_run_ddl(
      "CREATE INDEX IF NOT EXISTS idx_conv_ns_chan_ts"
      " ON conversation_log(ns_id, channel, ts DESC)");
  memory_run_ddl(
      "CREATE INDEX IF NOT EXISTS idx_conv_user_ts"
      " ON conversation_log(user_id, ts DESC)");
  memory_run_ddl(
      "CREATE INDEX IF NOT EXISTS idx_conv_dossier_ts"
      " ON conversation_log(dossier_id, ts DESC)");
  memory_run_ddl(
      "CREATE INDEX IF NOT EXISTS idx_conv_refs_gin"
      " ON conversation_log USING GIN (referenced_dossiers)");

  memory_run_ddl(
      "CREATE TABLE IF NOT EXISTS conversation_embeddings ("
      " msg_id  BIGINT      PRIMARY KEY REFERENCES conversation_log(id) ON DELETE CASCADE,"
      " model   VARCHAR(64) NOT NULL,"
      " dim     INTEGER     NOT NULL,"
      " vec     BYTEA       NOT NULL"
      ")");

  // Attach the dossier FK on conversation_log.dossier_id. Previously
  // added by the late-FK DO block in scripts/schema.sql; with the
  // conversation_log table owned here (R1), the FK attach moves in
  // too. Dossier tables were ensured by core's dossier_register_config
  // earlier in startup, so dossier(id) is guaranteed to exist by now.
  memory_run_ddl(
      "DO $$"
      " BEGIN"
      "   IF NOT EXISTS ("
      "     SELECT 1 FROM pg_constraint"
      "       WHERE conname = 'conversation_log_dossier_fk'"
      "   ) THEN"
      "     ALTER TABLE conversation_log"
      "       ADD CONSTRAINT conversation_log_dossier_fk"
      "       FOREIGN KEY (dossier_id)"
      "         REFERENCES dossier(id) ON DELETE SET NULL;"
      "   END IF;"
      " END$$");
}

// Dossier-keyed facts
//
// The only fact store. Facts hang off dossier.id -- what the bot has
// learned about a human -- never off userns_user.id, which identifies
// an auth principal and says nothing about who was talking.
//
// Facts carry no embeddings: they are retrieved by dossier id, and only
// conversation_log rows are cosine-scanned.

// Parse a single dossier_facts row produced by MEMORY_DOSSIER_FACT_SELECT_COLS.
void
memory_parse_dossier_fact_row(const db_result_t *r, uint32_t row,
    mem_dossier_fact_t *f)
{
  const char *v;

  memset(f, 0, sizeof(*f));

  v = db_result_get(r, row, 0);
  if(v != NULL) f->id = (int64_t)strtoll(v, NULL, 10);

  v = db_result_get(r, row, 1);
  if(v != NULL) f->dossier_id = (int64_t)strtoll(v, NULL, 10);

  v = db_result_get(r, row, 2);
  if(v != NULL) f->kind = (mem_fact_kind_t)strtol(v, NULL, 10);

  v = db_result_get(r, row, 3);
  if(v != NULL) snprintf(f->fact_key, sizeof(f->fact_key), "%s", v);

  v = db_result_get(r, row, 4);
  if(v != NULL) snprintf(f->fact_value, sizeof(f->fact_value), "%s", v);

  v = db_result_get(r, row, 5);
  if(v != NULL) snprintf(f->source, sizeof(f->source), "%s", v);

  v = db_result_get(r, row, 6);
  if(v != NULL) snprintf(f->channel, sizeof(f->channel), "%s", v);

  v = db_result_get(r, row, 7);
  if(v != NULL) f->confidence = (float)strtod(v, NULL);

  v = db_result_get(r, row, 8);
  if(v != NULL) f->observed_at = (time_t)strtoll(v, NULL, 10);

  v = db_result_get(r, row, 9);
  if(v != NULL) f->last_seen = (time_t)strtoll(v, NULL, 10);
}

bool
memory_upsert_dossier_fact(const mem_dossier_fact_t *fact,
    mem_merge_t policy)
{
  db_result_t *res;
  bool ok;
  size_t cap;
  char *sql;
  char *e_key;
  char *e_val;
  char *e_src;
  char *e_chan;

  if(!memory_ready || fact == NULL || fact->dossier_id <= 0)
    return(FAIL);

  e_key = db_escape(fact->fact_key);
  e_val = db_escape(fact->fact_value);
  e_src = db_escape(fact->source[0] ? fact->source : "llm_extract");
  e_chan = db_escape(fact->channel);

  if(e_key == NULL || e_val == NULL || e_src == NULL || e_chan == NULL)
  {
    if(e_key)  mem_free(e_key);
    if(e_val)  mem_free(e_val);
    if(e_src)  mem_free(e_src);
    if(e_chan) mem_free(e_chan);
    return(FAIL);
  }

  cap = MEM_SQL_SZ + MEM_FACT_VALUE_SZ * 2;
  sql = mem_alloc("memory", "upsert_p_sql", cap);

  switch(policy)
  {
    case MEM_MERGE_REPLACE:
      snprintf(sql, cap,
          "INSERT INTO dossier_facts"
          " (dossier_id, kind, fact_key, fact_value, source, channel, confidence)"
          " VALUES (%" PRId64 ", %d, '%s', '%s', '%s', '%s', %f)"
          " ON CONFLICT (dossier_id, kind, fact_key) DO UPDATE"
          " SET fact_value = EXCLUDED.fact_value,"
          "     source     = EXCLUDED.source,"
          "     channel    = EXCLUDED.channel,"
          "     confidence = EXCLUDED.confidence,"
          "     last_seen  = NOW()",
          fact->dossier_id, (int)fact->kind, e_key, e_val, e_src, e_chan,
          (double)fact->confidence);
      break;

    case MEM_MERGE_HIGHER_CONF:
      snprintf(sql, cap,
          "INSERT INTO dossier_facts"
          " (dossier_id, kind, fact_key, fact_value, source, channel, confidence)"
          " VALUES (%" PRId64 ", %d, '%s', '%s', '%s', '%s', %f)"
          " ON CONFLICT (dossier_id, kind, fact_key) DO UPDATE"
          " SET fact_value = CASE WHEN EXCLUDED.confidence > dossier_facts.confidence"
          "         THEN EXCLUDED.fact_value ELSE dossier_facts.fact_value END,"
          "     source     = CASE WHEN EXCLUDED.confidence > dossier_facts.confidence"
          "         THEN EXCLUDED.source     ELSE dossier_facts.source     END,"
          "     channel    = CASE WHEN EXCLUDED.confidence > dossier_facts.confidence"
          "         THEN EXCLUDED.channel    ELSE dossier_facts.channel    END,"
          "     confidence = GREATEST(dossier_facts.confidence, EXCLUDED.confidence),"
          "     last_seen  = NOW()",
          fact->dossier_id, (int)fact->kind, e_key, e_val, e_src, e_chan,
          (double)fact->confidence);
      break;

    case MEM_MERGE_APPEND_HISTORY:
      snprintf(sql, cap,
          "INSERT INTO dossier_facts"
          " (dossier_id, kind, fact_key, fact_value, source, channel, confidence)"
          " VALUES (%" PRId64 ", %d, '%s', '%s', '%s', '%s', %f)"
          " ON CONFLICT (dossier_id, kind, fact_key) DO UPDATE"
          " SET fact_value = dossier_facts.fact_value || E'\\n---\\n'"
          "                  || EXCLUDED.fact_value,"
          "     last_seen  = NOW(),"
          "     confidence = GREATEST(dossier_facts.confidence, EXCLUDED.confidence)",
          fact->dossier_id, (int)fact->kind, e_key, e_val, e_src, e_chan,
          (double)fact->confidence);
      break;

    default:
      mem_free(sql);
      mem_free(e_key); mem_free(e_val); mem_free(e_src); mem_free(e_chan);
      return(FAIL);
  }

  mem_free(e_key); mem_free(e_val); mem_free(e_src); mem_free(e_chan);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok;

  if(!ok && res->error[0] != '\0')
    clam(CLAM_WARN, "memory", "upsert_dossier_fact: %s", res->error);

  db_result_free(res);
  mem_free(sql);

  if(ok)
    memory_stat_bump_facts();

  return(ok ? SUCCESS : FAIL);
}

size_t
memory_get_dossier_facts(int64_t dossier_id, uint32_t kinds_mask,
    mem_dossier_fact_t *out, size_t cap)
{
  db_result_t *res;
  size_t n;
  char sql[2048];
  char kind_clause[256];

  if(!memory_ready || dossier_id <= 0 || out == NULL || cap == 0)
    return(0);

  kind_clause[0] = '\0';

  if(kinds_mask != MEM_FACT_KIND_ANY)
  {
    char buf[256];
    size_t w = 0;
    bool any = false;

    w += (size_t)snprintf(buf + w, sizeof(buf) - w, " AND kind IN (");

    for(uint32_t k = 0; k <= MEM_FACT_FREEFORM; k++)
    {
      if(kinds_mask & MEM_FACT_KIND_BIT(k))
      {
        w += (size_t)snprintf(buf + w, sizeof(buf) - w,
            "%s%u", any ? "," : "", k);
        any = true;
      }
    }

    w += (size_t)snprintf(buf + w, sizeof(buf) - w, ")");

    if(!any)
      return(0);

    snprintf(kind_clause, sizeof(kind_clause), "%s", buf);
  }

  snprintf(sql, sizeof(sql),
      "SELECT " MEMORY_DOSSIER_FACT_SELECT_COLS
      " FROM dossier_facts WHERE dossier_id = %" PRId64 "%s"
      " ORDER BY last_seen DESC LIMIT %zu",
      dossier_id, kind_clause, cap);

  res = db_result_alloc();
  n = 0;

  if(db_query(sql, res) == SUCCESS && res->ok)
  {
    for(uint32_t i = 0; i < res->rows && n < cap; i++)
      memory_parse_dossier_fact_row(res, i, &out[n++]);
  }

  else if(res->error[0] != '\0')
    clam(CLAM_WARN, "memory", "get_dossier_facts: %s", res->error);

  db_result_free(res);
  return(n);
}

bool
memory_forget_dossier_fact(int64_t fact_id)
{
  db_result_t *res;
  bool ok;
  char sql[128];

  if(!memory_ready)
    return(FAIL);

  snprintf(sql, sizeof(sql),
      "DELETE FROM dossier_facts WHERE id = %" PRId64, fact_id);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok;

  if(!ok && res->error[0] != '\0')
    clam(CLAM_WARN, "memory", "forget_dossier_fact: %s", res->error);

  db_result_free(res);

  if(ok)
    memory_stat_bump_forgets();

  return(ok ? SUCCESS : FAIL);
}

// Conversation log

// Gate A. A human does not post four lines in five seconds; a paste, or
// a bot dumping a table, does. Measured on the live corpus the shipped
// threshold costs 0 of 1,546 normal rows and 0 of the 25 rows belonging
// to the one human who triple-spaces after a sentence.
//
// ⭑ The retroactive delete is what makes this gate work at all. The
// decision is per-message at log time, so when line 1 of a paste arrives
// nothing yet indicates a burst and lines 1..max-1 are embedded before
// the threshold trips. Deleting the whole window's vectors is therefore
// part of the gate, not a tidy-up. It re-runs on every later message of
// the same burst, which is what sweeps up a straggler whose async embed
// landed after an earlier delete — self-healing by construction, so do
// NOT add in-flight tracking to close that race. Re-running the delete
// is cheaper than preventing it.
//
// ⚠ The look-back is a DB query and deliberately not an in-memory
// per-sender ring: a ring is zeroed by /plugin reload chat, which
// happens many times a day, and a zeroed timing guard reads as "not a
// burst" (the failure class finding_reload_zeroed_timestamp_guards
// records). idx_conv_dossier_ts makes this an index-only scan, measured
// at 0.015 ms. The window is expressed with the server's now() because
// mem_msg_t.ts is 0 for "NOW()" on the live path, and the row this call
// is deciding about was inserted moments ago by the same caller.
static bool
memory_burst_suppressed(int64_t dossier_id, uint32_t max, uint32_t secs)
{
  db_result_t *res;
  const char  *cell;
  char         sql[MEM_SQL_SZ];
  uint64_t     n;

  // An unattributable speaker cannot be burst-detected: counting a
  // channel's whole traffic as one sender's would suppress everyone.
  if(max == 0 || dossier_id <= 0)
    return(false);

  snprintf(sql, sizeof(sql),
      "SELECT count(*) FROM conversation_log"
      " WHERE dossier_id = %" PRId64
      " AND ts > now() - interval '%u seconds'", dossier_id, secs);

  res  = db_result_alloc();
  cell = NULL;
  n    = 0;

  if(db_query(sql, res) != SUCCESS || !res->ok)
    clam(CLAM_WARN, "memory", "burst count: %s", res->error);

  else if(res->rows > 0 && (cell = db_result_get(res, 0, 0)) != NULL)
    n = strtoull(cell, NULL, 10);

  db_result_free(res);

  if(n < (uint64_t)max)
    return(false);

  snprintf(sql, sizeof(sql),
      "DELETE FROM conversation_embeddings e USING conversation_log x"
      " WHERE x.id = e.msg_id AND x.dossier_id = %" PRId64
      " AND x.ts > now() - interval '%u seconds'", dossier_id, secs);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
    clam(CLAM_WARN, "memory", "burst purge: %s", res->error);

  db_result_free(res);

  clam(CLAM_DEBUG, "memory",
      "burst: dossier %" PRId64 " sent %" PRIu64 " lines in %us;"
      " window not embedded", dossier_id, n, secs);

  return(true);
}

void
memory_log_message(const mem_msg_t *msg)
{
  db_result_t *res;
  int64_t new_id;
  bool ok;
  char refs_cell[256];
  char *sql;
  char user_cell[32];
  char pers_cell[32];
  char *e_bot;
  char *e_meth;
  char *e_chan;
  char *e_text;
  mem_cfg_t cfg;

  if(!memory_ready || msg == NULL)
    return;

  memory_cfg_snapshot(&cfg);

  if(!cfg.enabled)
    return;

  e_bot = db_escape(msg->bot_name);
  e_meth = db_escape(msg->method);
  e_chan = db_escape(msg->channel);
  e_text = db_escape(msg->text);

  if(e_bot == NULL || e_meth == NULL || e_chan == NULL || e_text == NULL)
    goto cleanup;

  sql = mem_alloc("memory", "log_sql", MEM_SQL_SZ + MEM_MSG_TEXT_SZ);

  if(msg->user_id_or_0 > 0)
    snprintf(user_cell, sizeof(user_cell), "%d", msg->user_id_or_0);
  else
    snprintf(user_cell, sizeof(user_cell), "NULL");
  if(msg->dossier_id > 0)
    snprintf(pers_cell, sizeof(pers_cell), "%" PRId64, msg->dossier_id);
  else
    snprintf(pers_cell, sizeof(pers_cell), "NULL");

  // referenced_dossiers as a JSONB literal. NULL preserves the
  // "not computed" state; [] would be "computed and empty".
  if(msg->n_referenced == 0)
    snprintf(refs_cell, sizeof(refs_cell), "NULL");
  else
  {
    size_t off = 0;
    off += (size_t)snprintf(refs_cell + off, sizeof(refs_cell) - off, "'[");
    for(uint8_t i = 0; i < msg->n_referenced
        && off < sizeof(refs_cell) - 16; i++)
      off += (size_t)snprintf(refs_cell + off, sizeof(refs_cell) - off,
          "%s%" PRId64, i == 0 ? "" : ",",
          msg->referenced_dossiers[i]);
    snprintf(refs_cell + off, sizeof(refs_cell) - off, "]'::jsonb");
  }

  snprintf(sql, MEM_SQL_SZ + MEM_MSG_TEXT_SZ,
      "INSERT INTO conversation_log"
      " (user_id, ns_id, bot_name, method, channel, kind, text, dossier_id,"
      "  referenced_dossiers)"
      " VALUES (%s, %d, '%s', '%s', '%s', %d, '%s', %s, %s) RETURNING id",
      user_cell, msg->ns_id, e_bot, e_meth, e_chan,
      (int)msg->kind, e_text, pers_cell, refs_cell);

  res = db_result_alloc();
  new_id = 0;
  ok = false;

  if(db_query(sql, res) != SUCCESS || !res->ok)
    clam(CLAM_WARN, "memory", "log_message: %s", res->error);
  else
  {
    memory_stat_bump_logs();
    ok = true;

    if(res->rows > 0)
    {
      const char *id_s = db_result_get(res, 0, 0);
      if(id_s != NULL) new_id = (int64_t)strtoll(id_s, NULL, 10);
    }
  }

  db_result_free(res);
  mem_free(sql);

  // Embed eligibility:
  //   WITNESS       -> cfg.witness_embeds
  //   EXCHANGE_IN   -> always (directed at the bot)
  //   EXCHANGE_OUT  -> cfg.embed_own_replies
  if(ok && new_id > 0 && cfg.embed_model[0] != '\0' && msg->text[0] != '\0')
  {
    bool do_embed = false;

    switch(msg->kind)
    {
      case MEM_MSG_WITNESS:      do_embed = cfg.witness_embeds;    break;
      case MEM_MSG_EXCHANGE_IN:  do_embed = true;                  break;
      case MEM_MSG_EXCHANGE_OUT: do_embed = cfg.embed_own_replies; break;
    }

    // Gate A applies to EXCHANGE_IN as well: a human pasting *at* the
    // bot is still pasting. It lives here rather than in
    // memory_text_is_embeddable() because it needs the speaker and the
    // clock, which a text predicate has no business knowing — and
    // because the backfill must NOT inherit it. See the note at
    // memory_backfill.c's filter call.
    if(do_embed && memory_burst_suppressed(msg->dossier_id,
        cfg.embed_burst_max, cfg.embed_burst_secs))
      do_embed = false;

    if(do_embed)
      memory_submit_embed(new_id, cfg.embed_model, msg->text,
          cfg.embed_min_chars);
  }

cleanup:
  if(e_bot)  mem_free(e_bot);
  if(e_meth) mem_free(e_meth);
  if(e_chan) mem_free(e_chan);
  if(e_text) mem_free(e_text);
}

size_t
memory_recent_own_replies(const char *bot_name, const char *method,
    const char *channel, uint32_t ns_id, uint32_t max_age_secs,
    mem_recent_reply_t *out, size_t max_k)
{
  db_result_t *res;
  char sql[1024];
  char age_clause[64];
  char *e_bot;
  char *e_meth;
  char *e_chan;
  size_t n;

  if(!memory_ready || out == NULL || max_k == 0)
    return(0);
  if(bot_name == NULL || method == NULL || channel == NULL)
    return(0);

  e_bot = db_escape(bot_name);
  e_meth = db_escape(method);
  e_chan = db_escape(channel);

  n = 0;

  if(e_bot == NULL || e_meth == NULL || e_chan == NULL)
    goto cleanup;

  age_clause[0] = '\0';
  if(max_age_secs > 0)
    snprintf(age_clause, sizeof(age_clause),
        " AND ts >= NOW() - INTERVAL '%u seconds'", max_age_secs);

  snprintf(sql, sizeof(sql),
      "SELECT text, EXTRACT(EPOCH FROM ts)::bigint"
      " FROM conversation_log"
      " WHERE bot_name = '%s' AND method = '%s' AND channel = '%s'"
      "   AND ns_id = %u AND kind = %d%s"
      " ORDER BY ts DESC LIMIT %zu",
      e_bot, e_meth, e_chan, ns_id,
      (int)MEM_MSG_EXCHANGE_OUT, age_clause, max_k);

  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok)
  {
    for(uint32_t i = 0; i < res->rows && n < max_k; i++)
    {
      const char *text_s = db_result_get(res, i, 0);
      const char *ts_s   = db_result_get(res, i, 1);

      snprintf(out[n].text, sizeof(out[n].text),
          "%s", text_s ? text_s : "");
      out[n].ts = ts_s != NULL ? (int64_t)strtoll(ts_s, NULL, 10) : 0;
      n++;
    }
  }

  else if(res->error[0] != '\0')
    clam(CLAM_WARN, "memory", "recent_own_replies: %s", res->error);

  db_result_free(res);

cleanup:
  if(e_bot)  mem_free(e_bot);
  if(e_meth) mem_free(e_meth);
  if(e_chan) mem_free(e_chan);
  return(n);
}

// Decay sweep

void
memory_decay_sweep(void)
{
  db_result_t *res;
  char sql[1024];
  mem_cfg_t cfg;

  if(!memory_ready)
    return;

  memory_cfg_snapshot(&cfg);

  // decayed(c, t) = c * exp(- (elapsed_secs / seconds_per_day)
  //                          * ln(2) / half_life_days)
  // We fold the constant 86400 into the division (Postgres side).
  snprintf(sql, sizeof(sql),
      "DELETE FROM dossier_facts"
      " WHERE source <> 'admin_seed'"
      "   AND confidence * exp("
      "         - (EXTRACT(EPOCH FROM NOW() - observed_at) / 86400.0)"
      "         * ln(2) / %u.0"
      "       ) < %f",
      cfg.fact_decay_half_life_days,
      (double)cfg.min_fact_confidence);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    if(res->error[0] != '\0')
      clam(CLAM_WARN, "memory", "decay_sweep: %s", res->error);
  }

  else
  {
    if(res->rows_affected > 0)
      clam(CLAM_INFO, "memory", "decay sweep removed %u fact(s)",
          res->rows_affected);
  }

  db_result_free(res);

  memory_last_sweep = time(NULL);
  memory_stat_bump_sweeps();
}

// Periodic-task trampoline.
static void
memory_decay_sweep_task_cb(task_t *t)
{
  memory_decay_sweep();
  t->state = TASK_ENDED;
}

// Stats

void
memory_get_stats(memory_stats_t *out)
{
  if(out == NULL)
    return;

  pthread_mutex_lock(&memory_stat_mutex);
  out->total_facts  = memory_stat_facts;
  out->total_logs   = memory_stat_logs;
  out->decay_sweeps = memory_stat_sweeps;
  out->forgets      = memory_stat_forgets;
  pthread_mutex_unlock(&memory_stat_mutex);
}


// Lifecycle

void
memory_init(void)
{
  if(memory_ready)
    return;

  pthread_mutex_init(&memory_cfg_mutex, NULL);
  pthread_mutex_init(&memory_stat_mutex, NULL);
  pthread_rwlock_init(&memory_exclude_rx_lock, NULL);

  memset(&memory_cfg, 0, sizeof(memory_cfg));
  memory_cfg.enabled                    = true;
  memory_cfg.log_retention_days         = MEM_DEF_LOG_RETENTION_DAYS;
  memory_cfg.fact_decay_half_life_days  = MEM_DEF_FACT_DECAY_HALF_LIFE;
  memory_cfg.min_fact_confidence        = 0.6f;
  memory_cfg.rag_top_k                  = MEM_DEF_RAG_TOP_K;
  memory_cfg.rag_max_context_chars      = MEM_DEF_RAG_MAX_CONTEXT_CHARS;
  memory_cfg.decay_sweep_interval_secs  = MEM_DEF_DECAY_SWEEP_INTERVAL_SEC;

  memory_ready = true;

  clam(CLAM_INFO, "memory", "memory subsystem initialized");
}

void
memory_register_config(void)
{
  memory_register_kv();
}

void
memory_ensure_schema(void)
{
  mem_cfg_t cfg;

  memory_load_config();
  memory_ensure_tables();

  // Schedule periodic decay sweep.
  memory_cfg_snapshot(&cfg);

  memory_sweep_task = task_add_periodic("memory.decay",
      TASK_ANY, 200,
      cfg.decay_sweep_interval_secs * 1000,
      memory_decay_sweep_task_cb, NULL);
}

void
memory_register_commands(void)
{
  memory_register_cmds_internal();
}

void
memory_stop(void)
{
  // A backfill outlives any one command, so stop it here rather than
  // letting its pump re-arm into an unloading mapping.
  memory_backfill_stop();

  if(memory_sweep_task == TASK_HANDLE_NONE)
    return;

  task_cancel(memory_sweep_task);
  memory_sweep_task = TASK_HANDLE_NONE;

  clam(CLAM_DEBUG, "memory", "decay sweep cancelled");
}

void
memory_exit(void)
{
  if(!memory_ready)
    return;

  memory_ready = false;

  // Nothing to flush (all DB writes are synchronous). The sweep is
  // memory_stop()'s business — by here it must already be cancelled.
  memory_sweep_task = TASK_HANDLE_NONE;

  if(memory_exclude_rx_valid)
  {
    regfree(&memory_exclude_rx);
    memory_exclude_rx_valid = false;
  }

  memory_exclude_rx_pattern[0] = '\0';

  pthread_mutex_destroy(&memory_cfg_mutex);
  pthread_mutex_destroy(&memory_stat_mutex);
  pthread_rwlock_destroy(&memory_exclude_rx_lock);

  clam(CLAM_INFO, "memory", "memory subsystem shut down");
}

// Test hooks

#ifdef MEMORY_TEST_HOOKS

bool
memory_test_db_ok(void)
{
  db_result_t *r = db_result_alloc();
  bool ok = (db_query("SELECT 1", r) == SUCCESS) && r->ok;
  db_result_free(r);
  return(ok);
}

void
memory_test_reset_stats(void)
{
  pthread_mutex_lock(&memory_stat_mutex);
  memory_stat_facts   = 0;
  memory_stat_logs    = 0;
  memory_stat_sweeps  = 0;
  memory_stat_forgets = 0;
  pthread_mutex_unlock(&memory_stat_mutex);
}

// Write a test embedding row directly. Avoids dragging the live
// llm_embed_submit path into unit tests -- the retrieval logic (cosine
// scan, top-K, join filters) is what we actually want to exercise.
bool
memory_test_inject_embedding(int64_t id, const char *model,
    uint32_t dim, const float *vec)
{
  if(!memory_ready || model == NULL || vec == NULL || dim == 0)
    return(FAIL);

  return(memory_write_embedding("conversation_embeddings", "msg_id",
      id, model, dim, vec));
}

#endif // MEMORY_TEST_HOOKS
