#!/usr/bin/env bash
# java-start-check.sh's late capture in the SDL returns round, on its own functions as the script has them: a failed
# copy of the guest log is a note however short (copy_stamp), a round's measurement tries the copy three times
# (measure_log), and a round whose copies all failed takes its gl and park lines from the next good copy, only lines
# that came after that round (resolve_late); with none after it, the round stays late, so its missing gl line fails at
# the judge as before. JSC names another copy of the script (the red run on one without the late capture).
set -uo pipefail
cd "$(dirname "$0")/../.."
JSC=${JSC:-tools/java-start-check.sh}
work=$(mktemp -d)
trap 'rm -rf "${work:?}"' EXIT
failures=0
check() { if eval "$2"; then :; else echo "FAIL $1"; failures=$((failures + 1)); fi; }
: > "$work/functions.sh"
for fn in count_lines copy_stamp measure_log resolve_late; do
  body=$(sed -n "/^$fn() {/,/^}/p" "$JSC")
  check "$fn() is in $JSC" '[[ -n "$body" ]]'
  printf '%s\n' "$body" >> "$work/functions.sh"
done
# shellcheck disable=SC1091
source "$work/functions.sh"
sleep() { :; }   # measure_log's 2 s between tries, not waited here

# A failed copy has its note at once; a good short one has none (STAMPS unset).
STAMPS=""
out=$(copy_stamp "from guest-log" "$SECONDS" 01:02:03 1 "exit 1" 3>&1)
check "a failed copy of 0 s has its note" '[[ "$out" == "note    copy from guest-log 01:02:03-"*" (0 s) attempt 1: exit 1" ]]'
out=$(copy_stamp "from guest-log" "$SECONDS" 01:02:03 1 "exit 0" 3>&1)
check "a good copy of 0 s has none" '[[ -z "$out" ]]'

# measure_log: fetch_log fails FAILS times, then answers.
fetch_log() { calls=$((calls + 1)); [[ $calls -gt $FAILS ]]; }
calls=0 FAILS=2
measure_log; status=$?
check "two failed copies, then a good one: true after 3 tries (got $status after $calls)" '[[ $status -eq 0 && $calls -eq 3 ]]'
calls=0 FAILS=3
measure_log; status=$?
check "three failed copies: false after 3 tries (got $status after $calls)" '[[ $status -ne 0 && $calls -eq 3 ]]'

# resolve_late: return 2's copies failed when the local log had 3 lines; the next good copy has more.
CHECK=$work
log() { printf '%s\n' "$@" > "$CHECK/guest-log"; }
gl_line() { echo "11-13 23:3$1.000  1 1 I/apkrt   : gl tex=$1/$1 priv=79 color=$1/$1 depth=3/79 msaa=0/0 mtl=$1 ctx=1"; }
log "$(gl_line 1)" "I/apkrt   : angle park n=1 raw=1" "I/minato : trip 2" "$(gl_line 2)" "I/apkrt   : angle park n=2 raw=2" \
  "$(gl_line 3)"
gls=("gl tex=0/0" "gl tex=1/1" "") late=("2:3")
out=$(resolve_late; declare -p gls late)
check "the last gl line after the failed round, into its place" \
  '[[ "$out" == *"note    late capture after return 2: gl tex=3/3 priv=79"* && "$out" == *"[2]=\"gl tex=3/3 priv=79"* ]]'
check "the park line after it" '[[ "$out" == *"note      angle park n=2 raw=2"* ]]'
check "nothing left late" '[[ "$out" == *"declare -a late="* && "$out" != *"2:3"* ]]'
# Only lines from before the round: it stays late and its gl stays empty (the judge's FAIL).
log "$(gl_line 1)" "I/apkrt   : angle park n=1 raw=1" "I/minato : trip 2"
gls=("gl tex=0/0" "gl tex=1/1" "") late=("2:3")
out=$(resolve_late; declare -p gls late)
check "no gl line after the round: no note, the round stays late" \
  '[[ "$out" != *"late capture"* && "$out" == *"[2]=\"\""* && "$out" == *"2:3"* ]]'
# No local log at all (the next copy failed too): it stays late.
rm -f "$CHECK/guest-log"
gls=("gl tex=0/0" "") late=("1:5")
out=$(resolve_late; declare -p late)
check "no local log: the round stays late" '[[ "$out" == *"1:5"* ]]'

if [[ $failures -gt 0 ]]; then
  echo "returns_late_capture_test: $failures FAIL"
  exit 1
fi
echo "returns_late_capture_test: ok"
