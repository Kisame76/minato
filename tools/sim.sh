#!/usr/bin/env bash
# Build Minato for the iOS Simulator, install it and launch it.
# Usage: tools/sim.sh [--headless] [simulator name or UDID]
# Without a name the newest available iPhone simulator is used.
# Optional environment, as for tools/deploy.sh:
#   CONFIGURATION   Debug (default) or Release, with the runtime's and graphics/host's Release builds;
#                   tools/benchmark.sh builds Release
#   BUILD_SETTINGS  extra xcodebuild settings, e.g. "APK_EMBED_FIXTURES=YES"
#   INSTALL_ONLY    when set, installs the app without launching it
#   DERIVED         the derived data folder (build/DerivedData by default), e.g. a build of its own that does not
#                   replace the bundle another step installs (device-suite.sh's with the fixtures)
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.minato"
DERIVED="${DERIVED:-build/DerivedData}"
CONFIGURATION="${CONFIGURATION:-Debug}"

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
# A Release build links the Release builds of the runtime and of graphics/host (ADR-0037, decision 3);
# a Debug build packs the Debug builds again.
RELEASE=()
[[ "$CONFIGURATION" != Release ]] || RELEASE=(--release)
tools/build-native.sh --app ${RELEASE[@]+"${RELEASE[@]}"}
tools/build-graphics-host.sh ${RELEASE[@]+"${RELEASE[@]}"}
read -r -a EXTRA_SETTINGS <<< "${BUILD_SETTINGS:-}"
# The ${a[@]+"${a[@]}"} form keeps empty arrays working under `set -u` in macOS's bash 3.2.
xcodebuild -project Minato.xcodeproj -scheme Minato -configuration "$CONFIGURATION" \
  -destination "id=$SIM_UDID" -derivedDataPath "$DERIVED" -quiet build \
  ${EXTRA_SETTINGS[@]+"${EXTRA_SETTINGS[@]}"}

APP="$DERIVED/Build/Products/$CONFIGURATION-iphonesimulator/Minato.app"
# Once and loudly: a failed boot or a crashing SpringBoard ends here (tools/boot-simulator.sh).
tools/boot-simulator.sh "$SIM_UDID" || exit 1
xcrun simctl install "$SIM_UDID" "$APP"
if [[ -n "${INSTALL_ONLY:-}" ]]; then
  echo "Installed on $SIM_NAME ($SIM_UDID)"
  exit 0
fi
xcrun simctl launch "$SIM_UDID" "$BUNDLE_ID"
[[ "$HEADLESS" == 1 ]] || open -a Simulator
echo "Running on $SIM_NAME ($SIM_UDID)"
