// botmanager — MIT
// Knowledge URL ingest: fetch, strip, chunk verbatim.

#include "acquire_priv.h"

#include "cmd.h"
#include "curl.h"
#include "method.h"
#include "task.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

// Per-request closure. Allocated by knowledge_fetch_start, carried
// across the curl hop and then across the task hop, and freed by
// whichever of the three gives up on it first.
typedef struct
{
  cmd_ctx_t     ctx;
  method_msg_t  msg;
  char          corpus [KNOWLEDGE_CORPUS_NAME_SZ];
  char          url    [KNOWLEDGE_SOURCE_URL_SZ];   // as typed — what we fetch
  char          canon  [KNOWLEDGE_SOURCE_URL_SZ];   // what the rows are keyed by
  char          heading[KNOWLEDGE_SECTION_SZ];
  char         *text;         // stripped page; owned here, freed by the task
  size_t        text_len;
} kw_fetch_t;

static void kw_fetch_done(const curl_response_t *resp);
static void kw_fetch_ingest_task(task_t *t);

static void
kw_fetch_free(kw_fetch_t *fc)
{
  if(fc->text != NULL)
    mem_free(fc->text);

  mem_free(fc);
}

// Only markup carries text worth stripping. An absent Content-Type gets
// the benefit of the doubt, as urlgrabber's ug_content_is_html does;
// "text" joins its two because a fetched .txt is a legitimate seed and
// the stripper passes tagless bytes through unharmed.
static bool
kw_content_is_text(const char *ct)
{
  char lower[128];
  size_t n;

  if(ct == NULL || ct[0] == '\0')
    return(true);

  strlcpy(lower, ct, sizeof(lower));
  n = strlen(lower);

  for(size_t i = 0; i < n; i++)
    if(lower[i] >= 'A' && lower[i] <= 'Z')
      lower[i] = (char)(lower[i] - 'A' + 'a');

  return(strstr(lower, "html") != NULL
      || strstr(lower, "xml")  != NULL
      || strstr(lower, "text") != NULL);
}

void
knowledge_fetch_start(const cmd_ctx_t *ctx, const char *corpus,
    const char *url)
{
  // The headers a browser sends on a top-level navigation, in the order
  // it sends them. A copy of urlgrabber's array rather than an include:
  // an extension may not depend on a feature (PLUGIN.md §Layer Rules),
  // and seven string literals are the cheaper of the two wrongs.
  static const char *const nav_headers[] = {
    "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
    "Accept-Language: en-US,en;q=0.9",
    "Upgrade-Insecure-Requests: 1",
    "Sec-Fetch-Dest: document",
    "Sec-Fetch-Mode: navigate",
    "Sec-Fetch-Site: none",
    "Sec-Fetch-User: ?1"
  };

  kw_fetch_t     *fc;
  curl_request_t *req;
  const char     *ua;
  uint32_t        timeout;
  char            line[KNOWLEDGE_SOURCE_URL_SZ + 128];

  if(knowledge_corpus_upsert(corpus, NULL) != SUCCESS)
  {
    cmd_reply(ctx, "error: corpus upsert failed");
    return;
  }

  if(knowledge_corpus_is_page_chunked(corpus))
    cmd_reply(ctx,
        "note: this corpus is page-chunked — the acquisition engine keeps"
        " one chunk per page here and supersedes the rest, so the many"
        " chunks this fetch writes may not survive its next tick"
        " (KNOWLEDGE.md §Page-chunked corpora).");

  fc = mem_alloc("knowledge", "fetch_ctx", sizeof(*fc));
  memset(fc, 0, sizeof(*fc));

  fc->ctx = *ctx;

  if(ctx->msg != NULL)
    fc->msg = *ctx->msg;

  fc->ctx.msg      = &fc->msg;
  fc->ctx.args     = NULL;
  fc->ctx.username = NULL;
  fc->ctx.parsed   = NULL;
  fc->ctx.data     = NULL;

  // Neither survives the dispatching turn and nothing on this path
  // reads either — cmd_reply routes by msg->inst_name. NULLing them
  // makes the dangling deref unrepresentable rather than merely
  // unreached (method.h §method_msg_t).
  fc->ctx.bot  = NULL;
  fc->msg.inst = NULL;

  strlcpy(fc->corpus, corpus, sizeof(fc->corpus));
  strlcpy(fc->url, url, sizeof(fc->url));
  util_url_canon(url, fc->canon, sizeof(fc->canon));

  // The URL as typed goes on the wire; only the row's identity is
  // canonical. Acquire holds the same split (3984264).
  req = curl_request_create(CURL_METHOD_GET, fc->url, kw_fetch_done, fc);

  if(req == NULL)
  {
    cmd_reply(ctx, "error: could not build the fetch request");
    kw_fetch_free(fc);
    return;
  }

  // The curl core prepends, so walk the list backwards to land it on
  // the wire in browser order.
  for(size_t i = sizeof(nav_headers) / sizeof(nav_headers[0]); i > 0; i--)
    curl_request_add_header(req, nav_headers[i - 1]);

  ua = kv_get_str(KW_KV_FETCH_UA);

  if(ua != NULL && ua[0] != '\0')
    curl_request_set_user_agent(req, ua);

  timeout = (uint32_t)kv_get_uint(KW_KV_FETCH_TIMEOUT);

  if(timeout > 0)
    curl_request_set_timeout(req, timeout);

  // Idempotent and long; first to shed on a shutdown drain.
  curl_request_set_prio(req, CURL_PRIO_BULK);
  curl_request_set_follow_redirects(req, true);

  if(curl_request_submit(req) != SUCCESS)
  {
    cmd_reply(ctx, "error: could not submit the fetch request");
    kw_fetch_free(fc);
    return;
  }

  snprintf(line, sizeof(line), "fetching '%s' into corpus '%s' …",
      fc->url, fc->corpus);
  cmd_reply(ctx, line);
}

// Curl multi worker thread — must not block. Strips the page and hands
// the bytes on; everything that can wait happens in the task below.
static void
kw_fetch_done(const curl_response_t *resp)
{
  kw_fetch_t *fc = resp->user_data;
  char        raw[KNOWLEDGE_SECTION_SZ];
  char        line[KNOWLEDGE_SOURCE_URL_SZ + 256];

  if(fc == NULL)
    return;

  if(resp->curl_code != 0 || resp->status != 200
      || resp->body == NULL || resp->body_len == 0)
  {
    snprintf(line, sizeof(line),
        "fetch failed: HTTP %ld, curl %d (%s). A WAF refusal usually"
        " arrives here as a TRANSPORT error rather than a status — the"
        " connection is dropped before any status line — so this is not"
        " necessarily a site that is down.",
        resp->status, resp->curl_code,
        resp->error != NULL ? resp->error : "no detail");
    cmd_reply(&fc->ctx, line);
    kw_fetch_free(fc);
    return;
  }

  if(!kw_content_is_text(resp->content_type))
  {
    snprintf(line, sizeof(line),
        "refusing '%s' — the stripper only reads markup",
        resp->content_type);
    cmd_reply(&fc->ctx, line);
    kw_fetch_free(fc);
    return;
  }

  // acq_strip_html elides and collapses, never expands, so one output
  // byte per input byte is a cap that cannot truncate. That is why
  // there is no constant here.
  fc->text = mem_alloc("knowledge", "fetch_strip", resp->body_len + 1);
  fc->text_len = acq_strip_html(resp->body, resp->body_len, fc->text,
      resp->body_len + 1);

  if(fc->text_len == 0)
  {
    cmd_reply(&fc->ctx, "nothing left after stripping the page — no"
        " text outside its markup");
    kw_fetch_free(fc);
    return;
  }

  // The <title> comes back as raw bytes, entities and all, so it goes
  // back through the stripper to be decoded and collapsed. An empty
  // heading is legal — and is part of the dedup key either way.
  if(acq_find_title(resp->body, resp->body_len, raw, sizeof(raw)))
    acq_strip_html(raw, strlen(raw), fc->heading, sizeof(fc->heading));

  // Hop to a worker before chunking. knowledge_batch_add flushes
  // through llm_embed_submit_wait → curl_request_submit_wait, which
  // waits for a queue slot that this very thread has to free: doing the
  // ingest here self-deadlocks (knowledge.c:564 says it in the same
  // words).
  if(task_add("knowledge_fetch", TASK_THREAD, 100, kw_fetch_ingest_task,
        fc) == NULL)
  {
    cmd_reply(&fc->ctx, "error: could not schedule the ingest task");
    kw_fetch_free(fc);
  }
}

// Worker thread — may block.
static void
kw_fetch_ingest_task(task_t *t)
{
  kw_fetch_t *fc = t->data;
  knowledge_ingest_stats_t st;

  // Wide enough for the "page:" line, which is the only one here that
  // formats two of the closure's bounded fields into the same buffer.
  char line[KNOWLEDGE_SOURCE_URL_SZ + KNOWLEDGE_SECTION_SZ + 64];

  t->state = TASK_ENDED;                // one-shot; set before any return

  if(fc == NULL)
    return;

  if(knowledge_ingest_text(fc->corpus, fc->canon, fc->heading, fc->text,
        fc->text_len, &st) != SUCCESS)
  {
    cmd_reply(&fc->ctx, "error: ingest failed");
    kw_fetch_free(fc);
    return;
  }

  snprintf(line, sizeof(line),
      "ingested %zu new chunk(s) from %zu page(s)"
      " (%zu skipped, %zu already present, %zu re-embedded).",
      st.chunks, st.files, st.skipped, st.duplicates, st.reembedded);
  cmd_reply(&fc->ctx, line);

  if(fc->heading[0] != '\0')
  {
    snprintf(line, sizeof(line), "page: %s  →  %s", fc->heading, fc->canon);
    cmd_reply(&fc->ctx, line);
  }

  snprintf(line, sizeof(line),
      "embeds submitted: %llu; failed: %llu. "
      "(Completion is asynchronous; /show knowledge tracks progress.)",
      (unsigned long long)st.embed_ok,
      (unsigned long long)st.embed_fail);
  cmd_reply(&fc->ctx, line);

  // Says the same true thing here as on the file path: the counts above
  // are a prefix of the page rather than the page, the chunks already
  // inserted carry no vector, and the repair is this same command run
  // again.
  if(st.aborted)
  {
    cmd_reply(&fc->ctx,
        "ABORTED: the embed engine stopped taking work (see the curl"
        " submit_wait warning), so the walk stopped early.");

    snprintf(line, sizeof(line),
        "%llu chunk(s) are stored WITHOUT an embedding and are invisible"
        " to retrieval.",
        (unsigned long long)st.embed_fail);
    cmd_reply(&fc->ctx, line);

    cmd_reply(&fc->ctx,
        "Fix the engine, then run the same command again: ingest is"
        " idempotent, so a re-run skips what is already stored and"
        " embeds only the chunks that still have no vector.");
  }

  kw_fetch_free(fc);
}
