#!/bin/bash
# Fetches MoltenVK as released by Khronos, and vk.xml, for Minato (ADR-0030); the work is in
# tools/fetch-moltenvk.py. Never called by tools/build-native.sh: the app build only embeds what
# build/MoltenVK.xcframework holds.
set -euo pipefail
exec python3 "$(cd "$(dirname "$0")" && pwd)/fetch-moltenvk.py" "$@"
