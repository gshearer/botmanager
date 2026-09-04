# personalities/

This directory holds the two data files the `chat` bot kind reads at
every reply: a **personality** (who the bot is) and an **output
contract** (how the bot must look on the wire). Both are plain-text
files with YAML-style frontmatter. No DB, no registry, no preload —
the chat driver resolves the path, reads the file, parses it, and
frees it once the reply is assembled. **Editing a file takes effect
on the next inbound message.**

```
personalities/
├── README.md             (this file)
├── AGENTS.md             deeper reference for authoring agents
├── <persona>.txt         one file per persona
└── contracts/
    ├── default.txt       human-passing output contract
    ├── support.txt       openly-a-bot output contract
    └── <contract>.txt    one file per contract
```

If you need the philosophy (two-axis authoring model, SKIP semantics,
nl_bridge subtleties, security notes, failure-mode catalog), read
`AGENTS.md`. This README is the **format reference and examples**.

---

## What's actually in a personality file?

Two things, stacked:

1. **Frontmatter** — a few `key: value` lines telling the bot system its
   *name*, a one-liner *description*, and (optionally) the *interests*
   that drive what the bot reads about in the background.
2. **Body** — free-form prose describing *who the bot is*. This becomes
   the system prompt the LLM sees on every reply.

That's it. No code, no compilation, no DB row to insert. You drop a
`.txt` file here, point a bot at it via one KV key, and the next inbound
message uses it.

The body shapes *voice and engagement*. The frontmatter's `interests:`
block shapes *what the bot has read about lately* — see the Topics
section below.

---

## Quick start

```sh
# 1. Drop a file.
cat > personalities/mybot.txt <<'EOF'
---
name: mybot
description: One-line summary of the character.
---
You are MyBot, a helpful channel regular ...
EOF

# 2. Point a bot at it.
tools/botmanctl set kv bot.<name>.personality mybot

# 3. List + verify.
tools/botmanctl show personalities
```

No restart needed. The next message the bot receives will use the new
file.

---

## File grammar (personality and contract)

Both file types share the same grammar: a **frontmatter block** (YAML-
flavored `key: value` lines) followed by a **body** (the LLM system
prompt).

```
---                       (optional leading fence)
key: value
key: value
---                       (closing fence OR a blank line)

body text — fed verbatim to the model as system prompt
```

Rules:

- UTF-8, plain text.
- Leading `---` is optional. The frontmatter ends at the first blank
  line *or* closing `---`.
- `# ...` lines inside frontmatter are treated as comments.
- Unknown keys are silently ignored (legacy-safe).
- Body starts after the terminator; leading blank lines are stripped.
- The body is NOT a template. No interpolation, no variables — the
  LLM sees exactly what's in the file.

### Stem = filename

Files are resolved by stem: `mybot.txt` → `mybot`. Keep stems to
`[a-z0-9_-]`. The `name:` field in the frontmatter is conventionally
the same as the stem but not enforced — the stem is what operators
bind to KV.

---

## Personality file

### Frontmatter fields

| Field         | Required | Type    | Notes                                       |
|---------------|----------|---------|---------------------------------------------|
| `name`        | yes      | string  | Display label. Usually matches the stem.    |
| `description` | yes      | string  | One-liner shown by `/show personalities`.   |
| `version`     | no       | int     | Defaults to 1. Bump when you rewrite.       |
| `interests`   | no       | JSON    | Topic-of-interest array (see below).        |

### Body

Free-form first-person system prompt. Establish:

- **Identity.** Name, nick, age, occupation, context.
- **Voice.** Register, sentence length, humor, when to be direct.
- **Engagement policy.** When to answer, when to volunteer, when to
  SKIP (on overheard chatter), how to handle direct address.
- **Constraints specific to the persona.** E.g. "never claim to be an
  AI" goes in the persona body, not the contract.

See `pacmanpundit.txt` and `lessclam.txt` for two different takes —
one quiet sysadmin, one openly-a-bot chat companion.

### Minimal example

```yaml
---
name: mybot
description: A laconic regular who answers questions about Rust and otherwise stays quiet.
---
You are MyBot, an experienced Rust developer who hangs out in this
channel because you like the problems that pass through it.

Voice: dry, lowercase-leaning, 1-3 sentences per reply. Answer first,
reasoning second.

Engagement: always answer when directly addressed (by nick, DM, or
`/me` aimed at you). Otherwise SKIP on overheard chatter unless you
have a real technical contribution.
```

---

## The `interests:` block

An optional JSON array of topic objects that tells the **acquisition
engine** what the bot cares about. The engine pulls in fresh material
about those topics in the background and stashes it in the bot's
*acquired corpus*. The bot then retrieves from that corpus on every
reply, the same way it retrieves from any other knowledge corpus
(like the Arch wiki). End result: the bot can casually mention things
it has "read" recently without anyone asking it to.

### The five keys

A topic is a thing the character follows, written the way a human would
say it. There are five keys, and only the first is required.

```yaml
interests:
  [
    {"topic": "cryptocurrency",
     "aka": ["bitcoin", "ethereum", "solana"],
     "for": ["doc"],
     "attention": "normal",
     "watch": ["https://www.coindesk.com/latest-crypto-news"]}
  ]
```

| Key | Required | Type | Default | What it is |
|---|---|---|---|---|
| `topic` | **yes** | string | — | The thing. It is the topic's name, the string a proactive search runs, **and** the first substring inbound chat is scanned against — one string doing all three. ≤ 63 chars. |
| `aka` | no | string[] | `[]` | Its other names. Triggers and alias search terms. `topic` + `aka` share 16 slots, ≤ 63 chars each. |
| `for` | no | string[] | `[]` | Nicks who care. When there is news, the bot prefers a channel one of them is in and addresses the line to whoever is there. Empty = address the room. Max 8. |
| `attention` | no | word or int | `normal` | How much of the bot's own initiative this topic gets: `off` / `low` / `normal` / `high` (0 / 25 / 50 / 100), or a bare `0`–`100`. |
| `watch` | no | string[] or object[] | `[]` | Feed URLs polled on their own schedule. A bare URL, or `{"url": …, "every": 1800}`. Max 8. |

Three advanced keys exist and are deliberately absent from the example:

| Key | Type | Default | What it is |
|---|---|---|---|
| `category` | string | `general` | SearXNG category: `general`, `images`, `news`, `videos`, `music`. Steers searches only; feeds ignore it. **If your topic is news, say so** — a news topic searching `general` returns front pages and reads as working. |
| `encyclopedic` | bool | `false` | Try Wikipedia before the search engine. Good for "what is X", bad for "what happened to X this week". |
| `max_sources` | int | `0` | Per-topic override for `acquire.max_sources_per_query`. `0` inherits; a topic may ask for fewer results, never more. |

**Any other key is named in a WARN and ignored** — a typo'd `"wach"`
tells you so rather than vanishing.

### How a topic actually fires (mental model)

A single topic drives **three independent paths** that all dump into the
same corpus. You don't need all three — pick what fits.

```
                          ┌─────────────────┐
chat line matches a name →│ reactive search │  → SXNG → fetch → digest → corpus
                          └─────────────────┘
                          ┌─────────────────┐
periodic tick          →  │ proactive search│  → SXNG → fetch → digest → corpus
                          └─────────────────┘
                          ┌─────────────────┐
per-feed cadence       →  │ feed dispatcher │  → fetch → digest → corpus
                          └─────────────────┘
```

| Path | Driven by | What it searches |
|---|---|---|
| **Reactive search** | Someone says `topic` or one of its `aka` names in chat | **What they said**, not the topic. The engine lifts the longest capitalised run near the hit — so `solana just shipped Firedancer` searches *Firedancer*. It falls back to `topic` when the line contained nothing more specific than the name that matched. |
| **Proactive search** | Per-bot tick (`acquire.tick_cadence_secs`, default 600 s), weighted random pick by `attention` | `topic`, verbatim. A scheduled run has no chat line to lift a subject from. |
| **Feed dispatcher** | Each `watch` entry's own `every` | Nothing — the URL is fetched verbatim. Names and queries are not used here. |

These paths are **independent**. A `watch` URL fires on its own schedule
whether or not anyone says anything. A name match fires a search whether
or not you have feeds.

⚠ **`attention: off` does not switch a topic off.** It means *"never go
looking"* — the proactive picker skips it. The reactive scan is not
weighted and still fires on any name match. There is no "poll but never
chase" setting; a topic's names are always live triggers.

### What "reactive" really means

Common misconception: *"I want the bot to fetch my URL when someone
mentions a name."* That is **not** what names + `watch` does. A name
match fires a SearXNG search. The `watch` URLs fetch on their own
cadence and populate the corpus. The bot answers about your topic on a
mention because *corpus retrieval* (RAG) surfaces the relevant chunk —
not because the name "called" your feed.

If your URL is being polled regularly, the corpus has fresh content,
and RAG does the rest naturally.

### A name is an unanchored substring

`topic` and every `aka` entry are matched as plain substrings,
case-insensitively, anywhere in a line. Nothing warns you about an
unfortunate one, because the parser cannot tell it from a deliberate
one:

- `"AI"` fires on *said*, *again*, *maintain*.
- `"away"` fires on *always*. `"home"` fires on *homework*.
- `"News"` fires on essentially every line containing the word.

Prefer a phrase: `"smart home"` over `"home"`, `"LLM inference"` over
`"AI"`.

### It must be valid JSON, NOT YAML

`json-c` parses this block. YAML pipe form (`|`) silently produces zero
topics. Keep it as a real JSON array:

```yaml
interests:
  [
    { ... },
    { ... }
  ]
```

> **Gotchas that silently kill the whole block:**
> - **A blank line inside the array.** The value runs from `interests:`
>   until a blank line or `---`, so a blank line between two objects
>   truncates the JSON mid-array and registers **nothing**. This is the
>   most common one by a distance.
> - Forgotten closing quote on a key — e.g. `"category: "news"` — breaks
>   that topic and often cascades to the whole array.
> - A trailing comma is *not* one of these: json-c accepts it. If the
>   block looks malformed to you and still parses, the failure is
>   semantic — a wrong key name, or a missing `topic`.
> - When in doubt, paste the block (without the `interests:` prefix)
>   into `python3 -m json.tool` to confirm it parses.

**Confirm it, don't assume it.** The block is not re-read per message —
it is handed to the engine at bind time. After editing it, run:

```sh
tools/botmanctl -- bot <name> refresh_prompts
```

It reports what it parsed on a line of its own: `interests: 7 topics
registered`, or `interests: DECLARED BUT NONE PARSED — check the JSON
block`, or that the bot has no `acquired_corpus` and registered nothing
at all.

### `for:` — addressing the people who care

`for` names people, and naming is all it does. When the topic ingests
something new, the volunteer path prefers a channel one of those nicks
is in and tells the bot to address the line to whoever is actually
there. Nobody in any room it can speak in? It posts unaddressed.

⛔ **The bot is told the nick and nothing else, on purpose.** A `for:`
list is configuration you typed; nobody told the bot anything about
these people, so it may name them and may not say or imply why it did.
Do not add a persona sentence explaining the list — the bot cannot
answer *"how do you know I care about that?"* and must not sound like
it can.

### `watch` — feed URLs (RSS / HTML)

A `watch` array declares URLs the acquisition engine polls *in addition
to* the search paths. Each entry is a bare URL string or an object:

```json
"watch": [
  "https://archlinux.org/news/",
  {"url": "https://archlinux.org/feeds/news/", "every": 3600, "type": "rss"}
]
```

| Field | Required | Type | Notes |
|---|---|---|---|
| `url` | yes | string | `http://` or `https://` only. Loopback / RFC1918 rejected. |
| `every` | no | int | Seconds. Clamped up to a 300 s minimum, which is also the default. |
| `type` | no | string | `"html"` (default) or `"rss"` (RSS 2.0 / Atom, auto-detected). |

Behavior:

- **RSS/Atom** → libxml2 parses the feed, extracts each item's title +
  body, dedupes against a per-feed seen-GUID ring (256 entries), and
  sends survivors through the digest → ingest pipeline.
- **HTML** → libxml2 XPath prefers `<article>` or `<main>` subtrees;
  falls back to full-body tag-strip. Uses `<title>` as the subject.
- **Conditional GET** — ETag / Last-Modified are remembered in-memory;
  subsequent fetches carry `If-None-Match` / `If-Modified-Since`. A
  `304` skips the whole digest chain.
- **Body-hash dedup** — even without a server-side 304, an unchanged
  body short-circuits.
- **State is in-memory only.** Restart or personality reload = first
  tick re-fetches; the chunk-id ring and ingest-edge dedup absorb the
  duplicates.
- **Global kill switch:** `tools/botmanctl set kv acquire.sources_enabled false`.

### Full `interests:` example

```yaml
interests:
  [
    {"topic": "arch linux",
     "aka": ["archlinux", "pacman", "AUR", "pkgbuild"],
     "attention": "high",
     "category": "news",
     "watch": [
       {"url": "https://archlinux.org/feeds/news/", "every": 3600, "type": "rss"},
       {"url": "https://archlinux.org/news/",       "every": 7200}
     ]},
    {"topic": "linux kernel",
     "aka": ["kernel", "initramfs", "mkinitcpio", "dkms"],
     "attention": 30,
     "category": "news"},
    {"topic": "systemd",
     "aka": ["journalctl", "systemctl", "unit file", "OnCalendar"],
     "attention": "off"}
  ]
```

### Common patterns (recipes)

**1. A topic the bot brings up on its own and reacts to**

```json
{"topic": "LLM inference",
 "aka": ["vLLM", "llama.cpp", "context window", "tool use"],
 "attention": "high",
 "category": "news"}
```

**2. Curated feeds, minimal searching**

```json
{"topic": "verge ai coverage",
 "attention": "off",
 "watch": [{"url": "https://www.theverge.com/ai-artificial-intelligence",
            "every": 1800}]}
```

`attention: off` stops the proactive picker ever selecting it, so the
feed is the only *scheduled* source. ⚠ The topic string is still a
trigger, so pick one nobody says in chat — that is what makes this
recipe quiet, not a mode setting.

**3. Reactive lookup, no feed**

Quiet until somebody says the word, then searches what they said.

```json
{"topic": "systemd",
 "aka": ["journalctl", "systemctl", "unit file"],
 "attention": "off"}
```

**4. A topic somebody in particular follows**

```json
{"topic": "cryptocurrency",
 "aka": ["crypto", "bitcoin", "ethereum", "solana"],
 "for": ["doc"],
 "attention": "normal",
 "category": "news"}
```

### Picking `attention` (decision table)

| Want | `attention` |
|---|---|
| The bot's headline interest — brings it up often | `high` (100) |
| A normal interest | `normal` (50), the default |
| Follows it, rarely raises it unprompted | `low` (25), or any integer |
| Never goes looking; still answers when it comes up | `off` (0) |

⚠ `off` is not "ignore". Every topic is scanned reactively regardless.

### Inspecting

```sh
tools/botmanctl show acquire               # global engine state
tools/botmanctl acquire source trigger <bot> <url>   # arm a feed to fire next tick
```

Per-topic / per-feed state (cadence, last-fetched, ETag, GUID ring
fill) lives in memory only; per-topic activity is also persisted in
the `acquire_topic_stats` DB table (`last_proactive`, `last_reactive`,
`total_queries`, `total_ingested`).

---

## Output contract file

Output contracts live under `contracts/` and share the same grammar.
They describe **wire hygiene** (what may appear in output bytes) plus
runtime sentinels (`SKIP`, `/me`). Shipped contracts:

- `default.txt` — "pass as a real human." No markdown, no emoji, no
  parentheticals, no angle-bracket tokens. Prime-directive framing.
- `support.txt` — "you are openly a bot." Same wire hygiene, drops
  the human-passing directive.

### Frontmatter fields

| Field         | Required | Type   | Notes                                  |
|---------------|----------|--------|----------------------------------------|
| `name`        | yes      | string | Stem/label.                            |
| `description` | yes      | string | One-liner shown by `/show contracts`.  |

(`version` and `interests` are ignored on contract files.)

### Minimal example

```
---
name: support
description: Output contract for openly-a-bot helpers.
---
Output rules:
- Plain text only. No markdown, no emoji, no parentheticals.
- Actions on their own line via "/me <text>".
- Emit the single word SKIP on a line by itself when you have nothing
  to say and the message wasn't directed at you. Direct address is
  always answered.
```

### Why two files, not one

- **N × M, not N.** Five human-passing personas share one contract.
  Adding a wire rule is a single edit.
- **Swappable deployment.** Same persona can ship under `default`
  (pass-as-human) or `support` (openly-a-bot) without editing the
  persona.
- **Splice order.** The driver puts the contract *last* in the
  system prompt so its rules have recency in the model's working
  memory. Authors don't need to think about ordering.

See `AGENTS.md` for the `SKIP` ABI details, `/me` framing, and the
`nl_bridge` command-routing layer.

---

## Binding a bot to a personality + contract

All three keys are KV; set with `tools/botmanctl set kv <key> <value>`.

| KV                                   | Purpose                                                          |
|--------------------------------------|------------------------------------------------------------------|
| `bot.<n>.personality`                | Active personality stem (this directory).                        |
| `bot.<n>.behavior.contract`          | Active contract stem (under `contracts/`).                       |
| `bot.chat.personalitypath`           | Absolute path to the personalities dir (defaults to `./personalities`). |
| `bot.chat.contractpath`              | Absolute path to the contracts dir (defaults to `./personalities/contracts`). |
| `plugin.chat.default_contract`       | Fallback contract when the per-bot key is empty (ships as `default`). |

Pin the two path keys to absolute paths when the daemon's CWD is
something other than the repo root (e.g. `build/`).

---

## Per-bot runtime knobs worth knowing

These are **bot-level** settings, not personality frontmatter.
Personalities should never encode deployment choices.

| KV                                     | Effect                                                                 |
|----------------------------------------|------------------------------------------------------------------------|
| `bot.<n>.chat_model`                   | LLM chat model (fallback `llm.default_chat_model`).                    |
| `bot.<n>.speak_temperature`            | Sampling temperature × 100 (`70` = 0.70).                              |
| `bot.<n>.max_reply_tokens`             | Hard cap on assistant reply length.                                    |
| `bot.<n>.corpus`                       | Semicolon-list of knowledge corpora to RAG over.                       |
| `bot.<n>.acquired_corpus`              | Corpus the acquisition engine ingests into.                            |
| `bot.<n>.behavior.witness_log`         | Log overheard lines to `conversation_log` (default true).              |
| `bot.<n>.behavior.anonymous_dossiers`  | Create dossiers for unregistered senders (default false).              |
| `bot.<n>.behavior.mute_until`          | Epoch-seconds silence window (mute expires automatically).             |
| `bot.<n>.behavior.coalesce_ms`         | Paste-coalescing idle window (default 1500).                           |
| `bot.<n>.behavior.max_inflight`        | Concurrent in-flight LLM replies.                                      |
| `bot.<n>.behavior.nl_bridge_cmds`      | NL-command allowlist (`*`, CSV, or empty to disable).                  |
| `bot.<n>.behavior.speak.*`             | Speak-policy probabilities, cooldowns, engagement windows.             |
| `bot.<n>.behavior.fact_extract.*`      | Periodic conversation-log → dossier-fact sweep.                        |
| `bot.<n>.behavior.volunteer.*`         | Spontaneous post-acquire speech (default off).                         |
| `bot.<n>.behavior.mention.*`           | `ABOUT PEOPLE MENTIONED` prompt-block budgets.                         |

Engine-global knobs worth knowing about:

| KV                                  | Effect                                                           |
|-------------------------------------|------------------------------------------------------------------|
| `acquire.enabled`                   | Master kill-switch for the acquisition engine.                   |
| `acquire.sources_enabled`           | Master kill-switch for the RSS/HTML source path (leaves SXNG on). |
| `acquire.tick_cadence_secs`         | Per-bot tick interval (default 600).                             |
| `acquire.max_reactive_per_hour`     | Per-(bot, topic) rate limit.                                     |
| `acquire.relevance_threshold`       | Digest relevance score floor (0–100, default 50).                |

Full list: `tools/botmanctl show kv acquire.` and
`tools/botmanctl show kv bot.<n>.`.

---

## Live edits and reload

- **Editing the file on disk is enough.** The next inbound message
  picks up the change.
- **Changing an `interests:` block requires re-registering with the
  acquisition engine.** Easiest way:
  ```sh
  tools/botmanctl bot <n> refresh_prompts
  ```
  (`/bot stop` + `/bot start` *also* works but is heavier.)
- **Changing the contract** takes effect on the next reply — no
  refresh needed because the chat driver resolves the contract path
  per message.

---

## Validation and troubleshooting

Always list after any change:

```sh
tools/botmanctl show personalities
tools/botmanctl show contracts
tools/botmanctl show acquire
```

Parse errors render as red rows with the parser's complaint; the rest
of the listing continues.

Common failure modes:

| Symptom                                              | Cause / fix                                                                 |
|------------------------------------------------------|-----------------------------------------------------------------------------|
| `parse error: continue` + `zero topics parsed`       | A blank line inside the topic array (it ends the value), a missing closing quote on a key (`"category: "news"`), or YAML `|` form. Note json-c *accepts* a trailing comma. Validate with `python3 -m json.tool`. |
| `interests: DECLARED BUT NONE PARSED`                | The reply from `bot <name> refresh_prompts`. Read the WARN lines in the log — each names the topic index and what was wrong with it. |
| `interests: bot has no acquired_corpus`              | The block is fine; the bot has nowhere to put what it reads. Set `bot.<n>.acquired_corpus`. |
| `interests[N] (topic='…'): unknown key '…'`          | Key name typo. Common: `search_mode` (it is `category`), `keywords` / `mode` / `query` (all removed — see §The five keys). |
| Bot fires SearXNG when you only wanted a feed        | Every topic is scanned reactively; there is no setting that stops it. `attention: off` silences the *proactive* path only. Give the topic a `topic`/`aka` nobody says in chat. |
| Feed seems to fetch on the wrong cadence             | `every` lives on each `watch` entry, not on the topic. Minimum, and default, is 300 s. |
| Bot replies in third person                          | Contract not loading (check `bot.<n>.behavior.contract`) or persona is too weak on voice. |
| Bot emits markdown / emoji / parentheticals          | Contract missing a wire rule, or persona body is overriding it.             |
| `interests[N]: missing or empty 'topic'`             | `topic` is the one required key.                                            |
| `interests[N].watch[M]: unknown type='...'`          | Only `rss` and `html` are supported today.                                  |
| `/weather paris` appears as literal chat text        | NL bridge disabled. Set `bot.<n>.behavior.nl_bridge_cmds`.                  |
| Bot claims to be an AI                               | Add an explicit "never break character" clause to the *persona* body.       |

---

## Security / safety notes

- `name` values containing `/` are rejected — a KV-controlled stem
  can't escape the configured path root.
- Personality and contract bodies go straight into the system prompt.
  Anything written here ships on every call; **don't put secrets in
  these files**.
- Source URLs are trusted config but still SSRF-guarded: non-HTTP
  schemes, loopback, RFC1918, link-local, and multicast targets are
  rejected at fetch time.
- Feed state (ETag, last-modified, body hash, seen-GUID ring) is
  in-memory only. A reload resets it and the first tick re-fetches
  once.

---

## Going deeper

`AGENTS.md` covers the reasoning behind the two-file split, the full
`SKIP` ABI, the `nl_bridge` command-routing layer, the security
posture for author-agents, authoring checklists, and a failure-mode
catalog written for LLM agents working inside this directory.
