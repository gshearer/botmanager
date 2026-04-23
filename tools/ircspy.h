#ifndef BM_TOOLS_IRCSPY_H
#define BM_TOOLS_IRCSPY_H

// Default connection parameters (match bottest.txt test server).
#define DEFAULT_HOST     "10.68.254.254"
#define DEFAULT_PORT     6697
#define DEFAULT_NICK     "agent"
#define DEFAULT_USER     "agent"
#define DEFAULT_REAL     "BotManager IRC Observer"
#define DEFAULT_CHANNEL  "#botman"

// I/O buffer sizes.
#define BUF_SZ           4096
#define CMD_SZ           512   // RFC 2812: max 512 bytes per message

// Poll timeout (5 minutes, milliseconds).
#define POLL_TIMEOUT_MS  300000

// Max nick retry on collision.
#define NICK_RETRY_MAX   9

// Control socket defaults.
#define CTL_SOCK_PATH    "/tmp/ircspy.sock"
#define CTL_IDLE_SEC     3      // seconds of silence before end-of-response

#ifdef IRCSPY_INTERNAL

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <openssl/ssl.h>

// Configuration parsed from command-line arguments.
struct irc_cfg
{
  const char *host;
  uint16_t    port;
  const char *nick;
  const char *user;
  const char *realname;
  const char *channel;
  const char *ctl_path;
  bool        use_tls;
  bool        tls_verify;
  bool        raw_mode;
};

// Global state.
static volatile sig_atomic_t g_quit = 0;

static int       g_fd  = -1;
static SSL_CTX  *g_ctx = NULL;
static SSL      *g_ssl = NULL;

// Line buffer for partial IRC reads.
static char g_linebuf[BUF_SZ];
static int  g_lineoff = 0;

// Current channel (for bare-text sends).
static char g_channel[CMD_SZ];

// Current nick (may change on collision).
static char g_nick[CMD_SZ];
static int  g_nick_suffix = 0;

// Registration state.
static bool g_registered = false;

// Control socket state.
static int    g_ctl_listen  = -1;   // listening socket fd
static int    g_ctl_client  = -1;   // connected client fd
static time_t g_ctl_deadline = 0;   // response collection deadline (monotonic)
static bool   g_stdin_open  = true;  // false after stdin EOF
static char   g_ctl_path[256];      // socket path for cleanup

// Poll index tracker for the dynamic poll set built each iteration of
// the main loop. Slots default to -1 when the corresponding source is
// not in the poll set this iteration.
struct poll_idx
{
  int irc;
  int stdin_fd;
  int ctl_listen;
  int ctl_client;
  int nfds;
};

#endif // IRCSPY_INTERNAL

#endif // BM_TOOLS_IRCSPY_H
