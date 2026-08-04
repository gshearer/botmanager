#!/bin/bash
# deploy.sh — copy earbridge to the robot and build it there.
#
# earbridge is the one artifact in this project that runs on another machine:
# raw microphone PCM never leaves the Reachy Mini over REST, so the segmenter
# has to live on the CM4. This script does the two steps that need no root —
# copy and compile — and then PRINTS the privileged step rather than running
# it. Installing a system unit on the robot is an operator gate (G4), and a
# deploy script that quietly sudo's on a device you cannot easily reimage is
# not a convenience.
#
# Prerequisites on the robot (gate G3, measured already satisfied 2026-08-03
# on stock Debian 13): gcc, make, libasound2-dev.
#
# Usage:
#   senses/earbridge/deploy.sh              # copy + build
#   REACHY_HOST=other.host deploy.sh        # different robot
#   REACHY_USER=pi deploy.sh                # different login

set -euo pipefail

REACHY_HOST="${REACHY_HOST:-reachy.iot.hiigara.shearer.tech}"
REACHY_USER="${REACHY_USER:-pollen}"
REMOTE_DIR="${REMOTE_DIR:-earbridge}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET="$REACHY_USER@$REACHY_HOST"

echo "==> deploying to $TARGET:~/$REMOTE_DIR"

if ! ssh -o BatchMode=yes -o ConnectTimeout=10 "$TARGET" true 2>/dev/null; then
  echo "deploy: cannot reach $TARGET over ssh with key auth." >&2
  echo "        The robot is on wifi; check it is powered and on the network." >&2
  exit 1
fi

ssh "$TARGET" "mkdir -p ~/$REMOTE_DIR"

echo "==> copying sources"
scp -q "$HERE/earbridge.c" "$HERE/earbridge.h" "$HERE/Makefile" \
       "$HERE/earbridge.service" "$TARGET:~/$REMOTE_DIR/"

echo "==> building on the robot (aarch64)"
ssh "$TARGET" "cd ~/$REMOTE_DIR && make clean && make" || {
  echo "deploy: remote build failed." >&2
  echo "        If this is 'alsa/asoundlib.h: No such file', gate G3 is not" >&2
  echo "        satisfied: sudo apt install gcc make libasound2-dev" >&2
  exit 1
}

echo "==> built:"
ssh "$TARGET" "ls -l ~/$REMOTE_DIR/earbridge"

cat <<EOF

==> Copy and build are done. The remaining step needs root ON THE ROBOT and is
    deliberately left to the operator (gate G4). Run:

      ssh $TARGET
      sudo cp ~/$REMOTE_DIR/earbridge.service /etc/systemd/system/
      sudo systemctl daemon-reload
      sudo systemctl enable --now earbridge
      systemctl status earbridge

    To try it WITHOUT installing anything first (recommended — this is what
    the RCH-5 verify does):

      ssh $TARGET '~/$REMOTE_DIR/earbridge --take ch0'
      curl -s http://$REACHY_HOST:8090/health

EOF
