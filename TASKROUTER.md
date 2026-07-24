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
| **Use a CLI tool (`botmanctl`, `ircspy`, `ircspyctl`)** | `tools/AGENTS.md` |
| **Fresh-start the daemon** | `scripts/AGENTS.md` |
| Add a user command (command-surface plugin) | `CMD.md` + `PLUGIN.md` §Layer Rules (placement decision-tree) |
| Add or work on a misc toy / novelty command (`!math`, `!8ball`) | `plugins/misc/AGENTS.md` |
| Add or modify a plugin layering rule | `PLUGIN.md` §Layer Rules + `AGENTS.md` §Plugin Layers |
| **File a new plugin (which folder?)** | `PLUGIN.md` §Layer Rules (placement decision-tree) |
| Add a service plugin (external API) | `PLUGIN.md` + `plugins/service/AGENTS.md` |
| Add/modify a protocol driver (IRC, ...) | `PLUGIN.md` + `plugins/protocol/AGENTS.md` + `include/method.h` |
| Add/modify a method driver (text, voice, ...) | `PLUGIN.md` + `plugins/method/AGENTS.md` + `include/bot.h` |
| Add/modify a feature plugin (whenmoon, weather, crypto, stock, ...) | `PLUGIN.md` + `plugins/feature/AGENTS.md` + `include/bot.h` |
| Add/modify an extension subsystem (inference) | `PLUGIN.md` + `plugins/extension/AGENTS.md` |
| Write, iterate, or backtest a trading strategy (cp1, mako, riptide, ...) | `plugins/feature/whenmoon/strategy/AGENTS.md` |
| Run or interpret a backtest / sweep (the iteration loop) | `plugins/feature/whenmoon/strategy/AGENTS.md` §iteration loop |
| Add/modify an exchange driver (coinbase, kraken, ...) | `PLUGIN.md` + `plugins/service/AGENTS.md` + `plugins/feature/exchange/AGENTS.md` |
| Add/modify a DB driver | `PLUGIN.md` + `plugins/db/AGENTS.md` + `include/db.h` |
| Work on a core subsystem | `core/AGENTS.md` + relevant `include/*.h` |
| Add a log/event context (`clam()`) or subscribe to one | `CLAM.md` |
| Work on the inference plugin C code (LLM client, knowledge store, acquisition engine) | `plugins/extension/inference/AGENTS.md` + `plugins/extension/inference/engine/inference.h` |
| Use the LLM client (chat/embed, streaming) | `plugins/extension/inference/LLM.md` + `plugins/extension/inference/engine/inference.h` |
| Use the memory subsystem (facts, conversation log, RAG) | `plugins/method/text/MEMSTORE.md` + `plugins/method/text/memory.h` |
| Use the knowledge store (per-persona corpus RAG) | `plugins/extension/inference/KNOWLEDGE.md` + `plugins/extension/inference/engine/inference.h` |
| Use the acquisition engine (autonomous + reactive knowledge learning) | `plugins/extension/inference/ACQUIRE.md` + `plugins/extension/inference/engine/inference.h` |
| Work on the text method (command dispatch + LLM conversation) | `plugins/method/text/AGENTS.md` + `plugins/method/text/CHATBOT.md` |
| Work on command dispatch / the identity + auth surface (`identify`, `deauth`, `register`, `id`) | `plugins/method/text/dispatch.h` + `plugins/method/text/AGENTS.md` §The two halves |
| Turn conversation on or off for a bot | `plugins/method/text/AGENTS.md` §The two halves (`bot.<name>.behavior.chat.enabled`) |
| Add/modify conversational admin commands (`/dossier`, `/bot … hush`, `/show bot …`) | `plugins/method/text/AGENTS.md` + `plugins/method/text/CHATBOT.md` §Admin commands |
| Stand up a new text bot instance | `plugins/method/text/CHATBOT.md` §Creating + `scripts/AGENTS.md` §freshstart.sh |
| Add/modify an output contract | `personalities/AGENTS.md` §contracts |
| Work on the `/claude` bridge / daemon-restart flow | `plugins/extension/inference/claude/AGENTS.md` + `scripts/botman-restart.sh` |
| Work on `!imagine` (text-to-image) or the `image` LLM kind | `plugins/extension/inference/imagine_zimage/` (the loaded variant) + `plugins/extension/inference/imagine/imagine_cmd.c` (dormant, inference-backed) + `plugins/extension/inference/LLM.md` |
| Work on GIF search (`!giphy`) | `plugins/service/giphy/` — one leaf-service plugin: `giphy.c` (provider) + `giphy_cmd.c` (command surface), contract in `giphy_api.h` |
| Work on stock/equity/fund/commodity quotes (`!stock`) | `TODO.md` §STOCK-1..5 + `include/stockquote.h` (contract) + `plugins/service/yahoofinance/` (provider) + `plugins/feature/stock/` (command) |
| Work on video-game info (`!rawg`) | `plugins/service/rawg/` — one leaf-service plugin: `rawg.c` (provider) + `rawg_cmd.c` (command surface), contract in `rawg_api.h` |
| Work on movie / TV / actor info (`!tmdb`) | `plugins/service/tmdb/` — one leaf-service plugin: `tmdb.c` (provider) + `tmdb_cmd.c` (command surface), contract in `tmdb_api.h` |
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
