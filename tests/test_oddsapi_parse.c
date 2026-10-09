// botmanager — MIT
// Cases for the oddsapi parse layer, where every failure is a plausible
// wrong number: a median taken across ±100 in American space, a side
// read by position from a book that lists away first, or a "best" price
// found at a different point all reach a channel as a line nobody can
// tell is wrong.
//
// Every row drives a distinct code path rather than a distinct input.
// The expected prices were computed outside this code, from the
// fixtures in tests/oddsapi/.
#include "test.h"

#define ODDSAPI_INTERNAL
#include "oddsapi.h"

#define OUT_SZ  1024
#define BODY_SZ (1024 * 1024)   // a live league pull runs ~150 KB

static char body[BODY_SZ];

// The fixture's bytes in `body`, or 0 when it cannot be read whole — a
// truncated reply would fail to parse and read as a skip, not a failure.
static size_t
fixture(const char *name)
{
  char   path[512];
  FILE  *f;
  size_t n;

  snprintf(path, sizeof(path), "%s/%s", ODDSAPI_FIXTURE_DIR, name);

  if((f = fopen(path, "r")) == NULL)
    return(0);

  n = fread(body, 1, sizeof(body) - 1, f);
  fclose(f);
  body[n] = '\0';
  return(n < sizeof(body) - 1 ? n : 0);
}

// A price is signed; test_check_sz would print a miss as 2^64 - 115.
static void
check_px(const char *table, const char *name, int32_t want, int32_t got)
{
  char w[16], g[16];

  snprintf(w, sizeof(w), "%d", want);
  snprintf(g, sizeof(g), "%d", got);
  test_check_str(table, name, w, g);
}

// ----------------------------------------------------------------------
// Flatten a result so one string comparison covers the whole of it.
// ----------------------------------------------------------------------

static void
render_line(const oddsapi_line_t *l, char *out, size_t cap)
{
  snprintf(out, cap, "%s ml %d/%d/%d spr %g %d/%d tot %g %d/%d", l->key,
      l->ml_away, l->ml_home, l->ml_draw, (double)l->spr_home_pt,
      l->spr_home_px, l->spr_away_px, (double)l->tot_pt, l->over_px,
      l->under_px);
}

static void
render_consensus(const oddsapi_consensus_t *c, char *out, size_t cap)
{
  snprintf(out, cap, "ml %d/%d/%d spr %g %d/%d tot %g %d/%d n %u%u%u%u%u%u%u",
      c->px[ODDSAPI_COL_ML_AWAY], c->px[ODDSAPI_COL_ML_HOME],
      c->px[ODDSAPI_COL_ML_DRAW], (double)c->spr_home_pt,
      c->px[ODDSAPI_COL_SPR_HOME], c->px[ODDSAPI_COL_SPR_AWAY],
      (double)c->tot_pt, c->px[ODDSAPI_COL_OVER], c->px[ODDSAPI_COL_UNDER],
      c->n[0], c->n[1], c->n[2], c->n[3], c->n[4], c->n[5], c->n[6]);
}

static void
render_best(const oddsapi_best_t *b, char *out, size_t cap)
{
  size_t pos = 0;

  out[0] = '\0';

  for(int col = 0; col < ODDSAPI_COL_COUNT && pos < cap; col++)
    pos += (size_t)snprintf(out + pos, cap - pos, "%s%d@%u",
        col > 0 ? " " : "", b[col].px, b[col].book);
}

static void
render_futures(const oddsapi_futures_t *f, char *out, size_t cap)
{
  size_t pos = 0;

  out[0] = '\0';

  for(uint32_t i = 0; i < f->n && pos < cap; i++)
  {
    oddsapi_best_t best;
    int32_t        med;

    oddsapi_contender_consensus(&f->contenders[i], &med, &best);
    pos += (size_t)snprintf(out + pos, cap - pos, "%s%s %d best %d@%s",
        i > 0 ? "; " : "", f->contenders[i].name, med, best.px,
        f->book_key[best.book]);
  }
}

// ----------------------------------------------------------------------
// Tables
// ----------------------------------------------------------------------

static const struct
{
  int32_t     am;
  const char *dec;
} price_cases[] = {
  { -500, "1.2000" }, { -200, "1.5000" }, { -110, "1.9091" },
  {  100, "2.0000" }, {  105, "2.0500" }, {  150, "2.5000" },
  { 1200, "13.0000" },
};

static const struct
{
  const char *name;
  double      dec;
  int32_t     want;
} dec_cases[] = {
  { "evens is +100, never 0", 2.0, 100 },
  { "a decimal of 1 pays nothing and has no price", 1.0, 0 },
  { "below 1 is not a price", 0.5, 0 },
  { "past the ceiling is not a price", 20000.0, 0 },
};

static const struct
{
  const char *name;
  const char *doc;
  bool        ok;
  uint32_t    n;
} shape_cases[] = {
  { "an error object is not a slate", "{\"message\":\"Unknown sport\"}",
    false, 0 },
  { "an empty reply is an empty slate", "[]", true, 0 },
  { "an element without teams is skipped, not stored empty",
    "[5,{\"id\":\"x\",\"commence_time\":1}]", true, 0 },
};

// The slate sorts by start time, so the game listed first is last here.
static const struct
{
  const char *name;
  uint32_t    game;
  uint8_t     book;
  const char *want;
} line_cases[] = {
  { "outcomes listed away-first still land on their side", 2, 1,
    "fanduel ml 118/-138/0 spr -2.5 -108/-112 tot 47.5 -112/-108" },
  { "a book missing a market keeps the others", 0, 5,
    "betrivers ml -300/245/0 spr 0 0/0 tot 42.5 -110/-110" },
  { "a moneyline-only book reads absent spread and total", 1, 1,
    "fanduel ml 100/-120/0 spr 0 0/0 tot 0 0/0" },
};

static const struct
{
  const char *name;
  uint32_t    game;
  const char *consensus;
  const char *best;
} game_cases[] = {
  { "first game: a spread tie the median breaks", 0,
    "ml -300/245/0 spr 7 -112/-107 tot 42.5 -110/-110 n 6602211",
    "-290@2 250@1 0@0 -110@3 -105@2 -110@5 -110@5" },
  { "second game: a moneyline straddling ±100 is +100, not 0", 1,
    "ml 100/-120/0 spr -1 -110/-110 tot 0 0/0 n 3302200",
    "105@2 -115@0 0@0 -110@0 -110@0 0@0 0@0" },
  { "third game: an even count, a tie both ways, an off-point book", 2,
    "ml 114/-133/0 spr -2.5 -109/-111 tot 47.5 -110/-110 n 4402233",
    "118@1 -130@2 0@0 -108@1 -110@0 -108@3 -108@1" },
};

static const struct
{
  const char *name;
  const char *word;
  const char *want;
} league_cases[] = {
  { "an alias, any case", "NfL", "americanfootball_nfl" },
  { "the second alias", "cfb", "americanfootball_ncaaf" },
  { "a raw sport key passes through", "basketball_wnba", "basketball_wnba" },
  { "a sport key is not case-folded", "BASKETBALL_WNBA", "" },
  { "a team is not a league", "chiefs", "" },
};

static const struct
{
  const char *name;
  const char *team;
  const char *query;
  bool        want;
} team_cases[] = {
  { "a nickname, any case", "Kansas City Chiefs", "CHIEFS", true },
  { "a city two teams share matches both", "New York Jets", "new york",
    true },
  { "two letters match too much to mean anything", "Kansas City Chiefs",
    "ch", false },
  { "a miss", "Buffalo Bills", "giants", false },
};

int
main(void)
{
  char        out[OUT_SZ];
  mem_stats_t before, after;

  mem_init();
  mem_get_stats(&before);

  for(size_t i = 0; i < sizeof(price_cases) / sizeof(price_cases[0]); i++)
  {
    char name[32];

    snprintf(name, sizeof(name), "%+d", price_cases[i].am);
    snprintf(out, sizeof(out), "%.4f", oddsapi_am_to_dec(price_cases[i].am));
    test_check_str("am->dec", name, price_cases[i].dec, out);
    check_px("am->dec->am", name, price_cases[i].am,
        oddsapi_dec_to_am(oddsapi_am_to_dec(price_cases[i].am)));
  }

  for(size_t i = 0; i < sizeof(dec_cases) / sizeof(dec_cases[0]); i++)
    check_px("dec->am", dec_cases[i].name, dec_cases[i].want,
        oddsapi_dec_to_am(dec_cases[i].dec));

  for(size_t i = 0; i < sizeof(shape_cases) / sizeof(shape_cases[0]); i++)
  {
    oddsapi_slate_t s;
    bool            ok;

    ok = oddsapi_parse_odds(shape_cases[i].doc, strlen(shape_cases[i].doc), &s);
    test_check_bool("shape", shape_cases[i].name, shape_cases[i].ok, ok);
    test_check_sz("shape", shape_cases[i].name, shape_cases[i].n, s.ngames);
    test_check_bool("shape", "no games, no array", true, s.games == NULL);
    oddsapi_slate_clear(&s);
  }

  // The NFL slate: sort order, side matching, book dropping, and the
  // consensus and best price of each game.
  {
    oddsapi_slate_t s;
    size_t          n = fixture("odds_nfl.json");

    if(n == 0 || !oddsapi_parse_odds(body, n, &s))
      return(test_skip("oddsapi_parse", "odds_nfl.json would not parse"));

    snprintf(out, sizeof(out), "%s %s %u: %s %u, %s %u, %s %u", s.sport_key,
        s.sport_title, s.ngames, s.games[0].id, s.games[0].nbooks,
        s.games[1].id, s.games[1].nbooks, s.games[2].id, s.games[2].nbooks);
    test_check_str("slate", "sorted by start, books counted after dropping",
        "americanfootball_nfl NFL 3: g_sf_sea 6, g_dal_phi 3, g_kc_buf 4",
        out);

    for(size_t i = 0; i < sizeof(line_cases) / sizeof(line_cases[0]); i++)
    {
      render_line(&s.games[line_cases[i].game].books[line_cases[i].book], out,
          sizeof(out));
      test_check_str("line", line_cases[i].name, line_cases[i].want, out);
    }

    for(size_t i = 0; i < sizeof(game_cases) / sizeof(game_cases[0]); i++)
    {
      oddsapi_consensus_t c;
      oddsapi_best_t      b[ODDSAPI_COL_COUNT];

      oddsapi_consensus(&s.games[game_cases[i].game], &c);
      render_consensus(&c, out, sizeof(out));
      test_check_str("consensus", game_cases[i].name, game_cases[i].consensus,
          out);

      oddsapi_best(&s.games[game_cases[i].game], &c, b);
      render_best(b, out, sizeof(out));
      test_check_str("best", game_cases[i].name, game_cases[i].best, out);
    }

    oddsapi_slate_clear(&s);
  }

  // A three-way market: the draw is its own column, matched by name.
  {
    oddsapi_slate_t     s;
    oddsapi_consensus_t c;
    size_t              n = fixture("odds_soccer.json");

    if(n == 0 || !oddsapi_parse_odds(body, n, &s))
      return(test_skip("oddsapi_parse", "odds_soccer.json would not parse"));

    oddsapi_consensus(&s.games[0], &c);
    render_consensus(&c, out, sizeof(out));
    test_check_str("consensus", "a draw is priced when the sport has one",
        "ml 315/-117/265 spr 0 0/0 tot 0 0/0 n 2220000", out);
    oddsapi_slate_clear(&s);
  }

  {
    oddsapi_events_t e;
    size_t           n = fixture("events.json");

    if(n == 0 || !oddsapi_parse_events(body, n, &e))
      return(test_skip("oddsapi_parse", "events.json would not parse"));

    snprintf(out, sizeof(out), "%u: %s, %s", e.n, e.events[0].id,
        e.n > 1 ? e.events[1].id : "");
    test_check_str("events", "sorted, and an event missing a team is gone",
        "2: e_nyj_mia, e_nyg_dal", out);
    oddsapi_events_clear(&e);
  }

  {
    oddsapi_scores_t s;
    size_t           n = fixture("scores.json");
    size_t           pos = 0;

    if(n == 0 || !oddsapi_parse_scores(body, n, &s))
      return(test_skip("oddsapi_parse", "scores.json would not parse"));

    for(uint32_t i = 0; i < s.n; i++)
      pos += (size_t)snprintf(out + pos, sizeof(out) - pos,
          "%s%s c%d s%d %s-%s @%lld", i > 0 ? "; " : "", s.scores[i].id,
          s.scores[i].completed, s.scores[i].has_scores, s.scores[i].away_score,
          s.scores[i].home_score, (long long)s.scores[i].last_update);

    test_check_str("scores",
        "scores land by name; null scores and a null update are absent",
        "s_final c1 s1 10-24 @1760117000; s_live c0 s1 21-17 @1760199000; "
        "s_next c0 s0 - @0", out);
    oddsapi_scores_clear(&s);
  }

  {
    oddsapi_sports_t s;
    size_t           n = fixture("sports.json");
    size_t           pos = 0;

    if(n == 0 || !oddsapi_parse_sports(body, n, &s))
      return(test_skip("oddsapi_parse", "sports.json would not parse"));

    for(uint32_t i = 0; i < s.n; i++)
      pos += (size_t)snprintf(out + pos, sizeof(out) - pos, "%s%s a%d o%d",
          i > 0 ? "; " : "", s.sports[i].key, s.sports[i].active,
          s.sports[i].has_outrights);

    test_check_str("sports", "active and outright flags survive",
        "americanfootball_nfl a1 o0; americanfootball_nfl_super_bowl_winner "
        "a1 o1; basketball_wnba a0 o0", out);
    oddsapi_sports_clear(&s);
  }

  // Futures: the book table counts only books that quoted an outright,
  // and a long shot past int16_t survives.
  {
    oddsapi_futures_t f;
    size_t            n = fixture("futures.json");

    if(n == 0 || !oddsapi_parse_futures(body, n, &f))
      return(test_skip("oddsapi_parse", "futures.json would not parse"));

    test_check_sz("futures", "a book with no outright is not in the table",
        3, f.nbooks);

    oddsapi_futures_sort(&f);
    render_futures(&f, out, sizeof(out));
    test_check_str("futures", "favourite first, medians in decimal space",
        "Kansas City Chiefs 475 best 500@fanduel; "
        "Buffalo Bills 600 best 650@fanduel; "
        "Detroit Lions 750 best 800@fanduel; "
        "Carolina Panthers 50000 best 50000@fanduel", out);
    oddsapi_futures_clear(&f);
  }

  // Replies recorded from the live API. Their contents are whatever the
  // day held, so the cases assert what must hold of any reply: every
  // row survives, both sides of every market land, nothing is priced 0.
  {
    oddsapi_sports_t sp;
    oddsapi_events_t ev;
    oddsapi_slate_t  sl;
    size_t           n;
    uint32_t         outrights = 0, books = 0, unsided = 0, unpriced = 0;
    bool             sorted = true;

    if((n = fixture("live_sports.json")) == 0
        || !oddsapi_parse_sports(body, n, &sp))
      return(test_skip("oddsapi_parse", "live_sports.json would not parse"));

    for(uint32_t i = 0; i < sp.n; i++)
      outrights += sp.sports[i].has_outrights;

    test_check_sz("live", "every sport survives", 180, sp.n);
    test_check_sz("live", "outright markets are flagged", 12, outrights);
    oddsapi_sports_clear(&sp);

    if((n = fixture("live_events.json")) == 0
        || !oddsapi_parse_events(body, n, &ev))
      return(test_skip("oddsapi_parse", "live_events.json would not parse"));

    test_check_sz("live", "every event survives", 26, ev.n);
    oddsapi_events_clear(&ev);

    if((n = fixture("live_odds_nfl.json")) == 0
        || !oddsapi_parse_odds(body, n, &sl))
      return(test_skip("oddsapi_parse", "live_odds_nfl.json would not parse"));

    for(uint32_t g = 0; g < sl.ngames; g++)
    {
      oddsapi_consensus_t c;

      if(g > 0 && sl.games[g].commence < sl.games[g - 1].commence)
        sorted = false;

      for(uint8_t b = 0; b < sl.games[g].nbooks; b++)
      {
        const oddsapi_line_t *l = &sl.games[g].books[b];

        books++;
        unsided += l->ml_home == 0 || l->ml_away == 0
            || l->spr_home_px == 0 || l->spr_away_px == 0;
        unpriced += l->over_px == 0 || l->under_px == 0;
      }

      oddsapi_consensus(&sl.games[g], &c);
      unpriced += c.px[ODDSAPI_COL_ML_HOME] == 0
          || c.px[ODDSAPI_COL_SPR_HOME] == 0 || c.px[ODDSAPI_COL_OVER] == 0;
    }

    test_check_sz("live", "every game survives", 26, sl.ngames);
    test_check_sz("live", "every book survives", 120, books);
    test_check_bool("live", "the slate is in start order", true, sorted);
    test_check_sz("live", "both sides of every market land by name", 0,
        unsided);
    test_check_sz("live", "nothing a book quoted reads as absent", 0,
        unpriced);
    oddsapi_slate_clear(&sl);
  }

  // A contender nobody prices cannot come out of the parser, so the
  // sort's last rule is driven by hand.
  {
    oddsapi_contender_t c[2] = {
      { .name = "Unpriced" },
      { .name = "Priced", .n = 1, .px = { 2000 } },
    };
    oddsapi_futures_t f = { .n = 2, .contenders = c };

    oddsapi_futures_sort(&f);
    test_check_str("futures", "an unpriced contender sorts last", "Priced",
        c[0].name);
  }

  for(size_t i = 0; i < sizeof(league_cases) / sizeof(league_cases[0]); i++)
  {
    const oddsapi_league_t *l = oddsapi_league_find(league_cases[i].word);

    test_check_str("league", league_cases[i].name, league_cases[i].want,
        l != NULL ? l->sport_key : "");
  }

  test_check_bool("league", "the API lists no EPL futures market", true,
      oddsapi_league_find("epl")->outright_key == NULL);

  for(size_t i = 0; i < sizeof(team_cases) / sizeof(team_cases[0]); i++)
    test_check_bool("team", team_cases[i].name, team_cases[i].want,
        oddsapi_team_match(team_cases[i].team, team_cases[i].query));

  mem_get_stats(&after);
  test_check_sz("memory", "every _clear gave back what its parse took",
      before.active, after.active);

  return(test_report("oddsapi_parse"));
}
