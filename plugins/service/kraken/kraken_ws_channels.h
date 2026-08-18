// kraken_ws_channels.h — Kraken WebSocket v2 channel multiplexer.
//
// Layered on top of kraken_ws.c's two-session transport, and the only
// place that decides which of the two a channel belongs on. Owns:
//   * the local subscriber list (one per `exchange_ws_subscribe` caller),
//   * a (channel, symbol) slot table that refcounts shared subscribers,
//   * the JSON-RPC req_id correlator that pairs subscribe acks with the
//     slot they originated from,
//   * the per-channel parser that turns one inbound `update` frame into
//     fanned-out `exchange_ws_event_t` events.
//
// On reconnect the slot table drives a full resubscribe so consumer
// callbacks never miss a beat across a flap — scoped to the session that
// flapped, since the other gateway forgot nothing.

#ifndef BM_KRAKEN_WS_CHANNELS_H
#define BM_KRAKEN_WS_CHANNELS_H

#include "exchange_api.h"
#include "kraken_ws.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Lifecycle. Idempotent and safe to call before the WS reader has spawned.
void    kr_ws_channels_init  (void);
void    kr_ws_channels_deinit(void);

// Called from kraken_ws.c the moment a session transitions to OPEN.
// Rebuilds that session's upstream subscription set from the live slot
// table and emits one subscribe per channel covering all live products.
// Resets that session's slots before rendering — nothing it sent can
// still be answered — so a session that had previously received
// subscribe acks gets a fresh resubscribe after a flap, and reaps the
// ones whose last consumer left while a frame was in flight. Slots
// belonging to the OTHER session are left untouched. The session lock
// must NOT be held by the caller — this function calls kr_ws_send_text
// internally.
void    kr_ws_channels_on_open(kr_ws_session_id_t sid);

// Whether the transport should hold `sid` open. KR_WS_PUBLIC is always
// wanted; KR_WS_PRIVATE only while credentials are configured and the
// slot table holds a private subscription — which is what makes the
// private session lazy. Takes kr_ws_ch.mu, so the caller must hold no
// session lock (lock order: kr_ws_ch.mu outer, w->lock inner).
bool    kr_ws_channels_session_wanted(kr_ws_session_id_t sid);

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
//
// The handle is this multiplexer's own subscriber node, and it lives and
// dies with this mapping: kr_ws_channels_deinit frees every node still
// outstanding. The abstraction wraps it and never forwards one issued by
// an earlier registration, so nothing stale reaches here (OBS-19).
//
// OBS-51: SUCCESS means the handle covers EVERY (channel, symbol) pair
// asked for. A call that cannot seat all of them seats none, gives back
// what it took and returns FAIL — partial coverage is unreportable here
// and the consumer cannot repair it, so refusing is the honest answer.
bool    kr_ws_subscribe(const exchange_ws_channel_t *channels,
            uint32_t n_channels, const char *const *product_ids,
            uint32_t n_products, exchange_ws_event_cb_t cb, void *user,
            void **out_handle);

void    kr_ws_unsubscribe(void *driver_sub);

#endif // BM_KRAKEN_WS_CHANNELS_H
