#!/usr/bin/env bash
# Build APKRunner for the connected iPhone, install it and launch it.
# Needs: Apple ID in Xcode (Settings > Accounts), the iPhone paired, unlocked
# and connected, and Developer Mode enabled on the iPhone.
# After installing, it hands the app the certificate Xcode signed it with, for signing converted
# code (tools/hand-over-certificate.sh), when the app does not have that one yet.
# Usage: tools/deploy.sh [device name or identifier]
# Optional environment:
#   BUILD_SETTINGS  extra xcodebuild settings, e.g. "CODE_SIGN_ENTITLEMENTS=" to sign without app/APKRunner.entitlements
#   LAUNCH_ARGS     arguments for the app; its output is then shown here until it exits
#   INSTALL_ONLY    when set, installs the app without launching it
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.apkrunner"
DERIVED="build/DerivedData"

read -r DEVICE_ID DEVICE_UDID DEV_MODE CONN_STATE < <(python3 tools/find-device.py "${1:-}") || true

if [[ -z "${DEVICE_ID:-}" ]]; then
  echo "No paired iPhone found. Connect it by cable and trust this Mac." >&2
  exit 1
fi
if [[ "$DEV_MODE" == "disabled" ]]; then
  echo "Developer Mode is off. iPhone: Settings > Privacy & Security > Developer Mode, then restart." >&2
  exit 1
fi

read -r -a EXTRA_SETTINGS <<< "${BUILD_SETTINGS:-}"
read -r -a APP_ARGS <<< "${LAUNCH_ARGS:-}"

tools/gen.sh
tools/build-native.sh --app
# The ${a[@]+"${a[@]}"} form keeps empty arrays working under `set -u` in macOS's bash 3.2.
xcodebuild -project APKRunner.xcodeproj -scheme APKRunner -configuration Debug \
  -destination "id=$DEVICE_UDID" -derivedDataPath "$DERIVED" \
  -allowProvisioningUpdates -allowProvisioningDeviceRegistration -quiet build \
  ${EXTRA_SETTINGS[@]+"${EXTRA_SETTINGS[@]}"}

APP="$DERIVED/Build/Products/Debug-iphoneos/APKRunner.app"
xcrun devicectl device install app --device "$DEVICE_ID" "$APP"
tools/hand-over-certificate.sh "$DEVICE_ID" ||
  echo "The signing certificate was not handed over; converted code loads only once the app has it." >&2
if [[ -n "${INSTALL_ONLY:-}" ]]; then
  echo "Installed on device $DEVICE_ID (connection: $CONN_STATE)"
elif [[ ${#APP_ARGS[@]} -gt 0 ]]; then
  xcrun devicectl device process launch --device "$DEVICE_ID" --terminate-existing --console \
    "$BUNDLE_ID" "${APP_ARGS[@]}"
else
  xcrun devicectl device process launch --device "$DEVICE_ID" --terminate-existing "$BUNDLE_ID"
  echo "Launched on device $DEVICE_ID (connection: $CONN_STATE)"
fi
