#!/usr/bin/env bash
# The swipe between Home, Library and Settings (app/UITests/TabSwipeUITests.swift) against the Minato installed on a
# Simulator (name or UDID as $1, tools/find-simulator.py's choice without) or, with --device, on the paired iPhone.
# Minato must be a Debug build: its DEBUG swipes start a swipe while a page slides, which XCUITest cannot.
# The runner (MinatoUITests) is built here; for the iPhone it needs an Xcode account (Xcode > Settings > Accounts).
# Minato starts in the light appearance (MINATO_APPEARANCE, its Debug build), where a page is never black, so the test
# counts black pages on the iPhone in dark mode too; the Simulator's own appearance stays as it is.
set -uo pipefail
cd "$(dirname "$0")/.."
ON_DEVICE=no
if [[ "${1:-}" == --device ]]; then
  ON_DEVICE=yes
  shift
fi
WORK=build/tab-swipe-check
mkdir -p "$WORK"
if [[ "$ON_DEVICE" == yes ]]; then
  read -r _ UDID _ _ < <(python3 tools/find-device.py "${1:-}") || true
  [[ -n "${UDID:-}" ]] || { echo "No paired iPhone found. Connect it by cable and trust this Mac." >&2; exit 1; }
  destination="generic/platform=iOS"
else
  read -r UDID _ < <(python3 tools/find-simulator.py "${1:-}") || true
  [[ -n "${UDID:-}" ]] || { echo "No booted Simulator found." >&2; exit 1; }
  destination="generic/platform=iOS Simulator"
fi
# Only a build for the iPhone is signed with a profile: Xcode talks to Apple (and needs the signed-in account) only when
# the local profile of Minato or of the runner is about to run out. tools/provisioning-updates.sh's output is the words
# (nothing, or the flag), as java-start-check's runner build takes them, so the tool alone decides.
SIGNING=()
if [[ "$ON_DEVICE" == yes ]]; then
  read -r -a SIGNING <<< "$(tools/provisioning-updates.sh io.github.kisame76.minato \
    io.github.kisame76.minato.uitests.xctrunner)"
fi
if ! { tools/gen.sh > "$WORK/build.log" 2>&1 &&
    xcodebuild build-for-testing -project Minato.xcodeproj -scheme MinatoUITests -destination "$destination" \
      -derivedDataPath "$WORK/runner" ${SIGNING[@]+"${SIGNING[@]}"} -quiet >> "$WORK/build.log" 2>&1; }; then
  if grep -q -E "No Accounts|No Account for Team|requires a development team" "$WORK/build.log"; then
    echo "skipped  the UI test runner needs an Xcode account (Xcode > Settings > Accounts)"
  else
    echo "FAIL     the UI test runner did not build: $(grep -m 1 'error:' "$WORK/build.log")"
  fi
  exit 1
fi
run=$(ls "$WORK"/runner/Build/Products/*.xctestrun | head -n 1)
# TESTS names other classes of the runner, SelectModeUITests say (Library's Select mode; the library must hold an
# app).
only=()
for test in ${TESTS:-TabSwipeUITests}; do only+=(-only-testing:"MinatoUITests/$test"); done
xcodebuild test-without-building -xctestrun "$run" -destination "id=$UDID" \
  "${only[@]}" > "$WORK/test.log" 2>&1
status=$?
grep -o "MINATO-UITEST .*" "$WORK/test.log" | sed 's/^MINATO-UITEST/note    /'
grep -E "^Test Case .*(passed|failed)" "$WORK/test.log" |
  sed -E 's/^Test Case .-\[MinatoUITests\.([^]]*)\]. /\1 /'
if [[ $status == 0 ]]; then
  echo "ok       ${TESTS:-TabSwipeUITests} passed"
else
  echo "FAIL     ${TESTS:-TabSwipeUITests} failed (or the run did): $WORK/test.log"
fi
exit $status
