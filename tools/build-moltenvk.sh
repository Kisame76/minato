#!/bin/bash
# Builds MoltenVK from its pinned sources with the private Metal API compiled in and off by default (ADR-0044 b));
# the work is in tools/build-moltenvk.py. Never called by tools/build-native.sh: the app build only embeds what
# build/MoltenVK.xcframework holds.
set -euo pipefail
exec python3 "$(cd "$(dirname "$0")" && pwd)/build-moltenvk.py" "$@"
