// botmanager — MIT
// The Odds API's replies turned into this plugin's types, and a game
// priced from its books: consensus, best price, the league table.
//
// Everything here is a pure function of its arguments — no KV, no curl,
// no module state — which is what lets tests/test_oddsapi_parse.c drive
// it directly. The provider (oddsapi.c) owns the wire.
#define ODDSAPI_INTERNAL
#define ODDSAPI_PARSE_INTERNAL
#include "oddsapi.h"

// ----------------------------------------------------------------------
// Prices
// ----------------------------------------------------------------------

double
oddsapi_am_to_dec(int32_t am)
{
  if(am >= ODDSAPI_PX_MIN)
    return(1.0 + am / 100.0);

  if(am <= -ODDSAPI_PX_MIN)
    return(1.0 + 100.0 / -am);

  return(0.0);
}

// Outside the decimals that ±ODDSAPI_PX_MAX bound there is no American
// price to return, and lround() past int32_t would be undefined.
int32_t
oddsapi_dec_to_am(double dec)
{
  if(!(dec >= 1.0 + 100.0 / ODDSAPI_PX_MAX && dec <= 1.0 + ODDSAPI_PX_MAX / 100.0))
    return(0);

  if(dec >= 2.0)
    return((int32_t)lround((dec - 1.0) * 100.0));

  return((int32_t)-lround(100.0 / (dec - 1.0)));
}

// The boundary for one price: anything that is not an American price in
// range becomes absent, never a number.
static int32_t
oddsapi_price(struct json_object *outcome)
{
  double d;

  if(!json_get_double(outcome, "price", &d)
      || !(fabs(d) >= ODDSAPI_PX_MIN && fabs(d) <= ODDSAPI_PX_MAX))
    return(0);

  return((int32_t)lround(d));
}

static bool
oddsapi_point(struct json_object *outcome, float *out)
{
  double d;

  if(!json_get_double(outcome, "point", &d) || !isfinite(d) || fabs(d) > 1000.0)
    return(false);

  *out = (float)d;
  return(true);
}

// ----------------------------------------------------------------------
// Element walkers
// ----------------------------------------------------------------------

// The reply's root array and its length clamped to ODDSAPI_ROWS_MAX, or
// NULL when the body is not a JSON array. The caller json_object_puts.
static struct json_object *
oddsapi_root_array(const char *body, size_t len, size_t *n)
{
  struct json_object *root = json_parse_buf(body, len, ODDSAPI_CTX);

  if(root == NULL)
    return(NULL);

  if(!json_object_is_type(root, json_type_array))
  {
    json_object_put(root);
    return(NULL);
  }

  *n = json_object_array_length(root);

  if(*n > ODDSAPI_ROWS_MAX)
    *n = ODDSAPI_ROWS_MAX;

  return(root);
}

// The fields every game-shaped element shares. A game without both
// teams cannot have its outcomes matched to sides, so it is not a game.
static bool
oddsapi_event_head(struct json_object *ev, char *id, char *home, char *away,
    time_t *commence)
{
  int64_t t = 0;

  if(!json_get_str(ev, "id", id, ODDSAPI_ID_SZ)
      || !json_get_str(ev, "home_team", home, ODDSAPI_TEAM_SZ)
      || !json_get_str(ev, "away_team", away, ODDSAPI_TEAM_SZ)
      || !json_get_int64(ev, "commence_time", &t))
    return(false);

  *commence = (time_t)t;
  return(id[0] != '\0' && home[0] != '\0' && away[0] != '\0' && t > 0);
}

// One book's markets for one game. False when the book quoted nothing
// this plugin reads, so the caller drops it rather than store it empty.
static bool
oddsapi_line_parse(struct json_object *book, const oddsapi_game_t *g,
    oddsapi_line_t *out)
{
  struct json_object *markets = json_get_array(book, "markets");
  bool                any     = false;
  size_t              nm;

  memset(out, 0, sizeof(*out));

  if(markets == NULL)
    return(false);

  json_get_str(book, "key", out->key, sizeof(out->key));
  json_get_str(book, "title", out->title, sizeof(out->title));
  nm = json_object_array_length(markets);

  for(size_t m = 0; m < nm; m++)
  {
    struct json_object *mk   = json_object_array_get_idx(markets, m);
    struct json_object *outs = json_get_array(mk, "outcomes");
    char                key[16];
    size_t              no;

    if(outs == NULL || !json_get_str(mk, "key", key, sizeof(key)))
      continue;

    no = json_object_array_length(outs);

    for(size_t o = 0; o < no; o++)
    {
      struct json_object *oc = json_object_array_get_idx(outs, o);
      char                name[ODDSAPI_TEAM_SZ];
      int32_t             px = oddsapi_price(oc);
      bool                home, away;
      float               pt;

      if(px == 0 || !json_get_str(oc, "name", name, sizeof(name)))
        continue;

      home = strcmp(name, g->home) == 0;
      away = strcmp(name, g->away) == 0;

      if(strcmp(key, "h2h") == 0)
      {
        if(home)
          out->ml_home = px;

        else if(away)
          out->ml_away = px;

        else if(strcmp(name, "Draw") == 0)
          out->ml_draw = px;

        else
          continue;
      }

      else if(strcmp(key, "spreads") == 0)
      {
        if((!home && !away) || !oddsapi_point(oc, &pt))
          continue;

        // The home side's own point wins; the away side's is its mirror
        // and stands in only until the home side arrives.
        if(home)
        {
          out->spr_home_pt = pt;
          out->spr_home_px = px;
        }

        else
        {
          out->spr_away_px = px;

          if(out->spr_home_px == 0)
            out->spr_home_pt = -pt;
        }
      }

      else if(strcmp(key, "totals") == 0)
      {
        bool over = strcmp(name, "Over") == 0;

        if((!over && strcmp(name, "Under") != 0) || !oddsapi_point(oc, &pt))
          continue;

        if(over)
        {
          out->tot_pt  = pt;
          out->over_px = px;
        }

        else
        {
          out->under_px = px;

          if(out->over_px == 0)
            out->tot_pt = pt;
        }
      }

      else
        continue;

      any = true;
    }
  }

  return(any);
}

// ----------------------------------------------------------------------
// Reply parsers
// ----------------------------------------------------------------------

static int
oddsapi_game_cmp(const void *a, const void *b)
{
  const oddsapi_game_t *ga = a;
  const oddsapi_game_t *gb = b;

  if(ga->commence != gb->commence)
    return(ga->commence < gb->commence ? -1 : 1);

  return(strcmp(ga->id, gb->id));
}

bool
oddsapi_parse_odds(const char *body, size_t len, oddsapi_slate_t *out)
{
  struct json_object *root;
  size_t              n = 0;

  memset(out, 0, sizeof(*out));

  if((root = oddsapi_root_array(body, len, &n)) == NULL)
    return(false);

  if(n > 0)
    out->games = mem_alloc(ODDSAPI_CTX, "slate", n * sizeof(*out->games));

  for(size_t i = 0; i < n; i++)
  {
    struct json_object *ev    = json_object_array_get_idx(root, i);
    struct json_object *books = json_get_array(ev, "bookmakers");
    oddsapi_game_t     *g     = &out->games[out->ngames];
    size_t              nb;

    memset(g, 0, sizeof(*g));

    if(!oddsapi_event_head(ev, g->id, g->home, g->away, &g->commence))
      continue;

    nb = books != NULL ? json_object_array_length(books) : 0;

    for(size_t b = 0; b < nb && g->nbooks < ODDSAPI_BOOKS_MAX; b++)
    {
      if(oddsapi_line_parse(json_object_array_get_idx(books, b), g,
          &g->books[g->nbooks]))
        g->nbooks++;
    }

    if(out->ngames == 0)
    {
      json_get_str(ev, "sport_key", out->sport_key, sizeof(out->sport_key));
      json_get_str(ev, "sport_title", out->sport_title,
          sizeof(out->sport_title));
    }

    out->ngames++;
  }

  json_object_put(root);

  if(out->ngames == 0)
    oddsapi_slate_clear(out);

  else
    qsort(out->games, out->ngames, sizeof(*out->games), oddsapi_game_cmp);

  return(true);
}

void
oddsapi_slate_clear(oddsapi_slate_t *s)
{
  if(s->games != NULL)
    mem_free(s->games);

  memset(s, 0, sizeof(*s));
}

static int
oddsapi_event_cmp(const void *a, const void *b)
{
  const oddsapi_event_t *ea = a;
  const oddsapi_event_t *eb = b;

  if(ea->commence != eb->commence)
    return(ea->commence < eb->commence ? -1 : 1);

  return(strcmp(ea->id, eb->id));
}

bool
oddsapi_parse_events(const char *body, size_t len, oddsapi_events_t *out)
{
  struct json_object *root;
  size_t              n = 0;

  memset(out, 0, sizeof(*out));

  if((root = oddsapi_root_array(body, len, &n)) == NULL)
    return(false);

  if(n > 0)
    out->events = mem_alloc(ODDSAPI_CTX, "events", n * sizeof(*out->events));

  for(size_t i = 0; i < n; i++)
  {
    struct json_object *ev = json_object_array_get_idx(root, i);
    oddsapi_event_t    *e  = &out->events[out->n];

    memset(e, 0, sizeof(*e));

    if(!oddsapi_event_head(ev, e->id, e->home, e->away, &e->commence))
      continue;

    if(out->n == 0)
      json_get_str(ev, "sport_key", out->sport_key, sizeof(out->sport_key));

    out->n++;
  }

  json_object_put(root);

  if(out->n == 0)
    oddsapi_events_clear(out);

  else
    qsort(out->events, out->n, sizeof(*out->events), oddsapi_event_cmp);

  return(true);
}

void
oddsapi_events_clear(oddsapi_events_t *e)
{
  if(e->events != NULL)
    mem_free(e->events);

  memset(e, 0, sizeof(*e));
}

static int
oddsapi_score_cmp(const void *a, const void *b)
{
  const oddsapi_score_t *sa = a;
  const oddsapi_score_t *sb = b;

  if(sa->commence != sb->commence)
    return(sa->commence < sb->commence ? -1 : 1);

  return(strcmp(sa->id, sb->id));
}

bool
oddsapi_parse_scores(const char *body, size_t len, oddsapi_scores_t *out)
{
  struct json_object *root;
  size_t              n = 0;

  memset(out, 0, sizeof(*out));

  if((root = oddsapi_root_array(body, len, &n)) == NULL)
    return(false);

  if(n > 0)
    out->scores = mem_alloc(ODDSAPI_CTX, "scores", n * sizeof(*out->scores));

  for(size_t i = 0; i < n; i++)
  {
    struct json_object *ev  = json_object_array_get_idx(root, i);
    struct json_object *arr = json_get_array(ev, "scores");
    oddsapi_score_t    *s   = &out->scores[out->n];
    int64_t             t   = 0;
    size_t              ns;

    memset(s, 0, sizeof(*s));

    if(!oddsapi_event_head(ev, s->id, s->home, s->away, &s->commence))
      continue;

    json_get_bool(ev, "completed", &s->completed);

    if(json_get_int64(ev, "last_update", &t))
      s->last_update = (time_t)t;

    s->has_scores = arr != NULL;
    ns            = arr != NULL ? json_object_array_length(arr) : 0;

    for(size_t k = 0; k < ns; k++)
    {
      struct json_object *sc = json_object_array_get_idx(arr, k);
      char                name[ODDSAPI_TEAM_SZ];

      if(!json_get_str(sc, "name", name, sizeof(name)))
        continue;

      if(strcmp(name, s->home) == 0)
        json_get_str(sc, "score", s->home_score, sizeof(s->home_score));

      else if(strcmp(name, s->away) == 0)
        json_get_str(sc, "score", s->away_score, sizeof(s->away_score));
    }

    if(out->n == 0)
    {
      json_get_str(ev, "sport_key", out->sport_key, sizeof(out->sport_key));
      json_get_str(ev, "sport_title", out->sport_title,
          sizeof(out->sport_title));
    }

    out->n++;
  }

  json_object_put(root);

  if(out->n == 0)
    oddsapi_scores_clear(out);

  else
    qsort(out->scores, out->n, sizeof(*out->scores), oddsapi_score_cmp);

  return(true);
}

void
oddsapi_scores_clear(oddsapi_scores_t *s)
{
  if(s->scores != NULL)
    mem_free(s->scores);

  memset(s, 0, sizeof(*s));
}

bool
oddsapi_parse_sports(const char *body, size_t len, oddsapi_sports_t *out)
{
  struct json_object *root;
  size_t              n = 0;

  memset(out, 0, sizeof(*out));

  if((root = oddsapi_root_array(body, len, &n)) == NULL)
    return(false);

  if(n > 0)
    out->sports = mem_alloc(ODDSAPI_CTX, "sports", n * sizeof(*out->sports));

  for(size_t i = 0; i < n; i++)
  {
    struct json_object *el = json_object_array_get_idx(root, i);
    oddsapi_sport_t    *s  = &out->sports[out->n];

    memset(s, 0, sizeof(*s));

    if(!json_get_str(el, "key", s->key, sizeof(s->key)) || s->key[0] == '\0')
      continue;

    json_get_str(el, "group", s->group, sizeof(s->group));
    json_get_str(el, "title", s->title, sizeof(s->title));
    json_get_bool(el, "active", &s->active);
    json_get_bool(el, "has_outrights", &s->has_outrights);
    out->n++;
  }

  json_object_put(root);

  if(out->n == 0)
    oddsapi_sports_clear(out);

  return(true);
}

void
oddsapi_sports_clear(oddsapi_sports_t *s)
{
  if(s->sports != NULL)
    mem_free(s->sports);

  memset(s, 0, sizeof(*s));
}

// One book's outright market folded into the futures: each outcome joins
// the contender of that name, or starts one while `cap` allows. A book
// quoting one contender twice keeps its first price.
static void
oddsapi_book_futures(struct json_object *book, oddsapi_futures_t *out,
    uint32_t cap)
{
  struct json_object *markets = json_get_array(book, "markets");
  char                key[ODDSAPI_BOOK_KEY_SZ] = "";
  uint8_t             bi;
  size_t              nm;

  if(markets == NULL || !json_get_str(book, "key", key, sizeof(key))
      || key[0] == '\0')
    return;

  for(bi = 0; bi < out->nbooks; bi++)
  {
    if(strcmp(out->book_key[bi], key) == 0)
      break;
  }

  if(bi == ODDSAPI_BOOKS_MAX)
    return;

  nm = json_object_array_length(markets);

  for(size_t m = 0; m < nm; m++)
  {
    struct json_object *mk   = json_object_array_get_idx(markets, m);
    struct json_object *outs = json_get_array(mk, "outcomes");
    char                mkey[16];
    size_t              no;

    if(outs == NULL || !json_get_str(mk, "key", mkey, sizeof(mkey))
        || strcmp(mkey, "outrights") != 0)
      continue;

    no = json_object_array_length(outs);

    for(size_t o = 0; o < no; o++)
    {
      struct json_object  *oc = json_object_array_get_idx(outs, o);
      char                 name[ODDSAPI_TEAM_SZ];
      int32_t              px = oddsapi_price(oc);
      oddsapi_contender_t *c  = NULL;
      bool                 quoted = false;

      if(px == 0 || !json_get_str(oc, "name", name, sizeof(name))
          || name[0] == '\0')
        continue;

      for(uint32_t k = 0; k < out->n && c == NULL; k++)
      {
        if(strcmp(out->contenders[k].name, name) == 0)
          c = &out->contenders[k];
      }

      if(c == NULL)
      {
        if(out->n == cap)
          continue;

        c = &out->contenders[out->n++];
        strlcpy(c->name, name, sizeof(c->name));
      }

      for(uint8_t k = 0; k < c->n; k++)
        quoted |= c->book[k] == bi;

      if(quoted)
        continue;

      // A book joins the table with its first price, so the table never
      // counts a book that quoted no outright.
      if(bi == out->nbooks)
      {
        strlcpy(out->book_key[bi], key, sizeof(out->book_key[bi]));
        json_get_str(book, "title", out->book_title[bi],
            sizeof(out->book_title[bi]));
        out->nbooks++;
      }

      c->px[c->n]   = px;
      c->book[c->n] = bi;
      c->n++;
    }
  }
}

// One pseudo-event per outright market in practice, but the reply is an
// array, so every element is folded in. The contender array is sized to
// the cap, filled, then shrunk to what was found.
bool
oddsapi_parse_futures(const char *body, size_t len, oddsapi_futures_t *out)
{
  struct json_object *root;
  size_t              n = 0;

  memset(out, 0, sizeof(*out));

  if((root = oddsapi_root_array(body, len, &n)) == NULL)
    return(false);

  out->contenders = mem_alloc(ODDSAPI_CTX, "futures",
      ODDSAPI_ROWS_MAX * sizeof(*out->contenders));

  for(size_t i = 0; i < n; i++)
  {
    struct json_object *ev    = json_object_array_get_idx(root, i);
    struct json_object *books = json_get_array(ev, "bookmakers");
    size_t              nb    = books != NULL ? json_object_array_length(books) : 0;

    if(out->sport_key[0] == '\0')
    {
      json_get_str(ev, "sport_key", out->sport_key, sizeof(out->sport_key));
      json_get_str(ev, "sport_title", out->sport_title,
          sizeof(out->sport_title));
    }

    for(size_t b = 0; b < nb; b++)
      oddsapi_book_futures(json_object_array_get_idx(books, b), out,
          ODDSAPI_ROWS_MAX);
  }

  json_object_put(root);

  if(out->n == 0)
  {
    mem_free(out->contenders);
    out->contenders = NULL;
  }

  else
    out->contenders = mem_realloc(out->contenders,
        out->n * sizeof(*out->contenders));

  return(true);
}

void
oddsapi_futures_clear(oddsapi_futures_t *f)
{
  if(f->contenders != NULL)
    mem_free(f->contenders);

  memset(f, 0, sizeof(*f));
}

// ----------------------------------------------------------------------
// Consensus and best price
// ----------------------------------------------------------------------

// Sorts `v` in place. An even count takes the mean of the two middles.
static double
oddsapi_median(double *v, uint8_t n)
{
  if(n == 0)
    return(0.0);

  for(uint8_t i = 1; i < n; i++)
  {
    double  x = v[i];
    uint8_t j = i;

    for(; j > 0 && v[j - 1] > x; j--)
      v[j] = v[j - 1];

    v[j] = x;
  }

  return(n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0);
}

// The point most books stand on. A tie goes to the point nearest the
// median of every point, and a tie there to the smaller magnitude —
// the line that moved least.
static bool
oddsapi_modal_point(const float *pts, uint8_t n, float *out)
{
  double  all[ODDSAPI_BOOKS_MAX];
  double  mid;
  uint8_t best_n = 0;

  if(n == 0)
    return(false);

  for(uint8_t i = 0; i < n; i++)
    all[i] = pts[i];

  mid = oddsapi_median(all, n);

  for(uint8_t i = 0; i < n; i++)
  {
    uint8_t cnt = 0;
    double  d_i, d_b;
    bool    better;

    for(uint8_t j = 0; j < n; j++)
      cnt += fabsf(pts[i] - pts[j]) < ODDSAPI_PT_EPS;

    d_i = fabs(pts[i] - mid);
    d_b = fabs(*out - mid);

    if(cnt != best_n)
      better = cnt > best_n;

    else if(fabs(d_i - d_b) >= ODDSAPI_PT_EPS)
      better = d_i < d_b;

    else
      better = fabsf(pts[i]) < fabsf(*out) - ODDSAPI_PT_EPS;

    if(better)
    {
      best_n = cnt;
      *out   = pts[i];
    }
  }

  return(true);
}

static int32_t
oddsapi_col_px(const oddsapi_line_t *l, oddsapi_col_t col)
{
  switch(col)
  {
    case ODDSAPI_COL_ML_AWAY:  return(l->ml_away);
    case ODDSAPI_COL_ML_HOME:  return(l->ml_home);
    case ODDSAPI_COL_ML_DRAW:  return(l->ml_draw);
    case ODDSAPI_COL_SPR_HOME: return(l->spr_home_px);
    case ODDSAPI_COL_SPR_AWAY: return(l->spr_away_px);
    case ODDSAPI_COL_OVER:     return(l->over_px);
    case ODDSAPI_COL_UNDER:    return(l->under_px);
    case ODDSAPI_COL_COUNT:    break;
  }

  return(0);
}

static bool
oddsapi_col_on_point(const oddsapi_line_t *l, oddsapi_col_t col,
    const oddsapi_consensus_t *c)
{
  switch(col)
  {
    case ODDSAPI_COL_SPR_HOME:
    case ODDSAPI_COL_SPR_AWAY:
      return(fabsf(l->spr_home_pt - c->spr_home_pt) < ODDSAPI_PT_EPS);

    case ODDSAPI_COL_OVER:
    case ODDSAPI_COL_UNDER:
      return(fabsf(l->tot_pt - c->tot_pt) < ODDSAPI_PT_EPS);

    default:
      return(true);
  }
}

void
oddsapi_consensus(const oddsapi_game_t *g, oddsapi_consensus_t *out)
{
  float   spr[ODDSAPI_BOOKS_MAX];
  float   tot[ODDSAPI_BOOKS_MAX];
  uint8_t ns = 0, nt = 0;

  memset(out, 0, sizeof(*out));

  for(uint8_t b = 0; b < g->nbooks; b++)
  {
    const oddsapi_line_t *l = &g->books[b];

    if(l->spr_home_px != 0 || l->spr_away_px != 0)
      spr[ns++] = l->spr_home_pt;

    if(l->over_px != 0 || l->under_px != 0)
      tot[nt++] = l->tot_pt;
  }

  oddsapi_modal_point(spr, ns, &out->spr_home_pt);
  oddsapi_modal_point(tot, nt, &out->tot_pt);

  for(int col = 0; col < ODDSAPI_COL_COUNT; col++)
  {
    double  dec[ODDSAPI_BOOKS_MAX];
    uint8_t n = 0;

    for(uint8_t b = 0; b < g->nbooks; b++)
    {
      int32_t px = oddsapi_col_px(&g->books[b], (oddsapi_col_t)col);

      if(px != 0 && oddsapi_col_on_point(&g->books[b], (oddsapi_col_t)col, out))
        dec[n++] = oddsapi_am_to_dec(px);
    }

    out->n[col]  = n;
    out->px[col] = n > 0 ? oddsapi_dec_to_am(oddsapi_median(dec, n)) : 0;
  }
}

// A tie keeps the book listed first.
void
oddsapi_best(const oddsapi_game_t *g, const oddsapi_consensus_t *c,
    oddsapi_best_t out[ODDSAPI_COL_COUNT])
{
  memset(out, 0, ODDSAPI_COL_COUNT * sizeof(*out));

  for(int col = 0; col < ODDSAPI_COL_COUNT; col++)
  {
    for(uint8_t b = 0; b < g->nbooks; b++)
    {
      int32_t px = oddsapi_col_px(&g->books[b], (oddsapi_col_t)col);

      if(px == 0 || !oddsapi_col_on_point(&g->books[b], (oddsapi_col_t)col, c))
        continue;

      if(out[col].px == 0
          || oddsapi_am_to_dec(px) > oddsapi_am_to_dec(out[col].px))
      {
        out[col].px   = px;
        out[col].book = b;
      }
    }
  }
}

// `best->book` indexes the futures' book table, as the contender's own
// `book[]` does.
void
oddsapi_contender_consensus(const oddsapi_contender_t *c, int32_t *median_px,
    oddsapi_best_t *best)
{
  double dec[ODDSAPI_BOOKS_MAX];

  memset(best, 0, sizeof(*best));

  for(uint8_t i = 0; i < c->n; i++)
  {
    dec[i] = oddsapi_am_to_dec(c->px[i]);

    if(best->px == 0 || dec[i] > oddsapi_am_to_dec(best->px))
    {
      best->px   = c->px[i];
      best->book = c->book[i];
    }
  }

  *median_px = c->n > 0 ? oddsapi_dec_to_am(oddsapi_median(dec, c->n)) : 0;
}

static int
oddsapi_contender_cmp(const void *a, const void *b)
{
  const oddsapi_contender_t *ca = a;
  const oddsapi_contender_t *cb = b;
  oddsapi_best_t             unused;
  int32_t                    pa, pb;
  double                     da, db;

  oddsapi_contender_consensus(ca, &pa, &unused);
  oddsapi_contender_consensus(cb, &pb, &unused);
  da = oddsapi_am_to_dec(pa);
  db = oddsapi_am_to_dec(pb);

  if((da == 0.0) != (db == 0.0))
    return(da == 0.0 ? 1 : -1);

  if(da != db)
    return(da < db ? -1 : 1);

  return(strcmp(ca->name, cb->name));
}

void
oddsapi_futures_sort(oddsapi_futures_t *f)
{
  if(f->n > 1)
    qsort(f->contenders, f->n, sizeof(*f->contenders), oddsapi_contender_cmp);
}

// ----------------------------------------------------------------------
// Leagues and teams
// ----------------------------------------------------------------------

const oddsapi_league_t *
oddsapi_league_find(const char *word)
{
  for(size_t i = 0; i < sizeof(oddsapi_leagues) / sizeof(oddsapi_leagues[0]); i++)
  {
    const oddsapi_league_t *l = &oddsapi_leagues[i];

    if(strcasecmp(word, l->alias) == 0
        || (l->alt != NULL && strcasecmp(word, l->alt) == 0)
        || strcmp(word, l->sport_key) == 0)
      return(l);
  }

  return(NULL);
}

const oddsapi_league_t *
oddsapi_league_table(size_t *n)
{
  *n = sizeof(oddsapi_leagues) / sizeof(oddsapi_leagues[0]);
  return(oddsapi_leagues);
}

bool
oddsapi_team_match(const char *team, const char *query)
{
  return(strlen(query) >= ODDSAPI_QUERY_MIN && strcasestr(team, query) != NULL);
}
