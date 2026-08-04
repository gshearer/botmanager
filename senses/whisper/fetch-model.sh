#!/bin/bash
# fetch-model.sh — Fetch a whisper.cpp ggml model and prove it arrived intact
# (RCH-2, reachy senses).
#
# Upstream ships models/download-ggml-model.sh, and this script prefers it
# when the whisper.cpp checkout is present. What upstream does NOT do is
# verify what landed: a proxy error page, a truncated transfer or a resumed
# download against a changed remote all leave a file that whisper-server
# only rejects minutes later, at model-load time, with an opaque message.
# So every path through this script ends in the same three checks:
#
#   1. magic   — the first four bytes are "ggml" (or "GGUF" for newer files)
#   2. size    — at least 90% of the model's published size
#   3. sha256  — matched against a sidecar written on the first good fetch,
#                so a later re-run can tell "already have it" from "have
#                something else now"
#
# Re-running with the model already in place is a no-op that costs three
# checks and no network. Nothing here needs root, and nothing is installed.
#
# Usage:
#   senses/whisper/fetch-model.sh                 # large-v3-turbo, the pinned model
#   senses/whisper/fetch-model.sh base.en         # any upstream model name
#
# Environment overrides:
#   DEST_DIR   where the .bin lands     (default: ~/src/whisper.cpp/models)
#   WHISPER_DIR whisper.cpp checkout    (default: ~/src/whisper.cpp)
#   SHA256     expected digest; when    (default: unset — the sidecar written
#              set it is enforced and             by the first good fetch is
#              replaces the sidecar               the reference instead)
#   FORCE      any non-empty value      (default: unset)
#              re-fetches even if the
#              model is already valid

set -euo pipefail

MODEL="${1:-large-v3-turbo}"
WHISPER_DIR="${WHISPER_DIR:-$HOME/src/whisper.cpp}"
DEST_DIR="${DEST_DIR:-$WHISPER_DIR/models}"
HF_BASE="https://huggingface.co/ggerganov/whisper.cpp/resolve/main"

BIN="$DEST_DIR/ggml-${MODEL}.bin"
SIDECAR="$BIN.sha256"

# Published sizes, in MiB, for the models anyone here would plausibly ask
# for. Used only as a floor (90% of nominal) — the point is to catch an HTML
# error page or a half-transfer, not to pin a byte count that upstream is
# free to change. An unlisted model falls back to a generic 10 MiB floor.
nominal_mib() {
    case "$1" in
        tiny|tiny.en)               echo 75    ;;
        base|base.en)               echo 142   ;;
        small|small.en)             echo 466   ;;
        medium|medium.en)           echo 1500  ;;
        large-v3-turbo)             echo 1550  ;;
        large-v1|large-v2|large-v3) echo 3095  ;;
        *)                          echo 10    ;;
    esac
}

die() { echo "fetch-model: $*" >&2 ; exit 1 ; }

# --- the three checks -------------------------------------------------------

check_magic() {
    local magic
    magic=$(head -c 4 "$BIN" | tr -d '\0')
    case "$magic" in
        ggml|GGUF) return 0 ;;
        *) echo "    magic is '$magic', expected 'ggml' or 'GGUF'" >&2 ; return 1 ;;
    esac
}

check_size() {
    local bytes floor
    bytes=$(stat -c %s "$BIN")
    floor=$(( $(nominal_mib "$MODEL") * 1024 * 1024 * 9 / 10 ))
    if [ "$bytes" -lt "$floor" ]; then
        echo "    $bytes bytes is short of the $floor byte floor for $MODEL" >&2
        return 1
    fi
    echo "    size   $(( bytes / 1024 / 1024 )) MiB"
}

check_sha256() {
    local got want
    got=$(sha256sum "$BIN" | cut -d' ' -f1)
    want="${SHA256:-}"
    if [ -z "$want" ] && [ -r "$SIDECAR" ]; then
        want=$(cut -d' ' -f1 < "$SIDECAR")
    fi

    if [ -z "$want" ]; then
        echo "$got  ggml-${MODEL}.bin" > "$SIDECAR"
        echo "    sha256 $got (recorded — later runs verify against it)"
        return 0
    fi
    if [ "$got" != "$want" ]; then
        echo "    sha256 $got does not match the expected $want" >&2
        return 1
    fi
    echo "    sha256 $got (matches)"
}

verify() {
    echo "==> verifying $BIN"
    check_magic && check_size && check_sha256
}

# --- fetch ------------------------------------------------------------------

if [ -f "$BIN" ] && [ -z "${FORCE:-}" ]; then
    if verify; then
        echo "==> $MODEL is already present and intact — nothing to do"
        exit 0
    fi
    echo "==> the model on disk failed verification — refetching"
fi

mkdir -p "$DEST_DIR"

if [ -x "$WHISPER_DIR/models/download-ggml-model.sh" ] && [ "$DEST_DIR" = "$WHISPER_DIR/models" ]; then
    echo "==> fetching $MODEL via the upstream downloader"
    ( cd "$WHISPER_DIR" && sh ./models/download-ggml-model.sh "$MODEL" )
else
    echo "==> fetching $MODEL from $HF_BASE"
    if ! curl -L --fail --progress-bar -o "$BIN.part" "$HF_BASE/ggml-${MODEL}.bin"; then
        rm -f "$BIN.part"
        die "download failed — is '$MODEL' a real upstream model name?"
    fi
    mv "$BIN.part" "$BIN"
fi

[ -f "$BIN" ] || die "$BIN is missing after the fetch"

# A fresh fetch invalidates any recorded digest: the sidecar describes the
# file that WAS there, and re-recording is the whole point of FORCE.
if [ -n "${FORCE:-}" ]; then
    rm -f "$SIDECAR"
fi

verify || die "the fetched model failed verification — delete $BIN and retry"

echo "==> $MODEL ready at $BIN"
