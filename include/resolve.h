#ifndef BM_RESOLVE_H
#define BM_RESOLVE_H

#include <stdbool.h>
#include <stdint.h>

#include <arpa/inet.h>
#include <sys/socket.h>

// -----------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------

#define RESOLVE_NAME_SZ  256
#define RESOLVE_TXT_SZ   512

// -----------------------------------------------------------------------
// Types
// -----------------------------------------------------------------------

// DNS record types supported by the resolver.
typedef enum
{
  RESOLVE_A,          // IPv4 address
  RESOLVE_AAAA,       // IPv6 address
  RESOLVE_MX,         // mail exchange
  RESOLVE_TXT,        // text record
  RESOLVE_CNAME,      // canonical name
  RESOLVE_NS,         // name server
  RESOLVE_PTR,        // reverse lookup
  RESOLVE_SRV,        // service locator
  RESOLVE_SOA         // start of authority
} resolve_type_t;

// Individual DNS record. Tagged union keyed by type.
typedef struct
{
  resolve_type_t type;
  uint32_t       ttl;

  union
  {
    struct
    {
      char                    addr[INET_ADDRSTRLEN];
      struct sockaddr_storage sa;
      socklen_t               sa_len;
    } a;

    struct
    {
      char                    addr[INET6_ADDRSTRLEN];
      struct sockaddr_storage sa;
      socklen_t               sa_len;
    } aaaa;

    struct
    {
      uint16_t priority;
      char     exchange[RESOLVE_NAME_SZ];
    } mx;

    struct { char text[RESOLVE_TXT_SZ]; } txt;
    struct { char name[RESOLVE_NAME_SZ]; } cname;
    struct { char name[RESOLVE_NAME_SZ]; } ns;
    struct { char name[RESOLVE_NAME_SZ]; } ptr;

    struct
    {
      uint16_t priority;
      uint16_t weight;
      uint16_t port;
      char     target[RESOLVE_NAME_SZ];
    } srv;

    struct
    {
      char     mname[RESOLVE_NAME_SZ];
      char     rname[RESOLVE_NAME_SZ];
      uint32_t serial;
      uint32_t refresh;
      uint32_t retry;
      uint32_t expire;
      uint32_t minimum;
    } soa;
  };
} resolve_record_t;

// Result delivered to the completion callback. The records array and the
// result itself are valid only for the duration of the callback.
typedef struct
{
  char              name[RESOLVE_NAME_SZ];  // queried hostname
  resolve_type_t    qtype;                  // queried record type
  resolve_record_t *records;                // result array
  uint32_t          count;                  // number of records
  int               status;                 // 0 = success
  const char       *error;                  // human-readable (NULL on success)
  void             *user_data;              // caller's opaque pointer
} resolve_result_t;

// Completion callback. Invoked on a worker thread. Result is valid only
// for the duration of the callback; callers must copy what they need.
typedef void (*resolve_cb_t)(const resolve_result_t *result);

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

// Submit an async DNS lookup. Thread-safe.
// returns: SUCCESS or FAIL (subsystem not ready, bad params, at capacity)
// name: hostname to resolve
// qtype: record type to query
// cb: completion callback (invoked on a worker thread)
// user_data: opaque data passed through to callback
bool resolve_lookup(const char *name, resolve_type_t qtype,
    resolve_cb_t cb, void *user_data);

// returns: human-readable name of a record type ("A", "AAAA", "MX", ...)
// type: record type enum value
const char *resolve_type_name(resolve_type_t type);

// Parse a record type string into an enum value (case-insensitive).
// returns: SUCCESS or FAIL (unrecognized type)
// str: type name (e.g., "a", "MX", "srv")
// out: destination for parsed type
bool resolve_type_parse(const char *str, resolve_type_t *out);

// Number of distinct resolve_type_t values for per-type counters.
#define RESOLVE_TYPE_COUNT  9

// Resolver subsystem statistics.
typedef struct
{
  uint64_t queries;                       // lifetime total lookups
  uint64_t failures;                      // lifetime lookup failures
  uint64_t by_type[RESOLVE_TYPE_COUNT];   // queries per record type (indexed by resolve_type_t)
} resolve_stats_t;

// Get resolver subsystem statistics (thread-safe snapshot).
// out: destination for the snapshot
void resolve_get_stats(resolve_stats_t *out);

// Initialize the resolver subsystem. Call after task_init()/pool_init().
void resolve_init(void);

// Register resolver KV configuration. Call after kv_load().
void resolve_register_config(void);

// Shut down the resolver. Drains the freelist.
void resolve_exit(void);

// Register the "resolve" user command. Call after cmd_init().
void resolve_register_commands(void);

// -----------------------------------------------------------------------
// Internal (only available to resolve.c)
// -----------------------------------------------------------------------

#ifdef RESOLVE_INTERNAL

#include "cmd.h"
#include "colors.h"
#include "common.h"
#include "clam.h"
#include "kv.h"
#include "mem.h"
#include "method.h"
#include "task.h"
#include "validate.h"

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <resolv.h>
#include <arpa/nameser.h>

// Maximum records returned from a single query.
#define RESOLVE_MAX_RECORDS 64

// DNS wire-format answer buffer size.
#define RESOLVE_ANSWER_SZ   4096

// Pending resolve request (freelist-managed).
typedef struct resolve_request
{
  char                    name[RESOLVE_NAME_SZ];
  resolve_type_t          qtype;
  resolve_cb_t            cb;
  void                   *user_data;
  time_t                  submitted;
  struct resolve_request *next;
} resolve_request_t;

// Runtime configuration.
typedef struct
{
  uint32_t timeout;       // per-query timeout in seconds
  uint32_t max_pending;   // max concurrent resolve tasks
} resolve_cfg_t;

// Module state.
static bool                resolve_ready    = false;
static resolve_request_t  *resolve_req_free = NULL;
static pthread_mutex_t     resolve_req_lock;
static uint32_t            resolve_pending  = 0;
static resolve_cfg_t       resolve_cfg;

// Lifetime counters for statistics (atomic, no lock needed).
static uint64_t            resolve_stat_queries  = 0;
static uint64_t            resolve_stat_failures = 0;
static uint64_t            resolve_stat_by_type[RESOLVE_TYPE_COUNT] = {0};

// Maximum records accumulated across parallel queries for the
// !resolve user command.
#define RESOLVE_CMD_MAX_RECORDS  128

// Number of record types queried for hostname lookups.
#define RESOLVE_CMD_HOST_QTYPES  7

// Reply line buffer size.
#define RESOLVE_CMD_REPLY_SZ     512

// Per-command request context. Tracks outstanding parallel queries
// and accumulates records from all callbacks.
typedef struct resolve_cmd_request
{
  cmd_ctx_t           ctx;                                  // saved cmd context
  method_msg_t        msg;                                  // backing for ctx.msg
  char                target[RESOLVE_NAME_SZ];              // original input
  bool                is_reverse;                           // PTR lookup for IP
  bool                verbose;                              // show all record types
  uint8_t             pending;                              // queries outstanding
  pthread_mutex_t     mu;
  resolve_record_t    records[RESOLVE_CMD_MAX_RECORDS];     // accumulated results
  uint32_t            count;                                // records collected
  uint32_t            errors;                               // queries that failed

  struct resolve_cmd_request *next;                         // freelist linkage
} resolve_cmd_request_t;

// Command request freelist.
static resolve_cmd_request_t *resolve_cmd_free = NULL;
static pthread_mutex_t        resolve_cmd_free_mu;

// Validates a resolve target (hostname or IP address).
static bool resolve_validate_target(const char *str);

#endif // RESOLVE_INTERNAL

#endif // BM_RESOLVE_H
