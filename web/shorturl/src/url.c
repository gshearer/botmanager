// botmanager — MIT
// shorturl: trust boundary — validates a destination URL before it can reach a header.

#include "url.h"

#include <string.h>

bool
url_valid(const char *url)
{
  size_t length;
  size_t i;

  if(!url || !*url)
    return(false);

  // strnlen, not strlen: an over-long value must be rejected, not measured.
  length = strnlen(url, SHORTURL_TARGET_MAX + 1);

  if(length > SHORTURL_TARGET_MAX)
    return(false);

  for(i = 0; i < length; i++)
  {
    unsigned char c = (unsigned char)url[i];

    if(c < 0x20 || c == 0x7f)
      return(false);
  }

  return(true);
}
