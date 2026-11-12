#!/usr/bin/env bash
# Red test for tools/public-numbers-check.sh: a clean tree passes, and each doctored copy of one number (in a text or in
# the code the number comes from) fails with its own line; a reworded sentence is WRONG, a missing source file is SKIP.
# The tree is read only and its files are copied. Usage: tools/tests/public_numbers_check_test.sh [tree]
set -u
here="$(cd "$(dirname "$0")/.." && pwd)"
tree="${1:-$(cd "$here/.." && pwd)}"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/pnc-test.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT
failures=0
FILES=(README.md CHANGELOG.md THIRD_PARTY.md project.yml docs/COMPATIBILITY.md docs/BENCHMARKS.md
  fixtures/expected/service-inventory-test.logcat fixtures/apps/sdl-audio-test/main.c third_party/sources.json
  tools/renpy-check.sh tools/build-framework-native.sh tools/build-bionic.sh runtime/android/stubgen.py
  importer/APKKit/Sources/APKSession
  importer/APKKit/Sources/APKLibrary/HomeScreenGrid.swift framework/java/android/app/LocalAlarmManager.java
  framework/java/android/app/NotificationService.java
  framework/tests/java/com/android/minato/notifications/NotificationTextTest.java)
fresh() {
  rm -rf "$scratch/t"; mkdir "$scratch/t"
  (cd "$tree" && tar -cf - "${FILES[@]}" 2>/dev/null) | tar -xf - -C "$scratch/t"
}
edit() {  # edit <file> <from> <to>: exactly one literal replacement, or the test itself is broken
  /usr/bin/python3 - "$scratch/t/$1" "$2" "$3" <<'PY' || { echo "TEST BROKEN: '$2' not in $1"; exit 3; }
import sys
p, a, b = sys.argv[1:]
s = open(p, encoding="utf-8").read()
if s.count(a) < 1:
    sys.exit(1)
open(p, "w", encoding="utf-8").write(s.replace(a, b))
PY
}
expect() {  # expect <name> <exit status> <pattern that must be in the output>
  local name="$1" want="$2" pattern="$3" out status
  out="$("$here/public-numbers-check.sh" "$scratch/t" 2>&1)"; status=$?
  if [[ $status -eq $want ]] && grep -q -E "$pattern" <<< "$out"; then echo "ok   $name"
  else echo "FAIL $name (exit $status, wanted $want, pattern: $pattern)"; failures=$((failures + 1)); fi
}
fresh; expect "a clean tree passes" 0 "0 WRONG"
fresh; edit docs/COMPATIBILITY.md "Android 15's 88 public system services" "Android 15's 89 public system services"
expect "a doctored service count in COMPATIBILITY is WRONG" 1 "WRONG services: COMPATIBILITY, public"
fresh; edit docs/COMPATIBILITY.md "**17 closed," "**18 closed,"
expect "a doctored closed count is WRONG" 1 "WRONG gaps: closed in total"
fresh; edit CHANGELOG.md "closed itself within 10 seconds" "closed itself within 12 seconds"
expect "a doctored number in the CHANGELOG is WRONG" 1 "WRONG changelog: an app that closed itself"
fresh; edit importer/APKKit/Sources/APKSession/ClosedItself.swift "window: Double = 10" "window: Double = 12"
expect "a doctored source constant is WRONG" 1 "WRONG changelog: an app that closed itself"
fresh; edit CHANGELOG.md "at most once in 60 seconds" "at most once a minute"
expect "a reworded sentence is WRONG, not silently OK" 1 "WRONG changelog: onLowMemory.*not in the text"
fresh; edit README.md "iOS 26 or later" "iOS 25 or later"
expect "a doctored platform number in the README is WRONG" 1 "WRONG platform: iOS floor"
fresh; edit docs/COMPATIBILITY.md "MoltenVK 1.4.2 reports" "MoltenVK 1.4.3 reports"
expect "a doctored version is WRONG" 1 "WRONG versions: MoltenVK"
fresh; edit docs/COMPATIBILITY.md "(56 % of the native FPS in that test)" "(57 % of the native FPS in that test)"
expect "a doctored benchmark percent is WRONG" 1 "WRONG benchmark: ES 3.1"
fresh; edit docs/COMPATIBILITY.md "the API 35 NDK" "the API 36 NDK"
expect "a doctored API level of the NDK stubs in COMPATIBILITY is WRONG" 1 "WRONG platform: API level of the NDK stubs"
fresh; edit runtime/android/stubgen.py "API = 35" "API = 36"
expect "a doctored API level in the stub generator is WRONG" 1 "WRONG platform: API level of the NDK stubs"
fresh; edit tools/build-bionic.sh "android-35" "android-36"
expect "a doctored platform of the bionic build is WRONG" 1 "WRONG platform: API level of the NDK stubs"
fresh; rm "$scratch/t/docs/BENCHMARKS.md"
expect "a missing source file is SKIP, not OK and not WRONG" 0 "SKIP +benchmark"
echo "$failures failed"; exit $((failures != 0))
