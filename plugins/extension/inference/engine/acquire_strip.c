// botmanager — MIT
// Acquisition engine: HTML-to-text stripper.
//
// Its own file because it is PURE — no allocator, no lock, no database,
// no config — so tests/test_acq_strip.c compiles it directly, the way
// knowledge_rank.c is compiled into its own suite. The image extractor
// it used to share acquire_html.c with reaches all four, and that is
// the whole reason for the seam.

#include "acquire_priv.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

// The elements whose content is markup machinery rather than prose, and
// which therefore go with their tags. Measured 2026-09-01 against three
// live pages a topic had actually fetched: eliding <script> bodies cut
// what this function emits by 97-99% on two of them, <style> took most
// of what remained on the third (75,161 -> 8,827 chars), and <noscript>
// a little more. <svg> and <template> were measured alongside and moved
// 0-80 bytes, so the set is three and not five.
static const char *const acq_raw_text_tags[] = {
  "script", "style", "noscript"
};

// If `pos` opens one of those elements, set `*end` to the index of the
// '>' closing its end tag and return true — the caller's loop steps past
// it. An unterminated element returns false and is then elided as an
// ordinary tag, so malformed markup costs one tag rather than every byte
// after it.
static bool
acq_raw_text_span(const char *in, size_t in_len, size_t pos, size_t *end)
{
  for(size_t t = 0; t < sizeof(acq_raw_text_tags)
      / sizeof(acq_raw_text_tags[0]); t++)
  {
    const char  *tag = acq_raw_text_tags[t];
    const size_t len = strlen(tag);
    char         after;
    size_t       j;

    if(pos + 1 + len >= in_len
        || strncasecmp(in + pos + 1, tag, len) != 0)
      continue;

    // A name boundary, so <scriptable> is not <script>.
    after = in[pos + 1 + len];

    if(after != '>' && after != '/' && after != ' ' && after != '\t'
        && after != '\r' && after != '\n')
      continue;

    // HTML ends a raw-text element at its first end tag, so there is no
    // nesting to track.
    for(j = pos + 1 + len; j + len + 2 <= in_len; j++)
    {
      if(in[j] != '<' || in[j + 1] != '/'
          || strncasecmp(in + j + 2, tag, len) != 0)
        continue;

      j += 2 + len;

      while(j < in_len && in[j] != '>')
        j++;

      if(j >= in_len)
        return(false);

      *end = j;
      return(true);
    }

    return(false);
  }

  return(false);
}

// Elements that do not imply a break in the text around them. Every
// OTHER tag is a word boundary, because `</p><p>` is two sentences and
// gluing them yields a token that is in no index and no vocabulary —
// `NewsSkip`, `MagicalSearch`, `contentWatch`. Measured 2026-09-01 on
// three live pages: 59, 64 and 125 such joins removed, for ~2% more
// output. ⚠ The list is the EXCEPTION, so an unknown or invented tag
// breaks — that is the safe way round, since a missed break corrupts a
// word and a spurious one costs a space.
static const char *const acq_inline_tags[] = {
  "a", "abbr", "b", "bdi", "bdo", "cite", "code", "em", "font", "i",
  "mark", "q", "s", "small", "span", "strong", "sub", "sup", "time",
  "u", "wbr"
};

// True if the tag opening at `pos` is one of those. `pos` indexes the
// '<'; an end tag's '/' is skipped, and anything that does not parse as
// a name at all is treated as a break.
static bool
acq_tag_is_inline(const char *in, size_t in_len, size_t pos)
{
  char   name[16];
  size_t n = 0;
  size_t j = pos + 1;

  if(j < in_len && in[j] == '/')
    j++;

  while(j < in_len && n + 1 < sizeof(name)
      && ((in[j] >= 'a' && in[j] <= 'z')
          || (in[j] >= 'A' && in[j] <= 'Z')
          || (in[j] >= '0' && in[j] <= '9')))
  {
    name[n++] = (in[j] >= 'A' && in[j] <= 'Z')
        ? (char)(in[j] + ('a' - 'A')) : in[j];
    j++;
  }

  name[n] = '\0';

  if(n == 0)
    return(false);

  for(size_t t = 0; t < sizeof(acq_inline_tags)
      / sizeof(acq_inline_tags[0]); t++)
    if(strcmp(name, acq_inline_tags[t]) == 0)
      return(true);

  return(false);
}

// Minimal in-place HTML-to-text stripper for digest input. Walks the
// input once:
//   - Tags (<...>) are elided, and a raw-text element takes its content
//     with it. ⚠ That content is not a rounding error: the digester
//     reads only the first `acquire.digest_body_truncate_chars` bytes
//     (6000), and a modern news page opens with far more inline script
//     than that — so before 2026-09-01 the digester saw the <title> and
//     then several kilobytes of cookie parsing, and the article itself
//     never reached it. The tell in the corpus was a digest describing
//     the page rather than the story.
//   - A small set of entities is decoded (&amp; &lt; &gt; &quot;
//     &apos; &nbsp; &#38; &#039;). Unknown entities copy through.
//   - Whitespace runs collapse to single spaces.
//   - Leading/trailing whitespace trimmed.
//
// The output is NUL-terminated, guaranteed <= out_cap - 1 bytes.
// Returns bytes written (excl. NUL).
size_t
acq_strip_html(const char *in, size_t in_len, char *out, size_t out_cap)
{
  size_t w;
  bool   last_ws;
  if(out_cap == 0) return(0);

  w = 0;
  last_ws = true;

  for(size_t i = 0; i < in_len && w + 1 < out_cap; i++)
  {
    unsigned char c = (unsigned char)in[i];

    // Tag elision. Both arms end at a word boundary unless the tag is
    // inline; the write below is safe because the loop's own condition
    // guarantees `w + 1 < out_cap` here.
    if(c == '<')
    {
      size_t span_end;
      bool   breaks;

      if(acq_raw_text_span(in, in_len, i, &span_end))
      {
        i      = span_end;          // the loop's i++ steps past the '>'
        breaks = true;              // raw text is never inline prose
      }

      else
      {
        breaks = !acq_tag_is_inline(in, in_len, i);

        while(i < in_len && in[i] != '>')
          i++;
      }

      if(breaks && !last_ws)
      {
        out[w++] = ' ';
        last_ws  = true;
      }

      continue;
    }

    // Entity decode (very small table).
    if(c == '&')
    {
      // Find the ';' within a short window.
      size_t end = i + 1;

      while(end < in_len && end - i < 8 && in[end] != ';')
        end++;

      if(end < in_len && in[end] == ';')
      {
        size_t len = end - i - 1;
        const char *ent = in + i + 1;
        char decoded = 0;

        if(len == 3 && strncasecmp(ent, "amp", 3) == 0)   decoded = '&';
        else if(len == 2 && strncasecmp(ent, "lt", 2) == 0)   decoded = '<';
        else if(len == 2 && strncasecmp(ent, "gt", 2) == 0)   decoded = '>';
        else if(len == 4 && strncasecmp(ent, "quot", 4) == 0) decoded = '"';
        else if(len == 4 && strncasecmp(ent, "apos", 4) == 0) decoded = '\'';
        else if(len == 4 && strncasecmp(ent, "nbsp", 4) == 0) decoded = ' ';
        else if(len >= 2 && ent[0] == '#')
        {
          // Numeric entity, common subset. ⚠ The hex form is not
          // exotic — `&#x27;` is how most CMSs write an apostrophe,
          // and decimal-only silently DELETED it, so every possessive
          // in the corpus read "Lipas". strtol stops at the ';'.
          long v = (ent[1] == 'x' || ent[1] == 'X')
              ? strtol(ent + 2, NULL, 16)
              : strtol(ent + 1, NULL, 10);

          if(v > 0 && v < 128) decoded = (char)v;
        }

        if(decoded != 0)
        {
          if(decoded == ' ' || decoded == '\t' || decoded == '\n')
          {
            if(!last_ws)
            {
              out[w++] = ' ';
              last_ws = true;
            }
          }

          else
          {
            out[w++] = decoded;
            last_ws  = false;
          }

          i = end;                // jump past ';'
          continue;
        }

        // Unknown entity — skip the whole &...; token to avoid noise.
        i = end;
        continue;
      }

      // No terminator — treat '&' as literal.
    }

    if(c == '\r' || c == '\n' || c == '\t' || c == ' ')
    {
      if(!last_ws)
      {
        out[w++] = ' ';
        last_ws  = true;
      }
      continue;
    }

    out[w++] = (char)c;
    last_ws  = false;
  }

  // Trim trailing whitespace.
  while(w > 0 && out[w - 1] == ' ')
    w--;

  out[w] = '\0';
  return(w);
}
