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

### How a topic actually fires (mental model)

A single topic can drive **three independent paths** that all dump into
the same corpus. You don't need all three — pick what fits.

```
                          ┌─────────────────┐
chat line matches kw   →  │ reactive search │  → SXNG → fetch → digest → corpus
                          └─────────────────┘
                          ┌─────────────────┐
periodic tick          →  │ proactive search│  → SXNG → fetch → digest → corpus
                          └─────────────────┘
                          ┌─────────────────┐
per-source cadence     →  │ feed dispatcher │  → fetch → digest → corpus
                          └─────────────────┘
```

| Path | Driven by | Topic fields it reads |
|---|---|---|
| **Reactive search** | Someone says one of your `keywords` in chat | `keywords[]`, `query_template`, `category` |
| **Proactive search** | Per-bot tick (every `acquire.tick_cadence_secs`, default 600s), weighted random pick | `query`, `upcoming_query`, `proactive_weight`, `category` |
| **Feed dispatcher** | Each `sources[]` entry's own `cadence_secs` | `sources[]` (URLs are fetched verbatim — keywords/queries are NOT used here) |

These paths are **independent**. A `sources[]` URL fires on its own
schedule whether or not the keywords match. A keyword match fires a
SearXNG search whether or not you have explicit sources. If you want
"only my curated URLs, no SearXNG," see the recipes below.

### What "reactive" really means

Common misconception: *"I want the bot to fetch my URL when someone
mentions a keyword."* That is **not** what `keywords` + `sources` does.
Keywords trigger SearXNG searches. The `sources[]` URLs fetch on their
own cadence and populate the corpus. The bot answers about your topic
on a keyword mention because the *corpus retrieval* (RAG) surfaces the
relevant chunk — not because the keyword "called" your source.

If your URL is being polled regularly, the corpus has fresh content,
and RAG does the rest naturally.

### It must be valid JSON, NOT YAML

`json-c` parses this block. YAML pipe form (`|`) silently produces
zero topics. Keep it as a real JSON array:

```yaml
interests:
  [
    { ... },
    { ... }
  ]
```

Newlines inside the JSON are fine; the slurper preserves them.

> **Common gotchas that silently kill the whole block:**
> - Trailing comma after the last `,` in an object or array.
> - Forgotten closing quote on a key — e.g. `"category: "news"` (missing
>   the `"` after the key name) breaks the parser for that topic AND
>   often cascades to drop the whole array.
> - Unknown field names are silently ignored. `"search_mode": "news"`
>   compiles fine but does nothing — the field is `"category"`.
> - When in doubt, paste the block (without the `interests:` prefix) into
>   `python3 -m json.tool` to confirm it parses.

### Topic object fields

| Field              | Required | Type     | Default            | Notes                                                                    |
|--------------------|----------|----------|--------------------|--------------------------------------------------------------------------|
| `name`             | yes      | string   | —                  | Short label (`archlinux`, `kernel`). Used in logs and the subject row.  |
| `mode`             | yes      | string   | —                  | `"active"`, `"reactive"`, or `"mixed"` (case-insensitive).              |
| `keywords`         | no       | string[] | `[]`               | Triggers reactive matches when they appear in a message. Max 16.        |
| `query`            | no\*     | string   | `""`               | Literal query string for the proactive path.                            |
| `query_template`   | no\*     | string   | `""`               | Template with `{subject}` — filled per reactive hit.                    |
| `upcoming_query`   | no       | string   | `""`               | Fires every 10th proactive tick when set (look-ahead queries).          |
| `proactive_weight` | no       | int 0-100| mode-dependent†   | Weighted-random selector for proactive ticks. `0` = reactive-only.      |
| `cadence_secs`     | no       | int      | `0` (engine default) | Reserved — currently parsed but not consumed. Use per-source cadence instead. |
| `max_sources`      | no       | int      | `0` (inherit)      | Per-topic override for `acquire.max_sources_per_query`. `0` inherits the global cap. Upper bound is `plugin.searxng.max_results`. |
| `category`         | no       | string   | `"general"`        | SearXNG category: `general`, `images`, `news`, `videos`, `music`. Steers SXNG paths only; feeds ignore it. |
| `sources`          | no       | object[] | `[]`               | Explicit RSS/HTML feed URLs (see below). Max 8 per topic.               |

\*`query` or `query_template` should be set for anything other than a
pure-reactive keyword-only topic. If both are empty the topic never
produces a query proactively (reactive falls back to the matched
subject as the query).

†Default `proactive_weight`: `active` = 100, `reactive` = 0, `mixed` =
50.

### Mode semantics

| Mode        | Fires on keyword hits (reactive) | Fires on proactive tick | Typical default weight |
|-------------|----------------------------------|-------------------------|------------------------|
| `active`    | yes                              | yes                     | 100                    |
| `reactive`  | yes                              | no                      | 0                      |
| `mixed`     | yes                              | yes (lower weight)      | 50                     |

### Source feeds (RSS / HTML)

A `sources` array on a topic declares explicit URLs the acquisition
engine polls *in addition to* the SearXNG path. Each entry is one of:

```json
{"type": "rss",  "url": "https://example.com/feed.xml", "cadence_secs": 3600}
{"type": "html", "url": "https://example.com/news",     "cadence_secs": 7200}
```

| Field          | Required | Type   | Notes                                                         |
|----------------|----------|--------|---------------------------------------------------------------|
| `type`         | yes      | string | `"rss"` (RSS 2.0 / Atom, auto-detected) or `"html"`.          |
| `url`          | yes      | string | `http://` or `https://` only. Loopback / RFC1918 rejected.    |
| `cadence_secs` | no       | int    | Clamped up to 300s minimum. Defaults to the clamp floor.      |

Behavior:

- **RSS/Atom** → libxml2 parses the feed, extracts each item's title +
  body, dedupes against a per-source seen-GUID ring (256 entries),
  and sends survivors through the digest → ingest pipeline.
- **HTML** → libxml2 XPath prefers `<article>` or `<main>` subtrees;
  falls back to full-body tag-strip. Uses `<title>` as the subject.
- **Conditional GET** — ETag / Last-Modified are remembered
  in-memory; subsequent fetches carry `If-None-Match` /
  `If-Modified-Since`. A `304` skips the whole digest chain.
- **Body-hash dedup** — even without server-side 304, an unchanged
  body short-circuits.
- **State is in-memory only.** Restart or personality reload =
  first tick re-fetches; chunk-id ring and ingest-edge dedup absorb
  duplicates.
- **Global kill switch:** `tools/botmanctl set kv acquire.sources_enabled false`.

### Full `interests:` example (real patterns, trimmed)

```yaml
interests:
  [
    {
      "name": "archlinux",
      "mode": "active",
      "proactive_weight": 100,
      "query": "archlinux news this week",
      "upcoming_query": "archlinux upcoming release notes",
      "category": "news",
      "keywords": ["archlinux", "arch linux", "pacman", "AUR"],
      "sources": [
        {"type": "rss",  "url": "https://archlinux.org/feeds/news/", "cadence_secs": 3600},
        {"type": "html", "url": "https://archlinux.org/news/",       "cadence_secs": 7200}
      ]
    },
    {
      "name": "linux",
      "mode": "mixed",
      "proactive_weight": 30,
      "query": "linux kernel news this week",
      "category": "news",
      "keywords": ["linux", "kernel", "sysadmin"]
    },
    {
      "name": "systemd",
      "mode": "reactive",
      "keywords": ["systemd", "journalctl", "systemctl", "OnCalendar"],
      "query_template": "{subject} systemd documentation"
    }
  ]
```

### Common patterns (recipes)

Pick the recipe that matches your intent — these are the four shapes
that account for ~95% of real topic configs.

**1. Pure SearXNG search topic (no curated URLs)**

The bot autonomously searches the web for the topic on a tick, and also
when chat mentions a keyword.

```json
{
  "name": "ai-news",
  "mode": "active",
  "query": "ai industry news this week",
  "category": "news",
  "keywords": ["openai", "anthropic", "llm"]
}
```

**2. Pure feed topic (no SearXNG at all)**

The bot only learns from URLs you've explicitly listed. No web search
will ever fire for this topic. The keyword field is used purely to hint
the digest LLM what's relevant in the fetched pages.

```json
{
  "name": "verge-ai",
  "mode": "active",
  "proactive_weight": 0,
  "keywords": ["ai", "llm"],
  "sources": [
    {"type": "html", "url": "https://www.theverge.com/ai-artificial-intelligence",
     "cadence_secs": 1800}
  ]
}
```

Why `mode: "active"` + `proactive_weight: 0`? Because `mode: "active"`
makes the chat scanner *skip the topic for reactive triggers*, and
`proactive_weight: 0` keeps the proactive picker from ever selecting
it. The result: SXNG never runs, only the feed.

**3. Hybrid — feeds plus reactive search backup**

You have a curated feed but you also want SearXNG to fill in when chat
brings up specifics not on your feed.

```json
{
  "name": "kernel",
  "mode": "reactive",
  "keywords": ["kernel", "linux 6.", "scheduler"],
  "query_template": "{subject} linux kernel",
  "sources": [
    {"type": "rss", "url": "https://www.kernel.org/feeds/all.atom.xml",
     "cadence_secs": 3600}
  ]
}
```

**4. Reactive lookup, no feed**

Quiet when nobody's talking about it; fires a SearXNG search the moment
a keyword shows up.

```json
{
  "name": "systemd",
  "mode": "reactive",
  "keywords": ["systemd", "journalctl", "systemctl"],
  "query_template": "{subject} systemd documentation"
}
```

### Picking `mode` (decision table)

| Want | `mode` | Notes |
|---|---|---|
| Bot brings the topic up on its own AND reacts to chat | `active` | Default `proactive_weight` 100 — turn down if it dominates picks. |
| Bot only speaks up when chat mentions it | `reactive` | Default `proactive_weight` 0 — never picked proactively. |
| Light proactive coverage, plus reactive | `mixed` | Default `proactive_weight` 50. |
| Curated URLs only, no web search ever | `active` + `proactive_weight: 0` | Counter-intuitive: `active` disables the *reactive scanner*, weight 0 disables the *proactive picker*. Feeds are unaffected. |

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
| `parse error: continue` + `zero topics parsed`       | Trailing comma in JSON, missing closing quote on a key (`"category: "news"`), or YAML `|` form. Validate with `python3 -m json.tool`. |
| Topic seems to ignore a field you set                | Field name typo — unknown keys are silently dropped. Common: `search_mode` (should be `category`), `kind` (should be `type` inside `sources`). |
| Bot fires SearXNG when you only wanted a feed        | Reactive scanner triggers on `keywords[]` regardless of `sources`. Use `mode: "active"` + `proactive_weight: 0` to silence both SXNG paths. |
| Feed seems to fetch on the wrong cadence             | `cadence_secs` lives on each `sources[]` entry, not the topic. Topic-level `cadence_secs` is currently a no-op. Per-feed minimum is 300s. |
| Bot replies in third person                          | Contract not loading (check `bot.<n>.behavior.contract`) or persona is too weak on voice. |
| Bot emits markdown / emoji / parentheticals          | Contract missing a wire rule, or persona body is overriding it.             |
| `interests[N]: missing or unknown 'mode'`            | `mode` must be `active`, `reactive`, or `mixed`.                            |
| `interests[N].sources[M]: unknown type='...'`        | Only `rss` and `html` are supported today.                                  |
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
