#include "validate.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// Validate that str contains only alphanumeric characters and underscores.
// returns: true if valid, false if NULL, empty, or contains invalid chars
// str: NUL-terminated string to validate
// maxlen: maximum allowed length (0 = no limit)
bool
validate_alnum(const char *str, size_t maxlen)
{
  if(str == NULL || str[0] == '\0')
    return false;

  size_t len = 0;

  for(const char *p = str; *p != '\0'; p++)
  {
    if(!isalnum((unsigned char)*p) && *p != '_')
      return false;

    len++;

    if(maxlen > 0 && len > maxlen)
      return false;
  }

  return true;
}

// Validate that str contains only ASCII digits within the given length bounds.
// returns: true if valid, false if NULL, empty, too short, or too long
// str: NUL-terminated string to validate
// minlen: minimum required length
// maxlen: maximum allowed length (0 = no limit)
bool
validate_digits(const char *str, size_t minlen, size_t maxlen)
{
  if(str == NULL || str[0] == '\0')
    return false;

  size_t len = 0;

  for(const char *p = str; *p != '\0'; p++)
  {
    if(*p < '0' || *p > '9')
      return false;

    len++;

    if(maxlen > 0 && len > maxlen)
      return false;
  }

  return len >= minlen;
}

// Validate that str is a valid hostname (alphanumeric, dots, hyphens, colons).
// returns: true if valid, false if NULL, empty, or contains invalid chars
// str: NUL-terminated string to validate
bool
validate_hostname(const char *str)
{
  if(str == NULL || str[0] == '\0')
    return false;

  for(const char *p = str; *p != '\0'; p++)
  {
    if(!isalnum((unsigned char)*p) && *p != '.' && *p != '-' && *p != ':')
      return false;
  }

  return true;
}

// Parse and validate a decimal port number string in the range 1-65535.
// returns: true if valid, false if NULL, empty, non-numeric, or out of range
// str: NUL-terminated string to validate
// out: if non-NULL, receives the parsed port number on success
bool
validate_port(const char *str, uint16_t *out)
{
  if(str == NULL || str[0] == '\0')
    return false;

  for(const char *p = str; *p != '\0'; p++)
  {
    if(*p < '0' || *p > '9')
      return false;
  }

  long val = strtol(str, NULL, 10);

  if(val < 1 || val > 65535)
    return false;

  if(out != NULL)
    *out = (uint16_t)val;

  return true;
}

// Validate an IRC channel name: rejects spaces, control chars, and commas.
// returns: true if valid, false if NULL, empty, or contains forbidden chars
// str: NUL-terminated channel name to validate
bool
validate_irc_channel(const char *str)
{
  if(str == NULL || str[0] == '\0')
    return false;

  for(const char *p = str; *p != '\0'; p++)
  {
    if(*p <= 0x20 || *p == 0x07 || *p == ',')
      return false;
  }

  return true;
}
