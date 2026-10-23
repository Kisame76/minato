#!/usr/bin/env bash
# Build Minato for the connected iPhone, install it and launch it.
# Needs: Apple ID in Xcode (Settings > Accounts), the iPhone paired, unlocked
# and connected, and Developer Mode enabled on the iPhone.
# After installing, it hands the app the certificate Xcode signed it with, for signing converted
# code (tools/hand-over-certificate.sh), when the app does not have that one yet; only a Debug build can take it.
# Usage: tools/deploy.sh [device name or identifier]
# Optional environment:
#   CONFIGURATION   Debug (default) or Release, with the runtime's and graphics/host's Release builds;
#                   tools/benchmark.sh builds Release
#   BUILD_SETTINGS  extra xcodebuild settings, e.g. "CODE_SIGN_ENTITLEMENTS=" to sign without app/Minato.entitlements
#   LAUNCH_ARGS     arguments for the app; its output is then shown here until it exits
#   INSTALL_ONLY    when set, installs the app without launching it
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.minato"
DERIVED="build/DerivedData"
CONFIGURATION="${CONFIGURATION:-Debug}"

read -r DEVICE_ID DEVICE_UDID DEV_MODE CONN_STATE < <(python3 tools/find-device.py "${1:-}") || true

if [[ -z "${DEVICE_ID:-}" ]]; then
  echo "No paired iPhone found. Connect it by cable and trust this Mac." >&2
  exit 1
fi
if [[ "$DEV_MODE" == "disabled" ]]; then
  echo "Developer Mode is off. iPhone: Settings > Privacy & Security > Developer Mode, then restart." >&2
  exit 1
fi
python3 tools/require-cable.py "$DEVICE_ID" || exit 1 # only by cable, never over the network

read -r -a EXTRA_SETTINGS <<< "${BUILD_SETTINGS:-}"
read -r -a APP_ARGS <<< "${LAUNCH_ARGS:-}"

tools/gen.sh
# A Release build links the Release builds of the runtime and of graphics/host (ADR-0037, decision 3);
# a Debug build packs the Debug builds again.
RELEASE=()
[[ "$CONFIGURATION" != Release ]] || RELEASE=(--release)
tools/build-native.sh --app ${RELEASE[@]+"${RELEASE[@]}"}
tools/build-graphics-host.sh ${RELEASE[@]+"${RELEASE[@]}"}
# The ${a[@]+"${a[@]}"} form keeps empty arrays working under `set -u` in macOS's bash 3.2.
xcodebuild -project Minato.xcodeproj -scheme Minato -configuration "$CONFIGURATION" \
  -destination "id=$DEVICE_UDID" -derivedDataPath "$DERIVED" \
  -allowProvisioningUpdates -allowProvisioningDeviceRegistration -quiet build \
  ${EXTRA_SETTINGS[@]+"${EXTRA_SETTINGS[@]}"}

APP="$DERIVED/Build/Products/$CONFIGURATION-iphoneos/Minato.app"
xcrun devicectl device install app --device "$DEVICE_ID" "$APP"
# Only the Debug build answers the hand-over (app/Sources/Signing/CertificateHandover.swift); asking a
# Release build would wait out devicectl's 120 s.
if [[ "$CONFIGURATION" == Debug ]]; then
  tools/hand-over-certificate.sh "$DEVICE_ID" ||
    echo "The signing certificate was not handed over; converted code loads only once the app has it." >&2
else
  echo "Signing certificate: not handed over to a $CONFIGURATION build; the app keeps the one a Debug install handed over."
fi
if [[ -n "${INSTALL_ONLY:-}" ]]; then
  echo "Installed on device $DEVICE_ID (connection: $CONN_STATE)"
elif [[ ${#APP_ARGS[@]} -gt 0 ]]; then
  xcrun devicectl device process launch --device "$DEVICE_ID" --terminate-existing --console \
    "$BUNDLE_ID" "${APP_ARGS[@]}"
else
  xcrun devicectl device process launch --device "$DEVICE_ID" --terminate-existing "$BUNDLE_ID"
  echo "Launched on device $DEVICE_ID (connection: $CONN_STATE)"
fi
