#define IRC_INTERNAL
#include "irc.h"

// Resolve server host/port/tls from the network definition at the
// current server_idx. Collects all servers in the network, sorts by
// priority, and picks the entry at server_idx (wrapping if needed).
// returns: SUCCESS or FAIL (no servers defined)
static bool
irc_resolve_server(irc_state_t *st)
{
  char prefix[KV_KEY_SZ];
  char key[KV_KEY_SZ];
  const char *v;

  // Collect server names for this network.
  snprintf(prefix, sizeof(prefix), IRC_NET_PREFIX "%s.", st->network);

  irc_srv_list_t srvs;
  memset(&srvs, 0, sizeof(srvs));
  kv_iterate_prefix(prefix, irc_srv_list_cb, &srvs);

  if(srvs.count == 0)
    return(FAIL);

  // Build sortable entries with priorities.
  irc_srv_entry_t entries[IRC_MAX_SRVS];

  for(uint32_t i = 0; i < srvs.count; i++)
  {
    snprintf(entries[i].name, IRC_SRV_NAME_SZ, "%s", srvs.names[i]);

    snprintf(key, sizeof(key), IRC_NET_PREFIX "%s.%s.priority",
        st->network, srvs.names[i]);
    entries[i].priority = (uint16_t)kv_get_uint(key);
  }

  // Sort by priority (lower = preferred).
  for(uint32_t i = 0; i < srvs.count - 1; i++)
  {
    for(uint32_t j = i + 1; j < srvs.count; j++)
    {
      if(entries[j].priority < entries[i].priority)
      {
        irc_srv_entry_t tmp = entries[i];
        entries[i] = entries[j];
        entries[j] = tmp;
      }
    }
  }

  // Pick server at server_idx, wrapping around.
  uint32_t idx = st->server_idx % srvs.count;
  const char *srv = entries[idx].name;

  // Read address.
  snprintf(key, sizeof(key), IRC_NET_PREFIX "%s.%s.address",
      st->network, srv);
  v = kv_get_str(key);

  if(v == NULL || v[0] == '\0')
    return(FAIL);

  strncpy(st->host, v, IRC_HOST_SZ - 1);
  st->host[IRC_HOST_SZ - 1] = '\0';

  // Read port.
  snprintf(key, sizeof(key), IRC_NET_PREFIX "%s.%s.port",
      st->network, srv);
  st->port = (uint16_t)kv_get_uint(key);

  if(st->port == 0)
    st->port = 6667;

  // Read TLS settings.
  snprintf(key, sizeof(key), IRC_NET_PREFIX "%s.%s.tls",
      st->network, srv);
  st->tls = (uint8_t)kv_get_uint(key);

  snprintf(key, sizeof(key), IRC_NET_PREFIX "%s.%s.tls_verify",
      st->network, srv);
  st->tls_verify = (uint8_t)kv_get_uint(key);

  return(SUCCESS);
}

// Read KV configuration into state struct. Called at each connect attempt
// so reconnections pick up config changes.
// returns: SUCCESS or FAIL (network/nick missing or no servers defined)
static bool
irc_load_config(irc_state_t *st)
{
  const char *v;
  char key[KV_KEY_SZ];

  // --- Instance-level keys (bot.<botname>.irc.*) ---

  snprintf(key, sizeof(key), "%snetwork", st->kv_prefix);
  v = kv_get_str(key);

  if(v == NULL || v[0] == '\0')
  {
    clam(CLAM_WARN, "irc", "%snetwork is not configured", st->kv_prefix);
    return(FAIL);
  }

  strncpy(st->network, v, IRC_NET_NAME_SZ - 1);
  st->network[IRC_NET_NAME_SZ - 1] = '\0';

  snprintf(key, sizeof(key), "%snick", st->kv_prefix);
  v = kv_get_str(key);

  if(v != NULL)
    strncpy(st->nick, v, IRC_NICK_SZ - 1);

  snprintf(key, sizeof(key), "%snick2", st->kv_prefix);
  v = kv_get_str(key);

  if(v != NULL)
    strncpy(st->nick2, v, IRC_NICK_SZ - 1);

  snprintf(key, sizeof(key), "%snick3", st->kv_prefix);
  v = kv_get_str(key);

  if(v != NULL)
    strncpy(st->nick3, v, IRC_NICK_SZ - 1);

  snprintf(key, sizeof(key), "%suser", st->kv_prefix);
  v = kv_get_str(key);

  if(v != NULL)
    strncpy(st->user, v, IRC_NICK_SZ - 1);

  snprintf(key, sizeof(key), "%srealname", st->kv_prefix);
  v = kv_get_str(key);

  if(v != NULL)
    strncpy(st->realname, v, KV_STR_SZ - 1);

  // --- Network-level keys (irc.net.<name>.*) ---

  snprintf(key, sizeof(key), IRC_NET_PREFIX "%s.pass", st->network);
  v = kv_get_str(key);

  if(v != NULL)
    strncpy(st->pass, v, KV_STR_SZ - 1);

  snprintf(key, sizeof(key), IRC_NET_PREFIX "%s.reconnect_delay",
      st->network);
  st->reconnect_delay = (uint32_t)kv_get_uint(key);

  if(st->reconnect_delay == 0)
    st->reconnect_delay = 30;

  // Resolve the current server from the network definition.
  // irc_resolve_server() handles wrapping internally.
  if(irc_resolve_server(st) != SUCCESS)
  {
    clam(CLAM_WARN, "irc",
        "network '%s' has no servers defined", st->network);
    return(FAIL);
  }

  // Validate required fields.
  if(st->nick[0] == '\0')
  {
    clam(CLAM_WARN, "irc", "%snick is not configured", st->kv_prefix);
    return(FAIL);
  }

  // Default user to nick if not set.
  if(st->user[0] == '\0')
    snprintf(st->user, IRC_NICK_SZ, "%s", st->nick);

  // Reset nick attempt counter for fresh connection.
  st->nick_attempt = 0;

  return(SUCCESS);
}

// -----------------------------------------------------------------------
// IRC protocol parsing
// -----------------------------------------------------------------------

// Parse prefix: nick!user@host
// prefix: input prefix string (without leading ':')
// nick, user, host: output buffers
static void
irc_parse_prefix(const char *prefix, char *nick, char *user, char *host)
{
  nick[0] = '\0';
  user[0] = '\0';
  host[0] = '\0';

  const char *bang = strchr(prefix, '!');
  const char *at   = strchr(prefix, '@');

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
  {
    // Server name only, or malformed prefix.
    snprintf(nick, IRC_NICK_SZ, "%s", prefix);
  }
}

// Parse a single IRC line (CRLF already stripped).
// line: input line
// out: parsed message output
static void
irc_parse_line(const char *line, irc_parsed_msg_t *out)
{
  memset(out, 0, sizeof(*out));

  const char *pos = line;

  // Optional prefix.
  if(*pos == ':')
  {
    pos++;
    const char *end = strchr(pos, ' ');

    if(end == NULL)
      return;

    size_t plen = (size_t)(end - pos);

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

    if(end == NULL)
    {
      strncpy(out->command, pos, sizeof(out->command) - 1);
      return;
    }

    size_t clen = (size_t)(end - pos);

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

    if(trail != NULL)
    {
      // Everything before " :" is params, after is trailing.
      size_t plen = (size_t)(trail - pos);

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

// -----------------------------------------------------------------------
// Send helpers (via core socket service)
// -----------------------------------------------------------------------

// Send a raw IRC line. Appends \r\n. Thread-safe (sock_send is safe).
// returns: SUCCESS or FAIL
bool
irc_send_raw(irc_state_t *st, const char *fmt, ...)
{
  char line[IRC_LINE_SZ];
  va_list ap;

  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
  va_end(ap);

  if(n < 0)
    return(FAIL);

  if((size_t)n > sizeof(line) - 3)
    n = (int)(sizeof(line) - 3);

  line[n]     = '\r';
  line[n + 1] = '\n';
  line[n + 2] = '\0';

  size_t total = (size_t)(n + 2);

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

// Send a PRIVMSG, splitting long messages to stay within 512-byte limit.
// returns: SUCCESS or FAIL
static bool
irc_send_privmsg(irc_state_t *st, const char *target, const char *text)
{
  // Overhead: "PRIVMSG <target> :<text>\r\n"
  // 10 = strlen("PRIVMSG ") + strlen(" :") = 8 + 2
  size_t overhead = 10 + strlen(target) + 2;  // +2 for \r\n
  size_t max_text = 510 - 10 - strlen(target);

  if(max_text < 1 || overhead > 510)
  {
    clam(CLAM_WARN, "irc", "target too long for PRIVMSG: '%s'", target);
    return(FAIL);
  }

  size_t text_len = strlen(text);

  if(text_len <= max_text)
    return(irc_send_raw(st, "PRIVMSG %s :%s", target, text));

  // Split into multiple messages.
  const char *pos = text;
  size_t remaining = text_len;

  while(remaining > 0)
  {
    size_t chunk = remaining;

    if(chunk > max_text)
    {
      chunk = max_text;

      // Try to split on a space boundary.
      size_t last_space = chunk;

      while(last_space > 0 && pos[last_space - 1] != ' ')
        last_space--;

      if(last_space > max_text / 2)
        chunk = last_space;
    }

    char buf[IRC_LINE_SZ];
    size_t copy = chunk;

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

// -----------------------------------------------------------------------
// IRC registration and channel management
// -----------------------------------------------------------------------

// Send PASS, NICK, USER registration commands.
static void
irc_send_registration(irc_state_t *st)
{
  if(st->pass[0] != '\0')
    irc_send_raw(st, "PASS %s", st->pass);

  strncpy(st->cur_nick, st->nick, IRC_NICK_SZ - 1);
  st->cur_nick[IRC_NICK_SZ - 1] = '\0';

  irc_send_raw(st, "NICK %s", st->cur_nick);
  irc_send_raw(st, "USER %s 0 * :%s", st->user, st->realname);
}

// Expand ${var} variables in a template string.
// Supported: ${name} (bot name), ${version}, ${nick}, ${channel}.
// Unknown variables are left as-is.
static void
irc_expand_vars(const char *tmpl, char *out, size_t out_sz,
    const irc_state_t *st, const char *channel)
{
  // Derive bot name from inst_name by stripping "_irc" suffix.
  char botname[METHOD_NAME_SZ] = {0};
  const char *suffix = "_irc";
  size_t nlen = strlen(st->inst_name);
  size_t slen = strlen(suffix);

  if(nlen > slen &&
      strcmp(st->inst_name + nlen - slen, suffix) == 0)
  {
    memcpy(botname, st->inst_name, nlen - slen);
    botname[nlen - slen] = '\0';
  }
  else
    strncpy(botname, st->inst_name, METHOD_NAME_SZ - 1);

  size_t pos = 0;
  const char *p = tmpl;

  while(*p != '\0' && pos < out_sz - 1)
  {
    if(p[0] == '$' && p[1] == '{')
    {
      const char *end = strchr(p + 2, '}');

      if(end != NULL)
      {
        size_t vlen = (size_t)(end - p - 2);
        const char *replacement = NULL;

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

// Handle a self-JOIN: check announce config and send announcetext.
// channel: full channel name including '#' prefix.
static void
irc_handle_self_join(irc_state_t *st, const char *channel)
{
  if(channel == NULL || channel[0] != '#')
    return;

  // Strip '#' for KV key lookup.
  const char *channame = channel + 1;
  char key[KV_KEY_SZ];

  // Check announce flag.
  snprintf(key, sizeof(key), "%schan.%s.announce",
      st->kv_prefix, channame);
  uint64_t announce = kv_get_uint(key);

  if(announce == 0)
    return;

  // Read announcetext.
  snprintf(key, sizeof(key), "%schan.%s.announcetext",
      st->kv_prefix, channame);
  const char *text = kv_get_str(key);

  if(text == NULL || text[0] == '\0')
    return;

  // Expand variables and send.
  char expanded[IRC_LINE_SZ];
  irc_expand_vars(text, expanded, sizeof(expanded), st, channel);

  if(expanded[0] != '\0')
    irc_send_privmsg(st, channel, expanded);
}

// kv_iterate_prefix callback: extract unique channel names from keys.
// Key format: "bot.<name>.irc.chan.<channame>.<property>"
// Runs under KV lock — must not call kv_* functions.
void
irc_chan_collect_cb(const char *key, kv_type_t type,
    const char *value_str, void *data)
{
  (void)type;
  (void)value_str;

  irc_chan_collect_t *cc = data;

  if(cc->count >= IRC_MAX_CHANS)
    return;

  // Extract channel name: everything between prefix and the next '.'.
  const char *suffix = key + cc->prefix_len;
  const char *dot = strchr(suffix, '.');

  if(dot == NULL)
    return;

  size_t nlen = (size_t)(dot - suffix);

  if(nlen == 0 || nlen >= IRC_CHAN_SZ)
    return;

  // Deduplicate: check if we already have this channel.
  for(uint32_t i = 0; i < cc->count; i++)
  {
    if(strncmp(cc->names[i], suffix, nlen) == 0 &&
        cc->names[i][nlen] == '\0')
      return;
  }

  memcpy(cc->names[cc->count], suffix, nlen);
  cc->names[cc->count][nlen] = '\0';
  cc->count++;
}

// Join channels with autojoin enabled from per-channel KV config.
// Reads: bot.<botname>.irc.chan.<name>.autojoin
//        bot.<botname>.irc.chan.<name>.key
static void
irc_join_channels(irc_state_t *st)
{
  // Build the channel prefix: "bot.<botname>.irc.chan."
  char chan_prefix[KV_KEY_SZ];
  snprintf(chan_prefix, sizeof(chan_prefix), "%schan.", st->kv_prefix);

  // Discover all configured channels.
  irc_chan_collect_t cc;
  memset(&cc, 0, sizeof(cc));
  cc.prefix_len = strlen(chan_prefix);

  kv_iterate_prefix(chan_prefix, irc_chan_collect_cb, &cc);

  if(cc.count == 0)
    return;

  // Join each channel that has autojoin enabled.
  for(uint32_t i = 0; i < cc.count; i++)
  {
    char key[KV_KEY_SZ];

    // Check autojoin.
    snprintf(key, sizeof(key), "%s%s.autojoin", chan_prefix, cc.names[i]);
    uint64_t autojoin = kv_get_uint(key);

    if(autojoin == 0)
      continue;

    // Read channel key (password).
    snprintf(key, sizeof(key), "%s%s.key", chan_prefix, cc.names[i]);
    const char *chankey = kv_get_str(key);

    if(chankey != NULL && chankey[0] != '\0')
    {
      clam(CLAM_INFO, "irc", "joining #%s (with key)", cc.names[i]);
      irc_send_raw(st, "JOIN #%s %s", cc.names[i], chankey);
    }

    else
    {
      clam(CLAM_INFO, "irc", "joining #%s", cc.names[i]);
      irc_send_raw(st, "JOIN #%s", cc.names[i]);
    }
  }
}

// -----------------------------------------------------------------------
// Context cache (nick -> host mapping for MFA)
// -----------------------------------------------------------------------

// Update the nick -> host ring buffer.
static void
irc_ctx_update(irc_state_t *st, const char *nick, const char *host)
{
  if(nick[0] == '\0' || host[0] == '\0')
    return;

  pthread_mutex_lock(&st->ctx_mutex);

  uint32_t idx = st->ctx_idx % IRC_CTX_CACHE;

  snprintf(st->ctx_cache[idx].nick, IRC_NICK_SZ, "%s", nick);
  snprintf(st->ctx_cache[idx].host, IRC_HOST_SZ, "%s", host);

  st->ctx_idx++;

  pthread_mutex_unlock(&st->ctx_mutex);
}

// -----------------------------------------------------------------------
// IRC message handling
// -----------------------------------------------------------------------

// Handle a PRIVMSG: deliver to subscribers via method abstraction.
static void
irc_handle_privmsg(irc_state_t *st, const irc_parsed_msg_t *p)
{
  // Update context cache with sender's host.
  irc_ctx_update(st, p->nick, p->host);

  // Build message context on stack.
  method_msg_t msg;
  memset(&msg, 0, sizeof(msg));

  strncpy(msg.sender, p->nick, METHOD_SENDER_SZ - 1);

  // Determine channel: if the target is our nick, it's a DM (leave empty).
  // Extract the first word from params as the target.
  char target[METHOD_CHANNEL_SZ];
  {
    const char *end = strchr(p->params, ' ');
    size_t tlen = end ? (size_t)(end - p->params) : strlen(p->params);

    if(tlen >= METHOD_CHANNEL_SZ) tlen = METHOD_CHANNEL_SZ - 1;

    memcpy(target, p->params, tlen);
    target[tlen] = '\0';
  }

  if(target[0] != '\0' &&
      strncasecmp(target, st->cur_nick, IRC_NICK_SZ) != 0)
  {
    memcpy(msg.channel, target, METHOD_CHANNEL_SZ);
  }

  strncpy(msg.text, p->trailing, METHOD_TEXT_SZ - 1);
  msg.timestamp = time(NULL);

  // Store full prefix in metadata for diagnostics / auth context.
  strncpy(msg.metadata, p->prefix, METHOD_META_SZ - 1);

  method_deliver(st->inst, &msg);
}

// Handle a single parsed IRC line.
static void
irc_handle_line(irc_state_t *st, const char *line)
{
  irc_parsed_msg_t p;
  irc_parse_line(line, &p);

  clam(CLAM_DEBUG3, "irc", "<< %s", line);

  // PING — respond immediately.
  if(strcmp(p.command, "PING") == 0)
  {
    irc_send_raw(st, "PONG :%s", p.has_trailing ? p.trailing : "");
    return;
  }

  // 001 RPL_WELCOME — registration complete.
  if(strcmp(p.command, "001") == 0)
  {
    st->registered = true;
    clam(CLAM_INFO, "irc", "registered as %s on %s",
        st->cur_nick, st->host);
    method_set_state(st->inst, METHOD_AVAILABLE);
    irc_join_channels(st);
    return;
  }

  // JOIN — check if we joined a channel, handle announce.
  if(strcmp(p.command, "JOIN") == 0)
  {
    // Only handle our own joins.
    if(strncasecmp(p.nick, st->cur_nick, IRC_NICK_SZ) == 0)
    {
      // Channel is in trailing or params depending on server.
      const char *channel = p.has_trailing ? p.trailing : p.params;

      if(channel[0] != '\0')
      {
        clam(CLAM_INFO, "irc", "joined %s", channel);
        irc_handle_self_join(st, channel);
      }
    }

    return;
  }

  // PRIVMSG — deliver to subscribers.
  if(strcmp(p.command, "PRIVMSG") == 0)
  {
    irc_handle_privmsg(st, &p);
    return;
  }

  // 433 ERR_NICKNAMEINUSE — try fallback nicks in order.
  if(strcmp(p.command, "433") == 0)
  {
    const char *next = NULL;

    st->nick_attempt++;

    if(st->nick_attempt == 1 && st->nick2[0] != '\0')
      next = st->nick2;
    else if(st->nick_attempt == 2 && st->nick3[0] != '\0')
      next = st->nick3;

    if(next != NULL)
    {
      strncpy(st->cur_nick, next, IRC_NICK_SZ - 1);
      st->cur_nick[IRC_NICK_SZ - 1] = '\0';
      clam(CLAM_WARN, "irc", "nick in use, trying '%s'", st->cur_nick);
      irc_send_raw(st, "NICK %s", st->cur_nick);
    }

    else
    {
      clam(CLAM_WARN, "irc", "all configured nicks are in use");
    }

    return;
  }

  // ERROR — server is disconnecting us.
  if(strcmp(p.command, "ERROR") == 0)
  {
    clam(CLAM_WARN, "irc", "server error: %s",
        p.has_trailing ? p.trailing : p.params);
    st->connected = false;
    return;
  }

  // Other messages — log at debug level.
  clam(CLAM_DEBUG2, "irc", "%s %s %s%s%s",
      p.command, p.params,
      p.has_trailing ? ":" : "",
      p.has_trailing ? p.trailing : "",
      "");
}

// -----------------------------------------------------------------------
// Buffer processing
// -----------------------------------------------------------------------

// Extract complete lines from the read buffer and dispatch them.
static void
irc_process_buffer(irc_state_t *st)
{
  char *start = st->buf;
  char *end;

  while((end = strstr(start, "\r\n")) != NULL)
  {
    *end = '\0';

    if(start[0] != '\0')
      irc_handle_line(st, start);

    start = end + 2;
  }

  // Shift remaining data to front of buffer.
  size_t remaining = (size_t)(st->buf + st->buf_len - start);

  if(remaining > 0 && start != st->buf)
    memmove(st->buf, start, remaining);

  st->buf_len = remaining;

  // Safety: discard if buffer fills without a complete line.
  if(st->buf_len >= IRC_BUF_SZ - 1)
  {
    clam(CLAM_WARN, "irc", "read buffer overflow, discarding %zu bytes",
        st->buf_len);
    st->buf_len = 0;
  }
}

// -----------------------------------------------------------------------
// Socket event callback (invoked on epoll worker thread)
// -----------------------------------------------------------------------

// Handle socket events from the core socket service. Dispatches
// connect, data, disconnect, and error events for the IRC session.
// event: socket event descriptor (type, data, error info)
// user_data: irc_state_t pointer for this connection
static void
irc_sock_cb(const sock_event_t *event, void *user_data)
{
  irc_state_t *st = user_data;

  switch(event->type)
  {
    case SOCK_EVENT_CONNECTED:
      st->connected = true;
      method_set_state(st->inst, METHOD_RUNNING);
      irc_send_registration(st);
      break;

    case SOCK_EVENT_DATA:
    {
      // Append incoming data to the line assembly buffer.
      size_t avail = IRC_BUF_SZ - st->buf_len - 1;
      size_t copy = event->data_len;

      if(copy > avail)
        copy = avail;

      if(copy > 0)
      {
        memcpy(st->buf + st->buf_len, event->data, copy);
        st->buf_len += copy;
        st->buf[st->buf_len] = '\0';
      }

      irc_process_buffer(st);
      break;
    }

    case SOCK_EVENT_DISCONNECT:
      st->registered = false;
      st->connected = false;
      st->buf_len = 0;
      st->server_idx++;  // try next server on reconnect
      method_set_state(st->inst, METHOD_ENABLED);

      if(!pool_shutting_down() && !st->shutdown)
      {
        clam(CLAM_INFO, "irc",
            "reconnecting in %us", st->reconnect_delay);

        task_add_deferred("irc_reconnect", TASK_THREAD, 100,
            st->reconnect_delay * 1000, irc_reconnect_task, st);
      }

      break;

    case SOCK_EVENT_ERROR:
      clam(CLAM_WARN, "irc", "socket error: %s",
          event->err ? strerror(event->err) : "unknown");

      st->registered = false;
      st->connected = false;
      st->buf_len = 0;
      st->server_idx++;  // try next server on reconnect
      method_set_state(st->inst, METHOD_ENABLED);

      if(!pool_shutting_down() && !st->shutdown)
      {
        clam(CLAM_INFO, "irc",
            "reconnecting in %us", st->reconnect_delay);

        task_add_deferred("irc_reconnect", TASK_THREAD, 100,
            st->reconnect_delay * 1000, irc_reconnect_task, st);
      }

      break;
  }
}

// -----------------------------------------------------------------------
// Connection and reconnection
// -----------------------------------------------------------------------

// Attempt to connect to the IRC server via the core socket service.
static void
irc_attempt_connect(irc_state_t *st)
{
  if(pool_shutting_down() || st->shutdown)
    return;

  // Reload config (may have changed since last attempt).
  if(irc_load_config(st) != SUCCESS)
  {
    clam(CLAM_WARN, "irc",
        "configuration incomplete, retrying in %us", st->reconnect_delay);

    {
      char tname[METHOD_NAME_SZ + 16];
      snprintf(tname, sizeof(tname), "irc_recon_%s", st->inst_name);
      task_add_deferred(tname, TASK_THREAD, 100,
          st->reconnect_delay * 1000, irc_reconnect_task, st);
    }
    return;
  }

  // Create session if needed (first connect or after destroy).
  if(st->session == NULL)
  {
    st->session = sock_create(st->inst_name, SOCK_TCP, irc_sock_cb, st);

    if(st->session == NULL)
    {
      clam(CLAM_WARN, "irc", "failed to create socket session");

      task_add_deferred("irc_reconnect", TASK_THREAD, 100,
          st->reconnect_delay * 1000, irc_reconnect_task, st);
      return;
    }
  }

  // Enable TLS if the network server requires it.
  if(st->tls)
    sock_set_tls(st->session, st->tls_verify);

  // Initiate async connect (DNS + TCP).
  if(sock_connect(st->session, st->host, st->port, NULL) != SUCCESS)
  {
    clam(CLAM_WARN, "irc", "sock_connect failed, retrying in %us",
        st->reconnect_delay);

    {
      char tname[METHOD_NAME_SZ + 16];
      snprintf(tname, sizeof(tname), "irc_recon_%s", st->inst_name);
      task_add_deferred(tname, TASK_THREAD, 100,
          st->reconnect_delay * 1000, irc_reconnect_task, st);
    }
    return;
  }

  clam(CLAM_DEBUG, "irc", "connecting to %s:%u", st->host, st->port);
}

// Deferred task callback for reconnection.
static void
irc_reconnect_task(task_t *t)
{
  irc_state_t *st = t->data;

  // Destroy stale session before reconnecting.
  if(st->session != NULL)
  {
    sock_destroy(st->session);
    st->session = NULL;
  }

  irc_attempt_connect(st);
  t->state = TASK_ENDED;
}

// -----------------------------------------------------------------------
// Method driver callbacks
// -----------------------------------------------------------------------

// Allocate and initialize a new IRC method instance state.
// Derives the bot name and KV prefix from the instance name.
// returns: opaque irc_state_t handle
// inst_name: method instance name (e.g. "mybot_irc")
static void *
irc_create(const char *inst_name)
{
  irc_state_t *st = mem_alloc("irc", "state", sizeof(*st));
  memset(st, 0, sizeof(*st));

  strncpy(st->inst_name, inst_name, METHOD_NAME_SZ - 1);

  // Derive bot name from inst_name by stripping the trailing "_irc".
  // Convention: inst_name = "{botname}_irc" (set by admin_cmd_bot_bind).
  const char *suffix = "_irc";
  size_t nlen = strlen(inst_name);
  size_t slen = strlen(suffix);
  char botname[BOT_NAME_SZ] = {0};

  if(nlen > slen &&
      strcmp(inst_name + nlen - slen, suffix) == 0)
    strncpy(botname, inst_name, nlen - slen);
  else
    strncpy(botname, inst_name, BOT_NAME_SZ - 1);

  snprintf(st->kv_prefix, KV_KEY_SZ,
      "bot.%s.irc.", botname);

  st->reconnect_delay = 30;

  pthread_mutex_init(&st->ctx_mutex, NULL);
  return(st);
}

// Free an IRC method instance state and its resources.
// handle: irc_state_t pointer returned by irc_create
static void
irc_destroy(void *handle)
{
  irc_state_t *st = handle;

  pthread_mutex_destroy(&st->ctx_mutex);
  mem_free(st);
}

// Gracefully disconnect from the IRC server. Sends a QUIT message
// and closes the socket. Sets the shutdown flag to prevent reconnection.
// handle: irc_state_t pointer
static void
irc_disconnect(void *handle)
{
  irc_state_t *st = handle;

  st->shutdown = true;

  if(st->session != NULL && st->connected)
  {
    // Best-effort QUIT message.
    irc_send_raw(st, "QUIT :shutting down");
    sock_close(st->session);
  }
}

// Send a PRIVMSG to the given target (channel or nick).
// Requires the connection to be registered before sending.
// returns: SUCCESS or FAIL
// handle: irc_state_t pointer
// target: destination channel or nick
// text: message text to send
static bool
irc_send(void *handle, const char *target, const char *text)
{
  irc_state_t *st = handle;

  if(!st->connected || !st->registered)
    return(FAIL);

  return(irc_send_privmsg(st, target, text));
}

// Look up a sender's host from the nick->host context cache.
// Used by the auth subsystem for host-based identity verification.
// returns: SUCCESS if found, FAIL if sender not in cache
// handle: irc_state_t pointer
// sender: nick to look up
// ctx: output buffer for the host string
// ctx_sz: size of the output buffer
static bool
irc_get_context(void *handle, const char *sender,
    char *ctx, size_t ctx_sz)
{
  irc_state_t *st = handle;

  pthread_mutex_lock(&st->ctx_mutex);

  for(uint32_t i = 0; i < IRC_CTX_CACHE; i++)
  {
    if(st->ctx_cache[i].nick[0] != '\0' &&
        strncasecmp(st->ctx_cache[i].nick, sender, IRC_NICK_SZ) == 0)
    {
      strncpy(ctx, st->ctx_cache[i].host, ctx_sz - 1);
      ctx[ctx_sz - 1] = '\0';
      pthread_mutex_unlock(&st->ctx_mutex);
      return(SUCCESS);
    }
  }

  pthread_mutex_unlock(&st->ctx_mutex);
  return(FAIL);
}

// -----------------------------------------------------------------------
// IRC network/server configuration management
// -----------------------------------------------------------------------

// Validate a network name: alphanumeric, dash, underscore only.
// No dots allowed (they are the KV key separator).
// returns: true if valid
// name: name to validate
bool
irc_valid_name(const char *name)
{
  if(name == NULL || name[0] == '\0')
    return(false);

  for(const char *p = name; *p != '\0'; p++)
  {
    if(!isalnum((unsigned char)*p) && *p != '-' && *p != '_')
      return(false);
  }

  return(true);
}

// Convert an address (hostname or IP) to a KV-safe key.
// Dots and colons are replaced with dashes.
// out: output buffer
// out_sz: output buffer size
void
irc_address_to_key(const char *address, char *out, size_t out_sz)
{
  size_t i;

  for(i = 0; address[i] != '\0' && i < out_sz - 1; i++)
  {
    if(address[i] == '.' || address[i] == ':')
      out[i] = '-';
    else
      out[i] = address[i];
  }

  out[i] = '\0';
}

// -----------------------------------------------------------------------

// Extract the Nth dot-separated segment from a key string.
// returns: SUCCESS or FAIL
// key: full key string (e.g., "irc.net.freenode.srv1.address")
// segment: 0-indexed segment number
// out: output buffer
// out_sz: output buffer size
bool
irc_extract_segment(const char *key, uint32_t segment,
    char *out, size_t out_sz)
{
  const char *p = key;
  uint32_t seg = 0;

  while(seg < segment)
  {
    p = strchr(p, '.');

    if(p == NULL)
      return(FAIL);

    p++;
    seg++;
  }

  const char *end = strchr(p, '.');
  size_t len = end ? (size_t)(end - p) : strlen(p);

  if(len == 0 || len >= out_sz)
    return(FAIL);

  memcpy(out, p, len);
  out[len] = '\0';
  return(SUCCESS);
}

// Restore dynamic irc.net.* KV entries from the database on init.
// Called during plugin init, before kv_load() runs globally.
void
irc_init_networks(void)
{
  db_result_t *r = db_result_alloc();

  if(db_query("SELECT key, type, value FROM kv "
               "WHERE key LIKE 'irc.net.%'", r) != SUCCESS)
  {
    db_result_free(r);
    return;
  }

  uint32_t restored = 0;

  for(uint32_t i = 0; i < r->rows; i++)
  {
    const char *db_key  = db_result_get(r, i, 0);
    const char *db_type = db_result_get(r, i, 1);
    const char *db_val  = db_result_get(r, i, 2);

    if(db_key == NULL || db_type == NULL || db_val == NULL)
      continue;

    kv_type_t type = (kv_type_t)atoi(db_type);

    if(kv_register(db_key, type, db_val, NULL, NULL) == SUCCESS)
      restored++;
  }

  db_result_free(r);

  if(restored > 0)
    clam(CLAM_INFO, "irc",
        "restored %u network configuration entries", restored);
}

// Collect unique server names within a network. Shared between
// irc_resolve_server() and irc_commands.c listing functions.
void
irc_srv_list_cb(const char *key, kv_type_t type,
    const char *val, void *data)
{
  (void)type;
  (void)val;

  irc_srv_list_t *list = data;
  char name[IRC_SRV_NAME_SZ];

  // Key format: irc.net.<NETWORK>.<SERVER>.<PROPERTY>
  // Server is segment 3.
  if(irc_extract_segment(key, 3, name, sizeof(name)) != SUCCESS)
    return;

  // Deduplicate.
  for(uint32_t i = 0; i < list->count; i++)
  {
    if(strcmp(list->names[i], name) == 0)
      return;
  }

  if(list->count < IRC_MAX_SRVS)
  {
    snprintf(list->names[list->count], IRC_SRV_NAME_SZ, "%s", name);
    list->count++;
  }
}

// -----------------------------------------------------------------------
// Driver struct
// -----------------------------------------------------------------------

// Initiate an IRC connection. Resolves the method instance back-pointer
// and begins the async connect sequence (config load, DNS, TCP).
// returns: SUCCESS or FAIL (instance not found)
// handle: irc_state_t pointer
static bool
irc_connect(void *handle)
{
  irc_state_t *st = handle;

  // Resolve the back-pointer to the method instance.
  st->inst = method_find(st->inst_name);

  if(st->inst == NULL)
  {
    clam(CLAM_WARN, "irc", "connect: method instance '%s' not found",
        st->inst_name);
    return(FAIL);
  }

  irc_attempt_connect(st);
  return(SUCCESS);
}

// -----------------------------------------------------------------------
// Plugin lifecycle callbacks
// -----------------------------------------------------------------------

// Initialize the IRC plugin. Restores network KV entries from the
// database and registers all /irc and /show irc console commands.
// returns: SUCCESS
static bool
irc_init(void)
{
  // KV schema auto-registered by plugin loader.

  // Restore dynamic irc.net.* entries from DB so kv_load() can populate
  // them. Must happen before kv_load() which runs after all plugins init.
  irc_init_networks();

  // Register all /irc and /show irc console commands.
  irc_register_commands();

  return(SUCCESS);
}

// Start the IRC plugin. The driver is already available via the plugin
// descriptor; instances are created on demand when bots bind to IRC.
// returns: SUCCESS
static bool
irc_start(void)
{
  // The IRC driver is made available through the plugin descriptor's
  // ext field. Method instances are created on demand by bot_start()
  // when a bot binds to IRC.
  clam(CLAM_INFO, "irc", "method plugin started (driver available)");
  return(SUCCESS);
}

// Stop the IRC plugin. Per-instance disconnection is handled by the
// driver disconnect callback when bot_stop() or bot_destroy() runs.
// returns: SUCCESS
static bool
irc_stop(void)
{
  // Per-instance disconnect is handled by the driver disconnect
  // callback when bot_stop() or bot_destroy() is called.
  return(SUCCESS);
}

// Tear down the IRC plugin. Unregisters all console commands
// in leaf-first order to avoid dangling parent references.
static void
irc_deinit(void)
{
  // Per-instance cleanup is handled by the driver destroy callback
  // when method_unregister() is called from bot_stop()/bot_destroy().
  // Unregister leaf subcommands first, then parents.

  // Leaves under /irc network.
  cmd_unregister("list");   // network list
  cmd_unregister("del");    // network del

  // Leaves under /irc server.
  cmd_unregister("add");    // server add
  cmd_unregister("del");    // server del
  cmd_unregister("list");   // server list

  // Leaves under /irc channel.
  cmd_unregister("add");    // channel add
  cmd_unregister("del");    // channel del
  cmd_unregister("list");   // channel list

  // Parent subcommands under /irc.
  cmd_unregister("network");
  cmd_unregister("server");
  cmd_unregister("channel");
  cmd_unregister("irc-schema");

  // /show irc subtree.
  cmd_unregister("networks");
  cmd_unregister("servers");
  cmd_unregister("show-irc");

  // Root.
  cmd_unregister("irc");
}

// -----------------------------------------------------------------------
// KV schema and plugin descriptor
// -----------------------------------------------------------------------

// No plugin-level KV schema. IRC network definitions
// (plugin.irc.network.<name>.server.<N>.host, etc.) are dynamic and
// created at runtime via direct KV manipulation.

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "irc",
  .version         = "1.0",
  .type            = PLUGIN_METHOD,
  .kind            = "irc",
  .provides        = { { .name = "method_irc" } },
  .provides_count  = 1,
  .requires_count  = 0,
  .kv_schema            = NULL,
  .kv_schema_count      = 0,
  .kv_inst_schema       = irc_inst_kv_schema,
  .kv_inst_schema_count = sizeof(irc_inst_kv_schema) / sizeof(irc_inst_kv_schema[0]),
  .init            = irc_init,
  .start           = irc_start,
  .stop            = irc_stop,
  .deinit          = irc_deinit,
  .ext             = &irc_driver,
  .kv_groups       = irc_kv_groups,
  .kv_groups_count = IRC_KV_GROUPS_COUNT,
};
