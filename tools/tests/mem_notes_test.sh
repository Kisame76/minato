#!/usr/bin/env bash
# java-start-check.sh's mem_notes prints, from a round's guest log, the runtime memory-picture lines that matter:
# the first "mem foot=" sample, the one with the highest foot=, every warning sample, and the last, as "note  mem ..."
# lines with no logcat prefix and no paths, de-duplicated when those pick the same line.
set -uo pipefail
cd "$(dirname "$0")/../.."
JSC=${JSC:-tools/java-start-check.sh}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
: > "$work/functions.sh"
sed -n "/^mem_notes() {/,/^}/p" "$JSC" >> "$work/functions.sh"
# shellcheck disable=SC1091
source "$work/functions.sh" 2>/dev/null || true
P="I/apkrt   : mem foot="
F="int=1 comp=0 gfx=2 media=0 purg=0 purgc=0 other=50 ext=0 anon=1 file=1 gpu=3 malloc=4 hmalloc=5 untag=6"
log="$work/guest.log"
{
  echo "I/renpy-memory-test: scene 1"
  echo "${P}100 $F"
  echo "some noise that is not a mem line"
  echo "${P}300 $F warn"
  echo "${P}250 $F"
  echo "${P}200 $F"
} > "$log"
out=$(mem_notes "$log")
printf '%s\n' "$out" | sed 's/^/  out> /'
failures=0
check() { if eval "$2"; then :; else echo "FAIL $1"; failures=$((failures + 1)); fi; }
check "prints the first sample"            'grep -q "^note  mem foot=100 " <<< "$out"'
check "prints the peak foot= sample"       'grep -q "^note  mem foot=300 .* warn$" <<< "$out"'
check "prints the last sample"             'grep -q "^note  mem foot=200 " <<< "$out"'
check "strips the logcat prefix"           '! grep -q "I/apkrt" <<< "$out"'
check "drops non-mem lines"                '! grep -qE "noise|scene 1" <<< "$out"'
check "de-dups peak and warn into one"     '[[ $(grep -c "^note  mem " <<< "$out") == 3 ]]'
# A single sample: first == peak == last, printed once.
printf '%s42 %s\n' "$P" "$F" > "$work/one.log"
one=$(mem_notes "$work/one.log")
check "a single sample prints once"        '[[ $(grep -c "^note  mem " <<< "$one") == 1 ]]'
# No mem lines: nothing printed, success.
printf 'I/renpy-memory-test: nothing here\n' > "$work/none.log"
none=$(mem_notes "$work/none.log"); rc=$?
check "no mem lines -> no output"          '[[ -z "$none" ]]'
check "no mem lines -> success"            '[[ "$rc" == 0 ]]'
[[ "$failures" == 0 ]] && echo "mem_notes_test: ok"
exit $((failures > 0))
