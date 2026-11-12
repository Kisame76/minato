#!/usr/bin/env bash
# java-start-check.sh's failed_line: a start without its last line whose events say Minato could not start it names
# that reason ("could not start host path ..." of a bundle a failed build left half made, iPhone 2026-11-11) instead of
# "no last line"; without such an event, and for any other failure, the line stays as it was.
set -uo pipefail
cd "$(dirname "$0")/../.."
JSC=${JSC:-tools/java-start-check.sh}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
: > "$work/functions.sh"
for fn in fail failed_line thread_dump; do sed -n "/^$fn() {/,/^}/p" "$JSC" >> "$work/functions.sh"; done
echo 'screenshot() { :; }' >> "$work/functions.sh"
failures=0
check() { if [[ "$2" == *"$3"* ]]; then :; else echo "FAIL $1: $2"; failures=$((failures + 1)); fi; }
run() {  # events text, message -> the FAIL line
  printf '%b' "$1" > "$work/events"
  (failures=0; EVENTS="$work/events" CHECK="$work"; source "$work/functions.sh"; failed_line "$2" app-1) \
    2>>"$work/stderr" | grep '^FAIL'
}
reason='could not start host path "/x/Minato.app/Guest/apex-lib64/com.android.art"'
check "the start's reason replaces it" "$(run "1.0 7 tap p\n2.0 7 $reason\n" "no last line")" \
  "FAIL    $reason (no last line)"
check "without the event it stays" "$(run "1.0 7 tap p\n" "no last line")" "FAIL    no last line"
check "other failures stay" "$(run "2.0 7 $reason\n" "the menu did not open")" "FAIL    the menu did not open"
check "a longer no-last-line message" "$(run "2.0 7 $reason\n" "no last line of the plain start")" \
  "FAIL    $reason (no last line of the plain start)"
# The stall watchdog's dump (thread_dump), named even when later lines pushed it past the tail of 40; and nothing on
# stderr in any case: a helper that failed_line calls and the test did not load would only say "command not found"
# there.
{ echo "E/apkrt   : stall: black screen; background no"
  echo "E/apkrt   : thread 3 [RenderThread] blocked: futex_wait"
  for i in $(seq 1 50); do echo "I/app: line $i"; done; } > "$work/guest-log"
printf '1.0 7 tap p\n' > "$work/events"
all=$( (failures=0; EVENTS="$work/events" CHECK="$work"; source "$work/functions.sh"
  failed_line "no last line" app-1) 2>>"$work/stderr")
check "the watchdog's dump is named" "$all" "the stall watchdog named the threads"
check "its thread with its name" "$all" "      | E/apkrt   : thread 3 [RenderThread] blocked: futex_wait"
if [[ -s "$work/stderr" ]]; then
  echo "FAIL nothing on stderr:"
  sed 's/^/  | /' "$work/stderr"
  failures=$((failures + 1))
fi
if (( failures > 0 )); then echo "failed_line_test: $failures FAILED"; exit 1; fi
echo "failed_line_test: ok"
