// botmanager — MIT
// IRC raw-line formatters and template expansion. Line parsing lives in
// irc_parse.c.
#define IRC_INTERNAL
#include "irc.h"

// The trust boundary for everything this driver puts on the wire. A CR or
// an LF inside the formatted body ends the command there and hands the
// server the rest as a further one — from a bot that holds OPER — so they
// are folded to spaces. Nothing arrives here holding a line break on
// purpose: irc_send_raw supplies the terminator itself.
//
// Only those two, and deliberately not the whole control range
// sig_reason_sanitize folds for the QUIT reason: colour (\x03), bold
// (\x02), reset (\x0f) and the CTCP markers (\x01) are put in by
// color_translate and irc_send_emote on purpose and have to reach the
// channel intact. Returns how many bytes were folded.
static int
irc_fold_line_breaks(char *line, int len)
{
  int folded = 0;

  for(int i = 0; i < len; i++)
    if(line[i] == '\r' || line[i] == '\n')
    {
      line[i] = ' ';
      folded++;
    }

  return(folded);
}

// Raw IRC line send: format, append CRLF, write to the session.
// Callers use irc_send_privmsg / irc_send_emote for IRC-specific wrappers;
// this helper exists for bare protocol commands (NICK, USER, JOIN, ...).
bool
irc_send_raw(irc_state_t *st, const char *fmt, ...)
{
  char line[IRC_LINE_SZ];
  sock_session_t *s;
  va_list ap;
  size_t total;
  int folded;
  int n;
  bool rc;

  va_start(ap, fmt);
  n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
  va_end(ap);

  if(n < 0)
    return(FAIL);

  if((size_t)n > sizeof(line) - 3)
    n = (int)(sizeof(line) - 3);

  // After the format, before the terminator: the one place every line
  // this driver sends passes through, and the only one that can see a
  // break a caller's %s argument smuggled in.
  folded = irc_fold_line_breaks(line, n);

  if(folded > 0)
    clam(CLAM_WARN, "irc", "folded %d line break(s) out of an outbound %.*s",
        folded, (int)strcspn(line, " "), line);

  line[n]     = '\r';
  line[n + 1] = '\n';
  line[n + 2] = '\0';

  total = (size_t)(n + 2);

  if(!st->connected)
    return(FAIL);

  // Any thread may send; the owner may be destroying the bot on
  // another. A reference is what makes the two safe together.
  s = irc_session_ref(st);

  if(s == NULL)
    return(FAIL);

  rc = sock_send(s, line, total);
  sock_release(s);

  if(rc != SUCCESS)
  {
    clam(CLAM_WARN, "irc", "send failed");
    return(FAIL);
  }

  clam(CLAM_DEBUG3, "irc", ">> %.*s", n, line);
  return(SUCCESS);
}

// PRIVMSG builder with 512-byte line splitting (IRC RFC 2812).
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

  strlcpy(st->cur_nick, st->nick, IRC_NICK_SZ);

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
    strlcpy(botname, st->inst_name, METHOD_NAME_SZ);

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
