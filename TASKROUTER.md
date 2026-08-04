## TASKROUTER.md — Go to your task and minimize input token spend

This is the project's **single routing index**: each task maps to the
smallest set of docs that lets you execute it. When you add or move a
module, folder, feature, or doc, update the relevant row here **in the
same commit** (per `AGENTS.md §MUST`). Keep rows task-shaped ("I want
to X") and point at the per-directory `AGENTS.md` leaf rather than
listing every file — the leaf carries the detail and is maintained
alongside its code.

| Your Task | Read These Files |
|-----------|------------------|
| **Get oriented in a fresh context (project briefing)** | `PRIMER.md` |
| **Find out what is queued, or whether something already shipped** | `TODO.md` §Where the work is (queue state per plugin, one table) → then the plugin's own `TODO.md`. For "did X ship?" read the `ACHIEVED.md` **ledger** — root for cross-cutting, `<plugin>/ACHIEVED.md` otherwise. Never grep the shards for this |
| **Record shipped work / read a chunk's provenance** | Add a row to the nearest `ACHIEVED.md` ledger (chunk · date · **commit SHA** · one-clause outcome · shard). Verbose notes are optional and go in `<dir>/achieved/YYYY-MM.md`; read one with `grep -n '^## <CHUNK-ID>' <dir>/achieved/*.md` and never open a shard whole |
| **Use a CLI tool (`botmanctl`, `ircspy`, `ircspyctl`)** | `tools/AGENTS.md` |
| **Fresh-start the daemon** | `scripts/AGENTS.md` |
| Work on the factory automation (steward, digest, research/family lanes, weekend charter) | `scripts/factory/<lane>.prompt.md` (the operative instructions) + its `scripts/factory/*.service`/`*.timer` pair — **units must be re-installed to `~/.config/systemd/user/` after any edit**. ⚠ Every timer is stopped and deleted (operator 2026-07-28); FACTORY-3/4 are parked in `BACKBURNER.md` and re-authorising unattended runs is an operator decision |
| Add a user command (command-surface plugin) | `CMD.md` + `PLUGIN.md` §Layer Rules (placement decision-tree) |
| Add or work on a misc toy / novelty command (`!math`, `!8ball`) | `plugins/misc/AGENTS.md` |
| Add or modify a plugin layering rule | `PLUGIN.md` §Layer Rules + `AGENTS.md` §Plugin Layers |
| Work on plugin lifecycle / hot-reload, or make a plugin's `deinit()` mirror its `init()` | **`PLUGIN.md §Lifecycle Contract` — authoritative, read it rather than any summary** + `core/plugin.c` (`plugin_audit`, `plugin_reclaim`, `plugin_quiesce`, `plugin_reload`) + `core/cmd.c` (`cmd_unregister_path`). Worklist: `/plugin audit <name>`; daemon-free gate: `scripts/plugin_audit.sh`. The one trap the contract cannot check for you: **a pointer another plugin handed you is invisible to every audit** — register a `plugin_unmap_notify_register()` listener or it kills the daemon later |
| **File a new plugin (which folder?)** | `PLUGIN.md` §Layer Rules (placement decision-tree) |
| Add a service plugin (external API) | `PLUGIN.md` + `plugins/service/AGENTS.md` |
| Wire a consumer to a service's `*_async()` API, or write one | **`PLUGIN.md §Async failure semantics` first** — two opposite `FAIL` conventions live in this tree, and picking the wrong one double-frees the consumer's closure and kills the daemon with no FATAL line |
| Add/modify a protocol driver (IRC, ...) | `PLUGIN.md` + `plugins/protocol/AGENTS.md` + `include/method.h` |
| Add/modify a method driver (text, voice, ...) | `PLUGIN.md` + `plugins/method/AGENTS.md` + `include/bot.h` |
| Add/modify a feature plugin (weather, crypto, stock, urlgrabber, userquote, ...) | `PLUGIN.md` + `plugins/feature/AGENTS.md` + `include/bot.h` |
| Add/modify an extension subsystem (inference, whenmoon) | `PLUGIN.md` + `plugins/extension/AGENTS.md` |
| Work on the whenmoon plugin C code (markets, candles, backtester, treasuries) | `plugins/extension/whenmoon/AGENTS.md` |
| Act as CFO / touch a treasury or the live disc fund | `plugins/extension/whenmoon/CFO.md` (read FIRST — it is the office) |
| Write, iterate, or backtest a trading strategy (cp1, mako, riptide, ...) | `plugins/extension/whenmoon/strategy/AGENTS.md` |
| Run or interpret a backtest / sweep (the iteration loop) | `plugins/extension/whenmoon/strategy/AGENTS.md` §iteration loop |
| Add/modify an exchange driver (coinbase, gemini, kraken, ...) | `PLUGIN.md` + `plugins/service/AGENTS.md` + `plugins/feature/exchange/AGENTS.md` + that driver's own `plugins/service/<name>/README.md` (auth + protocol specifics) |
| Add/modify a DB driver | `PLUGIN.md` + `plugins/db/AGENTS.md` + `include/db.h` |
| Work on a core subsystem | `core/AGENTS.md` + relevant `include/*.h` |
| Add a log/event context (`clam()`) or subscribe to one | `CLAM.md` |
| Work on the inference plugin C code (LLM client, knowledge store, acquisition engine) | `plugins/extension/inference/AGENTS.md` + `plugins/extension/inference/engine/inference.h` |
| Use the LLM client (chat/embed/image/**stt**/**tts**, streaming) | `plugins/extension/inference/LLM.md` + `plugins/extension/inference/engine/inference.h`. The speech kinds are the two odd ones: `stt` sends the engine's only non-JSON body (multipart WAV to `<base>/inference`, so its service URL carries **no** `/v1`) and `tts` receives a binary one (`audio/*` or it is treated as an error) |
| Use the memory subsystem (facts, conversation log, RAG) | `plugins/method/text/MEMSTORE.md` + `plugins/method/text/memory.h` |
| Use the knowledge store (per-persona corpus RAG) | `plugins/extension/inference/KNOWLEDGE.md` + `plugins/extension/inference/engine/inference.h` |
| Use the acquisition engine (autonomous + reactive knowledge learning) | `plugins/extension/inference/ACQUIRE.md` + `plugins/extension/inference/engine/inference.h` |
| Work on the text method (command dispatch + LLM conversation) | `plugins/method/text/AGENTS.md` + `plugins/method/text/CHATBOT.md` |
| Work on command dispatch / the identity + auth surface (`identify`, `deauth`, `register`, `id`) | `plugins/method/text/dispatch.h` + `plugins/method/text/AGENTS.md` §The two halves |
| Turn conversation on or off for a bot | `plugins/method/text/AGENTS.md` §The two halves (`bot.<name>.behavior.chat.enabled`) |
| Add/modify conversational admin commands (`/dossier`, `/bot … hush`, `/show bot …`) | `plugins/method/text/AGENTS.md` + `plugins/method/text/CHATBOT.md` §Admin commands |
| Stand up a new text bot instance | `plugins/method/text/CHATBOT.md` §Creating + `scripts/AGENTS.md` §freshstart.sh |
| Add/modify an output contract | `personalities/AGENTS.md` §contracts |
| Make a bot's output **bold or coloured** (`!ask` answers, chat replies, any future emitter) | `personalities/AGENTS.md` §Bold and colour (the vocabulary + what a contract should say about it) + `core/colors.c` `color_markup_translate()` — the shared `**bold**` / `<red>…</red>` → `colors.h` marker rewrite, applied by `ask_cmd.c` and `reply.c:send_line_marked`. ⚠ It is **unconditional** on the chat path: a contract that never mentions markup still gets the model's unprompted Markdown bold rendered instead of leaked. `prompts/ask_default.txt` is the reference wording |
| Work on the `/claude` bridge / daemon-restart flow | `plugins/extension/inference/claude/AGENTS.md` + `scripts/botman-restart.sh` |
| Work on `!imagine` (text-to-image) or the `image` LLM kind | `plugins/extension/inference/imagine_zimage/` (the loaded variant) + `plugins/extension/inference/imagine/imagine_cmd.c` (dormant, inference-backed) + `plugins/extension/inference/LLM.md` |
| Touch the `zimage-service/` backend behind `!imagine` | `zimage-service/README.md` → `QUICKSTART.md` / `CONFIGURATION.md` / `API_OPTIONS.md` (13 docs, gitignored, out-of-tree Python). ⚠ **Temporary by design** — it exists only because ollama's imagegen runner is still MLX-only on Linux/CUDA (re-verified on ollama 0.32.5, 2026-07-30). It retires the moment ollama ships a CUDA imagegen runner, at which point `!imagine` flips to the dormant b64 path above. Invest accordingly. It also **holds ~23.6 GB of the 4090 while running**, so any other GPU work must account for that |
| Work on a **leaf service with a command surface** (`!giphy`, `!rawg`, `!tmdb`, `!wotd`/`!dict`, and every future one) | `plugins/service/<name>/` — one plugin, always the same three files: `<name>.c` (provider) + `<name>_cmd.c` (command surface) + `<name>_api.h` (contract). The pattern and when a service may carry commands at all: `plugins/service/AGENTS.md` §Leaf services with a command surface + `PLUGIN.md §Layer Rules` Rule 1 |
| Work on stock/equity/fund/commodity quotes (`!stock`) | `include/stockquote.h` (contract) + `plugins/service/yahoofinance/` (provider) + `plugins/feature/stock/` (command). ⚠ The Yahoo v7 enrichment tier is built but **shipped switched off** — `getcrumb` 429s this public IP; the wait-then-re-test procedure is `TODO.md §STOCK-4-RECHECK` and it is the only open item there |
| Work on per-userns symbol lists (`!stock`/`!crypto` `--list`/`--add`/`--del`, `@name`) | `plugins/feature/stock/stock_lists.c` + `plugins/feature/crypto/crypto_lists.c` — a list is **typed** (one table per class, no `class` column); do not build a shared lists service, and do not re-litigate it (rejected alternatives are in `achieved/2026-07.md §LIST-1 + LIST-2`) |
| Work on `!note` (leave a message for an absent user, delivered when they're next seen) | `plugins/feature/note/AGENTS.md` |
| **Work on the Reachy Mini robot** (body, senses, voice embodiment) | `plugins/protocol/reachy/TODO.md` — read its §C charter FIRST (measured hardware facts, ratified decisions, the do-not-re-attempt list, and the operator install gates), then the chunk you are executing. Code today: `plugins/service/reachyapi/` (REST mechanism + `reachyapi_api.h`), `plugins/feature/reachycmd/` (`!reachy list|do|**say**|wake|sleep|volume|track|wobble`, `show reachy` — `say` is synthesize→upload→play over the engine's `tts` kind, voiced by `plugin.reachycmd.tts_model|tts_voice|tts_speed`), `plugins/protocol/reachy/` (**the method driver** — `/bot addmethod <bot> reachy` gives a text bot ears: an `reachy_ears_<bot>` persist thread long-polls earbridge, hands each utterance to the engine's `stt` kind, gates it on `bot.<b>.reachy.attention.mode` and `method_deliver`s it into the ordinary chat pipeline; `send()` is still a stub until RCH-7 gives it a mouth), and `senses/` — the out-of-daemon speech hosts, **both live**: `senses/whisper/` (unit + verifying model fetcher for the packaged whisper.cpp STT server on :8004) , `senses/kokorod/` (**ours**, a standalone C Kokoro TTS daemon on :8003 — `POST /v1/audio/speech` → 24 kHz **stereo** WAV peak-normalised to −3 dBFS; both of those output properties are mandatory, see its ledger). Nothing under `senses/` is a plugin or links core; kokorod is meson-guarded on `dependency('sherpa-onnx')` so a tree without the package still builds. Model weights are gitignored. Third host: `senses/earbridge/` — **ours, C, and it runs ON the robot's CM4**, not here (plain Makefile + `deploy.sh`, built remotely; raw mic PCM never leaves the robot over REST). It serves utterances at `http://reachy…:8090/utt?after=&wait=` as 16 kHz mono WAV with `X-Utt-*` headers, and gates each one on the loudest 250 ms window (`--floor-db`, default −35 dBFS) because the XVF3800's speech flag fires on the CM4's own cooling fan and whisper transcribes fan noise into fluent English — **tune that flag by watching `quiet_discards` against `utterances` in `/health`; it is a knob, not a constant, because the fan drifts**. **When the question is "what is the robot actually hearing?", run `scripts/reachy_mic_probe.sh`** — it records raw, VAD-ungated audio off the same shared PCM and hands back a playable WAV plus a level timeline, which is the only way to tell a sensor problem from a transcription problem. ⚠ **Async replies never reach `botmanctl`** — `bctl_reply_target` is cleared the moment dispatch returns, so verify every async robot command over IRC in `#botman` (`tools/ircspy` + `tools/ircspyctl`); botmanctl still proves the *action* (watch `GET /api/move/running` on the robot) |
| Work on the attack duelling game (`!attack`, `!heal`, `!defer`, `show attack`; character sheets in `plugins/feature/attack/characters/`) | `plugins/feature/attack/AGENTS.md` |
| Work on the one-shot LLM command (`!ask`) | `plugins/extension/inference/ask/` + `plugins/extension/inference/LLM.md` |
| Work on the search commands (`!search`, `!image`, `!news`, `!video`, `!music`) | `plugins/extension/inference/search/searxng_cmd.c` (command surface) + `plugins/service/searxng/searxng_api.h` (mechanism) |
| Merge a command surface into its leaf service | `plugins/service/AGENTS.md` §Leaf services with a command surface + `PLUGIN.md §Layer Rules` Rule 1 |
| Work with the IRC protocol driver or its identity projection | `plugins/protocol/AGENTS.md` + `plugins/protocol/irc/AGENTS.md` + `plugins/protocol/irc/irc_identity.h` |
| Ingest or refresh a knowledge corpus | `plugins/extension/inference/KNOWLEDGE.md` + `scripts/AGENTS.md` §fetch_archwiki.sh |
| Work on the dossier subsystem / `/dossier` admin commands | `plugins/method/text/DOSSIER.md` + `plugins/method/text/dossier.h` |
| Work on LLM fact extraction (dossier_facts) | `plugins/method/text/FACT_EXTRACT.md` + `plugins/method/text/extract.h` |
| Edit or add a chat personality | `personalities/AGENTS.md` + `plugins/method/text/CHATBOT.md` |
| Build system changes | `BUILD.md` + root `meson.build` + relevant subdir `meson.build` |
| Understand overall architecture | `DESIGN.md` |
| Find a header's purpose | `include/AGENTS.md` |
