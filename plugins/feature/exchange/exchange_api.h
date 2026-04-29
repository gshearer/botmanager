#ifndef BM_EXCHANGE_API_H
#define BM_EXCHANGE_API_H

// Public mechanism API for the feature_exchange plugin.
//
// `exchange_request()` is the single entry point for routing a REST
// operation to a registered exchange-protocol plugin (coinbase, kraken,
// …). The abstraction owns:
//
//   * priority queueing (P0 transactional, P50 backfill, P254 user
//     download — see EXCHANGE_PRIO_* constants),
//   * a per-exchange token bucket sized from the protocol plugin's
//     advertised rps (minus a small headroom),
//   * reserved-slot policy so high-priority traffic cannot be drowned
//     by lower-priority backfill (default reserved[0]=1),
//   * 429 / 5xx retry with exponential backoff (250 → 4000 ms cap, 5
//     attempts).
//
// Exchange-protocol plugins implement `exchange_protocol_vtable_t` and
// self-register at init via `exchange_register()`. Consumers (whenmoon,
// future strategy engine) only ever call the four public functions
// declared below.
//
// Shim shape mirrors plugins/service/coinbase/coinbase_api.h: per-
// symbol atomic cache guard, union to launder void*↔function-pointer
// conversion, FATAL + abort on a dlsym miss (which implies a broken
// plugin-dependency graph).
//
// The exchange plugin's own translation units define EXCHANGE_INTERNAL
// before including this header so the static-inline shims below are
// skipped (they would collide with the real definitions).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Priority tiers carried on every request. Lower numeric value = higher
// priority. The token bucket reserves slots for P0 traffic so a flood of
// P254 backfill cannot starve a transactional buy/sell. Reservation
// counts are KV-tunable per exchange.
#define EXCHANGE_PRIO_TRANSACTIONAL    0
#define EXCHANGE_PRIO_MARKET_BACKFILL  50
#define EXCHANGE_PRIO_USER_DOWNLOAD  254

// Op-kind classifier carried into the protocol plugin's `build_request`
// vtable hook. The protocol plugin uses it to pick public vs. signed
// HTTP and the matching curl method.
typedef enum
{
  EXCHANGE_OP_REST_GET,
  EXCHANGE_OP_REST_POST,
  EXCHANGE_OP_REST_DELETE,
  EXCHANGE_OP_PRIVATE_REST_GET,
  EXCHANGE_OP_PRIVATE_REST_POST,
  EXCHANGE_OP_PRIVATE_REST_DELETE
} exchange_op_kind_t;

// Response delivered to the caller after retry / classification.
//
//   http_status — HTTP status code on transport success (>= 0); 0 when
//                 the request never reached the server (transport error
//                 or hard pre-flight failure).
//   body        — response body bytes (UTF-8 in practice); pointer is
//                 only valid for the duration of the callback.
//   body_len    — bytes addressable through `body`. Zero when the
//                 protocol plugin returned no body or on hard error.
//   err         — non-NULL human-readable description on failure (final
//                 retry exhausted, hard pre-flight refusal, …); NULL
//                 on success.
//   user        — verbatim user pointer the caller passed to
//                 exchange_request().
//
// Invoked on the curl worker thread that completed the underlying
// transport. Consumers must not block.
typedef void (*exchange_response_cb_t)(int http_status,
    const char *body, size_t body_len,
    const char *err, void *user);

// Per-exchange protocol vtable.
//
// `build_request` prepares an opaque protocol-specific request handle
// from the (kind, path, body) triple. The abstraction owns the handle
// for the lifetime of the request and frees it via `free_request` after
// completion (success, exhausted retry, or hard transport refusal).
//
// `submit` hands the prepared handle to the underlying transport. The
// protocol plugin's curl completion routes back into the abstraction
// via the `cb`/`user` pair the abstraction supplies — never via the
// caller's typed callback. The abstraction then classifies the status,
// applies retry policy, and finally invokes the caller's
// exchange_response_cb_t with the raw body.
//
// `advertised_rps` / `advertised_burst` are static knobs read once at
// `exchange_register()` time. The abstraction sizes its token bucket
// from these (with a small headroom subtracted from `rps`).
typedef struct
{
  bool (*build_request)(exchange_op_kind_t kind,
      const char *path, const char *body_json,
      void **out_handle);

  bool (*submit)(void *handle, exchange_response_cb_t cb, void *user);

  void (*free_request)(void *handle);

  uint32_t advertised_rps;
  uint32_t advertised_burst;
} exchange_protocol_vtable_t;

// ------------------------------------------------------------------
// Real function declarations — visible only inside the exchange
// plugin (where EXCHANGE_INTERNAL is defined). External consumers go
// through the static-inline dlsym shims defined further down. Pattern
// matches plugins/service/coinbase/coinbase_api.h.
// ------------------------------------------------------------------

#ifdef EXCHANGE_INTERNAL

// Submit a request to the named exchange. Returns FAIL when the
// exchange is unknown, the abstraction is shutting down, or the
// internal queue could not accept the request; the callback is NOT
// invoked on FAIL. Returns SUCCESS once the request has been queued —
// the callback fires asynchronously on the curl worker thread.
//
// `path` and `body_json` are copied into the abstraction's internal
// buffers; callers may free both as soon as the call returns. `body_json`
// may be NULL for GET / DELETE.
bool exchange_request(const char *exchange, uint8_t prio,
    exchange_op_kind_t kind, const char *path, const char *body_json,
    exchange_response_cb_t cb, void *user);

// Register an exchange-protocol implementation. Called from the
// protocol plugin's `init()` (e.g. coinbase_init) once. The vtable
// pointer must outlive the abstraction (typical: const file-scope).
// Returns FAIL when `name` is empty or already registered.
bool exchange_register(const char *name,
    const exchange_protocol_vtable_t *vt);

// Tear down a registered exchange. Pending requests for `name` are
// surfaced as failures to their callbacks. Safe to call from the
// protocol plugin's `deinit()`.
void exchange_unregister(const char *name);

#endif // EXCHANGE_INTERNAL

#ifndef EXCHANGE_INTERNAL

#include "clam.h"
#include "plugin.h"

#include <stdlib.h>  // abort

static inline bool
exchange_request(const char *exchange, uint8_t prio,
    exchange_op_kind_t kind, const char *path, const char *body_json,
    exchange_response_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, uint8_t, exchange_op_kind_t,
      const char *, const char *, exchange_response_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("exchange", "exchange_request");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "exchange",
          "dlsym failed: exchange_request");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(exchange, prio, kind, path, body_json, cb, user));
}

static inline bool
exchange_register(const char *name, const exchange_protocol_vtable_t *vt)
{
  typedef bool (*fn_t)(const char *, const exchange_protocol_vtable_t *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("exchange", "exchange_register");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "exchange",
          "dlsym failed: exchange_register");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(name, vt));
}

static inline void
exchange_unregister(const char *name)
{
  typedef void (*fn_t)(const char *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("exchange", "exchange_unregister");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "exchange",
          "dlsym failed: exchange_unregister");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  fn(name);
}

#endif // !EXCHANGE_INTERNAL

#endif // BM_EXCHANGE_API_H
