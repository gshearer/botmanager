// botmanager — MIT
// shorturl: trust boundary — untrusted bytes into a validated token_t.

#define TOKEN_INTERNAL

#include "token.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/random.h>

static const char base62_alphabet[] =
  "0123456789"
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
  "abcdefghijklmnopqrstuvwxyz";

// 62 * 4 = 248. Rejecting 248..255 keeps the modulus below uniform; taking the
// remainder of a full byte would bias the first six characters of the alphabet.
#define BASE62_REJECT_FROM 248

static bool
base62_is_member(unsigned char c)
{
  return((c >= '0' && c <= '9') ||
         (c >= 'A' && c <= 'Z') ||
         (c >= 'a' && c <= 'z'));
}

bool
token_parse(const char *raw, token_t *out)
{
  size_t i;

  if(!raw)
    return(false);

  // strnlen, not strlen: raw is not known to be terminated, and inspecting one
  // byte past the expected length is what distinguishes an exact-length token
  // from a longer string sharing its prefix.
  if(strnlen(raw, SHORTURL_TOKEN_LEN + 1) != SHORTURL_TOKEN_LEN)
    return(false);

  for(i = 0; i < SHORTURL_TOKEN_LEN; i++)
    if(!base62_is_member((unsigned char)raw[i]))
      return(false);

  memcpy(out->s, raw, SHORTURL_TOKEN_LEN);
  out->s[SHORTURL_TOKEN_LEN] = '\0';

  return(true);
}

bool
token_generate(token_t *out)
{
  uint8_t entropy[SHORTURL_TOKEN_LEN * 2];
  size_t filled = 0;

  while(filled < SHORTURL_TOKEN_LEN)
  {
    ssize_t got;
    ssize_t i;

    got = getrandom(entropy, sizeof entropy, 0);

    if(got < 0)
    {
      if(errno == EINTR)
        continue;

      return(false);
    }

    for(i = 0; i < got && filled < SHORTURL_TOKEN_LEN; i++)
    {
      if(entropy[i] >= BASE62_REJECT_FROM)
        continue;

      out->s[filled++] = base62_alphabet[entropy[i] % 62];
    }
  }

  out->s[SHORTURL_TOKEN_LEN] = '\0';

  return(true);
}
