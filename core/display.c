// botmanager — MIT
// Column geometry shared by every table and card the bots draw.

#define DISPLAY_INTERNAL
#include "display.h"

#include "colors.h"

#include <stdio.h>
#include <string.h>

size_t
display_vis_len(const char *s)
{
  size_t n = 0;

  while(*s != '\0')
  {
    unsigned char c = (unsigned char)*s;

    if(c == '\x01' && s[1] != '\0')
    {
      s += 2;
      continue;
    }

    if((c & 0xc0) != 0x80)
      n++;

    s++;
  }

  return(n);
}

void
display_align_right(char *buf, size_t cap, int width)
{
  size_t vis = display_vis_len(buf);
  size_t raw = strlen(buf);
  int    pad = width - (int)vis;
  int    i;

  if(pad <= 0 || raw + (size_t)pad + 1 > cap)
    return;

  memmove(buf + pad, buf, raw + 1);

  for(i = 0; i < pad; i++)
    buf[i] = ' ';
}

void
display_align_left(char *buf, size_t cap, int width)
{
  size_t vis = display_vis_len(buf);
  size_t raw = strlen(buf);
  int    pad = width - (int)vis;
  int    i;

  if(pad <= 0)
    return;

  // Pad as far as the buffer allows. A cell too small for its own
  // column is already misaligned; stopping short of the terminator
  // keeps it a short cell rather than an overflow.
  if(raw + (size_t)pad + 1 > cap)
    pad = (int)(cap - raw - 1);

  for(i = 0; i < pad; i++)
    buf[raw + (size_t)i] = ' ';

  buf[raw + (size_t)pad] = '\0';
}

void
display_fit(const char *src, int cols, char *dst, size_t cap,
    const char *mark)
{
  size_t reserve = mark != NULL ? strlen(mark) + 1 : 1;
  size_t n       = 0;
  int    w       = 0;

  while(*src != '\0' && w < cols)
  {
    unsigned char c   = (unsigned char)*src;
    size_t        len = 1;
    size_t        k;

    if((c & 0xe0) == 0xc0)      len = 2;
    else if((c & 0xf0) == 0xe0) len = 3;
    else if((c & 0xf8) == 0xf0) len = 4;

    // A NUL inside the sequence — a name byte-truncated mid-glyph on
    // its way into a VARCHAR, or a provider's own truncation — bounds
    // len to the bytes actually there, so src never advances past the
    // terminator.
    for(k = 0; k < len; k++)
      if(src[k] == '\0')
      {
        len = k;
        break;
      }

    if(len == 0 || n + len + reserve > cap)
      break;

    for(k = 0; k < len; k++)
      dst[n++] = src[k];

    src += len;
    w++;
  }

  dst[n] = '\0';

  // `reserve` was held back on every copy above, so this fits unless
  // the caller's buffer was too small for the mark alone.
  if(*src != '\0' && mark != NULL && n + reserve <= cap)
    memcpy(dst + n, mark, reserve);
}

void
display_rule(char *line, size_t cap, int cols)
{
  size_t n = strlen(line);
  int    i;

  // Both markers and the terminator have to fit before a single glyph
  // is worth drawing. A rule wearing an opening gray and no closing
  // reset would bleed its color into everything printed after it, so
  // the two are all-or-nothing.
  if(n + (sizeof(CLR_GRAY) - 1) + sizeof(CLR_RESET) > cap)
    return;

  memcpy(line + n, CLR_GRAY, sizeof(CLR_GRAY) - 1);
  n += sizeof(CLR_GRAY) - 1;

  for(i = 0; i < cols && n + 3 + sizeof(CLR_RESET) <= cap; i++)
  {
    memcpy(line + n, "─", 3);
    n += 3;
  }

  memcpy(line + n, CLR_RESET, sizeof(CLR_RESET));
}

void
display_cat(char *line, size_t cap, const char *cell)
{
  size_t n = strlen(line);

  if(n + 1 < cap)
    snprintf(line + n, cap - n, "%s", cell);
}
