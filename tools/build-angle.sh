#!/bin/bash
# Fetches and builds ANGLE's libGLESv2 for Minato (ADR-0029); the work is in tools/build-angle.py.
# Never called by tools/build-native.sh: the app build only embeds what build/ANGLE.xcframework holds.
set -euo pipefail
exec python3 "$(cd "$(dirname "$0")" && pwd)/build-angle.py" "$@"
