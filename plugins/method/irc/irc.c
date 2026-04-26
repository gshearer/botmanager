// botmanager — MIT
// IRC method plugin: socket lifecycle, protocol dispatch, per-channel state.
#define IRC_INTERNAL
#include "irc.h"

static bool
irc_resolve_server(irc_state_t *st)
{
  char prefix[KV_KEY_SZ];
  char key[KV_KEY_SZ];
  const char *v;
  irc_srv_list_t srvs;
  irc_srv_entry_t entries[IRC_MAX_SRVS];
  uint32_t idx;
  const char *srv;

  // Collect server names for this network.
  snprintf(prefix, sizeof(prefix), IRC_NET_PREFIX "%s.", st->network);

  memset(&srvs, 0, sizeof(srvs));
  kv_iterate_prefix(prefix, irc_srv_list_cb, &srvs);

  if(srvs.count == 0)
    return(FAIL);

  // Build sortable entries with priorities.
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
  idx = st->server_idx % srvs.count;
  srv = entries[idx].name;

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


static void
irc_handle_self_join(irc_state_t *st, const char *channel)
{
  const char *channame;
  char key[KV_KEY_SZ];
  uint64_t announce;
  const char *text;
  char expanded[IRC_LINE_SZ];

  if(channel == NULL || channel[0] != '#')
    return;

  // Strip '#' for KV key lookup.
  channame = channel + 1;

  // Check announce flag.
  snprintf(key, sizeof(key), "%schan.%s.announce",
      st->kv_prefix, channame);
  announce = kv_get_uint(key);

  if(announce == 0)
    return;

  // Read announcetext.
  snprintf(key, sizeof(key), "%schan.%s.announcetext",
      st->kv_prefix, channame);
  text = kv_get_str(key);

  if(text == NULL || text[0] == '\0')
    return;

  // Expand variables and send.
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
  irc_chan_collect_t *cc = data;
  const char *suffix;
  const char *dot;
  size_t nlen;

  (void)type;
  (void)value_str;

  if(cc->count >= IRC_MAX_CHANS)
    return;

  // Extract channel name: everything between prefix and the next '.'.
  suffix = key + cc->prefix_len;
  dot = strchr(suffix, '.');

  if(dot == NULL)
    return;

  nlen = (size_t)(dot - suffix);

  if(nlen == 0 || nlen >= IRC_CHAN_SZ)
    return;

  for(uint32_t i = 0; i < cc->count; i++)
    if(strncmp(cc->names[i], suffix, nlen) == 0 &&
        cc->names[i][nlen] == '\0')
      return;

  memcpy(cc->names[cc->count], suffix, nlen);
  cc->names[cc->count][nlen] = '\0';
  cc->count++;
}

static void
irc_join_channels(irc_state_t *st)
{
  // Build the channel prefix: "bot.<botname>.irc.chan."
  char chan_prefix[KV_KEY_SZ];
  irc_chan_collect_t cc;

  snprintf(chan_prefix, sizeof(chan_prefix), "%schan.", st->kv_prefix);

  // Discover all configured channels.
  memset(&cc, 0, sizeof(cc));
  cc.prefix_len = strlen(chan_prefix);

  kv_iterate_prefix(chan_prefix, irc_chan_collect_cb, &cc);

  if(cc.count == 0)
    return;

  // Join each channel that has autojoin enabled.
  for(uint32_t i = 0; i < cc.count; i++)
  {
    char key[KV_KEY_SZ];
    uint64_t autojoin;
    const char *chankey;

    // Check autojoin.
    snprintf(key, sizeof(key), "%s%s.autojoin", chan_prefix, cc.names[i]);
    autojoin = kv_get_uint(key);

    if(autojoin == 0)
      continue;

    // Read channel key (password).
    snprintf(key, sizeof(key), "%s%s.key", chan_prefix, cc.names[i]);
    chankey = kv_get_str(key);

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

// IRC message handling

// Handle a PRIVMSG: deliver to subscribers via method abstraction.
static void
irc_handle_privmsg(irc_state_t *st, const irc_parsed_msg_t *p)
{
  method_msg_t msg;
  char target[METHOD_CHANNEL_SZ];
  const char *body;

  // Update context cache with sender's host.
  irc_ctx_update(st, p->nick, p->host);

  // Build message context on stack.
  memset(&msg, 0, sizeof(msg));

  strncpy(msg.sender, p->nick, METHOD_SENDER_SZ - 1);

  // Determine channel: if the target is our nick, it's a DM (leave empty).
  // Extract the first word from params as the target.
  {
    const char *end = strchr(p->params, ' ');
    size_t tlen = end ? (size_t)(end - p->params) : strlen(p->params);

    if(tlen >= METHOD_CHANNEL_SZ) tlen = METHOD_CHANNEL_SZ - 1;

    memcpy(target, p->params, tlen);
    target[tlen] = '\0';
  }

  if(target[0] != '\0' &&
      strncasecmp(target, st->cur_nick, IRC_NICK_SZ) != 0)
    memcpy(msg.channel, target, METHOD_CHANNEL_SZ);

  // Detect CTCP ACTION ("\x01ACTION <text>\x01" or unterminated variant).
  // Flag the message as an action and strip the CTCP markers so subscribers
  // see only the human-readable text. Non-ACTION CTCPs are dropped: we do
  // not want to accidentally deliver PING/VERSION payloads as chat lines.
  body = p->trailing;
  if(body[0] == '\001')
  {
    const char *after = body + 1;
    if(strncmp(after, "ACTION ", 7) == 0 || strcmp(after, "ACTION") == 0)
    {
      const char *at = after + (after[6] == ' ' ? 7 : 6);
      size_t len = strlen(at);
      if(len > 0 && at[len - 1] == '\001') len--;  // strip trailing marker
      if(len >= METHOD_TEXT_SZ) len = METHOD_TEXT_SZ - 1;
      memcpy(msg.text, at, len);
      msg.text[len] = '\0';
      msg.is_action = true;
    }

    else
      // Non-ACTION CTCP — ignore (PING, VERSION, etc. are not chat lines).
      return;
  }

  else
    strncpy(msg.text, body, METHOD_TEXT_SZ - 1);
  msg.timestamp = time(NULL);

  // Store full prefix in metadata for diagnostics / auth context.
  strncpy(msg.metadata, p->prefix, METHOD_META_SZ - 1);

  method_deliver(st->inst, &msg);
}

// Kick-unidentified deferred task

// Closure for the delayed kick check.
typedef struct
{
  char inst_name[METHOD_NAME_SZ]; // method instance name (e.g. "mybot_irc")
  char channel[IRC_CHAN_SZ + 1];  // full channel name including '#'
  char nick[IRC_NICK_SZ];        // nick at time of join
  char host[IRC_HOST_SZ];        // host at time of join (for MFA context)
} irc_kick_check_t;

// Derive the bot name from an IRC method instance name by stripping "_irc".
// out: output buffer (must be at least METHOD_NAME_SZ)
static void
irc_botname_from_inst(const char *inst_name, char *out, size_t out_sz)
{
  const char *suffix = "_irc";
  size_t nlen = strlen(inst_name);
  size_t slen = strlen(suffix);

  if(nlen > slen && strcmp(inst_name + nlen - slen, suffix) == 0)
  {
    size_t copy = nlen - slen;

    if(copy >= out_sz)
      copy = out_sz - 1;

    memcpy(out, inst_name, copy);
    out[copy] = '\0';
  }

  else
  {
    strncpy(out, inst_name, out_sz - 1);
    out[out_sz - 1] = '\0';
  }
}

// Deferred task callback: check if user is still unidentified, kick if so.
static void
irc_kick_unident_task(task_t *t)
{
  irc_kick_check_t *kc = t->data;
  method_inst_t *inst;
  irc_state_t *st;
  irc_channel_t *ch;
  bool have_ops;
  bool still_here = false;
  char botname[METHOD_NAME_SZ] = {0};
  bot_inst_t *bot;
  char mfa[IRC_PREFIX_SZ];
  const char *user;
  const char *channame;
  char key[KV_KEY_SZ];
  const char *msg;

  t->state = TASK_ENDED;

  if(kc == NULL)
    return;

  // Resolve method instance.
  inst = method_find(kc->inst_name);

  if(inst == NULL)
  {
    mem_free(kc);
    return;
  }

  st = method_get_handle(inst);

  if(st == NULL || !st->connected)
  {
    mem_free(kc);
    return;
  }

  // Check bot still has ops on the channel.
  pthread_mutex_lock(&st->chan_mutex);

  ch = irc_chan_find(st, kc->channel);
  have_ops = (ch != NULL) ? ch->have_ops : false;

  if(ch != NULL)
    still_here = (irc_member_find(ch, kc->nick) != NULL);

  pthread_mutex_unlock(&st->chan_mutex);

  if(!have_ops || !still_here)
  {
    mem_free(kc);
    return;
  }

  // Check if user is now identified.
  irc_botname_from_inst(kc->inst_name, botname, sizeof(botname));

  bot = bot_find(botname);

  if(bot == NULL)
  {
    mem_free(kc);
    return;
  }

  // Build MFA context string (nick!user@host format).
  snprintf(mfa, sizeof(mfa), "%s!*@%s", kc->nick,
      kc->host[0] ? kc->host : "*");

  user = bot_session_find_ex(bot, inst, kc->nick, mfa, NULL);

  if(user != NULL)
  {
    // User identified in time.
    mem_free(kc);
    return;
  }

  // Still unidentified — kick.
  channame = (kc->channel[0] == '#') ?
      kc->channel + 1 : kc->channel;

  snprintf(key, sizeof(key), "%schan.%s.admin.kick_unident_msg",
      st->kv_prefix, channame);
  msg = kv_get_str(key);

  if(msg == NULL || msg[0] == '\0')
    msg = "You must identify to use this channel";

  irc_send_raw(st, "KICK %s %s :%s", kc->channel, kc->nick, msg);
  clam(CLAM_INFO, "irc", "%s: kicked unidentified user %s",
      kc->channel, kc->nick);

  mem_free(kc);
}

static void irc_handle_ping    (irc_state_t *, const irc_parsed_msg_t *);
static void irc_handle_welcome (irc_state_t *, const irc_parsed_msg_t *);
static void irc_handle_namreply(irc_state_t *, const irc_parsed_msg_t *);
static void irc_handle_endnames(irc_state_t *, const irc_parsed_msg_t *);
static void irc_handle_topic332(irc_state_t *, const irc_parsed_msg_t *);
static void irc_handle_join    (irc_state_t *, const irc_parsed_msg_t *);
static void irc_handle_part    (irc_state_t *, const irc_parsed_msg_t *);
static void irc_handle_quit    (irc_state_t *, const irc_parsed_msg_t *);
static void irc_handle_kick    (irc_state_t *, const irc_parsed_msg_t *);
static void irc_handle_nick    (irc_state_t *, const irc_parsed_msg_t *);
static void irc_handle_topic   (irc_state_t *, const irc_parsed_msg_t *);
static void irc_handle_mode    (irc_state_t *, const irc_parsed_msg_t *);
static void irc_handle_nickinuse(irc_state_t *, const irc_parsed_msg_t *);
static void irc_handle_error   (irc_state_t *, const irc_parsed_msg_t *);

static const struct {
  const char *cmd;
  void      (*fn)(irc_state_t *, const irc_parsed_msg_t *);
} irc_cmd_table[] = {
  { "PING",    irc_handle_ping     },
  { "001",     irc_handle_welcome  },
  { "353",     irc_handle_namreply },
  { "366",     irc_handle_endnames },
  { "332",     irc_handle_topic332 },
  { "JOIN",    irc_handle_join     },
  { "PART",    irc_handle_part     },
  { "QUIT",    irc_handle_quit     },
  { "KICK",    irc_handle_kick     },
  { "NICK",    irc_handle_nick     },
  { "TOPIC",   irc_handle_topic    },
  { "MODE",    irc_handle_mode     },
  { "PRIVMSG", irc_handle_privmsg  },
  { "433",     irc_handle_nickinuse},
  { "ERROR",   irc_handle_error    },
  { NULL,      NULL                }
};

static void
irc_handle_ping(irc_state_t *st, const irc_parsed_msg_t *pp)
{
  const irc_parsed_msg_t p = *pp;

  irc_send_raw(st, "PONG :%s", p.has_trailing ? p.trailing : "");
}

static void
irc_handle_welcome(irc_state_t *st, const irc_parsed_msg_t *pp)
{
  (void)pp;
  st->registered = true;
  clam(CLAM_INFO, "irc", "registered as %s on %s", st->cur_nick, st->host);
  method_set_state(st->inst, METHOD_AVAILABLE);
  irc_join_channels(st);
}

static void
irc_handle_namreply(irc_state_t *st, const irc_parsed_msg_t *pp)
{
  const irc_parsed_msg_t p = *pp;
  // Find the channel name (last word in params starting with # or &).
  const char *chan = NULL;
  const char *s = p.params;

  while(*s != '\0')
  {
    if(*s == '#' || *s == '&')
    {
      chan = s;
      break;
    }

    s++;
  }

  if(chan == NULL || !p.has_trailing)
    return;

  // Extract channel name (up to space or end).
  char channel[IRC_CHAN_SZ + 1];
  size_t i = 0;
  const char *p_nick;

  while(chan[i] != '\0' && chan[i] != ' ' && i < IRC_CHAN_SZ)
  {
    channel[i] = chan[i];
    i++;
  }

  channel[i] = '\0';

  // Parse nicks from trailing, capturing mode prefixes (@+%~&).
  p_nick = p.trailing;

  while(*p_nick != '\0')
  {
    uint8_t flags = 0;
    char nick[IRC_NICK_SZ];
    size_t nlen = 0;

    while(*p_nick == ' ')
      p_nick++;

    if(*p_nick == '\0')
      break;

    while(*p_nick == '@' || *p_nick == '+' || *p_nick == '%' ||
          *p_nick == '~' || *p_nick == '&')
    {
      if(*p_nick == '@') flags |= IRC_MFLAG_OP;
      else if(*p_nick == '+') flags |= IRC_MFLAG_VOICE;
      else if(*p_nick == '%') flags |= IRC_MFLAG_HALFOP;
      else if(*p_nick == '~') flags |= IRC_MFLAG_OWNER;
      else if(*p_nick == '&') flags |= IRC_MFLAG_ADMIN;
      p_nick++;
    }

    while(*p_nick != '\0' && *p_nick != ' ' && nlen < IRC_NICK_SZ - 1)
      nick[nlen++] = *p_nick++;

    nick[nlen] = '\0';

    if(nlen == 0)
      continue;

    irc_chan_add_nick(st, channel, nick);

    if(flags != 0)
    {
      irc_channel_t *ch;

      pthread_mutex_lock(&st->chan_mutex);

      ch = irc_chan_find(st, channel);

      if(ch != NULL)
      {
        irc_member_t *m = irc_member_find(ch, nick);

        if(m != NULL)
          m->mode_flags = flags;
      }

      pthread_mutex_unlock(&st->chan_mutex);
    }
  }
}

static void
irc_handle_endnames(irc_state_t *st, const irc_parsed_msg_t *pp)
{
  const irc_parsed_msg_t p = *pp;
  const char *s = p.params;
  char channel[IRC_CHAN_SZ + 1];
  size_t ci = 0;
  irc_channel_t *ch;
  bool has_ops;

  while(*s != '\0' && *s != ' ')
    s++;

  while(*s == ' ')
    s++;

  while(*s != '\0' && *s != ' ' && ci < IRC_CHAN_SZ)
  {
    channel[ci] = *s++;
    ci++;
  }

  channel[ci] = '\0';

  if(channel[0] != '#' && channel[0] != '&')
    return;

  pthread_mutex_lock(&st->chan_mutex);

  ch = irc_chan_find(st, channel);

  if(ch != NULL)
  {
    irc_member_t *self = irc_member_find(ch, st->cur_nick);

    if(self != NULL && (self->mode_flags & IRC_MFLAG_OP))
      ch->have_ops = true;
  }

  has_ops = (ch != NULL) ? ch->have_ops : false;

  pthread_mutex_unlock(&st->chan_mutex);

  if(has_ops)
    irc_apply_chan_admin(st, channel);
}

static void
irc_handle_topic332(irc_state_t *st, const irc_parsed_msg_t *pp)
{
  const irc_parsed_msg_t p = *pp;
  const char *s = p.params;
  char channel[IRC_CHAN_SZ + 1];
  size_t ci = 0;
  irc_channel_t *ch;

  while(*s != '\0' && *s != ' ')
    s++;

  while(*s == ' ')
    s++;

  while(*s != '\0' && *s != ' ' && ci < IRC_CHAN_SZ)
  {
    channel[ci] = *s++;
    ci++;
  }

  channel[ci] = '\0';

  if(!p.has_trailing || (channel[0] != '#' && channel[0] != '&'))
    return;

  pthread_mutex_lock(&st->chan_mutex);

  ch = irc_chan_find(st, channel);

  if(ch != NULL)
  {
    strncpy(ch->topic, p.trailing, IRC_LINE_SZ - 1);
    ch->topic[IRC_LINE_SZ - 1] = '\0';
  }

  pthread_mutex_unlock(&st->chan_mutex);
}

static void
irc_handle_join(irc_state_t *st, const irc_parsed_msg_t *pp)
{
  const irc_parsed_msg_t p = *pp;
  // Channel is in trailing or params depending on server.
  const char *channel = p.has_trailing ? p.trailing : p.params;
  const char *channame;
  char key[KV_KEY_SZ];
  irc_channel_t *ch;
  bool have_ops;
  char botname[METHOD_NAME_SZ] = {0};
  bot_inst_t *bot;
  char mfa[IRC_PREFIX_SZ];
  const char *user;
  uint32_t delay;
  irc_kick_check_t *kc;

  if(channel[0] == '\0')
    return;

  irc_chan_add_nick(st, channel, p.nick);

  // Handle our own joins.
  if(strncasecmp(p.nick, st->cur_nick, IRC_NICK_SZ) == 0)
  {
    clam(CLAM_INFO, "irc", "joined %s", channel);
    irc_handle_self_join(st, channel);
    return;
  }

  if(channel[0] != '#')
    return;

  // Kick-unident enforcement: unidentified joiner on an admin-managed channel.
  channame = channel + 1;
  snprintf(key, sizeof(key), "%schan.%s.admin.enabled",
      st->kv_prefix, channame);

  if(kv_get_uint(key) == 0)
    return;

  snprintf(key, sizeof(key), "%schan.%s.admin.kick_unident",
      st->kv_prefix, channame);

  if(kv_get_uint(key) == 0)
    return;

  pthread_mutex_lock(&st->chan_mutex);
  ch = irc_chan_find(st, channel);
  have_ops = (ch != NULL) ? ch->have_ops : false;
  pthread_mutex_unlock(&st->chan_mutex);

  if(!have_ops)
    return;

  irc_botname_from_inst(st->inst_name, botname, sizeof(botname));
  bot = bot_find(botname);
  snprintf(mfa, sizeof(mfa), "%s!%s@%s", p.nick, p.user, p.host);
  user = (bot != NULL) ?
      bot_session_find_ex(bot, st->inst, p.nick, mfa, NULL) : NULL;

  if(user != NULL)
    return;

  snprintf(key, sizeof(key), "%schan.%s.admin.kick_unident_delay",
      st->kv_prefix, channame);
  delay = (uint32_t)kv_get_uint(key);

  if(delay == 0)
    delay = 30;

  kc = mem_alloc("irc", "kick_check", sizeof(*kc));
  strncpy(kc->inst_name, st->inst_name, METHOD_NAME_SZ - 1);
  kc->inst_name[METHOD_NAME_SZ - 1] = '\0';
  strncpy(kc->channel, channel, IRC_CHAN_SZ);
  kc->channel[IRC_CHAN_SZ] = '\0';
  strncpy(kc->nick, p.nick, IRC_NICK_SZ - 1);
  kc->nick[IRC_NICK_SZ - 1] = '\0';
  strncpy(kc->host, p.host, IRC_HOST_SZ - 1);
  kc->host[IRC_HOST_SZ - 1] = '\0';

  task_add_deferred("irc-kick-unident",
      TASK_ANY, 200, delay * 1000, irc_kick_unident_task, kc);
}

static void
irc_handle_part(irc_state_t *st, const irc_parsed_msg_t *pp)
{
  const irc_parsed_msg_t p = *pp;
  char channel[IRC_CHAN_SZ + 1];
  size_t i = 0;

  while(p.params[i] != '\0' && p.params[i] != ' ' && i < IRC_CHAN_SZ)
  {
    channel[i] = p.params[i];
    i++;
  }

  channel[i] = '\0';

  if(strncasecmp(p.nick, st->cur_nick, IRC_NICK_SZ) == 0)
    irc_chan_remove(st, channel);
  else
    irc_chan_remove_nick(st, channel, p.nick);
}

static void
irc_handle_quit(irc_state_t *st, const irc_parsed_msg_t *pp)
{
  const irc_parsed_msg_t p = *pp;

  irc_chan_remove_nick_all(st, p.nick);
}

static void
irc_handle_kick(irc_state_t *st, const irc_parsed_msg_t *pp)
{
  const irc_parsed_msg_t p = *pp;
  char channel[IRC_CHAN_SZ + 1];
  char kicked[IRC_NICK_SZ];
  size_t i = 0;
  const char *k;
  size_t j = 0;

  while(p.params[i] != '\0' && p.params[i] != ' ' && i < IRC_CHAN_SZ)
  {
    channel[i] = p.params[i];
    i++;
  }

  channel[i] = '\0';

  k = p.params + i;

  while(*k == ' ')
    k++;

  while(*k != '\0' && *k != ' ' && j < IRC_NICK_SZ - 1)
    kicked[j++] = *k++;

  kicked[j] = '\0';

  if(strncasecmp(kicked, st->cur_nick, IRC_NICK_SZ) == 0)
    irc_chan_remove(st, channel);
  else
    irc_chan_remove_nick(st, channel, kicked);
}

static void
irc_handle_nick(irc_state_t *st, const irc_parsed_msg_t *pp)
{
  const irc_parsed_msg_t p = *pp;
  const char *new_nick = p.has_trailing ? p.trailing : p.params;
  method_msg_t nmsg;

  if(new_nick[0] == '\0')
    return;

  irc_chan_rename_nick(st, p.nick, new_nick);

  if(strncasecmp(p.nick, st->cur_nick, IRC_NICK_SZ) == 0)
  {
    strncpy(st->cur_nick, new_nick, IRC_NICK_SZ - 1);
    st->cur_nick[IRC_NICK_SZ - 1] = '\0';
  }

  // Deliver NICK_CHANGE so dossier-aware bots can merge both identities.
  memset(&nmsg, 0, sizeof(nmsg));
  nmsg.kind = METHOD_MSG_NICK_CHANGE;
  strncpy(nmsg.sender, p.nick, METHOD_SENDER_SZ - 1);
  strncpy(nmsg.text,   new_nick, METHOD_TEXT_SZ - 1);
  snprintf(nmsg.metadata, METHOD_META_SZ, "%s!%s@%s",
      new_nick, p.user, p.host);
  strncpy(nmsg.prev_metadata, p.prefix, METHOD_META_SZ - 1);
  nmsg.timestamp = time(NULL);
  method_deliver(st->inst, &nmsg);
}

static void
irc_handle_topic(irc_state_t *st, const irc_parsed_msg_t *pp)
{
  const irc_parsed_msg_t p = *pp;
  char channel[IRC_CHAN_SZ + 1];
  size_t ci = 0;
  const char *new_topic;
  irc_channel_t *ch;
  bool have_ops = false;
  const char *channame;
  const char *wanted;
  char key[KV_KEY_SZ];

  while(p.params[ci] != '\0' && p.params[ci] != ' ' && ci < IRC_CHAN_SZ)
  {
    channel[ci] = p.params[ci];
    ci++;
  }

  channel[ci] = '\0';

  if(channel[0] != '#' && channel[0] != '&')
    return;

  new_topic = p.has_trailing ? p.trailing : "";

  pthread_mutex_lock(&st->chan_mutex);

  ch = irc_chan_find(st, channel);

  if(ch != NULL)
  {
    strncpy(ch->topic, new_topic, IRC_LINE_SZ - 1);
    ch->topic[IRC_LINE_SZ - 1] = '\0';
    have_ops = ch->have_ops;
  }

  pthread_mutex_unlock(&st->chan_mutex);

  if(!have_ops || strncasecmp(p.nick, st->cur_nick, IRC_NICK_SZ) == 0)
    return;

  channame = channel + 1;
  snprintf(key, sizeof(key), "%schan.%s.admin.enabled",
      st->kv_prefix, channame);

  if(kv_get_uint(key) == 0)
    return;

  snprintf(key, sizeof(key), "%schan.%s.admin.topic.enabled",
      st->kv_prefix, channame);

  if(kv_get_uint(key) == 0)
    return;

  snprintf(key, sizeof(key), "%schan.%s.admin.topic.value",
      st->kv_prefix, channame);
  wanted = kv_get_str(key);

  if(wanted != NULL && wanted[0] != '\0' && strcmp(new_topic, wanted) != 0)
  {
    irc_send_raw(st, "TOPIC %s :%s", channel, wanted);
    clam(CLAM_INFO, "irc", "%s: restored topic (changed by %s)",
        channel, p.nick);
  }
}

static void
irc_handle_mode(irc_state_t *st, const irc_parsed_msg_t *pp)
{
  const irc_parsed_msg_t p = *pp;
  char channel[IRC_CHAN_SZ + 1];
  size_t ci = 0;
  const char *mp;
  const char *mode_str;
  bool adding = true;
  bool key_removed = false;
  const char *param_ptr;
  bool is_self;
  irc_channel_t *ch;
  bool had_ops;
  bool now_has_ops;
  const char *channame;
  const char *val;
  char key[KV_KEY_SZ];

  while(p.params[ci] != '\0' && p.params[ci] != ' ' && ci < IRC_CHAN_SZ)
  {
    channel[ci] = p.params[ci];
    ci++;
  }

  channel[ci] = '\0';

  if(channel[0] != '#' && channel[0] != '&')
    return;

  mp = p.params + ci;

  while(*mp == ' ')
    mp++;

  if(*mp == '\0')
    return;

  mode_str = mp;

  while(*mp != '\0' && *mp != ' ')
    mp++;

  while(*mp == ' ')
    mp++;

  // mp now points at the first parameter nick (or '\0').
  param_ptr = mp;
  is_self = (strncasecmp(p.nick, st->cur_nick, IRC_NICK_SZ) == 0);

  pthread_mutex_lock(&st->chan_mutex);

  ch = irc_chan_find(st, channel);
  had_ops = (ch != NULL) ? ch->have_ops : false;

  for(const char *mc = mode_str; *mc != '\0' && *mc != ' '; mc++)
  {
    uint8_t flag;
    bool takes_param;

    if(*mc == '+')
    {
      adding = true;
      continue;
    }

    if(*mc == '-')
    {
      adding = false;
      continue;
    }

    flag = irc_mode_to_flag(*mc);

    takes_param = (*mc == 'o' || *mc == 'v' || *mc == 'h' ||
        *mc == 'q' || *mc == 'a' || *mc == 'k' || *mc == 'b' ||
        (*mc == 'l' && adding));

    if(*mc == 'k' && !adding)
      key_removed = true;

    if(takes_param)
    {
      char param_nick[IRC_NICK_SZ];
      size_t pn = 0;

      while(*param_ptr != '\0' && *param_ptr != ' ' &&
          pn < IRC_NICK_SZ - 1)
        param_nick[pn++] = *param_ptr++;

      param_nick[pn] = '\0';

      while(*param_ptr == ' ')
        param_ptr++;

      if(flag != 0 && ch != NULL && pn > 0)
      {
        irc_member_t *m = irc_member_find(ch, param_nick);

        if(m != NULL)
        {
          if(adding)
            m->mode_flags |= flag;
          else
            m->mode_flags &= ~flag;
        }

        if(flag == IRC_MFLAG_OP &&
            strncasecmp(param_nick, st->cur_nick, IRC_NICK_SZ) == 0)
        {
          ch->have_ops = adding;

          if(!adding)
            clam(CLAM_INFO, "irc", "%s: lost ops (deopped by %s)",
                channel, p.nick);
        }
      }
    }
  }

  now_has_ops = (ch != NULL) ? ch->have_ops : false;

  pthread_mutex_unlock(&st->chan_mutex);

  if(!had_ops && now_has_ops)
    irc_apply_chan_admin(st, channel);

  if(!key_removed || !now_has_ops || is_self)
    return;

  channame = (channel[0] == '#') ? channel + 1 : channel;
  snprintf(key, sizeof(key), "%schan.%s.admin.enabled",
      st->kv_prefix, channame);

  if(kv_get_uint(key) == 0)
    return;

  snprintf(key, sizeof(key), "%schan.%s.admin.key.enabled",
      st->kv_prefix, channame);

  if(kv_get_uint(key) == 0)
    return;

  snprintf(key, sizeof(key), "%schan.%s.admin.key.value",
      st->kv_prefix, channame);
  val = kv_get_str(key);

  if(val != NULL && val[0] != '\0')
  {
    irc_send_raw(st, "MODE %s +k %s", channel, val);
    clam(CLAM_INFO, "irc", "%s: restored channel key (removed by %s)",
        channel, p.nick);
  }
}

static void
irc_handle_nickinuse(irc_state_t *st, const irc_parsed_msg_t *pp)
{
  const char *next = NULL;

  (void)pp;
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
    clam(CLAM_WARN, "irc", "all configured nicks are in use");
}

static void
irc_handle_error(irc_state_t *st, const irc_parsed_msg_t *pp)
{
  const irc_parsed_msg_t p = *pp;

  clam(CLAM_WARN, "irc", "server error: %s",
      p.has_trailing ? p.trailing : p.params);
  st->connected = false;
}

static void
irc_handle_line(irc_state_t *st, const char *line)
{
  irc_parsed_msg_t p;
  size_t i;

  irc_parse_line(line, &p);
  clam(CLAM_DEBUG3, "irc", "<< %s", line);

  for(i = 0; irc_cmd_table[i].cmd != NULL; i++)
  {
    if(strcmp(p.command, irc_cmd_table[i].cmd) == 0)
    {
      irc_cmd_table[i].fn(st, &p);
      return;
    }
  }

  clam(CLAM_DEBUG2, "irc", "%s %s %s%s%s",
      p.command, p.params,
      p.has_trailing ? ":" : "",
      p.has_trailing ? p.trailing : "",
      "");
}

// Buffer processing

// Extract complete lines from the read buffer and dispatch them.
static void
irc_process_buffer(irc_state_t *st)
{
  char *start = st->buf;
  char *end;
  size_t remaining;

  while((end = strstr(start, "\r\n")) != NULL)
  {
    *end = '\0';

    if(start[0] != '\0')
      irc_handle_line(st, start);

    start = end + 2;
  }

  // Shift remaining data to front of buffer.
  remaining = (size_t)(st->buf + st->buf_len - start);

  if(remaining > 0 && start != st->buf)
    memmove(st->buf, start, remaining);

  st->buf_len = remaining;

  if(st->buf_len >= IRC_BUF_SZ - 1)
  {
    clam(CLAM_WARN, "irc", "read buffer overflow, discarding %zu bytes",
        st->buf_len);
    st->buf_len = 0;
  }
}

// Socket event callback (invoked on epoll worker thread)

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

        st->reconnect_task = task_add_deferred("irc_reconnect",
            TASK_THREAD, 100,
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

        st->reconnect_task = task_add_deferred("irc_reconnect",
            TASK_THREAD, 100,
            st->reconnect_delay * 1000, irc_reconnect_task, st);
      }

      break;
  }
}

// Connection and reconnection

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
      st->reconnect_task = task_add_deferred(tname, TASK_THREAD, 100,
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

      st->reconnect_task = task_add_deferred("irc_reconnect",
          TASK_THREAD, 100,
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
      st->reconnect_task = task_add_deferred(tname, TASK_THREAD, 100,
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

  // Clear the pending-task handle so irc_disconnect does not try to
  // cancel a task that is already firing. A stale handle would still
  // be safe (task_cancel on an already-ended id is a debug-logged
  // no-op), but zeroing avoids the spurious log line.
  st->reconnect_task = TASK_HANDLE_NONE;

  // Another path (explicit stop, shutdown) may have disabled us
  // between scheduling and firing. Bail before touching the session.
  if(st->shutdown || pool_shutting_down())
  {
    t->state = TASK_ENDED;
    return;
  }

  // Destroy stale session before reconnecting.
  if(st->session != NULL)
  {
    sock_destroy(st->session);
    st->session = NULL;
  }

  irc_attempt_connect(st);
  t->state = TASK_ENDED;
}

// Method driver callbacks

static void *
irc_create(const char *inst_name)
{
  irc_state_t *st = mem_alloc("irc", "state", sizeof(*st));
  const char *suffix = "_irc";
  size_t nlen = strlen(inst_name);
  size_t slen = strlen(suffix);
  char botname[BOT_NAME_SZ] = {0};

  memset(st, 0, sizeof(*st));

  strncpy(st->inst_name, inst_name, METHOD_NAME_SZ - 1);

  // Derive bot name from inst_name by stripping the trailing "_irc".
  // Convention: inst_name = "{botname}_irc" (set by admin_cmd_bot_bind).

  if(nlen > slen &&
      strcmp(inst_name + nlen - slen, suffix) == 0)
    strncpy(botname, inst_name, nlen - slen);
  else
    strncpy(botname, inst_name, BOT_NAME_SZ - 1);

  snprintf(st->kv_prefix, KV_KEY_SZ,
      "bot.%s.irc.", botname);

  st->reconnect_delay = 30;

  pthread_mutex_init(&st->ctx_mutex, NULL);
  pthread_mutex_init(&st->chan_mutex, NULL);
  return(st);
}

static void
irc_destroy(void *handle)
{
  irc_state_t *st = handle;

  irc_chan_clear_all(st);
  pthread_mutex_destroy(&st->chan_mutex);
  pthread_mutex_destroy(&st->ctx_mutex);
  mem_free(st);
}

// Gracefully disconnect from the IRC server. Sends a QUIT message
// when registration was complete, tears down any socket (even
// half-open sessions mid-handshake to avoid leaked fds that remain
// visible to the peer), and neutralises any pending reconnect task
// so it does not fire after the method has been stopped.
static void
irc_disconnect(void *handle)
{
  irc_state_t *st = handle;

  st->shutdown = true;

  // Cancel any pending deferred reconnect so the scheduler drops it
  // before the delay expires. Without this, a stop/start cycle during
  // the reconnect backoff could fire a spurious second connection
  // (previously observed on Libera as a stale pacmanpundit_ nick).
  if(st->reconnect_task != TASK_HANDLE_NONE)
  {
    task_cancel(st->reconnect_task);
    st->reconnect_task = TASK_HANDLE_NONE;
  }

  irc_chan_clear_all(st);

  // Always close an existing session, connected or not. A half-open
  // session whose fd we leak here remains visible to the peer and
  // only clears on ping-timeout (several minutes on Libera).
  if(st->session != NULL)
  {
    if(st->connected)
      irc_send_raw(st, "QUIT :shutting down");
    sock_close(st->session);
  }
}

static bool
irc_send(void *handle, const char *target, const char *text)
{
  irc_state_t *st = handle;

  if(!st->connected || !st->registered)
    return(FAIL);

  return(irc_send_privmsg(st, target, text));
}

// Send a CTCP ACTION (the on-the-wire form of /me) to the given target.
// The text is wrapped in CTCP \x01 markers inside a normal PRIVMSG.
static bool
irc_send_emote(void *handle, const char *target, const char *text)
{
  irc_state_t *st = handle;
  char buf[IRC_LINE_SZ];
  int n;

  if(!st->connected || !st->registered)
    return(FAIL);

  // Keep within IRC's 512-byte wire limit; the 9-byte overhead for
  // "ACTION " + two \x01 is small, so split on spaces if needed.
  n = snprintf(buf, sizeof(buf), "\001ACTION %s\001", text);
  if(n < 0) return(FAIL);

  return(irc_send_privmsg(st, target, buf));
}

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

// List members of an IRC channel, invoking cb for each nick.
static void
irc_list_channel(void *handle, const char *channel,
    method_chan_member_cb_t cb, void *data)
{
  irc_state_t *st = handle;
  irc_channel_t *ch;

  pthread_mutex_lock(&st->chan_mutex);

  ch = irc_chan_find(st, channel);

  if(ch == NULL)
  {
    pthread_mutex_unlock(&st->chan_mutex);
    return;
  }

  for(irc_member_t *m = ch->members; m != NULL; m = m->next)
    cb(m->nick, data);

  pthread_mutex_unlock(&st->chan_mutex);
}

// List channels the bot is currently joined to. Entries are added on
// JOIN and removed on PART/KICK/QUIT/disconnect, so the list reflects
// live membership — not the autojoin KV configuration. Callback runs
// with chan_mutex held; it must not acquire additional IRC locks or
// call back into the IRC driver.
static void
irc_list_joined_channels(void *handle,
    method_joined_channel_cb_t cb, void *data)
{
  irc_state_t *st = handle;

  pthread_mutex_lock(&st->chan_mutex);

  for(irc_channel_t *ch = st->channels; ch != NULL; ch = ch->next)
    cb(ch->name, data);

  pthread_mutex_unlock(&st->chan_mutex);
}

// Build an identity signature JSON from an inbound message. The
// message metadata carries the full IRC prefix ("nick!ident@host") —
// see the PRIVMSG handling in irc_handle_line. Delegates the JSON
// shaping to irc_dossier_build_signature so the unit tests can
// exercise the same code path without loading the plugin.
//
// Registered with the chat plugin's identity registry at plugin_start
// (see irc_start below); signature matches chat_identity_signer_t.
static bool
irc_identity_signature(const method_msg_t *msg,
    char *out_json, size_t out_sz)
{
  if(msg == NULL)
    return(FAIL);

  return(irc_dossier_build_signature(msg->metadata, out_json, out_sz));
}

// Get the bot's current IRC nickname.
static bool
irc_get_self(void *handle, char *buf, size_t buf_sz)
{
  irc_state_t *st = handle;

  if(st->cur_nick[0] == '\0')
    return(FAIL);

  strncpy(buf, st->cur_nick, buf_sz - 1);
  buf[buf_sz - 1] = '\0';
  return(SUCCESS);
}

// IRC network/server configuration management

bool
irc_valid_name(const char *name)
{
  if(name == NULL || name[0] == '\0')
    return(false);

  for(const char *p = name; *p != '\0'; p++)
    if(!isalnum((unsigned char)*p) && *p != '-' && *p != '_')
      return(false);

  return(true);
}

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

bool
irc_extract_segment(const char *key, uint32_t segment,
    char *out, size_t out_sz)
{
  const char *p = key;
  uint32_t seg = 0;
  const char *end;
  size_t len;

  while(seg < segment)
  {
    p = strchr(p, '.');

    if(p == NULL)
      return(FAIL);

    p++;
    seg++;
  }

  end = strchr(p, '.');
  len = end ? (size_t)(end - p) : strlen(p);

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
  uint32_t restored = 0;

  if(db_query("SELECT key, type, value FROM kv "
               "WHERE key LIKE 'irc.net.%'", r) != SUCCESS)
  {
    db_result_free(r);
    return;
  }

  for(uint32_t i = 0; i < r->rows; i++)
  {
    const char *db_key  = db_result_get(r, i, 0);
    const char *db_type = db_result_get(r, i, 1);
    const char *db_val  = db_result_get(r, i, 2);
    kv_type_t type;

    if(db_key == NULL || db_type == NULL || db_val == NULL)
      continue;

    type = (kv_type_t)atoi(db_type);

    if(kv_register(db_key, type, db_val, NULL, NULL, NULL) == SUCCESS)
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
  irc_srv_list_t *list = data;
  char name[IRC_SRV_NAME_SZ];

  (void)type;
  (void)val;

  // Key format: irc.net.<NETWORK>.<SERVER>.<PROPERTY>
  // Server is segment 3.
  if(irc_extract_segment(key, 3, name, sizeof(name)) != SUCCESS)
    return;

  // Deduplicate.
  for(uint32_t i = 0; i < list->count; i++)
    if(strcmp(list->names[i], name) == 0)
      return;

  if(list->count < IRC_MAX_SRVS)
  {
    snprintf(list->names[list->count], IRC_SRV_NAME_SZ, "%s", name);
    list->count++;
  }
}

// Driver struct

// Initiate an IRC connection. Resolves the method instance back-pointer
// and begins the async connect sequence (config load, DNS, TCP).
// returns: SUCCESS or FAIL (instance not found)
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

// KV callbacks

// When the primary nick is set, auto-populate nick2 (nick_) and nick3
// (nick__) so the bot always has alternate nicknames available.
void
irc_nick_kv_cb(const char *key, void *data)
{
  const char *nick;
  char key2[KV_KEY_SZ];
  char key3[KV_KEY_SZ];
  char val2[IRC_NICK_SZ];
  char val3[IRC_NICK_SZ];

  (void)data;

  nick = kv_get_str(key);

  if(nick == NULL || nick[0] == '\0')
    return;

  // key is "bot.<botname>.irc.nick" — derive nick2/nick3 by appending
  // "2" and "3" to the key, and "_" / "__" to the nick value.
  snprintf(key2, sizeof(key2), "%s2", key);
  snprintf(key3, sizeof(key3), "%s3", key);

  snprintf(val2, sizeof(val2), "%s_", nick);
  snprintf(val3, sizeof(val3), "%s__", nick);

  kv_set_str(key2, val2);
  kv_set_str(key3, val3);
}

// Plugin lifecycle callbacks

static bool
irc_init(void)
{
  // KV schema auto-registered by plugin loader.

  // Restore dynamic irc.net.* entries from DB so kv_load() can populate
  // them. Must happen before kv_load() which runs after all plugins init.
  irc_init_networks();

  // Register all /irc and /show irc operator commands.
  irc_register_commands();

  return(SUCCESS);
}

// Function-pointer typedef for the chat plugin's chat_identity_register
// entry point. Matches plugins/bot/chat/identity.h exactly; declared
// locally so the IRC plugin does not need to include a chat-plugin
// header across the RTLD_LOCAL boundary. The typedefs for the signer
// and scorer signatures match the contract-level types already used by
// the IRC scorer functions (dossier_method_sig_t etc.).
typedef bool (*irc_chat_identity_signer_t)(const method_msg_t *msg,
    char *out_json, size_t out_sz);
typedef float (*irc_chat_identity_scorer_t)(
    const dossier_method_sig_t *a, const dossier_method_sig_t *b);
typedef float (*irc_chat_identity_token_scorer_t)(const char *token,
    const dossier_method_sig_t *sig);
typedef bool (*irc_chat_identity_register_fn_t)(const char *method_kind,
    irc_chat_identity_signer_t       signer,
    irc_chat_identity_scorer_t       scorer,
    irc_chat_identity_token_scorer_t token_scorer);

static bool
irc_start(void)
{
  irc_chat_identity_register_fn_t reg;
  union { void *obj; irc_chat_identity_register_fn_t fn; } u;

  // Publish the IRC identity triple to the chat plugin's identity
  // registry. Chat may not be loaded (command-bot-only deployment); in
  // that case dlsym returns NULL and we silently skip — identity
  // resolution just won't work for this method_kind in this run.
  u.obj = plugin_dlsym("chat", "chat_identity_register");
  reg   = u.fn;

  if(reg != NULL)
  {
    if(reg("irc", irc_identity_signature,
           irc_identity_score, irc_identity_token_score) != SUCCESS)
      clam(CLAM_WARN, "irc", "chat_identity_register('irc') failed");
  }

  // The IRC driver is made available through the plugin descriptor's
  // ext field. Method instances are created on demand by bot_start()
  // when a bot binds to IRC.
  clam(CLAM_INFO, "irc", "method plugin started (driver available)");
  return(SUCCESS);
}

static bool
irc_stop(void)
{
  // Per-instance disconnect is handled by the driver disconnect
  // callback when bot_stop() or bot_destroy() is called.
  return(SUCCESS);
}

// Tear down the IRC plugin. Unregisters all operator commands
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

// KV schema and plugin descriptor

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
