// botmanager — MIT
// mIRC/ANSI colour-code escape helpers for formatted output.
#include "colors.h"

#include <stdbool.h>
#include <string.h>
#include <strings.h>

// Look up the native color string for an abstract marker identifier.
// ct: color table (must not be NULL)
static const char *
color_lookup(char id, const color_table_t *ct)
{
  switch(id)
  {
    case 'R': return(ct->red);
    case 'G': return(ct->green);
    case 'Y': return(ct->yellow);
    case 'B': return(ct->blue);
    case 'P': return(ct->purple);
    case 'C': return(ct->cyan);
    case 'W': return(ct->white);
    case 'O': return(ct->orange);
    case 'A': return(ct->gray);
    case 'b': return(ct->bold);
    case 'X': return(ct->reset);
    default:  return(NULL);
  }
}

// Translate abstract color markers (\x01 + id) in src to native color
// strings using the given color table. If ct is NULL, markers are
// stripped (replaced with nothing). Always NUL-terminates dst.
size_t
color_translate(char *dst, size_t dst_sz, const char *src,
    const color_table_t *ct)
{
  size_t di = 0;
  size_t cap;

  if(dst == NULL || dst_sz == 0)
    return(0);

  cap = dst_sz - 1;

  for(size_t si = 0; src[si] != '\0'; si++)
  {
    if(src[si] == '\x01' && src[si + 1] != '\0')
    {
      si++;

      if(ct != NULL)
      {
        const char *seq = color_lookup(src[si], ct);

        if(seq != NULL)
        {
          size_t len = strlen(seq);

          if(di + len > cap)
            len = cap - di;

          memcpy(dst + di, seq, len);
          di += len;
        }
      }

      continue;
    }

    if(di < cap)
      dst[di++] = src[si];
  }

  dst[di] = '\0';
  return(di);
}

// The markup an LLM can type, and the abstract marker each maps to.
// Asked for a control byte a model reproduces the *notation* instead —
// the six characters \033[1m, not ESC — so the wire needs a vocabulary
// made of ordinary printable text. Markdown bold is what models already
// emit unprompted; the colour tags mirror the abstract table above.
static const struct
{
  const char *name;
  char        id;
} color_markup_names[] = {
  { "red",    'R' },
  { "green",  'G' },
  { "yellow", 'Y' },
  { "blue",   'B' },
  { "purple", 'P' },
  { "cyan",   'C' },
  { "white",  'W' },
  { "orange", 'O' },
  { "gray",   'A' },
  { "grey",   'A' },
};

// Recognise a markup token at p. On a match *id receives the abstract
// marker identifier ('b' bold toggle, 'X' close, else a colour) and
// *len the token's byte length.
static bool
color_markup_token(const char *p, char *id, size_t *len)
{
  const size_t n = sizeof(color_markup_names) / sizeof(color_markup_names[0]);

  if(p[0] == '*' && p[1] == '*')
  {
    *id  = 'b';
    *len = 2;
    return(true);
  }

  if(p[0] != '<')
    return(false);

  // A closing tag resets everything; which colour it names doesn't
  // matter, only that it names one — so real text like "</dev/null"
  // passes through untouched.
  if(p[1] == '/')
  {
    for(size_t i = 0; i < n; i++)
    {
      size_t nl = strlen(color_markup_names[i].name);

      if(strncasecmp(p + 2, color_markup_names[i].name, nl) == 0
          && p[2 + nl] == '>')
      {
        *id  = 'X';
        *len = nl + 3;
        return(true);
      }
    }

    return(false);
  }

  for(size_t i = 0; i < n; i++)
  {
    size_t nl = strlen(color_markup_names[i].name);

    if(strncasecmp(p + 1, color_markup_names[i].name, nl) == 0
        && p[1 + nl] == '>')
    {
      *id  = color_markup_names[i].id;
      *len = nl + 2;
      return(true);
    }
  }

  return(false);
}

size_t
color_markup_translate(char *dst, size_t dst_sz, const char *src)
{
  size_t di = 0;
  size_t cap;

  if(dst == NULL || dst_sz == 0)
    return(0);

  if(src == NULL)
  {
    dst[0] = '\0';
    return(0);
  }

  cap = dst_sz - 1;

  for(size_t si = 0; src[si] != '\0'; )
  {
    char   id;
    size_t len;

    if(color_markup_token(src + si, &id, &len))
    {
      if(di + 2 > cap)
        break;

      dst[di++] = '\x01';
      dst[di++] = id;
      si += len;
      continue;
    }

    // A bare \x01 in generated text would read as a marker downstream
    // and swallow the byte after it. Drop it: the text must not be
    // able to forge formatting or eat its own characters.
    if(src[si] == '\x01')
    {
      si++;
      continue;
    }

    if(di >= cap)
      break;

    dst[di++] = src[si++];
  }

  dst[di] = '\0';
  return(di);
}
