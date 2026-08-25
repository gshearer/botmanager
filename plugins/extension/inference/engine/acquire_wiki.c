// botmanager — MIT
// Acquisition engine: the encyclopedia source — one article and its
// Wikidata facts, in place of a search and a scrape.

#include "acquire_priv.h"

#include "plugin.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Types only, and no abort-on-miss shim: a topic that asked for the
// encyclopedia while the wikimedia plugin is unloaded must fall through
// to the search path, not take the daemon with it. The pointers are
// materialised below by our own dlsym, exactly as searxng's are.
#define WIKIMEDIA_TYPES_ONLY
#include "wikimedia_api.h"

// The fact block's budget. It is prepended to the article, and the
// digester truncates the whole body at acquire.digest_body_truncate_chars
// — so the facts go first, and they are small enough that the prose
// still gets most of that budget.
#define ACQ_WIKI_FACTS_SZ  2048

typedef async_rc_t (*acq_wm_resolve_fn_t)(const char *, wm_resolve_cb_t,
    void *);
typedef async_rc_t (*acq_wm_prose_fn_t)(const char *, bool, wm_prose_cb_t,
    void *);
typedef async_rc_t (*acq_wm_facts_fn_t)(const char *, wm_facts_cb_t, void *);

typedef struct
{
  acq_wm_resolve_fn_t resolve;
  acq_wm_prose_fn_t   prose;
  acq_wm_facts_fn_t   facts;
} acq_wm_vt_t;

// Resolved on first use and re-resolved after a NULL. Core owns these
// slots for the lifetime of the registration: it holds their addresses
// and NULLs them when the wikimedia plugin unloads, which is what turns
// the next job back into a search. Read and written under
// acquire_cfg_mutex, as the searxng pointer is.
static void *acq_wm_slot_resolve = NULL;
static void *acq_wm_slot_prose   = NULL;
static void *acq_wm_slot_facts   = NULL;

// One attempt at the encyclopedia. Owns nothing of the job but the
// pointer: whichever leaf ends the attempt either hands the job back to
// the search path or releases it, never both.
typedef struct
{
  acq_job_ctx_t *job;

  char           qid  [WM_QID_SZ];
  char           title[WM_TITLE_SZ];
  char           url  [WM_PAGE_URL_SZ];

  char          *prose;      // deep copy — the service lends its buffer
  size_t         prose_len;

  char           facts[ACQ_WIKI_FACTS_SZ];
} acq_wiki_ctx_t;

static void acq_wiki_resolve_done(const wm_resolve_res_t *res, void *user);
static void acq_wiki_prose_done(const wm_prose_res_t *res, void *user);
static void acq_wiki_facts_done(const wm_facts_res_t *res, void *user);
static void acq_wiki_digest_done(const acquire_digest_response_t *resp);
static void acq_wiki_digest(acq_wiki_ctx_t *w);

static void *
acq_wm_sym(const char *symbol, void **slot)
{
  void *fn;

  pthread_mutex_lock(&acquire_cfg_mutex);
  fn = *slot;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  if(fn != NULL)
    return(fn);

  fn = plugin_dlsym_cached(WIKIMEDIA_CTX, symbol, slot);

  pthread_mutex_lock(&acquire_cfg_mutex);
  *slot = fn;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  return(fn);
}

// All three verbs or none: a chain that could resolve the subject but
// not read the article would spend a request to learn nothing. POSIX
// permits round-tripping a function pointer through a void * via
// dlsym; the unions keep ISO C's stricter reading quiet.
static bool
acq_wm_vt(acq_wm_vt_t *out)
{
  union { void *obj; acq_wm_resolve_fn_t fn; } r;
  union { void *obj; acq_wm_prose_fn_t   fn; } p;
  union { void *obj; acq_wm_facts_fn_t   fn; } f;

  r.obj = acq_wm_sym("wm_resolve_async", &acq_wm_slot_resolve);
  p.obj = acq_wm_sym("wm_prose_async",   &acq_wm_slot_prose);
  f.obj = acq_wm_sym("wm_facts_async",   &acq_wm_slot_facts);

  out->resolve = r.fn;
  out->prose   = p.fn;
  out->facts   = f.fn;

  return(r.obj != NULL && p.obj != NULL && f.obj != NULL);
}

// Why the encyclopedia is not answering, in the words the log wants.
// ⛔ Every one of these is the ABSENCE of an answer: none of them may
// ever reach a corpus as a fact about the subject.
static const char *
acq_wiki_why(wm_status_t status)
{
  switch(status)
  {
    case WM_OK:           return("answered nothing");
    case WM_NOT_FOUND:    return("knows nothing of it");
    case WM_TRANSPORT:    return("could not be read");
    case WM_RATE_LIMITED: return("is rate-limiting us");
    case WM_UNAVAILABLE:  return("is stopping");
  }

  return("declined");
}

static void
acq_wiki_ctx_free(acq_wiki_ctx_t *w)
{
  if(w->prose != NULL)
    mem_free(w->prose);

  mem_free(w);
}

// The encyclopedia had nothing, or could not be reached. Hand the job
// to the search path: this is a preferred source and never a
// replacement, so a subject Wikipedia has never heard of — and a
// Wikipedia we could not reach — must still cost the job nothing.
static void
acq_wiki_fall_through(acq_wiki_ctx_t *w, const char *why)
{
  acq_job_ctx_t *job = w->job;

  clam(CLAM_DEBUG, ACQUIRE_CTX,
      "wikimedia %s bot=%s topic=%s subject='%s' — falling through"
      " to search", why, job->bot_name, job->topic_name, job->subject);

  acq_wiki_ctx_free(w);
  acq_reactive_start_search(job);
}

// The attempt is over and the job with it — the reserved source is
// given back here, and its release is what writes the topic stats.
static void
acq_wiki_finish(acq_wiki_ctx_t *w)
{
  acq_job_ctx_t *job = w->job;

  acq_wiki_ctx_free(w);
  acq_job_release_source(job);
}

bool
acq_wiki_try(acq_job_ctx_t *ctx)
{
  acq_wm_vt_t     vt;
  acq_wiki_ctx_t *w;

  if(!acq_wm_vt(&vt))
  {
    clam(CLAM_DEBUG, ACQUIRE_CTX,
        "wikimedia plugin not available bot=%s topic=%s — searching"
        " instead", ctx->bot_name, ctx->topic_name);
    return(false);
  }

  w = mem_alloc(ACQUIRE_CTX, "wiki_attempt", sizeof(*w));
  memset(w, 0, sizeof(*w));
  w->job = ctx;

  // Whichever field holds a thing somebody could look up. A reactive
  // job's subject is the noun phrase a chat line named; a proactive
  // one's is its topic, which is a slug ("shacarri"), and the phrase an
  // author wrote is the query ("sha'carri richardson"). A reactive
  // query is no use here at all — it is a template with the subject
  // pasted into a search string.
  if(vt.resolve(ctx->is_proactive ? ctx->query : ctx->subject,
      acq_wiki_resolve_done, w) != ASYNC_AIRBORNE)
  {
    mem_free(w);
    return(false);
  }

  return(true);
}

static void
acq_wiki_resolve_done(const wm_resolve_res_t *res, void *user)
{
  acq_wiki_ctx_t       *w = (acq_wiki_ctx_t *)user;
  const wm_candidate_t *hit;
  acq_wm_vt_t           vt;

  if(res->status != WM_OK || res->n == 0)
  {
    acq_wiki_fall_through(w, acq_wiki_why(res->status));
    return;
  }

  // hits[0] is the sitelink-ranked winner. A wrong winner is not
  // silent here the way it is on a command: the digester scores the
  // article against the topic's keywords, and an article about the
  // wrong Paris does not clear acquire.relevance_threshold.
  hit = &res->hits[0];
  strlcpy(w->qid,   hit->qid,   sizeof(w->qid));
  strlcpy(w->title, hit->title, sizeof(w->title));

  if(!acq_wm_vt(&vt))
  {
    acq_wiki_fall_through(w, "went away mid-flight");
    return;
  }

  // Named by its article where the item has one and by the item itself
  // where it does not — the service turns a QID into a sitelink for us.
  // `full` is the whole article rather than the lead: what a digest
  // reads is a knob (acquire.digest_body_truncate_chars), and raising
  // it must not mean going back to the wire for the rest of the page.
  if(vt.prose(w->title[0] != '\0' ? w->title : w->qid, true,
      acq_wiki_prose_done, w) != ASYNC_AIRBORNE)
    acq_wiki_fall_through(w, "would not take a prose request");
}

static void
acq_wiki_prose_done(const wm_prose_res_t *res, void *user)
{
  acq_wiki_ctx_t *w = (acq_wiki_ctx_t *)user;
  acq_wm_vt_t     vt;
  size_t          len;

  if(res->status != WM_OK || res->text == NULL || res->len == 0)
  {
    acq_wiki_fall_through(w, acq_wiki_why(res->status));
    return;
  }

  // Borrowed for this callback only, and an article runs to tens of KB.
  // Cap it where the scraped path caps its stripped page, so both
  // sources hand the digester bodies of the same order.
  len = res->len;

  if(len > ACQUIRE_REACTIVE_STRIPPED_MAX - 1)
    len = ACQUIRE_REACTIVE_STRIPPED_MAX - 1;

  w->prose = mem_alloc(ACQUIRE_CTX, "wiki_prose", len + 1);
  memcpy(w->prose, res->text, len);
  w->prose[len] = '\0';
  w->prose_len  = len;

  // Whatever the wiki finally answered under: a redirect names its
  // target, and the URL is built from that title rather than ours.
  strlcpy(w->title, res->title, sizeof(w->title));
  strlcpy(w->url,   res->url,   sizeof(w->url));

  if(res->qid[0] != '\0')
    strlcpy(w->qid, res->qid, sizeof(w->qid));

  // The facts are the half a digest cannot invent, but they are not
  // what makes the page worth ingesting — losing them costs the block
  // and not the article.
  if(w->qid[0] == '\0' || !acq_wm_vt(&vt)
      || vt.facts(w->qid, acq_wiki_facts_done, w) != ASYNC_AIRBORNE)
    acq_wiki_digest(w);
}

// Absent, unknown and none are three answers (wikimedia_api.h). A block
// that spelled the last two blank would leave the digester to guess
// which of the three it was reading.
static const char *
acq_wiki_value(const wm_value_t *v)
{
  switch(v->kind)
  {
    case WM_VAL_UNKNOWN: return("(not recorded)");
    case WM_VAL_NONE:    return("(none)");
    default:             return(v->text);
  }
}

// Render the fact block. Every write is bounded by what is left, and
// the first one that will not fit ends the block on a whole line —
// half a fact is worse than one fact fewer.
static void
acq_wiki_render_facts(const wm_facts_res_t *res, char *out, size_t cap)
{
  size_t at;
  int    n;

  n = snprintf(out, cap, "Wikidata facts for %s (%s)%s%s\n",
      res->label[0] != '\0' ? res->label : res->qid, res->qid,
      res->description[0] != '\0' ? " — " : "", res->description);

  if(n < 0 || (size_t)n >= cap)
  {
    out[0] = '\0';
    return;
  }

  at = (size_t)n;

  for(uint8_t i = 0; i < res->n; i++)
  {
    const wm_fact_t *f = &res->facts[i];

    if(f->n == 0)
      continue;

    n = snprintf(out + at, cap - at, "- %s:",
        f->label[0] != '\0' ? f->label : f->property);

    if(n < 0 || (size_t)n >= cap - at)
      break;

    at += (size_t)n;

    for(uint8_t v = 0; v < f->n; v++)
    {
      n = snprintf(out + at, cap - at, "%s %s", v == 0 ? "" : ";",
          acq_wiki_value(&f->values[v]));

      if(n < 0 || (size_t)n >= cap - at)
        break;

      at += (size_t)n;
    }

    n = (f->total > f->n)
        ? snprintf(out + at, cap - at, " (+%u more)\n", f->total - f->n)
        : snprintf(out + at, cap - at, "\n");

    if(n < 0 || (size_t)n >= cap - at)
      break;

    at += (size_t)n;
  }

  // A write that would not fit left its partial output behind; end the
  // block where the last complete line did.
  out[at] = '\0';
}

static void
acq_wiki_facts_done(const wm_facts_res_t *res, void *user)
{
  acq_wiki_ctx_t *w = (acq_wiki_ctx_t *)user;

  if(res->status == WM_OK)
    acq_wiki_render_facts(res, w->facts, sizeof(w->facts));

  acq_wiki_digest(w);
}

static void
acq_wiki_digest(acq_wiki_ctx_t *w)
{
  acq_job_ctx_t *job  = w->job;
  size_t         flen = strlen(w->facts);
  size_t         blen = flen + w->prose_len;
  char          *body = mem_alloc(ACQUIRE_CTX, "wiki_body", blen + 1);

  memcpy(body, w->facts, flen);
  memcpy(body + flen, w->prose, w->prose_len);
  body[blen] = '\0';

  // One article, so one source. Reserve it before the submit, the way
  // the search path reserves one per result: the digest callback is
  // what gives it back.
  pthread_mutex_lock(&job->lock);
  job->pending_sources = 1;
  pthread_mutex_unlock(&job->lock);

  clam(CLAM_DEBUG, ACQUIRE_CTX,
      "wikimedia digest bot=%s topic=%s subject='%s' article='%s'"
      " facts=%zu prose=%zu",
      job->bot_name, job->topic_name, job->subject, w->title,
      flen, w->prose_len);

  // A digester that will not take this body will not take a scraped
  // one either — its refusals are an unset model or a prompt that did
  // not fit — so there is nothing for a search to do with the job.
  if(acquire_digest_submit(job->topic_name, job->keywords_csv, body, blen,
      acq_wiki_digest_done, w) != SUCCESS)
  {
    mem_free(body);
    acq_wiki_finish(w);
    return;
  }

  // acquire_digest_submit copies what it needs into its own prompt.
  mem_free(body);
}

static void
acq_wiki_digest_done(const acquire_digest_response_t *resp)
{
  acq_wiki_ctx_t *w   = (acq_wiki_ctx_t *)resp->user_data;
  acq_job_ctx_t  *job = w->job;
  uint32_t        threshold;

  if(!resp->ok || resp->summary == NULL || resp->summary[0] == '\0')
  {
    acq_wiki_finish(w);
    return;
  }

  pthread_mutex_lock(&acquire_cfg_mutex);
  threshold = acquire_cfg.relevance_threshold;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  // The subject resolved to an entity, which says nothing about whether
  // the article is about the topic. This is the gate that catches a
  // sitelink-ranked winner that was the wrong one.
  if(resp->relevance < threshold)
  {
    clam(CLAM_DEBUG, ACQUIRE_CTX,
        "wikimedia relevance=%u below threshold=%u bot=%s topic=%s"
        " article='%s'",
        resp->relevance, threshold, job->bot_name, job->topic_name,
        w->title);
    acq_wiki_finish(w);
    return;
  }

  if(job->dest_corpus[0] == '\0')
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "wikimedia: no dest_corpus for bot=%s; dropping ingest",
        job->bot_name);
    acq_wiki_finish(w);
    return;
  }

  // The article title labels the page and the article URL names it —
  // the two halves knowledge_page_supersede() collapses a re-digest by.
  // No images: an extract carries none, and the infobox they would
  // come from is exactly what the plaintext endpoint leaves behind.
  if(acq_ingest_digest_result(job->bot_name, job->topic_name, job->subject,
      job->dest_corpus, job->is_proactive, resp, NULL, 0, w->title,
      w->url) == SUCCESS)
  {
    pthread_mutex_lock(&job->lock);
    job->n_inserted++;
    pthread_mutex_unlock(&job->lock);
  }

  acq_wiki_finish(w);
}
