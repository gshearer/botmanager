// botmanager — MIT
// IRC line and prefix parsing. Its own translation unit because these
// two functions read bytes a peer chose and everything downstream —
// including the identity resolver — believes what they produce, so they
// are the tree's one IRC surface worth a test table of its own.
#define IRC_INTERNAL
#include "irc.h"

// Copy `len` bytes into a `sz`-byte field. False means they did not fit
// and the field is not what the peer sent.
static bool
irc_field_set(char *dst, size_t sz, const char *src, size_t len)
{
  if(len >= sz)
    return(false);

  memcpy(dst, src, len);
  dst[len] = '\0';

  return(true);
}

bool
irc_parse_prefix(const char *prefix, char *nick, char *user, char *host)
{
  const char *bang;
  const char *at;

  nick[0] = '\0';
  user[0] = '\0';
  host[0] = '\0';

  bang = strchr(prefix, '!');
  at   = strchr(prefix, '@');

  if(bang != NULL && at != NULL && at > bang)
  {
    // The three fields a user prefix carries are what an MFA pattern is
    // rebuilt from (irc.c's `nick!user@host`), so a shortened one is an
    // authority decision taken on bytes nobody sent.
    if(!irc_field_set(nick, IRC_NICK_SZ, prefix, (size_t)(bang - prefix))
        || !irc_field_set(user, IRC_NICK_SZ, bang + 1,
            (size_t)(at - bang - 1))
        || !irc_field_set(host, IRC_HOST_SZ, at + 1, strlen(at + 1)))
      return(FAIL);

    return(SUCCESS);
  }

  // A server name, or a malformed prefix: no identity is resolved from
  // it and the untruncated text stays in irc_parsed_msg_t.prefix, so a
  // name past IRC_NICK_SZ is shortened here rather than costing the
  // line — server names routinely run longer than a nick may.
  strlcpy(nick, prefix, IRC_NICK_SZ);

  return(SUCCESS);
}

bool
irc_parse_line(const char *line, irc_parsed_msg_t *out)
{
  const char *pos;

  memset(out, 0, sizeof(*out));

  pos = line;

  // Optional prefix.
  if(*pos == ':')
  {
    const char *end;

    pos++;
    end = strchr(pos, ' ');

    if(end == NULL)
      return(FAIL);

    // Sized to hold every legal prefix, so a refusal here means the peer
    // sent something no conforming one can — and shortening it would
    // hand a rewritten host to the identity resolver.
    if(!irc_field_set(out->prefix, IRC_PREFIX_SZ, pos, (size_t)(end - pos)))
    {
      clam(CLAM_WARN, "irc", "prefix over %d bytes, dropping line",
          IRC_PREFIX_SZ - 1);
      return(FAIL);
    }

    if(irc_parse_prefix(out->prefix, out->nick, out->user, out->host) != SUCCESS)
    {
      clam(CLAM_WARN, "irc", "prefix '%s' does not fit nick/user/host, "
          "dropping line", out->prefix);
      return(FAIL);
    }

    pos = end + 1;

    // Skip extra spaces.
    while(*pos == ' ')
      pos++;
  }

  // Command. A verb longer than the field is not one we could dispatch,
  // and a shortened one is a different verb.
  {
    const char *end = strchr(pos, ' ');
    size_t clen = (end != NULL) ? (size_t)(end - pos) : strlen(pos);

    if(!irc_field_set(out->command, sizeof(out->command), pos, clen))
    {
      clam(CLAM_WARN, "irc", "command over %zu bytes, dropping line",
          sizeof(out->command) - 1);
      return(FAIL);
    }

    if(end == NULL)
      return(SUCCESS);

    pos = end + 1;

    while(*pos == ' ')
      pos++;
  }

  // Params and trailing. These carry a message rather than an identity,
  // so an over-long line is delivered short and said so — the whole line
  // is bounded by IRC_BUF_SZ, not by the RFC's 512.
  {
    const char *trail = strstr(pos, " :");
    size_t plen;

    if(trail != NULL)
    {
      // Everything before " :" is params, after is trailing.
      plen = (size_t)(trail - pos);

      if(plen >= IRC_LINE_SZ)
      {
        plen = IRC_LINE_SZ - 1;
        clam(CLAM_WARN, "irc", "params truncated to %d bytes",
            IRC_LINE_SZ - 1);
      }

      memcpy(out->params, pos, plen);
      out->params[plen] = '\0';

      if(strlcpy(out->trailing, trail + 2, IRC_LINE_SZ) >= IRC_LINE_SZ)
        clam(CLAM_WARN, "irc", "trailing text truncated to %d bytes",
            IRC_LINE_SZ - 1);

      out->has_trailing = true;
    }

    else if(*pos == ':')
    {
      // No params, just trailing.
      if(strlcpy(out->trailing, pos + 1, IRC_LINE_SZ) >= IRC_LINE_SZ)
        clam(CLAM_WARN, "irc", "trailing text truncated to %d bytes",
            IRC_LINE_SZ - 1);

      out->has_trailing = true;
    }

    else
    {
      // All remaining text is params.
      if(strlcpy(out->params, pos, IRC_LINE_SZ) >= IRC_LINE_SZ)
        clam(CLAM_WARN, "irc", "params truncated to %d bytes",
            IRC_LINE_SZ - 1);
    }
  }

  return(SUCCESS);
}
