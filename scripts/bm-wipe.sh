#!/bin/bash
# bm-wipe.sh — factory defaults for a botmanager instance.
#
# Returns an installation to the state of a first-ever run: every database
# object the daemon owns is dropped, and every runtime artifact it wrote is
# removed. The one thing kept is the bootstrap config file, because it holds
# the database credentials the daemon needs in order to come back up at all.
#
# There are no options and there is no partial mode — that is the point.
# Configuration, credentials, users, quotes, notes, watchlists, knowledge
# corpora, dossiers, conversation history, game state and market candle
# history all go. Take a copy of anything you want to keep BEFORE running
# this; nothing it removes is recoverable afterwards.
#
#   usage: scripts/bm-wipe.sh
#
# Config resolution mirrors the daemon (core/bconf.c):
#   $BOTMAN_CONF, else $XDG_CONFIG_HOME/botmanager/botman.conf,
#   else $HOME/.config/botmanager/botman.conf
#
# Non-interactive callers must set BM_WIPE_ASSUME_YES=1. Without a terminal
# to type the confirmation into, the script refuses rather than assumes.
#
# botmanager — MIT

set -euo pipefail

readonly PROG="${0##*/}"

die() { printf '%s: %s\n' "$PROG" "$*" >&2; exit 1; }
say() { printf '==> %s\n' "$*"; }

[ "$#" -eq 0 ] || die "takes no arguments — it wipes everything, by design"

command -v psql >/dev/null 2>&1 || die "psql not found in PATH"

# ---- bootstrap config ------------------------------------------------------

if [ -n "${BOTMAN_CONF:-}" ]; then
  CONF="$BOTMAN_CONF"
elif [ -n "${XDG_CONFIG_HOME:-}" ]; then
  CONF="$XDG_CONFIG_HOME/botmanager/botman.conf"
else
  CONF="$HOME/.config/botmanager/botman.conf"
fi

[ -f "$CONF" ] || die "config not found: $CONF"

# botman.conf is KEY="VALUE", one per line; the daemon takes the last
# assignment of a key, so this does too.
conf_get() { sed -n "s/^$1=\"\(.*\)\"/\1/p" "$CONF" | tail -1; }

DBHOST="$(conf_get DBHOST)"; DBHOST="${DBHOST:-localhost}"
DBPORT="$(conf_get DBPORT)"; DBPORT="${DBPORT:-5432}"
DBNAME="$(conf_get DBNAME)"
DBUSER="$(conf_get DBUSER)"
DBPASS="$(conf_get DBPASS)"
LOGPATH="$(conf_get LOG_PATH)"

[ -n "$DBNAME" ] && [ -n "$DBUSER" ] || die "DBNAME and DBUSER must be set in $CONF"

export PGPASSWORD="$DBPASS"

psql_q() {
  psql -h "$DBHOST" -p "$DBPORT" -U "$DBUSER" -d "$DBNAME" \
       -v ON_ERROR_STOP=1 -qAt -c "$1"
}

psql_q 'select 1' >/dev/null 2>&1 \
    || die "cannot connect to $DBNAME@$DBHOST:$DBPORT as $DBUSER"

# The pid, lock and socket paths are built from $HOME directly in
# core/main.c and core/botmanctl.c — they do not consult XDG_CONFIG_HOME —
# so this is $HOME even when the config itself came from somewhere else.
RUNDIR="$HOME/.config/botmanager"
DATADIR="$HOME/.local/share/botmanager"

# ---- what is about to be destroyed -----------------------------------------

n_tables="$(psql_q "select count(*) from pg_tables where schemaname = current_schema();")"

# Credential rows are named, never printed. They are installed by hand and
# live in no file anywhere, so the only defence against silently losing one
# is knowing which ones you had.
creds=""
if [ "$(psql_q "select to_regclass('kv') is not null;")" = "t" ]; then
  creds="$(psql_q "select key from kv
                    where key ~ '(\.creds\.|password|passphrase|secret|apikey|api_key|private_key|operator_name)'
                      and value <> ''
                    order by key;")"
fi

echo
echo "  This is a FACTORY RESET of the botmanager instance below."
echo
printf '  database   %s@%s:%s as %s (%s tables)\n' \
    "$DBNAME" "$DBHOST" "$DBPORT" "$DBUSER" "$n_tables"
printf '  runtime    %s (socket, pidfile, lockfile)\n' "$RUNDIR"
printf '  data       %s\n' "$DATADIR"
[ -n "$LOGPATH" ] && printf '  log        %s\n' "$LOGPATH"
printf '  KEPT       %s\n' "$CONF"
echo

if [ -n "$creds" ]; then
  echo "  Credential keys that will be emptied — reinstall each by hand"
  echo "  afterwards; no file in this tree holds them:"
  printf '    %s\n' $creds
  echo
fi

# ---- confirm ---------------------------------------------------------------

if [ "${BM_WIPE_ASSUME_YES:-0}" = "1" ]; then
  say "BM_WIPE_ASSUME_YES=1 — proceeding without confirmation"
elif [ -t 0 ]; then
  printf '  Type WIPE to continue: '
  read -r answer
  [ "$answer" = "WIPE" ] || die "aborted"
else
  die "no terminal to confirm on; set BM_WIPE_ASSUME_YES=1 to proceed"
fi

# ---- stop the daemon -------------------------------------------------------

pid=""
if [ -r "$RUNDIR/botman.pid" ]; then
  pid="$(tr -dc '0-9' < "$RUNDIR/botman.pid")"
  # A pidfile outlives a crash. Only believe one whose process is alive.
  if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
      pid=""
  fi
fi
if [ -z "$pid" ] && command -v pgrep >/dev/null 2>&1; then
  pid="$(pgrep -x -u "$(id -u)" botman 2>/dev/null | head -1 || true)"
fi

if [ -n "$pid" ]; then
  say "stopping botman (pid $pid)"
  kill -15 "$pid" 2>/dev/null || true
  # SIGTERM, 15 s, then SIGKILL. The drain can take a while when a download
  # or a backtest is in flight, and a daemon still alive when the new one
  # starts collides on the socket bind.
  for _ in $(seq 1 15); do
      kill -0 "$pid" 2>/dev/null || break
      sleep 1
  done
  if kill -0 "$pid" 2>/dev/null; then
      say "did not exit on SIGTERM after 15 s — SIGKILL"
      kill -9 "$pid" 2>/dev/null || true
      sleep 1
  fi
else
  say "no running daemon"
fi

# ---- drop every object the daemon owns -------------------------------------
#
# Enumerated from the catalog rather than from a hand-written list. Every
# maintained list of table names this project has kept went stale — a plugin
# adds a table, the list does not, and the wipe silently leaves state behind
# for the next instance to inherit. The catalog cannot go stale.
#
# Extension-owned objects are skipped: an extension is installed into the
# database by an administrator, not created by botmanager, and dropping one
# is not this script's business.

say "dropping every object in the daemon's schema"

psql -h "$DBHOST" -p "$DBPORT" -U "$DBUSER" -d "$DBNAME" -v ON_ERROR_STOP=1 -q <<'SQL'
DO $wipe$
DECLARE
  r record;
BEGIN
  -- Views first: a view over a table would otherwise force its CASCADE.
  FOR r IN SELECT c.oid::regclass AS ident, c.relkind AS kind
             FROM pg_class c
             JOIN pg_namespace n ON n.oid = c.relnamespace
            WHERE n.nspname = current_schema()
              AND c.relkind IN ('v', 'm')
              AND NOT EXISTS (SELECT 1 FROM pg_depend d
                               WHERE d.objid = c.oid AND d.deptype = 'e')
  LOOP
    IF r.kind = 'm' THEN
      EXECUTE format('DROP MATERIALIZED VIEW IF EXISTS %s CASCADE', r.ident);
    ELSE
      EXECUTE format('DROP VIEW IF EXISTS %s CASCADE', r.ident);
    END IF;
  END LOOP;

  -- Tables. CASCADE takes their indexes, constraints, triggers, owned
  -- sequences and the composite type each one carries.
  FOR r IN SELECT c.oid::regclass AS ident
             FROM pg_class c
             JOIN pg_namespace n ON n.oid = c.relnamespace
            WHERE n.nspname = current_schema()
              AND c.relkind IN ('r', 'p', 'f')
              AND NOT EXISTS (SELECT 1 FROM pg_depend d
                               WHERE d.objid = c.oid AND d.deptype = 'e')
  LOOP
    EXECUTE format('DROP TABLE IF EXISTS %s CASCADE', r.ident);
  END LOOP;

  -- Sequences that no table owned.
  FOR r IN SELECT c.oid::regclass AS ident
             FROM pg_class c
             JOIN pg_namespace n ON n.oid = c.relnamespace
            WHERE n.nspname = current_schema()
              AND c.relkind = 'S'
              AND NOT EXISTS (SELECT 1 FROM pg_depend d
                               WHERE d.objid = c.oid AND d.deptype = 'e')
  LOOP
    EXECUTE format('DROP SEQUENCE IF EXISTS %s CASCADE', r.ident);
  END LOOP;

  -- Stored routines. whenmoon installs wm_candle_upsample() at every
  -- daemon start (plugins/extension/whenmoon/sql/candle_upsample.sql), so
  -- dropping it here is safe: the next start puts it back.
  FOR r IN SELECT p.oid::regprocedure AS ident, p.prokind AS kind
             FROM pg_proc p
             JOIN pg_namespace n ON n.oid = p.pronamespace
            WHERE n.nspname = current_schema()
              AND NOT EXISTS (SELECT 1 FROM pg_depend d
                               WHERE d.objid = p.oid AND d.deptype = 'e')
  LOOP
    IF r.kind = 'p' THEN
      EXECUTE format('DROP PROCEDURE IF EXISTS %s CASCADE', r.ident);
    ELSE
      EXECUTE format('DROP FUNCTION IF EXISTS %s CASCADE', r.ident);
    END IF;
  END LOOP;

  -- Standalone enums and domains. A table's composite type went with the
  -- table above, which is why relkind 'c' is not considered here.
  FOR r IN SELECT t.oid::regtype AS ident
             FROM pg_type t
             JOIN pg_namespace n ON n.oid = t.typnamespace
            WHERE n.nspname = current_schema()
              AND t.typtype IN ('e', 'd')
              AND NOT EXISTS (SELECT 1 FROM pg_depend d
                               WHERE d.objid = t.oid AND d.deptype = 'e')
  LOOP
    EXECUTE format('DROP TYPE IF EXISTS %s CASCADE', r.ident);
  END LOOP;
END
$wipe$;
SQL

left="$(psql_q "select count(*) from pg_tables where schemaname = current_schema();")"
[ "$left" = "0" ] || die "$left table(s) survived the drop — check ownership of $DBNAME"

say "database empty ($n_tables table(s) dropped, plus every view, sequence, routine and enum)"

# ---- runtime and data artifacts --------------------------------------------

rm -f "$RUNDIR/botman.sock" "$RUNDIR/botman.pid" "$RUNDIR/botman.lock"
say "removed socket, pidfile and lockfile from $RUNDIR"

if [ -n "$LOGPATH" ] && [ -f "$LOGPATH" ]; then
  rm -f "$LOGPATH"
  say "removed log $LOGPATH"
fi

if [ -d "$DATADIR" ]; then
  rm -rf "${DATADIR:?}"
  say "removed data directory $DATADIR"
fi

echo
say "factory reset complete."
echo
echo "  The daemon recreates its own schema on the next start; nothing here"
echo "  needs to run first. What it will NOT recreate is configuration —"
echo "  bots, methods, user namespaces, LLM registry rows and every KV knob"
echo "  are gone with the database, and API credentials must be reinstalled"
echo "  by hand because no file in this tree holds them."
echo
