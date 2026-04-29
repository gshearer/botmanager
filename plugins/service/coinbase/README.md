# coinbase — Coinbase Exchange Plugin

Service plugin (`PLUGIN_SERVICE`) for the Coinbase Exchange
(the former Coinbase Pro / GDAX product, not Advanced Trade or Prime).
Exposes a mechanism API over both REST and a WebSocket feed so
consumers — `plugins/feature/whenmoon/`, future `plugins/cmd/coinbase/`
command surfaces, other internal callers — can access products,
candles, live trades, order books, accounts, and place orders without
knowing how Coinbase authentication or streaming works.

## Layout

`plugins/service/coinbase/` (`PLUGIN_SERVICE`, kind `coinbase`).
Provides the `exchange_coinbase` capability tag. A hard `requires`
on `feature_exchange` ensures the dispatch abstraction is up before
this plugin's init runs.

Candle + trade traffic now routes through the feature_exchange
priority queue + token bucket via `cb_exchange_register_vtable()` in
`coinbase_init`. Other typed APIs (products, ticker, orders, accounts)
keep the legacy direct-curl path until the live-trading framework is
unpaused — see EX-1 outcomes in `TODO.md` for the rationale.

## Scope

Two surfaces, served by the same plugin:

| Surface | URL | Auth | Purpose |
|---------|-----|------|---------|
| REST (Exchange API) | `https://api.exchange.coinbase.com` | HMAC-SHA256 headers `CB-ACCESS-KEY`, `CB-ACCESS-SIGN`, `CB-ACCESS-TIMESTAMP`, `CB-ACCESS-PASSPHRASE` | Historical data (candles, trades), snapshots (products, ticker, book), order management, account balances |
| WebSocket Feed | `wss://ws-feed.exchange.coinbase.com` | Optional HMAC signature in the `subscribe` message | Live streams: ticker, level2 order book, matches, heartbeat, and authenticated user/full channels |

Sandbox URLs (`api-public.sandbox.exchange.coinbase.com`,
`ws-feed-public.sandbox.exchange.coinbase.com`) are selectable via
the `plugin.coinbase.sandbox` KV toggle.

## Layering

| Aspect | Value |
|--------|-------|
| Plugin type | `PLUGIN_SERVICE` |
| Plugin kind | `coinbase` |
| Provides feature | `service_coinbase` |
| Requires | *(none — libcurl/openssl/json-c link at the meson level)* |
| Home directory | `plugins/service/coinbase/` |
| Shared library | `libcoinbase.so` |

Hard layering rules apply (`plugins/service/AGENTS.md`):

1. **Zero user commands.** Coinbase-related `/` commands (if any
   are added) belong either in `plugins/feature/whenmoon/` (when they
   mutate whenmoon bot state) or `plugins/cmd/coinbase/` (for
   standalone users).
2. **No upward includes or `plugin_dlsym`.** Service plugins stay
   pure mechanism.
3. **KV schema is ours.** All operator-facing knobs sit under
   `plugin.coinbase.*`.

## KV Knobs (scaffolded today)

| Key | Type | Default | Role |
|-----|------|---------|------|
| `plugin.coinbase.sandbox` | BOOL | `false` | Use sandbox URLs. |
| `plugin.coinbase.rest_url_prod` | STR | `https://api.exchange.coinbase.com` | REST base URL (prod). |
| `plugin.coinbase.rest_url_sandbox` | STR | `https://api-public.sandbox.exchange.coinbase.com` | REST base URL (sandbox). |
| `plugin.coinbase.ws_url_prod` | STR | `wss://ws-feed.exchange.coinbase.com` | WebSocket URL (prod). |
| `plugin.coinbase.ws_url_sandbox` | STR | `wss://ws-feed-public.sandbox.exchange.coinbase.com` | WebSocket URL (sandbox). |
| `plugin.coinbase.apikey` | STR | `` | API key. Empty = public-only mode. |
| `plugin.coinbase.apisecret` | STR | `` | Base64 secret exactly as issued; decoded at sign time. |
| `plugin.coinbase.passphrase` | STR | `` | Passphrase assigned at key creation. |
| `plugin.coinbase.rest_enabled` | BOOL | `true` | Enable REST dispatcher. |
| `plugin.coinbase.ws_enabled` | BOOL | `false` | Enable WebSocket reader. |
| `plugin.coinbase.cache_ttl` | UINT32 | `5` | Seconds before a snapshot is refreshed on demand. |
| `plugin.coinbase.ws_reconnect_ms` | UINT32 | `2000` | Initial WebSocket reconnect backoff. |
| `plugin.coinbase.request_timeout` | UINT32 | `15` | Per-call REST timeout. |

## External Dependencies

- `libcurl` (≥7.86 for the WebSocket client used in CB4; the
  project ships against 8.x).
- `libcrypto` via OpenSSL — HMAC-SHA256 + base64 for request
  signing.
- `json-c` — response parsing.

No libjwt dependency here — the Exchange API predates CDP JWT and
uses HMAC. The Advanced Trade API (`api.coinbase.com/api/v3/…`)
would pull in libjwt, and we are deliberately *not* targeting it
for whenmoon's first exchange.

## Consumer Access Shapes (planned)

Two access patterns will coexist, both routed through
`coinbase_api.h`:

1. **Pull (REST)**: `coinbase_fetch_candles_async(…)`,
   `coinbase_fetch_ticker_async(…)`, etc. Consumer supplies a
   typed completion callback; delivery happens on the curl worker.
2. **Push (WebSocket)**: `coinbase_ws_subscribe(channels[],
   product_ids[], cb, user)` registers a durable subscription.
   Events arrive via `coinbase_ws_event_cb_t` on the WS reader
   thread. Reconnect, resubscribe, and sequence-gap detection are
   owned by the plugin, not the consumer.

Consumers (whenmoon's `market.*` subsystem, any admin command
surface) go through the dlsym-shim wrappers in `coinbase_api.h` —
never via direct linker references.

## Do Not

- Do not register `cmd_register` / `cmd_unregister` calls here;
  this is a service plugin.
- Do not textually reference `old/whenmoon/exch_cbat.c` for API
  shape; that code targeted the Advanced Trade endpoint, not the
  Exchange API we're wrapping here.
