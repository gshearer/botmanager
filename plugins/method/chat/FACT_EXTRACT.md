# FACT_EXTRACT.md — LLM-driven dossier fact extraction

Chunk F of the dossier work. The extractor turns rows of
`conversation_log` into structured `dossier_facts` via a chat LLM
call. It runs as a periodic per-bot sweep; operators can also fire
a one-shot via `/bot <name> dossiersweep`.

The subsystem lives in `plugins/method/chat/extract.c` /
`plugins/method/chat/extract_prompt.c` and is fronted by
`plugins/method/chat/extract.h`. Per-bot knobs live on the chatbot
plugin (`plugins/method/chat/chatbot.c`).

## What it does

One sweep, per bot instance, per tick:

1. Read the per-bot high-water mark from KV
   (`bot.<name>.behavior.fact_extract.hwm`).
2. Pull the next `batch_cap` rows from `conversation_log` past the
   hwm where `dossier_id IS NOT NULL` and `kind <> EXCHANGE_OUT`.
3. Build the participants block (union of senders + every id in
   `referenced_dossiers`, each tagged `sender` or `mentioned`,
   labeled with the dossier's current `display_label`).
4. Send one chat completion with the system prompt + the
   (participants, messages) user message.
5. Parse the JSON response, validate each candidate fact, and
   upsert accepted facts via
   `memory_upsert_dossier_fact(..., MEM_MERGE_HIGHER_CONF)` with
   `source = "llm_extract"`.
6. Advance the hwm unconditionally so bad batches are not retried
   forever.

## When it fires

- Periodic: `chatbot_start` calls `extract_schedule(bot, ns_id,
  interval_secs)` when `behavior.fact_extract.enabled = true`. The
  task is `task_add_periodic("extract.<bot>", ...)` with
  `behavior.fact_extract.interval_secs` (default 300s). `chatbot_stop`
  unschedules.
- Manual: `/bot <name> dossiersweep` (admin-only, level 100).
  Resolves the bot by name, pulls its userns, calls
  `extract_run_once` synchronously and replies with the fact
  count and elapsed ms. Honors the rate limit and enable gate.

## Prompt shape

System message is stable: safety + the output schema reminder.

User message packs:

- **Participants block**: one row per dossier:
  `[<dossier_id>] <display_label>  role=<sender|mentioned>`.
- **Messages block**: batch rows in channel order, each as
  `[<dossier_id>] <action?> <text>`. `<action?>` is `/me` when
  the witness text starts with `* <nick> ` (the convention
  `chatbot_log_line` uses for CTCP ACTION emotes).

Total prompt capped at `EXTRACT_PROMPT_MAX_SZ` (16 KiB).

## Output schema (strict)

```json
{
  "facts": [
    {
      "dossier_id": <int in participants>,
      "kind": "preference"|"attribute"|"relation"|"event"|"opinion"|"freeform",
      "fact_key": "<short key, or 'toward:<pid>' for relational>",
      "fact_value": "<value>",
      "confidence": <float in [0.0, 1.0]>
    }
  ],
  "aliases": [
    {
      "dossier_id": <int in participants>,
      "alias": "<3..32 alphanumeric>",
      "confidence": <float in [0.0, 1.0]>
    }
  ]
}
```

The `aliases` array is optional; see the Aliases section below.

No prose, no markdown. A leading / trailing ` ```json ` code fence
is stripped before parsing. Malformed JSON logs at CLAM_INFO
(model slop is expected) and bumps `llm_errors`.

## Validation rules

Each candidate fact must pass:

1. `dossier_id` appears in the participants list we sent.
2. `kind` maps to a valid `mem_fact_kind_t`.
3. `fact_key` length in `[1, MEM_FACT_KEY_SZ - 1]`; if prefixed
   with `toward:`, the suffix must be a decimal int that is also
   in participants.
4. `fact_value` length in `[1, MEM_FACT_VALUE_SZ - 1]`.
5. `confidence` in `[0.0, 1.0]` and `>= behavior.fact_extract.min_conf`.

Failures bump `facts_rejected_validation`. Accepted facts land
with `source = "llm_extract"` (distinguishable from admin-seeded
facts).

## KV knobs (per bot, `bot.<name>.behavior.fact_extract.*`)

| Key            | Type   | Default | Meaning |
| ---            | ---    | ---     | ---     |
| `enabled`      | bool   | false   | Gate for scheduling + run_once |
| `interval_secs`| uint32 | 300     | Periodic task interval |
| `max_per_hour` | uint32 | 20      | Sweep-rate ceiling (64-slot ring) |
| `min_conf`     | uint32 | 50      | Reject facts with confidence below `N/100` |
| `batch_cap`    | uint32 | 20      | Max rows per sweep |
| `hwm`          | uint64 | 0       | Largest processed `conversation_log.id` |
| `aliases_enabled`       | bool   | false | Gate for LLM-proposed dossier aliases (see below) |
| `aliases_min_conf`      | uint32 | 70    | Reject aliases with confidence below `N/100` |
| `aliases_per_sweep_max` | uint32 | 3     | Ceiling on aliases accepted per sweep |

Changing the interval live requires a chat bot restart — the task
API does not support cancel+reschedule on a running periodic.

## Rate limiting

`behavior.fact_extract.max_per_hour` clamps *sweeps*, not facts. Each call
to `extract_run_once` records its wall time in a per-bot 64-slot
ring; sweeps within the last 3600s are counted. Over the limit
bumps `sweeps_skipped_rate_limited` and returns 0 without
advancing hwm.

## Failure modes

- **Model wraps JSON in prose**: parse fails, no facts written,
  `llm_errors` bumps. Tighten the system prompt.
- **Model invents dossier_ids**: validator rejects,
  `facts_rejected_validation` bumps.
- **Runaway duplicate facts**: `MEM_MERGE_HIGHER_CONF` keeps the
  best one.
- **Crash mid-sweep**: hwm stayed put; next sweep re-processes
  the batch. Upserts are idempotent.
- **Disabled / rate-limited**: `extract_run_once` returns 0;
  `/bot <name> dossiersweep` reports 0 facts.

## Debugging

- Live counters: `extract_get_stats(&s)` exposes
  `sweeps_total`, `sweeps_skipped_rate_limited`, `llm_calls`,
  `llm_errors`, `facts_written`, `facts_rejected_validation`,
  `aliases_written`, `aliases_rejected_validation`.
- Log tag: `extract` via `clam(CLAM_INFO, ...)`. Live-tail with
  `tools/botmanctl -S 7`.
- `/show bot <bot> llm dossiers <pid>` renders the dossier's facts, including
  those written with `source='llm_extract'`.
- `/bot <name> dossiersweep` is the fastest way to trigger a
  sweep when debugging a live session.
- Determinism for manual probing: `llm_test_inject_response(content)`
  (under `LLM_TEST_HOOKS`) resolves the next `llm_chat_submit`
  synchronously with a canned body.

## Aliases

Beyond durable facts, the extraction sweep can also propose
informal nicknames that refer to a known dossier. Example: if the
transcript shows "did jaer finish that PR?" while Jaerchom is a
participant, the LLM may emit
`{"dossier_id": 3, "alias": "jaer", "confidence": 0.9}`. Accepted
aliases are stored as synthetic IRC signature rows on the target
dossier — empty `ident`/`host_tail`/`cloak` prevent the IRC pair
scorer from misrouting inbound messages, while the mention-
resolution token scorer (which only reads `nick`) happily matches
the alias.

### KV knobs (per-bot, under `bot.<name>.behavior.fact_extract.*`)

| Key                     | Type   | Default | Purpose |
| ---                     | ---    | ---     | ---     |
| `aliases_enabled`       | bool   | false   | Feature gate. Off by default. |
| `aliases_min_conf`      | uint32 | 70      | Minimum confidence (0-100) to accept an alias. Stricter than fact `min_conf` because a bad alias cross-contaminates future mention resolution. |
| `aliases_per_sweep_max` | uint32 | 3       | Maximum aliases accepted per sweep. |

### Validation

- `alias` must be 3-32 alphanumeric characters (no spaces,
  punctuation, hyphens, or multibyte UTF-8).
- `dossier_id` must appear in the participants list sent to the LLM.
- `alias` (case-insensitive) must not match the target dossier's
  own canonical nick (LLM regurgitation guard).
- `alias` (case-insensitive) must not match any other dossier's
  canonical nick in the same namespace (cross-contamination guard).
- `confidence` must be in `[0.0, 1.0]` and `>= aliases_min_conf/100`.

Failures bump `aliases_rejected_validation` and log at `CLAM_INFO`.

### Storage shape

Synthetic `dossier_signature.sig_json`:

```json
{"nick":"<alias>","ident":"","host_tail":"","cloak":"","alias":true}
```

The `alias:true` tag is audit-only — scorers ignore unknown fields.
Confidence is intentionally *not* embedded in the JSON: the
`ON CONFLICT (dossier_id, method_kind, sig_json)` upsert keys on
the full JSON text, so stable JSON key order and a confidence-free
payload let re-learns bump `seen_count` instead of creating
duplicate rows.

### Stats

Two counters on `extract_stats_t`:

- `aliases_written`
- `aliases_rejected_validation`

### Caveats

- `dossier_split_signature` can orphan an alias onto an unintended
  split target. Admins should review after splits.
- No bulk-purge admin command yet (follow-up).

## Related docs

- `DOSSIER.md` — dossier resolution + mention matching.
- `MEMSTORE.md` — `dossier_facts` storage and relational
  `toward:<pid>` convention.
- `CHATBOT.md` — chatbot plugin + per-bot KV schema.
- `LLM.md` — `llm_chat_submit` API.
