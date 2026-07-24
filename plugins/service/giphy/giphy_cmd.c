// botmanager — MIT
// giphy command surface: the one public !giphy command. Searches by
// phrase, pulls a random GIF for a tag (-r), or lists what is trending
// (-t), and renders the hits as colorized one-liners — or, with -v, as a
// metadata card per GIF.
//
// GIPHY_INTERNAL suppresses giphy_api.h's dlsym shims: this TU is linked
// into the same .so as giphy.c, so it calls the service entry points
// directly rather than resolving the plugin against itself.
#define GIPHY_INTERNAL
#define GIPHY_CMD_INTERNAL
#include "giphy_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Per-line body cap for %.*s slots. Leaves room for the widest leading
// label we emit plus the terminating NUL.
#define GIPHY_CMD_LINE_BODY  ((int)(GIPHY_CMD_REPLY_SZ - 24))

// Split of that budget for the brief line, which carries a title and a
// URL side by side. Sums (plus the colour markers) to under the line.
#define GIPHY_CMD_BRIEF_TITLE  180
#define GIPHY_CMD_BRIEF_URL    380

// ----------------------------------------------------------------------
// Formatting helpers
// ----------------------------------------------------------------------

// Giphy's audience ratings, graded the way a viewer reads them: green is
// safe anywhere, red wants a warning. An unrated GIF stays dim.
static const char *
giphy_rating_color(const char *rating)
{
  if(rating == NULL || rating[0] == '\0')
    return(CLR_GRAY);

  if(strcasecmp(rating, "g") == 0 || strcasecmp(rating, "pg") == 0)
    return(CLR_GREEN);

  if(strcasecmp(rating, "pg-13") == 0)
    return(CLR_YELLOW);

  return(CLR_RED);
}

static void
giphy_fmt_size(uint32_t bytes, char *buf, size_t sz)
{
  if(bytes == 0)
    buf[0] = '\0';

  else if(bytes >= 1024 * 1024)
    snprintf(buf, sz, "%.1fMB", (double)bytes / (1024.0 * 1024.0));

  else
    snprintf(buf, sz, "%.0fKB", (double)bytes / 1024.0);
}

// Giphy titles carry a trailing " GIF by <studio>" tail that is noise
// next to the uploader field we already print. Trim it, and fall back to
// a placeholder when the GIF was uploaded untitled.
static const char *
giphy_title_of(const giphy_result_t *g, char *buf, size_t sz)
{
  char *tail;

  if(g->title[0] == '\0')
    return("(untitled)");

  snprintf(buf, sz, "%s", g->title);
  tail = strstr(buf, " GIF by ");

  if(tail != NULL)
    *tail = '\0';

  return(buf[0] != '\0' ? buf : "(untitled)");
}

// One compact line: bold title, then the permalink.
static void
giphy_emit_brief(cmd_ctx_t *ctx, const giphy_result_t *g)
{
  char line [GIPHY_CMD_REPLY_SZ];
  char title[GIPHY_TITLE_SZ];

  snprintf(line, sizeof(line), CLR_BOLD "%.*s" CLR_RESET " %.*s",
      GIPHY_CMD_BRIEF_TITLE, giphy_title_of(g, title, sizeof(title)),
      GIPHY_CMD_BRIEF_URL, g->media_url);
  cmd_reply(ctx, line);
}

// A metadata card: numbered title, permalink, dimensions / weight /
// rating / attribution, and the Giphy landing page.
static void
giphy_emit_card(cmd_ctx_t *ctx, const giphy_result_t *g, size_t n)
{
  char line [GIPHY_CMD_REPLY_SZ];
  char title[GIPHY_TITLE_SZ];
  char size [16];
  char dims [32];

  snprintf(line, sizeof(line), CLR_BOLD "%zu. %s" CLR_RESET, n,
      giphy_title_of(g, title, sizeof(title)));
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line), "   " CLR_CYAN "%.*s" CLR_RESET,
      GIPHY_CMD_LINE_BODY, g->media_url);
  cmd_reply(ctx, line);

  giphy_fmt_size(g->size_bytes, size, sizeof(size));

  if(g->width_px > 0 && g->height_px > 0)
    snprintf(dims, sizeof(dims), "%ux%u", g->width_px, g->height_px);

  else
    dims[0] = '\0';

  snprintf(line, sizeof(line), "   %s%s" CLR_RESET "%s%s%s%s%s%s%s%s",
      giphy_rating_color(g->rating),
      g->rating[0] != '\0' ? g->rating : "unrated",
      dims[0] != '\0' ? " · " : "", dims,
      size[0] != '\0' ? " · " : "", size,
      g->username[0] != '\0' ? " · by " : "", g->username,
      g->source[0] != '\0' ? " · " : "", g->source);
  cmd_reply(ctx, line);

  if(g->page_url[0] != '\0')
  {
    snprintf(line, sizeof(line), "   " CLR_GRAY "%.*s" CLR_RESET,
        GIPHY_CMD_LINE_BODY, g->page_url);
    cmd_reply(ctx, line);
  }
}

// ----------------------------------------------------------------------
// Async completion
// ----------------------------------------------------------------------

static void
giphy_cmd_done(const giphy_response_t *resp)
{
  giphy_cmd_req_t *r   = (giphy_cmd_req_t *)resp->user_data;
  cmd_ctx_t        ctx = r->ctx;
  char             line[GIPHY_CMD_REPLY_SZ];

  ctx.msg = &r->msg;

  if(resp->status == GIPHY_NOT_FOUND || resp->n_results == 0)
  {
    if(r->query[0] != '\0')
      snprintf(line, sizeof(line), "giphy: nothing found for \"%s\"",
          r->query);

    else
      snprintf(line, sizeof(line), "giphy: nothing found");

    cmd_reply(&ctx, line);
    mem_free(r);
    return;
  }

  if(resp->status != GIPHY_OK)
  {
    snprintf(line, sizeof(line), "giphy: %s",
        giphy_status_str(resp->status));
    cmd_reply(&ctx, line);
    mem_free(r);
    return;
  }

  for(size_t i = 0; i < resp->n_results; i++)
  {
    if(r->verbose)
      giphy_emit_card(&ctx, &resp->results[i], i + 1);

    else
      giphy_emit_brief(&ctx, &resp->results[i]);
  }

  mem_free(r);
}

// ----------------------------------------------------------------------
// Argument parsing
// ----------------------------------------------------------------------

// Append one whitespace-separated token to the query buffer, inserting a
// single space before every token but the first. Silently drops tokens
// that would overflow — a truncated query beats a clobbered buffer.
static void
giphy_query_append(char *query, size_t cap, size_t *len, const char *tok)
{
  int wrote;

  if(*len + 1 >= cap)
    return;

  wrote = snprintf(query + *len, cap - *len, "%s%s",
      *len > 0 ? " " : "", tok);

  if(wrote > 0)
    *len += (size_t)wrote < cap - *len ? (size_t)wrote : cap - *len - 1;
}

// Pull the leading flags out of the raw argument string, leaving the
// residual phrase in a->query. Each flag is consumed on its first
// occurrence anywhere in the token stream:
//   -r          random GIF for the tag
//   -t          trending list (query ignored)
//   -v          verbose cards
//   -n <count>  request a specific number of results
// A "-n" whose successor is not a base-10 number degrades to two
// ordinary query words, so searching for a literal "-n foo" still works.
static void
giphy_parse_args(const char *args, giphy_cmd_args_t *a)
{
  char   scratch[METHOD_TEXT_SZ];
  char  *save;
  size_t qlen = 0;

  memset(a, 0, sizeof(*a));
  a->mode = GIPHY_MODE_SEARCH;

  if(args == NULL)
    return;

  snprintf(scratch, sizeof(scratch), "%s", args);

  for(char *tok = strtok_r(scratch, " \t", &save); tok != NULL;
      tok = strtok_r(NULL, " \t", &save))
  {
    if(a->mode == GIPHY_MODE_SEARCH && strcmp(tok, "-r") == 0)
    {
      a->mode = GIPHY_MODE_RANDOM;
      continue;
    }

    if(a->mode == GIPHY_MODE_SEARCH && strcmp(tok, "-t") == 0)
    {
      a->mode = GIPHY_MODE_TRENDING;
      continue;
    }

    if(!a->verbose && strcmp(tok, "-v") == 0)
    {
      a->verbose = true;
      continue;
    }

    if(a->count == 0 && strcmp(tok, "-n") == 0)
    {
      char         *num = strtok_r(NULL, " \t", &save);
      char         *end;
      unsigned long v;

      if(num == NULL)
        continue;                  // trailing "-n" with no value: drop

      v = strtoul(num, &end, 10);

      if(end != num && *end == '\0' && v > 0)
      {
        a->count = (uint32_t)v;
        continue;
      }

      // Not a count — keep both tokens as literal query words.
      giphy_query_append(a->query, sizeof(a->query), &qlen, tok);
      giphy_query_append(a->query, sizeof(a->query), &qlen, num);
      continue;
    }

    giphy_query_append(a->query, sizeof(a->query), &qlen, tok);
  }
}

// ----------------------------------------------------------------------
// Command body
// ----------------------------------------------------------------------

#define GIPHY_CMD_USAGE "giphy [-r] [-t] [-v] [-n <count>] <search terms>"

static void
giphy_cmd(const cmd_ctx_t *ctx)
{
  giphy_cmd_args_t a;
  giphy_cmd_req_t *r;

  giphy_parse_args(ctx->args, &a);

  // Trending needs no phrase; everything else does.
  if(a.query[0] == '\0' && a.mode != GIPHY_MODE_TRENDING)
  {
    cmd_reply(ctx, "Usage: " GIPHY_CMD_USAGE);
    return;
  }

  if(!giphy_configured())
  {
    cmd_reply(ctx, "giphy: no API key configured "
        "(operator: set plugin.giphy.creds.apikey)");
    return;
  }

  r = mem_alloc(GIPHY_CMD_CTX, "req", sizeof(*r));
  memset(r, 0, sizeof(*r));
  r->ctx     = *ctx;
  r->verbose = a.verbose;
  snprintf(r->query, sizeof(r->query), "%s", a.query);

  if(ctx->msg != NULL)
    r->msg = *ctx->msg;

  r->ctx.msg      = &r->msg;
  r->ctx.args     = NULL;
  r->ctx.username = NULL;
  r->ctx.parsed   = NULL;
  r->ctx.data     = NULL;

  if(giphy_fetch(a.mode, a.query, a.count, giphy_cmd_done, r) != SUCCESS)
  {
    cmd_reply(ctx, "giphy: failed to submit query");
    mem_free(r);
  }
}

// ----------------------------------------------------------------------
// Natural-language hints
// ----------------------------------------------------------------------

static const cmd_nl_slot_t giphy_nl_slots[] = {
  { .name = "query", .type = CMD_NL_ARG_TOPIC,
    .flags = CMD_NL_SLOT_REMAINDER },
};

static const cmd_nl_example_t giphy_nl_examples[] = {
  { .utterance  = "post a gif of a cat falling off a table",
    .invocation = "/giphy cat falling off a table" },
  { .utterance  = "give me a random dancing gif",
    .invocation = "/giphy -r dancing" },
  { .utterance  = "what gifs are trending right now",
    .invocation = "/giphy -t" },
};

static const cmd_nl_t giphy_nl = {
  .when          = "User asks for a GIF, an animated reaction, or what is "
                   "trending on Giphy.",
  .syntax        = "/giphy <search terms> | /giphy -r <tag> | /giphy -t",
  .slots         = giphy_nl_slots,
  .slot_count    = (uint8_t)(sizeof(giphy_nl_slots)
                             / sizeof(giphy_nl_slots[0])),
  .examples      = giphy_nl_examples,
  .example_count = (uint8_t)(sizeof(giphy_nl_examples)
                             / sizeof(giphy_nl_examples[0])),
};

// ----------------------------------------------------------------------
// Registration — driven by the service half's plugin lifecycle
// ----------------------------------------------------------------------

static const char giphy_help[] =
    "Find animated GIFs on Giphy.\n"
    "\n"
    "  !giphy <search terms>   best matches for a phrase\n"
    "  !giphy -r <tag>         one random GIF for a tag\n"
    "  !giphy -t               what is trending right now\n"
    "\n"
    "Flags:\n"
    "  -n <count>   number of results (capped at "
        "plugin.giphy.max_results)\n"
    "  -v           verbose cards: rating, dimensions, weight,\n"
    "               uploader, and the Giphy page link\n"
    "\n"
    "Results are filtered to plugin.giphy.rating and below; set\n"
    "that KV to \"none\"/\"off\" for the broadest set Giphy serves\n"
    "(which excludes adult content regardless of rating).\n"
    "\n"
    "Examples:\n"
    "  !giphy shrug\n"
    "  !giphy -n 3 happy dance\n"
    "  !giphy -r -v excited\n"
    "  !giphy -t -n 5";

// The registry records GIPHY_CTX as the providing module: after the
// merge the giphy plugin itself owns this command, and `/show commands`
// should say so.
bool
giphy_cmd_register(void)
{
  if(cmd_register(GIPHY_CTX, "giphy", GIPHY_CMD_USAGE,
      "Animated GIF search via Giphy (giphy.com)",
      giphy_help,
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      giphy_cmd, NULL, NULL, "gif",
      NULL, 0, NULL, &giphy_nl) != SUCCESS)
    return(FAIL);

  clam(CLAM_INFO, GIPHY_CMD_CTX, "!giphy registered");
  return(SUCCESS);
}

void
giphy_cmd_unregister(void)
{
  cmd_unregister("giphy");
  clam(CLAM_INFO, GIPHY_CMD_CTX, "!giphy unregistered");
}
