# CLAM Context Registry

CLAM is botmanager's internal log/event bus. Every line emitted via
`clam(sev, ctx, fmt, ...)` carries a **context string** (≤60 chars,
per `CLAM_CTX_SZ`) and a **message body** (≤1000 chars, per
`CLAM_MSG_SZ`). Subscribers register a POSIX ERE that is matched
against `"<context> <body>"`; matching subscribers receive the
event via their callback. FATAL messages bypass the filter.

This file is the authoritative registry of every context string
emitted in the codebase — a map of where to point a subscriber
regex to watch a given subsystem. **Convention:** whenever a commit
introduces a new CLAM context, it updates this file in the same
commit, so the registry never drifts from the source.

## Subscriber API

```c
#include "clam.h"

static void my_cb(const clam_msg_t *m) {
  // m->sev, m->context[CLAM_CTX_SZ], m->msg[CLAM_MSG_SZ]
}

// Receive INFO + WARN + FATAL; only contexts starting "mw.coinbase.":
clam_subscribe("hotbot-coinbase", CLAM_INFO,
    "^mw\\.coinbase\\.", my_cb);
```

Pass `NULL` for the regex to receive every context. Use the
**second column** of the entries below as the basis for your
regex (the literal context portion, before any `<placeholder>`).

## Core (`core/`)

| Context | Source | Description |
|---|---|---|
| `bconf` | core/bconf.c | bot config IO |
| `bconf_exit` | core/bconf.c | bconf subsystem teardown |
| `bconf_init` | core/bconf.c | bconf subsystem init |
| `bot` | core/bot.c | bot lifecycle (generic) |
| `bot_add` | core/bot_cmd.c | `/bot add` command |
| `bot_bind` | core/bot_cmd.c | `/bot bind` command |
| `bot_bind_method` | core/bot.c | method-binding step on a bot |
| `bot_clear_userns` | core/bot.c | clearing a bot's userns binding |
| `bot_create` | core/bot.c | bot create path |
| `bot_del` | core/bot_cmd.c | `/bot del` command |
| `bot_destroy` | core/bot.c | bot destroy path |
| `bot_discover_user` | core/bot.c | per-bot user discovery |
| `bot_exit` | core/bot.c | bot subsystem teardown |
| `bot_init` | core/bot.c | bot subsystem init |
| `bot_msg` | core/bot.c | inbound message routed to a bot |
| `bot_register_driver_kv` | core/bot.c | per-driver KV registration |
| `bot_register_method_kv` | core/bot.c | per-method KV registration |
| `bot_restore` | core/bot.c | bot restore from persistent store |
| `bot_say` | core/bot_cmd.c | `/say` command (bot emits to a channel) |
| `bot_session_auth` | core/bot.c | bot session auth events |
| `bot_session_clear` | core/bot.c | clearing a bot session |
| `bot_session_create` | core/bot.c | creating a bot session |
| `bot_session_reaper` | core/bot.c | session reaper task |
| `bot_session_remove` | core/bot.c | session remove path |
| `bot_set_userns` | core/bot.c | binding a userns to a bot |
| `bot_start` | core/bot.c | bot start lifecycle hook |
| `bot_stop` | core/bot.c | bot stop lifecycle hook |
| `bot_unbind` | core/bot_cmd.c | `/bot unbind` command |
| `bot_unbind_method` | core/bot.c | method-unbinding step on a bot |
| `botmanctl` | core/botmanctl.c | botmanctl protocol driver |
| `clam_exit` | core/clam.c | clam subsystem teardown |
| `clam_init` | core/clam.c | clam subsystem init |
| `clam_subscribe` | core/clam.c | clam subscribe surface |
| `clam_unsubscribe` | core/clam.c | clam unsubscribe surface |
| `cmd_bot_cleanup` | core/cmd.c | per-bot command-tree cleanup |
| `cmd_dispatch` | core/cmd.c | command dispatch (resolution + walk) |
| `cmd_dispatch_as` | core/cmd.c | dispatch on behalf of identity |
| `cmd_dispatch_resolved` | core/cmd.c | dispatch from pre-resolved node (NB1) |
| `cmd_exit` | core/cmd.c | command subsystem teardown |
| `cmd_init` | core/cmd.c | command subsystem init |
| `cmd_register` | core/cmd.c | command-tree registration |
| `cmd_set_prefix` | core/cmd.c | command prefix change |
| `cmd_unregister` | core/cmd.c | command-tree unregistration |
| `curl` | core/curl.c | libcurl pool + sync/async events |
| `db` | core/db.c | DB subsystem events (driver-agnostic) |
| `db_exit` | core/db.c | DB subsystem teardown |
| `db_init` | core/db.c | DB subsystem init |
| `db_query` | core/db.c | synchronous DB query path |
| `db_query_async` | core/db.c | asynchronous DB query path |
| `db_query_stream` | core/db.c | streaming (row-callback) DB read path |
| `kv_claim_orphans` | core/kv.c | materialize schema-less DB KV rows |
| `kv_delete` | core/kv.c | single-key KV delete (admin `/db delete kv`) |
| `kv_delete_prefix` | core/kv.c | bulk KV delete |
| `kv_exit` | core/kv.c | KV subsystem teardown |
| `kv_flush` | core/kv.c | KV flush to DB |
| `kv_init` | core/kv.c | KV subsystem init |
| `kv_load` | core/kv.c | KV load from DB |
| `kv_register` | core/kv.c | KV registration |
| `kv_register_nl` | core/kv.c | NL responder attach |
| `main` | core/main.c | daemon entry / startup banner |
| `method_exit` | core/method.c | method subsystem teardown |
| `method_init` | core/method.c | method subsystem init |
| `method_register` | core/method.c | method driver registration |
| `method_send` | core/method.c | method send (text) |
| `method_send_emote` | core/method.c | method send (emote) |
| `method_set_state` | core/method.c | method state transition |
| `method_subscribe` | core/method.c | method subscribe |
| `method_unregister` | core/method.c | method driver unregistration |
| `method_unsubscribe` | core/method.c | method unsubscribe |
| `plugin` | core/plugin.c | plugin discovery + load + lifecycle |
| `pool` | core/pool.c | worker pool runtime events |
| `pool_exit` | core/pool.c | worker pool teardown |
| `pool_init` | core/pool.c | worker pool init |
| `pool_shutdown` | core/pool.c | worker pool shutdown drain |
| `proc` | core/proc.c | child-process spawn / wait (`PROC_CTX`) |
| `resolve` | core/resolve.c | identity resolution path |
| `sig_exit` | core/sig.c | signal subsystem teardown |
| `sig_init` | core/sig.c | signal subsystem init |
| `sock` | core/sock.c | core socket helpers |
| `task_add_persist` | core/task.c | persistent task registration |
| `task_cancel` | core/task.c | task cancel surface |
| `task_exit` | core/task.c | task scheduler teardown |
| `task_finish` | core/task.c | task completion path |
| `task_init` | core/task.c | task scheduler init |
| `task_submit` | core/task.c | task submit surface |
| `urlgrabber` | plugins/feature/urlgrabber/*.c | URL-title watcher (detect, fetch, announce) |
| `userns` | core/userns_util.c | userns helpers |
| `userns_auth` | core/userns.c | userns auth events |
| `userns_delete` | core/userns.c | userns delete |
| `userns_exit` | core/userns.c | userns subsystem teardown |
| `userns_get` | core/userns.c | userns lookup |
| `userns_group_create` | core/userns.c | group create |
| `userns_group_create_desc` | core/userns.c | group description set on create |
| `userns_group_delete` | core/userns.c | group delete |
| `userns_group_set_desc` | core/userns.c | group description set |
| `userns_init` | core/userns.c | userns subsystem init |
| `userns_member_add` | core/userns.c | group member add |
| `userns_member_remove` | core/userns.c | group member remove |
| `userns_member_set_level` | core/userns.c | group member level set |
| `userns_user_add_mfa` | core/userns_mfa.c | per-user MFA enroll |
| `userns_user_create` | core/userns.c | user create |
| `userns_user_create_nopass` | core/userns.c | user create (no password) |
| `userns_user_delete` | core/userns.c | user delete |
| `userns_user_remove_mfa` | core/userns_mfa.c | per-user MFA unenroll |
| `userns_user_reset_password` | core/userns.c | password reset |
| `userns_user_set_password` | core/userns.c | password set |
| `util` | core/util.c | shared utility helpers |

## Cross-plugin contracts (`include/`)

| Context | Source | Description |
|---|---|---|
| `stockquote` | include/stockquote.h | provider-neutral stock-quote contract; capability-resolution shims (no provider loaded / missing symbol) |

## Service plugins (`plugins/service/`)

| Context | Source | Description |
|---|---|---|
| `coinbase` | plugins/service/coinbase/ | top-level coinbase plugin (`CB_CTX`) |
| `coinmarketcap` | plugins/service/coinmarketcap/coinmarketcap.c | CMC client (`CMC_CTX`) |
| `gemini` | plugins/service/gemini/gemini_sign.c | top-level gemini plugin (`GEM_CTX`) |
| `gemini.ws` | plugins/service/gemini/gemini_ws_channels.c | gemini WS transport (generic) |
| `gemini.ws.md` | plugins/service/gemini/gemini_ws_channels.c | gemini WS Market Data v2 |
| `gemini.ws.oe` | plugins/service/gemini/gemini_ws_channels.c | gemini WS Order Events |
| `kraken` | plugins/service/kraken/kraken_sign.c | top-level kraken plugin (`KR_CTX`) |
| `openweather` | plugins/service/openweather/openweather.c | OpenWeather API (`OW_CTX`) |
| `searxng` | plugins/service/searxng/searxng.c | SearXNG service client (`SXNG_CTX`) |
| `yahoofinance` | plugins/service/yahoofinance/yahoofinance.c | Yahoo Finance stock-quote provider (`YF_CTX`) |

## Command-surface plugins (`plugins/cmd/`)

| Context | Source | Description |
|---|---|---|
| `ask` | plugins/cmd/ask/ask_cmd.c | `!ask` one-shot LLM command (`ASK_CMD_CTX`) |
| `claude` | plugins/cmd/claude/claude.c | `/claude` bridge command (`CLAUDE_CTX`) |
| `crypto` | plugins/cmd/crypto/crypto.c | `/crypto` price command (`CRYPTO_CTX`) |
| `imagine` | plugins/cmd/imagine/imagine_cmd.c | `!imagine` text-to-image command (`IMG_CMD_CTX`) |
| `searxng` | plugins/cmd/searxng/searxng_cmd.c | `/searxng` command (`SEARXNG_CMD_CTX`) |
| `weather` | plugins/cmd/weather/weather.c | `/weather` command (`WEATHER_CTX`) |

## Method plugins (`plugins/method/`)

| Context | Source | Description |
|---|---|---|
| `autoidentify` | plugins/method/command/command.c | passive identify on session |
| `chatbot` | plugins/method/chat/volunteer.c | chatbot volunteer-to-speak path |
| `deauth` | plugins/method/command/command.c | session deauth |
| `dossier` | plugins/method/chat/dossier.c | dossier subsystem |
| `extract` | plugins/method/chat/extract.c | LLM-driven fact extraction |
| `identify` | plugins/method/command/command.c | `/identify` session auth |
| `interject` | plugins/method/chat/volunteer.c | chatbot interjection scoring |
| `irc` | plugins/protocol/irc/irc_protocol.c | IRC protocol driver |
| `memory` | plugins/method/chat/memory_rag.c | chat memory subsystem + RAG |
| `nl_bridge` | plugins/method/chat/reply.c | natural-language bridge surface |
| `nl_observe` | plugins/method/chat/nl_observe.c | post-dispatch chat observer (`OBS_CTX`) |
| `register` | plugins/method/command/command.c | `/register` user creation |
| `vision` | plugins/method/chat/vision.c | image-intent path |

## Feature plugins (`plugins/feature/`)

| Context | Source | Description |
|---|---|---|
| `acquire` | plugins/inference/acquire_digest.c | autonomous knowledge acquire (`ACQUIRE_CTX`) |
| `exchange` | plugins/feature/exchange/ | feature_exchange abstraction (`EXCHANGE_CTX`) |
| `inference` | plugins/inference/inference.c | inference plugin top-level (`INFERENCE_CTX`) |
| `knowledge` | plugins/inference/knowledge_file.c | knowledge corpus subsystem |
| `llm` | plugins/inference/llm_cmd.c | `/llm` command surface |
| `strategy.example_sma_cross` | plugins/feature/whenmoon/strategy/example_sma_cross/ | example SMA strategy log (`ESC_LOG_CTX`) |
| `strategy.juggernaut` | plugins/feature/whenmoon/strategy/juggernaut/ | juggernaut strategy log (`JUG_LOG_CTX`) |
| `strategy.mako` | plugins/feature/whenmoon/strategy/mako/ | mako strategy log (`MAKO_LOG_CTX`) |
| `strategy.testing` | plugins/feature/whenmoon/strategy/testing/ | testing strategy log (`TST_LOG_CTX`) |
| `userquote` | plugins/feature/userquote/ | quote book: schema, add/recall/del, migration (`UQ_CTX`) |
| `whenmoon` | plugins/feature/whenmoon/ | top-level whenmoon (`WHENMOON_CTX`) |
| `whenmoon mw` | plugins/feature/whenmoon/mw.c | marketwatch subsystem op log (`MW_CTX`) |
| `whenmoon.backtest` | plugins/feature/whenmoon/backtest.c | backtest engine (`WM_BT_CTX`) |
| `whenmoon.bt.report` | plugins/feature/whenmoon/wm_bt_report.c | backtest report emit warnings (`WM_BT_REPORT_CTX`) |
| `whenmoon.dl` | plugins/feature/whenmoon/dl_coverage.c | download / job-table events (`WM_DL_CTX`) |
| `whenmoon.live` | plugins/feature/whenmoon/live.c | live-trading runtime (`WM_LIVE_CTX`) |
| `whenmoon.sweep` | plugins/feature/whenmoon/sweep.c | strategy sweep runner (`WM_SWEEP_CTX`) |

## Marketwatch event topics — `mw.<exch>.<event>.<id>`

These are emitted at `CLAM_INFO` with a single-line JSON body in
the message. Subscribe with regexes anchored at `^mw\.`. The
exchange name uses canonical lowercase form (`coinbase`, `kraken`,
`gemini`); product ids use canonical hyphenated form (`BTC-USD`,
not Kraken's `XXBTZUSD` or Gemini's `btcusd`). Every body field
named `ts` is wall-clock epoch milliseconds (via `wm_now_ms()`),
not the monotonic clock.

### Price-move detectors (MW-3 / MW-4)

| Context pattern | When emitted | Body fields |
|---|---|---|
| `mw.<exch>.hot.<id>` | Pair entered HOT state (one or more signals crossed). | `ts, exch, id, price, pct_24h, vel_pct, vel_window_min, vol_z, hi_24h, lo_24h, vol_24h_q, trigger, state="hot"` |
| `mw.<exch>.cool.<id>` | Pair returned to baseline (post-hysteresis + cooldown). | Same shape; `trigger=""`, `state="cool"`. |
| `mw.<exch>.upd.<id>` | Pair still HOT; throttled re-emit (interval = `upd_throttle_sec`). | Same shape; `state="upd"`. |

`vol_z` is the rolling-mean+stdev z-score of the just-pushed
`vol_24h_quote` against the per-pair ring history (excluding the
just-pushed entry). One-sided (positive z only). Renders as `null`
when the exchange does not ship `vol_24h_quote` (Gemini pricefeed)
or the ring holds fewer than `MW_VOL_Z_MIN_SAMPLES` finite samples.

#### Trigger token values

`trigger` is a `+`-joined string built from the bitset of signals
that fired on the emitting tick. Possible tokens:

- `pct_24h` — `|pct_24h|` crossed the `pct_24h_thresh_x100` threshold.
- `velocity` — short-window % change crossed `vel_pct_thresh_x100`.
- `brk_hi` — this tick set a new 24h high.
- `brk_lo` — this tick set a new 24h low.
- `vol_z` — positive volume z-score crossed `vol_z_thresh_x100`.

### Lifecycle detectors (MW-5)

Pure edge triggers — no state machine, no hysteresis, no
cooldown — driven by per-pair `last_seen_tick` comparison against
a monotonic per-exchange tick counter. Emits on the **second** tick
after a listing/status change is observed; the bootstrap tick on
enable suppresses all three event types (every pair would
otherwise emit `add`). A disable + re-enable cycle re-triggers the
bootstrap path (`tick_id` resets to 0 on disable).

| Context pattern | When emitted | Body fields |
|---|---|---|
| `mw.<exch>.add.<id>` | New listing appeared (slot inserted on a non-bootstrap tick). | `ts, exch, id, price, status, pct_24h, vol_24h_q, state="add"` |
| `mw.<exch>.rem.<id>` | Listing disappeared (populated slot not refreshed this tick). | `ts, exch, id, last_price, last_status, last_seen_polls, state="rem"` |
| `mw.<exch>.stat.<id>` | Per-pair status flipped (`status` differs from the previous observation). | `ts, exch, id, price, prev_status, new_status, state="stat"` |

`status`, `prev_status`, `new_status`, `last_status` use the
canonical token set from `exchange_ticker_status_t`: `online`,
`offline`, `limit_only`, `post_only`, `unknown`.

`last_price` reads from the newest entry in the pair's ring at
the time of removal (the pair's last successful tick). It renders
`null` when the ring was empty (vanishingly rare — would require
removal on the very tick the pair was inserted, which is
suppressed by bootstrap).

`last_seen_polls` is the count of consecutive missed ticks at
which the REM fired — typically `1`, because the rem-sweep
tombstones a slot on the first missed tick.

If `n == 0` for a tick but the previous tick observed ≥100 pairs,
the rem-sweep is **skipped** and a single `WARN` is logged instead
(treated as a likely API outage rather than a mass-delisting).

### Example subscriber: IRC channel announcer

```c
// Receive all marketwatch HOT events from any exchange; format
// for an arbitrary IRC channel.
clam_subscribe("hotbot", CLAM_INFO, "^mw\\.[^.]+\\.hot\\.",
    my_irc_emit_cb);

// Subscribe to lifecycle events (listings) on a single exchange:
clam_subscribe("listings-kraken", CLAM_INFO,
    "^mw\\.kraken\\.(add|rem|stat)\\.", my_listing_cb);
```
