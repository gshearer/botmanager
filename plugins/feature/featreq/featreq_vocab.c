// botmanager — MIT
// featreq vocabulary: the two closed word lists this plugin stores,
// parses and prints, and the scanner that cuts a command line into
// candidates for them. One table per axis, and every consumer — the
// DDL's defaults, the command parsers, the renderers and the ORDER BY —
// reads its words from here, so the board cannot be sorted by a word it
// cannot print.

#define FEATREQ_INTERNAL
#include "featreq.h"

#include "colors.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef struct
{
  const char *word;    // stored in the DB, and printed
  const char *alt;     // a second accepted spelling, or NULL
  const char *color;
  bool        closed;  // statuses only: the request is finished with
} fr_word_t;

// Indexed by fr_type_t. `feat` rather than `new` because `new` is the
// status every row starts in, and one word cannot mean both axes.
static const fr_word_t fr_types[] = {
  { "feat",   NULL,     CLR_GREEN,  false },
  { "bug",    NULL,     CLR_RED,    false },
  { "change", "chg",    CLR_ORANGE, false },
};

// Indexed by fr_status_t. `alt` is an extra spelling the parser accepts;
// where the stored word carries a space it is also the ONLY spelling
// the command line can use, because an argument is one token.
//
// `closed` is what the default board leaves out. It lives here rather
// than in a test on the enum so that the statuses a request can END in
// are declared exactly once, beside the words themselves.
static const fr_word_t fr_statuses[] = {
  { "new",         NULL,        CLR_CYAN,   false },
  { "in progress", "in-prog",   CLR_YELLOW, false },
  { "on hold",     "on-hold",   CLR_ORANGE, false },
  { "completed",   "done",      CLR_GREEN,  true  },
  { "canceled",    "cancelled", CLR_GRAY,   true  },
};

#define FR_N_TYPES     (sizeof(fr_types)    / sizeof(fr_types[0]))
#define FR_N_STATUSES  (sizeof(fr_statuses) / sizeof(fr_statuses[0]))

// ------------------------------------------------------------------ //
// Lookup                                                              //
// ------------------------------------------------------------------ //

// The parsers below are the only place an fr_type_t or fr_status_t is
// made from anything outside this file, so an enum reaching these
// accessors is already one of the values its table lists. No accessor
// re-checks the index it was handed.

const char *
fr_type_word(fr_type_t t)
{
  return(fr_types[t].word);
}

const char *
fr_type_color(fr_type_t t)
{
  return(fr_types[t].color);
}

const char *
fr_status_word(fr_status_t s)
{
  return(fr_statuses[s].word);
}

// How a word is written as one command-line argument: itself, unless a
// space in it makes that impossible, in which case the alt is not an
// extra spelling but the only one.
static const char *
fr_arg_spelling(const fr_word_t *w)
{
  return((strchr(w->word, ' ') != NULL) ? w->alt : w->word);
}

const char *
fr_status_token(fr_status_t s)
{
  return(fr_arg_spelling(&fr_statuses[s]));
}

const char *
fr_status_color(fr_status_t s)
{
  return(fr_statuses[s].color);
}

// ------------------------------------------------------------------ //
// Parsing — the boundary every user-supplied word crosses             //
// ------------------------------------------------------------------ //

static bool
fr_word_find(const fr_word_t *tab, size_t n, const char *s, size_t *out)
{
  size_t i;

  if(s == NULL || s[0] == '\0')
    return(false);

  for(i = 0; i < n; i++)
    if(strcasecmp(tab[i].word, s) == 0
        || (tab[i].alt != NULL && strcasecmp(tab[i].alt, s) == 0))
    {
      *out = i;
      return(true);
    }

  return(false);
}

bool
fr_type_parse(const char *s, fr_type_t *out)
{
  size_t i;

  if(!fr_word_find(fr_types, FR_N_TYPES, s, &i))
    return(false);

  *out = (fr_type_t)i;
  return(true);
}

bool
fr_status_parse(const char *s, fr_status_t *out)
{
  size_t i;

  if(!fr_word_find(fr_statuses, FR_N_STATUSES, s, &i))
    return(false);

  *out = (fr_status_t)i;
  return(true);
}

// Unlike the type and status vocabularies this one is not stored, only
// parsed, so it is a plain list rather than a table: nothing else in the
// plugin needs to render a sort back out except fr_sort_tokens below.
static const struct
{
  const char *word;
  fr_sort_t   sort;
} fr_sorts[] = {
  { "new",    FR_SORT_NEW    },
  { "old",    FR_SORT_OLD    },
  { "status", FR_SORT_STATUS },
};

#define FR_N_SORTS  (sizeof(fr_sorts) / sizeof(fr_sorts[0]))

bool
fr_sort_parse(const char *s, fr_sort_t *out)
{
  size_t i;

  if(s == NULL || s[0] == '\0')
    return(false);

  for(i = 0; i < FR_N_SORTS; i++)
    if(strcasecmp(fr_sorts[i].word, s) == 0)
    {
      *out = fr_sorts[i].sort;
      return(true);
    }

  return(false);
}

void
fr_sort_tokens(char *out, size_t cap)
{
  size_t used = 0;
  size_t i;

  out[0] = '\0';

  for(i = 0; i < FR_N_SORTS && used + 1 < cap; i++)
  {
    used += (size_t)snprintf(out + used, cap - used, "%s%s",
        (i > 0) ? ", " : "", fr_sorts[i].word);

    if(used >= cap)
      break;
  }
}

// ------------------------------------------------------------------ //
// Rendering the vocabulary itself                                     //
// ------------------------------------------------------------------ //

// The spellings to advertise, joined by `sep`. Only what a caller can
// actually type goes in the list; the extra spellings the parser also
// accepts are in the commands' help_long, not in an error message.
static void
fr_tokens_join(const fr_word_t *tab, size_t n, const char *sep, char *out,
    size_t cap)
{
  size_t used = 0;
  size_t i;

  out[0] = '\0';

  for(i = 0; i < n && used + 1 < cap; i++)
  {
    used += (size_t)snprintf(out + used, cap - used, "%s%s",
        (i > 0) ? sep : "", fr_arg_spelling(&tab[i]));

    if(used >= cap)
      break;
  }
}

void
fr_type_tokens(char *out, size_t cap)
{
  fr_tokens_join(fr_types, FR_N_TYPES, ", ", out, cap);
}

void
fr_status_tokens(char *out, size_t cap)
{
  fr_tokens_join(fr_statuses, FR_N_STATUSES, ", ", out, cap);
}

// The same lists as a syntax placeholder — `<new|in-prog|…>` — for a
// usage line that would otherwise say `<status>` and leave the caller
// to guess. Pipe-joined, because that is what a usage line means by it.
static void
fr_syntax_join(const fr_word_t *tab, size_t n, char *out, size_t cap)
{
  char tokens[FR_SYNTAX_SZ];

  fr_tokens_join(tab, n, "|", tokens, sizeof(tokens));
  snprintf(out, cap, "<%s>", tokens);
}

void
fr_type_syntax(char *out, size_t cap)
{
  fr_syntax_join(fr_types, FR_N_TYPES, out, cap);
}

void
fr_status_syntax(char *out, size_t cap)
{
  fr_syntax_join(fr_statuses, FR_N_STATUSES, out, cap);
}

// The open board, as SQL: every status NOT marked closed above is one
// the operator still has something to do about. Rendered as a NOT IN
// over the closed words rather than an IN over the open ones — the
// closed set is the one that is defined by having ended, so a status
// added later is open until it says otherwise.
//
// The literals are safe for the same reason fr_status_case_sql's are:
// they are this file's own constants and none came from a user.
const char *
fr_status_open_sql(char *out, size_t cap)
{
  size_t used = 0;
  size_t i;
  size_t n    = 0;

  used = strlcpy(out, "status NOT IN (", cap);

  for(i = 0; i < FR_N_STATUSES && used < cap; i++)
  {
    if(!fr_statuses[i].closed)
      continue;

    used += (size_t)snprintf(out + used, cap - used, "%s'%s'",
        (n++ > 0) ? ", " : "", fr_statuses[i].word);
  }

  if(used < cap)
    snprintf(out + used, cap - used, ")");

  return(out);
}

// The status ordering, as SQL. Every word here is one of this file's
// own constants — none contains a quote and none came from a user — so
// they are literals in the statement rather than bound parameters,
// which a CASE arm cannot be anyway.
const char *
fr_status_case_sql(char *out, size_t cap)
{
  size_t used = 0;
  size_t i;

  used = strlcpy(out, "CASE status", cap);

  for(i = 0; i < FR_N_STATUSES && used < cap; i++)
    used += (size_t)snprintf(out + used, cap - used, " WHEN '%s' THEN %zu",
        fr_statuses[i].word, i);

  if(used < cap)
    snprintf(out + used, cap - used, " ELSE %zu END", FR_N_STATUSES);

  return(out);
}

// ------------------------------------------------------------------ //
// Argument scanning                                                   //
// ------------------------------------------------------------------ //

const char *
fr_skip_ws(const char *p)
{
  while(*p == ' ' || *p == '\t')
    p++;

  return(p);
}

const char *
fr_token(const char *p, char *out, size_t cap)
{
  size_t n = 0;

  while(*p != '\0' && *p != ' ' && *p != '\t')
  {
    if(n + 1 < cap)
      out[n++] = *p;

    p++;
  }

  out[n] = '\0';
  return(p);
}

// A token that is a bare run of digits, and not empty — how both
// surfaces recognise an id among the flags around it.
bool
fr_all_digits(const char *s)
{
  size_t i;

  for(i = 0; s[i] != '\0'; i++)
    if(s[i] < '0' || s[i] > '9')
      return(false);

  return(i > 0);
}
