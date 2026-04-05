-- BotManager initial database schema
-- This file creates the tables required by the core subsystems.

-- KV configuration table: stores all hierarchical key/value configuration.
-- Keys use dot-separated namespacing (core.*, plugin.*, bot.*).
-- The application also creates this table via CREATE TABLE IF NOT EXISTS
-- on startup, but this file serves as the canonical schema definition.

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
-- Password fields are populated by the authentication subsystem (future).

CREATE TABLE IF NOT EXISTS userns_user (
  id         SERIAL       PRIMARY KEY,
  ns_id      INTEGER      NOT NULL REFERENCES userns(id) ON DELETE CASCADE,
  username   VARCHAR(64)  NOT NULL,
  passhash   TEXT,
  created    TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
  UNIQUE(ns_id, username)
);

-- Groups within a namespace. Group names are unique per namespace.

CREATE TABLE IF NOT EXISTS userns_group (
  id       SERIAL       PRIMARY KEY,
  ns_id    INTEGER      NOT NULL REFERENCES userns(id) ON DELETE CASCADE,
  name     VARCHAR(64)  NOT NULL,
  created  TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
  UNIQUE(ns_id, name)
);

-- Group membership: maps users to groups within the same namespace.

CREATE TABLE IF NOT EXISTS userns_member (
  user_id   INTEGER  NOT NULL REFERENCES userns_user(id) ON DELETE CASCADE,
  group_id  INTEGER  NOT NULL REFERENCES userns_group(id) ON DELETE CASCADE,
  PRIMARY KEY(user_id, group_id)
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
