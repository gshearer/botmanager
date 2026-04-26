// botmanager — MIT
// Acquisition engine: reactive pipeline (SXNG → curl → digest → ingest),
// proactive dispatcher, per-bot tick + kick-drain, digest helper.

#include "acquire_priv.h"
#include "knowledge_priv.h"
#include "llm_priv.h"

#include "cmd.h"
#include "curl.h"
#include "db.h"
#include "method.h"
#include "plugin.h"
#include "userns.h"
#include "util.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// SXNG hookup — the searxng plugin is dlopen'd with RTLD_LOCAL so we
// cannot link against sxng_search directly. Resolve it on first use
// via plugin_dlsym and cache the function pointer; a missing plugin
// turns every reactive job into a WARN-and-skip.
//
// Pull in the searxng types (sxng_result_t, sxng_response_t,
// sxng_done_cb_t, sxng_category_t) but skip both the real prototype
// and the abort-on-miss shim — we want WARN-and-skip semantics, so
// the function pointer is materialised below via our own dlsym path.
#define SEARXNG_TYPES_ONLY
#include "searxng_api.h"

typedef bool (*acq_sxng_search_fn_t)(const char *query,
    sxng_category_t category, size_t n_wanted,
    sxng_done_cb_t cb, void *user_data);

// Cached sxng_search pointer. Resolved on first use via plugin_dlsym;
// NULL means the plugin wasn't loaded (or isn't yet). Guarded by
// acquire_cfg_mutex — the write is rare (once per process) and the
// reads are nearly free.
static acq_sxng_search_fn_t  acquire_sxng_search = NULL;

// A6R — on-demand reactive drain delay. One second is long enough for
// a burst of matches on the same topic to coalesce into a single drain
// task, short enough that a lone chat mention produces a query within
// the same tab switch.
#define ACQUIRE_KICK_DELAY_MS  1000

// Forward declaration for the per-bot kick-drain task callback
// (referenced by acq_bot_kick_submit_deferred before its definition).
static void acq_bot_kick_drain(task_t *t);

// Acquisition job context — heap-owned, carried through every async hop.
// Freed exactly once at the terminal leaf (or early abort). A7 extended
// this context from reactive-only to shared use: both the reactive drain
// and the proactive picker emit the same ctx shape and feed the same
// SXNG → fetch → digest → ingest chain. The `is_proactive` flag steers
// only log-line wording and the stats-bump path.

typedef struct
{
  char  bot_name   [ACQUIRE_BOT_NAME_SZ];
  char  topic_name [ACQUIRE_TOPIC_NAME_SZ];
  char  subject    [ACQUIRE_SUBJECT_SZ];
  char  dest_corpus[ACQUIRE_CORPUS_NAME_SZ];

  // Snapshotted from the topic so async callbacks don't need to
  // re-consult the live registry (avoids re-taking acquire_entries_lock
  // from the worker thread, and the topic list may have been replaced).
  char            keywords_csv[ACQUIRE_KEYWORD_SZ * ACQUIRE_KEYWORDS_MAX + ACQUIRE_KEYWORDS_MAX];
  char            query[ACQUIRE_TOPIC_QUERY_SZ];
  sxng_category_t category;

  // Per-topic override for max_sources_per_query, snapshotted from the
  // topic at ctx-build time. 0 = inherit acquire_cfg.max_sources_per_query.
  // Nonzero values are still clamped to the global cap so a personality
  // cannot exceed the site-wide safety ceiling.
  uint32_t topic_max_sources;

  // Number of sources we'll fetch for this job. Decremented once per
  // curl callback — the LAST one frees the ctx.
  uint32_t pending_sources;
  pthread_mutex_t lock;

  // Count of chunks successfully inserted. Used to bump
  // acquire_topic_stats.total_ingested in one UPSERT at termination.
  uint32_t n_inserted;

  // Set by the proactive picker; false for the reactive drain. Controls
  // stats UPSERT column (last_proactive vs last_reactive) and the
  // wording emitted by async pipeline log lines.
  bool is_proactive;
} reactive_job_ctx_t;

// One-liner used in the async pipeline's log strings so a single
// `acq_reactive_*` callback can narrate either path without branching.
static inline const char *
acq_ctx_mode(const reactive_job_ctx_t *ctx)
{
  return(ctx->is_proactive ? "proactive" : "reactive");
}

// Build "kw1, kw2, kw3" from a topic's keyword array.
static void
acq_build_keywords_csv(const acquire_topic_t *t, char *out, size_t out_cap)
{
  size_t w;
  if(out_cap == 0) return;
  out[0] = '\0';
  w = 0;

  for(size_t i = 0; i < t->n_keywords; i++)
  {
    const char *kw = t->keywords[i];

    int n;
    if(kw[0] == '\0')
      continue;

    n = snprintf(out + w, out_cap - w,
        "%s%s", (w == 0) ? "" : ", ", kw);

    if(n < 0 || (size_t)n >= out_cap - w)
      break;

    w += (size_t)n;
  }
}

// Build the search query from topic.query, topic.query_template (with
// {subject} substitution), or the subject itself. The output buffer is
// always the same size as t->query / t->query_template, so the manual
// copy loop below is used instead of snprintf("%s %s", ...) — the
// compiler cannot prove the combined length fits and issues a
// -Wformat-truncation otherwise.
static void
acq_build_query(const acquire_topic_t *t, const char *subject,
    char *out, size_t out_cap)
{
  if(out_cap == 0) return;
  out[0] = '\0';

  if(t->query[0] != '\0')
  {
    snprintf(out, out_cap, "%s", t->query);
    return;
  }

  if(t->query_template[0] != '\0')
  {
    const char *ph = strstr(t->query_template, "{subject}");
    size_t w = 0;

    size_t tpl_len;
    size_t subj_len;
    if(ph != NULL)
    {
      // Copy template prefix, subject, template suffix manually so the
      // compiler can see each piece's bounded length.
      size_t prefix_len = (size_t)(ph - t->query_template);

      const char *suffix;
      size_t suffix_len;
      size_t subj_len;
      if(prefix_len > out_cap - 1) prefix_len = out_cap - 1;
      memcpy(out, t->query_template, prefix_len);
      w = prefix_len;

      suffix = ph + 9;
      subj_len = strlen(subject);

      if(w + subj_len >= out_cap) subj_len = out_cap - 1 - w;
      memcpy(out + w, subject, subj_len);
      w += subj_len;

      suffix_len = strlen(suffix);

      if(w + suffix_len >= out_cap) suffix_len = out_cap - 1 - w;
      memcpy(out + w, suffix, suffix_len);
      w += suffix_len;

      out[w] = '\0';
      return;
    }

    // No placeholder: template then space then subject.
    tpl_len = strlen(t->query_template);

    if(tpl_len > out_cap - 1) tpl_len = out_cap - 1;
    memcpy(out, t->query_template, tpl_len);
    w = tpl_len;

    if(w + 1 < out_cap)
      out[w++] = ' ';

    subj_len = strlen(subject);

    if(w + subj_len >= out_cap) subj_len = out_cap - 1 - w;
    memcpy(out + w, subject, subj_len);
    w += subj_len;

    out[w] = '\0';
    return;
  }

  snprintf(out, out_cap, "%s", subject);
}

// Best-effort upsert of per-(bot, topic) stats. Both reactive and
// proactive terminations call here; `is_proactive` swaps the timestamp
// column between `last_reactive` and `last_proactive`. `total_queries`
// and `total_ingested` are path-agnostic. Swallows DB errors with a
// WARN — the ingest already happened, bookkeeping is nice-to-have.
static void
acq_topic_stats_bump(const char *bot, const char *topic,
    uint32_t n_inserted, bool is_proactive)
{
  char *e_bot   = db_escape(bot);
  char *e_topic = db_escape(topic);

  const char *ts_col;
  db_result_t *res;
  char        sql[512];
  if(e_bot == NULL || e_topic == NULL)
  {
    if(e_bot)   mem_free(e_bot);
    if(e_topic) mem_free(e_topic);
    return;
  }

  ts_col = is_proactive ? "last_proactive" : "last_reactive";

  snprintf(sql, sizeof(sql),
      "INSERT INTO acquire_topic_stats"
      " (bot_name, topic_name, %s, total_queries, total_ingested)"
      " VALUES ('%s', '%s', NOW(), 1, %u)"
      " ON CONFLICT (bot_name, topic_name) DO UPDATE SET"
      "   %s             = NOW(),"
      "   total_queries  = acquire_topic_stats.total_queries + 1,"
      "   total_ingested = acquire_topic_stats.total_ingested + EXCLUDED.total_ingested",
      ts_col, e_bot, e_topic, n_inserted, ts_col);

  mem_free(e_bot);
  mem_free(e_topic);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
    clam(CLAM_WARN, ACQUIRE_CTX, "topic_stats upsert failed: %s",
        res != NULL ? res->error : "(null)");

  db_result_free(res);
}

// Terminal release: decrements pending_sources and, on zero, writes
// stats and frees the ctx. Safe to call from any thread.
static void
reactive_job_release_source(reactive_job_ctx_t *ctx)
{
  bool last;
  uint32_t n_inserted;
  pthread_mutex_lock(&ctx->lock);
  last = (--ctx->pending_sources == 0);
  n_inserted = ctx->n_inserted;
  pthread_mutex_unlock(&ctx->lock);

  if(!last)
    return;

  // Proactive runs still UPSERT when n_inserted==0 — zero-yield runs
  // are useful feedback for the operator tuning weights/queries, and
  // total_queries is the primary "we tried" counter.
  if(n_inserted > 0 || ctx->is_proactive)
    acq_topic_stats_bump(ctx->bot_name, ctx->topic_name, n_inserted,
        ctx->is_proactive);

  clam(CLAM_DEBUG, ACQUIRE_CTX,
      "%s done bot=%s topic=%s subject='%s' inserted=%u",
      acq_ctx_mode(ctx), ctx->bot_name, ctx->topic_name, ctx->subject,
      n_inserted);

  pthread_mutex_destroy(&ctx->lock);
  mem_free(ctx);
}

// Per-source ctx (I1).
//
// The shared reactive_job_ctx_t fans N parallel curl fetches from one
// SXNG query via pending_sources. Image extraction needs per-source
// state (the page URL plus the harvested image array) that must
// survive from the curl callback (where the raw HTML is still valid)
// into the digest callback (where the chunk id lands). We wrap the
// shared parent with a small per-source struct that owns exactly that
// state and proxies the parent's release on teardown.
//
// The source ctx is allocated in acq_reactive_sxng_done right before
// curl_get, carried as curl's user_data, and handed through as
// digest's user_data. acq_source_release frees the source ctx and
// propagates a single reactive_job_release_source call to the parent,
// so the refcount accounting at the reactive_job_ctx_t layer is
// preserved exactly.

typedef struct
{
  reactive_job_ctx_t  *parent;
  char                 page_url[KNOWLEDGE_IMAGE_URL_SZ];
  acq_image_extract_t *images;      // NULL when extraction disabled / empty
  size_t               n_images;
} acq_source_ctx_t;

static void
acq_source_release(acq_source_ctx_t *src)
{
  if(src == NULL)
    return;

  if(src->parent != NULL)
    reactive_job_release_source(src->parent);

  if(src->images != NULL)
    mem_free(src->images);

  mem_free(src);
}

// Forward declarations for the async pipeline.
static void acq_reactive_start_search(reactive_job_ctx_t *ctx);
static void acq_reactive_sxng_done(const sxng_response_t *resp);
static void acq_reactive_curl_done(const curl_response_t *cresp);
static void acq_reactive_digest_done(
    const acquire_digest_response_t *resp);

// Resolve (or re-resolve) the sxng_search function pointer via the
// plugin subsystem. Returns the cached pointer.
static acq_sxng_search_fn_t
acq_sxng_resolve(void)
{
  acq_sxng_search_fn_t fn;
  union
  {
    void                 *obj;
    acq_sxng_search_fn_t  fn;
  } u;
  pthread_mutex_lock(&acquire_cfg_mutex);
  fn = acquire_sxng_search;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  if(fn != NULL)
    return(fn);

  // POSIX permits round-tripping a function pointer through a void * via
  // dlsym; ISO C is stricter. The union-cast below silences -Wpedantic
  // while keeping the conversion localised and explicit.

  u.obj = plugin_dlsym("searxng", "sxng_search");
  fn = u.fn;

  pthread_mutex_lock(&acquire_cfg_mutex);
  acquire_sxng_search = fn;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  return(fn);
}

static void
acq_reactive_start_search(reactive_job_ctx_t *ctx)
{
  acq_sxng_search_fn_t sxng = acq_sxng_resolve();

  uint32_t n_wanted;
  if(sxng == NULL)
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "%s: searxng plugin not available; dropping job"
        " bot=%s topic=%s subject='%s'",
        acq_ctx_mode(ctx), ctx->bot_name, ctx->topic_name, ctx->subject);
    pthread_mutex_destroy(&ctx->lock);
    mem_free(ctx);
    return;
  }

  pthread_mutex_lock(&acquire_cfg_mutex);
  n_wanted = acquire_cfg.max_sources_per_query;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  // Per-topic override wins when nonzero; lets a personality tune
  // individual topics without having to raise the global default (which
  // would amplify traffic for every other topic on every bot). The real
  // upper bound is plugin.searxng.max_results (SXNG-side clamp).
  if(ctx->topic_max_sources > 0)
    n_wanted = ctx->topic_max_sources;

  pthread_mutex_lock(&acquire_stat_mutex);
  acquire_stats.total_queries++;
  pthread_mutex_unlock(&acquire_stat_mutex);

  clam(CLAM_DEBUG, ACQUIRE_CTX,
      "%s search bot=%s topic=%s subject='%s' query='%s'"
      " category=%s n=%u",
      acq_ctx_mode(ctx), ctx->bot_name, ctx->topic_name, ctx->subject,
      ctx->query, sxng_category_name(ctx->category), n_wanted);

  if(sxng(ctx->query, ctx->category, n_wanted,
      acq_reactive_sxng_done, ctx) != SUCCESS)
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "%s: sxng_search failed to submit bot=%s topic=%s",
        acq_ctx_mode(ctx), ctx->bot_name, ctx->topic_name);
    pthread_mutex_destroy(&ctx->lock);
    mem_free(ctx);
  }
}

// curl_get callback: extract images off the raw HTML, strip + digest,
// release the per-source ctx on error. The per-source ctx wraps the
// shared reactive_job_ctx_t; image state must be stashed here because
// the raw `cresp->body` is freed before the digest callback fires.
static void
acq_reactive_curl_done(const curl_response_t *cresp)
{
  acq_source_ctx_t   *src = (acq_source_ctx_t *)cresp->user_data;
  reactive_job_ctx_t *ctx = src->parent;

  bool     imgs_on;
  char *stripped;
  uint32_t img_cap;
  size_t strip_len;
  if(cresp->curl_code != 0 || cresp->status != 200
      || cresp->body == NULL || cresp->body_len == 0)
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "%s fetch failed bot=%s topic=%s http=%ld err='%s'",
        acq_ctx_mode(ctx), ctx->bot_name, ctx->topic_name, cresp->status,
        cresp->error != NULL ? cresp->error : "(transport)");
    acq_source_release(src);
    return;
  }

  // I1 — harvest images off the raw body BEFORE strip. Allocate the
  // slot array lazily to keep the no-images path allocation-free.
  pthread_mutex_lock(&acquire_cfg_mutex);
  imgs_on = acquire_cfg.images_enabled;
  img_cap = acquire_cfg.image_max_per_page;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  if(imgs_on && img_cap > 0)
  {
    size_t cap = img_cap;

    if(cap > ACQUIRE_IMAGE_MAX_PER_PAGE_CEIL)
      cap = ACQUIRE_IMAGE_MAX_PER_PAGE_CEIL;

    src->images = mem_alloc(ACQUIRE_CTX, "extracted_images",
        sizeof(*src->images) * cap);

    if(src->images != NULL)
    {
      src->n_images = acq_extract_images(cresp->body, cresp->body_len,
          src->page_url, src->images, cap);

      if(src->n_images == 0)
      {
        mem_free(src->images);
        src->images = NULL;
      }
    }
  }

  // Strip to a fresh heap buffer sized to the plain-text cap.
  stripped = mem_alloc(ACQUIRE_CTX, "reactive_strip",
      ACQUIRE_REACTIVE_STRIPPED_MAX);

  strip_len = acq_strip_html(cresp->body, cresp->body_len,
      stripped, ACQUIRE_REACTIVE_STRIPPED_MAX);

  if(strip_len == 0)
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "%s strip yielded empty body bot=%s topic=%s",
        acq_ctx_mode(ctx), ctx->bot_name, ctx->topic_name);
    mem_free(stripped);
    acq_source_release(src);
    return;
  }

  if(acquire_digest_submit(ctx->topic_name, ctx->keywords_csv,
      stripped, strip_len, acq_reactive_digest_done, src) != SUCCESS)
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "%s digest submit failed bot=%s topic=%s",
        acq_ctx_mode(ctx), ctx->bot_name, ctx->topic_name);
    mem_free(stripped);
    acq_source_release(src);
    return;
  }

  // acquire_digest_submit copies its body internally; free the
  // stripped buffer here.
  mem_free(stripped);
}

// Shared ingest seam. Callers: the SXNG reactive/proactive chain
// (acq_reactive_digest_done) and the personality-declared feed chain
// (acq_feed_digest_done). Both land here after relevance + dest_corpus
// checks pass. Keeps knowledge_insert_chunk + image attach + ingest
// callback dispatch in one place.
//
// `images` may be NULL (feed path has no per-item image extraction in
// the first cut). `page_url` feeds into knowledge_insert_image as the
// page context for each image; ignored when n_images == 0.
//
// Returns SUCCESS if the chunk landed, FAIL otherwise. The reactive
// caller uses that signal to bump its per-ctx ingest count only when
// the insert actually took effect (preserves the pre-seam invariant).
bool
acq_ingest_digest_result(const char *bot_name, const char *topic_name,
    const char *subject, const char *dest_corpus, bool is_proactive,
    const acquire_digest_response_t *resp,
    const acq_image_extract_t *images, size_t n_images,
    const char *page_url)
{
  const char *mode;
  char        section[KNOWLEDGE_SECTION_SZ];
  int64_t     id;

  mode = is_proactive ? "proactive" : "reactive";

  // Section heading carries the subject so retrieval surfaces it.
  snprintf(section, sizeof(section), "%s: %s",
      topic_name, subject);

  id = 0;

  if(knowledge_insert_chunk(dest_corpus, NULL, section,
      resp->summary, &id) != SUCCESS)
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "%s: knowledge_insert_chunk failed corpus='%s' bot=%s topic=%s",
        mode, dest_corpus, bot_name, topic_name);
    return(FAIL);
  }

  {
    acquire_ingest_cb_t cb;
    void               *user;

    pthread_mutex_lock(&acquire_stat_mutex);
    acquire_stats.total_ingested++;
    pthread_mutex_unlock(&acquire_stat_mutex);

    clam(CLAM_INFO, ACQUIRE_CTX,
        "%s ingested bot=%s topic=%s subject='%s' chunk=%ld relevance=%u",
        mode, bot_name, topic_name, subject,
        (long)id, resp->relevance);

    // I1 — attach harvested images to the freshly-inserted chunk. Each
    // failure is logged but non-fatal: the chunk landed, which is the
    // primary objective. `subject` is the resolved subject (person /
    // thing / topic), not the keyword CSV.
    for(size_t i = 0; i < n_images; i++)
    {
      const acq_image_extract_t *img = &images[i];

      if(knowledge_insert_image(id, img->url,
            page_url != NULL ? page_url : "",
            img->caption, subject,
            img->width_px, img->height_px) != SUCCESS)
        clam(CLAM_DEBUG, ACQUIRE_CTX,
            "image insert failed chunk=%ld url='%s'",
            (long)id, img->url);
    }

    // V1 — notify the post-ingest consumer (chatbot volunteer module)
    // after the chunk + images have landed. Snapshot the callback
    // under its mutex so register/unregister can't race with fire;
    // invoke outside the lock so a slow consumer can't block acquire.
    pthread_mutex_lock(&acquire_ingest_cb_mutex);
    cb = acquire_ingest_cb;
    user = acquire_ingest_user;
    pthread_mutex_unlock(&acquire_ingest_cb_mutex);

    if(cb != NULL)
      cb(bot_name, topic_name, subject,
          dest_corpus, id, resp->relevance,
          is_proactive
              ? ACQUIRE_INGEST_PROACTIVE
              : ACQUIRE_INGEST_REACTIVE,
          user);
  }

  return(SUCCESS);
}

static void
acq_reactive_digest_done(const acquire_digest_response_t *resp)
{
  acq_source_ctx_t   *src = (acq_source_ctx_t *)resp->user_data;
  reactive_job_ctx_t *ctx = src->parent;

  uint32_t threshold;
  if(!resp->ok || resp->summary == NULL || resp->summary[0] == '\0')
  {
    acq_source_release(src);
    return;
  }

  pthread_mutex_lock(&acquire_cfg_mutex);
  threshold = acquire_cfg.relevance_threshold;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  if(resp->relevance < threshold)
  {
    clam(CLAM_DEBUG, ACQUIRE_CTX,
        "%s relevance=%u below threshold=%u bot=%s topic=%s",
        acq_ctx_mode(ctx), resp->relevance, threshold, ctx->bot_name,
        ctx->topic_name);
    acq_source_release(src);
    return;
  }

  if(ctx->dest_corpus[0] == '\0')
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "%s: no dest_corpus for bot=%s; dropping ingest",
        acq_ctx_mode(ctx), ctx->bot_name);
    acq_source_release(src);
    return;
  }

  if(acq_ingest_digest_result(ctx->bot_name, ctx->topic_name, ctx->subject,
      ctx->dest_corpus, ctx->is_proactive, resp,
      src->images, src->n_images, src->page_url) == SUCCESS)
  {
    pthread_mutex_lock(&ctx->lock);
    ctx->n_inserted++;
    pthread_mutex_unlock(&ctx->lock);
  }

  acq_source_release(src);
}

static void
acq_reactive_sxng_done(const sxng_response_t *resp)
{
  reactive_job_ctx_t *ctx = (reactive_job_ctx_t *)resp->user_data;

  uint32_t cap;
  size_t take;
  size_t submitted;
  if(!resp->ok || resp->n_results == 0)
  {
    clam(CLAM_DEBUG, ACQUIRE_CTX,
        "%s search empty bot=%s topic=%s subject='%s' err='%s'",
        acq_ctx_mode(ctx), ctx->bot_name, ctx->topic_name, ctx->subject,
        resp->error != NULL ? resp->error : "(no-results)");

    // No per-result callbacks will fire; free directly.
    pthread_mutex_destroy(&ctx->lock);
    mem_free(ctx);
    return;
  }

  pthread_mutex_lock(&acquire_cfg_mutex);
  cap = acquire_cfg.max_sources_per_query;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  // Mirror the per-topic override applied at search submit time; keeps
  // the post-result walk in sync with the requested result count.
  if(ctx->topic_max_sources > 0)
    cap = ctx->topic_max_sources;

  take = resp->n_results < cap ? resp->n_results : cap;

  if(take == 0)
  {
    pthread_mutex_destroy(&ctx->lock);
    mem_free(ctx);
    return;
  }

  // Reserve a refcount per source BEFORE kicking off any curl_get —
  // otherwise an early failure could drop the count to zero and free
  // the ctx before we've queued the rest.
  pthread_mutex_lock(&ctx->lock);
  ctx->pending_sources = (uint32_t)take;
  pthread_mutex_unlock(&ctx->lock);

  submitted = 0;

  for(size_t i = 0; i < take; i++)
  {
    const sxng_result_t *r = &resp->results[i];

    acq_source_ctx_t *src;
    if(r->url[0] == '\0')
    {
      // Release this slot's refcount and continue.
      reactive_job_release_source(ctx);
      continue;
    }

    // I1 — wrap the parent ctx in a per-source ctx so curl_done and
    // digest_done can carry image-extractor state without touching
    // the shared parent's layout.
    src = mem_alloc(ACQUIRE_CTX, "reactive_source",
        sizeof(*src));

    if(src == NULL)
    {
      clam(CLAM_WARN, ACQUIRE_CTX,
          "%s: source ctx alloc failed url='%s'",
          acq_ctx_mode(ctx), r->url);
      reactive_job_release_source(ctx);
      continue;
    }

    memset(src, 0, sizeof(*src));
    src->parent = ctx;

    // sxng url buffers (2048) are larger than our page_url slot (1024);
    // bound-copy through a local so -Wformat-truncation stays silent.
    {
      size_t ulen = strnlen(r->url, sizeof(src->page_url) - 1);

      memcpy(src->page_url, r->url, ulen);
      src->page_url[ulen] = '\0';
    }

    if(curl_get(r->url, acq_reactive_curl_done, src) != SUCCESS)
    {
      clam(CLAM_WARN, ACQUIRE_CTX,
          "%s: curl_get failed url='%s'", acq_ctx_mode(ctx), r->url);
      mem_free(src);
      reactive_job_release_source(ctx);
      continue;
    }

    submitted++;
  }

  if(submitted == 0)
    // All submits failed: release_source calls above already ran,
    // and the last one freed the ctx. Nothing to do here.
    return;
}

// Allocate + populate a job ctx from a dequeued job. Takes a read-
// locked snapshot of the topic; caller holds acquire_entries_lock.
static reactive_job_ctx_t *
acq_reactive_build_ctx(const acquire_bot_entry_t *e,
    const acquire_topic_t *t, const char *subject)
{
  reactive_job_ctx_t *ctx = mem_alloc(ACQUIRE_CTX, "reactive_ctx",
      sizeof(*ctx));

  memset(ctx, 0, sizeof(*ctx));
  pthread_mutex_init(&ctx->lock, NULL);

  snprintf(ctx->bot_name,    sizeof(ctx->bot_name),    "%s", e->name);
  snprintf(ctx->topic_name,  sizeof(ctx->topic_name),  "%s", t->name);
  snprintf(ctx->subject,     sizeof(ctx->subject),     "%s", subject);
  snprintf(ctx->dest_corpus, sizeof(ctx->dest_corpus), "%s", e->dest_corpus);

  acq_build_keywords_csv(t, ctx->keywords_csv, sizeof(ctx->keywords_csv));
  acq_build_query(t, subject, ctx->query, sizeof(ctx->query));
  ctx->category = sxng_category_from_name(t->category);
  ctx->topic_max_sources = t->max_sources;

  return(ctx);
}

// Process exactly one dequeued reactive job. Runs on the tick thread.
// All downstream work is async; this function returns promptly.
static void
acq_reactive_process_one(acquire_bot_entry_t *e,
    const acquire_reactive_job_t *job)
{
  const acquire_topic_t *t_ref;
  uint32_t max_per_hour;
  reactive_job_ctx_t    *ctx;
  pthread_mutex_lock(&acquire_stat_mutex);
  acquire_stats.total_reactive_drained++;
  pthread_mutex_unlock(&acquire_stat_mutex);

  // Snapshot everything we need from the topic registry under a read
  // lock; once we kick the async chain we release the lock cleanly.
  ctx = NULL;

  pthread_rwlock_rdlock(&acquire_entries_lock);

  t_ref = acquire_find_topic(e, job->topic_name);

  if(t_ref == NULL)
  {
    pthread_rwlock_unlock(&acquire_entries_lock);
    clam(CLAM_DEBUG, ACQUIRE_CTX,
        "reactive: topic '%s' vanished before tick bot=%s — dropping",
        job->topic_name, e->name);
    return;
  }

  // Rate-limit before building the heavy ctx.
  pthread_mutex_lock(&acquire_cfg_mutex);
  max_per_hour = acquire_cfg.max_reactive_per_hour;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  if(acquire_rate_exceeded(e, t_ref->name, max_per_hour))
  {
    pthread_rwlock_unlock(&acquire_entries_lock);

    pthread_mutex_lock(&acquire_stat_mutex);
    acquire_stats.total_reactive_rate_drops++;
    pthread_mutex_unlock(&acquire_stat_mutex);

    clam(CLAM_DEBUG, ACQUIRE_CTX,
        "reactive rate-limited bot=%s topic=%s (max=%u/hour) — dropping",
        e->name, job->topic_name, max_per_hour);
    return;
  }

  ctx = acq_reactive_build_ctx(e, t_ref, job->subject);

  pthread_rwlock_unlock(&acquire_entries_lock);

  acq_reactive_start_search(ctx);
}

// Proactive path (A7) — picks one topic per tick by weighted-random
// selection over `topic->proactive_weight`, then dispatches through the
// same SXNG → fetch → digest → ingest chain as reactive. Weight=0
// topics never fire. Topics without a concrete `query` string are
// skipped (proactive runs use `topic->query` verbatim — no `{subject}`
// substitution, no celeb gate).

// Weighted-random topic pick. Returns NULL if the bot has no topics,
// or if every topic has proactive_weight==0 (e.g. the bot is
// reactive-only). Caller holds acquire_entries_lock (rdlock or wrlock).
static const acquire_topic_t *
acq_proactive_pick_topic(const acquire_bot_entry_t *e, size_t *out_idx)
{
  uint32_t total;
  uint32_t roll;
  uint32_t acc;
  if(e->n_topics == 0)
    return(NULL);

  // Sum weights. Overflow is unreachable: n_topics is personality-capped
  // and weights are bounded 0..100, so the sum fits comfortably in
  // uint32_t (and even in int for util_rand).
  total = 0;

  for(size_t i = 0; i < e->n_topics; i++)
    total += e->topics[i].proactive_weight;

  if(total == 0)
    return(NULL);

  // util_rand(upper) returns [0, upper); treat the roll as a position
  // in the cumulative weight distribution.
  roll = (uint32_t)util_rand((int)total);
  acc = 0;

  for(size_t i = 0; i < e->n_topics; i++)
  {
    uint32_t w = e->topics[i].proactive_weight;

    if(w == 0)
      continue;

    acc += w;

    if(roll < acc)
    {
      if(out_idx != NULL)
        *out_idx = i;

      return(&e->topics[i]);
    }
  }

  // Unreachable unless every weight dropped to 0 between the sum and
  // the walk — treat as "skip".
  return(NULL);
}

// Select the query text for a proactive run. Returns a pointer into
// the topic (either `query` or `upcoming_query`) — both are fixed-size
// char arrays owned by the topic copy, so the returned pointer is
// valid under the same lock that protects the topic struct. Returns
// NULL if no concrete query is available (caller skips the run).
//
// The 1/10 upcoming-query cadence fires when the topic's running
// proactive counter is at its ninth increment (mod 10) AND the topic
// defines an upcoming_query. The counter is incremented by the caller
// iff this function returns non-NULL (i.e. we actually fire).
static const char *
acq_proactive_select_query(const acquire_topic_t *t, uint32_t counter)
{
  if((counter % 10) == 9 && t->upcoming_query[0] != '\0')
    return(t->upcoming_query);

  if(t->query[0] != '\0')
    return(t->query);

  return(NULL);
}

// Fire one proactive query for an already-resolved topic. Caller holds
// acquire_entries_lock (rdlock); this function releases the lock
// unconditionally before returning.
//
// Shared between the tick dispatcher (which picks the topic via weighted
// random) and /acquire trigger (which looks the topic up by name). The
// behavior from rate-limit through async dispatch is byte-identical
// across both entry points.
//
// If out_query is non-NULL and the dispatch succeeds or is declined for
// NO_QUERY, the chosen (or attempted-but-empty) query text is copied
// into it — useful for the admin reply. `out_query_sz` is the buffer
// size; the result is always NUL-terminated.
acq_proactive_result_t
acq_proactive_fire_locked(acquire_bot_entry_t *e, size_t topic_idx,
    char *out_query, size_t out_query_sz)
{
  const acquire_topic_t *t = &e->topics[topic_idx];

  uint32_t max_per_hour;
  uint32_t counter;
  const char *q;
  reactive_job_ctx_t *ctx;
  bool is_upcoming;
  if(out_query != NULL && out_query_sz > 0)
    out_query[0] = '\0';

  // Rate-limit check shares the reactive limiter — a proactive fire
  // that exceeds the hourly cap is skipped just like a reactive one.
  pthread_mutex_lock(&acquire_cfg_mutex);
  max_per_hour = acquire_cfg.max_reactive_per_hour;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  if(acquire_rate_exceeded(e, t->name, max_per_hour))
  {
    pthread_rwlock_unlock(&acquire_entries_lock);
    clam(CLAM_DEBUG, ACQUIRE_CTX,
        "proactive rate-limited bot=%s topic=%s (max=%u/hour) — skipping",
        e->name, t->name, max_per_hour);
    return(ACQ_PROACTIVE_RATE_LIMITED);
  }

  // Counter lives on the entry, parallel to topics[]. It was allocated
  // alongside `e->topics` in acquire_register_topics and freed in
  // acquire_entry_free_topics. NULL only if n_topics==0, which the
  // caller has already ruled out.
  counter = 0;

  if(e->topic_proactive_counter != NULL)
    counter = e->topic_proactive_counter[topic_idx];

  q = acq_proactive_select_query(t, counter);

  if(q == NULL)
  {
    pthread_rwlock_unlock(&acquire_entries_lock);
    clam(CLAM_DEBUG, ACQUIRE_CTX,
        "proactive skip bot=%s topic=%s: no concrete query"
        " (topic uses a template; proactive runs need topic.query)",
        e->name, t->name);
    return(ACQ_PROACTIVE_NO_QUERY);
  }

  // Commit the counter bump now — we're definitely firing. Doing it
  // before the async chain keeps the 1/10 upcoming-query cadence
  // aligned with "queries attempted", not "queries that landed a
  // chunk" (ingest failures shouldn't desync the counter).
  if(e->topic_proactive_counter != NULL)
    e->topic_proactive_counter[topic_idx]++;

  // Build the ctx. Subject = topic name so the eventual chunk's
  // section_heading reads "<topic>: <topic>" — useful for eyeballing
  // a corpus and immediately seeing which proactive run produced
  // which chunk. Keywords CSV still comes off the topic so the
  // digester has the same lexical anchors as the reactive path.
  ctx = mem_alloc(ACQUIRE_CTX, "proactive_ctx",
      sizeof(*ctx));
  memset(ctx, 0, sizeof(*ctx));
  pthread_mutex_init(&ctx->lock, NULL);

  snprintf(ctx->bot_name,    sizeof(ctx->bot_name),    "%s", e->name);
  snprintf(ctx->topic_name,  sizeof(ctx->topic_name),  "%s", t->name);
  snprintf(ctx->subject,     sizeof(ctx->subject),     "%s", t->name);
  snprintf(ctx->dest_corpus, sizeof(ctx->dest_corpus), "%s", e->dest_corpus);
  snprintf(ctx->query,       sizeof(ctx->query),       "%s", q);

  acq_build_keywords_csv(t, ctx->keywords_csv, sizeof(ctx->keywords_csv));
  ctx->topic_max_sources = t->max_sources;

  ctx->is_proactive = true;

  if(out_query != NULL && out_query_sz > 0)
    snprintf(out_query, out_query_sz, "%s", q);

  is_upcoming = ((counter % 10) == 9 && t->upcoming_query[0] != '\0');

  pthread_rwlock_unlock(&acquire_entries_lock);

  clam(CLAM_DEBUG, ACQUIRE_CTX,
      "proactive fire bot=%s topic=%s query='%s' counter=%u%s",
      ctx->bot_name, ctx->topic_name, ctx->query, counter,
      is_upcoming ? " (upcoming)" : "");

  // Spec defers the celeb gate for proactive runs (no {subject}
  // substitution, so the query text is author-controlled). Straight
  // to search.
  acq_reactive_start_search(ctx);
  return(ACQ_PROACTIVE_OK);
}

// Per-tick proactive dispatcher. Runs inline on the tick thread;
// everything after acq_reactive_start_search is async. Thin wrapper
// around acq_proactive_fire_locked: pick a topic via weighted-random,
// then dispatch. /acquire trigger (A9) takes the name-lookup path
// but ends at the same helper.
static void
acq_proactive_process(acquire_bot_entry_t *e)
{
  size_t                  topic_idx;
  const acquire_topic_t  *t;
  pthread_rwlock_rdlock(&acquire_entries_lock);

  topic_idx = 0;
  t = acq_proactive_pick_topic(e, &topic_idx);

  if(t == NULL)
  {
    pthread_rwlock_unlock(&acquire_entries_lock);
    return;
  }

  (void)acq_proactive_fire_locked(e, topic_idx, NULL, 0);
}

// A6R — on-demand reactive drain (kick).
//
// The periodic tick still runs on its 600 s cadence as a proactive
// driver and a safety net. For latency we also kick a one-shot drain
// task whenever a chat observer enqueues a reactive job: that task
// fires after ACQUIRE_KICK_DELAY_MS and drains the per-bot ring
// through the same pipeline the periodic tick uses, without the
// proactive half. The kick is dedup'd per-bot via `e->kick_pending`
// so a burst of matches produces at most one drain in flight.

static void
acq_bot_kick_drain(task_t *t)
{
  acquire_bot_entry_t *e = (acquire_bot_entry_t *)t->data;

  // Clear the pending flag first so a fresh enqueue landing while we
  // run can schedule the next kick instead of silently coalescing
  // into a drain that's already past its ring-pop phase.
  bool active;
  bool     enabled;
  uint32_t max_reactive_per_tick;
  pthread_rwlock_wrlock(&acquire_entries_lock);
  e->kick_pending = false;
  active = e->active;
  pthread_rwlock_unlock(&acquire_entries_lock);

  pthread_mutex_lock(&acquire_cfg_mutex);
  enabled = acquire_cfg.enabled;
  max_reactive_per_tick = acquire_cfg.max_reactive_per_tick;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  if(!enabled || !active)
  {
    t->state = TASK_ENDED;
    return;
  }

  for(uint32_t i = 0; i < max_reactive_per_tick; i++)
  {
    acquire_reactive_job_t job;

    if(!acquire_ring_pop(e, &job))
      break;

    acq_reactive_process_one(e, &job);
  }

  clam(CLAM_DEBUG2, ACQUIRE_CTX,
      "kick drain bot=%s (max=%u)", e->name, max_reactive_per_tick);

  t->state = TASK_ENDED;
}

void
acq_bot_kick_submit_deferred(acquire_bot_entry_t *e)
{
  bool need_submit;
  char tname[TASK_NAME_SZ];
  task_handle_t t;
  if(!acquire_ready || e == NULL)
    return;

  need_submit = false;

  pthread_rwlock_wrlock(&acquire_entries_lock);
  if(e->active && !e->kick_pending)
  {
    e->kick_pending = true;
    need_submit     = true;
  }
  pthread_rwlock_unlock(&acquire_entries_lock);

  if(!need_submit)
    return;

  pthread_mutex_lock(&acquire_stat_mutex);
  acquire_stats.total_reactive_kicks++;
  pthread_mutex_unlock(&acquire_stat_mutex);

  // TASK_NAME_SZ is small (40); clamp the bot-name slice via the
  // printf precision so truncation is explicit and the compiler's
  // -Wformat-truncation heuristic stays quiet.

  snprintf(tname, sizeof(tname), "acq:kick:%.24s", e->name);

  t = task_add_deferred(tname, TASK_ANY, 200,
      ACQUIRE_KICK_DELAY_MS, acq_bot_kick_drain, e);

  if(t == TASK_HANDLE_NONE)
  {
    // Submit failure: roll back the flag so a later enqueue can try
    // again. Rare path — task_add_deferred failures mean pool is tearing
    // down, in which case the periodic tick safety net picks up the
    // stranded ring entries on its next fire.
    pthread_rwlock_wrlock(&acquire_entries_lock);
    e->kick_pending = false;
    pthread_rwlock_unlock(&acquire_entries_lock);

    clam(CLAM_WARN, ACQUIRE_CTX,
        "kick submit failed bot=%s — falling back to periodic tick",
        e->name);
  }
}

// Per-bot periodic tick (A6 drained reactive jobs; A7 appends one
// proactive query per tick through the same async chain.)

void
acquire_bot_tick(task_t *t)
{
  acquire_bot_entry_t *e = (acquire_bot_entry_t *)t->data;

  bool     enabled;
  bool    active;
  uint32_t tick_cadence_secs;
  size_t  n_topics;
  uint32_t max_reactive_per_tick;
  pthread_mutex_lock(&acquire_cfg_mutex);
  enabled = acquire_cfg.enabled;
  tick_cadence_secs = acquire_cfg.tick_cadence_secs;
  max_reactive_per_tick = acquire_cfg.max_reactive_per_tick;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  // Pick up any live changes to the cadence KV on the next fire. The
  // task subsystem reads `interval_ms` every time a periodic task
  // finishes, so mutating it here is the blessed path for runtime
  // cadence changes (no respawn needed).
  t->interval_ms = tick_cadence_secs * 1000;

  pthread_rwlock_rdlock(&acquire_entries_lock);
  active = e->active;
  n_topics = e->n_topics;
  pthread_rwlock_unlock(&acquire_entries_lock);

  if(!enabled || !active)
  {
    t->state = TASK_ENDED;
    return;
  }

  e->tick_count++;

  pthread_mutex_lock(&acquire_stat_mutex);
  acquire_stats.total_ticks++;
  pthread_mutex_unlock(&acquire_stat_mutex);

  // 1. Drain up to N reactive jobs through the full pipeline.
  for(uint32_t i = 0; i < max_reactive_per_tick; i++)
  {
    acquire_reactive_job_t job;

    if(!acquire_ring_pop(e, &job))
      break;

    acq_reactive_process_one(e, &job);
  }

  // 2. At most one proactive query per tick — weighted-random topic
  //    pick, shared rate limiter, dispatched through the same async
  //    chain as reactive. No-op for bots with zero topics or when
  //    every topic's proactive_weight is 0 (reactive-only bots).
  acq_proactive_process(e);

  // 3. At most one due personality-declared feed fetch per tick.
  //    No-op when acquire.sources_enabled=false or the bot has no
  //    feeds attached.
  acq_feeds_tick(e);

  clam(CLAM_DEBUG2, ACQUIRE_CTX,
      "tick bot=%s tick=%u topics=%zu",
      e->name, e->tick_count, n_topics);

  t->state = TASK_ENDED;
}
