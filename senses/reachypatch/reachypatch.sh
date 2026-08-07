#!/bin/sh
# botmanager — MIT
# Re-apply our tuning to a Reachy Mini daemon's vendored Python.
#
# The daemon exposes no configuration for how hard it wobbles its head
# while speaking: not an env var, not a config file, and not a parameter
# on POST /api/media/wobbling/enable. The only lever is a module
# constant inside its venv, which means the tuning is vendor code — it
# does not survive a daemon reinstall, and it does not exist on a robot
# nobody has patched.
#
# So it lives here instead. Run it against each robot after any daemon
# upgrade, and against every NEW robot once. It is idempotent: it keeps
# one pristine .orig sidecar, always rewrites from a known pattern, and
# reports what the file holds afterwards.
#
#   ./reachypatch.sh                          # default host, sway 0.75
#   ./reachypatch.sh --host reachy2.local     # a second robot
#   ./reachypatch.sh --sway-master 1.0        # dial it back up
#   ./reachypatch.sh --sway-db-high -6        # unpin the loudness curve
#   ./reachypatch.sh --revert                 # restore the vendor file
#   ./reachypatch.sh --show                   # read, change nothing
#
# A knob left unnamed on the command line is left ALONE in the file, so
# the two can be tuned independently and in either order.
#
# ⚠ Re-check this on every daemon upgrade. If Pollen ever exposes the
# amplitude through the API, this script should be DELETED rather than
# maintained — see plugins/method/reachy/TODO.md §F.

set -eu

HOST="reachy.iot.hiigara.shearer.tech"
USER="pollen"
SWAY_MASTER="0.75"          # vendor default is 1.5; this is half the throw
SWAY_DB_HIGH=""             # vendor default is -18.0; empty = leave it alone
ACTION="patch"

TAPPER="/venvs/mini_daemon/lib/python3.12/site-packages/reachy_mini/motion/speech_tapper.py"
UNIT="reachy-mini-daemon"
MARK="# botmanager (senses/reachypatch) — vendor defaults 1.5 / -18.0"

usage()
{
  sed -n '3,26p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --host)        HOST="$2"; shift 2 ;;
    --user)        USER="$2"; shift 2 ;;
    --sway-master) SWAY_MASTER="$2"; shift 2 ;;
    --sway-db-high) SWAY_DB_HIGH="$2"; shift 2 ;;
    --revert)      ACTION="revert"; shift ;;
    --show)        ACTION="show"; shift ;;
    -h|--help)     usage 0 ;;
    *)             echo "unknown argument: $1" >&2; usage 1 ;;
  esac
done

RSH="ssh -o ConnectTimeout=10 ${USER}@${HOST}"

say() { printf '%s\n' "$*"; }

# Everything the robot has to do, in one round trip per action — the
# WiFi is flaky by design assumption (charter C2), so fewer is better.
case "$ACTION" in
  show)
    say "== ${HOST}"
    $RSH "grep -nE '^(SWAY_MASTER|SWAY_DB_HIGH)' '${TAPPER}'; \
          ls -l '${TAPPER}.orig' 2>/dev/null || echo '(no .orig — never patched)'"
    ;;

  revert)
    say "== reverting ${HOST}"
    $RSH "test -f '${TAPPER}.orig' || { echo 'no .orig to restore'; exit 1; }; \
          sudo cp '${TAPPER}.orig' '${TAPPER}'; \
          grep -n '^SWAY_MASTER' '${TAPPER}'; \
          sudo systemctl restart '${UNIT}'"
    say "reverted and daemon restarted."
    ;;

  patch)
    # Only the knobs actually named are rewritten; the rest of the
    # vendor file — including the other knob — is left untouched.
    SED="-e 's|^SWAY_MASTER *=.*|SWAY_MASTER = ${SWAY_MASTER}  ${MARK}|'"
    say "== patching ${HOST} (SWAY_MASTER=${SWAY_MASTER})"

    if [ -n "$SWAY_DB_HIGH" ]; then
      SED="${SED} -e 's|^SWAY_DB_HIGH *=.*|SWAY_DB_HIGH = ${SWAY_DB_HIGH}  ${MARK}|'"
      say "   and SWAY_DB_HIGH=${SWAY_DB_HIGH}"
    fi

    # The .orig is written ONCE, from whatever the vendor shipped. Never
    # overwrite it: a second run would otherwise snapshot our own patch
    # and there would be nothing left to revert to.
    $RSH "set -eu; \
          test -f '${TAPPER}.orig' || sudo cp -p '${TAPPER}' '${TAPPER}.orig'; \
          sudo sed -i ${SED} '${TAPPER}'; \
          grep -nE '^(SWAY_MASTER|SWAY_DB_HIGH)' '${TAPPER}'; \
          sudo systemctl restart '${UNIT}'"

    # The daemon takes a few seconds to serve again; the caller almost
    # always wants to know it came back, so wait rather than making them.
    say "waiting for the daemon to answer..."
    i=0
    while [ "$i" -lt 30 ]; do
      if curl -sf -m 3 "http://${HOST}:8000/api/media/status" >/dev/null 2>&1; then
        say "daemon is up."
        say ""
        say "⚠ THE RESTART DISCARDED EVERY CONNECT-TIME SETTING on this robot."
        say "  Wobbling, face tracking and volume are runtime-only state in the"
        say "  daemon — nothing persists them, and no endpoint even reports them."
        say "  A bound bot will speak with a MOTIONLESS HEAD until connect() runs"
        say "  again. 'wake' is NOT enough; it only touches the motors:"
        say ""
        say "      botmanctl 'bot stop <name>' && botmanctl 'bot start <name>'"
        say ""
        exit 0
      fi
      i=$((i + 1))
      sleep 1
    done

    say "⚠ daemon did not answer within 30s — check 'journalctl -u ${UNIT}' on the robot."
    exit 1
    ;;
esac
