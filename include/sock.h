#ifndef BM_SOCK_H
#define BM_SOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct sock_session sock_session_t;

typedef enum
{
  SOCK_TCP,
  SOCK_UDP,
  SOCK_ICMP,
  SOCK_UNIX
} sock_type_t;

typedef enum
{
  SOCK_EVENT_CONNECTED,       // connection established
  SOCK_EVENT_DATA,            // data available
  SOCK_EVENT_DISCONNECT,      // peer closed or connection lost
  SOCK_EVENT_ERROR            // error condition
} sock_event_type_t;

// Valid for the duration of the callback only — consumers must copy
// any data they need.
typedef struct
{
  sock_event_type_t  type;
  sock_session_t    *session;
  const uint8_t     *data;    // valid for SOCK_EVENT_DATA only
  size_t             data_len;
  int                err;     // errno for SOCK_EVENT_ERROR
} sock_event_t;

// Invoked on the epoll worker thread — callbacks must be fast and
// non-blocking.
typedef void (*sock_cb_t)(const sock_event_t *event, void *user_data);

// Does not connect yet. name is a human-readable label for logging
// (max 39 chars).
sock_session_t *sock_create(const char *name, sock_type_t type,
    sock_cb_t cb, void *user_data);

// Must be called after sock_create() and before sock_connect(). The
// TLS handshake is performed automatically after the TCP connection
// completes. If verify is true, verify the server certificate against
// the system CA store.
bool sock_set_tls(sock_session_t *session, bool verify);

// DNS resolution and connect happen asynchronously. The consumer is
// notified via SOCK_EVENT_CONNECTED or SOCK_EVENT_ERROR.
// host is NULL for SOCK_UNIX; path is NULL for TCP/UDP/ICMP.
bool sock_connect(sock_session_t *session, const char *host, uint16_t port,
    const char *path);

// Thread-safe; may be called from any thread. Data is copied
// internally and sent asynchronously.
bool sock_send(sock_session_t *session, const void *buf, size_t len);

// Initiate graceful close. SOCK_EVENT_DISCONNECT is delivered when
// teardown completes. Do not call sock_send() after this.
void sock_close(sock_session_t *session);

// Must not be called from within a callback for the same session. If
// the session is still connected, it is force-closed first.
void sock_destroy(sock_session_t *session);

// Returns -1 if not connected.
int sock_get_fd(const sock_session_t *session);

typedef struct
{
  uint32_t sessions;          // total active sessions
  uint32_t connected;         // sessions in connected state
  uint64_t bytes_in;          // total bytes received
  uint64_t bytes_out;         // total bytes sent
  uint8_t  workers;           // active epoll worker threads
  uint64_t connections;       // lifetime total connections established
  uint64_t disconnects;       // lifetime disconnections
  uint32_t tls_sessions;      // active TLS-wrapped sessions
} sock_stats_t;

void sock_get_stats(sock_stats_t *out);

const char *sock_type_name(sock_type_t type);
const char *sock_event_name(sock_event_type_t type);

// Must be called after pool_init().
void sock_init(void);

// Must be called after kv_init() and plugin_init_all() (so KV store
// is populated).
void sock_register_config(void);

// Closes all active sessions and frees resources. The epoll worker
// thread must already be joined (via pool_exit) before calling this.
void sock_exit(void);

// state is cast from sock_state_t.
const char *sock_state_name(int state);

// Invoked once per active session while the session list lock is
// held — must be fast.
typedef void (*sock_iter_cb_t)(uint32_t id, sock_type_t type, int state,
    const char *remote, uint64_t bytes_in, uint64_t bytes_out,
    bool tls, time_t connected_at, void *data);

void sock_iterate(sock_iter_cb_t cb, void *data);

#ifdef SOCK_INTERNAL

#include "common.h"
#include "clam.h"
#include "kv.h"
#include "alloc.h"
#include "pool.h"
#include "resolve.h"
#include "task.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <netinet/tcp.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#define SOCK_DEF_CONNECT_TIMEOUT   10
#define SOCK_DEF_READ_BUF_SZ      4096
#define SOCK_DEF_SEND_QUEUE_MAX    65536
#define SOCK_DEF_IDLE_TIMEOUT      0
#define SOCK_DEF_KEEPALIVE         0
#define SOCK_DEF_MAX_SESSIONS      256
#define SOCK_DEF_EPOLL_MAX_EVENTS  64
#define SOCK_DEF_EPOLL_TIMEOUT     500
#define SOCK_DEF_EPOLL_WORKERS     1

#define SOCK_NAME_SZ       40
#define SOCK_HOST_SZ       256
#define SOCK_PATH_SZ       108   // sun_path max
#define SOCK_MAX_WORKERS   16

typedef enum
{
  SOCK_STATE_CREATED,         // created, not yet connecting
  SOCK_STATE_RESOLVING,       // DNS resolution in progress
  SOCK_STATE_CONNECTING,      // non-blocking connect in progress
  SOCK_STATE_CONNECTED,       // live, registered with epoll
  SOCK_STATE_TLS_HANDSHAKE,   // TLS handshake in progress
  SOCK_STATE_CLOSING,         // graceful close initiated
  SOCK_STATE_CLOSED           // fd closed, awaiting destroy
} sock_state_t;

typedef struct sock_sendbuf
{
  uint8_t             *data;
  size_t               len;
  size_t               offset;    // bytes already sent
  struct sock_sendbuf *next;
} sock_sendbuf_t;

struct sock_session
{
  char                name[SOCK_NAME_SZ];
  sock_type_t         type;
  sock_state_t        state;
  int                 fd;

  sock_cb_t           cb;
  void               *cb_data;

  // Connect parameters (stored for async resolution).
  char                host[SOCK_HOST_SZ];
  uint16_t            port;
  char                path[SOCK_PATH_SZ];

  // Read buffer (owned by the service).
  uint8_t            *read_buf;
  uint32_t            read_buf_sz;

  // Send queue (protected by send_lock).
  pthread_mutex_t     send_lock;
  sock_sendbuf_t     *send_head;
  sock_sendbuf_t     *send_tail;
  uint32_t            send_queued;    // bytes currently queued
  bool                epollout_armed;

  time_t              connect_started;
  time_t              connected_at;
  time_t              last_activity;

  uint64_t            bytes_in;
  uint64_t            bytes_out;

  // For future multi-worker distribution.
  uint8_t             worker_id;

  SSL_CTX            *tls_ctx;
  SSL                *tls;
  bool                tls_enabled;
  bool                tls_verify;

  struct sock_session *next;
};

typedef struct
{
  uint8_t   id;
  int       epoll_fd;
  int       wake_fd;          // eventfd to wake epoll_wait
  task_t   *task;             // persist task handle
} sock_worker_t;

typedef struct
{
  uint32_t connect_timeout;
  uint32_t read_buf_sz;
  uint32_t send_queue_max;
  uint32_t idle_timeout;
  uint32_t keepalive;
  uint32_t max_sessions;
  uint32_t epoll_max_events;
  uint32_t epoll_timeout;
  uint32_t epoll_workers;
} sock_cfg_t;

static sock_session_t  *sock_list         = NULL;
static pthread_mutex_t  sock_mutex;
static uint32_t         sock_count        = 0;
static sock_worker_t    sock_workers[SOCK_MAX_WORKERS];
static uint8_t          sock_worker_count = 0;
static uint8_t          sock_next_worker  = 0;   // round-robin counter
static bool             sock_ready        = false;
static sock_cfg_t       sock_cfg;

static uint64_t         sock_total_in     = 0;
static uint64_t         sock_total_out    = 0;
static uint64_t         sock_total_conn   = 0;
static uint64_t         sock_total_disc   = 0;

static sock_sendbuf_t  *sock_sbuf_free    = NULL;
static pthread_mutex_t  sock_sbuf_mutex;

static void sock_epoll_task(task_t *t);
static void sock_unix_connect_task(task_t *t);
static void sock_resolve_done(const resolve_result_t *result);
static void sock_session_close_fd(sock_session_t *s);
static void sock_deliver(sock_session_t *s, sock_event_type_t type,
    const uint8_t *data, size_t len, int err);
static void sock_wake_worker(sock_session_t *s);
static void sock_tls_cleanup(sock_session_t *s);
static bool sock_tls_handshake(sock_session_t *s);

#endif // SOCK_INTERNAL

#endif // BM_SOCK_H
