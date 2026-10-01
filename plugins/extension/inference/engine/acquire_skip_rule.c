// botmanager — MIT
// Acquisition engine: page-skip rules that need no state (host, verdict).

#include "acquire_priv.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

// The URL is a search result, so it is whatever the indexed page said it
// was. The host is cut from the authority alone — everything before the
// first '/', '?' or '#' — so a path or query can never supply it, and an
// '@' there cannot either; only one inside the authority ends a userinfo.
bool
acq_url_host(const char *url, char *out, size_t out_cap)
{
  const char *auth;
  const char *end;
  const char *h_end;
  size_t      len;

  if(out_cap == 0)
    return(false);

  out[0] = '\0';

  if(strncasecmp(url, "http://", 7) == 0)
    auth = url + 7;

  else if(strncasecmp(url, "https://", 8) == 0)
    auth = url + 8;

  else
    return(false);

  end = auth + strcspn(auth, "/?#");

  for(const char *p = auth; p < end; p++)
    if(*p == '@')
      auth = p + 1;

  if(auth < end && *auth == '[')
  {
    h_end = memchr(auth, ']', (size_t)(end - auth));

    if(h_end == NULL)
      return(false);

    h_end++;
  }

  else
  {
    h_end = memchr(auth, ':', (size_t)(end - auth));

    if(h_end == NULL)
      h_end = end;
  }

  // One trailing dot is the fully-qualified spelling of the same host.
  if(h_end > auth && h_end[-1] == '.')
    h_end--;

  len = (size_t)(h_end - auth);

  if(len == 0 || len >= out_cap)
    return(false);

  // The key goes to the DB, the log and an IRC reply, so a byte no
  // hostname carries (a control code, a colour code, a space) refuses
  // the URL here rather than travelling. ':' and the brackets reach
  // this point only inside an IPv6 literal, and are refused outside one.
  for(size_t i = 0; i < len; i++)
  {
    unsigned char c = (unsigned char)auth[i];

    if(isalnum(c) || c == '.' || c == '-' || c == '_')
      continue;

    if(*auth == '[' && (c == ':' || c == '[' || c == ']'))
      continue;

    return(false);
  }

  for(size_t i = 0; i < len; i++)
    out[i] = (char)tolower((unsigned char)auth[i]);

  out[len] = '\0';
  return(true);
}

// A 401/403/451 is the host refusing us, not one page being missing, and
// a 200 that strips to almost nothing is the same refusal in a polite
// shape — msn.com served the three bytes "MSN" for every article. A 404
// names one URL and a 429 clears with time, so neither counts.
acq_fetch_verdict_t
acq_fetch_verdict(long curl_code, long status, size_t strip_len,
    uint32_t min_usable)
{
  if(curl_code != 0)
    return(ACQ_FETCH_TRANSIENT);

  if(status == 401 || status == 403 || status == 451)
    return(ACQ_FETCH_UNUSABLE);

  if(status != 200)
    return(ACQ_FETCH_TRANSIENT);

  if(strip_len == 0 || strip_len < min_usable)
    return(ACQ_FETCH_UNUSABLE);

  return(ACQ_FETCH_USABLE);
}
