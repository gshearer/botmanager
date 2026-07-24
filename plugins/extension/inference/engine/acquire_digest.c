// botmanager — MIT
// Acquisition engine: digest helper (LLM-based relevance + summary).

#include "acquire_priv.h"
#include "llm_priv.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Digest helper: summarize + relevance-score a page body against a topic

// Static system prompt. The model is told exactly once what the shape
// of its output must be; any deviation trips the parser.
static const char acquire_digest_system_prompt[] =
    "You are a content digester. You read a web page and decide whether"
    " it is relevant to a named topic, then produce a compact factual"
    " summary of what a follower of that topic would want to know."
    "\n\n"
    "Respond in exactly this format, no other text:\n"
    "RELEVANCE: <integer 0-100>\n"
    "SUMMARY: <one paragraph, 300-600 characters, plain text, no"
    " markdown, no lists, no URLs>\n";

// Per-call context: heap-owned, freed in the LLM done callback.
typedef struct
{
  char                 topic_name[ACQUIRE_TOPIC_NAME_SZ];
  acquire_digest_cb_t  cb;
  void                *user_data;
} acquire_digest_ctx_t;

// Build the user prompt into a caller-provided heap buffer. Returns the
// number of bytes written (excl. NUL) or 0 on overflow.
static size_t
acquire_digest_build_user_prompt(const char *topic_name,
    const char *keywords_csv, const char *body, size_t body_len,
    uint32_t truncate_chars, char *out, size_t out_cap)
{
  int prefix_len;
  size_t n;
  int tail_len;
  if(body_len > truncate_chars)
    body_len = truncate_chars;

  prefix_len = snprintf(out, out_cap,
      "Topic: %s\n"
      "Keywords: %s\n\n"
      "Page content:\n"
      "---\n",
      topic_name,
      keywords_csv != NULL ? keywords_csv : "");

  if(prefix_len < 0 || (size_t)prefix_len >= out_cap)
    return(0);

  n = (size_t)prefix_len;

  if(n + body_len + 8 >= out_cap)
    body_len = (out_cap > n + 8) ? (out_cap - n - 8) : 0;

  if(body_len > 0)
  {
    memcpy(out + n, body, body_len);
    n += body_len;
  }

  tail_len = snprintf(out + n, out_cap - n, "\n---\n");

  if(tail_len < 0 || n + (size_t)tail_len >= out_cap)
    return(0);

  n += (size_t)tail_len;
  out[n] = '\0';
  return(n);
}

// Scan LLM response for `RELEVANCE: <int>` + `SUMMARY: <text>` prefixes.
// Prefix match is case-insensitive and line-anchored (either at
// content start or immediately after a \n). Returns SUCCESS only when
// both markers were found and the relevance value was parseable.
static bool
acquire_digest_parse_response(const char *content, size_t content_len,
    uint32_t *out_rel, char *out_summary, size_t summary_cap)
{
  const char *end;
  char   *endp;
  size_t summary_len;
  const char *relevance_val;
  long    rel;
  const char *summary_val;
  const char *p;
  bool        at_line_start;
  if(content == NULL || content_len == 0)
    return(FAIL);

  end = content + content_len;
  relevance_val = NULL;
  summary_val = NULL;
  p = content;
  at_line_start = true;

  while(p < end)
  {
    if(at_line_start && relevance_val == NULL
        && (size_t)(end - p) >= 10
        && strncasecmp(p, "RELEVANCE:", 10) == 0)
    {
      p += 10;
      while(p < end && (*p == ' ' || *p == '\t'))
        p++;

      relevance_val = p;
      while(p < end && *p != '\n' && *p != '\r')
        p++;

      at_line_start = true;
      continue;
    }

    if(at_line_start && summary_val == NULL
        && (size_t)(end - p) >= 8
        && strncasecmp(p, "SUMMARY:", 8) == 0)
    {
      p += 8;
      while(p < end && (*p == ' ' || *p == '\t'))
        p++;

      summary_val = p;
      break;                       // summary consumes the rest
    }

    if(*p == '\n')
      at_line_start = true;
    else if(*p != '\r')
      at_line_start = false;

    p++;
  }

  if(relevance_val == NULL || summary_val == NULL)
    return(FAIL);

  endp = NULL;
  rel = strtol(relevance_val, &endp, 10);

  if(endp == relevance_val)
    return(FAIL);

  if(rel < 0)   rel = 0;
  if(rel > 100) rel = 100;

  *out_rel = (uint32_t)rel;

  summary_len = (size_t)(end - summary_val);

  while(summary_len > 0)
  {
    char c = summary_val[summary_len - 1];

    if(c != ' ' && c != '\t' && c != '\n' && c != '\r')
      break;

    summary_len--;
  }

  if(summary_len == 0)
    return(FAIL);

  if(summary_len >= summary_cap)
    summary_len = summary_cap - 1;

  memcpy(out_summary, summary_val, summary_len);
  out_summary[summary_len] = '\0';
  return(SUCCESS);
}

// Deliver a FAIL response with the given error, then free the ctx.
static void
acquire_digest_deliver_fail(acquire_digest_ctx_t *ctx, const char *err)
{
  acquire_digest_response_t resp = {
    .ok        = false,
    .relevance = 0,
    .summary   = NULL,
    .error     = err,
    .user_data = ctx->user_data,
  };

  pthread_mutex_lock(&acquire_stat_mutex);
  acquire_stats.total_summaries_fail++;
  pthread_mutex_unlock(&acquire_stat_mutex);

  if(ctx->cb != NULL)
    ctx->cb(&resp);

  mem_free(ctx);
}

static void
acquire_digest_done_cb(const llm_chat_response_t *r)
{
  acquire_digest_ctx_t *ctx = (acquire_digest_ctx_t *)r->user_data;

  uint32_t relevance;
  acquire_digest_response_t resp;
  char     summary[ACQUIRE_DIGEST_SUMMARY_SZ];
  if(!r->ok || r->content == NULL)
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "digest topic='%s' chat failure: http=%ld error='%s'",
        ctx->topic_name, r->http_status,
        r->error != NULL ? r->error : "");
    acquire_digest_deliver_fail(ctx, "llm chat failed");
    return;
  }

  relevance = 0;

  summary[0] = '\0';

  if(acquire_digest_parse_response(r->content, r->content_len,
      &relevance, summary, sizeof(summary)) != SUCCESS)
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "digest topic='%s' could not parse RELEVANCE/SUMMARY from"
        " %zu-byte response",
        ctx->topic_name, r->content_len);
    acquire_digest_deliver_fail(ctx, "malformed digest response");
    return;
  }

  pthread_mutex_lock(&acquire_stat_mutex);
  acquire_stats.total_summaries_ok++;
  pthread_mutex_unlock(&acquire_stat_mutex);

  clam(CLAM_DEBUG2, ACQUIRE_CTX,
      "digest topic='%s' relevance=%u summary_len=%zu",
      ctx->topic_name, relevance, strlen(summary));

  resp = (acquire_digest_response_t){
    .ok        = true,
    .relevance = relevance,
    .summary   = summary,
    .error     = NULL,
    .user_data = ctx->user_data,
  };

  if(ctx->cb != NULL)
    ctx->cb(&resp);

  mem_free(ctx);
}

bool
acquire_digest_submit(const char *topic_name,
    const char *topic_keywords_csv, const char *body, size_t body_len,
    acquire_digest_cb_t cb, void *user_data)
{
  const char *model;
  uint32_t truncate_chars;
  char *user_prompt;
  acquire_digest_ctx_t *ctx;
  llm_chat_params_t params;
  llm_message_t messages[2];
  memset(messages, 0, sizeof(messages));
  bool ok;
  size_t n;
  if(!acquire_ready)
    return(FAIL);
  if(cb == NULL || topic_name == NULL || topic_name[0] == '\0')
    return(FAIL);
  if(body == NULL || body_len == 0)
    return(FAIL);

  model = kv_get_str("llm.default_chat_model");

  if(model == NULL || model[0] == '\0' || !llm_model_exists(model))
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "digest topic='%s': llm.default_chat_model is unset or"
        " unregistered",
        topic_name);
    return(FAIL);
  }

  pthread_mutex_lock(&acquire_cfg_mutex);
  truncate_chars = acquire_cfg.digest_body_truncate_chars;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  user_prompt = mem_alloc(ACQUIRE_CTX, "digest_prompt",
      ACQUIRE_DIGEST_PROMPT_MAX);

  n = acquire_digest_build_user_prompt(topic_name,
      topic_keywords_csv, body, body_len, truncate_chars,
      user_prompt, ACQUIRE_DIGEST_PROMPT_MAX);

  if(n == 0)
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "digest topic='%s': prompt did not fit %d bytes",
        topic_name, ACQUIRE_DIGEST_PROMPT_MAX);
    mem_free(user_prompt);
    return(FAIL);
  }

  ctx = mem_alloc(ACQUIRE_CTX, "digest_ctx",
      sizeof(*ctx));

  snprintf(ctx->topic_name, sizeof(ctx->topic_name), "%s", topic_name);
  ctx->cb        = cb;
  ctx->user_data = user_data;

  memset(&params, 0, sizeof(params));
  params.temperature = 0.2f;    // deterministic summaries
  params.max_tokens  = 512;     // covers ~600-char summary comfortably
  params.stream      = false;

  messages[0].role    = LLM_ROLE_SYSTEM;
  messages[0].content = acquire_digest_system_prompt;
  messages[1].role    = LLM_ROLE_USER;
  messages[1].content = user_prompt;

  ok = llm_chat_submit(model, &params, messages, 2,
      acquire_digest_done_cb, NULL, ctx);

  mem_free(user_prompt);   // llm_chat_submit already copied the strings

  if(ok != SUCCESS)
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "digest topic='%s' llm_chat_submit failed", topic_name);
    mem_free(ctx);
    return(FAIL);
  }

  return(SUCCESS);
}
