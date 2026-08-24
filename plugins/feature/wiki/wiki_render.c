// botmanager — MIT
// Everything /wiki puts on a channel: the card, the one-fact answer,
// the property menu, the candidate list and a reverse query's matches.
// The service plugin hands over normalized types and no opinions; the
// opinions are all here.
#define WIKI_INTERNAL
#include "wiki.h"

#include <inttypes.h>
#include <stdlib.h>
#include <time.h>

// ----------------------------------------------------------------------
// Values, as a reader would say them
// ----------------------------------------------------------------------

static const char *const wiki_months[] = {
  "January", "February", "March",     "April",   "May",      "June",
  "July",    "August",   "September", "October", "November", "December"
};

static const char *
wiki_ordinal(long n)
{
  long tens = n % 100;

  if(tens >= 11 && tens <= 13)
    return("th");

  switch(n % 10)
  {
    case 1:  return("st");
    case 2:  return("nd");
    case 3:  return("rd");
    default: return("th");
  }
}

// Split a stamp into its parts. false where there is not even a year,
// which is what sends the raw text out unchanged.
static bool
wiki_time_ymd(const wm_time_t *t, long *y, long *m, long *d, bool *bc)
{
  const char *s = t->text;
  char       *end;

  *y  = 0;
  *m  = 0;
  *d  = 0;
  *bc = s[0] == '-';

  if(*bc)
    s++;

  *y = strtol(s, &end, 10);

  if(end == s)
    return(false);

  if(*end == '-')
  {
    *m = strtol(end + 1, &end, 10);

    if(*end == '-')
      *d = strtol(end + 1, &end, 10);
  }

  return(true);
}

// Wikidata stamps every date to the day and then says how much of it to
// believe. Padding a year out to the 1st of January is the one thing a
// renderer must never do — Hypatia died in March 415, not on March 1st.
static void
wiki_time_text(const wm_time_t *t, char *out, size_t cap)
{
  const char *era;
  long        y;
  long        m;
  long        d;
  bool        bc;

  if(!wiki_time_ymd(t, &y, &m, &d, &bc))
  {
    strlcpy(out, t->text, cap);
    return;
  }

  era = bc ? " BC" : "";

  if(t->precision >= 11 && d > 0 && m >= 1 && m <= 12)
    snprintf(out, cap, "%ld %s %ld%s", d, wiki_months[m - 1], y, era);

  else if(t->precision == 10 && m >= 1 && m <= 12)
    snprintf(out, cap, "%s %ld%s", wiki_months[m - 1], y, era);

  else if(t->precision == 9)
    snprintf(out, cap, "%ld%s", y, era);

  else if(t->precision == 8)
    snprintf(out, cap, "%lds%s", y - y % 10, era);

  else if(t->precision == 7)
  {
    long c = (y + 99) / 100;

    snprintf(out, cap, "%ld%s century%s", c, wiki_ordinal(c), era);
  }

  else if(t->precision == 6)
  {
    long m2 = (y + 999) / 1000;

    snprintf(out, cap, "%ld%s millennium%s", m2, wiki_ordinal(m2), era);
  }

  // Coarser than a millennium is geology, and the stamp's own year is
  // the only honest thing left to say about it.
  else
    snprintf(out, cap, "%ld%s", y, era);
}

// Recorded-as-unknown and recorded-as-none are answers, and they are
// different answers. A property with no statement at all never reaches
// here — the service answers that WM_NOT_FOUND.
void
wiki_value_text(const wm_value_t *v, char *out, size_t cap)
{
  switch(v->kind)
  {
    case WM_VAL_UNKNOWN:
      strlcpy(out, "unknown", cap);
      break;

    case WM_VAL_NONE:
      strlcpy(out, "none", cap);
      break;

    case WM_VAL_TIME:
      wiki_time_text(&v->time, out, cap);
      break;

    default:
      strlcpy(out, v->text, cap);
      break;
  }
}

// ----------------------------------------------------------------------
// Line building
// ----------------------------------------------------------------------

// A sunk answer is fenced by BYTES, not by columns — the interpret cue
// budgets METHOD_TEXT_SZ minus its own framing — so the trim is a byte
// trim, backed off to a code-point boundary so no model ever reads half
// a glyph.
static void
wiki_dense_reply(const cmd_ctx_t *ctx, char *text)
{
  size_t n = strlen(text);

  if(n > WIKI_DENSE_MAX)
  {
    n = WIKI_DENSE_MAX;

    while(n > 0 && (text[n] & 0xC0) == 0x80)
      n--;

    text[n] = '\0';
  }

  cmd_reply(ctx, text);
}

// Push `right` out to the house width. A line already that wide gets a
// single space instead, and so does one with no room left in its buffer.
static void
wiki_flush_right(char *line, size_t cap, const char *right)
{
  size_t len = strlen(line);
  size_t vis = display_vis_len(line) + display_vis_len(right);
  size_t pad = vis < DISPLAY_COLS ? DISPLAY_COLS - vis : 1;

  if(len + pad + strlen(right) + 1 > cap)
    pad = 1;

  if(len + pad + strlen(right) + 1 > cap)
    return;

  memset(line + len, ' ', pad);
  line[len + pad] = '\0';
  strlcat(line, right, cap);
}

// Send `text` as indented lines no wider than the house width. Breaks on
// spaces; a word wider than the line goes out whole rather than in
// halves.
static void
wiki_wrap(const cmd_ctx_t *ctx, const char *text)
{
  const char *p = text;

  while(*p != '\0')
  {
    char   chunk[WIKI_CELL_SZ * 2];
    char   line[WIKI_LINE_SZ];
    char  *sp;
    size_t used;

    display_fit(p, DISPLAY_COLS - WIKI_INDENT, chunk, sizeof(chunk), NULL);
    used = strlen(chunk);

    if(used == 0)
      return;

    if(p[used] != '\0' && (sp = strrchr(chunk, ' ')) != NULL)
    {
      *sp  = '\0';
      used = (size_t)(sp - chunk) + 1;
    }

    snprintf(line, sizeof(line), "%*s%s", WIKI_INDENT, "", chunk);
    cmd_reply(ctx, line);

    p += used;

    while(*p == ' ')
      p++;
  }
}

// The heading every card and list wears: what we decided the subject is,
// what Wikidata says it is, and the id — because the id is what a user
// pins next time to skip the guessing.
static void
wiki_heading(const cmd_ctx_t *ctx, const char *label, const char *desc,
    const char *qid)
{
  char line[WIKI_LINE_SZ];
  char cell[WIKI_CELL_SZ];
  int  room;

  snprintf(line, sizeof(line), "%*s" CLR_BOLD "%s" CLR_RESET,
      WIKI_INDENT, "", label[0] != '\0' ? label : qid);

  // One column back from what is left: display_fit's mark is reserved
  // out of the BYTE budget and not the column one, so a truncated cell
  // is one column wider than it was asked for.
  room = DISPLAY_COLS - (int)display_vis_len(line)
      - (int)strlen(qid) - 6;

  if(desc[0] != '\0' && room > 8)
  {
    display_fit(desc, room, cell, sizeof(cell), "…");
    strlcat(line, CLR_GRAY "  ·  ", sizeof(line));
    strlcat(line, cell, sizeof(line));
    strlcat(line, CLR_RESET, sizeof(line));
  }

  snprintf(cell, sizeof(cell), CLR_GRAY "%s" CLR_RESET, qid);
  wiki_flush_right(line, sizeof(line), cell);
  cmd_reply_table_head(ctx, line);
}

// One `label   value` row of a card.
static void
wiki_row(const cmd_ctx_t *ctx, const char *label, const char *value)
{
  char line[WIKI_LINE_SZ];
  char lab[WIKI_CELL_SZ];
  char val[WIKI_CELL_SZ];

  display_fit(label, WIKI_LABEL_COLS - 2, lab, sizeof(lab), "…");
  display_align_left(lab, sizeof(lab), WIKI_LABEL_COLS);
  display_fit(value, WIKI_VALUE_COLS - 1, val, sizeof(val), "…");

  snprintf(line, sizeof(line), "%*s" CLR_GRAY "%s" CLR_RESET "%s",
      WIKI_INDENT, "", lab, val);
  cmd_reply(ctx, line);
}

// The values of one fact, joined, with what was left behind counted
// rather than hidden.
static void
wiki_fact_values(const wm_fact_t *f, char *out, size_t cap)
{
  char cell[WM_VALUE_SZ + 32];

  out[0] = '\0';

  for(uint8_t i = 0; i < f->n; i++)
  {
    wiki_value_text(&f->values[i], cell, sizeof(cell));

    if(i > 0)
      strlcat(out, ", ", cap);

    strlcat(out, cell, cap);
  }

  if(f->total > f->n)
  {
    snprintf(cell, sizeof(cell), " (+%u)", (unsigned)(f->total - f->n));
    strlcat(out, cell, cap);
  }
}

// One statement, with whatever qualifies it. "(start time 1987)" is
// Wikidata's own wording and it is what makes a band's line-up readable
// without a second query.
static void
wiki_claim_text(const wm_claim_t *c, char *out, size_t cap)
{
  char cell[WM_VALUE_SZ + 32];

  wiki_value_text(&c->value, out, cap);

  for(uint8_t q = 0; q < c->n_quals; q++)
  {
    const wm_qualifier_t *ql = &c->quals[q];

    wiki_value_text(&ql->value, cell, sizeof(cell));
    strlcat(out, q == 0 ? " (" : ", ", cap);

    if(ql->label[0] != '\0')
    {
      strlcat(out, ql->label, cap);
      strlcat(out, " ", cap);
    }

    strlcat(out, cell, cap);

    if(q + 1 == c->n_quals)
      strlcat(out, ")", cap);
  }
}

// The alternates line. Its whole job is to tell the reader which word to
// add next time, so it names them — ids belong under -l.
static void
wiki_also(const cmd_ctx_t *ctx, const wiki_req_t *r)
{
  char    line[WIKI_LINE_SZ];
  uint8_t shown = 0;

  if(r->dense || r->n_alt < 2)
    return;

  snprintf(line, sizeof(line), "%*s" CLR_GRAY "also:" CLR_RESET,
      WIKI_INDENT, "");

  for(uint8_t i = 1; i < r->n_alt && shown < 3; i++)
  {
    if(r->alt[i].label[0] == '\0'
        || strcmp(r->alt[i].label, r->subj.label) == 0)
      continue;

    strlcat(line, shown == 0 ? " " : CLR_GRAY " · " CLR_RESET,
        sizeof(line));
    strlcat(line, r->alt[i].label, sizeof(line));
    shown++;
  }

  if(shown > 0)
    cmd_reply(ctx, line);
}

// ----------------------------------------------------------------------
// The five answers
// ----------------------------------------------------------------------

void
wiki_render_card(const cmd_ctx_t *ctx, const wiki_req_t *r,
    const wm_facts_res_t *f)
{
  const char *label = f->label[0]   != '\0' ? f->label : r->subj.label;
  const char *desc  = f->description[0] != '\0' ? f->description
                                                : r->subj.description;
  uint8_t     rows  = r->verbose ? WIKI_CARD_ROWS_FULL : WIKI_CARD_ROWS;
  char        line[WIKI_LINE_SZ];
  char        vals[WIKI_TEXT_SZ];

  if(r->dense)
  {
    snprintf(line, sizeof(line), "%s (%s)%s%s.", label, f->qid,
        desc[0] != '\0' ? ": " : "", desc);

    for(uint8_t i = 0; i < f->n && strlen(line) < WIKI_DENSE_MAX; i++)
    {
      wiki_fact_values(&f->facts[i], vals, sizeof(vals));
      strlcat(line, " ", sizeof(line));
      strlcat(line, f->facts[i].label, sizeof(line));
      strlcat(line, ": ", sizeof(line));
      strlcat(line, vals, sizeof(line));
      strlcat(line, ".", sizeof(line));
    }

    wiki_dense_reply(ctx, line);
    return;
  }

  wiki_heading(ctx, label, desc, f->qid);

  if(r->verbose && r->lead[0] != '\0')
  {
    wiki_wrap(ctx, r->lead);
    cmd_reply(ctx, " ");
  }

  for(uint8_t i = 0; i < f->n && i < rows; i++)
  {
    wiki_fact_values(&f->facts[i], vals, sizeof(vals));
    wiki_row(ctx, f->facts[i].label, vals);
  }

  if(f->n == 0)
    wiki_row(ctx, "facts", "none this item's type publishes a menu for");

  else if(f->n > rows)
  {
    snprintf(line, sizeof(line), CLR_GRAY "%u more; -v shows them" CLR_RESET,
        (unsigned)(f->n - rows));
    wiki_row(ctx, "", line);
  }

  wiki_also(ctx, r);
}

void
wiki_render_fact(const cmd_ctx_t *ctx, const wiki_req_t *r,
    const wm_claims_res_t *c)
{
  const char *who = r->subj.label[0] != '\0' ? r->subj.label : c->entity;
  const char *what = c->label[0] != '\0' ? c->label : c->property;
  char        text[WIKI_TEXT_SZ];
  char        cell[WM_VALUE_SZ + 64];

  snprintf(text, sizeof(text), "%s — %s: ", who, what);

  for(uint8_t i = 0; i < c->n; i++)
  {
    wiki_claim_text(&c->claims[i], cell, sizeof(cell));

    if(i > 0)
      strlcat(text, ", ", sizeof(text));

    strlcat(text, cell, sizeof(text));
  }

  if(r->dense)
  {
    wiki_dense_reply(ctx, text);
    return;
  }

  wiki_wrap(ctx, text);
  wiki_also(ctx, r);
}

void
wiki_render_menu(const cmd_ctx_t *ctx, const wiki_req_t *r,
    const wm_facts_res_t *f)
{
  const char *who = f->label[0] != '\0' ? f->label
                  : r->subj.label[0] != '\0' ? r->subj.label : r->qid;
  char        text[WIKI_TEXT_SZ];

  snprintf(text, sizeof(text), "%s%s%s — ask about: ", who,
      f->class_label[0] != '\0' ? ", a " : "", f->class_label);

  for(uint8_t i = 0, shown = 0; i < f->n; i++)
  {
    if(f->facts[i].label[0] == '\0')
      continue;

    if(shown++ > 0)
      strlcat(text, " · ", sizeof(text));

    strlcat(text, f->facts[i].label, sizeof(text));
  }

  if(r->dense)
  {
    wiki_dense_reply(ctx, text);
    return;
  }

  wiki_wrap(ctx, text);
}

void
wiki_render_list(const cmd_ctx_t *ctx, const wiki_req_t *r,
    const wm_resolve_res_t *res)
{
  uint8_t cap = r->limit > 0 ? r->limit : WM_CANDIDATES_MAX;
  char    line[WIKI_LINE_SZ];
  char    head[WIKI_LINE_SZ];
  char    id[WIKI_CELL_SZ];
  char    label[WIKI_CELL_SZ];
  char    desc[WIKI_CELL_SZ];
  char    links[16];

  if(r->dense)
  {
    line[0] = '\0';

    for(uint8_t i = 0; i < res->n && i < cap; i++)
    {
      snprintf(id, sizeof(id), "%s%s (%s)", i > 0 ? ", " : "",
          res->hits[i].label, res->hits[i].qid);
      strlcat(line, id, sizeof(line));
    }

    wiki_dense_reply(ctx, line);
    return;
  }

  snprintf(head, sizeof(head), "%*s" CLR_BOLD "%-*s%-*s%-*s%*s" CLR_RESET,
      WIKI_INDENT, "", WIKI_LIST_QID, "id", WIKI_LIST_LABEL, "label",
      WIKI_LIST_DESC + 2, "description", WIKI_LIST_LINKS, "wikis");
  cmd_reply_table_head(ctx, head);

  for(uint8_t i = 0; i < res->n && i < cap; i++)
  {
    const wm_candidate_t *h = &res->hits[i];

    display_fit(h->qid, WIKI_LIST_QID - 1, id, sizeof(id), NULL);
    display_align_left(id, sizeof(id), WIKI_LIST_QID);

    display_fit(h->label, WIKI_LIST_LABEL - 2, label, sizeof(label), "…");
    display_align_left(label, sizeof(label), WIKI_LIST_LABEL);

    display_fit(h->description, WIKI_LIST_DESC, desc, sizeof(desc), "…");
    display_align_left(desc, sizeof(desc), WIKI_LIST_DESC + 2);

    snprintf(links, sizeof(links), "%" PRId32, h->sitelinks);
    display_align_right(links, sizeof(links), WIKI_LIST_LINKS);

    snprintf(line, sizeof(line), "%*s" CLR_WHITE "%s" CLR_RESET "%s"
        CLR_GRAY "%s%s" CLR_RESET,
        WIKI_INDENT, "", id, label, desc, links);
    cmd_reply(ctx, line);
  }
}

void
wiki_render_matches(const cmd_ctx_t *ctx, const wiki_req_t *r,
    const wm_resolve_res_t *res)
{
  uint8_t cap   = r->limit > 0 ? r->limit : WIKI_MATCHES_DEF;
  uint8_t shown = res->n < cap ? res->n : cap;
  char    text[WIKI_TEXT_SZ];
  char    head[WIKI_TEXT_SZ];

  snprintf(head, sizeof(head), "%s = %s (%s) — %" PRId32 " match%s%s",
      r->property, r->subj.label[0] != '\0' ? r->subj.label : r->subject,
      r->subj.qid, res->total, res->total == 1 ? "" : "es",
      res->total > shown ? ", best first" : "");

  text[0] = '\0';

  for(uint8_t i = 0; i < shown; i++)
  {
    if(i > 0)
      strlcat(text, " · ", sizeof(text));

    strlcat(text, res->hits[i].label[0] != '\0' ? res->hits[i].label
                                                : res->hits[i].qid,
        sizeof(text));
  }

  if(r->dense)
  {
    char one[WIKI_LINE_SZ];

    strlcpy(one, head, sizeof(one));
    strlcat(one, ": ", sizeof(one));
    strlcat(one, text, sizeof(one));
    wiki_dense_reply(ctx, one);
    return;
  }

  wiki_wrap(ctx, head);
  wiki_wrap(ctx, text);
}

// Whole years between two dates, counting the last birthday rather than
// the difference in years. A stamp too coarse to carry a month answers
// with the year difference and the caller says "about".
static long
wiki_years(long y1, long m1, long d1, long y2, long m2, long d2)
{
  long n = y2 - y1;

  if(m1 > 0 && m2 > 0 && (m2 < m1 || (m2 == m1 && d2 < d1)))
    n--;

  return(n);
}

void
wiki_render_age(const cmd_ctx_t *ctx, const wiki_req_t *r,
    wm_status_t death, const wm_value_t *died)
{
  const char *who = r->subj.label[0] != '\0' ? r->subj.label : r->qid;
  char        text[WIKI_TEXT_SZ];
  char        bornt[64];
  char        diedt[64];
  wm_value_t  bv;
  struct tm   now;
  time_t      t = time(NULL);
  long        by;
  long        bm;
  long        bd;
  bool        bc;

  memset(&bv, 0, sizeof(bv));
  bv.kind = WM_VAL_TIME;
  bv.time = r->born;
  wiki_value_text(&bv, bornt, sizeof(bornt));

  if(!wiki_time_ymd(&r->born, &by, &bm, &bd, &bc) || bc)
  {
    // Before the common era the arithmetic below is wrong and nobody
    // wants the answer anyway; the date itself is the honest reply.
    snprintf(text, sizeof(text), "%s — born %s.", who, bornt);
  }

  else if(death == WM_NOT_FOUND)
  {
    // Nothing recorded a death, so the count runs to today.
    gmtime_r(&t, &now);
    snprintf(text, sizeof(text), "%s — %s%ld, born %s.", who,
        r->born.precision >= 10 ? "" : "about ",
        wiki_years(by, bm, bd, now.tm_year + 1900L, now.tm_mon + 1L,
            now.tm_mday),
        bornt);
  }

  else if(death != WM_OK)
  {
    snprintf(text, sizeof(text), "%s — born %s; could not check whether "
        "they are still living.", who, bornt);
  }

  else if(died == NULL || died->kind != WM_VAL_TIME)
  {
    // A death is recorded and its date is not. Counting to today would
    // be a confident wrong answer, which is the one thing to avoid.
    snprintf(text, sizeof(text), "%s — born %s; dead, date not recorded.",
        who, bornt);
  }

  else
  {
    long dy;
    long dm;
    long dd;
    bool dbc;

    wiki_value_text(died, diedt, sizeof(diedt));
    wiki_time_ymd(&died->time, &dy, &dm, &dd, &dbc);
    snprintf(text, sizeof(text), "%s — died aged %ld (%s – %s).", who,
        wiki_years(by, bm, bd, dy, dm, dd), bornt, diedt);
  }

  if(r->dense)
  {
    wiki_dense_reply(ctx, text);
    return;
  }

  wiki_wrap(ctx, text);
  wiki_also(ctx, r);
}

// A failure a user can act on: which of the four it was, and what was
// being looked for when it happened.
void
wiki_render_status(const cmd_ctx_t *ctx, wm_status_t status,
    const char *message, const char *what)
{
  char line[WIKI_TEXT_SZ];

  switch(status)
  {
    case WM_NOT_FOUND:
      snprintf(line, sizeof(line), "Nothing on Wikidata for %s.", what);
      break;

    case WM_UNAVAILABLE:
      snprintf(line, sizeof(line), "The wikimedia plugin is going away; "
          "try %s again in a moment.", what);
      break;

    default:
      snprintf(line, sizeof(line), "Could not reach Wikidata for %s%s%s.",
          what, message != NULL && message[0] != '\0' ? ": " : "",
          message != NULL ? message : "");
      break;
  }

  cmd_reply(ctx, line);
}
