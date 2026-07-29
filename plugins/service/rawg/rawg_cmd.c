// botmanager — MIT
// rawg command surface: parses subcommands/flags, queries the service
// half of its own plugin, and renders colorized video-game cards,
// searches, and trending/best/new lists.
//
// RAWG_INTERNAL suppresses rawg_api.h's dlsym shims: this TU is linked
// into the same .so as rawg.c, so it calls the service entry points
// directly rather than resolving the plugin against itself.
#define RAWG_INTERNAL
#define RAWGCMD_INTERNAL
#include "rawg_cmd.h"

#include "colors.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char rawg_usage[] =
  "rawg [-v] <game> | rawg -s <query> | rawg trending [best|new] [year]";

// ----------------------------------------------------------------------
// Small formatters
// ----------------------------------------------------------------------

// Copy up to `cols` UTF-8 display columns of `src` into `dst`, never
// splitting a code point, appending "…" when truncated.
static void
rawg_fit(const char *src, int cols, char *dst, size_t sz)
{
  size_t n     = 0;
  int    w     = 0;
  bool   trunc = false;

  while(*src != '\0' && w < cols)
  {
    unsigned char c   = (unsigned char)*src;
    size_t        len = 1;

    if((c & 0xe0) == 0xc0)      len = 2;
    else if((c & 0xf0) == 0xe0) len = 3;
    else if((c & 0xf8) == 0xf0) len = 4;

    for(size_t k = 0; k < len; k++)
      if(src[k] == '\0')
      {
        len = k;
        break;
      }

    if(len == 0 || n + len + 4 > sz)   // +4 reserves room for a trailing "…"
      break;

    for(size_t k = 0; k < len; k++)
      dst[n++] = src[k];

    src += len;
    w++;
  }

  if(*src != '\0')
    trunc = true;

  dst[n] = '\0';

  if(trunc && n + 4 <= sz)
    snprintf(dst + n, sz - n, "…");
}

// Compact integer count: 11200 -> "11.2k", 1500000 -> "1.5M".
static void
rawg_fmt_count(int32_t v, char *buf, size_t sz)
{
  if(v >= 1000000)
    snprintf(buf, sz, "%.1fM", (double)v / 1e6);

  else if(v >= 1000)
    snprintf(buf, sz, "%.1fk", (double)v / 1e3);

  else
    snprintf(buf, sz, "%d", v);
}

static const char *
rawg_rating_color(double rating, int32_t ratings_count)
{
  if(ratings_count <= 0 || rating <= 0.0)
    return(CLR_GRAY);

  if(rating >= 4.0)
    return(CLR_GREEN);

  if(rating >= 3.0)
    return(CLR_YELLOW);

  return(CLR_RED);
}

// "★ 4.5/5 ██████████" over the 0–5 scale, one colour by grade, or a dim
// "★ unrated".
static void
rawg_fmt_rating(double rating, int32_t ratings_count, char *buf, size_t sz)
{
  const char *clr = rawg_rating_color(rating, ratings_count);
  char        bar[RAWG_GAUGE_CELLS * 4 + 4];
  char       *bp  = bar;
  int         filled;

  if(ratings_count <= 0 || rating <= 0.0)
  {
    snprintf(buf, sz, CLR_GRAY "★ unrated" CLR_RESET);
    return;
  }

  filled = (int)(rating / 5.0 * RAWG_GAUGE_CELLS + 0.5);

  if(filled < 0)
    filled = 0;

  if(filled > RAWG_GAUGE_CELLS)
    filled = RAWG_GAUGE_CELLS;

  for(int i = 0; i < RAWG_GAUGE_CELLS; i++)
  {
    const char *g = i < filled ? "█" : "░";

    while(*g != '\0')
      *bp++ = *g++;
  }

  *bp = '\0';

  snprintf(buf, sz, "%s★ %.1f/5 %s" CLR_RESET, clr, rating, bar);
}

// Compact rating for list rows: "★4.5" (coloured) or a dim "★ —".
static void
rawg_fmt_rating_compact(double rating, int32_t ratings_count,
    char *buf, size_t sz)
{
  if(ratings_count <= 0 || rating <= 0.0)
    snprintf(buf, sz, CLR_GRAY "★ —" CLR_RESET);

  else
    snprintf(buf, sz, "%s★%.1f" CLR_RESET,
        rawg_rating_color(rating, ratings_count), rating);
}

// Metacritic badge "MC 96" coloured by band, or "" when absent.
static void
rawg_fmt_metacritic(int32_t mc, char *buf, size_t sz)
{
  const char *clr;

  if(mc <= 0)
  {
    buf[0] = '\0';
    return;
  }

  clr = mc >= 75 ? CLR_GREEN : mc >= 50 ? CLR_YELLOW : CLR_RED;
  snprintf(buf, sz, "%sMC %d" CLR_RESET, clr, mc);
}

// ----------------------------------------------------------------------
// Error mapping
// ----------------------------------------------------------------------

static const char *
rawg_err_note(rawg_status_t st)
{
  switch(st)
  {
    case RAWG_NOT_FOUND:    return("no results");
    case RAWG_RATE_LIMITED: return(CLR_ORANGE "RAWG is rate-limiting us — try again shortly" CLR_RESET);
    case RAWG_AUTH:         return("RAWG rejected our key (check plugin.rawg.creds.apikey)");
    case RAWG_UNAVAILABLE:  return("RAWG isn't configured");
    case RAWG_OK:           return("");
    case RAWG_TRANSPORT:    return("RAWG is unreachable right now");
  }

  return("RAWG is unavailable right now");
}

// ----------------------------------------------------------------------
// Request factory — deep-copies the command context so it survives the
// async round-trip (mirrors tmdb_req_new).
// ----------------------------------------------------------------------

static rawg_req_t *
rawg_req_new(const cmd_ctx_t *ctx)
{
  rawg_req_t *r = mem_alloc(RAWGCMD_CTX, "req", sizeof(*r));

  memset(r, 0, sizeof(*r));
  r->ctx = *ctx;

  if(ctx->msg != NULL)
    r->msg = *ctx->msg;

  r->ctx.msg      = &r->msg;
  r->ctx.args     = NULL;
  r->ctx.username = NULL;
  r->ctx.parsed   = NULL;

  return(r);
}

// ----------------------------------------------------------------------
// Card renderer
// ----------------------------------------------------------------------

static void
rawg_render_game(const cmd_ctx_t *ctx, const rawg_game_t *g, bool verbose)
{
  char line[RAWGCMD_REPLY_SZ];

  // line 1 — identity
  {
    char year[48];

    if(g->year > 0)
      snprintf(year, sizeof(year), " (" CLR_YELLOW "%d" CLR_RESET ")",
          g->year);
    else if(g->tba)
      snprintf(year, sizeof(year), " (" CLR_YELLOW "TBA" CLR_RESET ")");
    else
      year[0] = '\0';

    if(g->platforms[0] != '\0')
      snprintf(line, sizeof(line),
          CLR_BOLD "%s" CLR_RESET "%s " CLR_GRAY "· %s" CLR_RESET,
          g->name, year, g->platforms);
    else
      snprintf(line, sizeof(line), CLR_BOLD "%s" CLR_RESET "%s",
          g->name, year);

    cmd_reply(ctx, line);
  }

  // line 2 — ratings + facts
  {
    char rating[128];
    char reviews[48];
    char mc[48];
    char extra[96];
    size_t pos;

    rawg_fmt_rating(g->rating, g->ratings_count, rating, sizeof(rating));

    if(g->ratings_count > 0)
    {
      char c[24];

      rawg_fmt_count(g->ratings_count, c, sizeof(c));
      snprintf(reviews, sizeof(reviews), " " CLR_GRAY "(%s)" CLR_RESET, c);
    }

    else
      reviews[0] = '\0';

    rawg_fmt_metacritic(g->metacritic, mc, sizeof(mc));

    extra[0] = '\0';
    pos      = 0;

    if(g->playtime > 0)
      pos += (size_t)snprintf(extra + pos, sizeof(extra) - pos, "  ·  ~%dh",
          g->playtime);

    if(g->esrb[0] != '\0' && pos < sizeof(extra))
      pos += (size_t)snprintf(extra + pos, sizeof(extra) - pos,
          "  ·  " CLR_GRAY "%s" CLR_RESET, g->esrb);

    snprintf(line, sizeof(line), "%s%s%s%s%s",
        rating, reviews, mc[0] != '\0' ? "  ·  " : "", mc, extra);
    cmd_reply(ctx, line);
  }

  // line 3 — genres
  if(g->genres[0] != '\0')
  {
    snprintf(line, sizeof(line), CLR_GRAY "%s" CLR_RESET, g->genres);
    cmd_reply(ctx, line);
  }

  // line 4 — description (verbose)
  if(verbose && g->description[0] != '\0')
  {
    char desc[RAWGCMD_REPLY_SZ];

    rawg_fit(g->description, 340, desc, sizeof(desc));
    cmd_reply(ctx, desc);
  }

  // line 5 — developers / publishers / stores (verbose)
  if(verbose && (g->developers[0] != '\0' || g->publishers[0] != '\0'
      || g->stores[0] != '\0'))
  {
    size_t pos = 0;

    line[0] = '\0';

    if(g->developers[0] != '\0')
      pos += (size_t)snprintf(line + pos, sizeof(line) - pos,
          CLR_GRAY "Dev" CLR_RESET " %s", g->developers);

    if(g->publishers[0] != '\0' && pos < sizeof(line))
      pos += (size_t)snprintf(line + pos, sizeof(line) - pos,
          "%s" CLR_GRAY "Pub" CLR_RESET " %s",
          pos > 0 ? "  ·  " : "", g->publishers);

    if(g->stores[0] != '\0' && pos < sizeof(line))
      pos += (size_t)snprintf(line + pos, sizeof(line) - pos,
          "%s" CLR_GRAY "on" CLR_RESET " %s",
          pos > 0 ? "  ·  " : "", g->stores);

    if(line[0] != '\0')
      cmd_reply(ctx, line);
  }

  // line 6 — tags (verbose)
  if(verbose && g->tags[0] != '\0')
  {
    char tags[RAWG_TAGS_SZ];

    rawg_fit(g->tags, 220, tags, sizeof(tags));
    snprintf(line, sizeof(line), CLR_GRAY "tags" CLR_RESET " %s", tags);
    cmd_reply(ctx, line);
  }

  // line 7 — links
  {
    size_t pos = 0;

    line[0] = '\0';

    if(g->slug[0] != '\0')
      pos += (size_t)snprintf(line + pos, sizeof(line) - pos,
          CLR_CYAN "https://rawg.io/games/%s" CLR_RESET, g->slug);

    if(g->website[0] != '\0' && pos < sizeof(line))
      pos += (size_t)snprintf(line + pos, sizeof(line) - pos,
          "%s" CLR_CYAN "%s" CLR_RESET, pos > 0 ? "  ·  " : "", g->website);

    if(verbose && g->reddit_url[0] != '\0' && pos < sizeof(line))
      pos += (size_t)snprintf(line + pos, sizeof(line) - pos,
          "%s" CLR_GRAY "%s" CLR_RESET, pos > 0 ? "  ·  " : "",
          g->reddit_url);

    if(line[0] != '\0')
      cmd_reply(ctx, line);
  }

  // line 8 — key artwork (verbose). RAWG_URL_SZ is 256 and the links
  // line above can already carry three URLs into a 640-byte reply, so
  // this gets its own line rather than a fourth append that would
  // routinely truncate. RAWG serves this on the detail payload — no
  // extra request.
  if(verbose && g->background_image[0] != '\0')
  {
    snprintf(line, sizeof(line), CLR_GRAY "Artwork: " CLR_RESET
        CLR_CYAN "%s" CLR_RESET, g->background_image);
    cmd_reply(ctx, line);
  }
}

static void
rawg_render_list(const cmd_ctx_t *ctx, const rawg_req_t *r,
    const rawg_search_res_t *res)
{
  char line[RAWGCMD_REPLY_SZ];
  uint8_t max = res->n < RAWG_LIST_MAX ? res->n : RAWG_LIST_MAX;

  if(r->mode == RAWG_MODE_TRENDING)
  {
    const char *what = r->list_kind == RAWG_LIST_BEST ? "best reviewed"
                     : r->list_kind == RAWG_LIST_NEW  ? "new releases"
                     : "popular";
    char yr[24];

    if(r->year > 0)
      snprintf(yr, sizeof(yr), " · %d", r->year);
    else
      yr[0] = '\0';

    snprintf(line, sizeof(line),
        CLR_BOLD "Trending games" CLR_RESET " " CLR_GRAY "▸ %s%s" CLR_RESET,
        what, yr);
  }

  else
    snprintf(line, sizeof(line), CLR_BOLD "Matches for '%s'" CLR_RESET,
        r->query);

  cmd_reply(ctx, line);

  for(uint8_t i = 0; i < max; i++)
  {
    const rawg_hit_t *h = &res->hits[i];
    char when[32];
    char rating[64];
    char mc[48];
    char plat[RAWG_PLATFORMS_SZ + 24];

    if(r->list_kind == RAWG_LIST_NEW && h->released[0] != '\0')
      snprintf(when, sizeof(when), " (" CLR_YELLOW "%s" CLR_RESET ")",
          h->released);
    else if(h->year > 0)
      snprintf(when, sizeof(when), " (" CLR_YELLOW "%d" CLR_RESET ")",
          h->year);
    else
      when[0] = '\0';

    rawg_fmt_rating_compact(h->rating, h->ratings_count, rating,
        sizeof(rating));
    rawg_fmt_metacritic(h->metacritic, mc, sizeof(mc));

    if(h->platforms[0] != '\0')
      snprintf(plat, sizeof(plat), " " CLR_GRAY "[%s]" CLR_RESET,
          h->platforms);
    else
      plat[0] = '\0';

    snprintf(line, sizeof(line),
        CLR_GRAY "%2d." CLR_RESET " " CLR_BOLD "%s" CLR_RESET
        "%s %s%s%s%s",
        i + 1, h->name, when, rating,
        mc[0] != '\0' ? " " : "", mc, plat);
    cmd_reply(ctx, line);
  }
}

// ----------------------------------------------------------------------
// Async completion callbacks (fire on the curl worker thread)
// ----------------------------------------------------------------------

static void
rawg_on_game(const rawg_game_res_t *res, void *user)
{
  rawg_req_t *r   = (rawg_req_t *)user;
  cmd_ctx_t   ctx = r->ctx;

  ctx.msg = &r->msg;

  if(res->status == RAWG_OK)
    rawg_render_game(&ctx, &res->game, r->verbose);
  else
    cmd_reply(&ctx, rawg_err_note(res->status));

  mem_free(r);
}

static void
rawg_on_list(const rawg_search_res_t *res, void *user)
{
  rawg_req_t *r   = (rawg_req_t *)user;
  cmd_ctx_t   ctx = r->ctx;
  char        line[RAWGCMD_REPLY_SZ];

  ctx.msg = &r->msg;

  if(res->status != RAWG_OK)
    cmd_reply(&ctx, rawg_err_note(res->status));

  else if(res->n == 0)
  {
    if(r->mode == RAWG_MODE_TRENDING)
      cmd_reply(&ctx, "Nothing to list right now — try again shortly.");
    else
    {
      snprintf(line, sizeof(line), "No matches for '%s'.", r->query);
      cmd_reply(&ctx, line);
    }
  }

  else
    rawg_render_list(&ctx, r, res);

  mem_free(r);
}

// Default lookup: prefer an exact (case-insensitive) name match, else the
// highest-`added` (most popular) hit, then fetch its detail.
static void
rawg_on_pick(const rawg_search_res_t *res, void *user)
{
  rawg_req_t       *r    = (rawg_req_t *)user;
  cmd_ctx_t         ctx  = r->ctx;
  const rawg_hit_t *best = NULL;
  const rawg_hit_t *exact = NULL;
  char              line[RAWGCMD_REPLY_SZ];

  ctx.msg = &r->msg;

  if(res->status != RAWG_OK)
  {
    cmd_reply(&ctx, rawg_err_note(res->status));
    mem_free(r);
    return;
  }

  for(uint8_t i = 0; i < res->n; i++)
  {
    const rawg_hit_t *h = &res->hits[i];

    if(exact == NULL && strcasecmp(h->name, r->query) == 0)
      exact = h;

    if(best == NULL || h->added > best->added)
      best = h;
  }

  if(exact != NULL)
    best = exact;

  if(best == NULL)
  {
    snprintf(line, sizeof(line), "No matches for '%s'.", r->query);
    cmd_reply(&ctx, line);
    mem_free(r);
    return;
  }

  if(rawg_game_async(best->id, rawg_on_game, r) != SUCCESS)
  {
    cmd_reply(&ctx, "Couldn't load details — try again shortly.");
    mem_free(r);
  }
}

// ----------------------------------------------------------------------
// Argument parsing
// ----------------------------------------------------------------------

// Reconstruct the free-text query from the first non-flag token onward.
// `first` is the current strtok token; `rest` is strtok_r's saveptr (the
// untouched remainder of the line).
static void
rawg_query_from(const char *first, const char *rest, char *out, size_t sz)
{
  size_t l;

  snprintf(out, sz, "%s", first);

  if(rest == NULL)
    return;

  while(*rest == ' ' || *rest == '\t')
    rest++;

  if(*rest == '\0')
    return;

  l = strlen(out);

  if(l < sz)
    snprintf(out + l, sz - l, " %s", rest);
}

// A 4-digit token in [1970, 2099] → a calendar year, else 0.
static int32_t
rawg_year_token(const char *tok)
{
  int y = 0;

  if(strlen(tok) != 4)
    return(0);

  for(int i = 0; i < 4; i++)
  {
    if(tok[i] < '0' || tok[i] > '9')
      return(0);

    y = y * 10 + (tok[i] - '0');
  }

  if(y < 1970 || y > 2099)
    return(0);

  return((int32_t)y);
}

static void
rawg_parse(const char *args, rawg_args_t *out)
{
  char  buf[RAWGCMD_QUERY_SZ * 2];
  char *save = NULL;
  char *tok;

  memset(out, 0, sizeof(*out));
  out->mode      = RAWG_MODE_LOOKUP;
  out->list_kind = RAWG_LIST_POPULAR;

  if(args == NULL || args[0] == '\0')
    return;

  snprintf(buf, sizeof(buf), "%s", args);
  tok = strtok_r(buf, " \t", &save);

  while(tok != NULL)
  {
    if(strcmp(tok, "-v") == 0 || strcasecmp(tok, "--verbose") == 0)
    {
      out->verbose = true;
      tok = strtok_r(NULL, " \t", &save);
      continue;
    }

    if(strcmp(tok, "-s") == 0 || strcasecmp(tok, "--search") == 0)
    {
      out->mode = RAWG_MODE_LIST;
      tok = strtok_r(NULL, " \t", &save);
      continue;
    }

    if(strcasecmp(tok, "trending") == 0 || strcasecmp(tok, "popular") == 0
        || strcasecmp(tok, "best") == 0 || strcasecmp(tok, "top") == 0
        || strcasecmp(tok, "new") == 0 || strcasecmp(tok, "latest") == 0)
    {
      out->mode = RAWG_MODE_TRENDING;

      if(strcasecmp(tok, "best") == 0 || strcasecmp(tok, "top") == 0)
        out->list_kind = RAWG_LIST_BEST;
      else if(strcasecmp(tok, "new") == 0 || strcasecmp(tok, "latest") == 0)
        out->list_kind = RAWG_LIST_NEW;
      else
        out->list_kind = RAWG_LIST_POPULAR;

      for(tok = strtok_r(NULL, " \t", &save); tok != NULL;
          tok = strtok_r(NULL, " \t", &save))
      {
        int32_t yr = rawg_year_token(tok);

        if(yr > 0)
          out->year = yr;
        else if(strcasecmp(tok, "best") == 0 || strcasecmp(tok, "top") == 0)
          out->list_kind = RAWG_LIST_BEST;
        else if(strcasecmp(tok, "new") == 0 || strcasecmp(tok, "latest") == 0)
          out->list_kind = RAWG_LIST_NEW;
        else if(strcasecmp(tok, "popular") == 0)
          out->list_kind = RAWG_LIST_POPULAR;
      }

      return;
    }

    // First free-text token: this plus the remainder is the query.
    rawg_query_from(tok, save, out->query, sizeof(out->query));
    return;
  }
}

// ----------------------------------------------------------------------
// Command callback
// ----------------------------------------------------------------------

static void
rawg_cmd(const cmd_ctx_t *ctx)
{
  rawg_args_t  a;
  rawg_req_t  *r;
  bool         ok;

  if(ctx->args == NULL || ctx->args[0] == '\0')
  {
    cmd_reply(ctx, rawg_usage);
    return;
  }

  rawg_parse(ctx->args, &a);

  if(!rawg_configured())
  {
    cmd_reply(ctx, "RAWG isn't set up yet — the operator needs to set "
        "plugin.rawg.creds.apikey.");
    return;
  }

  if(a.mode == RAWG_MODE_TRENDING)
  {
    r = rawg_req_new(ctx);
    r->mode      = a.mode;
    r->list_kind = a.list_kind;
    r->year      = a.year;
    r->verbose   = a.verbose;

    if(rawg_list_async(a.list_kind, a.year, rawg_on_list, r) != SUCCESS)
    {
      cmd_reply(ctx, "Couldn't reach RAWG — try again shortly.");
      mem_free(r);
    }

    return;
  }

  if(a.query[0] == '\0')
  {
    cmd_reply(ctx, "Search for what? e.g. !rawg elden ring");
    return;
  }

  r = rawg_req_new(ctx);
  r->mode    = a.mode;
  r->verbose = a.verbose;
  snprintf(r->query, sizeof(r->query), "%s", a.query);

  if(a.mode == RAWG_MODE_LIST)
    ok = rawg_search_async(a.query, rawg_on_list, r) == SUCCESS;
  else
    ok = rawg_search_async(a.query, rawg_on_pick, r) == SUCCESS;

  if(!ok)
  {
    cmd_reply(ctx, "Couldn't reach RAWG — try again shortly.");
    mem_free(r);
  }
}

// ----------------------------------------------------------------------
// NL hints
// ----------------------------------------------------------------------

static const cmd_nl_slot_t rawg_nl_slots[] = {
  { .name  = "title",
    .type  = CMD_NL_ARG_TOPIC,
    .flags = CMD_NL_SLOT_REQUIRED },
};

static const cmd_nl_example_t rawg_nl_examples[] = {
  { .utterance  = "how good is elden ring",
    .invocation = "/rawg elden ring" },
  { .utterance  = "what games are popular right now",
    .invocation = "/rawg trending" },
  { .utterance  = "best rated games this year",
    .invocation = "/rawg best" },
};

static const cmd_nl_t rawg_nl = {
  .when          = "User asks about a video game — its rating, release, "
                   "platforms, developer — or what games are popular / "
                   "trending.",
  .syntax        = "/rawg <game> | /rawg -s <query> | "
                   "/rawg trending [best|new]",
  .slots         = rawg_nl_slots,
  .slot_count    = (uint8_t)(sizeof(rawg_nl_slots) / sizeof(rawg_nl_slots[0])),
  .examples      = rawg_nl_examples,
  .example_count = (uint8_t)(sizeof(rawg_nl_examples)
                             / sizeof(rawg_nl_examples[0])),
};

// ----------------------------------------------------------------------
// Registration — driven by the service half's plugin lifecycle
// ----------------------------------------------------------------------

// The registry records RAWG_CTX as the providing module: after the merge
// the rawg plugin itself owns this command, and `/show commands` should
// say so.
bool
rawg_cmd_register(void)
{
  if(cmd_register(RAWG_CTX, "rawg", rawg_usage,
      "Video-game info, search, and what's trending — from the RAWG "
      "database (rawg.io)",
      NULL,
      "everyone", 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      rawg_cmd, NULL, NULL, "game",
      NULL, 0, NULL, &rawg_nl) != SUCCESS)
    return(FAIL);

  clam(CLAM_INFO, RAWGCMD_CTX, "!rawg registered");
  return(SUCCESS);
}

void
rawg_cmd_unregister(void)
{
  cmd_unregister_path("rawg");
  clam(CLAM_INFO, RAWGCMD_CTX, "!rawg unregistered");
}
