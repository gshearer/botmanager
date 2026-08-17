-- BotManager initial database schema
-- This file creates the tables required by the core subsystems.
-- The application also creates these tables via CREATE TABLE IF NOT EXISTS
-- on startup, but this file serves as the canonical schema definition.

-- KV configuration table: stores all hierarchical key/value configuration.
-- Keys use dot-separated namespacing (core.*, plugin.*, bot.*).

CREATE TABLE IF NOT EXISTS kv (
  key   TEXT     PRIMARY KEY,
  type  SMALLINT NOT NULL,
  value TEXT     NOT NULL
);

-- User namespaces: shared identity/auth/authorization stores.
-- A namespace exists independently of bot instances; multiple bots may
-- share the same namespace. Created on first request.

CREATE TABLE IF NOT EXISTS userns (
  id       SERIAL       PRIMARY KEY,
  name     VARCHAR(64)  NOT NULL UNIQUE,
  created  TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);

-- Users within a namespace. Usernames are unique per namespace.
-- Passwords are hashed with argon2id and stored in passhash.
-- UUID is auto-generated at creation time for stable external identification.

CREATE TABLE IF NOT EXISTS userns_user (
  id             SERIAL       PRIMARY KEY,
  ns_id          INTEGER      NOT NULL REFERENCES userns(id) ON DELETE CASCADE,
  username       VARCHAR(64)  NOT NULL,
  uuid           VARCHAR(36)  NOT NULL DEFAULT '',
  passhash       TEXT,
  description    VARCHAR(101) NOT NULL DEFAULT '',
  passphrase     VARCHAR(101) NOT NULL DEFAULT '',
  autoidentify   BOOLEAN      NOT NULL DEFAULT FALSE,
  lastseen       TIMESTAMPTZ,
  lastseen_method VARCHAR(64) NOT NULL DEFAULT '',
  lastseen_mfa   VARCHAR(200) NOT NULL DEFAULT '',
  created        TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
  UNIQUE(ns_id, username)
);

-- Groups within a namespace. Group names are unique per namespace.

CREATE TABLE IF NOT EXISTS userns_group (
  id           SERIAL       PRIMARY KEY,
  ns_id        INTEGER      NOT NULL REFERENCES userns(id) ON DELETE CASCADE,
  name         VARCHAR(64)  NOT NULL,
  description  VARCHAR(101) NOT NULL DEFAULT '',
  created      TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
  UNIQUE(ns_id, name)
);

-- Group membership: maps users to groups within the same namespace.
-- Level is the privilege level (0-65535, higher = more access).

CREATE TABLE IF NOT EXISTS userns_member (
  user_id   INTEGER  NOT NULL REFERENCES userns_user(id) ON DELETE CASCADE,
  group_id  INTEGER  NOT NULL REFERENCES userns_group(id) ON DELETE CASCADE,
  level     INTEGER  NOT NULL DEFAULT 0,
  PRIMARY KEY(user_id, group_id)
);

-- Multi-MFA patterns per user. Patterns are in handle!username@hostname
-- format with glob matching (* and ?) on all three components.

CREATE TABLE IF NOT EXISTS user_mfa (
  id       SERIAL   PRIMARY KEY,
  user_id  INTEGER  NOT NULL REFERENCES userns_user(id) ON DELETE CASCADE,
  pattern  VARCHAR(200) NOT NULL,
  UNIQUE(user_id, pattern)
);

-- Bot instances: persisted bot configuration for restart recovery.
-- Each row represents a configured bot instance with its driver kind,
-- optional user namespace, and auto-start preference.

CREATE TABLE IF NOT EXISTS bot_instances (
  name         VARCHAR(64)  PRIMARY KEY,
  kind         VARCHAR(64)  NOT NULL,
  userns_name  VARCHAR(64),
  auto_start   BOOLEAN      NOT NULL DEFAULT FALSE,
  created      TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);

-- Bot method bindings: which method kinds are bound to each bot.
-- The method_kind (e.g., "irc") is combined with the bot name at
-- runtime to form the method instance name (e.g., "mybot_irc").

CREATE TABLE IF NOT EXISTS bot_methods (
  bot_name     VARCHAR(64)  NOT NULL REFERENCES bot_instances(name) ON DELETE CASCADE,
  method_kind  VARCHAR(64)  NOT NULL,
  PRIMARY KEY(bot_name, method_kind)
);

-- LLM registry: deliberately NOT defined here.
--
-- The inference engine owns four tables — llm_services, llm_models,
-- llm_service_models (the cached /models listing) and llm_model_params
-- (learned request-dialect directives) — and creates them itself in
-- llm_ensure_tables() (plugins/extension/inference/engine/llm.c).
--
-- A copy in this file is worse than no copy. Everything here runs
-- BEFORE the daemon starts, so a stale CREATE TABLE IF NOT EXISTS wins
-- the race and the engine's correct definition is silently skipped: the
-- table exists, so nothing errors, and every later insert fails against
-- columns that were renamed a refactor ago. That is exactly what this
-- file did until 2026-08-05, when it still described the pre-service
-- shape (endpoint_url / api_key_kv, no service_name).
--
-- The bearer token is not in any of these tables in any case; it lives
-- in KV under llm.service.<name>.creds.apikey.

-- Personalities: named LLM system-prompt + behavior bundles loaded by
-- the llm bot driver. The loader/parser lands in Chunk E; the table
-- is defined here for schema co-location.

CREATE TABLE IF NOT EXISTS personalities (
  name           VARCHAR(64)  PRIMARY KEY,
  description    VARCHAR(200) NOT NULL DEFAULT '',
  body           TEXT         NOT NULL,
  -- Output contract: wire-format rules (no parentheticals, sentinel
  -- token for opting out of a turn, etc). Required field, resolved at
  -- personality-load time from the path given by the personality
  -- file's "contract:" frontmatter. The contract content is stored
  -- inline rather than referenced by name so a personality is a
  -- self-contained behavioral definition with no live cross-table
  -- coupling. Multiple personalities can include the same contract
  -- file at load time; updating the contract = reload the personas.
  contract_body  TEXT         NOT NULL DEFAULT '',
  -- Topic list for the acquisition engine. Stored opaquely as JSON
  -- verbatim from the personality file's `interests:` frontmatter
  -- block; parsed at bot start into acquire_topic_t[] by
  -- llmbot_interests_parse. Empty = bot has no topics; no acquisition.
  interests_json TEXT         NOT NULL DEFAULT '',
  version        INTEGER      NOT NULL DEFAULT 1,
  updated        TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);
-- Knowledge-corpus binding now lives on the bot instance, not the
-- personality: set `bot.<name>.corpus` to a semicolon-separated
-- list of corpus names to enable retrieval. See KNOWLEDGE.md.

-- The memory subsystem's tables (conversation_log,
-- conversation_embeddings) used to be declared here. Chunk R1 re-homed
-- the memory subsystem into plugins/bot/chat/ and
-- memory_register_config() already ensures these tables idempotently at
-- plugin init time, so a fresh Postgres only grows them once the chat
-- plugin loads. See plugins/bot/chat/MEMSTORE.md.
--
-- The user-keyed fact store (user_facts, user_fact_embeddings) was
-- deleted 2026-08-07: facts hang off a dossier, not a userns_user.

-- Dossiers: the llm bot's single source of truth for participant
-- memory. A dossier represents one real person observed across chat
-- rooms, identified by a pluggable per-method signature (IRC nick +
-- ident + host-tail; Discord stable user-id; etc.). user_id is an
-- Dossier tables (dossier / dossier_signature / dossier_facts) live
-- in plugins/bot/chat/ as of R4. The chat plugin's dossier_init path
-- creates them on first run via dossier_register_config(); they are
-- not declared here.

-- Knowledge store: corpus-scoped RAG chunks for per-persona external
-- knowledge (Arch wiki, SEP, etc.). Parallel to dossier_facts /
-- conversation_log; same float32 LE BYTEA + cosine retrieval machinery.
-- See KNOWLEDGE.md and engine/inference.h, both under
-- plugins/extension/inference/.

CREATE TABLE IF NOT EXISTS knowledge_corpora (
  name           VARCHAR(64)  PRIMARY KEY,
  description    VARCHAR(200) NOT NULL DEFAULT '',
  last_ingested  TIMESTAMPTZ,
  created        TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS knowledge_chunks (
  id              BIGSERIAL    PRIMARY KEY,
  corpus          VARCHAR(64)  NOT NULL REFERENCES knowledge_corpora(name) ON DELETE CASCADE,
  source_url      TEXT         NOT NULL DEFAULT '',
  section_heading TEXT         NOT NULL DEFAULT '',
  text            TEXT         NOT NULL,
  created         TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);
CREATE INDEX IF NOT EXISTS idx_knowledge_chunks_corpus ON knowledge_chunks(corpus);
-- Ingest idempotency (OBS-16): a chunk is identified by its corpus, its
-- source, its heading and its content. md5 and not sha256 because
-- sha256 needs a bytea, convert_to() is only STABLE so an index may not
-- call it, and text::bytea reinterprets the value. The heading is in the
-- key because two sections legitimately carry the same body.
-- See plugins/extension/inference/KNOWLEDGE.md §Idempotency.
CREATE UNIQUE INDEX IF NOT EXISTS knowledge_chunks_dedup
  ON knowledge_chunks(corpus, source_url, section_heading, md5(text));

CREATE TABLE IF NOT EXISTS knowledge_chunk_embeddings (
  chunk_id  BIGINT      PRIMARY KEY REFERENCES knowledge_chunks(id) ON DELETE CASCADE,
  model     VARCHAR(64) NOT NULL,
  dim       INTEGER     NOT NULL,
  vec       BYTEA       NOT NULL
);

-- Images harvested off pages ingested into knowledge_chunks. One row per
-- <img> / og:image / twitter:image discovered during acquire, linked
-- back to the chunk that the surrounding page produced. No embeddings;
-- retrieval is identity-based (JOIN on chunk_id for attached images,
-- or subject-scan for explicit image intent).
CREATE TABLE IF NOT EXISTS knowledge_images (
  id         BIGSERIAL    PRIMARY KEY,
  chunk_id   BIGINT       REFERENCES knowledge_chunks(id) ON DELETE CASCADE,
  url        TEXT         NOT NULL,
  page_url   TEXT,
  caption    TEXT,
  subject    VARCHAR(128),
  width_px   INT,
  height_px  INT,
  created    TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);
CREATE INDEX IF NOT EXISTS idx_knowledge_images_chunk
    ON knowledge_images(chunk_id);
CREATE INDEX IF NOT EXISTS idx_knowledge_images_subject
    ON knowledge_images(subject);

-- Acquisition engine: autonomous + reactive knowledge learning. The
-- engine ticks per-bot, runs proactive SXNG queries for topics declared
-- in each bot's config, digests + ingests results into a knowledge
-- corpus. See ACQUIRE.md and engine/inference.h, both under
-- plugins/extension/inference/.

CREATE TABLE IF NOT EXISTS acquire_topic_stats (
  bot_name       VARCHAR(64)  NOT NULL,
  topic_name     VARCHAR(64)  NOT NULL,
  last_proactive TIMESTAMPTZ,
  last_reactive  TIMESTAMPTZ,
  total_queries  BIGINT NOT NULL DEFAULT 0,
  total_ingested BIGINT NOT NULL DEFAULT 0,
  PRIMARY KEY (bot_name, topic_name)
);

-- Late-add FK on conversation_log.dossier_id moved into the chat
-- plugin's memory_ensure_tables alongside the conversation_log table
-- itself (R1). Nothing to do here.

-- Exchange symbol/pair caches (EXCH-PRIME-1). Each exchange service
-- plugin persists its symbol/pair listing here so a daemon restart can
-- prime the in-memory cache from the DB instead of re-hitting the
-- exchange's REST surface on every boot. The network refresh only fires
-- when the snapshot is missing or older than the per-exchange
-- plugin.<exch>.{symbols,assetpairs}_refresh_sec staleness window. The
-- application mirrors these via CREATE TABLE IF NOT EXISTS on startup
-- (gem_symbols_ensure_table / kr_assetpairs_ensure_table).

-- Gemini: native concatenated symbol (lowercase, e.g. btcusd) is the
-- key; base/quote are the split ISO codes. The hyphenated abstraction
-- form is re-derived by gem_pairs_add on load, so it is not stored.
CREATE TABLE IF NOT EXISTS gemini_symbols (
  native     VARCHAR(16)  PRIMARY KEY,
  base       VARCHAR(8)   NOT NULL,
  quote      VARCHAR(8)   NOT NULL,
  fetched_at TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);

-- Kraken: altname (e.g. XBTUSD) is the key; canonical is the API pair
-- name (XXBTZUSD), wsname is the WS v2 name with the leading XBT/ →
-- BTC/ rewrite already applied (kr_pairs_add only rewrites a leading
-- XBT/, which the stored value no longer carries).
CREATE TABLE IF NOT EXISTS kraken_assetpairs (
  altname    VARCHAR(16)  PRIMARY KEY,
  canonical  VARCHAR(16)  NOT NULL,
  wsname     VARCHAR(32)  NOT NULL,
  fetched_at TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);
