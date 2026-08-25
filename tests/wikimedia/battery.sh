#!/bin/bash
# battery.sh - the WIKIMEDIA validation battery.
#
# usage: tests/wikimedia/battery.sh [options]
#
#   -m <modes>   which modes to run, any of A B C (default: ABC)
#   -f <file>    fixture (default: tests/wikimedia/cases.tsv)
#   -o <dir>     where transcripts and the score are written
#                (default: a fresh mktemp dir, named on the last line)
#   -n <runs>    mode B repeats per case (default: 3, majority-scored)
#   -c <case>    run only cases whose input contains this substring
#   -q           score only; do not print the failing transcripts
#   -h           this
#
# Three surfaces are scored separately because a single blended number would
# hide which half regressed:
#
#   A  typed !wiki through botmanctl, no model anywhere.  Deterministic, so a
#      move here is a real regression and the same tree must score the same.
#   B  natural-language prose in #botman through the NL bridge.  NOT
#      deterministic: each case runs -n times and the majority is its score,
#      with the disagreement count reported beside it.
#   C  corpus acquisition, read back out of the knowledge store.
#
# Four independent binary axes per case, so a failure says WHICH KIND:
#
#   resolve  the reply landed on the expected item (or on none, where none
#            was expected)
#   answer   expect_substr is present
#   clean    no forbid_substr appears
#   honest   a no-match case owned up instead of inventing
#
# A REFUSAL IS NEVER A SCORE.  Wikidata answers a handful of commands a minute
# with `x-envoy-ratelimited` and a Retry-After; the reply says so in words and
# carries the wait, so this harness reads that as "run this case again in N
# seconds" and only reports a skip after four attempts.  Mode C treats a
# wikimedia decline the same way, and treats a reactive-dedup hit as a skip
# outright.  Scoring a refusal as a miss is how the number stops meaning
# anything.
#
# What it costs, and why: mode A paces itself at ~30 s a case because that is
# roughly what the rate limit allows; mode B waits out the bot's own 30 s
# reply cooldown between messages and runs every case three times.  A full
# run is therefore the better part of an hour, and that is the endpoint's
# price, not the harness's.
#
# ⚠ Mode C's REACTIVE case is re-runnable only once an hour:
# `acquire.reactive_dedup_ttl_secs` (3600) skips a repeat of the same
# (bot, topic, subject).  The proactive case has no such window.

set -u -o pipefail

# --- where things are -------------------------------------------------------

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BMCTL="$ROOT/build/tools/botmanctl"
IRCSPY="$ROOT/build/tools/ircspy"

MODES="ABC"
FIXTURE="$HERE/cases.tsv"
OUT=""
B_RUNS=3
ONLY=""
QUIET=0

# botmanctl's -w is spent in full even when the answer is early, so the linger
# IS the pacing: one !wiki costs 3-9 Wikidata requests and a tight loop earns
# a 429 within a handful of them.
WAIT_A=10000
GAP_A=25
RETRY_MAX=4

# The chat bot's own reply_cooldown_secs is 30 per target, so a mode-B case
# sent inside that window is dropped rather than answered.  Pace outside it.
GAP_B=33
REPLY_B=75

# One acquisition is a resolve, a fetch, an LLM digest and an embed.
WAIT_C=240

BOT=botman
TOPIC=physics
CHAN='#botman'
SPY_NICK=claude_wikibat      # `claude*` + ident `claude` is what autoidentify matches
SPY_USER=claude

while getopts 'm:f:o:n:c:qh' opt; do
  case "$opt" in
    m) MODES="$OPTARG" ;;
    f) FIXTURE="$OPTARG" ;;
    o) OUT="$OPTARG" ;;
    n) B_RUNS="$OPTARG" ;;
    c) ONLY="$OPTARG" ;;
    q) QUIET=1 ;;
    h) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) exit 2 ;;
  esac
done

# `--` on EVERY dispatch, not just the ones that look like they need it.
# botmanctl parses its own flags first, so any leading `-` in the command -
# `wiki -l paris`, `set kv --delete <key>` - is eaten as a tool option and the
# command never reaches the daemon.  Measured 08-24: mode C's disarm failed
# exactly this way and left the bot registered and ticking.
bm() { "$BMCTL" -- "$@"; }
bmw() { local ms="$1"; shift; "$BMCTL" -w "$ms" -- "$@"; }

die()  { printf 'battery: %s\n' "$*" >&2; exit 1; }
say()  { printf '%s\n' "$*"; }
note() { printf '  . %s\n' "$*"; }

[ -x "$BMCTL" ]   || die "no botmanctl at $BMCTL - build first"
[ -r "$FIXTURE" ] || die "no fixture at $FIXTURE"

[ -n "$OUT" ] || OUT="$(mktemp -d -t wikibattery.XXXXXX)"
mkdir -p "$OUT"
FAILS="$OUT/failures.txt"
: > "$FAILS"

# --- the daemon's log, read as a slice --------------------------------------
#
# Routing is read from the log and never from the reply text: a reply that
# happens to be right is not a route.

CONF="${BOTMAN_CONF:-${XDG_CONFIG_HOME:-$HOME/.config}/botmanager/botman.conf}"
conf_get() { [ -r "$CONF" ] && sed -n "s/^$1=\"\(.*\)\"/\1/p" "$CONF" | tail -1; }

BOTLOG="$(conf_get LOG_PATH)"; BOTLOG="${BOTLOG:-/tmp/botman.log}"

log_mark()  { [ -r "$BOTLOG" ] && stat -c %s "$BOTLOG" || echo 0; }
log_since() { [ -r "$BOTLOG" ] && tail -c "+$(( ${1:-0} + 1 ))" "$BOTLOG" || true; }

strip_ansi() { sed 's/\x1b\[[0-9;]*m//g'; }

# One KV string, by its WHOLE key.  `show kv` matches by prefix, so asking for
# `bot.<n>.acquired_corpus` also answers `_max_mb` and `_ttl_days`; and an
# unset KV_STR renders as the literal `(empty)`, which is not its value.
kv_str() {
  local v
  v="$(bm show kv "$1" 2>/dev/null | strip_ansi \
       | sed -n "s|^  *$1 = \\(.*\\) (STR)\$|\\1|p" | head -1)"
  [ "$v" = "(empty)" ] && v=""
  printf '%s' "$v"
}

# --- scoring ----------------------------------------------------------------

declare -A PASS TOT
AXES="resolve answer clean honest"
for m in A B C; do for a in $AXES; do PASS[$m$a]=0; TOT[$m$a]=0; done; done
declare -A EXTRA          # per-mode free-form counters, printed beside the axes
SKIPPED=0

tally() { TOT[$1$2]=$(( ${TOT[$1$2]} + 1 )); [ "$3" = 1 ] && PASS[$1$2]=$(( ${PASS[$1$2]} + 1 )); return 0; }

# Case-insensitive substring, with '|' meaning "any of these".
has_any() {
  local hay="${1,,}" alts="$2" one
  [ -n "$alts" ] || return 1
  while IFS= read -r one; do
    [ -n "$one" ] || continue
    case "$hay" in *"${one,,}"*) return 0 ;; esac
  done < <(printf '%s\n' "$alts" | tr '|' '\n')
  return 1
}

has_none() {
  local hay="$1" alts="$2" one
  [ -n "$alts" ] || return 0
  while IFS= read -r one; do
    [ -n "$one" ] || continue
    case "${hay,,}" in *"${one,,}"*) return 1 ;; esac
  done < <(printf '%s\n' "$alts" | tr '|' '\n')
  return 0
}

has_qid()  { grep -qE "(^|[^A-Za-z0-9])$1([^0-9]|\$)" <<<"$2"; }
any_qid()  { grep -qE "(^|[^A-Za-z0-9])Q[0-9]{2,}([^0-9]|\$)" <<<"$1"; }

# The four wordings wiki_render_status() and its callers can produce for a
# refusal, plus the prose a re-voiced one turns into.
OWNS_UP='could not|couldn.t|cannot|can.t|nothing on wikidata|records no|not find|no record|no entry|no result|unable to|never heard|do not have|don.t have|no such|came up empty|not a real|made.up|not seeing|no idea|drew a blank'
RETRYABLE='rate-limiting us|could not reach wikidata|could not ask wikidata|plugin is going away'

is_retryable() { [[ "${1,,}" =~ $RETRYABLE ]]; }
retry_secs()   { sed -n 's/.*again in \([0-9]\{1,\}\)s.*/\1/p' <<<"$1" | head -1; }

# Score one reply against one case.  Echoes the per-axis verdicts as a string
# of `axis=0|1` words so mode B can take a majority over three of them.
verdicts() {
  local reply="$1" eqid="$2" esub="$3" efor="$4" v=""

  if [ "$eqid" != "-" ]; then
    if [ -n "$eqid" ]; then
      has_qid "$eqid" "$reply" && v+="resolve=1 " || v+="resolve=0 "
    else
      any_qid "$reply" && v+="resolve=0 " || v+="resolve=1 "
    fi
  fi

  [ -n "$esub" ] && { has_any  "$reply" "$esub" && v+="answer=1 " || v+="answer=0 "; }
  [ -n "$efor" ] && { has_none "$reply" "$efor" && v+="clean=1 "  || v+="clean=0 ";  }

  # The honest axis is the no-match case's: a reply that cannot answer must
  # say so.  Where a match WAS expected there is nothing for it to score.
  if [ "$eqid" = "" ]; then
    [[ "${reply,,}" =~ $OWNS_UP ]] && v+="honest=1 " || v+="honest=0 "
  fi

  printf '%s' "$v"
}

record() {
  local mode="$1" input="$2" reply="$3" v="$4" word axis ok bad=0
  for word in $v; do
    axis="${word%%=*}"; ok="${word##*=}"
    tally "$mode" "$axis" "$ok"
    [ "$ok" = 0 ] && bad=1
  done
  [ "$bad" = 0 ] && return 0
  {
    printf '\n--- %s  %s\n    axes: %s\n' "$mode" "$input" "$v"
    printf '%s\n' "$reply" | sed 's/^/    | /'
  } >> "$FAILS"
  return 0
}

# --- the fixture ------------------------------------------------------------
#
# Cases are read once into arrays: mode B walks its own list -n times, and
# reading a fixture three times invites it changing underneath a run.

# Tabs are read through a unit separator, NOT as IFS.  Tab is IFS WHITESPACE,
# so bash collapses a run of them into one delimiter and drops the leading and
# trailing ones - which silently shifts every field of a case with an empty
# expect_qid one column left.  Measured 08-24: it scored the nonsense case's
# forbid list as its expected id and the honest axis never ran at all.
declare -a C_MODE C_IN C_QID C_SUB C_FOR
n=0
while IFS=$'\037' read -r mode input eqid esub efor || [ -n "$mode" ]; do
  case "$mode" in ''|'#'*) continue ;; esac
  [ -n "$ONLY" ] && case "$input" in *"$ONLY"*) ;; *) continue ;; esac
  C_MODE[n]="$mode"; C_IN[n]="$input"; C_QID[n]="${eqid-}"
  C_SUB[n]="${esub-}"; C_FOR[n]="${efor-}"
  n=$(( n + 1 ))
done < <(tr '\t' '\037' < "$FIXTURE")
NCASES=$n

# --- mode A: typed !wiki, no model ------------------------------------------

# The fixture writes an argument line the way a person types it, quotes and
# all (`"green day" members`), so it is split the way a shell would.  eval is
# the only thing in bash that splits on quotes, and this guard is what makes
# it safe to point at a file: a case may quote, and may do nothing else.
split_args() {
  case "$1" in
    *'`'*|*'$'*|*';'*|*'&'*|*'|'*|*'<'*|*'>'*|*'('*|*')'*)
      die "case input has shell metacharacters, which no wiki argument needs: $1" ;;
  esac
  eval "ARGV=( $1 )"
}

# `--` because a case may open with a wiki flag and botmanctl's own getopt
# would otherwise eat it: `wiki -l paris` reached the tool as an unknown -l.
wiki_once() {
  local -a ARGV
  split_args "$1"
  bmw "$WAIT_A" wiki "${ARGV[@]}" 2>&1 | strip_ansi | sed '/^[[:space:]]*$/d'
}

mode_a() {
  local i reply tries wait
  say ""
  say "mode A - typed !wiki, no model"

  for (( i = 0; i < NCASES; i++ )); do
    [ "${C_MODE[i]}" = A ] || continue

    tries=0
    while : ; do
      reply="$(wiki_once "${C_IN[i]}")"
      is_retryable "$reply" || break
      tries=$(( tries + 1 ))
      if [ "$tries" -ge "$RETRY_MAX" ]; then
        note "SKIP  ${C_IN[i]} - still refused after $tries attempts"
        SKIPPED=$(( SKIPPED + 1 ))
        reply=""
        break
      fi
      wait="$(retry_secs "$reply")"; wait="${wait:-20}"
      note "wait ${wait}s - ${C_IN[i]}"
      sleep "$(( wait + 2 ))"
    done

    [ -n "$reply" ] || continue
    printf '%s\n' "$reply" > "$OUT/A-$i.txt"
    record A "${C_IN[i]}" "$reply" "$(verdicts "$reply" "${C_QID[i]}" "${C_SUB[i]}" "${C_FOR[i]}")"
    sleep "$GAP_A"
  done
}

# --- mode B: the NL bridge, in the channel ----------------------------------
#
# ircspy rather than botmanctl: the reply pipeline can outrun botmanctl's 60 s
# linger ceiling, and what is under test is what a CHANNEL is told.

spy_start() {
  SPY_DIR="$OUT/spy"; mkdir -p "$SPY_DIR"
  SPY_LOG="$SPY_DIR/ircspy.log"
  SPY_FIFO="$SPY_DIR/in.fifo"
  SPY_CTL="/tmp/wikibattery-$$.sock"
  rm -f "$SPY_FIFO"; mkfifo "$SPY_FIFO"

  # stdin latches shut the moment its last writer closes, after which ircspy
  # stays joined and silently reads nothing forever.  This holder outlives the
  # whole mode so that never happens mid-run.
  sleep "$SPY_BUDGET" > "$SPY_FIFO" &
  SPY_HOLDER=$!

  # The control socket goes to a short path, not beside the transcripts: a
  # unix socket name is capped at 108 bytes and an -o under a long scratch
  # directory silently loses the bind (and with it any hope of ircspyctl).
  rm -f "$SPY_CTL"
  "$IRCSPY" -n "$SPY_NICK" -u "$SPY_USER" -c "$CHAN" \
      -C "$SPY_CTL" < "$SPY_FIFO" > "$SPY_LOG" 2>&1 &
  SPY_PID=$!

  local waited=0
  while [ "$waited" -lt 30 ]; do
    grep -q "Joined $CHAN" "$SPY_LOG" 2>/dev/null && break
    sleep 1; waited=$(( waited + 1 ))
  done
  grep -q "Joined $CHAN" "$SPY_LOG" || { spy_stop; die "ircspy never joined $CHAN"; }

  # A taken nick drops ircspy to `agent1`, which the autoidentify mask no
  # longer matches - the session goes anonymous with no visible complaint.
  grep -q "Registered as $SPY_NICK" "$SPY_LOG" \
      || note "WARN ircspy is not registered as $SPY_NICK - a stray may hold the nick"
}

spy_stop() {
  [ -n "${SPY_CTL:-}" ]    && rm -f "$SPY_CTL"
  [ -n "${SPY_PID:-}" ]    && kill "$SPY_PID"    2>/dev/null
  [ -n "${SPY_HOLDER:-}" ] && kill "$SPY_HOLDER" 2>/dev/null
  SPY_PID=""; SPY_HOLDER=""
}

# Everything the bot said in the channel after byte $1 of the spy log.
spy_reply_since() {
  tail -c "+$(( $1 + 1 ))" "$SPY_LOG" 2>/dev/null \
      | sed -n "s/^\[$CHAN\] <$NICK> //p"
}

mode_b() {
  local i run mark lmark reply routed waited any btries
  local -a agg dis

  [ -x "$IRCSPY" ] || { note "no ircspy at $IRCSPY - mode B skipped"; return 0; }

  NICK="$(kv_str "bot.$BOT.irc.nick")"
  [ -n "$NICK" ] || { note "bot $BOT has no irc nick - mode B skipped"; return 0; }

  say ""
  say "mode B - natural language through the bridge, as $NICK in $CHAN"

  # A bot that is not in the channel answers nothing, and every case then
  # scores a silence that says nothing about the bridge.  Measured 08-24: a
  # daemon restart left botman off #botman with autojoin still set.
  bmw 4000 irc join "$BOT" "$CHAN" >/dev/null 2>&1
  sleep 3

  # Budget the fifo holder for the whole mode, generously: it latching shut
  # early is a silent wrong result, and an over-long sleep is killed at the end.
  local nb=0
  for (( i = 0; i < NCASES; i++ )); do [ "${C_MODE[i]}" = B ] && nb=$(( nb + 1 )); done
  [ "$nb" -gt 0 ] || return 0
  SPY_BUDGET=$(( nb * B_RUNS * (GAP_B + REPLY_B) + 120 ))
  spy_start
  trap spy_stop EXIT

  EXTRA[Brouted]=0; EXTRA[Bsent]=0

  for (( i = 0; i < NCASES; i++ )); do
    [ "${C_MODE[i]}" = B ] || continue
    agg=(); dis=(); btries=0

    for (( run = 1; run <= B_RUNS; run++ )); do
      mark="$(stat -c %s "$SPY_LOG")"
      lmark="$(log_mark)"
      printf '%s: %s\n' "$NICK" "${C_IN[i]}" > "$SPY_FIFO"
      EXTRA[Bsent]=$(( ${EXTRA[Bsent]} + 1 ))

      waited=0; reply=""
      while [ "$waited" -lt "$REPLY_B" ]; do
        sleep 3; waited=$(( waited + 3 ))
        any="$(spy_reply_since "$mark")"
        [ -n "$any" ] || continue
        reply="$any"
        # Give a multi-line answer time to finish arriving before scoring it.
        sleep 4; waited=$(( waited + 4 ))
        reply="$(spy_reply_since "$mark")"
        break
      done

      routed=0
      log_since "$lmark" | grep -q "nl_bridge.*dispatch /wiki" && routed=1
      [ "$routed" = 1 ] && EXTRA[Brouted]=$(( ${EXTRA[Brouted]} + 1 ))

      if [ -z "$reply" ]; then
        note "silence  run $run  ${C_IN[i]}"
        reply="(no reply within ${REPLY_B}s)"
      fi

      printf 'routed=%s\n%s\n' "$routed" "$reply" > "$OUT/B-$i-$run.txt"

      # A rate-limited leg is re-voiced into prose, so it is caught here too
      # and the run is repeated rather than scored - up to the same ceiling
      # mode A gives it, because a wedged backoff would otherwise repeat one
      # run for as long as the battery is allowed to live.
      if is_retryable "$reply"; then
        btries=$(( btries + 1 ))
        if [ "$btries" -lt "$RETRY_MAX" ]; then
          local w; w="$(retry_secs "$reply")"; w="${w:-20}"
          note "wait ${w}s - ${C_IN[i]}"
          sleep "$(( w + 2 ))"
          run=$(( run - 1 ))
          continue
        fi
        note "SKIP  run $run  ${C_IN[i]} - still refused after $btries attempts"
        SKIPPED=$(( SKIPPED + 1 ))
        [ "$run" -lt "$B_RUNS" ] && sleep "$GAP_B"
        continue
      fi

      agg+=("$(verdicts "$reply" "${C_QID[i]}" "${C_SUB[i]}" "${C_FOR[i]}")")
      dis+=("$reply")
      [ "$run" -lt "$B_RUNS" ] && sleep "$GAP_B"
    done

    b_majority "$i" "${agg[@]}"
    sleep "$GAP_B"
  done

  spy_stop
  trap - EXIT
}

# A case that flips between runs is a different problem from one that fails,
# and the fix is different too - so the majority is the score and the flip is
# counted beside it.
b_majority() {
  local i="$1"; shift
  local -a runs=("$@")
  local axis word ok sum v="" per unanimous=1 first=""

  for axis in $AXES; do
    sum=0; per=0
    for v in "${runs[@]}"; do
      for word in $v; do
        [ "${word%%=*}" = "$axis" ] || continue
        per=$(( per + 1 )); ok="${word##*=}"; sum=$(( sum + ok ))
      done
    done
    [ "$per" -gt 0 ] || continue
    [ "$sum" -gt 0 ] && [ "$sum" -lt "$per" ] && unanimous=0
    if [ $(( sum * 2 )) -ge "$per" ]; then tally B "$axis" 1; else tally B "$axis" 0; fi
    [ $(( sum * 2 )) -ge "$per" ] || first+="$axis "
  done

  [ "$unanimous" = 1 ] || EXTRA[Bflip]=$(( ${EXTRA[Bflip]:-0} + 1 ))

  [ -n "$first" ] || return 0
  {
    printf '\n--- B  %s\n    failed axes (majority of %d): %s\n' \
        "${C_IN[i]}" "${#runs[@]}" "$first"
    local r=1
    for v in "${runs[@]}"; do
      printf '    run %d: %s\n' "$r" "$v"
      sed 's/^/      | /' "$OUT/B-$i-$r.txt" 2>/dev/null
      r=$(( r + 1 ))
    done
  } >> "$FAILS"
}

# --- mode C: acquisition, read back out of the knowledge store --------------
#
# The store is the only honest observable here: the command that fires a job
# answers long before the job finishes, and the digest is written by a model
# on a pool worker.  A wikimedia-sourced row is the one whose section_heading
# carries the article's own title (`<topic>: <Article>`) - the search path
# that scrapes the same URL leaves the bare topic there, and its digest is a
# summary of raw page markup.  That difference IS the resolve axis.

pg_ready() {
  command -v psql >/dev/null 2>&1 || return 1
  [ -r "$CONF" ] || return 1
  PGHOST="$(conf_get DBHOST)"; PGHOST="${PGHOST:-localhost}"
  PGPORT="$(conf_get DBPORT)"; PGPORT="${PGPORT:-5432}"
  PGDATABASE="$(conf_get DBNAME)"
  PGUSER="$(conf_get DBUSER)"
  PGPASSWORD="$(conf_get DBPASS)"
  export PGHOST PGPORT PGDATABASE PGUSER PGPASSWORD
  [ -n "$PGDATABASE" ] && [ -n "$PGUSER" ] || return 1
  psql -qAt -c 'select 1' >/dev/null 2>&1
}

pgq() { psql -qAt -v ON_ERROR_STOP=1 -c "$1"; }

mode_c() {
  local i mark lmark rows waited saved corpus subject heading text
  local tries fired wait

  [ -x "$BMCTL" ] || return 0
  local nc=0
  for (( i = 0; i < NCASES; i++ )); do [ "${C_MODE[i]}" = C ] && nc=$(( nc + 1 )); done
  [ "$nc" -gt 0 ] || return 0

  pg_ready || { note "no reachable postgres - mode C skipped"; return 0; }

  say ""
  say "mode C - acquisition into the knowledge store"

  corpus="${BOT}_acquired"
  saved="$(kv_str "bot.$BOT.acquired_corpus")"

  # A registered bot ticks its interests every 600 s against third-party
  # sites, unattended.  The fixture is deliberately switched OFF between
  # runs, so this arms it and the trap disarms it however the run ends.
  c_disarm() {
    if [ -n "$saved" ]; then
      bm set kv "bot.$BOT.acquired_corpus" "$saved" >/dev/null 2>&1
    else
      bm set kv --delete "bot.$BOT.acquired_corpus" >/dev/null 2>&1
    fi
    bm plugin reload chat >/dev/null 2>&1

    # Say so loudly if it did not take.  A corpus left armed is a bot going
    # out to third-party sites every 600 s with nobody watching.
    local now; now="$(kv_str "bot.$BOT.acquired_corpus")"
    [ "$now" = "$saved" ] || printf \
        '  ! bot.%s.acquired_corpus is still %s - CLEAR IT BY HAND\n' \
        "$BOT" "${now:-(empty)}" >&2
  }
  trap 'c_disarm; spy_stop' EXIT

  bm knowledge corpus upsert "$corpus" >/dev/null 2>&1
  bm set kv "bot.$BOT.acquired_corpus" "$corpus" >/dev/null 2>&1
  bm plugin reload chat >/dev/null 2>&1
  sleep 3

  for (( i = 0; i < NCASES; i++ )); do
    [ "${C_MODE[i]}" = C ] || continue
    subject="${C_IN[i]}"

    tries=0
    while : ; do
      mark="$(pgq "select coalesce(max(id), 0) from knowledge_chunks where corpus = '$corpus';")"
      lmark="$(log_mark)"

      if [ "$subject" = "@trigger" ]; then
        fired="$(bm acquire trigger "$BOT" "$TOPIC" 2>&1 | strip_ansi)"
      else
        fired="$(bm acquire fire "$BOT" "$TOPIC" "$subject" 2>&1 | strip_ansi)"
      fi

      # A reactive job for the same (bot, topic, subject) inside
      # `acquire.reactive_dedup_ttl_secs` (3600) is skipped, so this case
      # cannot be re-run inside the hour and there is nothing to score.
      case "${fired,,}" in
        *dedup*)
          note "SKIP  $subject - reactive dedup, retry after acquire.reactive_dedup_ttl_secs"
          SKIPPED=$(( SKIPPED + 1 ))
          rows=""; break ;;
      esac

      waited=0; rows=""
      while [ "$waited" -lt "$WAIT_C" ]; do
        sleep 10; waited=$(( waited + 10 ))
        rows="$(pgq "select id || E'\t' || source_url || E'\t' || section_heading
                       || E'\t' || replace(left(text, 400), E'\n', ' ')
                     from knowledge_chunks
                     where corpus = '$corpus' and id > $mark order by id;")"
        [ -n "$rows" ] && break
      done

      # The errand was offered to wikimedia and wikimedia was refused, so the
      # job went to search - which says nothing about the encyclopedia and
      # must not be scored as if it did.
      log_since "$lmark" | grep -q 'wikimedia is rate-limiting us' || break
      tries=$(( tries + 1 ))
      if [ "$tries" -ge "$RETRY_MAX" ]; then
        note "SKIP  $subject - wikimedia refused every attempt"
        SKIPPED=$(( SKIPPED + 1 ))
        rows=""; break
      fi
      wait="$(log_since "$lmark" | sed -n 's/.*standing down for \([0-9]\{1,\}\)s.*/\1/p' | tail -1)"
      note "wait ${wait:-30}s - $subject"
      sleep "$(( ${wait:-30} + 3 ))"
    done

    [ -n "$rows" ] || continue
    printf '%s\n' "$rows" > "$OUT/C-$i.txt"

    # The encyclopedia answered iff a row carries the article's own title.
    # resolve is about WHICH SOURCE answered; answer is about whether the
    # content reached the store at all.  Coupling them scored a job that fell
    # through to search as having ingested nothing, which is false.
    heading="$(printf '%s\n' "$rows" | cut -f3 | grep -E "^$TOPIC: ." | head -1)"
    text="$(printf '%s\n' "$rows" | grep -F "$TOPIC" | cut -f3,4)"

    tally C resolve "$([ -n "$heading" ] && echo 1 || echo 0)"
    [ -n "${C_SUB[i]}" ] && { has_any  "$text" "${C_SUB[i]}" && tally C answer 1 || tally C answer 0; }
    [ -n "${C_FOR[i]}" ] && { has_none "$text" "${C_FOR[i]}" && tally C clean 1  || tally C clean 0;  }

    if [ -z "$heading" ] || { [ -n "${C_SUB[i]}" ] && ! has_any "$text" "${C_SUB[i]}"; }; then
      {
        printf '\n--- C  %s\n    wikimedia heading: %s\n' \
            "$subject" "${heading:-(none - the errand fell through to search)}"
        printf '%s\n' "${rows:-(no chunk)}" | sed 's/^/    | /'
      } >> "$FAILS"
    fi
  done

  c_disarm
  trap - EXIT
}

# --- the report -------------------------------------------------------------

pct() { [ "$2" -gt 0 ] && printf '%d%%' $(( $1 * 100 / $2 )) || printf 'n/a'; }

report() {
  local m a p t sp st line
  say ""
  say "==============================================================="
  printf 'WIKIMEDIA battery  %s  tree %s\n' \
      "$(date '+%Y-%m-%d %H:%M')" "$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo '?')"
  say "==============================================================="

  for m in A B C; do
    case "$MODES" in *"$m"*) ;; *) continue ;; esac
    sp=0; st=0; line=""
    for a in $AXES; do
      p="${PASS[$m$a]}"; t="${TOT[$m$a]}"
      sp=$(( sp + p )); st=$(( st + t ))
      if [ "$t" -gt 0 ]; then line+="$(printf '%-8s %d/%-4d' "$a" "$p" "$t")"
      else                    line+="$(printf '%-8s %-6s' "$a" 'n/a')"; fi
    done
    printf '  mode %s   %s  overall %s\n' "$m" "$line" "$(pct "$sp" "$st")"
  done

  [ "${EXTRA[Bsent]:-0}" -gt 0 ] && printf '           routed %s/%s   flipped cases %s\n' \
      "${EXTRA[Brouted]:-0}" "${EXTRA[Bsent]:-0}" "${EXTRA[Bflip]:-0}"
  # Skips are never failures and never scores; each one said why when it
  # happened, so this is only the count.
  [ "$SKIPPED" -gt 0 ] && printf \
      '           %d case(s) skipped and NOT scored - see the notes above\n' \
      "$SKIPPED"

  {
    for m in A B C; do
      case "$MODES" in *"$m"*) ;; *) continue ;; esac
      for a in $AXES; do printf '%s %s %s/%s\n' "$m" "$a" "${PASS[$m$a]}" "${TOT[$m$a]}"; done
    done
  } > "$OUT/score.txt"

  if [ -s "$FAILS" ] && [ "$QUIET" = 0 ]; then
    say ""
    say "failing cases, with what they actually said"
    say "---------------------------------------------------------------"
    cat "$FAILS"
  fi

  say ""
  say "transcripts: $OUT"
}

# --- go ---------------------------------------------------------------------

say "battery: $NCASES case(s) from $(basename "$FIXTURE"), modes $MODES"

case "$MODES" in *A*) mode_a ;; esac
case "$MODES" in *B*) mode_b ;; esac
case "$MODES" in *C*) mode_c ;; esac

report
