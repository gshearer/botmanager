# coinbase — Coinbase Advanced Trade Plugin

Service plugin (`PLUGIN_SERVICE`) for Coinbase Advanced Trade (the
retail-facing successor to Coinbase Pro / GDAX, served at
`api.coinbase.com/api/v3/brokerage/...`). Exposes a mechanism API
over both REST and a WebSocket feed so consumers —
`plugins/feature/whenmoon/`, future `plugins/cmd/coinbase/` command
surfaces, other internal callers — can access products, candles,
live trades, order books, accounts, and place orders without
knowing how Coinbase authentication or streaming works.

Auth is **CDP-only**: a per-request JWT signed with an EC P-256
private key. The legacy Coinbase Exchange HMAC scheme
(`apikey`+`apisecret`+`passphrase` against `api.exchange.coinbase.com`)
is not supported — Exchange is institutional-only and most retail
accounts cannot mint keys for it.

## Layout

`plugins/service/coinbase/` (`PLUGIN_SERVICE`, kind `coinbase`).
Provides the `exchange_coinbase` capability tag. A hard `requires`
on `feature_exchange` ensures the dispatch abstraction is up before
this plugin's init runs.

Candle + trade traffic routes through the feature_exchange
priority queue + token bucket via `cb_exchange_register_vtable()` in
`coinbase_init`. Other typed APIs (products, ticker, orders, accounts)
keep the legacy direct-curl path until the live-trading framework is
unpaused — see EX-1 outcomes in `TODO.md` for the rationale.

## Scope

Two surfaces, served by the same plugin:

| Surface | URL | Auth | Purpose |
|---------|-----|------|---------|
| REST (Advanced Trade) | `https://api.coinbase.com` | `Authorization: Bearer <jwt>`; per-request ES256 JWT signed by the CDP key | Snapshots (products, ticker, book), historical candles, order management, account balances |
| WebSocket Feed | `wss://advanced-trade-ws.coinbase.com` | Public channels are unauthenticated; the `user` channel embeds a JWT in the subscribe payload | Live streams: `ticker`, `level2`, `market_trades`, `heartbeats`, `candles`, `status`, authenticated `user` |

Sandbox URLs (`api-sandbox.coinbase.com`,
`advanced-trade-ws-sandbox.coinbase.com`) are selectable via the
`plugin.coinbase.sandbox` KV toggle. Sandbox is for code-shape
verification only — many endpoints behave differently or are absent
entirely; production testing requires a real key against prod.

## Layering

| Aspect | Value |
|--------|-------|
| Plugin type | `PLUGIN_SERVICE` |
| Plugin kind | `coinbase` |
| Provides feature | `exchange_coinbase` |
| Requires | `feature_exchange` |
| Home directory | `plugins/service/coinbase/` |
| Shared library | `libcoinbase.so` |

Hard layering rules apply (`plugins/service/AGENTS.md`):

1. **Zero user commands.** Coinbase-related `/` commands (if any
   are added) belong either in `plugins/feature/whenmoon/` (when they
   mutate whenmoon state) or `plugins/cmd/coinbase/` (for
   standalone users).
2. **No upward includes or `plugin_dlsym`.** Service plugins stay
   pure mechanism.
3. **KV schema is ours.** All operator-facing knobs sit under
   `plugin.coinbase.*`.

## KV Knobs

| Key | Type | Default | Role |
|-----|------|---------|------|
| `plugin.coinbase.sandbox` | BOOL | `false` | Use sandbox URLs. |
| `plugin.coinbase.rest_url_prod` | STR | `https://api.coinbase.com` | REST base URL (prod). |
| `plugin.coinbase.rest_url_sandbox` | STR | `https://api-sandbox.coinbase.com` | REST base URL (sandbox). |
| `plugin.coinbase.ws_url_prod` | STR | `wss://advanced-trade-ws.coinbase.com` | WebSocket URL (prod). |
| `plugin.coinbase.ws_url_sandbox` | STR | `wss://advanced-trade-ws-sandbox.coinbase.com` | WebSocket URL (sandbox). |
| `plugin.coinbase.creds.key_name` | STR (secret) | `` | CDP key id (`organizations/<org>/apiKeys/<uuid>`). Empty = public-only mode. |
| `plugin.coinbase.creds.private_key_pem` | STR (secret) | `` | EC P-256 PEM. Literal `\n` escape sequences are unescaped at parse time. |
| `plugin.coinbase.rest_enabled` | BOOL | `true` | Enable REST dispatcher. |
| `plugin.coinbase.ws_enabled` | BOOL | `false` | Enable WebSocket reader. |
| `plugin.coinbase.cache_ttl` | UINT32 | `5` | Seconds before a snapshot is refreshed on demand. |
| `plugin.coinbase.ws_reconnect_ms` | UINT32 | `2000` | Initial WebSocket reconnect backoff. |
| `plugin.coinbase.request_timeout` | UINT32 | `15` | Per-call REST timeout. |

The `creds.*` keys are auto-secret via `kv_is_secret_key` (the
`creds` segment is non-tail). Reads without admin context return
`KV_REDACTED_VALUE`.

### CDP credentials

Mint a CDP key at coinbase.com → Settings → API → "Create CDP key".
Coinbase emits a JSON file containing the `name`
(`organizations/<org-uuid>/apiKeys/<key-uuid>`) and a PEM-encoded EC
private key. Both go straight into KV:

```
set kv plugin.coinbase.creds.key_name organizations/<org>/apiKeys/<uuid>
set kv plugin.coinbase.creds.private_key_pem -----BEGIN EC PRIVATE KEY-----\n...\n-----END EC PRIVATE KEY-----\n
```

The PEM may be a single line with literal `\n` escape sequences —
`coinbase_sign_cdp.c::cb_pem_unescape` translates them to real
newlines before `PEM_read_bio_PrivateKey`. JWTs are minted per
request (`cb_sign_jwt`), valid for 120s, with a fresh 32-hex
`nonce` from `getrandom(2)`.

`cb_apikey_configured()` (alias for `cb_cdp_configured()`)
short-circuits any private call to `CB_ERR_NO_CREDS` if either KV
is empty.

## Namespace split: `plugin.coinbase.*` vs `plugin.whenmoon.exchange.coinbase.*`

Two distinct KV namespaces touch coinbase. They belong to different
plugins and serve different purposes — neither is redundant:

- **`plugin.coinbase.*`** — owned by *this plugin* (the coinbase
  service plugin). Configures *the thing that talks to Coinbase*:
  REST/WS URLs, credentials, sandbox toggle, REST cache TTL, WS
  reconnect backoff. Anything that changes bytes-on-the-wire toward
  `api.coinbase.com` lives here.

- **`plugin.whenmoon.exchange.coinbase.*`** — owned by the *whenmoon
  feature plugin* (`plugins/feature/whenmoon/`). Configures
  whenmoon's *consumer-side* policy when calling through coinbase:
  account-poll cadence (`account.refresh_sec`), per-exchange
  rate-limit (`rate_limit_rps`), and the WM-LT-8 live-trading
  kill-switch (`live`). These knobs change *how whenmoon uses*
  coinbase, not how coinbase itself is configured.

Rule of thumb: if a knob would still apply to a hypothetical second
consumer of the coinbase service plugin, it belongs in
`plugin.coinbase.*`. If it's specific to whenmoon's behavior around
coinbase calls, it belongs in `plugin.whenmoon.exchange.coinbase.*`.

## External Dependencies

- `libcurl` (≥7.86 for the WebSocket client; the project ships
  against 8.x).
- `libcrypto` via OpenSSL — ECDSA-P256 sign + base64 for JWT
  construction. No libjwt dependency; the JWT minter is ~250 LOC of
  EVP calls in `coinbase_sign_cdp.c`.
- `json-c` — response parsing.

## Consumer Access Shapes

Two access patterns coexist, both routed through `coinbase_api.h`:

1. **Pull (REST)**: `coinbase_fetch_candles_async(…)`,
   `coinbase_fetch_ticker_async(…)`, etc. Consumer supplies a typed
   completion callback; delivery happens on the curl worker.
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
- Do not reintroduce HMAC auth, the `apikey`/`apisecret`/
  `passphrase` KV triple, or any reference to
  `api.exchange.coinbase.com`. Exchange is dead for retail; CDP is
  the only supported auth path.
