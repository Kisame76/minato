#!/usr/bin/env bash
# java-start-check.sh's background reading of the resume round (away_mark, away_scan, judge_away, return_failed),
# against guest logs written in the test: Android 15's order (the plain Ren'Py game on the AVD: onStop done at once,
# "Entered background." before the return) passes; Minato's of 2026-11-11 (onStop to its 8 s backstop, the suspend only
# at the return, an ANR) gives the three FAIL lines; an app without Ren'Py's lines is judged by the ANR alone; and with
# RESUME_NO_REFERENCE the FAIL lines become notes that say why.
set -uo pipefail
cd "$(dirname "$0")/../.."
JSC=${JSC:-tools/java-start-check.sh}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
: > "$work/functions.sh"
# Each function as the script has it: a one-line one by its line, a longer one up to its closing brace.
for fn in fail ok count_lines seconds return_failed away_mark away_scan judge_away; do
  if grep -q "^$fn() {.*}$" "$JSC"; then grep "^$fn() {.*}$" "$JSC"; else sed -n "/^$fn() {\$/,/^}/p" "$JSC"; fi
done >> "$work/functions.sh"
failures=0
check() { if [[ "$2" == *"$3"* ]]; then :; else echo "FAIL $1: $2"; failures=$((failures + 1)); fi; }
none() { if [[ "$2" != *"$3"* ]]; then :; else echo "FAIL $1: $2"; failures=$((failures + 1)); fi; }
# A trip: the log before it, the lines that come in the background (each a scan 1 s apart), then those of the return.
trip() {  # before-lines, background-lines (one scan per "|"-group), return-lines
  (
    CHECK="$work/check" CHECK_REL=check failures=0
    mkdir -p "$CHECK"
    printf '%b' "$1" > "$CHECK/guest-log"
    fetch() { :; }
    failed_line() { echo "FAIL  $1"; }
    clock=1000
    now() { echo "$clock"; }
    # shellcheck disable=SC1091
    source "$work/functions.sh"
    away_mark
    IFS='|' read -r -a groups <<< "$2"
    for group in "${groups[@]}"; do
      printf '%b' "$group" >> "$CHECK/guest-log"
      clock=$((clock + 1))
      away_scan
    done
    printf '%b' "$3" >> "$CHECK/guest-log"
    judge_away app "15 s in the background"
  ) 2>&1
}
before='I/python  : minato-renpy-memory: scene 10\nV/python  : armOnStop()\n'
stop='V/python  : onStop() start.\n'
done_='V/python  : onStop() done.\n'
entered='I/python  : Entered background. ---\n'
resume='V/SDL     : nativeResume()\nI/python  : Entering foreground. ---\n'
anr_line='E/ActivityManager: ANR in io.example\n'
label='after 15 s in the background'
# Android 15's order: the whole suspend and onStop in one scan of the background.
suspend="V/SDL     : onPause()\n${stop}I/python  : Saving.\nV/python  : finishOnStop()\n$entered$done_"
android=$(trip "$before" "$suspend" "$resume")
check "Android's order: onStop at once" "$android" "ok      $label: the app's onStop took 0.0 s"
check "Android's order: suspend in the background" "$android" "the game's suspend ran in the background"
none "Android's order: no FAIL" "$android" "FAIL"
# Minato's of 2026-11-11: onStop to its backstop, an ANR, the suspend only at the return.
ours=$(trip "$before" "$stop|I/libEGL  : trace: 1 contexts\n|$anr_line|$done_" "${resume}I/python  : Saving.\n$entered")
check "ours: the ANR" "$ours" "FAIL  $label: the app's main thread did not answer in the background (ANR in io.example)"
check "ours: onStop's time" "$ours" "FAIL  $label: the app's onStop took 3.0 s in the background"
check "ours: the late suspend" "$ours" "FAIL  $label: the game's suspend ran only at the return"
stuck=$(trip "$before" "$stop|I/libEGL  : x\n" "$done_")
check "onStop that ends only after the return" "$stuck" "the app's onStop took the whole trip in the background"
# An app without Ren'Py's lines: judged by the ANR alone.
plain=$(trip 'I/app: start\n' 'I/app: onPause\n|I/app: onStop\n' 'I/app: onResume\n')
none "an app without Ren'Py's lines: no line" "$plain" "$label"
anr=$(trip 'I/app: start\n' "$anr_line" 'I/app: onResume\n')
check "an app without Ren'Py's lines: the ANR" "$anr" "FAIL  $label: the app's main thread did not answer"
# RESUME_NO_REFERENCE: notes that say why, no FAIL.
why="black after a return on Android 15 too"
noted=$(RESUME_NO_REFERENCE=$why trip "$before" "$stop|I/x: y\n|I/x: y\n|$done_" "$entered")
check "no reference: a note" "$noted" "note  $label: the app's onStop took 3.0 s in the background"
check "no reference: why" "$noted" "(not judged: $why)"
none "no reference: no FAIL" "$noted" "FAIL"
if (( failures > 0 )); then echo "judge_away_test: $failures FAILED"; exit 1; fi
echo "judge_away_test: ok"
