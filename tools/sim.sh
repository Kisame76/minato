#!/usr/bin/env bash
# Build Minato for the iOS Simulator, install it and launch it.
# Usage: tools/sim.sh [--headless] [simulator name or UDID]
# Without a name the newest available iPhone simulator is used.
# Optional environment, as for tools/deploy.sh:
#   BUILD_SETTINGS  extra xcodebuild settings, e.g. "APK_EMBED_FIXTURES=YES"
#   INSTALL_ONLY    when set, installs the app without launching it
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.minato"
DERIVED="build/DerivedData"

HEADLESS=0
if [[ "${1:-}" == "--headless" ]]; then
  HEADLESS=1
  shift
fi

read -r SIM_UDID SIM_NAME < <(python3 tools/find-simulator.py "${1:-}") || true

if [[ -z "${SIM_UDID:-}" ]]; then
  echo "No matching iPhone simulator found." >&2
  exit 1
fi

tools/gen.sh
tools/build-native.sh --app
read -r -a EXTRA_SETTINGS <<< "${BUILD_SETTINGS:-}"
# The ${a[@]+"${a[@]}"} form keeps empty arrays working under `set -u` in macOS's bash 3.2.
xcodebuild -project Minato.xcodeproj -scheme Minato -configuration Debug \
  -destination "id=$SIM_UDID" -derivedDataPath "$DERIVED" -quiet build \
  ${EXTRA_SETTINGS[@]+"${EXTRA_SETTINGS[@]}"}

APP="$DERIVED/Build/Products/Debug-iphonesimulator/Minato.app"
xcrun simctl boot "$SIM_UDID" 2>/dev/null || true # already booted is fine
xcrun simctl install "$SIM_UDID" "$APP"
if [[ -n "${INSTALL_ONLY:-}" ]]; then
  echo "Installed on $SIM_NAME ($SIM_UDID)"
  exit 0
fi
xcrun simctl launch "$SIM_UDID" "$BUNDLE_ID"
[[ "$HEADLESS" == 1 ]] || open -a Simulator
echo "Running on $SIM_NAME ($SIM_UDID)"
