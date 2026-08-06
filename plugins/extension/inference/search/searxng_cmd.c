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
//
// !news alone — with no query — is the one exception to "a search needs
// words": it runs the standing plugin.searxng.news_query and prints
// plugin.searxng.news_headlines stories as bare headlines.
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

// Headline-mode line budget: the link gets half the reply buffer, the
// headline the rest less the " — " joiner and the NUL, so the two
// precisions provably fit together. Both halves are far wider than an
// IRC line survives anyway.
#define SXNG_CMD_HEADLINE_TAIL  ((int)(SEARXNG_CMD_REPLY_SZ / 2))
#define SXNG_CMD_HEADLINE_HEAD  \
    ((int)(SEARXNG_CMD_REPLY_SZ - SXNG_CMD_HEADLINE_TAIL - 4))

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

// The link this result is rendered as. Every mode prints the page URL but
// one: a concise !image prints the direct image, because SearXNG's `url`
// for an image result is the page hosting it rather than the picture.
static const char *
searxng_cmd_result_link(const searxng_cmd_req_t *r, const sxng_result_t *rr)
{
  if(!r->verbose && rr->category == SXNG_CAT_IMAGES
      && rr->extras.image.src[0] != '\0')
    return(rr->extras.image.src);

  return(rr->url);
}

// Offer a URL we just printed to whoever grabs titles (today: urlgrabber),
// as a single log line. The contract — context, payload shape, and why a
// clam event rather than a call — is documented in CLAM.md and in
// plugins/feature/urlgrabber/urlgrabber.h; the short version is that the
// layer rule forbids an extension from depending on a feature, and an
// offer is a candidate rather than an order: the far side still applies
// its own opt-in, media and private-host filters.
static void
searxng_cmd_offer_url(const cmd_ctx_t *ctx, const char *url)
{
  const char *bot;
  const char *method;
  char        payload[CLAM_MSG_SZ];

  // A search answered in private has no room to annotate.
  if(ctx->bot == NULL || ctx->msg == NULL || ctx->msg->channel[0] == '\0')
    return;

  bot    = bot_inst_name(ctx->bot);
  method = method_inst_name(ctx->msg->inst);

  if(bot == NULL || method == NULL)
    return;

  // clam truncates at CLAM_MSG_SZ without saying so, and half a URL is
  // worse than no URL — decline to offer one that would not survive.
  if(snprintf(payload, sizeof(payload), "%s %s %s %s",
      bot, method, ctx->msg->channel, url) >= (int)sizeof(payload))
    return;

  clam(CLAM_INFO, "url_offer", "%s", payload);
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

    // Headline mode (a bare !news): one line per story — the headline,
    // then the article link last so a client renders it clickable. The
    // two precisions are sized to fit one reply buffer between them, so
    // a long headline can never crowd the link off the end. "-v" opts
    // back into the full verbose rendering below.
    if(r->headlines && !r->verbose)
    {
      snprintf(line, sizeof(line), "%.*s — %.*s",
          SXNG_CMD_HEADLINE_HEAD,
          rr->title[0] != '\0' ? rr->title : "(untitled)",
          SXNG_CMD_HEADLINE_TAIL, rr->url);
      cmd_reply(&ctx, line);
      continue;
    }

    // Default (concise) mode: just the URL, one per line. For image
    // results the SearXNG `url` field is the web page hosting the image,
    // not the image itself — prefer the direct `img_src` link so !image
    // returns something viewable. Fall back to the page URL if a result
    // carries no img_src.
    if(!r->verbose)
    {
      snprintf(line, sizeof(line), "%.*s", SXNG_CMD_LINE_BODY,
          searxng_cmd_result_link(r, rr));
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

  // A lone result is the one case worth a fetched title: the room is
  // looking at a single link and nothing else. Two or more and a title per
  // line would bury the results it set out to annotate, so a multi-result
  // search offers nothing at all.
  if(resp->n_results == 1)
    searxng_cmd_offer_url(&ctx, searxng_cmd_result_link(r, &resp->results[0]));

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
  bool               headlines;

  query[0]  = '\0';
  n_wanted  = 0;
  have_n    = false;
  verbose   = false;
  headlines = false;

  if(ctx->args != NULL && ctx->args[0] != '\0')
    searxng_cmd_parse_flags(ctx->args, query, sizeof(query),
        &n_wanted, &have_n, &verbose);

  // A word-less !news is not a usage error — it is headline mode, which
  // stands in the configured standing query and a headline-sized count.
  // Every other category still needs something to search for.
  if(query[0] == '\0')
  {
    const char *standing;

    if(category != SXNG_CAT_NEWS)
    {
      cmd_reply(ctx, "Usage: [-v] [-n <count>] <query>");
      return;
    }

    standing = kv_get_str("plugin.searxng.news_query");

    if(standing == NULL || standing[0] == '\0')
    {
      cmd_reply(ctx, "news: plugin.searxng.news_query is not set");
      return;
    }

    headlines = true;
    snprintf(query, sizeof(query), "%s", standing);

    if(!have_n || n_wanted == 0)
    {
      n_wanted = (uint32_t)kv_get_uint("plugin.searxng.news_headlines");
      have_n   = n_wanted > 0;
    }
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
  r->ctx       = *ctx;
  r->category  = category;
  r->verbose   = verbose;
  r->headlines = headlines;

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
  { "news", "n", "news [-v] [-n <count>] [query]",
    "News search via SearXNG; no query prints the headlines",
    searxng_cmd_news },
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
    "  news / n               news / current events (bare: headlines)\n"
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
    "!news on its own takes no query: it prints the current headlines\n"
    "(story and link, one per line) from the standing search in\n"
    "plugin.searxng.news_query, plugin.searxng.news_headlines of them.\n"
    "Both -n and -v still apply.\n"
    "\n"
    "Examples:\n"
    "  !s dua lipa\n"
    "  !n\n"
    "  !n -n 5 ai legislation\n"
    "  !i -v aurora borealis\n"
    "  !v arch linux install\n"
    "  !music aphex twin selected ambient";

static void
searxng_cmd_unregister_all(void)
{
  // Every entry registers at root (parent_path NULL), so the entry name
  // is also its unregister path.
  for(size_t i = 0; i < SEARXNG_CMD_TABLE_N; i++)
    cmd_unregister_path(searxng_cmd_table[i].name);
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
    { .name = "bot_chat" },
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
