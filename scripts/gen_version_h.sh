#!/bin/sh
# Generates version.h with an incremented build number.
# Usage: gen_version_h.sh <BUILDNUM_FILE> <VERSION_MAJOR> <VERSION_MINOR> <OUTPUT_FILE>

BUILDNUM_FILE="$1"
VERSION_MAJOR="$2"
VERSION_MINOR="$3"
OUTPUT="$4"

if [ ! -f "$BUILDNUM_FILE" ]; then
  echo 0 > "$BUILDNUM_FILE"
fi

NUM=$(cat "$BUILDNUM_FILE")
NUM=$((NUM + 1))
echo "$NUM" > "$BUILDNUM_FILE"

cat > "$OUTPUT" <<EOF
#ifndef BM_VERSION_H
#define BM_VERSION_H

#define BM_VERSION_MAJOR ${VERSION_MAJOR}
#define BM_VERSION_MINOR ${VERSION_MINOR}
#define BM_BUILD_NUM     ${NUM}
#define BM_VERSION_STR   "BotManager v${VERSION_MAJOR}.${VERSION_MINOR} (build #${NUM})"

#endif
EOF
