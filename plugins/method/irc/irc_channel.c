// botmanager — MIT
// IRC channel/member state tracking + admin-mode enforcement helpers.
#define IRC_INTERNAL
#include "irc.h"

// Context cache (nick -> host mapping for MFA)

// Update the nick -> host ring buffer.
void
irc_ctx_update(irc_state_t *st, const char *nick, const char *host)
{
  uint32_t idx;

  if(nick[0] == '\0' || host[0] == '\0')
    return;

  pthread_mutex_lock(&st->ctx_mutex);

  idx = st->ctx_idx % IRC_CTX_CACHE;

  snprintf(st->ctx_cache[idx].nick, IRC_NICK_SZ, "%s", nick);
  snprintf(st->ctx_cache[idx].host, IRC_HOST_SZ, "%s", host);

  st->ctx_idx++;

  pthread_mutex_unlock(&st->ctx_mutex);
}

// Channel member tracking

// Find a tracked channel by name (case-insensitive).
// Must be called with chan_mutex held.
irc_channel_t *
irc_chan_find(irc_state_t *st, const char *channel)
{
  for(irc_channel_t *ch = st->channels; ch != NULL; ch = ch->next)
    if(strcasecmp(ch->name, channel) == 0)
      return(ch);

  return(NULL);
}

// Find or create a tracked channel.
// Must be called with chan_mutex held.
irc_channel_t *
irc_chan_get(irc_state_t *st, const char *channel)
{
  irc_channel_t *ch = irc_chan_find(st, channel);

  if(ch != NULL)
    return(ch);

  ch = mem_alloc("irc", "channel", sizeof(*ch));
  memset(ch, 0, sizeof(*ch));
  strncpy(ch->name, channel, IRC_CHAN_SZ);
  ch->name[IRC_CHAN_SZ] = '\0';
  ch->next = st->channels;
  st->channels = ch;

  return(ch);
}

// Find a member in a channel by nick. Caller must hold chan_mutex.
// returns: member pointer, or NULL if not found
irc_member_t *
irc_member_find(irc_channel_t *ch, const char *nick)
{
  if(ch == NULL || nick == NULL)
    return(NULL);

  for(irc_member_t *m = ch->members; m != NULL; m = m->next)
    if(strcasecmp(m->nick, nick) == 0)
      return(m);

  return(NULL);
}

// Add a nick to a channel's member list (no-op if already present).
void
irc_chan_add_nick(irc_state_t *st, const char *channel, const char *nick)
{
  irc_channel_t *ch;
  irc_member_t *m;

  if(channel == NULL || nick == NULL || nick[0] == '\0')
    return;

  pthread_mutex_lock(&st->chan_mutex);

  ch = irc_chan_get(st, channel);

  // Check for duplicate.
  if(irc_member_find(ch, nick) != NULL)
  {
    pthread_mutex_unlock(&st->chan_mutex);
    return;
  }

  if(ch->member_count >= IRC_CHAN_MAX_MEMBERS)
  {
    pthread_mutex_unlock(&st->chan_mutex);
    return;
  }

  m = mem_alloc("irc", "member", sizeof(*m));
  strncpy(m->nick, nick, IRC_NICK_SZ - 1);
  m->nick[IRC_NICK_SZ - 1] = '\0';
  m->mode_flags = 0;
  m->next = ch->members;
  ch->members = m;
  ch->member_count++;

  pthread_mutex_unlock(&st->chan_mutex);
}

// Remove a nick from a channel's member list.
void
irc_chan_remove_nick(irc_state_t *st, const char *channel, const char *nick)
{
  irc_channel_t *ch;
  irc_member_t **pp;

  if(channel == NULL || nick == NULL)
    return;

  pthread_mutex_lock(&st->chan_mutex);

  ch = irc_chan_find(st, channel);

  if(ch == NULL)
  {
    pthread_mutex_unlock(&st->chan_mutex);
    return;
  }

  pp = &ch->members;

  while(*pp != NULL)
  {
    if(strcasecmp((*pp)->nick, nick) == 0)
    {
      irc_member_t *doomed = *pp;
      *pp = doomed->next;
      mem_free(doomed);
      ch->member_count--;
      break;
    }

    pp = &(*pp)->next;
  }

  pthread_mutex_unlock(&st->chan_mutex);
}

// Remove a nick from all channels (QUIT handling).
void
irc_chan_remove_nick_all(irc_state_t *st, const char *nick)
{
  if(nick == NULL)
    return;

  pthread_mutex_lock(&st->chan_mutex);

  for(irc_channel_t *ch = st->channels; ch != NULL; ch = ch->next)
  {
    irc_member_t **pp = &ch->members;

    while(*pp != NULL)
    {
      if(strcasecmp((*pp)->nick, nick) == 0)
      {
        irc_member_t *doomed = *pp;
        *pp = doomed->next;
        mem_free(doomed);
        ch->member_count--;
        break;
      }

      pp = &(*pp)->next;
    }
  }

  pthread_mutex_unlock(&st->chan_mutex);
}

// Rename a nick across all channels (NICK handling).
void
irc_chan_rename_nick(irc_state_t *st, const char *old_nick,
    const char *new_nick)
{
  if(old_nick == NULL || new_nick == NULL)
    return;

  pthread_mutex_lock(&st->chan_mutex);

  for(irc_channel_t *ch = st->channels; ch != NULL; ch = ch->next)
  {
    for(irc_member_t *m = ch->members; m != NULL; m = m->next)
    {
      if(strcasecmp(m->nick, old_nick) == 0)
      {
        strncpy(m->nick, new_nick, IRC_NICK_SZ - 1);
        m->nick[IRC_NICK_SZ - 1] = '\0';
        break;
      }
    }
  }

  pthread_mutex_unlock(&st->chan_mutex);
}

// Remove a channel entirely (when the bot parts/is kicked).
void
irc_chan_remove(irc_state_t *st, const char *channel)
{
  irc_channel_t **pp;

  if(channel == NULL)
    return;

  pthread_mutex_lock(&st->chan_mutex);

  pp = &st->channels;

  while(*pp != NULL)
  {
    if(strcasecmp((*pp)->name, channel) == 0)
    {
      irc_channel_t *doomed = *pp;
      irc_member_t *m;

      *pp = doomed->next;

      // Free all members.
      m = doomed->members;

      while(m != NULL)
      {
        irc_member_t *next = m->next;
        mem_free(m);
        m = next;
      }

      mem_free(doomed);
      break;
    }

    pp = &(*pp)->next;
  }

  pthread_mutex_unlock(&st->chan_mutex);
}

// Free all channel tracking data.
void
irc_chan_clear_all(irc_state_t *st)
{
  irc_channel_t *ch;

  pthread_mutex_lock(&st->chan_mutex);

  ch = st->channels;

  while(ch != NULL)
  {
    irc_channel_t *next_ch = ch->next;
    irc_member_t *m = ch->members;

    while(m != NULL)
    {
      irc_member_t *next_m = m->next;
      mem_free(m);
      m = next_m;
    }

    mem_free(ch);
    ch = next_ch;
  }

  st->channels = NULL;

  pthread_mutex_unlock(&st->chan_mutex);
}

// Channel admin (mode-based enforcement)

uint8_t
irc_mode_to_flag(char mode)
{
  switch(mode)
  {
    case 'o': return(IRC_MFLAG_OP);
    case 'v': return(IRC_MFLAG_VOICE);
    case 'h': return(IRC_MFLAG_HALFOP);
    case 'q': return(IRC_MFLAG_OWNER);
    case 'a': return(IRC_MFLAG_ADMIN);
    default:  return(0);
  }
}

// Apply channel admin settings (key, topic, etc.) when bot gains ops.
// Called with NO lock held (irc_send_raw may block on socket I/O).
void
irc_apply_chan_admin(irc_state_t *st, const char *channel)
{
  const char *channame;
  char key[KV_KEY_SZ];

  if(channel == NULL || channel[0] != '#')
    return;

  channame = channel + 1;

  // Check master switch.
  snprintf(key, sizeof(key), "%schan.%s.admin.enabled",
      st->kv_prefix, channame);

  if(kv_get_uint(key) == 0)
    return;

  clam(CLAM_INFO, "irc", "applying admin config on %s", channel);

  // Enforce channel key (+k).
  snprintf(key, sizeof(key), "%schan.%s.admin.key.enabled",
      st->kv_prefix, channame);

  if(kv_get_uint(key) != 0)
  {
    const char *val;

    snprintf(key, sizeof(key), "%schan.%s.admin.key.value",
        st->kv_prefix, channame);
    val = kv_get_str(key);

    if(val != NULL && val[0] != '\0')
    {
      irc_send_raw(st, "MODE %s +k %s", channel, val);
      clam(CLAM_INFO, "irc", "%s: set channel key", channel);
    }

    else
      clam(CLAM_WARN, "irc", "%s: admin.key.enabled but no key value set",
          channel);
  }

  // Enforce topic.
  snprintf(key, sizeof(key), "%schan.%s.admin.topic.enabled",
      st->kv_prefix, channame);

  if(kv_get_uint(key) != 0)
  {
    const char *val;

    snprintf(key, sizeof(key), "%schan.%s.admin.topic.value",
        st->kv_prefix, channame);
    val = kv_get_str(key);

    if(val != NULL && val[0] != '\0')
    {
      // Only set if current topic differs.
      bool need_set = true;
      irc_channel_t *ch;

      pthread_mutex_lock(&st->chan_mutex);

      ch = irc_chan_find(st, channel);

      if(ch != NULL && strcmp(ch->topic, val) == 0)
        need_set = false;

      pthread_mutex_unlock(&st->chan_mutex);

      if(need_set)
      {
        irc_send_raw(st, "TOPIC %s :%s", channel, val);
        clam(CLAM_INFO, "irc", "%s: set topic", channel);
      }
    }

    else
      clam(CLAM_WARN, "irc", "%s: admin.topic.enabled but no topic value set",
          channel);
  }
}
