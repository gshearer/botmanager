# gemini — Gemini Spot Exchange Plugin

Service plugin (`PLUGIN_SERVICE`) for Gemini Spot, served at
`https://api.gemini.com/v1/...` for REST and
`wss://api.gemini.com/v2/marketdata` (public) /
`wss://api.gemini.com/v1/order/events` (private) for WebSocket.
Provides the `exchange_gemini` capability tag and self-registers an
exchange vtable with `feature_exchange`. The same gemini plugin
handles every Gemini interaction — REST (candles, balances, orders,
fills, symbols) and WebSocket (l2, candles_1m,
order events).

Auth is **HMAC-SHA384** over a base64-encoded JSON payload — Gemini's
wire scheme is unchanged since the v1 REST API:

```
X-GEMINI-APIKEY:    <plugin.gemini.creds.apikey verbatim>
X-GEMINI-PAYLOAD:   base64(json_payload)            ; payload contains
                                                    ; request path + nonce
X-GEMINI-SIGNATURE: lowercase_hex(HMAC-SHA384(
                        base64-decode(plugin.gemini.creds.private_key),
                        payload_b64))
```

Note the asymmetry with Kraken: Gemini's signature output is
**lowercase hex**, not base64, and the payload is a complete JSON
envelope (including the request path) carried in a header — the HTTP
body for private POSTs is empty (`Content-Length: 0`). Every
parameter rides inside the signed payload. Gemini has no sandbox
concept; production endpoints are the only target.

## Layout

`plugins/service/gemini/` (`PLUGIN_SERVICE`, kind `gemini`). Provides
the `exchange_gemini` capability tag. A hard `requires` on
`feature_exchange` ensures the dispatch abstraction is up before this
plugin's `init` runs.

REST traffic routes through `feature_exchange`'s priority queue +
token bucket via `gem_exchange_register_vtable()` in `gem_start`.
WebSocket subscriptions are owned by the plugin's channel multiplexer
and surface to consumers as opaque `exchange_ws_sub_t` handles
allocated by `gem_ws_subscribe`.

## Scope

Two surfaces, served by the same plugin:

| Surface | URL | Auth | Purpose |
|---------|-----|------|---------|
| REST | `https://api.gemini.com` | Public GETs unauthenticated; private POSTs carry `X-GEMINI-APIKEY` + `X-GEMINI-PAYLOAD` + `X-GEMINI-SIGNATURE` HMAC-SHA384 | OHLC candles, balances, new/cancel/status order, active orders, my trades, symbols + symbols/details cache |
| WebSocket Market Data v2 | `wss://api.gemini.com/v2/marketdata` | Unauthenticated, multi-symbol, channel-based | Live streams: **`l2`** (book diffs → derived ticker, and `type:trade` prints arrive inside it) and **`candles_1m`**. ⛔ Those are the only two subscribable channels: `trades`/`trade` are both answered `{"reason":"InvalidJson","result":"error"}`, and **there is no `subscription_ack`** — this gateway acknowledges nothing, a subscribe with the snapshot and an unsubscribe with silence (`OBS-53`, `OBS-55`; rig `temp/obs53/`). |
| WebSocket Order Events | `wss://api.gemini.com/v1/order/events` | HMAC headers on the HTTP handshake — no token endpoint | Per-account stream: `initial / accepted / booked / fill / cancelled / closed` → fans out as `EXCH_WS_USER_KIND_ORDER` / `EXCH_WS_USER_KIND_FILL` events |

Gemini does not publish a sandbox surface. The REST + WS URLs are the
single production target.

## REST surface

Endpoint paths used by the typed wrappers in `gemini_orders.c`:

| Path | HTTP | Auth | Used by |
|------|------|------|---------|
| `/v1/symbols` | GET | none | symbols-cache primer (native list) |
| `/v1/symbols/details/<sym>` | GET | none | symbols-cache primer (base+quote split) |
| `/v2/candles/<sym>/<time_frame>` | GET | none | `gemini_fetch_candles_async` |
| `/v1/balances` | POST | HMAC | `gemini_get_balance_async` |
| `/v1/order/new` | POST | HMAC | `gemini_add_order_async` |
| `/v1/order/cancel` | POST | HMAC | `gemini_cancel_order_async` |
| `/v1/order/status` | POST | HMAC | `gemini_query_order_async` |
| `/v1/orders` | POST | HMAC | `gemini_active_orders_async` |
| `/v1/mytrades` | POST | HMAC | `gemini_mytrades_async` |

Gemini's private surface is **POST-only** — every parameter rides
inside the base64-encoded JSON payload header. A
`EXCHANGE_OP_PRIVATE_REST_GET` or `_DELETE` reaching
`gem_exchange_submit` is rejected cleanly with `"Gemini private
endpoints are POST-only"`. The capability layer doesn't issue those
op kinds today; the gate exists for defence in depth.

## Layering

| Aspect | Value |
|--------|-------|
| Plugin type | `PLUGIN_SERVICE` |
| Plugin kind | `gemini` |
| Provides feature | `exchange_gemini` |
| Requires | `feature_exchange` |
| Home directory | `plugins/service/gemini/` |
| Shared library | `libgemini.so` |

Hard layering rules apply (`plugins/service/AGENTS.md`):

1. **Zero user commands.** Gemini-related `/` commands belong in the
   whenmoon feature plugin (when they mutate whenmoon state) or in a
   `plugins/feature/gemini/` command surface (for standalone use).
   Today neither exists — whenmoon talks to gemini through the
   `feature_exchange` abstraction, never through `gemini_api.h`
   directly. The Rule 1 leaf exception (a graph-leaf service may carry
   its own command surface) does **not** apply here: `gemini` is a
   non-leaf service, so it stays mechanism-only.
2. **No upward includes or `plugin_dlsym`.** Service plugins stay
   pure mechanism.
3. **KV schema is ours.** All operator-facing knobs sit under
   `plugin.gemini.*`.

## KV Knobs

| Key | Type | Default | Role |
|-----|------|---------|------|
| `plugin.gemini.rest_url` | STR | `https://api.gemini.com` | REST base URL. |
| `plugin.gemini.ws_url_marketdata` | STR | `wss://api.gemini.com/v2/marketdata` | Public WebSocket URL (`l2`, `candles_1m` — the only two channels it has). |
| `plugin.gemini.ws_url_order_events` | STR | `wss://api.gemini.com/v1/order/events` | Private WebSocket URL. `?heartbeat=true` is appended at connect time only when the operator-set value does not already carry a query string. |
| `plugin.gemini.creds.apikey` | STR (secret) | `` | Master/primary/scoped API key id. Sent verbatim in `X-GEMINI-APIKEY`. |
| `plugin.gemini.creds.private_key` | STR (secret) | `` | Base64-encoded HMAC-SHA384 secret. Decoded once and cached. |
| `plugin.gemini.rest_enabled` | BOOL | `true` | Enable REST dispatcher. |
| `plugin.gemini.ws_enabled` | BOOL | `false` | Enable both WebSocket readers (Market Data v2 + Order Events). Order Events stays disconnected when creds are not configured even with this set. |
| `plugin.gemini.ws_reconnect_ms` | UINT32 | `2000` | Initial WebSocket reconnect backoff (capped at 60 s by `GEM_WS_MAX_BACKOFF_MS`). |
| `plugin.gemini.request_timeout` | UINT32 | `15` | Per-call REST timeout. |
| `plugin.gemini.symbols_refresh_sec` | UINT32 | `86400` | Cadence for the symbols-cache refresh. |
| `plugin.gemini.last_nonce` | UINT64 | `0` | Last-minted nonce; persisted per-request for restart safety. |

The `creds.*` keys are auto-secret via `kv_is_secret_key` (the
`creds` segment is non-tail). Reads without admin context return
`KV_REDACTED_VALUE`.

### Credentials

Mint a Gemini API key at gemini.com → Settings → API. Gemini exposes
two strings: the **API Key** and the **API Secret** (a base64-encoded
HMAC-SHA384 secret). Both go straight into KV:

```
set kv plugin.gemini.creds.apikey <api-key-string>
set kv plugin.gemini.creds.private_key <base64-secret>
```

`gem_apikey_configured()` returns true iff both KVs are non-empty
AND `creds.private_key` base64-decodes cleanly. The cached decoded
secret is invalidated transparently on any KV-edit — no daemon
restart needed.

Per `feedback_freshstart_never_shared`, credentials are installed
manually post-`scripts/freshstart.sh`; they are not stored in any
committed file and do not survive a freshstart.

### Nonce persistence

The plugin mints monotonically-increasing 64-bit nonces from
`gemini_sign.c`. After every mint the new value is best-effort
persisted to `plugin.gemini.last_nonce` so a daemon restart never
re-issues a stale nonce. On startup the seed is
`max(persisted_nonce, time(NULL) * 1000000)`. The torn-write cost is
one rejected request — the operator clears it by bumping the KV by
hand. Two daemon instances sharing a key are unsupported (Gemini
will reject the lower-nonce instance's traffic at the next collision).

### Symbol formats

Gemini exposes one wire form per pair, with letter-case driven by
surface:

| Form | Example (BTC/USD) | Where it surfaces |
|------|-------------------|-------------------|
| native, REST | `btcusd` | REST endpoint paths + payloads. |
| native, WS | `BTCUSD` | WebSocket subscribe payloads + frame `symbol` field. |
| abstraction | `BTC-USD` | `exchange_*_async(name, product_id, ...)` callers. |

The symbols cache (`gemini_pairs.c`, capacity `GEM_SYMS_CAP = 1024`)
maps any of the three onto the others. `gem_pair_to_native` /
`gem_pair_to_abstr` translate at the call site; cache miss falls
back to a heuristic split on the conventional quote currencies
(`usdt, usdc, busd, dai, usd, eur, gbp, sgd, btc, eth`).

**Startup, in order (`gem_start`).** Three steps, and the order is the
contract:

1. `gem_symbols_prime_sync()` — one SELECT of the persisted
   `gemini_symbols` snapshot (347 rows, measured 2026-08-17), applied
   unconditionally. **No network, no staleness test.**
2. `gem_exchange_register_vtable()` — registration fires
   feature_exchange's registration watch, and a consumer rebuilds its WS
   subscriptions *synchronously inside that call*, resolving each product
   against the cache step 1 just filled.
3. the periodic `gem.symbols` task, whose first tick fires immediately on
   submit and calls `gem_symbols_load_or_refresh_async()` — which
   re-judges the snapshot's age and refreshes from the network only when
   it is missing or older than `plugin.gemini.symbols_refresh_sec`
   (default 86400 s).

⚠ Step 1 exists because step 2 cannot wait for step 3 (`OBS-47`). It is
the second and last synchronous DB touch on the startup path — the other
is `gem_symbols_ensure_table()` in `gem_init` — and neither touches the
network: nothing blocks `start()` on Gemini's fan-out, so the operator
control socket comes up regardless. A missing, failed or empty snapshot
leaves the cache as it was and step 3 handles it.

⭑ A **stale** snapshot is applied and *then* refreshed, never discarded
in favour of a fetch that has not landed: it is the same data the fetch
will mostly return, and declining to apply it would empty the cache for
the whole duration of the fan-out below.

The cache is populated from the network in two stages: `GET /v1/symbols`
returns the flat native list, then a per-symbol
`GET /v1/symbols/details/<sym>` yields the base + quote split (no batch
endpoint exists). At Gemini's 347 spot symbols and the default
`rate_limit_rps = 5` that N+1 fan-out runs for a minute or more, entirely
in the background — which is precisely why the snapshot, and not the
fetch, is what registration waits on.

## Namespace split: `plugin.gemini.*` vs `plugin.whenmoon.exchange.gemini.*`

Two distinct KV namespaces touch gemini; they belong to different
plugins and serve different purposes:

- **`plugin.gemini.*`** — owned by *this plugin*. Configures *the
  thing that talks to Gemini*: REST/WS URLs, credentials, reconnect
  backoff, REST timeout, nonce state, symbols refresh cadence.
- **`plugin.whenmoon.exchange.gemini.*`** — owned by the *whenmoon
  feature plugin*. Configures whenmoon's consumer-side policy when
  calling through gemini: account-poll cadence (`account.refresh_sec`,
  default 30).
- **`plugin.exchange.gemini.*`** — owned by the *feature_exchange
  abstraction*. Configures the per-exchange priority queue + token
  bucket: `rate_limit_rps` (default 5; midpoint between Gemini's
  120/min public and 600/min private budgets) and `reserved_slots.*`
  (default: one slot reserved at `EXCHANGE_PRIO_TRANSACTIONAL`).

If a knob would still apply to a hypothetical second consumer of the
gemini service plugin, it belongs in `plugin.gemini.*`. If it's
specific to whenmoon's behaviour around gemini, it belongs in
`plugin.whenmoon.exchange.gemini.*`. If it concerns dispatch fairness
across consumers, it belongs in `plugin.exchange.gemini.*`.

## External Dependencies

- `libcurl` (≥7.86 for the WebSocket client; the project ships
  against 8.x).
- `libcrypto` via OpenSSL — HMAC-SHA384 + base64 for request signing.
  An in-tree `gem_b64_decode` handles the private-key base64
  (OpenSSL's `EVP_DecodeBlock` is not pad-aware).
- `json-c` — response parsing.

## Consumer Access Shapes

Consumers (whenmoon, future strategy plugins) go through the
`feature_exchange` abstraction (`exchange_api.h`) — not through this
plugin's `gemini_api.h` shims directly. The dlsym surface here is for
plugin-internal exports plus the rare consumer that needs Gemini-
specific behaviour. The abstraction translates `exchange_*_async(name,
…)` into the matching `gem_exch_*` vtable hook based on the resolved
`name`.

Two access patterns coexist, both routed via the vtable:

1. **Pull (REST)**: `exchange_fetch_candles_async`,
   `exchange_get_accounts_async`, `exchange_place_order_async`,
   `exchange_cancel_order_async`, `exchange_get_order_async`,
   `exchange_list_orders_async`, `exchange_list_fills_async`. The
   adapter translates between `exchange_*_t` and `gemini_*_t` at the
   seam; whenmoon never sees a `gemini_*` type.
2. **Push (WebSocket)**: `exchange_ws_subscribe(name, channels[],
   product_ids[], cb, user)` returns an opaque `exchange_ws_sub_t *`
   handle. Events arrive via `exchange_ws_event_cb_t` on the WS
   reader thread. Reconnect, resubscribe, heartbeat, and channel
   multiplexing are owned by the plugin, not the consumer.

### Order-type collapse

Gemini's REST surface uses verbose order-type strings (`"exchange
limit"`, `"exchange market"`, `"exchange stop"` /
`"exchange stop_limit"`). The plugin collapses them to the
abstraction's compact form (`"limit"`, `"market"`, `"stop"`) via the
shared `gem_type_to_generic` static inline in `gemini.h` — both
`gemini_exchange.c` (REST adapters) and `gemini_ws_channels.c` (Order
Events parser) use the same collapse so the generic
`exchange_order_t.type` field reads identically across surfaces.

### Channel mapping

| `exchange_ws_channel_t` | Gemini wire channel | Notes |
|-------------------------|---------------------|-------|
| `EXCH_WS_TICKER` | `l2` | Gemini publishes no native ticker channel; the multiplexer derives ticker events from `l2_updates` top-of-book frames. |
| `EXCH_WS_TRADES` | `l2` | One `EXCH_WS_TRADES` event per `type:trade` envelope — which arrives **inside the `l2` stream**. ⛔ There is no separate trades channel: `gem_ws_md_channel_name` renders `"trades"` and the endpoint answers `{"reason":"InvalidJson","result":"error"}`; `"trade"` singular, as this table used to claim, is refused the same way. Both measured 2026-08-17 (`OBS-55`). |
| `EXCH_WS_OHLC_1M` | `candles_1m` | 1-minute bars only. |
| `EXCH_WS_USER` | Order Events session | Maps `accepted / booked / fill / cancelled / closed` onto `EXCH_WS_USER_KIND_ORDER` / `EXCH_WS_USER_KIND_FILL` (fills carry both an `ORDER` update for status + a `FILL` event for the executed quantity). |
| `EXCH_WS_BOOK_L2` | unsupported | `gem_ws_subscribe` returns FAIL synchronously. |

⛔⛔ **The Market Data v2 socket does NOT emit a `subscription_ack`, and
this paragraph used to say that it did.** Measured live 2026-08-17
(`OBS-42`'s `G6`, filed as `OBS-53`): zero acks across ~7 minutes of
live `l2` feed, zero across the daemon's entire logged history, and
none to an external probe sending this driver's exact subscribe frame
for two different symbols — the reply is the `l2` snapshot and then a
`heartbeat`. A subscribe here is confirmed only by data arriving.
⚠ **Do not read the parser as evidence of the protocol.** The handler
below exists and is correct; nothing on this endpoint has ever reached
it. The Order Events socket is a separate question and is not covered
by this measurement — it needs credentials this tree does not have.

**Consequence, and it is live today**: `gateway_holds` is written only
by the ack handler, and the unsubscribe emit refuses a slot that does
not have it, so **no unsubscribe frame is ever sent to this venue** and
an unwanted slot sits at `refcount == 0` in `SUBSCRIBING`, which
compaction refuses by design. `gem_ws_channels_on_open`'s unconditional
reset is the only thing that reclaims it, so the strand is bounded by
the socket's lifetime rather than being permanent. Kraken, by contrast,
acks every subscribe and unsubscribes on the wire — verified side by
side on one daemon.

**An ack can outlive the consumer that caused it, and that is a case
the dispatcher handles rather than a case that cannot happen**
(`OBS-42`) — at any venue that sends one. A consumer leaving between
the subscribe frame going out and its ack landing correctly emits no
unsubscribe — at that moment the gateway does not hold the subscription
yet — so when the ack arrives it lands on a slot at `refcount == 0`.
`gem_ws_md_handle_sub_ack_locked` reports that, and the dispatcher reaps
the slot under the same lock hold. Without it the gateway streams that
symbol to nobody until some unrelated consumer happens to unsubscribe.

Because there is no req_id, **identity — `(channel, symbol_native)` —
is the only correlator this driver has**, and it is what the unsubscribe
emit re-derives its slot from after dropping `mu` around the send. It
must never re-derive by index: the slot table compacts by
swap-with-last, so an index taken before the lock was dropped names a
different slot afterwards, and the completion's writes land on whatever
was swapped in.

### Sequence gaps

Gemini WS frames carry no per-product sequence number that the
multiplexer trusts as authoritative. Recovery for missed
bars/trades is via the REST candle backfill path
(`gemini_fetch_candles_async` driven by whenmoon's downloader). The
`exchange_ws_event_t::seq_gap` field is always false from Gemini;
consumers that need authoritative gap detection rely on REST coverage
queries. This matches the shape used by Kraken — Coinbase's Advanced
Trade per-product `sequence` was the only one we ever wired, and even
that was ripped back to a placeholder in CB-WS-SEQ-1.

### Order Events handshake

`POST /v1/order/events` authenticates via the same `X-GEMINI-APIKEY`
/ `X-GEMINI-PAYLOAD` / `X-GEMINI-SIGNATURE` headers on the HTTP
handshake — there is NO token-fetch endpoint analogous to Kraken's
`GetWebSocketsToken`. The plugin re-signs every reconnect against
`{"request":"/v1/order/events","nonce":<n>}` with a fresh nonce
minted via `gem_next_nonce`. Order Events stays disconnected when
`gem_apikey_configured()` returns false, even with
`plugin.gemini.ws_enabled=true`.

## Granularity Coverage

`gem_fetch_candles_async` returns FAIL with `err="gemini: granularity
unsupported"` for the gaps in Gemini's offering:

| `exchange_granularity_t` | Gemini `time_frame` |
|--------------------------|---------------------|
| `EXCH_GRAN_1M` | `1m` |
| `EXCH_GRAN_5M` | `5m` |
| `EXCH_GRAN_15M` | `15m` |
| `EXCH_GRAN_30M` | `30m` |
| `EXCH_GRAN_1H` | `1hr` |
| `EXCH_GRAN_4H` | _unsupported_ |
| `EXCH_GRAN_1D` | `1day` |
| `EXCH_GRAN_1W` | _unsupported_ |

Whenmoon handles per-protocol gran rejection gracefully (Coinbase has
the same 4H gap). The 4H grain is consumed via aggregator cascade
from 1H bars when running on Gemini.

## Rate Limits

Gemini publishes 120 req/min on public endpoints and 600 req/min on
private endpoints (per-key). The exchange-abstraction layer enforces
`plugin.exchange.gemini.rate_limit_rps` (default 5) as a conservative
midpoint that does not exceed either budget. Burst is advertised at
15 to match the other backends. REST history is shallow: `/v2/candles`
returns ≤ 500 most-recent candles per call and accepts no `since`
parameter — `gemini_fetch_candles_async` filters `since_ms` /
`until_ms` client-side. See `gemini_rest_history_depth` memory for
the per-grain coverage that lands.

## Do Not

- Do not register `cmd_register` / `cmd_unregister` calls here; this
  is a service plugin.
- Do not call this plugin's symbols directly from non-gemini TUs —
  use the dlsym shims in `gemini_api.h` or, preferably, the generic
  `exchange_*_async` surface in `exchange_api.h`.
- Do not bypass the `creds`-segment secret tier by reading KV
  outside an admin context — secret-tier reads return
  `KV_REDACTED_VALUE` and the signer FAILs cleanly.
- Do not reintroduce sequence-gap detection. Gemini emits no
  authoritative per-product sequence; the same shape was ripped from
  the coinbase WS path in CB-WS-SEQ-1 and never wired for Kraken.
- Do not mix HMAC with any forthcoming v2 auth scheme. If Gemini
  ships an asymmetric key surface, rip the HMAC path entirely rather
  than living behind an `#ifdef` wall (per
  `feedback_no_deprecated_code`).
