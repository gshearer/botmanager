// botmanager — MIT
// searxng command-surface plugin: a public search interface over the
// searxng service plugin. Exposes one top-level command per SearXNG
// category, each usable by anyone in channels or private messages:
//
//   !searxng / !search / !s   general web search
//   !news    / !n             news / current events
//   !image   / !i             image search
//   !video   / !v             video search
//   !music                    music / audio search
//
// Every command accepts an optional "-n <count>" prefix to request a
// specific number of results (clamped to plugin.searxng.max_results);
// with no -n the count defaults to plugin.searxng.min_results. Results
// are formatted with category-appropriate metadata (image dimensions,
// video length, news publication date).
#define SEARXNG_CMD_INTERNAL
#include "searxng_cmd.h"

#include "kv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Forward declarations for cmd_register binding.
static void searxng_cmd_general(const cmd_ctx_t *ctx);
static void searxng_cmd_images (const cmd_ctx_t *ctx);
static void searxng_cmd_news   (const cmd_ctx_t *ctx);
static void searxng_cmd_videos (const cmd_ctx_t *ctx);
static void searxng_cmd_music  (const cmd_ctx_t *ctx);

// Per-line body cap for %.*s slots. Sized to leave room for the
// widest leading label we emit ("   img: ", 8 bytes) plus the
// terminating NUL — safe for the bare "   " indent used by the URL
// and snippet lines as well.
#define SXNG_CMD_LINE_BODY      ((int)(SEARXNG_CMD_REPLY_SZ - 16))

// Append a single result row's category-specific extras as separate
// reply lines. Skips empty fields silently.
static void
searxng_cmd_emit_extras(cmd_ctx_t *ctx, const sxng_result_t *rr)
{
  char line[SEARXNG_CMD_REPLY_SZ];

  switch(rr->category)
  {
    case SXNG_CAT_IMAGES:
      if(rr->extras.image.src[0] != '\0')
      {
        snprintf(line, sizeof(line), "   img: %.*s",
            SXNG_CMD_LINE_BODY, rr->extras.image.src);
        cmd_reply(ctx, line);
      }

      if(rr->extras.image.resolution[0] != '\0')
      {
        snprintf(line, sizeof(line), "   resolution: %s",
            rr->extras.image.resolution);
        cmd_reply(ctx, line);
      }
      break;

    case SXNG_CAT_NEWS:
      if(rr->extras.news.published[0] != '\0'
          || rr->extras.news.source[0] != '\0')
      {
        snprintf(line, sizeof(line), "   %s%s%s",
            rr->extras.news.source[0]    != '\0'
                ? rr->extras.news.source : "",
            (rr->extras.news.source[0]    != '\0'
              && rr->extras.news.published[0] != '\0')
                ? " — " : "",
            rr->extras.news.published[0] != '\0'
                ? rr->extras.news.published : "");
        cmd_reply(ctx, line);
      }
      break;

    case SXNG_CAT_VIDEOS:
      if(rr->extras.video.author[0] != '\0'
          || rr->extras.video.length[0] != '\0'
          || rr->extras.video.published[0] != '\0')
      {
        snprintf(line, sizeof(line), "   %s%s%s%s%s",
            rr->extras.video.author[0]    != '\0'
                ? rr->extras.video.author : "",
            (rr->extras.video.author[0] != '\0'
              && rr->extras.video.length[0] != '\0')
                ? " · " : "",
            rr->extras.video.length[0]    != '\0'
                ? rr->extras.video.length : "",
            (rr->extras.video.length[0] != '\0'
              && rr->extras.video.published[0] != '\0')
                ? " · " : "",
            rr->extras.video.published[0] != '\0'
                ? rr->extras.video.published : "");
        cmd_reply(ctx, line);
      }
      break;

    case SXNG_CAT_MUSIC:
      if(rr->extras.music.author[0] != '\0'
          || rr->extras.music.published[0] != '\0')
      {
        snprintf(line, sizeof(line), "   %s%s%s",
            rr->extras.music.author[0]    != '\0'
                ? rr->extras.music.author : "",
            (rr->extras.music.author[0] != '\0'
              && rr->extras.music.published[0] != '\0')
                ? " — " : "",
            rr->extras.music.published[0] != '\0'
                ? rr->extras.music.published : "");
        cmd_reply(ctx, line);
      }
      break;

    case SXNG_CAT_GENERAL:
    case SXNG_CAT__COUNT:
      break;
  }
}

// Async completion: format and free the per-request closure.
static void
searxng_cmd_done(const sxng_response_t *resp)
{
  searxng_cmd_req_t *r = (searxng_cmd_req_t *)resp->user_data;
  cmd_ctx_t          ctx = r->ctx;
  char               line[SEARXNG_CMD_REPLY_SZ];

  ctx.msg = &r->msg;

  if(!resp->ok)
  {
    snprintf(line, sizeof(line), "searxng error: %s",
        resp->error != NULL ? resp->error : "unknown");
    cmd_reply(&ctx, line);
    mem_free(r);
    return;
  }

  if(resp->n_results == 0)
  {
    snprintf(line, sizeof(line), "searxng %s: no results",
        sxng_category_name(resp->category));
    cmd_reply(&ctx, line);
    mem_free(r);
    return;
  }

  for(size_t i = 0; i < resp->n_results; i++)
  {
    const sxng_result_t *rr = &resp->results[i];

    // Default (concise) mode: just the URL, one per line. For image
    // results the SearXNG `url` field is the web page hosting the image,
    // not the image itself — prefer the direct `img_src` link so !image
    // returns something viewable. Fall back to the page URL if a result
    // carries no img_src.
    if(!r->verbose)
    {
      const char *link =
          (rr->category == SXNG_CAT_IMAGES && rr->extras.image.src[0] != '\0')
              ? rr->extras.image.src : rr->url;

      snprintf(line, sizeof(line), "%.*s", SXNG_CMD_LINE_BODY, link);
      cmd_reply(&ctx, line);
      continue;
    }

    // Verbose mode: numbered title, URL, snippet, category extras.
    snprintf(line, sizeof(line), "%zu. %s", i + 1,
        rr->title[0] != '\0' ? rr->title : "(untitled)");
    cmd_reply(&ctx, line);

    snprintf(line, sizeof(line), "   %.*s",
        SXNG_CMD_LINE_BODY, rr->url);
    cmd_reply(&ctx, line);

    if(rr->snippet[0] != '\0')
    {
      snprintf(line, sizeof(line), "   %.*s",
          SXNG_CMD_LINE_BODY, rr->snippet);
      cmd_reply(&ctx, line);
    }

    searxng_cmd_emit_extras(&ctx, rr);
  }

  mem_free(r);
}

// Append one whitespace-separated token to the query buffer, inserting a
// single space before every token but the first. Silently drops tokens
// that would overflow the buffer — a truncated query is preferable to a
// clobbered stack.
static void
searxng_cmd_query_append(char *query, size_t cap, size_t *len,
    const char *tok)
{
  int wrote;

  if(*len >= cap)
    return;

  wrote = snprintf(query + *len, cap - *len, "%s%s",
      *len > 0 ? " " : "", tok);

  if(wrote > 0)
    *len += (size_t)wrote < cap - *len ? (size_t)wrote : cap - *len - 1;
}

// Parse the optional leading flags out of a raw argument string, writing
// the residual query (flags removed) into `query`. Two flags are
// recognised, each consumed on its first occurrence anywhere in the token
// stream:
//   -n <count>  request a specific result count -> *count, *have_count
//   -v          verbose display                 -> *verbose
// A "-n" whose successor is not a base-10 number is treated as two
// ordinary query words, so a literal search for "-n foo" still works.
static void
searxng_cmd_parse_flags(const char *args, char *query, size_t cap,
    uint32_t *count, bool *have_count, bool *verbose)
{
  char   scratch[METHOD_TEXT_SZ];
  char  *save;
  size_t qlen;

  snprintf(scratch, sizeof(scratch), "%s", args);
  query[0]    = '\0';
  qlen        = 0;
  *have_count = false;
  *verbose    = false;

  for(char *tok = strtok_r(scratch, " \t", &save); tok != NULL;
      tok = strtok_r(NULL, " \t", &save))
  {
    if(!*verbose && strcmp(tok, "-v") == 0)
    {
      *verbose = true;
      continue;
    }

    if(!*have_count && strcmp(tok, "-n") == 0)
    {
      char *num = strtok_r(NULL, " \t", &save);
      char *end;
      unsigned long v;

      if(num == NULL)
        continue;                   // trailing "-n" with no value: drop

      v = strtoul(num, &end, 10);

      if(end != num && *end == '\0')
      {
        *count      = (uint32_t)v;
        *have_count = true;
        continue;
      }

      // Not a number — keep both tokens as literal query words.
      searxng_cmd_query_append(query, cap, &qlen, tok);
      searxng_cmd_query_append(query, cap, &qlen, num);
      continue;
    }

    searxng_cmd_query_append(query, cap, &qlen, tok);
  }
}

// Submit one search at the given category. Parses the optional -n flag,
// resolves the requested result count against plugin.searxng.min_results,
// and owns the per-call closure allocation; on submit failure the closure
// is freed in-line and an error reply is emitted. The async path always
// frees on its own.
static void
searxng_cmd_dispatch(const cmd_ctx_t *ctx, sxng_category_t category)
{
  searxng_cmd_req_t *r;
  char               query[METHOD_TEXT_SZ];
  uint32_t           n_wanted;
  bool               have_n;
  bool               verbose;

  if(ctx->args == NULL || ctx->args[0] == '\0')
  {
    cmd_reply(ctx, "Usage: [-v] [-n <count>] <query>");
    return;
  }

  n_wanted = 0;
  searxng_cmd_parse_flags(ctx->args, query, sizeof(query),
      &n_wanted, &have_n, &verbose);

  if(query[0] == '\0')
  {
    cmd_reply(ctx, "Usage: [-v] [-n <count>] <query>");
    return;
  }

  // No -n (or "-n 0") falls back to the configured floor. The service
  // clamps the final count to [min_results, max_results], so an
  // over-large -n is honoured only up to max_results.
  if(!have_n || n_wanted == 0)
  {
    n_wanted = (uint32_t)kv_get_uint("plugin.searxng.min_results");

    if(n_wanted == 0)
      n_wanted = 1;
  }

  r = mem_alloc(SEARXNG_CMD_CTX, "req", sizeof(*r));
  memset(r, 0, sizeof(*r));
  r->ctx      = *ctx;
  r->category = category;
  r->verbose  = verbose;

  if(ctx->msg != NULL)
    r->msg = *ctx->msg;

  r->ctx.msg      = &r->msg;
  r->ctx.args     = NULL;
  r->ctx.username = NULL;
  r->ctx.parsed   = NULL;
  r->ctx.data     = NULL;

  if(sxng_search(query, category, n_wanted, searxng_cmd_done, r) != SUCCESS)
  {
    cmd_reply(ctx, "searxng: failed to submit query "
        "(check plugin.searxng.endpoint)");
    mem_free(r);
  }
}

// Command callbacks. Each top-level command binds to one category; the
// namesake "searxng" and its "search"/"s" aliases all perform a general
// web search.

static void searxng_cmd_general(const cmd_ctx_t *ctx)
{ searxng_cmd_dispatch(ctx, SXNG_CAT_GENERAL); }

static void searxng_cmd_images(const cmd_ctx_t *ctx)
{ searxng_cmd_dispatch(ctx, SXNG_CAT_IMAGES); }

static void searxng_cmd_news(const cmd_ctx_t *ctx)
{ searxng_cmd_dispatch(ctx, SXNG_CAT_NEWS); }

static void searxng_cmd_videos(const cmd_ctx_t *ctx)
{ searxng_cmd_dispatch(ctx, SXNG_CAT_VIDEOS); }

static void searxng_cmd_music(const cmd_ctx_t *ctx)
{ searxng_cmd_dispatch(ctx, SXNG_CAT_MUSIC); }

// Plugin lifecycle

// One public, top-level search command. `abbrev` may be NULL for the
// namesake and the aliasless "music" command. All entries share the same
// permission profile: the "everyone" group at level 0, usable in both
// channels and private messages (CMD_SCOPE_ANY).
typedef struct
{
  const char *name;
  const char *abbrev;
  const char *usage;
  const char *desc;
  cmd_cb_t    cb;
} searxng_cmd_entry_t;

static const searxng_cmd_entry_t searxng_cmd_table[] = {
  { SEARXNG_CMD_CTX, NULL, "searxng [-v] [-n <count>] <query>",
    "Web search via SearXNG", searxng_cmd_general },
  { "search", "s", "search [-v] [-n <count>] <query>",
    "Web search via SearXNG", searxng_cmd_general },
  { "news", "n", "news [-v] [-n <count>] <query>",
    "News / current-events search via SearXNG", searxng_cmd_news },
  { "image", "i", "image [-v] [-n <count>] <query>",
    "Image search via SearXNG", searxng_cmd_images },
  { "video", "v", "video [-v] [-n <count>] <query>",
    "Video search via SearXNG", searxng_cmd_videos },
  { "music", NULL, "music [-v] [-n <count>] <query>",
    "Music / audio search via SearXNG", searxng_cmd_music },
};

#define SEARXNG_CMD_TABLE_N \
    (sizeof(searxng_cmd_table) / sizeof(searxng_cmd_table[0]))

static const char searxng_cmd_help[] =
    "Search the web via the configured SearXNG endpoint.\n"
    "\n"
    "Commands (all public, usable in channels or private messages):\n"
    "  searxng / search / s   web search\n"
    "  news / n               news / current events\n"
    "  image / i              image search\n"
    "  video / v              video search\n"
    "  music                  music / audio search\n"
    "\n"
    "By default only result URLs are returned. Add \"-v\" for\n"
    "verbose output (title, URL, snippet, and category metadata).\n"
    "\n"
    "Add \"-n <count>\" to request a specific number of results\n"
    "(capped at plugin.searxng.max_results). With no -n the count\n"
    "defaults to plugin.searxng.min_results.\n"
    "\n"
    "Examples:\n"
    "  !s dua lipa\n"
    "  !n -n 5 ai legislation\n"
    "  !i -v aurora borealis\n"
    "  !v arch linux install\n"
    "  !music aphex twin selected ambient";

static void
searxng_cmd_unregister_all(void)
{
  for(size_t i = 0; i < SEARXNG_CMD_TABLE_N; i++)
    cmd_unregister(searxng_cmd_table[i].name);
}

static bool
searxng_cmd_init(void)
{
  for(size_t i = 0; i < SEARXNG_CMD_TABLE_N; i++)
  {
    const searxng_cmd_entry_t *e = &searxng_cmd_table[i];

    if(cmd_register(SEARXNG_CMD_CTX, e->name, e->usage, e->desc,
        searxng_cmd_help, USERNS_GROUP_EVERYONE, 0,
        CMD_SCOPE_ANY, METHOD_T_ANY, e->cb, NULL,
        NULL, e->abbrev, NULL, 0, NULL, NULL) != SUCCESS)
    {
      searxng_cmd_unregister_all();
      return(FAIL);
    }
  }

  clam(CLAM_INFO, SEARXNG_CMD_CTX,
      "searxng command plugin initialized");
  return(SUCCESS);
}

static void
searxng_cmd_deinit(void)
{
  searxng_cmd_unregister_all();
  clam(CLAM_INFO, SEARXNG_CMD_CTX,
      "searxng command plugin deinitialized");
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "searxng_cmd",
  .version         = "1.0",
  .type            = PLUGIN_MISC,
  .kind            = "searxng_cmd",
  .provides        = { { .name = "cmd_searxng" } },
  .provides_count  = 1,
  .requires        = {
    { .name = "method_text" },
    { .name = "service_searxng" },
  },
  .requires_count  = 2,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = searxng_cmd_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = searxng_cmd_deinit,
  .ext             = NULL,
};
