# kraken — Kraken Spot Exchange Plugin

Service plugin (`PLUGIN_SERVICE`) for Kraken Spot, served at
`https://api.kraken.com/0/...` for REST and `wss://ws.kraken.com/v2`
/ `wss://ws-auth.kraken.com/v2` for WebSocket v2. Provides the
`exchange_kraken` capability tag and self-registers an exchange
vtable with `feature_exchange`. The same kraken plugin handles every
Kraken interaction — REST (candles, accounts, orders, fills) and WS
v2 (ticker, trade, ohlc, executions, balances).

Status: **KR-3 scaffolding.** The plugin descriptor, KV schema,
HMAC-SHA512 signer, nonce minter, and exchange-vtable registration
are in place. REST traffic still FAILs at the vtable seam — KR-4
fills in the candle / balance / order / fill endpoints. WebSocket v2
lands in KR-5. See `TODO.md` §KR-3..KR-6 for the chunk roadmap.

Auth is **HMAC-SHA512** — Kraken's wire scheme is unchanged since the
v1 REST API:

```
API-Key:  <plugin.kraken.creds.api_key verbatim>
API-Sign: base64(HMAC-SHA512(
              base64-decode(plugin.kraken.creds.private_key),
              uripath || SHA256(nonce_str || postdata)))
```

No JWTs, no per-request asymmetric crypto — this is symmetric MAC
over a uripath/postdata commitment.

## Layout

`plugins/service/kraken/` (`PLUGIN_SERVICE`, kind `kraken`). Provides
the `exchange_kraken` capability tag. A hard `requires` on
`feature_exchange` ensures the dispatch abstraction is up before this
plugin's `init` runs.

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
   whenmoon feature plugin (when they mutate whenmoon state) or in
   `plugins/cmd/kraken/` (for standalone use).
2. **No upward includes or `plugin_dlsym`.** Service plugins stay
   pure mechanism.
3. **KV schema is ours.** All operator-facing knobs sit under
   `plugin.kraken.*`.

## KV Knobs

| Key | Type | Default | Role |
|-----|------|---------|------|
| `plugin.kraken.rest_url` | STR | `https://api.kraken.com` | REST base URL. |
| `plugin.kraken.ws_url_public` | STR | `wss://ws.kraken.com/v2` | Public WebSocket URL (ticker, trade, ohlc, book). |
| `plugin.kraken.ws_url_private` | STR | `wss://ws-auth.kraken.com/v2` | Private WebSocket URL (executions, balances). |
| `plugin.kraken.creds.api_key` | STR (secret) | `` | API key string. Sent verbatim in `API-Key`. |
| `plugin.kraken.creds.private_key` | STR (secret) | `` | Base64-encoded HMAC secret. Decoded once and cached. |
| `plugin.kraken.rest_enabled` | BOOL | `true` | Enable REST dispatcher. |
| `plugin.kraken.ws_enabled` | BOOL | `false` | Enable WebSocket reader. |
| `plugin.kraken.ws_reconnect_ms` | UINT32 | `2000` | Initial WebSocket reconnect backoff. |
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
set kv plugin.kraken.creds.api_key <api-key-string>
set kv plugin.kraken.creds.private_key <base64-secret>
```

`kraken_apikey_configured()` returns true iff both KVs are non-empty
AND `creds.private_key` base64-decoded cleanly. The cached decoded
secret is invalidated transparently on any KV-edit — no daemon
restart needed.

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
  request signing.
- `json-c` — response parsing (added in KR-4).

## Consumer Access Shapes

Consumers (whenmoon, future strategy plugins) go through the
`feature_exchange` abstraction (`exchange_api.h`) — not through this
plugin's `kraken_api.h` shims directly. The dlsym surface here is
for plugin-internal exports + the rare consumer that needs Kraken-
specific behaviour (e.g. a Kraken-only admin command).

## Do Not

- Do not register `cmd_register` / `cmd_unregister` calls here; this
  is a service plugin.
- Do not call this plugin's symbols directly from non-kraken TUs —
  use the dlsym shims in `kraken_api.h` or, preferably, the generic
  `exchange_*_async` surface in `exchange_api.h`.
- Do not bypass the `creds`-segment secret tier by reading KV
  outside an admin context — secret-tier reads return
  `KV_REDACTED_VALUE` and the signer FAILs cleanly.
