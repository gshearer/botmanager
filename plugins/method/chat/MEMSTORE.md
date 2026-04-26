# MEMSTORE.md — Memory Subsystem (`plugins/method/chat/memory.c`)

Agent reference for the persistent memory store: facts, conversation log,
embeddings, RAG, decay. This is the subsystem behind the `/memory` CLI
command and `memory.*` KV keys. (For the compressed agent project briefing,
see `PRIMER.md` — different concept entirely.)

> **Sibling subsystem**: `plugins/inference/knowledge.c` stores
> **corpus-scoped** chunks (Arch wiki, SEP, product docs — external
> authoritative content) that a persona binds to via a `knowledge:`
> frontmatter field. Same float32 LE BYTEA + cosine machinery as this
> file, different tables, no per-user scoping. See `KNOWLEDGE.md`.
> The two subsystems share an embed model by default
> (`knowledge.embed_model` falls back to `memory.embed_model`). The
> **acquisition engine** (`plugins/inference/acquire.c`, see
> `ACQUIRE.md`) is the *writer* that keeps a per-bot knowledge
> corpus current by firing SearXNG queries, LLM-digesting the
> results, and ingesting summaries into `knowledge_chunks` — it
> feeds the knowledge store; neither it nor the knowledge store
> touches this file's per-user tables.

`plugins/method/chat/memory.c` provides a bot-agnostic store for two kinds of persistent
state:

1. **Conversation log** — every observed or emitted message, suitable for
   later retrieval and (Chunk D) embedding.
2. **User facts** — per-user `(kind, key) → value` memories extracted
   from dialogue or admin-seeded, with confidence scores and decay.

Chunks C+D ship the storage, embedding write path, and RAG retrieval.
Fact extraction lives in the chat bot driver (Chunk E+F).

## When to use it

- **Bot drivers** call `memory_log_message()` from their `on_message`
  entrypoints (WITNESS for overheard lines, EXCHANGE_IN for messages
  directed at the bot, EXCHANGE_OUT for the bot's own replies).
- **Fact extractors** (plugins or future prompt-driven paths) call
  `memory_upsert_fact()` whenever they derive a new fact. The merge
  policy controls conflict behavior.
- **Retrieval-augmented generation** uses `memory_retrieve()`. Chunk C
  returns zero results synchronously so callers can code against the
  final API shape today.

All keying is per-(namespace, user). Facts are tied to a
`userns_user.id`; log rows additionally carry `ns_id` so cross-user
group-channel retrieval works.

## Tables

All five tables are defined in [`scripts/schema.sql`](scripts/schema.sql)
and mirrored by `memory_ensure_tables()` with `CREATE TABLE IF NOT EXISTS`.

| Table | Purpose |
|-------|---------|
| `personalities` | Named LLM system-prompt + behavior bundles (loader lands in Chunk E) |
| `user_facts` | Per-user memories; `UNIQUE(user_id, kind, fact_key)` |
| `user_fact_embeddings` | 1:1 with `user_facts`; written asynchronously after every successful upsert |
| `conversation_log` | Every WITNESS / EXCHANGE_IN / EXCHANGE_OUT event |
| `conversation_embeddings` | 1:1 with `conversation_log`; written asynchronously for eligible kinds |

`conversation_log` carries two dossier columns:

- `dossier_id BIGINT NULL` — the sender's dossier (see `DOSSIER.md`).
  FK attached lazily by `dossier_register_config()`.
- `referenced_dossiers JSONB NULL` — dossier ids mentioned *by name* in
  the text (computed via `dossier_find_mentions` at witness time).
  `NULL` = not computed; writing an empty `[]` is reserved for
  "computed and empty" but not emitted today. Indexed via GIN
  (`idx_conv_refs_gin`) so mention-aware retrieval is cheap.

`dossier_facts.fact_key` supports a lightweight relational convention:
a fact on dossier A with `fact_key = "toward:<B>"` encodes "A's
attitude/observation directed at B". `memory_retrieve_dossier(B, …)`
pulls these alongside B's own facts so prompts get "what others have
said about B" context. The LLM fact extractor (`FACT_EXTRACT.md`) is
now a real producer of both `toward:<N>` and plain dossier facts,
stamped with `source='llm_extract'`.

`user_facts.kind` is a `SMALLINT` matching `mem_fact_kind_t`
(PREFERENCE, ATTRIBUTE, RELATION, EVENT, OPINION, FREEFORM).
`user_facts.source` distinguishes `llm_extract` (default) from
`admin_seed` — the decay sweep skips the latter.

FK cascades: deleting a `userns_user` row removes the user's facts and
fact embeddings; conversation log rows have `ON DELETE SET NULL` on
`user_id` (so the message text survives but the sender becomes
anonymous), and their embeddings cascade from `conversation_log.id`.

## Public API

```c
#include "memory.h"

// Log a line the bot just observed.
mem_msg_t m = { 0 };
m.ns_id        = ns->id;
m.user_id_or_0 = resolved_user_id;   // 0 if unknown
m.kind         = MEM_MSG_WITNESS;    // or EXCHANGE_IN / EXCHANGE_OUT
snprintf(m.bot_name, sizeof(m.bot_name), "%s", bot_name(bot));
snprintf(m.method,   sizeof(m.method),   "%s", method_name);
snprintf(m.channel,  sizeof(m.channel),  "%s", msg->channel);
snprintf(m.text,     sizeof(m.text),     "%s", msg->text);
memory_log_message(&m);

// Store a fact.
mem_fact_t f = { 0 };
f.user_id    = uid;
f.kind       = MEM_FACT_PREFERENCE;
snprintf(f.fact_key,   sizeof(f.fact_key),   "coffee");
snprintf(f.fact_value, sizeof(f.fact_value), "black");
snprintf(f.source,     sizeof(f.source),     "llm_extract");
f.confidence = 0.85f;
memory_upsert_fact(&f, MEM_MERGE_HIGHER_CONF);

// Read facts back.
mem_fact_t out[16];
size_t n = memory_get_facts(uid, MEM_FACT_KIND_ANY, out, 16);
```

`memory_retrieve()` embeds the query through `llm_embed_submit()` and,
on completion, cosine-scans both embedding tables, keeps the top-K hits
(merged across facts + msgs), fetches the backing rows, and invokes the
callback exactly once on the curl worker thread. When memory is
disabled, `memory.embed_model` is unset, or the query is empty, the
callback fires synchronously with zero results before `memory_retrieve`
returns — callers never need to special-case those cases.

## KV keys

All under the `memory.*` prefix. Defaults are applied if the row is
missing or out of range.

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `memory.enabled` | bool | `true` | Master switch |
| `memory.embed_model` | string | `""` | Name of an `llm` embed model. Empty disables all embedding + RAG paths |
| `memory.witness_embeds` | bool | `false` | Embed WITNESS (overheard) messages in addition to EXCHANGE_* |
| `memory.embed_own_replies` | bool | `false` | Embed the bot's own `EXCHANGE_OUT` replies (avoids echo-chamber RAG when off) |
| `memory.log_retention_days` | uint32 | `30` | Retention for `conversation_log` |
| `memory.fact_decay_half_life_days` | uint32 | `30` | Half-life for fact confidence |
| `memory.min_fact_confidence` | float | `0.6` | Floor for the decay sweep |
| `memory.rag_top_k` | uint32 | `8` | Default top-K for retrieve |
| `memory.rag_max_context_chars` | uint32 | `2048` | Cap on injected context size |
| `memory.decay_sweep_interval_secs` | uint32 | `3600` | Sweep cadence |

## Commands

All commands require the `admin` group. Destructive commands default
to `CMD_SCOPE_PRIVATE`.

| Command | Scope | Purpose |
|---------|-------|---------|
| `/memory facts <user>` | any | List facts for a user |
| `/memory fact del <id>` | private | Delete a fact by id |
| `/memory fact set <user> <kind> <key> <value> [conf]` | private | Admin-seed a fact (`source="admin_seed"`, not decayed) |
| `/memory log <user> [limit]` | private | Recent `conversation_log` rows for a user |
| `/memory forget user <user>` | private | Delete all facts + log rows for a user |
| `/memory rag <user> <query>` | private | Debug: run `memory_retrieve` and print hit counts |
| `/show memory` | any | Stats snapshot + KV values + pending sweep ETA |

Bot-scoped views of the same data live under the llm kind's show verbs
(see `CHATBOT.md`):

- `/show bot <bot> llm facts <user>|<dossier_id>` — namespace-scoped
  fact list (resolves the user through the bot's bound userns, or
  treats a numeric argument as a dossier id).
- `/show bot <bot> llm memories [<query>]` — last 20 entries from
  `conversation_log` for this bot, or RAG-style hits over the bot's
  namespace when a query is supplied.

**DM-fact leakage caveat**: facts carry a `channel` column that records
where they were first observed. A future group-reply retrieval path
should filter on `channel=''` (DM origin) except when the requester is
the addressee — otherwise private facts can leak into public replies.
The enforcement lives in the caller (the chat bot's RAG builder in Chunk E).

## Concurrency

The storage writes in `memory_log_message()` and `memory_upsert_fact()`
are synchronous: each blocks the calling thread for one DB round trip.
The embedding writes (and any cosine scan inside `memory_retrieve()`)
run asynchronously on the curl worker thread once the LLM embed job
completes. Bot drivers already run on worker threads, so the latency is
absorbed by the worker pool. If this becomes a bottleneck, switch to
`db_query_async()` — the public API is fire-and-forget for log_message
and returns SUCCESS/FAIL synchronously for upsert_fact, both
compatible with deferring the actual SQL.

Stats counters (`memory_get_stats`) are protected by a dedicated mutex;
the cached KV snapshot (`memory_cfg_t`) is also mutex-guarded. Commands
and the decay task read the snapshot under lock.

## Embeddings + RAG

Embeddings are fire-and-forget and eventually consistent. After the
storage write succeeds, memory submits an `llm_embed_submit()` job to
the configured embed model (`memory.embed_model`); the completion
callback writes a `(model, dim, float32 LE BYTEA)` row into the matching
embedding table. If the embed fails or the model is unregistered, the
base row still exists — retrieval just won't find it until a new embed
succeeds.

**What gets embedded:**

| Event | Embedded? |
|-------|-----------|
| `memory_upsert_fact()` (any merge policy) | Always, using the final `fact_value` |
| `memory_log_message(EXCHANGE_IN)` | Always (directed at the bot) |
| `memory_log_message(EXCHANGE_OUT)` | Only if `memory.embed_own_replies=true` |
| `memory_log_message(WITNESS)` | Only if `memory.witness_embeds=true` |

All paths no-op silently when `memory.embed_model` is empty, when the
model isn't registered, or when `memory.enabled=false`.

**Vector format**: each embedding row stores the float32 vector in
little-endian bytes inside a Postgres `BYTEA`. No pgvector extension
required — `memory_retrieve()` runs a plain-SQL scan and cosine-scores
each row in C. This is correct, portable, and fast enough at Chunk D's
target row counts; if it ever becomes the bottleneck the schema can be
swapped to pgvector in place.

**Retrieval** (`memory_retrieve()`):

1. Embed the query via `llm_embed_submit()` (async on the curl worker
   thread).
2. In the completion callback, SELECT rows from `user_fact_embeddings`
   (filtered by `user_id` when `user_id_or_0 != 0`) and
   `conversation_embeddings` (filtered by `ns_id`, and by `user_id`
   when non-zero), both matching the active `(model, dim)`.
3. Cosine-score every row against the query vector; maintain two
   top-K buffers (one per row type) using the `top_k` argument if
   non-zero, else `memory.rag_top_k`.
4. SELECT the chosen rows in a single `IN (…)` query per table,
   populate `mem_fact_t[]` / `mem_msg_t[]`, and invoke the callback
   exactly once.

The callback's arrays are owned by `memory.c` and valid only for the
callback's lifetime — consumers must copy any data they need to retain.

**Sync shortcut**: when memory is disabled, `embed_model` is empty, or
the query is empty, `memory_retrieve()` delivers zero results
synchronously before returning. Callers always see exactly one callback
invocation per successful call; they never have to special-case the
disabled path.
