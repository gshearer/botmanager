// botmanager — MIT
// Chat bot memory store: one-shot embed backfill over conversation_log.

#define MEMORY_INTERNAL
#include "memory.h"

#include "bot.h"
#include "cmd.h"
#include "db.h"
#include "inference.h"
#include "method.h"
#include "task.h"
#include "userns.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// A backfill embeds every conversation_log row in a namespace that the
// live path never got to — rows written while the embed model was unset,
// and rows whose kind was ineligible under an older configuration. It is
// an operator action, not a background service: it runs once, reports,
// and stops.
//
// Exactly one batch is ever in flight. The pump task reads a batch,
// submits it, and ENDs; the embed completion callback arms the next pump.
// That is what keeps a 1,500-row sweep from flooding the DB pool the way
// a naive loop over llm_embed_submit() would — see
// finding_wm_compile_async_flood.

typedef struct
{
  bool     running;
  bool     stopping;
  uint32_t ns_id;
  int64_t  hwm;                        // highest conversation_log.id read
  char     model[MEM_EMBED_MODEL_SZ];
  uint32_t min_chars;
  uint32_t batch_size;
  bool     kinds[3];                   // indexed by mem_msg_kind_t
  uint64_t scanned;
  uint64_t skipped;                    // rejected by the content gate
  uint64_t embedded;
  uint64_t failed;
  bool     in_flight;
  time_t   started;
  time_t   finished;
} memory_backfill_t;

// Guards every field of `bf`. Held only across field reads/writes, never
// across a DB query or a submit.
static pthread_mutex_t   backfill_mutex = PTHREAD_MUTEX_INITIALIZER;
static memory_backfill_t bf;

// One batch in transit. Heap-owned from fetch until the completion
// callback frees it; the texts are ours and must outlive the submit hop,
// because the singles-retry path re-submits them without re-reading the
// DB.
typedef struct
{
  size_t   n;
  int64_t  ids[MEM_EMBED_BATCH_MAX];
  char    *texts[MEM_EMBED_BATCH_MAX];
  char     model[MEM_EMBED_MODEL_SZ];
  bool     retry_singles;              // false once already a single
} memory_backfill_batch_t;

// File-local forward declarations. The submit → completion → retry →
// submit cycle is genuinely circular, so one of the three has to be
// declared ahead of its definition. They stay here rather than in
// memory.h on purpose: declaring a static in a header shared by four
// translation units is what produces irc.h's "declared 'static' but
// never defined" warnings, and this build is clean.
static void backfill_arm_pump(void);
static void backfill_embed_done(const llm_embed_response_t *resp);

static void
backfill_batch_free(memory_backfill_batch_t *b)
{
  for(size_t i = 0; i < b->n; i++)
    if(b->texts[i] != NULL)
      mem_free(b->texts[i]);

  mem_free(b);
}

// Render the eligible-kind list as a SQL IN() body. Mirrors the live
// switch in memory_log_message() exactly: EXCHANGE_IN always, the other
// two by KV. Never empty — EXCHANGE_IN is unconditional.
static void
backfill_kind_list(const bool *kinds, char *out, size_t out_sz)
{
  size_t pos = 0;

  out[0] = '\0';

  for(int k = 0; k < 3; k++)
  {
    int n;

    if(!kinds[k])
      continue;

    n = snprintf(out + pos, out_sz - pos, "%s%d", pos > 0 ? "," : "", k);

    if(n < 0 || (size_t)n >= out_sz - pos)
      return;

    pos += (size_t)n;
  }
}

// Read the next eligible rows past the high-water mark, applying the
// content gate in C so the live path and this one share one rule.
// Advances bf.hwm past every row examined, including skipped ones, so a
// rejected row is never re-read.
//
// returns: a batch with n >= 1, or NULL when the scan is exhausted.
static memory_backfill_batch_t *
backfill_fetch(void)
{
  memory_backfill_batch_t *b;
  char     sql[1024];
  char     kinds[16];
  uint32_t cap;
  uint32_t ns_id;
  uint32_t min_chars;

  pthread_mutex_lock(&backfill_mutex);
  ns_id     = bf.ns_id;
  cap       = bf.batch_size;
  min_chars = bf.min_chars;
  backfill_kind_list(bf.kinds, kinds, sizeof(kinds));
  pthread_mutex_unlock(&backfill_mutex);

  b = mem_alloc("memory", "backfill_batch", sizeof(*b));

  pthread_mutex_lock(&backfill_mutex);
  snprintf(b->model, sizeof(b->model), "%s", bf.model);
  pthread_mutex_unlock(&backfill_mutex);

  // A window of consecutive rejects advances the high-water mark without
  // filling a batch, so keep reading until a batch fills or a read comes
  // back genuinely empty. A chat corpus rejects ~40% of rows, and a run
  // of junk longer than one window is ordinary.
  for(;;)
  {
    db_result_t *res;
    int64_t      hwm;
    bool         rows_seen = false;

    pthread_mutex_lock(&backfill_mutex);
    hwm = bf.hwm;
    pthread_mutex_unlock(&backfill_mutex);

    // Scan wider than the batch: most rows in a chat corpus are short
    // and get rejected, so a LIMIT equal to the batch size would spend
    // one round trip per handful of keepers.
    snprintf(sql, sizeof(sql),
        "SELECT x.id, x.text"
        " FROM conversation_log x"
        " LEFT JOIN conversation_embeddings e ON e.msg_id = x.id"
        " WHERE e.msg_id IS NULL"
        "   AND x.ns_id = %u"
        "   AND x.id > %" PRId64
        "   AND x.kind IN (%s)"
        " ORDER BY x.id ASC LIMIT %u",
        ns_id, hwm, kinds, cap * 8);

    res = db_result_alloc();

    if(db_query(sql, res) != SUCCESS || !res->ok)
    {
      if(res->error[0] != '\0')
        clam(CLAM_WARN, "memory", "backfill scan: %s", res->error);

      db_result_free(res);
      backfill_batch_free(b);
      return(NULL);
    }

    for(uint32_t i = 0; i < res->rows && b->n < cap; i++)
    {
      const char *id_s   = db_result_get(res, i, 0);
      const char *text_s = db_result_get(res, i, 1);
      int64_t     id;

      if(id_s == NULL)
        continue;

      id = strtoll(id_s, NULL, 10);
      rows_seen = true;

      pthread_mutex_lock(&backfill_mutex);

      if(id > bf.hwm)
        bf.hwm = id;

      bf.scanned++;

      // The content gate — including its exclusion regex, which the
      // predicate reads from module state, so the backfill inherits it
      // with no signature change. It does NOT inherit the burst gate,
      // and that is intended, not an oversight: a backfill re-reads
      // history where the burst is long over and now() means nothing.
      if(text_s == NULL || !memory_text_is_embeddable(text_s, min_chars))
      {
        bf.skipped++;
        pthread_mutex_unlock(&backfill_mutex);
        continue;
      }

      pthread_mutex_unlock(&backfill_mutex);

      b->ids[b->n]   = id;
      b->texts[b->n] = mem_strdup("memory", "backfill_text", text_s);
      b->n++;
    }

    db_result_free(res);

    if(b->n > 0)
    {
      b->retry_singles = (b->n > 1);
      return(b);
    }

    // Nothing readable left at all — the scan is exhausted.
    if(!rows_seen)
    {
      backfill_batch_free(b);
      return(NULL);
    }
  }
}

// Mark the run finished and log the tally. Caller must not hold the mutex.
static void
backfill_finish(const char *why)
{
  uint64_t scanned, skipped, embedded, failed;
  long     secs;

  pthread_mutex_lock(&backfill_mutex);
  bf.running   = false;
  bf.stopping  = false;
  bf.in_flight = false;
  bf.finished  = time(NULL);
  scanned  = bf.scanned;
  skipped  = bf.skipped;
  embedded = bf.embedded;
  failed   = bf.failed;
  secs     = (long)(bf.finished - bf.started);
  pthread_mutex_unlock(&backfill_mutex);

  clam(CLAM_INFO, "memory",
      "backfill %s: scanned=%llu skipped=%llu embedded=%llu failed=%llu"
      " elapsed=%lds",
      why,
      (unsigned long long)scanned, (unsigned long long)skipped,
      (unsigned long long)embedded, (unsigned long long)failed,
      secs);
}

static bool
backfill_submit(memory_backfill_batch_t *b)
{
  const char *inputs[MEM_EMBED_BATCH_MAX];

  for(size_t i = 0; i < b->n; i++)
    inputs[i] = b->texts[i];

  // FAIL here means the callback was NOT invoked and the batch is still
  // ours to free — the convention memory_submit_embed() already relies
  // on. Guessing the other way double-frees; see
  // finding_async_fail_two_conventions.
  return(llm_embed_submit(b->model, inputs, b->n,
      backfill_embed_done, b));
}

// Re-submit each text of a failed multi-text batch on its own, so one
// bad row cannot cost the other 31. Consumes `b`.
static void
backfill_retry_singles(memory_backfill_batch_t *b)
{
  size_t retried = 0;

  for(size_t i = 0; i < b->n; i++)
  {
    memory_backfill_batch_t *s;

    if(b->texts[i] == NULL)
      continue;

    s = mem_alloc("memory", "backfill_batch", sizeof(*s));
    s->n            = 1;
    s->ids[0]       = b->ids[i];
    s->texts[0]     = b->texts[i];
    s->retry_singles = false;
    snprintf(s->model, sizeof(s->model), "%s", b->model);

    // Ownership of this text moved into `s`; clear it so the parent's
    // free does not touch it.
    b->texts[i] = NULL;

    if(backfill_submit(s) == SUCCESS)
    {
      retried++;
      continue;
    }

    pthread_mutex_lock(&backfill_mutex);
    bf.failed++;
    pthread_mutex_unlock(&backfill_mutex);
    backfill_batch_free(s);
  }

  backfill_batch_free(b);

  clam(CLAM_DEBUG, "memory", "backfill retrying %zu row(s) singly", retried);

  // Each single carries its own completion, and each will arm the pump.
  // Arming here too would fan the run out; the singles do it instead.
  if(retried == 0)
    backfill_arm_pump();
}

static void
backfill_embed_done(const llm_embed_response_t *resp)
{
  memory_backfill_batch_t *b = resp->user_data;
  bool stopping;

  if(!resp->ok || resp->dim == 0)
  {
    clam(CLAM_WARN, "memory", "backfill embed failed: %s",
        resp->error ? resp->error : "(no detail)");

    if(b->retry_singles)
    {
      backfill_retry_singles(b);
      return;
    }

    pthread_mutex_lock(&backfill_mutex);
    bf.failed += b->n;
    pthread_mutex_unlock(&backfill_mutex);
    backfill_batch_free(b);
    backfill_arm_pump();
    return;
  }

  for(size_t i = 0; i < b->n && i < resp->n_vectors; i++)
  {
    bool ok = memory_write_embedding("conversation_embeddings", "msg_id",
        b->ids[i], b->model, resp->dim, resp->vectors[i]);

    pthread_mutex_lock(&backfill_mutex);

    if(ok == SUCCESS)
      bf.embedded++;

    else
      bf.failed++;

    pthread_mutex_unlock(&backfill_mutex);
  }

  // A short vector count is the provider dropping inputs silently.
  if(resp->n_vectors < b->n)
  {
    pthread_mutex_lock(&backfill_mutex);
    bf.failed += b->n - resp->n_vectors;
    pthread_mutex_unlock(&backfill_mutex);

    clam(CLAM_WARN, "memory",
        "backfill: asked for %zu vectors, got %zu",
        b->n, resp->n_vectors);
  }

  backfill_batch_free(b);

  pthread_mutex_lock(&backfill_mutex);
  stopping = bf.stopping;
  pthread_mutex_unlock(&backfill_mutex);

  if(stopping)
  {
    backfill_finish("stopped");
    return;
  }

  backfill_arm_pump();
}

// One pump step: fetch, submit, end. The next step is armed by the
// completion callback, so exactly one batch is ever outstanding.
static void
backfill_pump_cb(task_t *t)
{
  memory_backfill_batch_t *b;
  bool stopping;

  t->state = TASK_ENDED;

  pthread_mutex_lock(&backfill_mutex);
  stopping     = bf.stopping;
  bf.in_flight = false;
  pthread_mutex_unlock(&backfill_mutex);

  if(stopping)
  {
    backfill_finish("stopped");
    return;
  }

  b = backfill_fetch();

  if(b == NULL)
  {
    backfill_finish("complete");
    return;
  }

  pthread_mutex_lock(&backfill_mutex);
  bf.in_flight = true;
  pthread_mutex_unlock(&backfill_mutex);

  if(backfill_submit(b) == SUCCESS)
    return;

  // Submit refused outright: the callback will not fire, so the batch is
  // ours to account for and free, and the pump must re-arm itself.
  clam(CLAM_WARN, "memory", "backfill submit refused for %zu row(s)", b->n);

  pthread_mutex_lock(&backfill_mutex);
  bf.failed += b->n;
  bf.in_flight = false;
  pthread_mutex_unlock(&backfill_mutex);

  backfill_batch_free(b);
  backfill_arm_pump();
}

static void
backfill_arm_pump(void)
{
  pthread_mutex_lock(&backfill_mutex);

  if(!bf.running || bf.stopping)
  {
    pthread_mutex_unlock(&backfill_mutex);
    backfill_finish(bf.stopping ? "stopped" : "complete");
    return;
  }

  bf.in_flight = true;
  pthread_mutex_unlock(&backfill_mutex);

  task_add("memory.backfill", TASK_THREAD, 120, backfill_pump_cb, NULL);
}

bool
memory_backfill_start(uint32_t ns_id, char *err, size_t err_sz)
{
  mem_cfg_t cfg;

  memory_cfg_snapshot(&cfg);

  if(!memory_ready || !cfg.enabled)
  {
    snprintf(err, err_sz, "memory subsystem is not enabled");
    return(FAIL);
  }

  if(cfg.embed_model[0] == '\0')
  {
    snprintf(err, err_sz, "memory.embed_model is unset — nothing to embed with");
    return(FAIL);
  }

  pthread_mutex_lock(&backfill_mutex);

  if(bf.running)
  {
    pthread_mutex_unlock(&backfill_mutex);
    snprintf(err, err_sz, "a backfill is already running");
    return(FAIL);
  }

  memset(&bf, 0, sizeof(bf));
  bf.running    = true;
  bf.ns_id      = ns_id;
  bf.min_chars  = cfg.embed_min_chars;
  bf.batch_size = cfg.embed_batch_size;
  bf.started    = time(NULL);
  snprintf(bf.model, sizeof(bf.model), "%s", cfg.embed_model);

  // Mirror the live eligibility switch, so a backfill embeds exactly
  // what a fresh message of the same kind would embed today.
  bf.kinds[MEM_MSG_WITNESS]      = cfg.witness_embeds;
  bf.kinds[MEM_MSG_EXCHANGE_IN]  = true;
  bf.kinds[MEM_MSG_EXCHANGE_OUT] = cfg.embed_own_replies;

  pthread_mutex_unlock(&backfill_mutex);

  clam(CLAM_INFO, "memory",
      "backfill starting: ns=%u model=%s min_chars=%u batch=%u"
      " kinds=[witness=%s exchange_in=yes exchange_out=%s]",
      ns_id, cfg.embed_model, cfg.embed_min_chars, cfg.embed_batch_size,
      cfg.witness_embeds ? "yes" : "no",
      cfg.embed_own_replies ? "yes" : "no");

  backfill_arm_pump();
  return(SUCCESS);
}

void
memory_backfill_stop(void)
{
  pthread_mutex_lock(&backfill_mutex);

  if(!bf.running)
  {
    pthread_mutex_unlock(&backfill_mutex);
    return;
  }

  bf.stopping = true;
  pthread_mutex_unlock(&backfill_mutex);

  clam(CLAM_INFO, "memory", "backfill stop requested");
}

void
memory_backfill_status(char *out, size_t out_sz)
{
  memory_backfill_t s;

  pthread_mutex_lock(&backfill_mutex);
  s = bf;
  pthread_mutex_unlock(&backfill_mutex);

  if(!s.running && s.started == 0)
  {
    snprintf(out, out_sz, "  backfill:    never run");
    return;
  }

  snprintf(out, out_sz,
      "  backfill:    %s scanned=%llu skipped=%llu embedded=%llu failed=%llu",
      s.running ? (s.stopping ? "stopping" : "running") : "done",
      (unsigned long long)s.scanned, (unsigned long long)s.skipped,
      (unsigned long long)s.embedded, (unsigned long long)s.failed);
}

// /bot <name> embedbackfill

static void
memory_verb_embedbackfill(const cmd_ctx_t *ctx)
{
  bot_inst_t *bot = ctx->bot;
  userns_t   *ns  = bot_get_userns(bot);
  char        err[160];
  char        buf[256];

  if(ns == NULL)
  {
    cmd_reply(ctx, "bot has no userns bound");
    return;
  }

  if(memory_backfill_start(ns->id, err, sizeof(err)) != SUCCESS)
  {
    cmd_reply(ctx, err);
    return;
  }

  snprintf(buf, sizeof(buf),
      "backfill started over namespace %u — it runs asynchronously;"
      " watch /show memstore for progress", ns->id);
  cmd_reply(ctx, buf);
}

// Kind-agnostic (kind_filter NULL): a kind_filter names method kinds,
// and embedding the conversation log is the mind's work, not any one
// method's.
bool
memory_backfill_cmd_register(void)
{
  if(cmd_register("memory", "embedbackfill",
        "bot <name> embedbackfill",
        "Embed conversation-log rows the live path never got to",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        memory_verb_embedbackfill, NULL, "bot", "ebf",
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
