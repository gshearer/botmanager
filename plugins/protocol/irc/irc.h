#ifndef BM_IRC_H
#define BM_IRC_H

#include "common.h"
#include "kv.h"
#include "method.h"
#include "sock.h"
#include "task.h"

#define IRC_BUF_SZ      4096
#define IRC_LINE_SZ     512
#define IRC_NICK_SZ     32
#define IRC_HOST_SZ     256
#define IRC_PREFIX_SZ   256
#define IRC_CTX_CACHE   64

// IRC network/server configuration name limits.
#define IRC_NET_NAME_SZ 32
#define IRC_SRV_NAME_SZ IRC_HOST_SZ

// KV key prefix for IRC network configuration.
// Full key format: irc.net.<NETWORK>.<SERVER>.<PROPERTY>
// Properties: address, port, priority, tls, tls_verify
#define IRC_NET_PREFIX  "irc.net."

// Maximum networks, servers, and channels for listing.
#define IRC_MAX_NETS    64
#define IRC_MAX_SRVS    64
#define IRC_MAX_CHANS   64

// Channel name size (without # prefix) for KV key segments.
#define IRC_CHAN_SZ     64
#define IRC_CHAN_MAX_MEMBERS 512

// Member mode flags (tracked from NAMES prefixes and MODE changes).
#define IRC_MFLAG_OP     0x01  // @ — channel operator
#define IRC_MFLAG_VOICE  0x02  // + — voiced
#define IRC_MFLAG_HALFOP 0x04  // % — half-operator
#define IRC_MFLAG_OWNER  0x08  // ~ — channel owner
#define IRC_MFLAG_ADMIN  0x10  // & — channel admin (protected)

// Parsed IRC protocol message.
typedef struct
{
  char prefix[IRC_PREFIX_SZ];       // full prefix string
  char nick[IRC_NICK_SZ];          // extracted nick from prefix
  char user[IRC_NICK_SZ];          // extracted user from prefix
  char host[IRC_HOST_SZ];          // extracted host from prefix
  char command[32];                 // PRIVMSG, PING, 001, etc.
  char params[IRC_LINE_SZ];        // middle parameters
  char trailing[IRC_LINE_SZ];      // text after final ':'
  bool has_trailing;
} irc_parsed_msg_t;

// Per-channel member tracking.
typedef struct irc_member
{
  char                nick[IRC_NICK_SZ];
  uint8_t             mode_flags;     // IRC_MFLAG_* bitmask
  struct irc_member  *next;
} irc_member_t;

typedef struct irc_channel
{
  char                 name[IRC_CHAN_SZ + 1]; // includes '#' prefix
  irc_member_t        *members;
  uint32_t             member_count;
  bool                 have_ops;      // true when bot has +o on this channel
  char                 topic[IRC_LINE_SZ]; // current topic (from 332/TOPIC)
  struct irc_channel  *next;
} irc_channel_t;

// IRC method instance state (opaque handle).
typedef struct
{
  // Instance identity.
  char              inst_name[METHOD_NAME_SZ];
  char              kv_prefix[KV_KEY_SZ]; // "bot.<botname>.irc."
  method_inst_t    *inst;           // set by connect(), not create()

  // Socket session (core socket service).
  sock_session_t   *session;

  // Connection state.
  bool              registered;     // true after RPL_WELCOME (001)
  bool              connected;      // true while socket is open
  bool              shutdown;       // signal to stop reconnecting

  // Read buffer for partial line assembly.
  char              buf[IRC_BUF_SZ];
  size_t            buf_len;

  // Configuration snapshot (read from KV at each connect attempt).
  char              network[IRC_NET_NAME_SZ]; // plugin.irc.network.<name>
  char              host[IRC_HOST_SZ];        // resolved from network
  uint16_t          port;                     // resolved from network
  uint8_t           tls;                      // resolved from network
  uint8_t           tls_verify;               // resolved from network
  char              nick[IRC_NICK_SZ];        // preferred nick
  char              nick2[IRC_NICK_SZ];       // first fallback
  char              nick3[IRC_NICK_SZ];       // second fallback
  char              cur_nick[IRC_NICK_SZ];    // current nick (may differ after 433)
  uint8_t           nick_attempt;             // 0=nick, 1=nick2, 2=nick3
  char              user[IRC_NICK_SZ];
  char              realname[KV_STR_SZ];
  char              pass[KV_STR_SZ];          // resolved from network
  uint32_t          reconnect_delay;          // resolved from network
  uint32_t          server_idx;               // current server index in network

  // Pending reconnect task. Set when task_add_deferred schedules a
  // reconnect; cleared by the task itself when it fires, and
  // cancelled by irc_disconnect so the task doesn't fire after the
  // method has been stopped.
  task_handle_t     reconnect_task;

  // Channel member tracking.
  irc_channel_t    *channels;
  pthread_mutex_t   chan_mutex;

  // nick -> host cache for get_context() (ring buffer).
  struct
  {
    char nick[IRC_NICK_SZ];
    char host[IRC_HOST_SZ];
  }                 ctx_cache[IRC_CTX_CACHE];
  pthread_mutex_t   ctx_mutex;
  uint32_t          ctx_idx;
} irc_state_t;

#ifdef IRC_INTERNAL

#include "bot.h"
#include "clam.h"
#include "cmd.h"
#include "db.h"
#include "alloc.h"
#include "plugin.h"
#include "pool.h"
#include "task.h"
#include "version.h"

#include <ctype.h>
#include <stdarg.h>
#include "validate.h"
#include "irc_dossier.h"

// Forward declarations (irc.c only).
static void irc_sock_cb(const sock_event_t *event, void *user_data);
static void irc_reconnect_task(task_t *t);
static void irc_attempt_connect(irc_state_t *st);

// Server name collection (shared by resolve and listing).
typedef struct
{
  char     names[IRC_MAX_SRVS][IRC_SRV_NAME_SZ];
  uint32_t count;
} irc_srv_list_t;

// Shared helper functions (defined in irc.c, used by irc_commands.c).
bool irc_extract_segment(const char *key, uint32_t segment,
    char *out, size_t out_sz);
void irc_srv_list_cb(const char *key, kv_type_t type,
    const char *val, void *data);
void irc_chan_collect_cb(const char *key, kv_type_t type,
    const char *val, void *data);
void irc_init_networks(void);
bool irc_send_raw(irc_state_t *st, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void irc_register_commands(void);

// Protocol helpers (defined in irc_protocol.c).
void irc_parse_prefix(const char *prefix, char *nick, char *user, char *host);
void irc_parse_line(const char *line, irc_parsed_msg_t *out);
bool irc_send_privmsg(irc_state_t *st, const char *target, const char *text);
void irc_send_registration(irc_state_t *st);
void irc_expand_vars(const char *tmpl, char *out, size_t out_sz,
    const irc_state_t *st, const char *channel);

// Server entry for sorting by priority during resolution.
typedef struct
{
  char     name[IRC_SRV_NAME_SZ];
  uint16_t priority;
} irc_srv_entry_t;

// Callback data for channel discovery via kv_iterate_prefix().
typedef struct
{
  char     names[IRC_MAX_CHANS][IRC_CHAN_SZ];
  uint32_t count;
  size_t   prefix_len;  // length of "bot.<name>.irc.chan."
} irc_chan_collect_t;

// Callback data for collecting unique network names.
typedef struct
{
  char     names[IRC_MAX_NETS][IRC_NET_NAME_SZ];
  uint32_t count;
} irc_net_list_t;

void irc_nick_kv_cb(const char *key, void *data);

// Instance-level KV schema: bare suffixes cloned into
// "bot.<botname>.irc.<suffix>" when a bot binds to IRC.
static const plugin_kv_entry_t irc_inst_kv_schema[] = {
  { "network",   KV_STR,    "",           "IRC network name to connect to",  NULL },
  { "nick",      KV_STR,    "",           "Primary nickname",                irc_nick_kv_cb },
  { "nick2",     KV_STR,    "",           "First alternate nickname",        NULL },
  { "nick3",     KV_STR,    "",           "Second alternate nickname",       NULL },
  { "user",      KV_STR,    "",           "IRC username (ident)",            NULL },
  { "realname",  KV_STR,    "BotManager", "IRC realname (GECOS) field",      NULL },
  { "prefix",    KV_STR,    "!",          "Command prefix character for this bot on IRC", NULL },
};

// Per-channel KV schema: suffixes appended to
// "bot.<botname>.irc.chan.<channel>." when a channel is added.
static const plugin_kv_entry_t irc_chan_kv_schema[] = {
  { "autojoin",     KV_BOOL,   "true",  "Auto-join this channel on connect (true/false)" },
  { "key",          KV_STR,    "",      "Channel key (password) for joining" },
  { "announce",     KV_BOOL,   "false", "Send announce text on join (true/false)" },
  { "announcetext", KV_STR,    "",      "Text to send to channel on join" },

  // Channel administration (enforced when bot has +o).
  { "admin.enabled",            KV_BOOL,   "false", "Enable channel administration when bot has ops" },
  { "admin.key.enabled",        KV_BOOL,   "false", "Maintain a channel key (+k)" },
  { "admin.key.value",          KV_STR,    "",      "Channel key to enforce" },
  { "admin.topic.enabled",      KV_BOOL,   "false", "Maintain channel topic" },
  { "admin.topic.value",        KV_STR,    "",      "Topic to enforce" },
  { "admin.kick_unident",       KV_BOOL,   "false", "Kick users who are not identified to the bot" },
  { "admin.kick_unident_delay", KV_UINT16, "30",    "Seconds to wait before kicking unidentified user" },
  { "admin.kick_unident_msg",   KV_STR,
      "You must identify to use this channel",
      "Kick message for unidentified users" },
};

#define IRC_CHAN_KV_COUNT \
    (sizeof(irc_chan_kv_schema) / sizeof(irc_chan_kv_schema[0]))

// Per-server KV schema: suffixes appended to
// "irc.net.<network>.<server>." when a server is added.
static const plugin_kv_entry_t irc_srv_kv_schema[] = {
  { "address",    KV_STR,    "",     "Server hostname or IP address" },
  { "port",       KV_UINT16, "6667", "Server port number" },
  { "priority",   KV_UINT16, "100",  "Server selection priority (lower = preferred)" },
  { "tls",        KV_BOOL,   "false", "Enable TLS encryption (true/false)" },
  { "tls_verify", KV_BOOL,   "true",  "Verify TLS certificate (true/false)" },
};

#define IRC_SRV_KV_COUNT \
    (sizeof(irc_srv_kv_schema) / sizeof(irc_srv_kv_schema[0]))

// Named KV schema groups for IRC dynamic entities.
static const plugin_kv_group_t irc_kv_groups[] = {
  {
    .name         = "channel",
    .description  = "Per-channel IRC configuration",
    .key_prefix   = "bot.%s.irc.chan.%s.",
    .prefix_args  = 2,
    .cmd_name     = "channel",
    .schema       = irc_chan_kv_schema,
    .schema_count = IRC_CHAN_KV_COUNT,
  },
  {
    .name         = "server",
    .description  = "Per-server IRC network configuration",
    .key_prefix   = "irc.net.%s.%s.",
    .prefix_args  = 2,
    .cmd_name     = "server",
    .schema       = irc_srv_kv_schema,
    .schema_count = IRC_SRV_KV_COUNT,
  },
};

#define IRC_KV_GROUPS_COUNT \
    (sizeof(irc_kv_groups) / sizeof(irc_kv_groups[0]))

// Channel member tracking helpers (defined in irc_channel.c).
irc_channel_t *irc_chan_find(irc_state_t *st, const char *channel);
irc_channel_t *irc_chan_get(irc_state_t *st, const char *channel);
irc_member_t  *irc_member_find(irc_channel_t *ch, const char *nick);
void irc_chan_add_nick(irc_state_t *st, const char *channel, const char *nick);
void irc_chan_remove_nick(irc_state_t *st, const char *channel, const char *nick);
void irc_chan_remove_nick_all(irc_state_t *st, const char *nick);
void irc_chan_rename_nick(irc_state_t *st, const char *old_nick, const char *new_nick);
void irc_chan_remove(irc_state_t *st, const char *channel);
void irc_chan_clear_all(irc_state_t *st);

// Context cache + mode/admin helpers (defined in irc_channel.c).
void    irc_ctx_update(irc_state_t *st, const char *nick, const char *host);
uint8_t irc_mode_to_flag(char mode);
void    irc_apply_chan_admin(irc_state_t *st, const char *channel);

// Shared validation (defined in irc.c, used by arg descriptors).
bool irc_valid_name(const char *name);
void irc_address_to_key(const char *address, char *out, size_t out_sz);

// Forward declarations for driver vtable.
static void *irc_create(const char *inst_name);
static void irc_destroy(void *handle);
static bool irc_connect(void *handle);
static void irc_disconnect(void *handle);
static bool irc_send(void *handle, const char *target, const char *text);
static bool irc_send_emote(void *handle, const char *target, const char *text);
static bool irc_get_context(void *handle, const char *sender,
    char *ctx, size_t ctx_sz);
static void irc_list_channel(void *handle, const char *channel,
    method_chan_member_cb_t cb, void *data);
static void irc_list_joined_channels(void *handle,
    method_joined_channel_cb_t cb, void *data);
static bool irc_get_self(void *handle, char *buf, size_t buf_sz);

// Identity signer for the chat plugin's identity registry (ND4).
// Not part of the method_driver_t vtable.
static bool irc_identity_signature(const method_msg_t *msg,
    char *out_json, size_t out_sz);

// Argument specs for IRC subcommands.
static const cmd_arg_desc_t ad_irc_netname[] = {
  { "name", CMD_ARG_CUSTOM, CMD_ARG_REQUIRED, IRC_NET_NAME_SZ, irc_valid_name },
};

static const cmd_arg_desc_t ad_irc_srv_add[] = {
  { "network", CMD_ARG_CUSTOM,    CMD_ARG_REQUIRED, IRC_NET_NAME_SZ, irc_valid_name },
  { "host",    CMD_ARG_HOSTNAME,  CMD_ARG_REQUIRED, IRC_HOST_SZ,     NULL },
  { "port",    CMD_ARG_PORT,      CMD_ARG_OPTIONAL, 8,               NULL },
};

static const cmd_arg_desc_t ad_irc_srv_del[] = {
  { "network", CMD_ARG_CUSTOM,    CMD_ARG_REQUIRED, IRC_NET_NAME_SZ, irc_valid_name },
  { "host",    CMD_ARG_HOSTNAME,  CMD_ARG_REQUIRED, IRC_HOST_SZ,     NULL },
};

static const cmd_arg_desc_t ad_irc_srv_list[] = {
  { "network", CMD_ARG_CUSTOM, CMD_ARG_OPTIONAL, IRC_NET_NAME_SZ, irc_valid_name },
};

static const cmd_arg_desc_t ad_irc_chan_add[] = {
  { "bot",     CMD_ARG_ALNUM,   CMD_ARG_REQUIRED, BOT_NAME_SZ,  NULL },
  { "channel", CMD_ARG_CHANNEL, CMD_ARG_REQUIRED, IRC_CHAN_SZ,   NULL },
  { "key",     CMD_ARG_NONE,    CMD_ARG_OPTIONAL, IRC_LINE_SZ,  NULL },
};

static const cmd_arg_desc_t ad_irc_chan_del[] = {
  { "bot",     CMD_ARG_ALNUM,   CMD_ARG_REQUIRED, BOT_NAME_SZ, NULL },
  { "channel", CMD_ARG_CHANNEL, CMD_ARG_REQUIRED, IRC_CHAN_SZ,  NULL },
};

static const cmd_arg_desc_t ad_irc_chan_list[] = {
  { "bot", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, BOT_NAME_SZ, NULL },
};

// IRC color table for mIRC-compatible color codes.
static const color_table_t irc_colors = {
  .red    = IRC_RED,
  .green  = IRC_GREEN,
  .yellow = IRC_YELLOW,
  .blue   = IRC_BLUE,
  .purple = IRC_PURPLE,
  .cyan   = IRC_CYAN,
  .white  = IRC_WHITE,
  .orange = IRC_ORANGE,
  .gray   = IRC_GRAY,
  .bold   = "\002",
  .reset  = IRC_RESET,
};

// IRC method driver vtable. Identity hooks (signer + scorer +
// token-scorer) are registered with the chat plugin's identity
// registry at plugin_start via plugin_dlsym, not published through
// method_driver_t; the vtable carries protocol essentials only.
static const method_driver_t irc_driver = {
  .name          = "irc",
  .caps          = METHOD_CAP_EMOTE,
  .colors        = &irc_colors,
  .create        = irc_create,
  .destroy       = irc_destroy,
  .connect       = irc_connect,
  .disconnect    = irc_disconnect,
  .send          = irc_send,
  .send_emote    = irc_send_emote,
  .get_context   = irc_get_context,
  .list_channel  = irc_list_channel,
  .list_joined_channels = irc_list_joined_channels,
  .get_self      = irc_get_self,
};

#endif // IRC_INTERNAL

#endif // BM_IRC_H
