#!/usr/bin/env bash
# Builds the host aapt2 (ADR-0039 b1) from build/sources with tools/aapt2/CMakeLists.txt into build/aapt2/aapt2,
# with Ninja and 6 jobs; Ninja rebuilds only what changed. Needs the sources of tools/fetch-sources.py --set framework
# and --set art. tools/aapt2-check.sh checks the result.
# Usage: tools/build-aapt2.sh
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S tools/aapt2 -B build/aapt2 -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/aapt2 -j 6
echo "build-aapt2: build/aapt2/aapt2"
