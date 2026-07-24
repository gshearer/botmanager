# kraken — Kraken Spot Exchange Plugin

Service plugin (`PLUGIN_SERVICE`) for Kraken Spot, served at
`https://api.kraken.com/0/...` for REST and `wss://ws.kraken.com/v2`
/ `wss://ws-auth.kraken.com/v2` for WebSocket v2. Provides the
`exchange_kraken` capability tag and self-registers an exchange
vtable with `feature_exchange`. The same kraken plugin handles every
Kraken interaction — REST (candles, accounts, orders, fills) and WS
v2 (ticker, trade, ohlc, executions, balances).

Auth is **HMAC-SHA512** — Kraken's wire scheme is unchanged since the
v1 REST API:

```
API-Key:  <plugin.kraken.creds.apikey verbatim>
API-Sign: base64(HMAC-SHA512(
              base64-decode(plugin.kraken.creds.private_key),
              uripath || SHA256(nonce_str || postdata)))
```

No JWTs, no per-request asymmetric crypto — this is symmetric MAC
over a uripath/postdata commitment. Kraken has no sandbox concept;
production endpoints are the only target.

## Layout

`plugins/service/kraken/` (`PLUGIN_SERVICE`, kind `kraken`). Provides
the `exchange_kraken` capability tag. A hard `requires` on
`feature_exchange` ensures the dispatch abstraction is up before this
plugin's `init` runs.

REST traffic routes through `feature_exchange`'s priority queue +
token bucket via `kr_exchange_register_vtable()` in `kr_start`.
WebSocket subscriptions are owned by the plugin's channel multiplexer
and surface to consumers as opaque `exchange_ws_sub_t` handles
allocated by `kr_ws_subscribe`.

## Scope

Two surfaces, served by the same plugin:

| Surface | URL | Auth | Purpose |
|---------|-----|------|---------|
| REST | `https://api.kraken.com` | `API-Key` + `API-Sign` HMAC-SHA512 on every private POST; public GETs are unauthenticated | OHLC candles, BalanceEx, AddOrder, CancelOrder, QueryOrders, Open/ClosedOrders, TradesHistory, AssetPairs cache |
| WebSocket v2 | `wss://ws.kraken.com/v2` (public), `wss://ws-auth.kraken.com/v2` (private) | Public channels are unauthenticated; private channels embed a token from `POST /0/private/GetWebSocketsToken` in the subscribe payload | Live streams: `ticker`, `trade`, `ohlc`, `executions`, `balances`, `heartbeat` |

Kraken does not publish a sandbox surface. The REST + WS URLs are the
single production target.

## Layering

| Aspect | Value |
|--------|-------|
| Plugin type | `PLUGIN_SERVICE` |
| Plugin kind | `kraken` |
| Provides feature | `exchange_kraken` |
| Requires | `feature_exchange` |
| Home directory | `plugins/service/kraken/` |
| Shared library | `libkraken.so` |

Hard layering rules apply (`plugins/service/AGENTS.md`):

1. **Zero user commands.** Kraken-related `/` commands belong in the
   whenmoon feature plugin (when they mutate whenmoon state) or in a
   `plugins/feature/kraken/` command surface (for standalone use). The
   Rule 1 leaf exception (a graph-leaf service may carry its own
   command surface) does **not** apply here: `kraken` is a non-leaf
   service, so it stays mechanism-only.
2. **No upward includes or `plugin_dlsym`.** Service plugins stay
   pure mechanism.
3. **KV schema is ours.** All operator-facing knobs sit under
   `plugin.kraken.*`.

## KV Knobs

| Key | Type | Default | Role |
|-----|------|---------|------|
| `plugin.kraken.rest_url` | STR | `https://api.kraken.com` | REST base URL. |
| `plugin.kraken.ws_url_public` | STR | `wss://ws.kraken.com/v2` | Public WebSocket URL (ticker, trade, ohlc). |
| `plugin.kraken.ws_url_private` | STR | `wss://ws-auth.kraken.com/v2` | Private WebSocket URL (executions, balances). |
| `plugin.kraken.creds.apikey` | STR (secret) | `` | API key string. Sent verbatim in `API-Key`. |
| `plugin.kraken.creds.private_key` | STR (secret) | `` | Base64-encoded HMAC secret. Decoded once and cached. |
| `plugin.kraken.rest_enabled` | BOOL | `true` | Enable REST dispatcher. |
| `plugin.kraken.ws_enabled` | BOOL | `false` | Enable WebSocket reader. |
| `plugin.kraken.ws_reconnect_ms` | UINT32 | `2000` | Initial WebSocket reconnect backoff (capped at 60 s). |
| `plugin.kraken.request_timeout` | UINT32 | `15` | Per-call REST timeout. |
| `plugin.kraken.assetpairs_refresh_sec` | UINT32 | `86400` | Cadence for the altname/canonical/wsname cache refresh. |
| `plugin.kraken.last_nonce` | UINT64 | `0` | Last-minted nonce; persisted per-request for restart safety. |

The `creds.*` keys are auto-secret via `kv_is_secret_key` (the
`creds` segment is non-tail). Reads without admin context return
`KV_REDACTED_VALUE`.

### Credentials

Mint a Kraken API key at kraken.com → Settings → API → "Generate New
Key". Kraken exposes two strings: the **API Key** and the **Private
Key** (a base64-encoded HMAC secret). Both go straight into KV:

```
set kv plugin.kraken.creds.apikey <api-key-string>
set kv plugin.kraken.creds.private_key <base64-secret>
```

`kraken_apikey_configured()` returns true iff both KVs are non-empty
AND `creds.private_key` base64-decodes cleanly. The cached decoded
secret is invalidated transparently on any KV-edit — no daemon
restart needed.

### Symbol formats

Kraken exposes three names per pair:

| Form | Example (BTC/USD) | Where it surfaces |
|------|-------------------|-------------------|
| altname | `XBTUSD` | Most REST endpoints accept this. |
| canonical | `XXBTZUSD` | Legacy fields; some REST endpoints return this. |
| wsname | `XBT/USD` | WebSocket v2 subscribe payloads. |

(`XBT` is Kraken's legacy code for bitcoin. The pair cache normalizes
abstraction-side ids like `BTC-USD` → `XBTUSD` for matching.)

The assetpairs cache (`kraken_pairs.c`, capacity 2048) maps any of the
three onto the others. `kr_pair_lookup_rest` / `_ws` translate at the
call site; cache miss → input passes through unchanged. Refreshed by
the periodic `kr.assetpairs` task, which fires immediately on
registration and then on the interval set by
`plugin.kraken.assetpairs_refresh_sec` (default 86400 s).

## Namespace split: `plugin.kraken.*` vs `plugin.whenmoon.exchange.kraken.*`

Two distinct KV namespaces touch kraken; they belong to different
plugins and serve different purposes:

- **`plugin.kraken.*`** — owned by *this plugin*. Configures *the
  thing that talks to Kraken*: REST/WS URLs, credentials, reconnect
  backoff, REST timeout, nonce state, assetpairs refresh cadence.
- **`plugin.whenmoon.exchange.kraken.*`** — owned by the *whenmoon
  feature plugin*. Configures whenmoon's consumer-side policy when
  calling through kraken: account-poll cadence, per-exchange rate
  limits, the live-trading kill-switch (`live`).

If a knob would still apply to a hypothetical second consumer of the
kraken service plugin, it belongs in `plugin.kraken.*`. If it's
specific to whenmoon's behaviour around kraken, it belongs in
`plugin.whenmoon.exchange.kraken.*`.

## External Dependencies

- `libcurl` (≥7.86 for the WebSocket client; the project ships
  against 8.x).
- `libcrypto` via OpenSSL — HMAC-SHA512 + SHA256 + base64 for
  request signing. An in-tree `kr_b64_decode` handles the private-key
  base64 (OpenSSL's `EVP_DecodeBlock` is not pad-aware).
- `json-c` — response parsing.

## Consumer Access Shapes

Consumers (whenmoon, future strategy plugins) go through the
`feature_exchange` abstraction (`exchange_api.h`) — not through this
plugin's `kraken_api.h` shims directly. The dlsym surface here is for
plugin-internal exports plus the rare consumer that needs Kraken-
specific behaviour (e.g. a Kraken-only admin command). The abstraction
translates `exchange_*_async(name, …)` into the matching `kr_*`
vtable hook based on the resolved `name`.

Two access patterns coexist, both routed via the vtable:

1. **Pull (REST)**: `exchange_fetch_candles_async`,
   `exchange_get_accounts_async`, `exchange_place_order_async`,
   `exchange_cancel_order_async`, `exchange_get_order_async`,
   `exchange_list_orders_async`, `exchange_list_fills_async`. The
   adapter translates between `exchange_*_t` and `kraken_*_t` at the
   seam; whenmoon never sees a `kraken_*` type.
2. **Push (WebSocket v2)**: `exchange_ws_subscribe(name, channels[],
   product_ids[], cb, user)` returns an opaque `exchange_ws_sub_t *`
   handle. Events arrive via `exchange_ws_event_cb_t` on the WS
   reader thread. Reconnect, resubscribe, token refresh, and channel
   multiplexing are owned by the plugin, not the consumer.

### Channel mapping

| `exchange_ws_channel_t` | Kraken v2 channel |
|-------------------------|-------------------|
| `EXCH_WS_TICKER` | `ticker` |
| `EXCH_WS_TRADES` | `trade` |
| `EXCH_WS_OHLC_1M` | `ohlc` |
| `EXCH_WS_USER` | `executions` + `balances` (expanded to two internal slots) |
| `EXCH_WS_BOOK_L2` | unsupported — `kr_ws_subscribe` FAILs cleanly |

Kraken v2 emits a `heartbeat` channel frame on every subscribed
session at ~1 Hz; the dispatcher recognises it and returns silently
(no fanout).

### Sequence gaps

Kraken WS v2 has no per-product sequence number, unlike Coinbase's
Advanced Trade feed. Recovery for missed bars/trades is via REST
candles + TradesHistory backfill (whenmoon's existing path). The
`exchange_ws_event_t::seq_gap` field is always false from Kraken;
consumers that need authoritative gap detection rely on REST coverage
queries.

### WS token cache

`POST /0/private/GetWebSocketsToken` mints a private-channel token
with a documented 15-minute lifetime. The plugin pre-emptively
refreshes at 14 min; concurrent fetches coalesce via a 32-slot waiter
queue. Cache is monotonic — never zeroed past the first success — so
a token-fetch failure during a reconnect doesn't strand existing
private slots.

## Do Not

- Do not register `cmd_register` / `cmd_unregister` calls here; this
  is a service plugin.
- Do not call this plugin's symbols directly from non-kraken TUs —
  use the dlsym shims in `kraken_api.h` or, preferably, the generic
  `exchange_*_async` surface in `exchange_api.h`.
- Do not bypass the `creds`-segment secret tier by reading KV
  outside an admin context — secret-tier reads return
  `KV_REDACTED_VALUE` and the signer FAILs cleanly.
- Do not reintroduce sequence-gap detection. Kraken v2 has no
  authoritative per-product sequence; the same shape was ripped from
  the coinbase WS path in CB-WS-SEQ-1 for the same reason.
- Do not mix HMAC with any forthcoming v2 auth scheme. If Kraken
  ships an asymmetric key surface, rip the HMAC path entirely rather
  than living behind an `#ifdef` wall.
