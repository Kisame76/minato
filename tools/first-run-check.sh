#!/usr/bin/env bash
# A new user's first run in the Simulator (app/UITests/FirstRunUITests.swift), for each release candidate: the given
# Minato.app (Release, as it ships) installed fresh on the Simulator, java-test.apk put into Files' On My iPhone in a
# folder of the check's own, then the runner: welcome, Continue, the empty library, "+" until the tile is Ready, the
# start, the menu by the top-edge swipe, Quit with no "quit unexpectedly", Settings. Prints each step's time. The folder
# goes again at the end; Minato stays installed, and what Minato held on this Simulator before is gone (it reinstalls).
# Usage: APP=<Minato.app> tools/first-run-check.sh [simulator name or UDID]
# A Simulator that is not booted is booted by tools/boot-simulator.sh (once; a failed boot ends the check).
set -uo pipefail
cd "$(dirname "$0")/.."
APP=${APP:?usage: APP=<Minato.app> tools/first-run-check.sh [simulator]}
APK=${FIRST_RUN_APK:-build/java-test/java-test.apk}
FOLDER="Minato First Run"
tools/check-bundle.sh "$APP" || exit 1
[[ -f "$APK" ]] || { echo "No $APK: run tools/build-java-test-app.sh java-test first." >&2; exit 1; }
read -r UDID _ < <(python3 tools/find-simulator.py "${1:-}") || true
[[ -n "${UDID:-}" ]] || { echo "No matching iPhone simulator found." >&2; exit 1; }
tools/boot-simulator.sh "$UDID" || exit 1

# Files' On My iPhone is the local storage provider's group container (one of several with a File Provider Storage).
storage=""
for group in "$HOME/Library/Developer/CoreSimulator/Devices/$UDID/data/Containers/Shared/AppGroup"/*; do
  id=$(plutil -extract MCMMetadataIdentifier raw "$group/.com.apple.mobile_container_manager.metadata.plist" \
    2>/dev/null)
  [[ "$id" == group.com.apple.FileProvider.LocalStorage ]] && storage="$group/File Provider Storage"
done
[[ -d "$storage" ]] || { echo "FAIL     no On My iPhone storage in this Simulator" >&2; exit 1; }
place="$storage/$FOLDER"
trap 'rm -rf "${place:?}"' EXIT
mkdir -p "$place"
cp "$APK" "$place/"

xcrun simctl terminate "$UDID" io.github.kisame76.minato > /dev/null 2>&1
xcrun simctl uninstall "$UDID" io.github.kisame76.minato
xcrun simctl install "$UDID" "$APP" || { echo "FAIL     the install of $APP" >&2; exit 1; }
TEST_RUNNER_MINATO_FOLDER=$FOLDER TEST_RUNNER_MINATO_APK=$(basename "$APK") TESTS=FirstRunUITests \
  tools/tab-swipe-check.sh "$UDID"
