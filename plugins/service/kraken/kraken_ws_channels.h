// kraken_ws_channels.h — Kraken WebSocket v2 channel multiplexer.
//
// Layered on top of kraken_ws.c's single-session transport. Owns:
//   * the local subscriber list (one per `exchange_ws_subscribe` caller),
//   * a (channel, symbol) slot table that refcounts shared subscribers,
//   * the JSON-RPC req_id correlator that pairs subscribe acks with the
//     slot they originated from,
//   * the per-channel parser that turns one inbound `update` frame into
//     fanned-out `exchange_ws_event_t` events.
//
// On reconnect the slot table drives a full resubscribe so consumer
// callbacks never miss a beat across a flap.

#ifndef BM_KRAKEN_WS_CHANNELS_H
#define BM_KRAKEN_WS_CHANNELS_H

#include "exchange_api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Lifecycle. Idempotent and safe to call before the WS reader has spawned.
void    kr_ws_channels_init  (void);
void    kr_ws_channels_deinit(void);

// Called from kraken_ws.c the moment the session transitions to OPEN.
// Rebuilds the upstream subscription set from the live slot table and
// emits one subscribe per channel covering all live products. Resets
// every slot's `sent_upstream` flag before rendering so a session that
// had previously received subscribe acks gets a fresh resubscribe after
// a flap. The session lock must NOT be held by the caller — this function
// calls kr_ws_send_text internally.
void    kr_ws_channels_on_open(void);

// Reader-thread hook for the reassembled text frame. Parses the JSON,
// routes acks to the correlator, fans out data frames to every matching
// local subscriber. Pointer is only valid for the duration of the call.
void    kr_ws_channels_dispatch(const char *buf, size_t len);

// Implementation of the exchange-vtable ws_subscribe / ws_unsubscribe
// slots wired in kraken_exchange.c. Returns FAIL synchronously (with
// `*out_handle == NULL`) when:
//   * any input pointer is NULL or counts are zero,
//   * a requested channel has no Kraken peer,
//   * private channels were requested but credentials are unconfigured.
// On SUCCESS the handle is non-NULL and every matching event fires `cb`
// until kr_ws_unsubscribe(handle).
bool    kr_ws_subscribe(const exchange_ws_channel_t *channels,
            uint32_t n_channels, const char *const *product_ids,
            uint32_t n_products, exchange_ws_event_cb_t cb, void *user,
            exchange_ws_sub_t **out_handle);

void    kr_ws_unsubscribe(exchange_ws_sub_t *handle);

#endif // BM_KRAKEN_WS_CHANNELS_H
