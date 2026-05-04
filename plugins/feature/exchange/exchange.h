// exchange.h — feature_exchange plugin internal header.
//
// Public mechanism in exchange_api.h. This header gates everything
// behind EXCHANGE_INTERNAL so the plugin's own translation units pull
// the real symbol declarations and skip the dlsym shim block in
// exchange_api.h.

#ifndef BM_EXCHANGE_H
#define BM_EXCHANGE_H

#ifdef EXCHANGE_INTERNAL

#include "clam.h"
#include "common.h"
#include "kv.h"
#include "alloc.h"
#include "plugin.h"
#include "task.h"

#include "exchange_api.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define EXCHANGE_CTX            "exchange"

// Plugin-wide tunables. The numeric ladder for retry/backoff matches
// the spec in /home/doc/.claude/plans/wm-rename-and-feature.md (EX-1).
#define EXCHANGE_RETRY_MAX_ATTEMPTS  5
#define EXCHANGE_RETRY_BASE_MS     250
#define EXCHANGE_RETRY_CAP_MS     4000

// Bucket headroom: subtract this from advertised_rps so non-downloader
// traffic outside the abstraction (ad-hoc /accounts polls, ws keepalive,
// …) cannot collide with our peak. 2 rps headroom on Coinbase's 10 cap
// matches the legacy whenmoon default of 8 rps.
#define EXCHANGE_RPS_HEADROOM        2

// Stall-pump delay: when exchange_dispatch leaves a non-empty queue
// because the bucket is dry, schedule a deferred re-dispatch this many
// ms in the future so queued requests drain even if no completion fires
// (the chain otherwise stays parked until a new submit or completion
// triggers exchange_dispatch — which may be ~minutes when the
// downloader saturated the burst in one kick round and is then waiting
// on the supervisor's stall release). task_add_deferred clamps to a
// 1 s minimum, so anything <= 1000 here is equivalent.
#define EXCHANGE_PUMP_DELAY_MS    1000

// Internal forward decls.
typedef struct exchange_req       exchange_req_t;
typedef struct exchange_limiter   exchange_limiter_t;
typedef struct exchange           exchange_t;

// Per-priority reservation: `reserved[p]` is the number of bucket tokens
// kept for priorities <= p. Lower-priority requests may take a token
// only when `tokens >= 1.0 + sum(reserved[p'] for p' < this->prio)`.
//
// Two named tiers are honoured today: 0 (transactional) and 50 (market
// backfill). Anything > 50 falls into "best effort" (no reserved
// budget). Future tiers can extend this without an ABI break — the
// limiter just sums whatever entries exist.
#define EXCHANGE_PRIO_TIERS  3
#define EXCHANGE_TIER_TXN    0
#define EXCHANGE_TIER_BACK   1
#define EXCHANGE_TIER_USER   2

struct exchange_limiter
{
  // Token bucket. Refilled lazily on every take/peek using the
  // monotonic-clock delta since `last_refill`.
  double                tokens;
  double                tokens_cap;
  double                tokens_per_sec;
  struct timespec       last_refill;

  // 429 deficit: positive value means the bucket must "pay back" extra
  // tokens before issuing new ones. Bumped by the retry classifier on
  // 429 to slow the next dispatch beyond the natural refill rate.
  double                deficit;

  // Reserved-slot policy. Indexed by EXCHANGE_TIER_* (a small mapping
  // applied to the request's 8-bit priority).
  uint32_t              reserved[EXCHANGE_PRIO_TIERS];

  // In-flight accounting (bookkeeping; consumers may want to refuse
  // unload until this drains to 0 in a future chunk).
  uint32_t              in_flight;
};

// Request travelling through the abstraction. Allocated by
// exchange_request(), owned by the abstraction until the user callback
// fires (success or terminal failure).
struct exchange_req
{
  exchange_t            *exch;
  uint8_t                prio;
  exchange_op_kind_t     kind;

  // Caller-owned strings copied at submit time. Freed with the request.
  char                  *path;
  char                  *body;
  size_t                 body_len;

  // User callback + user pointer. `user_cb` is NULL once delivery has
  // happened (defence in depth — should never re-fire).
  exchange_response_cb_t user_cb;
  void                  *user;

  // Retry state.
  uint32_t               attempts;          // 0 = first try

  // Protocol handle — set by vtable.build_request before vtable.submit.
  void                  *proto_handle;

  // Queue link (priority queue: ascending by prio).
  exchange_req_t        *next;
};

// Per-exchange registration entry.
struct exchange
{
  char                              name[32];
  const exchange_protocol_vtable_t *vt;

  pthread_mutex_t                   lock;
  exchange_limiter_t                limiter;

  // Priority queue head (ascending prio; head = highest priority).
  exchange_req_t                   *q_head;
  uint32_t                          q_count;

  // Plugin shutdown gate. `dead = true` causes all future submits to
  // fail and pending requests to fan out as failures on
  // exchange_unregister().
  bool                              dead;

  // Stall-pump task handle. Non-zero when a deferred re-dispatch is
  // armed for the moment the bucket refills. Set under e->lock; cleared
  // under e->lock from the pump callback (or by exchange_unregister
  // during teardown). Read/written only under e->lock.
  task_handle_t                     pump_handle;

  exchange_t                       *next;   // registry list
};

// exchange_registry.c
void          exchange_registry_init(void);
void          exchange_registry_destroy(void);
exchange_t   *exchange_find(const char *name);

// Append-or-update used by exchange_register. Returns FAIL on dup.
bool          exchange_registry_add(const char *name,
                  const exchange_protocol_vtable_t *vt);

// Iterate every registered exchange under the registry lock. Callback
// runs WITH the registry lock held — must not call back into the
// abstraction. Used by the deinit fan-out path only.
typedef void (*exchange_registry_iter_cb_t)(exchange_t *e, void *user);
void          exchange_registry_iterate(exchange_registry_iter_cb_t cb,
                  void *user);

// exchange_limiter.c
void          exchange_limiter_init(exchange_limiter_t *lim,
                  uint32_t advertised_rps, uint32_t advertised_burst);

// Caller must hold exchange->lock.
void          exchange_limiter_refill_locked(exchange_limiter_t *lim);
bool          exchange_limiter_take_locked(exchange_limiter_t *lim,
                  uint8_t prio);
void          exchange_limiter_refund_locked(exchange_limiter_t *lim);
void          exchange_limiter_penalty_locked(exchange_limiter_t *lim,
                  double extra_tokens);

// Map an 8-bit priority into the EXCHANGE_TIER_* index.
uint32_t      exchange_prio_tier(uint8_t prio);

// exchange_retry.c
//
// Outcome of a completed transport. The abstraction's internal
// completion callback feeds (status, err) here to decide whether to
// surface, retry, or fail terminally.
typedef enum
{
  EXCHANGE_OUTCOME_OK,         // 2xx — surface body to caller
  EXCHANGE_OUTCOME_RETRY,      // 429 / 5xx — try again with backoff
  EXCHANGE_OUTCOME_FAIL        // 4xx other / hard transport — surface err
} exchange_outcome_t;

exchange_outcome_t exchange_classify_status(int http_status,
    bool transport_err);

// 0-indexed attempt -> backoff in ms (clamped to EXCHANGE_RETRY_CAP_MS).
uint32_t      exchange_backoff_ms(uint32_t attempt);

// Build a generic human-readable error string for the given status. The
// returned pointer is a string literal or `buf` (caller-owned scratch).
const char   *exchange_describe_status(int http_status,
                  bool transport_err, const char *upstream_err,
                  char *buf, size_t cap);

// exchange_request.c
//
// Single dispatch primitive: walk `exch`'s priority queue, take a token
// for the highest-eligible request, hand off to vtable.submit. Re-enters
// itself on completion. Safe from any thread.
void          exchange_dispatch(exchange_t *exch);

// Allocator/free for the request struct. Caller owns the path/body
// pointers transferred in (deep-copied here from caller's strings).
exchange_req_t *exchange_req_new(exchange_t *exch, uint8_t prio,
    exchange_op_kind_t kind, const char *path, const char *body,
    exchange_response_cb_t cb, void *user);
void          exchange_req_free(exchange_req_t *r);

// Surface a terminal failure to the user callback. Releases the
// protocol handle (when set) and frees the request.
void          exchange_req_fail(exchange_req_t *r, int http_status,
                  const char *err);

// Surface success. Releases the protocol handle and frees the request.
void          exchange_req_succeed(exchange_req_t *r, int http_status,
                  const char *body, size_t body_len);

#endif // EXCHANGE_INTERNAL
#endif // BM_EXCHANGE_H
