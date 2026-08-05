// botmanager — MIT
// IRC plugin operator commands: /irc network|server|channel, /show irc.

#define IRC_INTERNAL
#include "irc.h"

#include <string.h>

// Server KV registration helper

static bool
irc_register_server_kv(const char *network, const char *server,
    const char *address, const char *port)
{
  char key[KV_KEY_SZ];

  if(plugin_kv_group_register(&irc_kv_groups[1], network, server) == 0)
    return(FAIL);

  // Override address and port with caller-provided values.

  if(address != NULL && address[0] != '\0')
  {
    snprintf(key, sizeof(key), IRC_NET_PREFIX "%s.%s.address",
        network, server);
    kv_set(key, address);
  }

  if(port != NULL && port[0] != '\0')
  {
    snprintf(key, sizeof(key), IRC_NET_PREFIX "%s.%s.port",
        network, server);
    kv_set(key, port);
  }

  return(SUCCESS);
}

// Network list callback (local to commands)

static void
irc_net_list_cb(const char *key, kv_type_t type,
    const char *val, void *data)
{
  irc_net_list_t *list = data;
  char name[IRC_NET_NAME_SZ];

  (void)type;
  (void)val;

  // Key format: irc.net.<NETWORK>.<SERVER>.<PROPERTY>
  // Network is segment 2.
  if(irc_extract_segment(key, 2, name, sizeof(name)) != SUCCESS)
    return;

  // Deduplicate.
  for(uint32_t i = 0; i < list->count; i++)
    if(strcmp(list->names[i], name) == 0)
      return;

  if(list->count < IRC_MAX_NETS)
  {
    snprintf(list->names[list->count], IRC_NET_NAME_SZ, "%s", name);
    list->count++;
  }
}

// /network operator command

// /network list — display all configured IRC networks.
static void
irc_cmd_network_list(const cmd_ctx_t *ctx)
{
  irc_net_list_t nets;
  memset(&nets, 0, sizeof(nets));

  kv_iterate_prefix(IRC_NET_PREFIX, irc_net_list_cb, &nets);

  if(nets.count == 0)
  {
    cmd_reply(ctx, "no IRC networks configured");
    cmd_reply(ctx,
        "use /server add <network> <name> <address> [port] to add one");
    return;
  }

  cmd_reply(ctx, "configured IRC networks:");

  for(uint32_t i = 0; i < nets.count; i++)
  {
    // Count servers for this network.
    char prefix[KV_KEY_SZ];
    irc_srv_list_t srvs;
    char line[128];

    memset(&srvs, 0, sizeof(srvs));
    snprintf(prefix, sizeof(prefix), IRC_NET_PREFIX "%s.", nets.names[i]);
    kv_iterate_prefix(prefix, irc_srv_list_cb, &srvs);

    snprintf(line, sizeof(line), "  %s (%u server%s)",
        nets.names[i], srvs.count,
        srvs.count == 1 ? "" : "s");
    cmd_reply(ctx, line);
  }
}

// /irc network del <name> — delete a network and all its servers.
static void
irc_cmd_network_del(const cmd_ctx_t *ctx)
{
  const char *name = ctx->parsed->argv[0];
  char prefix[KV_KEY_SZ];
  uint32_t deleted;
  char buf[128];

  snprintf(prefix, sizeof(prefix), IRC_NET_PREFIX "%s.", name);

  deleted = kv_delete_prefix(prefix);

  if(deleted > 0)
  {
    snprintf(buf, sizeof(buf),
        "deleted network '%s' (%u entries removed)", name, deleted);
    cmd_reply(ctx, buf);
    kv_flush();
  }

  else
  {
    snprintf(buf, sizeof(buf), "network not found: %s", name);
    cmd_reply(ctx, buf);
  }
}

// /irc network — parent handler, defaults to list when no subcommand given.
static void
irc_cmd_network(const cmd_ctx_t *ctx)
{
  // When invoked without a recognized subcommand, default to list.
  irc_cmd_network_list(ctx);
}

// /server operator command

// /irc server add <network> <host> [port]
static void
irc_cmd_server_add(const cmd_ctx_t *ctx)
{
  const char *network = ctx->parsed->argv[0];
  const char *address = ctx->parsed->argv[1];
  const char *port    = ctx->parsed->argc > 2 ? ctx->parsed->argv[2] : NULL;
  char srvkey[IRC_SRV_NAME_SZ] = {0};
  char check_key[KV_KEY_SZ];
  char buf[256];

  // Derive KV key from address (dots/colons become dashes).
  irc_address_to_key(address, srvkey, sizeof(srvkey));

  // Check if server already exists.
  snprintf(check_key, sizeof(check_key),
      IRC_NET_PREFIX "%s.%s.address", network, srvkey);

  if(kv_exists(check_key))
  {
    snprintf(buf, sizeof(buf),
        "server '%s' already exists in network '%s'", address, network);
    cmd_reply(ctx, buf);
    return;
  }

  // Register KV entries.
  if(irc_register_server_kv(network, srvkey, address,
      port != NULL ? port : "6667") != SUCCESS)
  {
    snprintf(buf, sizeof(buf),
        "failed to register server '%s' in network '%s'",
        address, network);
    cmd_reply(ctx, buf);
    return;
  }

  kv_flush();

  snprintf(buf, sizeof(buf), "added server '%s' to network '%s':",
      address, network);
  cmd_reply(ctx, buf);

  snprintf(buf, sizeof(buf), "  address:    %s", address);
  cmd_reply(ctx, buf);

  snprintf(buf, sizeof(buf), "  port:       %s", port != NULL ? port : "6667");
  cmd_reply(ctx, buf);

  cmd_reply(ctx, "  priority:   100");
  cmd_reply(ctx, "  tls:        off");
  cmd_reply(ctx, "  tls_verify: on");

  cmd_reply(ctx, "use /set to change properties, e.g.:");

  snprintf(buf, sizeof(buf),
      "  /set " IRC_NET_PREFIX "%s.%s.tls 1", network, srvkey);
  cmd_reply(ctx, buf);

  snprintf(buf, sizeof(buf),
      "  /set " IRC_NET_PREFIX "%s.%s.tls_verify 0", network, srvkey);
  cmd_reply(ctx, buf);
}

// /irc server del <network> <host>
static void
irc_cmd_server_del(const cmd_ctx_t *ctx)
{
  const char *network = ctx->parsed->argv[0];
  const char *address = ctx->parsed->argv[1];
  char srvkey[IRC_SRV_NAME_SZ] = {0};
  char prefix[KV_KEY_SZ];
  uint32_t deleted;
  char buf[128];

  // Derive KV key from address.
  irc_address_to_key(address, srvkey, sizeof(srvkey));

  snprintf(prefix, sizeof(prefix),
      IRC_NET_PREFIX "%s.%s.", network, srvkey);

  deleted = kv_delete_prefix(prefix);

  if(deleted > 0)
  {
    snprintf(buf, sizeof(buf),
        "deleted server '%s' from network '%s' (%u entries removed)",
        address, network, deleted);
    cmd_reply(ctx, buf);
    kv_flush();
  }

  else
  {
    snprintf(buf, sizeof(buf),
        "server '%s' not found in network '%s'", address, network);
    cmd_reply(ctx, buf);
  }
}

static void
irc_show_server_entry(const char *network, const char *server,
    const cmd_ctx_t *ctx)
{
  char key[KV_KEY_SZ];
  const char *addr = "";
  uint16_t port = 6667;
  uint16_t prio = 100;
  uint8_t  tls = 0;
  uint8_t  tlsv = 1;
  char line[256];

  snprintf(key, sizeof(key),
      IRC_NET_PREFIX "%s.%s.address", network, server);
  addr = kv_get_str(key);

  if(addr == NULL)
    addr = "";

  snprintf(key, sizeof(key),
      IRC_NET_PREFIX "%s.%s.port", network, server);
  port = (uint16_t)kv_get_uint(key);

  snprintf(key, sizeof(key),
      IRC_NET_PREFIX "%s.%s.priority", network, server);
  prio = (uint16_t)kv_get_uint(key);

  snprintf(key, sizeof(key),
      IRC_NET_PREFIX "%s.%s.tls", network, server);
  tls = (uint8_t)kv_get_uint(key);

  snprintf(key, sizeof(key),
      IRC_NET_PREFIX "%s.%s.tls_verify", network, server);
  tlsv = (uint8_t)kv_get_uint(key);

  snprintf(line, sizeof(line),
      "  %s:%u priority=%u tls=%s verify=%s",
      addr, port, prio,
      tls ? "on" : "off", tlsv ? "on" : "off");
  cmd_reply(ctx, line);
}

static void
irc_show_network_servers(const char *network, const cmd_ctx_t *ctx)
{
  char prefix[KV_KEY_SZ];
  irc_srv_list_t srvs;

  memset(&srvs, 0, sizeof(srvs));
  snprintf(prefix, sizeof(prefix), IRC_NET_PREFIX "%s.", network);
  kv_iterate_prefix(prefix, irc_srv_list_cb, &srvs);

  if(srvs.count == 0)
  {
    char buf[128];

    snprintf(buf, sizeof(buf), "%s: no servers configured", network);
    cmd_reply(ctx, buf);
    return;
  }

  {
    char buf[128];

    snprintf(buf, sizeof(buf), "%s:", network);
    cmd_reply(ctx, buf);
  }

  for(uint32_t s = 0; s < srvs.count; s++)
    irc_show_server_entry(network, srvs.names[s], ctx);
}

// /irc server list [network]
static void
irc_cmd_server_list(const cmd_ctx_t *ctx)
{
  const char *network = (ctx->parsed != NULL && ctx->parsed->argc > 0)
      ? ctx->parsed->argv[0] : NULL;

  // If a specific network was given, list only its servers.
  // If no network, list all.
  irc_net_list_t nets;
  memset(&nets, 0, sizeof(nets));

  if(network != NULL)
  {
    // Single network.
    snprintf(nets.names[0], IRC_NET_NAME_SZ, "%s", network);
    nets.count = 1;
  }

  else
    // All networks.
    kv_iterate_prefix(IRC_NET_PREFIX, irc_net_list_cb, &nets);

  if(nets.count == 0)
  {
    cmd_reply(ctx, "no IRC networks configured");
    return;
  }

  for(uint32_t n = 0; n < nets.count; n++)
    irc_show_network_servers(nets.names[n], ctx);
}

// /irc server — parent handler, defaults to list when no subcommand given.
static void
irc_cmd_server(const cmd_ctx_t *ctx)
{
  // When invoked without a recognized subcommand, default to list.
  irc_cmd_server_list(ctx);
}

// Helpers

static irc_state_t *
irc_find_state_for_bot(const char *botname)
{
  char inst_name[METHOD_NAME_SZ];
  method_inst_t *inst;

  snprintf(inst_name, sizeof(inst_name), "%s_irc", botname);

  inst = method_find(inst_name);

  if(inst == NULL)
    return(NULL);

  return(method_get_handle(inst));
}

// /channel operator command

// /irc channel add <bot> <#channel> [key]
static void
irc_cmd_channel_add(const cmd_ctx_t *ctx)
{
  const char *botname  = ctx->parsed->argv[0];
  const char *raw_chan  = ctx->parsed->argv[1];
  const char *chankey   = ctx->parsed->argc > 2 ? ctx->parsed->argv[2] : NULL;
  const char *channel;
  char prefix[KV_KEY_SZ];
  char key[KV_KEY_SZ];
  char buf[256];

  // Strip leading '#' if present.
  channel = (raw_chan[0] == '#') ? raw_chan + 1 : raw_chan;

  if(channel[0] == '\0')
  {
    cmd_reply(ctx, "channel name cannot be empty after '#'");
    return;
  }

  // Build the KV prefix for this bot's IRC channels.
  snprintf(prefix, sizeof(prefix),
      "bot.%s.irc.chan.%s.", botname, channel);

  // Check if channel already exists.
  snprintf(key, sizeof(key), "%sautojoin", prefix);

  if(kv_exists(key))
  {
    snprintf(buf, sizeof(buf),
        "channel '#%s' already configured for bot '%s'",
        channel, botname);
    cmd_reply(ctx, buf);
    return;
  }

  // Register channel KV entries from the schema group.
  if(plugin_kv_group_register(&irc_kv_groups[0], botname, channel) == 0)
  {
    cmd_reply(ctx, "failed to register channel configuration");
    return;
  }

  // Override the default for "key" when a channel key was provided.
  if(chankey != NULL)
  {
    snprintf(key, sizeof(key), "%skey", prefix);
    kv_set(key, chankey);
  }

  kv_flush();

  snprintf(buf, sizeof(buf),
      "added channel '#%s' to bot '%s' (autojoin enabled)",
      channel, botname);
  cmd_reply(ctx, buf);

  if(chankey != NULL)
    cmd_reply(ctx, "  channel key: (set)");
}

// /irc channel del <bot> <#channel>
static void
irc_cmd_channel_del(const cmd_ctx_t *ctx)
{
  const char *botname  = ctx->parsed->argv[0];
  const char *raw_chan  = ctx->parsed->argv[1];
  const char *channel;
  char prefix[KV_KEY_SZ];
  uint32_t deleted;
  char buf[128];

  // Strip leading '#' if present.
  channel = (raw_chan[0] == '#') ? raw_chan + 1 : raw_chan;

  snprintf(prefix, sizeof(prefix),
      "bot.%s.irc.chan.%s.", botname, channel);

  deleted = kv_delete_prefix(prefix);

  if(deleted > 0)
  {
    irc_state_t *st;

    snprintf(buf, sizeof(buf),
        "removed channel '#%s' from bot '%s' (%u entries removed)",
        channel, botname, deleted);
    cmd_reply(ctx, buf);
    kv_flush();

    // If the bot is connected, depart the channel.
    st = irc_find_state_for_bot(botname);

    if(st != NULL && st->connected)
    {
      snprintf(buf, sizeof(buf), "parting '#%s'", channel);
      cmd_reply(ctx, buf);

      irc_send_raw(st, "PART #%s", channel);
    }
  }

  else
  {
    snprintf(buf, sizeof(buf),
        "channel '#%s' not found for bot '%s'", channel, botname);
    cmd_reply(ctx, buf);
  }
}

// /irc channel list <bot>
static void
irc_cmd_channel_list(const cmd_ctx_t *ctx)
{
  const char *botname = ctx->parsed->argv[0];
  char chan_prefix[KV_KEY_SZ];
  irc_chan_collect_t cc;
  char buf[128];

  // Build the channel prefix for this bot.
  snprintf(chan_prefix, sizeof(chan_prefix),
      "bot.%s.irc.chan.", botname);

  // Discover all configured channels.
  memset(&cc, 0, sizeof(cc));
  cc.prefix_len = strlen(chan_prefix);
  kv_iterate_prefix(chan_prefix, irc_chan_collect_cb, &cc);

  if(cc.count == 0)
  {
    snprintf(buf, sizeof(buf),
        "no channels configured for bot '%s'", botname);
    cmd_reply(ctx, buf);
    return;
  }

  snprintf(buf, sizeof(buf), "%s:", botname);
  cmd_reply(ctx, buf);

  for(uint32_t c = 0; c < cc.count; c++)
  {
    char key[KV_KEY_SZ];
    uint8_t autojoin = 0;
    uint8_t announce = 0;
    const char *text = "";
    const char *has_key;
    char line[256];
    uint8_t admin;

    snprintf(key, sizeof(key), "%s%s.autojoin",
        chan_prefix, cc.names[c]);
    autojoin = (uint8_t)kv_get_uint(key);

    snprintf(key, sizeof(key), "%s%s.announce",
        chan_prefix, cc.names[c]);
    announce = (uint8_t)kv_get_uint(key);

    snprintf(key, sizeof(key), "%s%s.key",
        chan_prefix, cc.names[c]);
    has_key = kv_get_str(key);

    snprintf(key, sizeof(key), "%s%s.announcetext",
        chan_prefix, cc.names[c]);
    text = kv_get_str(key);

    snprintf(line, sizeof(line),
        "  #%s: autojoin=%s key=%s announce=%s",
        cc.names[c],
        autojoin ? "on" : "off",
        (has_key && has_key[0]) ? "(set)" : "(none)",
        announce ? "on" : "off");
    cmd_reply(ctx, line);

    if(announce && text != NULL && text[0] != '\0')
    {
      snprintf(line, sizeof(line), "    announcetext: %s", text);
      cmd_reply(ctx, line);
    }

    // Show admin settings if admin is enabled.
    snprintf(key, sizeof(key), "%s%s.admin.enabled",
        chan_prefix, cc.names[c]);
    admin = (uint8_t)kv_get_uint(key);

    if(admin)
    {
      uint8_t key_en;
      const char *key_val;
      uint8_t topic_en;
      const char *topic_val;
      uint8_t kick_un;

      snprintf(key, sizeof(key), "%s%s.admin.key.enabled",
          chan_prefix, cc.names[c]);
      key_en = (uint8_t)kv_get_uint(key);

      snprintf(key, sizeof(key), "%s%s.admin.key.value",
          chan_prefix, cc.names[c]);
      key_val = kv_get_str(key);

      snprintf(key, sizeof(key), "%s%s.admin.topic.enabled",
          chan_prefix, cc.names[c]);
      topic_en = (uint8_t)kv_get_uint(key);

      snprintf(key, sizeof(key), "%s%s.admin.topic.value",
          chan_prefix, cc.names[c]);
      topic_val = kv_get_str(key);

      snprintf(key, sizeof(key), "%s%s.admin.kick_unident",
          chan_prefix, cc.names[c]);
      kick_un = (uint8_t)kv_get_uint(key);

      snprintf(line, sizeof(line),
          "    admin: key=%s topic=%s kick_unident=%s",
          key_en ? ((key_val && key_val[0]) ? "(set)" : "(empty!)") : "off",
          topic_en ? ((topic_val && topic_val[0]) ? "(set)" : "(empty!)") : "off",
          kick_un ? "on" : "off");
      cmd_reply(ctx, line);
    }
  }
}

// /irc join <bot> <#channel> — instruct a bot to join a channel.
static void
irc_cmd_join(const cmd_ctx_t *ctx)
{
  const char *botname = ctx->parsed->argv[0];
  const char *raw_chan = ctx->parsed->argv[1];
  const char *channel;
  irc_state_t *st;
  char key[KV_KEY_SZ];
  const char *chankey;
  char buf[128];

  // Strip leading '#' if present.
  channel = (raw_chan[0] == '#') ? raw_chan + 1 : raw_chan;

  st = irc_find_state_for_bot(botname);

  if(st == NULL || !st->connected)
  {
    snprintf(buf, sizeof(buf),
        "bot '%s' is not connected to IRC", botname);
    cmd_reply(ctx, buf);
    return;
  }

  // Check for a channel key in the KV config.
  snprintf(key, sizeof(key), "bot.%s.irc.chan.%s.key", botname, channel);

  chankey = kv_get_str(key);

  if(chankey != NULL && chankey[0] != '\0')
    irc_send_raw(st, "JOIN #%s %s", channel, chankey);
  else
    irc_send_raw(st, "JOIN #%s", channel);

  snprintf(buf, sizeof(buf), "joining '#%s' on bot '%s'", channel, botname);
  cmd_reply(ctx, buf);
}

// /irc part <bot> <#channel> — instruct a bot to leave a channel.
static void
irc_cmd_part(const cmd_ctx_t *ctx)
{
  const char *botname = ctx->parsed->argv[0];
  const char *raw_chan = ctx->parsed->argv[1];
  const char *channel;
  irc_state_t *st;
  char buf[128];

  // Strip leading '#' if present.
  channel = (raw_chan[0] == '#') ? raw_chan + 1 : raw_chan;

  st = irc_find_state_for_bot(botname);

  if(st == NULL || !st->connected)
  {
    snprintf(buf, sizeof(buf),
        "bot '%s' is not connected to IRC", botname);
    cmd_reply(ctx, buf);
    return;
  }

  snprintf(buf, sizeof(buf), "parting '#%s' on bot '%s'", channel, botname);
  cmd_reply(ctx, buf);

  irc_send_raw(st, "PART #%s", channel);
}

// /irc channel — parent handler, shows usage.
static void
irc_cmd_channel(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /irc channel <subcommand> ...");
}

// /irc root and /show irc subcommands

// /irc — root parent handler, lists available subcommands.
static void
irc_cmd_irc(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /irc <subcommand> ...");
}

// /irc schema [group] — show entity schemas declared by the IRC plugin.
static void
irc_cmd_schema(const cmd_ctx_t *ctx)
{
  const char *group_name = ctx->args;
  const plugin_kv_group_t *g = NULL;
  char hdr[256];

  if(group_name == NULL || group_name[0] == '\0')
  {
    // List all schema groups.
    cmd_reply(ctx, "IRC entity schemas:");

    for(uint32_t i = 0; i < IRC_KV_GROUPS_COUNT; i++)
    {
      char line[256];

      snprintf(line, sizeof(line), "  %-12s — %s (managed by /irc %s)",
          irc_kv_groups[i].name,
          irc_kv_groups[i].description,
          irc_kv_groups[i].cmd_name);
      cmd_reply(ctx, line);
    }

    return;
  }

  // Find the specific schema group.
  for(uint32_t i = 0; i < IRC_KV_GROUPS_COUNT; i++)
  {
    if(strcmp(irc_kv_groups[i].name, group_name) == 0)
    {
      g = &irc_kv_groups[i];
      break;
    }
  }

  if(g == NULL)
  {
    char buf[128];

    snprintf(buf, sizeof(buf), "unknown schema group: %s", group_name);
    cmd_reply(ctx, buf);
    return;
  }

  // Show schema detail.
  snprintf(hdr, sizeof(hdr), "schema: %s — %s", g->name, g->description);
  cmd_reply(ctx, hdr);

  snprintf(hdr, sizeof(hdr), "  key pattern: %s", g->key_prefix);
  cmd_reply(ctx, hdr);

  snprintf(hdr, sizeof(hdr), "  command: /irc %s", g->cmd_name);
  cmd_reply(ctx, hdr);
  cmd_reply(ctx, "  properties:");

  for(uint32_t i = 0; i < g->schema_count; i++)
  {
    const plugin_kv_entry_t *e = &g->schema[i];
    char line[256];

    snprintf(line, sizeof(line), "    %-16s %-8s default: %s",
        e->key, kv_type_name(e->type),
        (e->default_val && e->default_val[0]) ? e->default_val : "(empty)");
    cmd_reply(ctx, line);
  }
}

// /show irc — parent handler, lists available subcommands.
static void
irc_cmd_show_irc(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /show irc <subcommand> ...");
}

// /show irc networks — list defined networks.
static void
irc_cmd_show_networks(const cmd_ctx_t *ctx)
{
  irc_cmd_network_list(ctx);
}

// /show irc servers [network] — list servers.
// Delegates to irc_cmd_server_list which reads ctx->parsed.
static void
irc_cmd_show_servers(const cmd_ctx_t *ctx)
{
  irc_cmd_server_list(ctx);
}

// Command registration

// Register all /irc and /show irc operator commands. Called from irc_init().
void
irc_register_commands(void)
{
  // Register /irc root parent command.
  cmd_register("irc", "irc",
      "irc <subcommand> ...",
      "IRC plugin commands",
      "Manage IRC networks, servers, and channels.\n"
      "  /irc network ...  — manage networks\n"
      "  /irc server  ...  — manage servers\n"
      "  /irc channel ...  — manage channels\n"
      "  /irc join ...     — join a channel\n"
      "  /irc part ...     — leave a channel\n"
      "  /irc schema  ...  — show entity schemas",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, irc_cmd_irc,
      NULL, NULL, NULL, NULL, 0, NULL, NULL);

  // Register subcommands under /irc.
  cmd_register("irc", "network",
      "irc network <list|del> [name]",
      "Manage IRC networks",
      "Manage the IRC network registry. Networks are named groups\n"
      "that organize IRC servers.\n"
      "  /irc network list        — list all networks\n"
      "  /irc network del <name>  — delete a network and its servers",
      USERNS_GROUP_ADMIN, 500, CMD_SCOPE_ANY, METHOD_T_ANY, irc_cmd_network,
      NULL, "irc", "net", NULL, 0, NULL, NULL);

  cmd_register("irc", "list",
      "irc network list",
      "List all IRC networks",
      NULL,
      USERNS_GROUP_ADMIN, 500, CMD_SCOPE_ANY, METHOD_T_ANY,
      irc_cmd_network_list, NULL, "network", "l", NULL, 0, NULL, NULL);

  cmd_register("irc", "del",
      "irc network del <name>",
      "Delete a network and its servers",
      NULL,
      USERNS_GROUP_ADMIN, 500, CMD_SCOPE_ANY, METHOD_T_ANY,
      irc_cmd_network_del, NULL, "network", "d", ad_irc_netname, 1, NULL, NULL);

  cmd_register("irc", "server",
      "irc server <add|del|list> ...",
      "Manage IRC servers",
      "Manage IRC servers within networks.\n"
      "  /irc server add <net> <host> [port]  — add a server\n"
      "  /irc server del <net> <host>         — remove a server\n"
      "  /irc server list [net]               — list servers",
      USERNS_GROUP_ADMIN, 500, CMD_SCOPE_ANY, METHOD_T_ANY, irc_cmd_server,
      NULL, "irc", "srv", NULL, 0, NULL, NULL);

  cmd_register("irc", "add",
      "irc server add <network> <host> [port]",
      "Add a server to a network",
      NULL,
      USERNS_GROUP_ADMIN, 500, CMD_SCOPE_ANY, METHOD_T_ANY,
      irc_cmd_server_add, NULL, "server", "a", ad_irc_srv_add, 3, NULL, NULL);

  cmd_register("irc", "del",
      "irc server del <network> <host>",
      "Remove a server from a network",
      NULL,
      USERNS_GROUP_ADMIN, 500, CMD_SCOPE_ANY, METHOD_T_ANY,
      irc_cmd_server_del, NULL, "server", "d", ad_irc_srv_del, 2, NULL, NULL);

  cmd_register("irc", "list",
      "irc server list [network]",
      "List servers",
      NULL,
      USERNS_GROUP_ADMIN, 500, CMD_SCOPE_ANY, METHOD_T_ANY,
      irc_cmd_server_list, NULL, "server", "l", ad_irc_srv_list, 1, NULL, NULL);

  cmd_register("irc", "channel",
      "irc channel <add|del|list> ...",
      "Manage IRC channels for a bot",
      "Manage IRC channel configuration for bot instances.\n"
      "  /irc channel add <bot> <#channel> [key]  — add a channel\n"
      "  /irc channel del <bot> <#channel>        — remove a channel\n"
      "  /irc channel list <bot>                  — list channels",
      USERNS_GROUP_ADMIN, 500, CMD_SCOPE_ANY, METHOD_T_ANY,
      irc_cmd_channel, NULL, "irc", "ch", NULL, 0, NULL, NULL);

  cmd_register("irc", "add",
      "irc channel add <bot> <#channel> [key]",
      "Add a channel to a bot",
      NULL,
      USERNS_GROUP_ADMIN, 500, CMD_SCOPE_ANY, METHOD_T_ANY,
      irc_cmd_channel_add, NULL, "channel", "a", ad_irc_chan_add, 3, NULL, NULL);

  cmd_register("irc", "del",
      "irc channel del <bot> <#channel>",
      "Remove a channel from a bot",
      NULL,
      USERNS_GROUP_ADMIN, 500, CMD_SCOPE_ANY, METHOD_T_ANY,
      irc_cmd_channel_del, NULL, "channel", "d", ad_irc_chan_del, 2, NULL, NULL);

  cmd_register("irc", "list",
      "irc channel list <bot>",
      "List channels for a bot",
      NULL,
      USERNS_GROUP_ADMIN, 500, CMD_SCOPE_ANY, METHOD_T_ANY,
      irc_cmd_channel_list, NULL, "channel", "l", ad_irc_chan_list, 1, NULL, NULL);

  cmd_register("irc", "join",
      "irc join <bot> <#channel>",
      "Instruct a bot to join a channel",
      "Sends a JOIN command for the specified channel on the bot's\n"
      "IRC connection. If a channel key is configured, it is sent\n"
      "automatically. The bot must be connected to IRC.",
      USERNS_GROUP_ADMIN, 500, CMD_SCOPE_ANY, METHOD_T_ANY, irc_cmd_join,
      NULL, "irc", NULL, ad_irc_chan_del, 2, NULL, NULL);

  cmd_register("irc", "part",
      "irc part <bot> <#channel>",
      "Instruct a bot to leave a channel",
      "Sends a PART command for the specified channel on the bot's\n"
      "IRC connection. The bot must be connected to IRC.",
      USERNS_GROUP_ADMIN, 500, CMD_SCOPE_ANY, METHOD_T_ANY, irc_cmd_part,
      NULL, "irc", NULL, ad_irc_chan_del, 2, NULL, NULL);

  cmd_register("irc", "irc-schema",
      "irc schema [group]",
      "Show IRC entity schemas",
      "Display configurable properties for IRC entities.\n"
      "  /irc schema          — list available schema groups\n"
      "  /irc schema channel  — show channel properties\n"
      "  /irc schema server   — show server properties",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, irc_cmd_schema,
      NULL, "irc", "schema", NULL, 0, NULL, NULL);

  // Register /show irc subcommand tree.
  // Internal name "show-irc" avoids collision with root /irc.
  // Abbreviation "irc" allows /show irc resolution.
  cmd_register("irc", "show-irc",
      "show irc <subcommand> ...",
      "IRC network and server information",
      "Display IRC network and server configuration.\n"
      "  /show irc networks  — list defined networks\n"
      "  /show irc servers   — list servers (optionally by network)",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      irc_cmd_show_irc, NULL, "show", "irc", NULL, 0, NULL, NULL);

  cmd_register("irc", "networks",
      "show irc networks",
      "List IRC networks",
      "Lists all defined IRC networks and the number of servers\n"
      "configured for each.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      irc_cmd_show_networks, NULL, "show-irc", "n", NULL, 0, NULL, NULL);

  cmd_register("irc", "servers",
      "show irc servers [network]",
      "List IRC servers",
      "Lists IRC servers with address, port, priority, and TLS\n"
      "settings. If a network name is given, only servers for\n"
      "that network are shown.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      irc_cmd_show_servers, NULL, "show-irc", "s", ad_irc_srv_list, 1, NULL, NULL);
}
