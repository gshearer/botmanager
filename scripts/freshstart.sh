#!/bin/bash
# freshstart.sh — Drop all botman tables and launch a clean instance.
# Reads the same bootstrap config that botman uses so the database
# target is always consistent.

usestdin=false
if [[ $1 == "human" ]]; then
  usestdin=true
  shift
fi

set -euo pipefail

CONF="${1:-$HOME/.config/botmanager/botman.conf}"

if [ ! -f "$CONF" ]; then
    echo "error: config not found: $CONF" >&2
    exit 1
fi

# Parse the bootstrap config (KEY="VALUE" format)
get() { sed -n "s/^$1=\"\(.*\)\"/\1/p" "$CONF" | tail -1; }

DBHOST="$(get DBHOST)"  ; DBHOST="${DBHOST:-localhost}"
DBPORT="$(get DBPORT)"  ; DBPORT="${DBPORT:-5432}"
DBNAME="$(get DBNAME)"
DBUSER="$(get DBUSER)"
DBPASS="$(get DBPASS)"

if [ -z "$DBNAME" ] || [ -z "$DBUSER" ]; then
    echo "error: DBNAME and DBUSER must be set in $CONF" >&2
    exit 1
fi

export PGPASSWORD="$DBPASS"

TABLES=(
    bot_methods
    bot_instances
    userns_member
    userns_group
    userns_user
    userns
    kv
)

echo "==> Dropping all botman tables in $DBNAME@$DBHOST:$DBPORT"
for t in "${TABLES[@]}"; do
    psql -h "$DBHOST" -p "$DBPORT" -U "$DBUSER" -d "$DBNAME" -q \
         -c "DROP TABLE IF EXISTS $t CASCADE;"
done
echo "==> Tables dropped."

cd /mnt/fast/doc/projects/botmanager/build

if [[ $usestdin == true ]]; then
  exec core/botman
else
  exec core/botman </dev/null
fi
