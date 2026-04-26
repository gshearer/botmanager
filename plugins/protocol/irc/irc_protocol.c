// botmanager — MIT
// IRC protocol parsing, raw-line formatters, template expansion.
#define IRC_INTERNAL
#include "irc.h"

// IRC protocol parsing

void
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
    size_t nlen = (size_t)(bang - prefix);
    size_t ulen = (size_t)(at - bang - 1);

    if(nlen >= IRC_NICK_SZ) nlen = IRC_NICK_SZ - 1;
    if(ulen >= IRC_NICK_SZ) ulen = IRC_NICK_SZ - 1;

    memcpy(nick, prefix, nlen);
    nick[nlen] = '\0';

    memcpy(user, bang + 1, ulen);
    user[ulen] = '\0';

    snprintf(host, IRC_HOST_SZ, "%s", at + 1);
  }

  else
    // Server name only, or malformed prefix.
    snprintf(nick, IRC_NICK_SZ, "%s", prefix);
}

void
irc_parse_line(const char *line, irc_parsed_msg_t *out)
{
  const char *pos;

  memset(out, 0, sizeof(*out));

  pos = line;

  // Optional prefix.
  if(*pos == ':')
  {
    const char *end;
    size_t plen;

    pos++;
    end = strchr(pos, ' ');

    if(end == NULL)
      return;

    plen = (size_t)(end - pos);

    if(plen >= IRC_PREFIX_SZ) plen = IRC_PREFIX_SZ - 1;

    memcpy(out->prefix, pos, plen);
    out->prefix[plen] = '\0';

    irc_parse_prefix(out->prefix, out->nick, out->user, out->host);
    pos = end + 1;

    // Skip extra spaces.
    while(*pos == ' ')
      pos++;
  }

  // Command.
  {
    const char *end = strchr(pos, ' ');
    size_t clen;

    if(end == NULL)
    {
      strncpy(out->command, pos, sizeof(out->command) - 1);
      return;
    }

    clen = (size_t)(end - pos);

    if(clen >= sizeof(out->command)) clen = sizeof(out->command) - 1;

    memcpy(out->command, pos, clen);
    out->command[clen] = '\0';
    pos = end + 1;

    while(*pos == ' ')
      pos++;
  }

  // Params and trailing.
  {
    const char *trail = strstr(pos, " :");
    size_t plen;

    if(trail != NULL)
    {
      // Everything before " :" is params, after is trailing.
      plen = (size_t)(trail - pos);

      if(plen >= IRC_LINE_SZ) plen = IRC_LINE_SZ - 1;

      memcpy(out->params, pos, plen);
      out->params[plen] = '\0';

      strncpy(out->trailing, trail + 2, IRC_LINE_SZ - 1);
      out->trailing[IRC_LINE_SZ - 1] = '\0';
      out->has_trailing = true;
    }

    else if(*pos == ':')
    {
      // No params, just trailing.
      strncpy(out->trailing, pos + 1, IRC_LINE_SZ - 1);
      out->trailing[IRC_LINE_SZ - 1] = '\0';
      out->has_trailing = true;
    }

    else
    {
      // All remaining text is params.
      strncpy(out->params, pos, IRC_LINE_SZ - 1);
      out->params[IRC_LINE_SZ - 1] = '\0';
    }
  }
}

// PRIVMSG builder with 512-byte line splitting (IRC RFC 2812).
// Raw IRC line send: format, append CRLF, write to the session.
// Callers use irc_send_privmsg / irc_send_emote for IRC-specific wrappers;
// this helper exists for bare protocol commands (NICK, USER, JOIN, ...).
bool
irc_send_raw(irc_state_t *st, const char *fmt, ...)
{
  char line[IRC_LINE_SZ];
  va_list ap;
  size_t total;
  int n;

  va_start(ap, fmt);
  n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
  va_end(ap);

  if(n < 0)
    return(FAIL);

  if((size_t)n > sizeof(line) - 3)
    n = (int)(sizeof(line) - 3);

  line[n]     = '\r';
  line[n + 1] = '\n';
  line[n + 2] = '\0';

  total = (size_t)(n + 2);

  if(st->session == NULL || !st->connected)
    return(FAIL);

  if(sock_send(st->session, line, total) != SUCCESS)
  {
    clam(CLAM_WARN, "irc", "send failed");
    return(FAIL);
  }

  clam(CLAM_DEBUG3, "irc", ">> %.*s", n, line);
  return(SUCCESS);
}

bool
irc_send_privmsg(irc_state_t *st, const char *target, const char *text)
{
  // Overhead: "PRIVMSG <target> :<text>\r\n"
  // 10 = strlen("PRIVMSG ") + strlen(" :") = 8 + 2
  size_t overhead = 10 + strlen(target) + 2;  // +2 for \r\n
  size_t max_text = 510 - 10 - strlen(target);
  size_t text_len;
  const char *pos;
  size_t remaining;

  if(max_text < 1 || overhead > 510)
  {
    clam(CLAM_WARN, "irc", "target too long for PRIVMSG: '%s'", target);
    return(FAIL);
  }

  text_len = strlen(text);

  if(text_len <= max_text)
    return(irc_send_raw(st, "PRIVMSG %s :%s", target, text));

  // Split into multiple messages.
  pos = text;
  remaining = text_len;

  while(remaining > 0)
  {
    size_t chunk = remaining;
    char buf[IRC_LINE_SZ];
    size_t copy;

    if(chunk > max_text)
    {
      size_t last_space;

      chunk = max_text;

      // Try to split on a space boundary.
      last_space = chunk;

      while(last_space > 0 && pos[last_space - 1] != ' ')
        last_space--;

      if(last_space > max_text / 2)
        chunk = last_space;
    }

    copy = chunk;

    if(copy >= sizeof(buf))
      copy = sizeof(buf) - 1;

    memcpy(buf, pos, copy);
    buf[copy] = '\0';

    if(irc_send_raw(st, "PRIVMSG %s :%s", target, buf) != SUCCESS)
      return(FAIL);

    pos += chunk;
    remaining -= chunk;
  }

  return(SUCCESS);
}

// Send PASS, NICK, USER registration commands.
void
irc_send_registration(irc_state_t *st)
{
  if(st->pass[0] != '\0')
    irc_send_raw(st, "PASS %s", st->pass);

  strncpy(st->cur_nick, st->nick, IRC_NICK_SZ - 1);
  st->cur_nick[IRC_NICK_SZ - 1] = '\0';

  irc_send_raw(st, "NICK %s", st->cur_nick);
  irc_send_raw(st, "USER %s 0 * :%s", st->user, st->realname);
}

void
irc_expand_vars(const char *tmpl, char *out, size_t out_sz,
    const irc_state_t *st, const char *channel)
{
  // Derive bot name from inst_name by stripping "_irc" suffix.
  char botname[METHOD_NAME_SZ] = {0};
  const char *suffix = "_irc";
  size_t nlen = strlen(st->inst_name);
  size_t slen = strlen(suffix);
  size_t pos = 0;
  const char *p;

  if(nlen > slen &&
      strcmp(st->inst_name + nlen - slen, suffix) == 0)
  {
    memcpy(botname, st->inst_name, nlen - slen);
    botname[nlen - slen] = '\0';
  }

  else
    strncpy(botname, st->inst_name, METHOD_NAME_SZ - 1);

  p = tmpl;

  while(*p != '\0' && pos < out_sz - 1)
  {
    if(p[0] == '$' && p[1] == '{')
    {
      const char *end = strchr(p + 2, '}');
      size_t vlen;
      const char *replacement = NULL;

      if(end != NULL)
      {
        vlen = (size_t)(end - p - 2);

        if(vlen == 4 && strncmp(p + 2, "name", 4) == 0)
          replacement = botname;
        else if(vlen == 7 && strncmp(p + 2, "version", 7) == 0)
          replacement = BM_VERSION_STR;
        else if(vlen == 4 && strncmp(p + 2, "nick", 4) == 0)
          replacement = st->cur_nick;
        else if(vlen == 7 && strncmp(p + 2, "channel", 7) == 0)
          replacement = channel;

        if(replacement != NULL)
        {
          size_t rlen = strlen(replacement);

          if(pos + rlen < out_sz)
          {
            memcpy(out + pos, replacement, rlen);
            pos += rlen;
          }

          p = end + 1;
          continue;
        }
      }
    }

    out[pos++] = *p++;
  }

  out[pos] = '\0';
}
