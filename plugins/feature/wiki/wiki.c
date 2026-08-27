// botmanager — MIT
// /wiki — a deterministic Wikidata and Wikipedia lookup. There is no
// model anywhere in this file and that is the point: the command has to
// work with the inference engine stopped, and a user gets savvy with its
// syntax the way they do with /s or /n.
#define WIKI_INTERNAL
#define WIKI_CMD_UNIT
#include "wiki.h"

#include <errno.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>

// ----------------------------------------------------------------------
// The request closure, and the registry that accounts for it
//
// This command hands a heap closure to the wikimedia service and gets
// it back on a curl worker. That is a Class-B holding (PLUGIN.md
// §Lifecycle Contract) and nothing central can reclaim it: when the
// service is reloaded under a flight it NULLs the caller's callback
// rather than calling into a mapping that is going away, and the
// closure is then nobody's. Measured 2026-08-24 — three orphans and
// 47 KB after three reloads under flight. stop() is where that is
// settled: wait the closures out, and refuse the unload rather than
// free one under a callback that is still running.
// ----------------------------------------------------------------------

static pthread_mutex_t wiki_active_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  wiki_active_idle  = PTHREAD_COND_INITIALIZER;
static wiki_req_t     *wiki_active_head  = NULL;
static uint32_t        wiki_active_n     = 0;

// Deep-copy the context: every answer here lands on a curl worker long
// after the command callback returned, and the copied method_msg_t is
// what carries reply_route.
static wiki_req_t *
wiki_req_new(const cmd_ctx_t *ctx, const wiki_req_t *from)
{
  wiki_req_t *r = mem_alloc(WIKI_CTX, "req", sizeof(*r));

  *r = *from;
  r->ctx = *ctx;

  if(ctx->msg != NULL)
    r->msg = *ctx->msg;

  r->ctx.msg      = &r->msg;
  r->ctx.args     = NULL;
  r->ctx.username = NULL;
  r->ctx.parsed   = NULL;

  pthread_mutex_lock(&wiki_active_mutex);
  r->next_active   = wiki_active_head;
  wiki_active_head = r;
  wiki_active_n++;
  pthread_mutex_unlock(&wiki_active_mutex);

  return(r);
}

// The one terminal path. Every branch of the flow ends here exactly
// once, whether it answered, failed or never left the ground.
static void
wiki_req_free(wiki_req_t *r)
{
  wiki_req_t **pp;

  pthread_mutex_lock(&wiki_active_mutex);

  for(pp = &wiki_active_head; *pp != NULL; pp = &(*pp)->next_active)
  {
    if(*pp != r)
      continue;

    *pp = r->next_active;
    wiki_active_n--;

    if(wiki_active_n == 0)
      pthread_cond_broadcast(&wiki_active_idle);

    break;
  }

  pthread_mutex_unlock(&wiki_active_mutex);
  mem_free(r);
}

// The context to reply through, rebuilt from the closure's own storage.
static cmd_ctx_t
wiki_ctx_of(wiki_req_t *r)
{
  cmd_ctx_t ctx = r->ctx;

  ctx.msg = &r->msg;

  return(ctx);
}

// A launch that never left the ground: nobody has been told, and the
// closure is still ours to free.
static void
wiki_grounded(wiki_req_t *r, const char *what)
{
  cmd_ctx_t ctx = wiki_ctx_of(r);
  char      line[WIKI_LINE_SZ];

  snprintf(line, sizeof(line), "Could not ask Wikidata about %s.", what);
  cmd_reply(&ctx, line);
  wiki_req_free(r);
}

static void
wiki_failed(wiki_req_t *r, wm_status_t status, const char *message,
    const char *what)
{
  cmd_ctx_t ctx = wiki_ctx_of(r);

  wiki_render_status(&ctx, status, message, what);
  wiki_req_free(r);
}

// The window a resolution produced, kept whole: hits[0] is the answer
// and the rest are what the `also:` line is for.
static void
wiki_adopt_window(wiki_req_t *r, const wm_resolve_res_t *res)
{
  // The service's window is wider than ours — a reverse query fills all
  // of it — and this array holds what an `also:` line could ever name.
  r->n_alt = res->n < WIKI_ALTS_MAX ? res->n : WIKI_ALTS_MAX;
  r->subj  = res->hits[0];
  memcpy(r->alt, res->hits, r->n_alt * sizeof(res->hits[0]));
  strlcpy(r->qid, res->hits[0].qid, sizeof(r->qid));
}

// ----------------------------------------------------------------------
// Parsing — simple if/then/else in evaluation order, no heuristics
// ----------------------------------------------------------------------

// The only hand-maintained data in this command, and it exists purely
// for the words search cannot reach. Every entry here has a measured
// reason: "birthday" matches P3150 ("item for day and month", almost
// always empty) and P569 never appears in the candidate list at all;
// "members" draws P463 "member of", P2124 "member count" and P1342
// "number of seats", none of which is a band's line-up.
//
// ⛔ An alias BYPASSES the service's fall-through — the test that picks
// P2048 over P2044 for a person's height — so a word only belongs here
// when one property is right for every subject. That is why "height" is
// deliberately absent: on a mountain the elevation IS the answer.
//
// If this table grows past twenty entries, the split is wrong and the
// split is what should be fixed.
static const struct
{
  const char *word;
  const char *property;
} wiki_alias[] = {
  { "birthday",  "P569" },
  { "born",      "P569" },
  { "birth",     "P569" },
  { "birthdate", "P569" },
  { "died",      "P570" },
  { "death",     "P570" },
  { "members",   "P527" },
  { "member",    "P527" },
  { "lineup",    "P527" },
  { "spouse",    "P26"  },
  { "wife",      "P26"  },
  { "husband",   "P26"  },
  { "kids",      "P40"  },
  { "children",  "P40"  },
  { "job",       "P106" },
  { "capital",   "P36"  },
  { "population","P1082" },
};

// NULL when nothing is aliased, which is the common case.
static const char *
wiki_alias_of(const char *word)
{
  for(size_t i = 0; i < sizeof(wiki_alias) / sizeof(wiki_alias[0]); i++)
  {
    if(strcasecmp(word, wiki_alias[i].word) == 0)
      return(wiki_alias[i].property);
  }

  return(NULL);
}

// Age is the one question Wikidata cannot be asked: nothing stores it.
// It is a birth date, a death date that may not exist, and arithmetic.
//
// "dead" and "alive" ask that same question from the other end, and
// they are answered HERE rather than by a P570 row in the alias table
// above, because a living person holds no death statement at all: P570
// would answer "is he dead" with an empty, which is also what an
// unfilled field looks like. The two-leg form knows the difference.
static bool
wiki_is_age(const char *word)
{
  static const char *const words[] = {
    "age", "how old", "alive", "still alive", "dead", "is dead"
  };

  for(size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
  {
    if(strcasecmp(word, words[i]) == 0)
      return(true);
  }

  return(false);
}

static const char wiki_usage[] =
    // ⚠ 136 bytes: /help renders this into CMD_USAGE_SZ + 16 behind a
    // "usage: " prefix and TRUNCATES silently past it.
    "wiki [-v] [-l] [-n N] <subject> [property] | wiki <subject> ? | "
    "wiki <property>=<value> | wiki Q<id> | wiki <person> age";

// Is this token a Wikidata item id? The service checks again at its own
// boundary before pasting one into a URL; this is the grammar's check,
// deciding whether the user pinned the subject or merely named it.
static bool
wiki_is_qid(const char *s)
{
  size_t n;

  if(s[0] != 'Q' && s[0] != 'q')
    return(false);

  for(n = 1; s[n] != '\0'; n++)
  {
    if(s[n] < '0' || s[n] > '9')
      return(false);
  }

  return(n > 1 && n < WM_QID_SZ);
}

// Peel a leading id off `s`, leaving the rest in `*rest`. Nothing is
// written and nothing moves unless the token really is one.
static bool
wiki_peel_qid(const char *s, char *out, size_t cap, const char **rest)
{
  char   tok[WM_QID_SZ];
  size_t n = 0;

  while(s[n] != '\0' && s[n] != ' ')
    n++;

  if(n == 0 || n >= cap || n >= sizeof(tok))
    return(false);

  memcpy(tok, s, n);
  tok[n] = '\0';

  if(!wiki_is_qid(tok))
    return(false);

  strlcpy(out, tok, cap);
  s += n;

  while(*s == ' ')
    s++;

  *rest = s;

  return(true);
}

// Take the next whitespace-delimited token from *p, in place.
static char *
wiki_token(char **p)
{
  char *tok;

  while(**p == ' ')
    (*p)++;

  tok = *p;

  while(**p != '\0' && **p != ' ')
    (*p)++;

  if(**p == ' ')
  {
    **p = '\0';
    (*p)++;
  }

  return(tok);
}

// The candidate readings of the line, longest property first. Only the
// trailing tokens are ever a property — a leading one would make
// "president of France" unanswerable.
static void
wiki_split_build(wiki_req_t *r)
{
  char    buf[WIKI_LINE_SZ];
  char   *tok[WIKI_TOKENS_MAX];
  char   *p = buf;
  uint8_t n = 0;

  strlcpy(buf, r->subject, sizeof(buf));

  while(*p != '\0' && n < (uint8_t)(sizeof(tok) / sizeof(tok[0])))
  {
    char *t = wiki_token(&p);

    if(t[0] != '\0')
      tok[n++] = t;
  }

  if(n < 2)
    return;

  for(uint8_t k = n - 1 < WIKI_SPLIT_MAX ? (uint8_t)(n - 1) : WIKI_SPLIT_MAX;
      k >= 1; k--)
  {
    wiki_split_t *s = &r->split[r->n_split];

    s->stem[0] = '\0';
    s->word[0] = '\0';

    for(uint8_t i = 0; i < n - k; i++)
    {
      if(i > 0)
        strlcat(s->stem, " ", sizeof(s->stem));

      strlcat(s->stem, tok[i], sizeof(s->stem));
    }

    for(uint8_t i = (uint8_t)(n - k); i < n; i++)
    {
      if(i > n - k)
        strlcat(s->word, " ", sizeof(s->word));

      strlcat(s->word, tok[i], sizeof(s->word));
    }

    r->n_split++;
  }
}

// false means "show the usage line". Everything this returns true for is
// a shape the flow below knows how to run.
static bool
wiki_parse(const char *args, wiki_req_t *r)
{
  char        buf[WIKI_LINE_SZ];
  const char *rest;
  char       *p;
  char       *eq;
  char       *quote_end = NULL;
  bool        list      = false;
  bool        menu      = false;
  size_t      n;

  strlcpy(buf, args != NULL ? args : "", sizeof(buf));
  p = buf;

  while(*p == ' ')
    p++;

  // Flags are leading-position only, as everywhere else in this tree.
  while(p[0] == '-' && p[1] != '\0')
  {
    char *flag = wiki_token(&p);

    if(strcmp(flag, "-v") == 0 || strcmp(flag, "--verbose") == 0)
      r->verbose = true;

    else if(strcmp(flag, "-l") == 0 || strcmp(flag, "--list") == 0)
      list = true;

    else if(strcmp(flag, "-n") == 0 || strcmp(flag, "--limit") == 0)
    {
      // Anything unreadable is 0, which is "the form's own default".
      // The ceiling is the service's window; without it -n 300 wraps to
      // 44 and the user is told a number nobody typed.
      unsigned long want = strtoul(wiki_token(&p), NULL, 10);

      r->limit = want > WM_CANDIDATES_MAX ? WM_CANDIDATES_MAX
                                          : (uint8_t)want;
    }

    else
      return(false);

    while(*p == ' ')
      p++;
  }

  n = strlen(p);

  while(n > 0 && p[n - 1] == ' ')
    p[--n] = '\0';

  // A trailing "?" asks what CAN be asked, so it is stripped before the
  // subject is read.
  if(n > 0 && p[n - 1] == '?')
  {
    p[--n] = '\0';

    while(n > 0 && p[n - 1] == ' ')
      p[--n] = '\0';

    menu = true;
  }

  if(n == 0)
    return(false);

  if(*p == '"')
  {
    quote_end = strchr(p + 1, '"');

    // A quote with no partner is a line that ran out mid-pin, not a
    // syntax error: drop the stray and read what is left the way an
    // unquoted line is read. Refusing costs more than the lost pin does,
    // because the asker is often the NL bridge, and a usage line reaches
    // the persona there as though it were the answer. Core's own
    // rest-of-line parser reads a lone quote the same way.
    if(quote_end == NULL)
      p++;
  }

  if(quote_end != NULL)
  {
    // Quotes pin the subject outright — the one escape from the split.
    *quote_end = '\0';
    strlcpy(r->subject, p + 1, sizeof(r->subject));
    p = quote_end + 1;

    while(*p == ' ')
      p++;

    strlcpy(r->property, p, sizeof(r->property));
  }

  else if(wiki_peel_qid(p, r->qid, sizeof(r->qid), &rest))
  {
    // An id pins the subject and skips resolution entirely.
    strlcpy(r->subject, r->qid, sizeof(r->subject));
    strlcpy(r->property, rest, sizeof(r->property));
  }

  else if((eq = strchr(p, '=')) != NULL)
  {
    // The reverse query: who holds this statement.
    *eq = '\0';
    strlcpy(r->property, p, sizeof(r->property));
    strlcpy(r->subject, eq + 1, sizeof(r->subject));

    while(r->subject[0] == ' ')
      memmove(r->subject, r->subject + 1, strlen(r->subject));

    if(r->property[0] == '\0' || r->subject[0] == '\0')
      return(false);

    r->form = WIKI_FORM_REVERSE;
    return(true);
  }

  else
    strlcpy(r->subject, p, sizeof(r->subject));

  if(r->subject[0] == '\0')
    return(false);

  if(menu)
    r->form = WIKI_FORM_MENU;

  else if(list)
    r->form = WIKI_FORM_LIST;

  else if(r->property[0] != '\0')
  {
    r->age  = wiki_is_age(r->property);
    r->form = WIKI_FORM_FACT;
  }

  else if(!r->verbose && r->qid[0] == '\0'
      && strchr(r->subject, ' ') != NULL)
  {
    // Nothing said which words were the property, so the line has to be
    // told apart. -v and a pinned QID both mean the whole string is the
    // subject and there is nothing to split.
    wiki_split_build(r);
    r->form = r->n_split > 0 ? WIKI_FORM_SPLIT : WIKI_FORM_CARD;
  }

  else
    r->form = WIKI_FORM_CARD;

  return(true);
}

// ----------------------------------------------------------------------
// The flow. Every step is a callback off the curl worker; the closure
// is freed on exactly one path per branch.
// ----------------------------------------------------------------------

static void wiki_card_begin(wiki_req_t *r);
static void wiki_split_probe(wiki_req_t *r);
static void wiki_split_run(wiki_req_t *r);
static void wiki_ask(wiki_req_t *r);

static void wiki_resolved(const wm_resolve_res_t *res, void *user);
static void wiki_facts_done(const wm_facts_res_t *f, void *user);
static void wiki_prose_done(const wm_prose_res_t *p, void *user);
static void wiki_fact_done(const wm_claims_res_t *c, void *user);
static void wiki_age_born(const wm_claims_res_t *c, void *user);
static void wiki_age_died(const wm_claims_res_t *c, void *user);
static void wiki_probe_done(const wm_property_res_t *p, void *user);
static void wiki_split_subject(const wm_resolve_res_t *res, void *user);
static void wiki_reverse_value(const wm_resolve_res_t *res, void *user);
static void wiki_matches_done(const wm_resolve_res_t *res, void *user);

// --- the card, and the -l list that shares its resolution ------------

static void
wiki_card_begin(wiki_req_t *r)
{
  if(r->qid[0] != '\0')
  {
    if(wm_facts_async(r->qid, wiki_facts_done, r) != ASYNC_AIRBORNE)
      wiki_grounded(r, r->qid);

    return;
  }

  if(wm_resolve_async(r->subject, wiki_resolved, r) != ASYNC_AIRBORNE)
    wiki_grounded(r, r->subject);
}

// One resolution serves the card, the list, the menu and the pinned-fact
// forms; what happens next is the form's business.
static void
wiki_resolved(const wm_resolve_res_t *res, void *user)
{
  wiki_req_t *r = user;
  cmd_ctx_t   ctx;

  if(res->status != WM_OK)
  {
    wiki_failed(r, res->status, res->message, r->subject);
    return;
  }

  wiki_adopt_window(r, res);
  ctx = wiki_ctx_of(r);

  switch(r->form)
  {
    case WIKI_FORM_LIST:
      wiki_render_list(&ctx, r, res);
      wiki_req_free(r);
      return;

    case WIKI_FORM_FACT:
      wiki_ask(r);
      return;

    default:
      break;
  }

  // The verbose card carries the article's opening, so the prose leg
  // runs before the facts rather than after the answer is drawn.
  if(r->verbose && r->subj.title[0] != '\0'
      && wm_prose_async(r->subj.title, false, wiki_prose_done, r)
          == ASYNC_AIRBORNE)
    return;

  if(wm_facts_async(r->qid, wiki_facts_done, r) != ASYNC_AIRBORNE)
    wiki_grounded(r, r->subject);
}

static void
wiki_prose_done(const wm_prose_res_t *p, void *user)
{
  wiki_req_t *r = user;

  // No article is not a failure of the card — it just has no opening.
  if(p->status == WM_OK && p->text != NULL)
    strlcpy(r->lead, p->text, sizeof(r->lead));

  if(wm_facts_async(r->qid, wiki_facts_done, r) != ASYNC_AIRBORNE)
    wiki_grounded(r, r->subject);
}

static void
wiki_facts_done(const wm_facts_res_t *f, void *user)
{
  wiki_req_t *r   = user;
  cmd_ctx_t   ctx = wiki_ctx_of(r);
  wm_facts_res_t empty;

  if(f->status != WM_OK)
  {
    // An item whose type publishes no menu still has a name and a
    // description, and printing those beats printing an error.
    if(f->status == WM_NOT_FOUND && r->subj.label[0] != '\0')
    {
      memset(&empty, 0, sizeof(empty));
      empty.status = WM_OK;
      strlcpy(empty.qid, r->qid, sizeof(empty.qid));

      if(r->form == WIKI_FORM_MENU)
        wiki_render_menu(&ctx, r, &empty);
      else
        wiki_render_card(&ctx, r, &empty);

      wiki_req_free(r);
      return;
    }

    wiki_failed(r, f->status, f->message, r->subject);
    return;
  }

  if(r->form == WIKI_FORM_MENU)
    wiki_render_menu(&ctx, r, f);
  else
    wiki_render_card(&ctx, r, f);

  wiki_req_free(r);
}

// --- one fact ---------------------------------------------------------

// The subject is resolved and the property is named: ask. An alias is a
// P-id, which the service takes as the answer outright and never
// searches for — one fewer request, and the right answer for the words
// where search is measurably wrong.
static void
wiki_ask(wiki_req_t *r)
{
  const char *alias;

  if(r->age)
  {
    if(wm_claims_async(r->qid, "P569", wiki_age_born, r) != ASYNC_AIRBORNE)
      wiki_grounded(r, r->property);

    return;
  }

  alias = wiki_alias_of(r->property);

  if(wm_claims_async(r->qid, alias != NULL ? alias : r->property,
      wiki_fact_done, r) != ASYNC_AIRBORNE)
    wiki_grounded(r, r->property);
}


static void
wiki_fact_done(const wm_claims_res_t *c, void *user)
{
  wiki_req_t *r   = user;
  cmd_ctx_t   ctx = wiki_ctx_of(r);

  if(c->status == WM_OK && c->n > 0)
  {
    wiki_render_fact(&ctx, r, c);
    wiki_req_free(r);
    return;
  }

  // A split that found no value falls through to the next reading, and
  // finally to the whole line being the subject. That test is the whole
  // reason the split works: it is what stops "green day" parsing "day"
  // as a property.
  if(r->form == WIKI_FORM_SPLIT && c->status == WM_NOT_FOUND)
  {
    r->at++;
    wiki_split_run(r);
    return;
  }

  if(c->status == WM_NOT_FOUND)
  {
    char line[WIKI_LINE_SZ];

    snprintf(line, sizeof(line), "Wikidata records no %s for %s.",
        r->property, r->subj.label[0] != '\0' ? r->subj.label : r->qid);
    cmd_reply(&ctx, line);
    wiki_req_free(r);
    return;
  }

  wiki_failed(r, c->status, c->message, r->property);
}

// --- age, which nobody stores -----------------------------------------

static void
wiki_age_born(const wm_claims_res_t *c, void *user)
{
  wiki_req_t *r = user;

  if(c->status == WM_OK && c->n > 0
      && c->claims[0].value.kind == WM_VAL_TIME)
  {
    r->born = c->claims[0].value.time;

    // The second leg is the whole reason this is not one line in the
    // fact renderer: an age counted to today is only right for the
    // living, and nothing in the first answer says which this is.
    if(wm_claims_async(r->qid, "P570", wiki_age_died, r) != ASYNC_AIRBORNE)
      wiki_grounded(r, r->property);

    return;
  }

  if(r->form == WIKI_FORM_SPLIT && c->status == WM_NOT_FOUND)
  {
    r->at++;
    wiki_split_run(r);
    return;
  }

  wiki_failed(r, c->status == WM_OK ? WM_NOT_FOUND : c->status, c->message,
      r->subject);
}

static void
wiki_age_died(const wm_claims_res_t *c, void *user)
{
  wiki_req_t *r   = user;
  cmd_ctx_t   ctx = wiki_ctx_of(r);

  // WM_NOT_FOUND here means alive, and that is an answer rather than a
  // failure — the distinction the whole service is built around. A
  // TRANSPORT failure is neither, and must never read as the first.
  wiki_render_age(&ctx, r, c->status,
      c->status == WM_OK && c->n > 0 ? &c->claims[0].value : NULL);
  wiki_req_free(r);
}

// --- the greedy split -------------------------------------------------

// Ask, of each candidate tail, whether it names a property at all. That
// is all this phase decides — which of them is RIGHT is settled by
// value presence, inside the service, one call later.
static void
wiki_split_probe(wiki_req_t *r)
{
  if(r->at >= r->n_split)
  {
    r->at = 0;
    wiki_split_run(r);
    return;
  }

  // An aliased word is settled vocabulary: it needs no search, and not
  // making one is the largest single saving in the whole command.
  if(wiki_alias_of(r->split[r->at].word) != NULL
      || wiki_is_age(r->split[r->at].word))
  {
    r->split[r->at].viable = true;
    r->at++;
    wiki_split_probe(r);
    return;
  }

  if(wm_property_async(r->split[r->at].word, wiki_probe_done, r)
      != ASYNC_AIRBORNE)
  {
    // A probe that could not fly leaves the reading unproven rather
    // than refuted; the rest of the split still runs.
    r->at++;
    wiki_split_probe(r);
  }
}

static void
wiki_probe_done(const wm_property_res_t *p, void *user)
{
  wiki_req_t *r = user;

  // A backoff refuses every call after this one too, so working through
  // the rest of the split would only spell out unproven readings and end
  // in the same refusal. Say it once, now, with the wait attached.
  if(p->status == WM_RATE_LIMITED)
  {
    wiki_failed(r, p->status, p->message, r->subject);
    return;
  }

  r->split[r->at].viable = p->status == WM_OK && p->n > 0;
  r->at++;
  wiki_split_probe(r);
}

// Try the readings in order, longest property first, and take the first
// that both resolves and has a value.
static void
wiki_split_run(wiki_req_t *r)
{
  while(r->at < r->n_split && !r->split[r->at].viable)
    r->at++;

  if(r->at >= r->n_split)
  {
    // No tail named a property with anything to say, so the whole line
    // is the subject — and the subject a discarded reading resolved to
    // goes with it. Leaving it behind is how `wiki green day` answers
    // with the colour.
    r->form   = WIKI_FORM_CARD;
    r->qid[0] = '\0';
    r->n_alt  = 0;
    r->age    = false;
    memset(&r->subj, 0, sizeof(r->subj));
    wiki_card_begin(r);
    return;
  }

  if(wm_resolve_async(r->split[r->at].stem, wiki_split_subject, r)
      != ASYNC_AIRBORNE)
    wiki_grounded(r, r->split[r->at].stem);
}

static void
wiki_split_subject(const wm_resolve_res_t *res, void *user)
{
  wiki_req_t *r = user;

  // Only "no such subject" refutes a reading. A rate limit or a dead
  // wire says nothing about which words were the property, and reading
  // it as if it did would answer a different question quietly.
  if(res->status == WM_NOT_FOUND)
  {
    r->at++;
    wiki_split_run(r);
    return;
  }

  // The reading is unproven, so it is not the thing to name back: the
  // question was the whole line, and "could not reach Wikidata for
  // isaac" is a worse answer than the one the user typed.
  if(res->status != WM_OK)
  {
    wiki_failed(r, res->status, res->message, r->subject);
    return;
  }

  wiki_adopt_window(r, res);
  strlcpy(r->property, r->split[r->at].word, sizeof(r->property));
  r->age = wiki_is_age(r->property);
  wiki_ask(r);
}

// --- the reverse query ------------------------------------------------

static void
wiki_reverse_value(const wm_resolve_res_t *res, void *user)
{
  wiki_req_t *r = user;

  if(res->status != WM_OK)
  {
    wiki_failed(r, res->status, res->message, r->subject);
    return;
  }

  wiki_adopt_window(r, res);

  if(wm_reverse_async(r->property, r->qid, wiki_matches_done, r)
      != ASYNC_AIRBORNE)
    wiki_grounded(r, r->property);
}

static void
wiki_matches_done(const wm_resolve_res_t *res, void *user)
{
  wiki_req_t *r   = user;
  cmd_ctx_t   ctx = wiki_ctx_of(r);

  if(res->status != WM_OK)
  {
    char what[WIKI_LINE_SZ];

    snprintf(what, sizeof(what), "%s = %s", r->property,
        r->subj.label[0] != '\0' ? r->subj.label : r->subject);
    wiki_failed(r, res->status, res->message, what);
    return;
  }

  wiki_render_matches(&ctx, r, res);
  wiki_req_free(r);
}

// ----------------------------------------------------------------------
// Command surface
// ----------------------------------------------------------------------

static void
wiki_cmd(const cmd_ctx_t *ctx)
{
  wiki_req_t  stack;
  wiki_req_t *r;

  memset(&stack, 0, sizeof(stack));

  if(!wiki_parse(ctx->args, &stack))
  {
    cmd_reply(ctx, wiki_usage);
    return;
  }

  // A sunk reply is going to the model, not to a person: dense, no
  // colour, no alternates line, and inside the interpret cue's fence.
  stack.dense = ctx->msg != NULL && ctx->msg->reply_sink_id != 0;
  r           = wiki_req_new(ctx, &stack);

  switch(r->form)
  {
    case WIKI_FORM_SPLIT:
      wiki_split_probe(r);
      break;

    case WIKI_FORM_REVERSE:
      if(wm_resolve_async(r->subject, wiki_reverse_value, r)
          != ASYNC_AIRBORNE)
        wiki_grounded(r, r->subject);

      break;

    case WIKI_FORM_FACT:
      if(r->qid[0] != '\0')
      {
        wiki_ask(r);
        break;
      }

      if(wm_resolve_async(r->subject, wiki_resolved, r) != ASYNC_AIRBORNE)
        wiki_grounded(r, r->subject);

      break;

    case WIKI_FORM_MENU:
      wiki_card_begin(r);
      break;

    case WIKI_FORM_LIST:
      if(wm_resolve_async(r->subject, wiki_resolved, r) != ASYNC_AIRBORNE)
        wiki_grounded(r, r->subject);

      break;

    default:
      wiki_card_begin(r);
      break;
  }
}

// The bridge's view of this command. Everything it teaches is grammar
// the model would otherwise guess at, and the two things it guesses
// wrong are the word ORDER (the property is the tail, never the head)
// and the trailing `?`, which is a different question entirely.
//
// ⛔ The reverse form (`<property>=<value>`) is deliberately NOT taught.
// It is the one shape that is easy to emit backwards — "who directed
// Alien" is `/wiki alien director`, not `/wiki director=alien` — and a
// confident wrong reading answers a question nobody asked. A person who
// wants it can type it.
static const cmd_nl_slot_t wiki_slots[] = {
  { .name = "subject",  .type = CMD_NL_ARG_TOPIC,
    .flags = CMD_NL_SLOT_REQUIRED },
  { .name = "property", .type = CMD_NL_ARG_TOPIC,
    .flags = CMD_NL_SLOT_OPTIONAL | CMD_NL_SLOT_REMAINDER },
};

static const cmd_nl_example_t wiki_examples[] = {
  { .utterance  = "who was hypatia?",
    .invocation = "/wiki hypatia" },
  { .utterance  = "how tall is tom cruise?",
    .invocation = "/wiki tom cruise height" },
  { .utterance  = "what did marie curie die of?",
    .invocation = "/wiki marie curie cause of death" },
  { .utterance  = "who's in green day these days?",
    .invocation = "/wiki \"green day\" members" },
  { .utterance  = "is clint eastwood dead?",
    .invocation = "/wiki clint eastwood age" },
};

static const cmd_nl_t wiki_nl = {
  // ⚠ This whole declaration has a size budget — see cmd_nl_t in cmd.h.
  .when          = "A question of fact about a real, identifiable thing — "
                   "person, place, film, band, book, song, species, "
                   "element, company — what it is, or one recorded "
                   "property of it. Use it EVEN WHEN you think you know: "
                   "you are confidently wrong about dates, heights and "
                   "composers often enough to be worth the lookup. Not for "
                   "opinions or arithmetic.",
  .syntax        = "/wiki <subject> [property] — subject FIRST, property "
                   "LAST, in the user's own words. Quote the subject to "
                   "pin it. Never append a question mark; a trailing ? "
                   "asks a different question.",
  .slots         = wiki_slots,
  .slot_count    = (uint8_t)(sizeof(wiki_slots) / sizeof(wiki_slots[0])),
  .examples      = wiki_examples,
  .example_count = (uint8_t)(sizeof(wiki_examples)
                             / sizeof(wiki_examples[0])),
};

static const cmd_decl_t wiki_decl = {
  .module      = WIKI_CTX,
  .name        = "wiki",
  .usage       = wiki_usage,
  .description = "Look something up on Wikidata and Wikipedia",
  .help_long   =
      "Ask about a thing, or about one property of it: `wiki tom cruise "
      "height`. The trailing words are tried as a property and the rest "
      "is the subject, longest match first, and a property with no value "
      "on that subject falls through — which is why `wiki green day` is a "
      "band and not the colour's day. Quote the subject to pin it "
      "(`wiki \"green day\" members`), or name it by id (`wiki Q937 "
      "born`). A trailing ? lists what can be asked about a thing of that "
      "type. `wiki <person> age` — or `alive`, or `dead` — is the one "
      "question Wikidata cannot be asked, because nothing stores it: it "
      "is answered from the birth date plus whether a death date exists, "
      "so the living get an age and the dead get the age they reached. "
      "`<property>=<value>` runs the query backwards and finds what "
      "holds that statement. -l shows the candidates with their ids, -n "
      "caps a list, and -v gives the long card with the article's "
      "opening — it takes the whole line as the subject and splits no "
      "property off it.",
  .group       = "everyone",
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = wiki_cmd,
  .nl          = &wiki_nl,
};

static bool
wiki_init(void)
{
  if(cmd_register(&wiki_decl) != SUCCESS)
    return(FAIL);

  clam(CLAM_INFO, WIKI_CTX, "wiki command plugin initialized");

  return(SUCCESS);
}

// Wait the airborne closures out. A wikimedia request is bounded by
// plugin.wikimedia.timeout, so what does not land inside this budget is
// wedged rather than slow — and a refused unload leaves the plugin
// running and intact, which is strictly better than freeing a closure
// under a callback that is one instruction from touching it.
static bool
wiki_stop(void)
{
  struct timespec deadline;
  uint32_t        left;

  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += WIKI_STOP_DRAIN_MS / 1000;

  pthread_mutex_lock(&wiki_active_mutex);

  while(wiki_active_n > 0
      && pthread_cond_timedwait(&wiki_active_idle, &wiki_active_mutex,
             &deadline) != ETIMEDOUT)
    ;

  left = wiki_active_n;
  pthread_mutex_unlock(&wiki_active_mutex);

  if(left == 0)
    return(SUCCESS);

  clam(CLAM_WARN, WIKI_CTX, "%u lookup(s) still airborne after a %u ms "
      "drain; refusing the unload rather than freeing them under their "
      "own callbacks", left, (uint32_t)WIKI_STOP_DRAIN_MS);

  return(FAIL);
}

static void
wiki_deinit(void)
{
  cmd_unregister_path("wiki");

  clam(CLAM_INFO, WIKI_CTX, "wiki command plugin deinitialized");
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = WIKI_CTX,
  .version         = "1.0",
  .type            = PLUGIN_MISC,
  .kind            = WIKI_CTX,
  .provides        = { { .name = "cmd_wiki" } },
  .provides_count  = 1,
  .requires        = {
    { .name = "bot_chat" },
    { .name = "service_wikimedia" },
  },
  .requires_count  = 2,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = wiki_init,
  .start           = NULL,
  .stop            = wiki_stop,
  .deinit          = wiki_deinit,
  .ext             = NULL,
};
