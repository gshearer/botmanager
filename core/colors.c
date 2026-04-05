#include "colors.h"

#include <string.h>

// Look up the native color string for an abstract marker identifier.
// id: marker character following \x01
// ct: color table (must not be NULL)
// returns: native color string, or NULL if unrecognized
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
// dst: output buffer
// dst_sz: size of output buffer
// src: input string with abstract color markers
// ct: color table, or NULL to strip markers
// returns: bytes written to dst (excluding NUL)
size_t
color_translate(char *dst, size_t dst_sz, const char *src,
    const color_table_t *ct)
{
  if(dst == NULL || dst_sz == 0)
    return(0);

  size_t di = 0;
  size_t cap = dst_sz - 1;

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
