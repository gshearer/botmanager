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
#include "mem.h"
#include "plugin.h"
#include "pool.h"
#include "task.h"
#include "version.h"

#include <ctype.h>
#include <stdarg.h>
#include "validate.h"

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

// Instance-level KV schema: bare suffixes cloned into
// "bot.<botname>.irc.<suffix>" when a bot binds to IRC.
static const plugin_kv_entry_t irc_inst_kv_schema[] = {
  { "network",   KV_STR,    ""           },
  { "nick",      KV_STR,    ""           },
  { "nick2",     KV_STR,    ""           },
  { "nick3",     KV_STR,    ""           },
  { "user",      KV_STR,    ""           },
  { "realname",  KV_STR,    "BotManager" },
  { "prefix",    KV_STR,    "!"          },
};

// Per-channel KV schema: suffixes appended to
// "bot.<botname>.irc.chan.<channel>." when a channel is added.
static const plugin_kv_entry_t irc_chan_kv_schema[] = {
  { "autojoin",     KV_UINT8,  "1" },
  { "key",          KV_STR,    ""  },
  { "announce",     KV_UINT8,  "0" },
  { "announcetext", KV_STR,    ""  },
};

#define IRC_CHAN_KV_COUNT \
    (sizeof(irc_chan_kv_schema) / sizeof(irc_chan_kv_schema[0]))

// Per-server KV schema: suffixes appended to
// "irc.net.<network>.<server>." when a server is added.
static const plugin_kv_entry_t irc_srv_kv_schema[] = {
  { "address",    KV_STR,    ""     },
  { "port",       KV_UINT16, "6667" },
  { "priority",   KV_UINT16, "100"  },
  { "tls",        KV_UINT8,  "0"    },
  { "tls_verify", KV_UINT8,  "1"    },
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

// Shared validation (defined in irc.c, used by arg descriptors).
bool irc_valid_name(const char *name);
void irc_address_to_key(const char *address, char *out, size_t out_sz);

// Forward declarations for driver vtable.
static void *irc_create(const char *inst_name);
static void irc_destroy(void *handle);
static bool irc_connect(void *handle);
static void irc_disconnect(void *handle);
static bool irc_send(void *handle, const char *target, const char *text);
static bool irc_get_context(void *handle, const char *sender,
    char *ctx, size_t ctx_sz);

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

// IRC method driver vtable.
static const method_driver_t irc_driver = {
  .name        = "irc",
  .colors      = &irc_colors,
  .create      = irc_create,
  .destroy     = irc_destroy,
  .connect     = irc_connect,
  .disconnect  = irc_disconnect,
  .send        = irc_send,
  .get_context = irc_get_context,
};

#endif // IRC_INTERNAL

#endif // BM_IRC_H
