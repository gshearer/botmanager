// gemini_ws_channels.h — Gemini WebSocket channel multiplexer.
//
// Layered on top of gemini_ws.c's two-session transport. Owns:
//   * the local subscriber list (one per `exchange_ws_subscribe` call),
//   * a per-(channel, native_symbol) slot table that refcounts shared
//     subscribers so N consumers watching the same feed share one
//     upstream subscription,
//   * a monotonic req_id counter (informational — Gemini Market Data
//     v2 does not use JSON-RPC req_ids on the wire; correlation
//     against `subscription_ack` envelopes goes through (channel,
//     symbol) matching instead),
//   * per-channel parsers for `l2_updates`, `trade`, `candles_1m_updates`,
//     plus the Order Events frame walker that surfaces
//     `accepted/booked/fill/cancelled/rejected/closed` as
//     EXCH_WS_USER_KIND_ORDER / EXCH_WS_USER_KIND_FILL events.
//
// On reconnect the slot table drives a full resubscribe so consumer
// callbacks never miss a beat across a flap. The OE session has no
// subscribe frame — open is acked by the server with a
// `subscription_ack` envelope, then a stream of order events.

#ifndef BM_GEMINI_WS_CHANNELS_H
#define BM_GEMINI_WS_CHANNELS_H

#include "exchange_api.h"
#include "gemini_ws.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Lifecycle. Idempotent and safe to call before the WS readers spawn.
void    gem_ws_channels_init  (void);
void    gem_ws_channels_deinit(void);

// Called from gemini_ws.c the moment a session transitions to OPEN.
//
//   GEM_WS_MD: rebuild the upstream MD subscription set from the live
//   slot table and emit one subscribe per channel covering every live
//   native symbol. Resets every slot's `gateway_holds` flag and its
//   state before rendering — UNCONDITIONALLY, including slots at
//   refcount 0 — then compacts, then emits. The reset is what makes a
//   departed slot forgettable: before OBS-42 it skipped refcount-0
//   slots, which is exactly the set that could not be forgotten any
//   other way, and the table's occupancy drifted from live
//   subscriptions towards ever-subscribed pairs until it filled.
//
//   GEM_WS_OE: no subscribe frame — Gemini's order-events stream is
//   account-implicit. The hook just logs the OPEN transition.
//
// The session lock must NOT be held by the caller — this function
// calls gem_ws_send_text internally.
void    gem_ws_channels_on_open(gem_ws_session_id_t sid);

// Reader-thread hooks for the reassembled text frames. Parse the JSON
// and fan out data frames to every matching local subscriber. Pointer
// is only valid for the call duration.
void    gem_ws_channels_dispatch_md(const char *buf, size_t len);
void    gem_ws_channels_dispatch_oe(const char *buf, size_t len);

// Implementation of the exchange-vtable ws_subscribe / ws_unsubscribe
// slots wired in gemini_exchange.c. Returns FAIL synchronously (with
// `*out_handle == NULL`) when:
//   * any input pointer is NULL or counts are zero,
//   * a requested channel has no Gemini peer (EXCH_WS_BOOK_L2 today),
//   * EXCH_WS_USER was requested but credentials are unconfigured.
// On SUCCESS the handle is non-NULL and every matching event fires
// `cb` until gem_ws_unsubscribe(handle).
//
// The handle is this multiplexer's own subscriber node, and it lives and
// dies with this mapping: gem_ws_channels_deinit frees every node still
// outstanding. The abstraction wraps it and never forwards one issued by
// an earlier registration, so nothing stale reaches here (OBS-19).
//
// OBS-51: SUCCESS means the handle covers EVERY (channel, symbol) pair
// asked for. A call that cannot seat all of them seats none, gives back
// what it took and returns FAIL — partial coverage is unreportable here
// and the consumer cannot repair it, so refusing is the honest answer.
bool    gem_ws_subscribe(const exchange_ws_channel_t *channels,
            uint32_t n_channels, const char *const *product_ids,
            uint32_t n_products, exchange_ws_event_cb_t cb, void *user,
            void **out_handle);

void    gem_ws_unsubscribe(void *driver_sub);

#endif // BM_GEMINI_WS_CHANNELS_H
