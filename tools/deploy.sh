#!/usr/bin/env bash
# Build APKRunner for the connected iPhone, install it and launch it.
# Needs: Apple ID in Xcode (Settings > Accounts), the iPhone paired, unlocked
# and connected, and Developer Mode enabled on the iPhone.
# Usage: tools/deploy.sh [device name or identifier]
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.apkrunner"
DERIVED="build/DerivedData"

DEVICES_JSON="$(mktemp)"
trap 'rm -f "$DEVICES_JSON"' EXIT
xcrun devicectl list devices --json-output "$DEVICES_JSON" >/dev/null

# Prints "<identifier> <udid> <developer mode> <connection state>" of the first
# matching physical iOS device. Reads the devicectl JSON v5 "properties" layout
# and falls back to the deprecated fields.
read -r DEVICE_ID DEVICE_UDID DEV_MODE CONN_STATE < <(python3 - "$DEVICES_JSON" "${1:-}" <<'PY'
import json
import sys

path, wanted = sys.argv[1], sys.argv[2]
for device in json.load(open(path))["result"]["devices"]:
    props = device.get("properties", {})
    hardware = props.get("hardware") or device.get("hardwareProperties", {})
    state = props.get("state", {})
    name = state.get("name") or device.get("deviceProperties", {}).get("name", "")
    if hardware.get("reality") != "physical" or hardware.get("platform") != "iOS":
        continue
    if wanted and wanted not in (device["identifier"], hardware.get("udid"), name):
        continue
    dev_mode = state.get("developerModeStatus") or device.get("deviceProperties", {}).get("developerModeStatus")
    if isinstance(dev_mode, dict):
        dev_mode = next(iter(dev_mode), "unknown")
    connection = props.get("connection", {}).get("state") or device.get("connectionProperties", {}).get("tunnelState", "unknown")
    print(device["identifier"], hardware.get("udid", ""), dev_mode or "unknown", connection)
    break
PY
) || true

if [[ -z "${DEVICE_ID:-}" ]]; then
  echo "No paired iPhone found. Connect it by cable and trust this Mac." >&2
  exit 1
fi
if [[ "$DEV_MODE" == "disabled" ]]; then
  echo "Developer Mode is off. iPhone: Settings > Privacy & Security > Developer Mode, then restart." >&2
  exit 1
fi

tools/gen.sh
xcodebuild -project APKRunner.xcodeproj -scheme APKRunner -configuration Debug \
  -destination "id=$DEVICE_UDID" -derivedDataPath "$DERIVED" \
  -allowProvisioningUpdates -allowProvisioningDeviceRegistration -quiet build

APP="$DERIVED/Build/Products/Debug-iphoneos/APKRunner.app"
xcrun devicectl device install app --device "$DEVICE_ID" "$APP"
xcrun devicectl device process launch --device "$DEVICE_ID" "$BUNDLE_ID"
echo "Launched on device $DEVICE_ID (connection: $CONN_STATE)"
