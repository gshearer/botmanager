// market.h — per-market state (candles, live px) for whenmoon bots.
//
// Internal to the whenmoon plugin. Consumers outside this plugin must
// NOT include this header; cross-plugin access would have to go through
// a future public `whenmoon_api.h` dlsym shim, which does not yet
// exist. Gate: WHENMOON_INTERNAL.

#ifndef BM_WHENMOON_MARKET_H
#define BM_WHENMOON_MARKET_H

#ifdef WHENMOON_INTERNAL

#include "coinbase_api.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// Ring-buffer capacity for candles. Backfill lands COINBASE_MAX_CANDLES
// rows at most (300); we keep the buffer at 512 so live 1m-bar inserts
// after backfill do not immediately wrap over fresh data in a restart.
#define WM_MARKET_CANDLE_CAP   512

// Compile-time cap on declared product ids per bot. The KV value is a
// comma-separated string; tokens beyond this count are dropped with a
// CLAM_INFO at parse time.
#define WM_MARKET_MAX          16

typedef struct
{
  char                  product_id[COINBASE_PRODUCT_ID_SZ];

  // Candle ring. `candles` holds WM_MARKET_CANDLE_CAP rows. `n_candles`
  // saturates at WM_MARKET_CANDLE_CAP; `write_pos` is the next slot to
  // overwrite. Consumers reading for a snapshot should lock `lock`,
  // walk `[write_pos - n_candles, write_pos)` modulo cap, then unlock.
  coinbase_candle_t     candles[WM_MARKET_CANDLE_CAP];
  uint32_t              n_candles;
  uint32_t              write_pos;

  // Last observed ticker price (0.0 until the first ticker event
  // lands). last_tick_ms is the event timestamp coinbase_ws_ticker_t
  // reports; 0 when unparseable.
  double                last_px;
  int64_t               last_tick_ms;

  pthread_mutex_t       lock;
} whenmoon_market_t;

struct whenmoon_markets
{
  whenmoon_market_t    *arr;       // size n_markets
  uint32_t              n_markets;

  // One shared WebSocket subscription covering every product_id +
  // {HEARTBEAT, TICKER, MATCHES}. NULL when n_markets == 0 or
  // coinbase_ws_subscribe failed at init time.
  coinbase_ws_sub_t    *ws_sub;
};

// Forward decl to keep this header independent of whenmoon.h.
struct whenmoon_state;

// Init: reads bot.<name>.whenmoon.markets, allocates the container,
// kicks off backfill + ws subscribe. SUCCESS even when the KV is empty
// (the bot runs with no markets). FAIL on allocation failure.
bool wm_market_init(struct whenmoon_state *st);

// Destroy: unsubscribes, destroys per-market mutexes, frees the
// container. Safe to call on a zero-initialised (null `markets`) state.
void wm_market_destroy(struct whenmoon_state *st);

// Async callback invoked by coinbase on backfill completion. `user` is
// a heap-owned wm_market_backfill_ctx_t* that the callback frees.
void wm_market_on_candles(const coinbase_candles_result_t *res, void *user);

// WebSocket event fanout — one handler shared across every product.
// `user` is the whenmoon_state_t*. Invoked on the coinbase WS reader
// thread; keep work minimal.
void wm_market_on_event(const coinbase_ws_event_t *ev, void *user);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_MARKET_H
