// botmanager — MIT
// tmdb command surface: parses subcommands/flags, queries the service
// half of its own plugin, and renders colorized movie / TV / actor
// cards, searches, and trending lists.
//
// TMDB_INTERNAL suppresses tmdb_api.h's dlsym shims: this TU is linked
// into the same .so as tmdb.c, so it calls the service entry points
// directly rather than resolving the plugin against itself.
#define TMDB_INTERNAL
#define TMDBCMD_INTERNAL
#include "tmdb_cmd.h"

#include "colors.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static const char tmdb_usage[] =
  "tmdb [-v] <title> | tmdb {movie|tv|actor} <name> | "
  "tmdb trending [movies|tv] [day|week] | tmdb -s <query>";

// ----------------------------------------------------------------------
// Small formatters
// ----------------------------------------------------------------------

// Copy up to `cols` UTF-8 display columns of `src` into `dst`, never
// splitting a code point, appending "…" when truncated.
static void
tmdb_fit(const char *src, int cols, char *dst, size_t sz)
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
tmdb_fmt_count(int32_t v, char *buf, size_t sz)
{
  if(v >= 1000000)
    snprintf(buf, sz, "%.1fM", (double)v / 1e6);

  else if(v >= 1000)
    snprintf(buf, sz, "%.1fk", (double)v / 1e3);

  else
    snprintf(buf, sz, "%d", v);
}

// "2h46m" / "45m" / "" when unknown.
static void
tmdb_fmt_runtime(int32_t min, char *buf, size_t sz)
{
  if(min <= 0)
    buf[0] = '\0';

  else if(min >= 60)
    snprintf(buf, sz, "%dh%dm", min / 60, min % 60);

  else
    snprintf(buf, sz, "%dm", min);
}

// "$120M" / "$1.2B" / "" when unknown.
static void
tmdb_fmt_money(int64_t v, char *buf, size_t sz)
{
  if(v <= 0)
    buf[0] = '\0';

  else if(v >= 1000000000LL)
    snprintf(buf, sz, "$%.1fB", (double)v / 1e9);

  else if(v >= 1000000LL)
    snprintf(buf, sz, "$%.0fM", (double)v / 1e6);

  else
    snprintf(buf, sz, "$%lld", (long long)v);
}

static const char *
tmdb_media_label(tmdb_media_t m)
{
  switch(m)
  {
    case TMDB_MEDIA_MOVIE:  return("movie");
    case TMDB_MEDIA_TV:     return("TV");
    case TMDB_MEDIA_PERSON: return("person");
    default:                return("");
  }
}

static const char *
tmdb_rating_color(double rating, int32_t votes)
{
  if(votes <= 0 || rating <= 0.0)
    return(CLR_GRAY);

  if(rating >= 7.5)
    return(CLR_GREEN);

  if(rating >= 5.0)
    return(CLR_YELLOW);

  return(CLR_RED);
}

// "★ 8.2 ██████████" — one colour by grade, or a dim "★ unrated".
static void
tmdb_fmt_rating(double rating, int32_t votes, char *buf, size_t sz)
{
  const char *clr = tmdb_rating_color(rating, votes);
  char        bar[TMDB_GAUGE_CELLS * 4 + 4];
  char       *bp  = bar;
  int         filled;

  if(votes <= 0 || rating <= 0.0)
  {
    snprintf(buf, sz, CLR_GRAY "★ unrated" CLR_RESET);
    return;
  }

  filled = (int)(rating / 10.0 * TMDB_GAUGE_CELLS + 0.5);

  if(filled < 0)
    filled = 0;

  if(filled > TMDB_GAUGE_CELLS)
    filled = TMDB_GAUGE_CELLS;

  for(int i = 0; i < TMDB_GAUGE_CELLS; i++)
  {
    const char *g = i < filled ? "█" : "░";

    while(*g != '\0')
      *bp++ = *g++;
  }

  *bp = '\0';

  snprintf(buf, sz, "%s★ %.1f %s" CLR_RESET, clr, rating, bar);
}

// Compact rating for list rows: "★8.2" (coloured) or a dim "★ —".
static void
tmdb_fmt_rating_compact(double rating, int32_t votes, char *buf, size_t sz)
{
  if(votes <= 0 || rating <= 0.0)
    snprintf(buf, sz, CLR_GRAY "★ —" CLR_RESET);

  else
    snprintf(buf, sz, "%s★%.1f" CLR_RESET,
        tmdb_rating_color(rating, votes), rating);
}

// Age in years from an ISO birthday, evaluated at deathday (if any) else
// today. Returns -1 when the birthday is unknown/malformed.
static int
tmdb_age(const char *birth, const char *death)
{
  int by = 0, bm = 1, bd = 1;
  int ey = 0, em = 1, ed = 1;
  int age;

  if(birth == NULL || sscanf(birth, "%d-%d-%d", &by, &bm, &bd) < 1 || by == 0)
    return(-1);

  if(death != NULL && death[0] != '\0'
      && sscanf(death, "%d-%d-%d", &ey, &em, &ed) >= 1 && ey != 0)
  {
    // evaluated at date of death
  }

  else
  {
    time_t    now = time(NULL);
    struct tm tmv;

    gmtime_r(&now, &tmv);
    ey = tmv.tm_year + 1900;
    em = tmv.tm_mon + 1;
    ed = tmv.tm_mday;
  }

  age = ey - by;

  if(em < bm || (em == bm && ed < bd))
    age--;

  return(age);
}

// ----------------------------------------------------------------------
// Error mapping
// ----------------------------------------------------------------------

static const char *
tmdb_err_note(tmdb_status_t st)
{
  switch(st)
  {
    case TMDB_NOT_FOUND:    return("no results");
    case TMDB_RATE_LIMITED: return("TMDB is rate-limiting us — try again shortly");
    case TMDB_AUTH:         return("TMDB rejected our token (check plugin.tmdb.creds.apikey)");
    case TMDB_UNAVAILABLE:  return("TMDB isn't configured");
    case TMDB_OK:           return("");
    case TMDB_TRANSPORT:    return("TMDB is unreachable right now");
  }

  return("TMDB is unavailable right now");
}

// ----------------------------------------------------------------------
// Request factory — deep-copies the command context so it survives the
// async round-trip (mirrors stock_req_new).
// ----------------------------------------------------------------------

static tmdb_req_t *
tmdb_req_new(const cmd_ctx_t *ctx)
{
  tmdb_req_t *r = mem_alloc(TMDBCMD_CTX, "req", sizeof(*r));

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
// Card renderers
// ----------------------------------------------------------------------

static void
tmdb_render_title(const cmd_ctx_t *ctx, const tmdb_title_t *t, bool verbose)
{
  char line[TMDBCMD_REPLY_SZ];
  bool tv = t->media == TMDB_MEDIA_TV;

  // line 1 — identity
  {
    char meta[96];

    if(tv && t->status[0] != '\0')
      snprintf(meta, sizeof(meta), "TV · %s", t->status);
    else
      snprintf(meta, sizeof(meta), "%s", tmdb_media_label(t->media));

    if(t->year > 0)
      snprintf(line, sizeof(line),
          CLR_BOLD "%s" CLR_RESET " (" CLR_YELLOW "%d" CLR_RESET ") "
          CLR_GRAY "· %s" CLR_RESET,
          t->title, t->year, meta);
    else
      snprintf(line, sizeof(line),
          CLR_BOLD "%s" CLR_RESET " " CLR_GRAY "· %s" CLR_RESET,
          t->title, meta);

    cmd_reply(ctx, line);
  }

  // line 2 — rating + facts
  {
    char rating[128];
    char votes[48];
    char facts[192];
    char rt[24];

    tmdb_fmt_rating(t->rating, t->votes, rating, sizeof(rating));

    if(t->votes > 0)
    {
      char c[24];

      tmdb_fmt_count(t->votes, c, sizeof(c));
      snprintf(votes, sizeof(votes), " " CLR_GRAY "(%s)" CLR_RESET, c);
    }

    else
      votes[0] = '\0';

    facts[0] = '\0';

    if(t->genres[0] != '\0')
      snprintf(facts, sizeof(facts), "  ·  %s", t->genres);

    if(tv)
    {
      size_t l = strlen(facts);

      if(t->seasons > 0)
        snprintf(facts + l, sizeof(facts) - l, "  ·  %d season%s",
            t->seasons, t->seasons == 1 ? "" : "s");
    }

    else
    {
      tmdb_fmt_runtime(t->runtime, rt, sizeof(rt));

      if(rt[0] != '\0')
      {
        size_t l = strlen(facts);

        snprintf(facts + l, sizeof(facts) - l, "  ·  %s", rt);
      }
    }

    snprintf(line, sizeof(line), "%s%s%s", rating, votes, facts);
    cmd_reply(ctx, line);
  }

  // line 3 — tagline (verbose only, if present)
  if(verbose && t->tagline[0] != '\0')
  {
    char tag[TMDB_TAGLINE_SZ];

    tmdb_fit(t->tagline, 180, tag, sizeof(tag));
    snprintf(line, sizeof(line), CLR_GRAY "“%s”" CLR_RESET, tag);
    cmd_reply(ctx, line);
  }

  // line 4 — overview
  if(t->overview[0] != '\0')
  {
    char ov[TMDBCMD_REPLY_SZ];

    tmdb_fit(t->overview, verbose ? 340 : 240, ov, sizeof(ov));
    cmd_reply(ctx, ov);
  }

  // line 5 — verbose money / air dates
  if(verbose)
  {
    if(tv)
    {
      if(t->networks[0] != '\0' || t->release_date[0] != '\0')
      {
        char aired[256];

        if(t->release_date[0] != '\0' && t->last_air_date[0] != '\0')
          snprintf(aired, sizeof(aired), CLR_GRAY "Aired" CLR_RESET " %s – %s",
              t->release_date, t->last_air_date);
        else if(t->release_date[0] != '\0')
          snprintf(aired, sizeof(aired), CLR_GRAY "Aired" CLR_RESET " %s",
              t->release_date);
        else
          aired[0] = '\0';

        if(t->episodes > 0)
        {
          size_t l = strlen(aired);

          snprintf(aired + l, sizeof(aired) - l, "%s%d episodes",
              aired[0] != '\0' ? "  ·  " : "", t->episodes);
        }

        if(t->networks[0] != '\0')
        {
          size_t l = strlen(aired);

          snprintf(aired + l, sizeof(aired) - l, "%s" CLR_GRAY "on"
              CLR_RESET " %s", aired[0] != '\0' ? "  ·  " : "", t->networks);
        }

        if(aired[0] != '\0')
          cmd_reply(ctx, aired);
      }
    }

    else if(t->budget > 0 || t->revenue > 0)
    {
      char bud[32];
      char box[32];

      tmdb_fmt_money(t->budget, bud, sizeof(bud));
      tmdb_fmt_money(t->revenue, box, sizeof(box));

      if(bud[0] != '\0' && box[0] != '\0')
        snprintf(line, sizeof(line),
            CLR_GRAY "Budget" CLR_RESET " %s   " CLR_GRAY "Box office"
            CLR_RESET " %s", bud, box);
      else
        snprintf(line, sizeof(line), CLR_GRAY "%s" CLR_RESET " %s",
            bud[0] != '\0' ? "Budget" : "Box office",
            bud[0] != '\0' ? bud : box);

      cmd_reply(ctx, line);
    }
  }

  // line 6 — credits
  if(t->director[0] != '\0' || t->cast[0] != '\0')
  {
    const char *dlabel = tv ? "Creator" : "Dir";

    if(t->director[0] != '\0' && t->cast[0] != '\0')
      snprintf(line, sizeof(line),
          CLR_GRAY "%s" CLR_RESET " %s " CLR_GRAY "·" CLR_RESET " %s",
          dlabel, t->director, t->cast);
    else if(t->director[0] != '\0')
      snprintf(line, sizeof(line), CLR_GRAY "%s" CLR_RESET " %s",
          dlabel, t->director);
    else
      snprintf(line, sizeof(line), "%s", t->cast);

    cmd_reply(ctx, line);
  }

  // line 7 — links
  {
    size_t pos = 0;

    line[0] = '\0';

    if(t->imdb_id[0] != '\0')
      pos += (size_t)snprintf(line + pos, sizeof(line) - pos,
          CLR_CYAN "https://www.imdb.com/title/%s" CLR_RESET, t->imdb_id);

    if(t->trailer_key[0] != '\0' && pos < sizeof(line))
      pos += (size_t)snprintf(line + pos, sizeof(line) - pos,
          "%s" CLR_CYAN "▶ https://youtu.be/%s" CLR_RESET,
          pos > 0 ? "  ·  " : "", t->trailer_key);

    if(verbose && t->id > 0 && pos < sizeof(line))
      pos += (size_t)snprintf(line + pos, sizeof(line) - pos,
          "%s" CLR_GRAY "https://www.themoviedb.org/%s/%d" CLR_RESET,
          pos > 0 ? "  ·  " : "",
          tv ? "tv" : "movie", t->id);

    if(line[0] != '\0')
      cmd_reply(ctx, line);
  }
}

static void
tmdb_render_person(const cmd_ctx_t *ctx, const tmdb_person_t *p, bool verbose)
{
  char line[TMDBCMD_REPLY_SZ];
  int  age = tmdb_age(p->birthday, p->deathday);

  // line 1 — identity
  {
    char meta[128];
    size_t pos = 0;

    meta[0] = '\0';

    if(p->known_for_dept[0] != '\0')
      pos += (size_t)snprintf(meta + pos, sizeof(meta) - pos, " · %s",
          p->known_for_dept);

    if(p->deathday[0] != '\0')
    {
      pos += (size_t)snprintf(meta + pos, sizeof(meta) - pos, " · %s–%s",
          p->birthday[0] != '\0' ? p->birthday : "?", p->deathday);

      if(age >= 0)
        pos += (size_t)snprintf(meta + pos, sizeof(meta) - pos,
            " (aged %d)", age);
    }

    else if(p->birthday[0] != '\0')
    {
      pos += (size_t)snprintf(meta + pos, sizeof(meta) - pos, " · b. %s",
          p->birthday);

      if(age >= 0)
        pos += (size_t)snprintf(meta + pos, sizeof(meta) - pos, " (%d)", age);
    }

    snprintf(line, sizeof(line),
        CLR_BOLD "%s" CLR_RESET CLR_GRAY "%s" CLR_RESET, p->name, meta);
    cmd_reply(ctx, line);
  }

  // line 2 — known for
  if(p->known_for[0] != '\0')
  {
    char kf[TMDB_KNOWNFOR_SZ];

    tmdb_fit(p->known_for, 300, kf, sizeof(kf));
    snprintf(line, sizeof(line),
        CLR_GRAY "★ known for" CLR_RESET " %s", kf);
    cmd_reply(ctx, line);
  }

  // line 3 — biography (verbose)
  if(verbose && p->biography[0] != '\0')
  {
    char bio[TMDBCMD_REPLY_SZ];

    tmdb_fit(p->biography, 340, bio, sizeof(bio));
    cmd_reply(ctx, bio);
  }

  // line 4 — birthplace + links (verbose)
  if(verbose)
  {
    size_t pos = 0;

    line[0] = '\0';

    if(p->place_of_birth[0] != '\0')
      pos += (size_t)snprintf(line + pos, sizeof(line) - pos,
          CLR_GRAY "from" CLR_RESET " %s", p->place_of_birth);

    if(p->imdb_id[0] != '\0' && pos < sizeof(line))
      pos += (size_t)snprintf(line + pos, sizeof(line) - pos,
          "%s" CLR_CYAN "https://www.imdb.com/name/%s" CLR_RESET,
          pos > 0 ? "  ·  " : "", p->imdb_id);

    if(p->id > 0 && pos < sizeof(line))
      pos += (size_t)snprintf(line + pos, sizeof(line) - pos,
          "%s" CLR_GRAY "https://www.themoviedb.org/person/%d" CLR_RESET,
          pos > 0 ? "  ·  " : "", p->id);

    if(line[0] != '\0')
      cmd_reply(ctx, line);
  }
}

static void
tmdb_render_list(const cmd_ctx_t *ctx, const tmdb_req_t *r,
    const tmdb_search_res_t *res)
{
  char line[TMDBCMD_REPLY_SZ];
  uint8_t max = res->n < TMDB_LIST_MAX ? res->n : TMDB_LIST_MAX;

  if(r->mode == TMDB_MODE_TRENDING)
  {
    const char *what = r->forced == TMDB_MEDIA_MOVIE ? "Movies"
                     : r->forced == TMDB_MEDIA_TV    ? "TV"
                     : "titles";

    snprintf(line, sizeof(line),
        CLR_BOLD "Trending %s" CLR_RESET " " CLR_GRAY "▸ %s" CLR_RESET,
        what, r->weekly ? "this week" : "today");
  }

  else
    snprintf(line, sizeof(line), CLR_BOLD "Matches for '%s'" CLR_RESET,
        r->query);

  cmd_reply(ctx, line);

  for(uint8_t i = 0; i < max; i++)
  {
    const tmdb_hit_t *h = &res->hits[i];
    char year[32];
    char rating[64];
    char label[80];

    if(h->year > 0)
      snprintf(year, sizeof(year), " (" CLR_YELLOW "%d" CLR_RESET ")", h->year);
    else
      year[0] = '\0';

    if(h->media == TMDB_MEDIA_PERSON)
    {
      rating[0] = '\0';

      if(h->known_for_dept[0] != '\0')
        snprintf(label, sizeof(label), " " CLR_GRAY "[person · %s]" CLR_RESET,
            h->known_for_dept);
      else
        snprintf(label, sizeof(label), " " CLR_GRAY "[person]" CLR_RESET);
    }

    else
    {
      char rc[48];

      tmdb_fmt_rating_compact(h->rating, h->rating > 0 ? 1 : 0, rc, sizeof(rc));
      snprintf(rating, sizeof(rating), " %s", rc);
      snprintf(label, sizeof(label), " " CLR_GRAY "[%s]" CLR_RESET,
          tmdb_media_label(h->media));
    }

    snprintf(line, sizeof(line),
        CLR_GRAY "%2d." CLR_RESET " " CLR_BOLD "%s" CLR_RESET "%s%s%s",
        i + 1, h->title, year, rating, label);
    cmd_reply(ctx, line);
  }
}

// ----------------------------------------------------------------------
// Async completion callbacks (fire on the curl worker thread)
// ----------------------------------------------------------------------

static void
tmdb_on_title(const tmdb_title_res_t *res, void *user)
{
  tmdb_req_t *r   = (tmdb_req_t *)user;
  cmd_ctx_t   ctx = r->ctx;

  ctx.msg = &r->msg;

  if(res->status == TMDB_OK)
    tmdb_render_title(&ctx, &res->title, r->verbose);
  else
    cmd_reply(&ctx, tmdb_err_note(res->status));

  mem_free(r);
}

static void
tmdb_on_person(const tmdb_person_res_t *res, void *user)
{
  tmdb_req_t *r   = (tmdb_req_t *)user;
  cmd_ctx_t   ctx = r->ctx;

  ctx.msg = &r->msg;

  if(res->status == TMDB_OK)
    tmdb_render_person(&ctx, &res->person, r->verbose);
  else
    cmd_reply(&ctx, tmdb_err_note(res->status));

  mem_free(r);
}

static void
tmdb_on_list(const tmdb_search_res_t *res, void *user)
{
  tmdb_req_t *r   = (tmdb_req_t *)user;
  cmd_ctx_t   ctx = r->ctx;
  char        line[TMDBCMD_REPLY_SZ];

  ctx.msg = &r->msg;

  if(res->status != TMDB_OK)
    cmd_reply(&ctx, tmdb_err_note(res->status));

  else if(res->n == 0)
  {
    if(r->mode == TMDB_MODE_TRENDING)
      cmd_reply(&ctx, "Nothing trending right now — try again shortly.");
    else
    {
      snprintf(line, sizeof(line), "No matches for '%s'.", r->query);
      cmd_reply(&ctx, line);
    }
  }

  else
    tmdb_render_list(&ctx, r, res);

  mem_free(r);
}

// Default lookup: choose the highest-popularity hit, then fetch its detail.
static void
tmdb_on_pick(const tmdb_search_res_t *res, void *user)
{
  tmdb_req_t       *r   = (tmdb_req_t *)user;
  cmd_ctx_t         ctx = r->ctx;
  const tmdb_hit_t *best = NULL;
  char              line[TMDBCMD_REPLY_SZ];

  ctx.msg = &r->msg;

  if(res->status != TMDB_OK)
  {
    cmd_reply(&ctx, tmdb_err_note(res->status));
    mem_free(r);
    return;
  }

  for(uint8_t i = 0; i < res->n; i++)
  {
    const tmdb_hit_t *h = &res->hits[i];

    if(h->media != TMDB_MEDIA_MOVIE && h->media != TMDB_MEDIA_TV
        && h->media != TMDB_MEDIA_PERSON)
      continue;

    if(best == NULL || h->popularity > best->popularity)
      best = h;
  }

  if(best == NULL)
  {
    snprintf(line, sizeof(line), "No matches for '%s'.", r->query);
    cmd_reply(&ctx, line);
    mem_free(r);
    return;
  }

  if(best->media == TMDB_MEDIA_PERSON)
  {
    if(tmdb_person_async(best->id, tmdb_on_person, r) != SUCCESS)
    {
      cmd_reply(&ctx, "Couldn't load details — try again shortly.");
      mem_free(r);
    }
  }

  else
  {
    if(tmdb_title_async(best->media, best->id, tmdb_on_title, r) != SUCCESS)
    {
      cmd_reply(&ctx, "Couldn't load details — try again shortly.");
      mem_free(r);
    }
  }
}

// ----------------------------------------------------------------------
// Argument parsing
// ----------------------------------------------------------------------

// Reconstruct the free-text query from the first non-flag token onward.
// `first` is the current strtok token; `rest` is strtok_r's saveptr (the
// untouched remainder of the line).
static void
tmdb_query_from(const char *first, const char *rest, char *out, size_t sz)
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

static void
tmdb_parse(const char *args, tmdb_args_t *out)
{
  char  buf[TMDBCMD_QUERY_SZ * 2];
  char *save = NULL;
  char *tok;
  bool  have_sub = false;

  memset(out, 0, sizeof(*out));
  out->mode   = TMDB_MODE_LOOKUP;
  out->forced = TMDB_MEDIA_UNKNOWN;

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
      out->mode = TMDB_MODE_LIST;
      tok = strtok_r(NULL, " \t", &save);
      continue;
    }

    if(!have_sub && (strcasecmp(tok, "movie") == 0
        || strcasecmp(tok, "film") == 0))
    {
      out->forced = TMDB_MEDIA_MOVIE;
      have_sub    = true;
      tok = strtok_r(NULL, " \t", &save);
      continue;
    }

    if(!have_sub && (strcasecmp(tok, "tv") == 0
        || strcasecmp(tok, "show") == 0 || strcasecmp(tok, "series") == 0))
    {
      out->forced = TMDB_MEDIA_TV;
      have_sub    = true;
      tok = strtok_r(NULL, " \t", &save);
      continue;
    }

    if(!have_sub && (strcasecmp(tok, "actor") == 0
        || strcasecmp(tok, "person") == 0 || strcasecmp(tok, "cast") == 0))
    {
      out->forced = TMDB_MEDIA_PERSON;
      have_sub    = true;
      tok = strtok_r(NULL, " \t", &save);
      continue;
    }

    if(!have_sub && (strcasecmp(tok, "trending") == 0
        || strcasecmp(tok, "trend") == 0))
    {
      out->mode = TMDB_MODE_TRENDING;

      for(tok = strtok_r(NULL, " \t", &save); tok != NULL;
          tok = strtok_r(NULL, " \t", &save))
      {
        if(strcasecmp(tok, "movies") == 0 || strcasecmp(tok, "movie") == 0
            || strcasecmp(tok, "films") == 0)
          out->forced = TMDB_MEDIA_MOVIE;
        else if(strcasecmp(tok, "tv") == 0 || strcasecmp(tok, "shows") == 0)
          out->forced = TMDB_MEDIA_TV;
        else if(strcasecmp(tok, "all") == 0)
          out->forced = TMDB_MEDIA_UNKNOWN;
        else if(strcasecmp(tok, "week") == 0 || strcasecmp(tok, "weekly") == 0)
          out->weekly = true;
        else if(strcasecmp(tok, "day") == 0 || strcasecmp(tok, "today") == 0)
          out->weekly = false;
      }

      return;
    }

    // First free-text token: this plus the remainder is the query.
    tmdb_query_from(tok, save, out->query, sizeof(out->query));
    return;
  }
}

// ----------------------------------------------------------------------
// Command callback
// ----------------------------------------------------------------------

static void
tmdb_cmd(const cmd_ctx_t *ctx)
{
  tmdb_args_t  a;
  tmdb_req_t  *r;
  bool         ok;

  if(ctx->args == NULL || ctx->args[0] == '\0')
  {
    cmd_reply(ctx, tmdb_usage);
    return;
  }

  tmdb_parse(ctx->args, &a);

  if(!tmdb_configured())
  {
    cmd_reply(ctx, "TMDB isn't set up yet — the operator needs to set "
        "plugin.tmdb.creds.apikey.");
    return;
  }

  if(a.mode == TMDB_MODE_TRENDING)
  {
    r = tmdb_req_new(ctx);
    r->mode    = a.mode;
    r->forced  = a.forced;
    r->weekly  = a.weekly;
    r->verbose = a.verbose;

    if(tmdb_trending_async(a.forced, a.weekly, tmdb_on_list, r) != SUCCESS)
    {
      cmd_reply(ctx, "Couldn't reach TMDB — try again shortly.");
      mem_free(r);
    }

    return;
  }

  if(a.query[0] == '\0')
  {
    cmd_reply(ctx, "Search for what? e.g. !tmdb dune part two");
    return;
  }

  r = tmdb_req_new(ctx);
  r->mode    = a.mode;
  r->forced  = a.forced;
  r->verbose = a.verbose;
  snprintf(r->query, sizeof(r->query), "%s", a.query);

  if(a.mode == TMDB_MODE_LIST)
    ok = tmdb_search_async(a.forced, a.query, tmdb_on_list, r) == SUCCESS;
  else
    ok = tmdb_search_async(a.forced, a.query, tmdb_on_pick, r) == SUCCESS;

  if(!ok)
  {
    cmd_reply(ctx, "Couldn't reach TMDB — try again shortly.");
    mem_free(r);
  }
}

// ----------------------------------------------------------------------
// NL hints
// ----------------------------------------------------------------------

static const cmd_nl_slot_t tmdb_nl_slots[] = {
  { .name  = "title",
    .type  = CMD_NL_ARG_TOPIC,
    .flags = CMD_NL_SLOT_REQUIRED },
};

static const cmd_nl_example_t tmdb_nl_examples[] = {
  { .utterance  = "what's the movie dune part two about",
    .invocation = "/tmdb dune part two" },
  { .utterance  = "tell me about the actor zendaya",
    .invocation = "/tmdb actor zendaya" },
  { .utterance  = "what tv shows are trending this week",
    .invocation = "/tmdb trending tv week" },
};

static const cmd_nl_t tmdb_nl = {
  .when          = "User asks about a movie, TV show, or actor, or what's "
                   "trending on The Movie Database.",
  .syntax        = "/tmdb <title> | /tmdb actor <name> | "
                   "/tmdb trending [movies|tv]",
  .slots         = tmdb_nl_slots,
  .slot_count    = (uint8_t)(sizeof(tmdb_nl_slots) / sizeof(tmdb_nl_slots[0])),
  .examples      = tmdb_nl_examples,
  .example_count = (uint8_t)(sizeof(tmdb_nl_examples)
                             / sizeof(tmdb_nl_examples[0])),
};

// ----------------------------------------------------------------------
// Registration — driven by the service half's plugin lifecycle
// ----------------------------------------------------------------------

// The registry records TMDB_CTX as the providing module: after the merge
// the tmdb plugin itself owns this command, and `/show commands` should
// say so.
bool
tmdb_cmd_register(void)
{
  if(cmd_register(TMDB_CTX, "tmdb", tmdb_usage,
      "Movie, TV, and actor info from The Movie Database (themoviedb.org)",
      NULL,
      "everyone", 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      tmdb_cmd, NULL, NULL, NULL,
      NULL, 0, NULL, &tmdb_nl) != SUCCESS)
    return(FAIL);

  clam(CLAM_INFO, TMDBCMD_CTX, "!tmdb registered");
  return(SUCCESS);
}

void
tmdb_cmd_unregister(void)
{
  cmd_unregister("tmdb");
  clam(CLAM_INFO, TMDBCMD_CTX, "!tmdb unregistered");
}
