#!/bin/sh
# Generates version.h with an incremented build number.
# Usage: gen_version_h.sh <BUILDNUM_FILE> <VERSION_MAJOR> <VERSION_MINOR> <OUTPUT_FILE>
#
# Version schema is major.minor.build. The build number counts builds of one
# major.minor pair and restarts at 0 whenever either of them changes, so the
# BUILDNUM file stores the pair it was counting for alongside the count.

BUILDNUM_FILE="$1"
VERSION_MAJOR="$2"
VERSION_MINOR="$3"
OUTPUT="$4"

SERIES="${VERSION_MAJOR}.${VERSION_MINOR}"

NUM=0
if [ -f "$BUILDNUM_FILE" ]; then
  read -r STAMP COUNT < "$BUILDNUM_FILE"
  if [ -z "$COUNT" ]; then
    # Pre-schema file: a bare count with no series stamp. Adopt it as this
    # series' count rather than losing the history.
    COUNT="$STAMP"
    STAMP="$SERIES"
  fi
  if [ "$STAMP" = "$SERIES" ]; then
    NUM="$COUNT"
  fi
fi

NUM=$((NUM + 1))
echo "$SERIES $NUM" > "$BUILDNUM_FILE"

cat > "$OUTPUT" <<EOT
#ifndef BM_VERSION_H
#define BM_VERSION_H

#define BM_VERSION_MAJOR ${VERSION_MAJOR}
#define BM_VERSION_MINOR ${VERSION_MINOR}
#define BM_VERSION_BUILD ${NUM}
#define BM_VERSION_STR   "BotManager v${VERSION_MAJOR}.${VERSION_MINOR}.${NUM}"

#endif
EOT
