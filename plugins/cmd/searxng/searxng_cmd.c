// botmanager — MIT
// /searxng command-surface plugin: admin-only diagnostic that submits
// queries to the configured SearXNG endpoint via the searxng service
// plugin's public API. Exposes one subcommand per SearXNG category so
// the command bot can drive all of general / images / news / videos /
// music from a single command tree.
#define SEARXNG_CMD_INTERNAL
#include "searxng_cmd.h"

#include <stdio.h>
#include <string.h>

// Forward declarations for cmd_register binding.
static void searxng_cmd_root  (const cmd_ctx_t *ctx);
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

  snprintf(line, sizeof(line), "searxng %s: %zu result%s",
      sxng_category_name(resp->category),
      resp->n_results, resp->n_results == 1 ? "" : "s");
  cmd_reply(&ctx, line);

  for(size_t i = 0; i < resp->n_results; i++)
  {
    const sxng_result_t *rr = &resp->results[i];

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

// Submit one search at the given category. Owns the per-call closure
// allocation; on submit failure the closure is freed in-line and an
// error reply is emitted. The async path always frees on its own.
static void
searxng_cmd_dispatch(const cmd_ctx_t *ctx, sxng_category_t category)
{
  searxng_cmd_req_t *r;

  if(ctx->args == NULL || ctx->args[0] == '\0')
  {
    char line[SEARXNG_CMD_REPLY_SZ];

    snprintf(line, sizeof(line), "Usage: searxng %s <query>",
        sxng_category_name(category));
    cmd_reply(ctx, line);
    return;
  }

  r = mem_alloc(SEARXNG_CMD_CTX, "req", sizeof(*r));
  memset(r, 0, sizeof(*r));
  r->ctx      = *ctx;
  r->category = category;

  if(ctx->msg != NULL)
    r->msg = *ctx->msg;

  r->ctx.msg      = &r->msg;
  r->ctx.args     = NULL;
  r->ctx.username = NULL;
  r->ctx.parsed   = NULL;
  r->ctx.data     = NULL;

  if(sxng_search(ctx->args, category, 0, searxng_cmd_done, r) != SUCCESS)
  {
    cmd_reply(ctx, "searxng: failed to submit query "
        "(check plugin.searxng.endpoint)");
    mem_free(r);
  }
}

// Command callbacks

static void
searxng_cmd_root(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /searxng <subcommand>");
  cmd_reply(ctx, "  general <query> — web search (default)");
  cmd_reply(ctx, "  images  <query> — image search");
  cmd_reply(ctx, "  news    <query> — news / current events");
  cmd_reply(ctx, "  videos  <query> — video search");
  cmd_reply(ctx, "  music   <query> — music / audio search");
}

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

static bool
searxng_cmd_register_sub(const char *name, const char *usage,
    const char *desc, cmd_cb_t cb)
{
  return(cmd_register(SEARXNG_CMD_CTX, name, usage, desc, NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      cb, NULL, SEARXNG_CMD_CTX, NULL,
      NULL, 0, NULL, NULL));
}

static bool
searxng_cmd_init(void)
{
  if(cmd_register(SEARXNG_CMD_CTX, SEARXNG_CMD_CTX,
      "searxng <category> <query>",
      "Submit a query to the configured SearXNG endpoint",
      "Administrative helper for the SearXNG service plugin.\n"
      "Exercises the endpoint configured at\n"
      "plugin.searxng.endpoint and prints the parsed results,\n"
      "with category-appropriate metadata (image dimensions,\n"
      "video length, news publication date).\n"
      "\n"
      "Categories: general, images, news, videos, music.\n"
      "\n"
      "Examples:\n"
      "  /searxng general dua lipa\n"
      "  /searxng news ai legislation\n"
      "  /searxng images aurora borealis\n"
      "  /searxng videos arch linux install\n"
      "  /searxng music aphex twin selected ambient",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      searxng_cmd_root, NULL, NULL, NULL,
      NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(searxng_cmd_register_sub("general", "general <query>",
        "Web search via SearXNG", searxng_cmd_general) != SUCCESS
      || searxng_cmd_register_sub("images", "images <query>",
        "Image search via SearXNG", searxng_cmd_images) != SUCCESS
      || searxng_cmd_register_sub("news", "news <query>",
        "News search via SearXNG", searxng_cmd_news) != SUCCESS
      || searxng_cmd_register_sub("videos", "videos <query>",
        "Video search via SearXNG", searxng_cmd_videos) != SUCCESS
      || searxng_cmd_register_sub("music", "music <query>",
        "Music search via SearXNG", searxng_cmd_music) != SUCCESS)
  {
    cmd_unregister("general");
    cmd_unregister("images");
    cmd_unregister("news");
    cmd_unregister("videos");
    cmd_unregister("music");
    cmd_unregister(SEARXNG_CMD_CTX);
    return(FAIL);
  }

  clam(CLAM_INFO, SEARXNG_CMD_CTX,
      "searxng command plugin initialized");
  return(SUCCESS);
}

static void
searxng_cmd_deinit(void)
{
  cmd_unregister("general");
  cmd_unregister("images");
  cmd_unregister("news");
  cmd_unregister("videos");
  cmd_unregister("music");
  cmd_unregister(SEARXNG_CMD_CTX);
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
    { .name = "bot_command" },
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
