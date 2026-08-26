#!/usr/bin/env bash
#
# tool_refusal_probe.sh — what does a CHANNEL get when a tool refuses?
#
# The interpret cue hands a command's output to a persona and the persona
# speaks it.  When the command REFUSES its arguments, the sink carries the
# refusal as an ordinary line — cmd_sink_deliver takes (id, const char *) and
# nothing else — so the only thing standing between a usage line and the
# channel is the wording of personalities/base.txt's [[tool-answer]] block.
# This measures what actually comes out.
#
# ── why /in and not the NL bridge ────────────────────────────────────────────
# The bridge picks its own slash-line, so a probe through it measures the
# model's word choice as much as the cue.  The deferred spine arms the SAME
# capture (chatbot_interpret_begin, deferred.c:506) from a TYPED command, so
# the verb and its arguments are ours and the only model in the loop is the
# one under test.  deferred.c:1071 says this is deliberate: "the command is
# the authority on its own arguments, so a bad-args row voices the command's
# own error through the capture when it fires."
#
# ── the two arms ─────────────────────────────────────────────────────────────
#   treat    !in 1m wotd bogus      wordnik_parse_args -> bad_arg -> usage line
#   control  !in 1m dict serendipity  a real answer, different subject
#
# ⚠⚠ ORDER IS A CONFOUND AND IT BIT THIS PROBE.  The bot answers from the
# channel as well as the wire, so a control run that FOLLOWS three refusals
# about the same command inherits them — the first such run here came back
# "the tool actually worked this time", which is a reference to the failures
# before it, not an independent reading.  Run one arm per invocation, and
# give the control a different command AND a different subject.  This is the
# same trap tests/wikimedia/battery.sh records for mode B.
#
# ⚠ An identical prompt does not get an identical reply, so a single run
# measures the sampler (plugins/bot/chat/AGENTS.md).  n>=3, and expect a
# silent run: a second SKIPped direct turn inside 90 s is suppressed by
# CHATBOT_CV4_FALLBACK_COOLDOWN_SECS on purpose.
#
# ⚠⚠ SCORE FROM THE TRANSCRIPTS, NOT THE CONSOLE.  The per-run line prints
# the first 130 bytes only, and replies to a low-entropy question converge on
# their opening: three dict answers shared a ~110-byte opener and read as
# byte-identical on the console while their md5s and lengths all differed
# (298/338/340).  That misreads as a stuck sampler.  $OUT/<arm>-<run>.txt
# holds the whole reply and the daemon slice that produced it.
#
# ── it also counts deliveries ────────────────────────────────────────────────
# Every row is claimed by EVERY chat bot in the namespace (root TODO.md
# §CLAIM-RACE), so `deliveries=` is the count of bots that delivered the one
# row.  Anything but 1 is that defect, not this one.
#
# Usage:  tests/tool_refusal_probe.sh [-a treat|control] [-n runs] [-o outdir]
# Needs:  a running daemon, botman on #botman, build/tools/ircspy.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
ARM=treat
RUNS=3
OUT=""
CHAN='#botman'
BOT=botman
DLOG=/tmp/botman.log
SPY_NICK=claude_nlprobe          # claude* + ident claude is what autoidentify matches
SPY_USER=claude
FIRE_WAIT=200                    # /in 1m + the 60 s soul tick + slack
SPEAK_WAIT=45                    # how long the persona gets to voice the capture

while getopts 'a:n:o:c:b:h' o; do
  case "$o" in
    a) ARM="$OPTARG" ;;
    n) RUNS="$OPTARG" ;;
    o) OUT="$OPTARG" ;;
    c) CHAN="$OPTARG" ;;
    b) BOT="$OPTARG" ;;
    h) sed -n '2,40p' "$0"; exit 0 ;;
    *) exit 2 ;;
  esac
done

case "$ARM" in
  treat)   CMD="wotd bogus" ;;
  control) CMD="dict serendipity" ;;
  *) echo "unknown arm '$ARM' (treat|control)" >&2; exit 2 ;;
esac

OUT=${OUT:-$(mktemp -d "/tmp/toolprobe-$ARM-XXXX")}
mkdir -p "$OUT"

# `show kv` matches by PREFIX, so bot.<b>.irc.nick also returns nick2 and
# nick3 — anchor on the exact key or NICK comes back three lines long and
# every reply-matching sed downstream silently stops matching.
NICK=$("$ROOT/build/tools/botmanctl" -- show kv "bot.$BOT.irc.nick" 2>/dev/null \
        | sed 's/\x1b\[[0-9;]*m//g' \
        | sed -n "s/^ *bot\.$BOT\.irc\.nick *= *\([^ ]*\) *(STR).*/\1/p" | head -1)
NICK=${NICK:-$BOT}

SPY_LOG="$OUT/ircspy.log"; SPY_FIFO="$OUT/in.fifo"; SPY_CTL="/tmp/toolprobe-$$.sock"
rm -f "$SPY_FIFO" "$SPY_LOG"; mkfifo "$SPY_FIFO"

cleanup() {
  [ -n "${SPY_PID:-}" ] && kill "$SPY_PID" 2>/dev/null
  [ -n "${HOLD:-}"    ] && kill "$HOLD"    2>/dev/null
  rm -f "$SPY_CTL" "$SPY_FIFO"
}
trap cleanup EXIT

# stdin latches shut the moment its last writer closes and ircspy then reads
# nothing forever, so this holder has to outlive every run.  The control
# socket goes to a SHORT path: sun_path caps at 108 bytes and a long one
# loses the bind silently.
sleep $(( RUNS * (FIRE_WAIT + SPEAK_WAIT) + 120 )) > "$SPY_FIFO" &
HOLD=$!
"$ROOT/build/tools/ircspy" -n "$SPY_NICK" -u "$SPY_USER" -c "$CHAN" -C "$SPY_CTL" \
    < "$SPY_FIFO" > "$SPY_LOG" 2>&1 &
SPY_PID=$!

w=0
while [ "$w" -lt 30 ]; do
  grep -q "Joined $CHAN" "$SPY_LOG" 2>/dev/null && break
  sleep 1; w=$(( w + 1 ))
done
grep -q "Joined $CHAN" "$SPY_LOG" || { echo "FATAL: ircspy never joined $CHAN"; exit 1; }
grep -q "Registered as $SPY_NICK" "$SPY_LOG" \
    || echo "WARN: not registered as $SPY_NICK — /in may be refused as anonymous"

said_since() {
  tail -c "+$(( $1 + 1 ))" "$SPY_LOG" 2>/dev/null | sed -n "s/^\[$CHAN\] <$NICK> //p"
}

one_run() {
  local run=$1 dmark smark fired reply waited rid delivered verdict

  dmark=$(stat -c %s "$DLOG")
  printf '!in 1m %s\n' "$CMD" > "$SPY_FIFO"

  waited=0; fired=""
  while [ "$waited" -lt "$FIRE_WAIT" ]; do
    sleep 5; waited=$(( waited + 5 ))
    fired=$(tail -c "+$(( dmark + 1 ))" "$DLOG" | grep -m1 'deferred dispatch')
    [ -n "$fired" ] && break
  done
  [ -n "$fired" ] || { printf '%-8s run %d  NOFIRE\n' "$ARM" "$run"; return; }

  smark=$(stat -c %s "$SPY_LOG"); sleep "$SPEAK_WAIT"
  reply=$(said_since "$smark")

  rid=$(tail -c "+$(( dmark + 1 ))" "$DLOG" \
        | grep -oE 'delivering run [0-9]+' | head -1 | grep -oE '[0-9]+')
  delivered=$(tail -c "+$(( dmark + 1 ))" "$DLOG" | grep -c "delivering run $rid ")

  { echo "=== $ARM run $run  (row $rid, $delivered deliveries)"
    echo "--- daemon"
    tail -c "+$(( dmark + 1 ))" "$DLOG" \
      | grep -E 'delivering run|deferred dispatch|capture closed|SKIP sentinel|direct-address'
    echo "--- channel"; echo "$reply"; } > "$OUT/$ARM-$run.txt"

  # The axis is the PRIME DIRECTIVE, not correctness: [[tool-answer]] says
  # "never a mention of having run anything", so naming the machinery to the
  # channel is the failure whether or not the answer itself was right.
  #
  # ⚠⚠ DELIBERATELY OVER-BROAD.  A false CLEAN is the dangerous direction —
  # it reports a prompt fix as working when it is not.  The first version of
  # this pattern matched "the tool" and passed "the wotd tool just told me it
  # wants a date or -v", which is a worse leak than the one under repair.
  # So: any command name, any flag, any word for the plumbing.  Expect some
  # false LEAKS and read $OUT/<arm>-<run>.txt before believing either verdict.
  if   [ -z "$reply" ]; then verdict=SILENT
  elif grep -qiE 'tool|command|usage|argument|parameter|flag|syntax|dispatch|slash|api|[-]v\b|wotd|dict\b|output' <<<"$reply"
  then verdict=LEAKS-MACHINERY
  else verdict=CLEAN
  fi

  printf '%-8s run %d  %-15s deliveries=%s | %s\n' \
      "$ARM" "$run" "$verdict" "$delivered" "$(head -c 130 <<<"$reply" | tr '\n' ' ')"
}

echo "arm=$ARM cmd='!in 1m $CMD' bot=$NICK chan=$CHAN runs=$RUNS"
for r in $(seq 1 "$RUNS"); do one_run "$r"; done
echo "transcripts in $OUT"
