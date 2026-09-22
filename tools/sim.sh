#!/usr/bin/env bash
# Build APKRunner for the iOS Simulator, install it and launch it.
# Usage: tools/sim.sh [--headless] [simulator name or UDID]
# Without a name the newest available iPhone simulator is used.
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.apkrunner"
DERIVED="build/DerivedData"

HEADLESS=0
if [[ "${1:-}" == "--headless" ]]; then
  HEADLESS=1
  shift
fi

read -r SIM_UDID SIM_NAME < <(xcrun simctl list devices available --json | python3 -c '
import json, sys

wanted = sys.argv[1]
devices = json.load(sys.stdin)["devices"]
for runtime in sorted(devices, reverse=True):
    if "iOS" not in runtime:
        continue
    for device in devices[runtime]:
        if wanted and wanted not in (device["name"], device["udid"]):
            continue
        if not wanted and not device["name"].startswith("iPhone"):
            continue
        print(device["udid"], device["name"])
        sys.exit(0)
' "${1:-}") || true

if [[ -z "${SIM_UDID:-}" ]]; then
  echo "No matching iPhone simulator found." >&2
  exit 1
fi

tools/gen.sh
tools/build-native.sh --app
xcodebuild -project APKRunner.xcodeproj -scheme APKRunner -configuration Debug \
  -destination "id=$SIM_UDID" -derivedDataPath "$DERIVED" -quiet build

APP="$DERIVED/Build/Products/Debug-iphonesimulator/APKRunner.app"
xcrun simctl boot "$SIM_UDID" 2>/dev/null || true # already booted is fine
xcrun simctl install "$SIM_UDID" "$APP"
xcrun simctl launch "$SIM_UDID" "$BUNDLE_ID"
[[ "$HEADLESS" == 1 ]] || open -a Simulator
echo "Running on $SIM_NAME ($SIM_UDID)"
