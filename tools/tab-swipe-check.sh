#!/usr/bin/env bash
# The swipe between Home, Library and Settings (app/UITests/TabSwipeUITests.swift) against the Minato installed on a
# Simulator (name or UDID as $1, tools/find-simulator.py's choice without) or, with --device, on the paired iPhone.
# Minato must be a Debug build: its DEBUG swipes start a swipe while a page slides, which XCUITest cannot.
# The runner (MinatoUITests) is built here; for the iPhone it needs an Xcode account (Xcode > Settings > Accounts).
# In the Simulator the light appearance is set, where a page is never black, so the test also counts black pages.
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
  light=0
else
  read -r UDID _ < <(python3 tools/find-simulator.py "${1:-}") || true
  [[ -n "${UDID:-}" ]] || { echo "No booted Simulator found." >&2; exit 1; }
  destination="generic/platform=iOS Simulator"
  xcrun simctl ui "$UDID" appearance light
  light=1
fi
if ! { tools/gen.sh > "$WORK/build.log" 2>&1 &&
    xcodebuild build-for-testing -project Minato.xcodeproj -scheme MinatoUITests -destination "$destination" \
      -derivedDataPath "$WORK/runner" -allowProvisioningUpdates -quiet >> "$WORK/build.log" 2>&1; }; then
  if grep -q -E "No Accounts|No Account for Team|requires a development team" "$WORK/build.log"; then
    echo "skipped  the UI test runner needs an Xcode account (Xcode > Settings > Accounts)"
  else
    echo "FAIL     the UI test runner did not build: $(grep -m 1 'error:' "$WORK/build.log")"
  fi
  exit 1
fi
run=$(ls "$WORK"/runner/Build/Products/*.xctestrun | head -n 1)
TEST_RUNNER_MINATO_LIGHT=$light xcodebuild test-without-building -xctestrun "$run" -destination "id=$UDID" \
  -only-testing:MinatoUITests/TabSwipeUITests > "$WORK/test.log" 2>&1
status=$?
grep -o "MINATO-UITEST .*" "$WORK/test.log" | sed 's/^MINATO-UITEST/note    /'
grep -E "^Test Case .*(passed|failed)" "$WORK/test.log" |
  sed -E 's/^Test Case .-\[MinatoUITests.TabSwipeUITests ([^]]*)\]. /\1 /'
if [[ $status == 0 ]]; then
  echo "ok       every page was whole after each swipe"
else
  echo "FAIL     a swipe left a broken page (or the run failed): $WORK/test.log"
fi
exit $status
