#!/usr/bin/env bash
# Runs the feasibility spikes S1–S3 and S1b (docs/STATUS.md) on the connected iPhone and prints the report.
# Usage: tools/spikes.sh [--without-increased-memory] [device name or identifier]
#   --without-increased-memory  signs the app without its entitlements file, so without
#                               com.apple.developer.kernel.increased-memory-limit (S2 baseline)
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "${1:-}" == "--without-increased-memory" ]]; then
  export BUILD_SETTINGS="CODE_SIGN_ENTITLEMENTS="
  shift
fi
LAUNCH_ARGS="-runSpikes" tools/deploy.sh "$@"

echo "Entitlements of this build:"
codesign -d --entitlements - --xml build/DerivedData/Build/Products/Debug-iphoneos/Minato.app 2>/dev/null \
  | plutil -convert json -o - - | python3 -c 'import json, sys; print(*("  " + key for key in json.load(sys.stdin)), sep="\n")'
