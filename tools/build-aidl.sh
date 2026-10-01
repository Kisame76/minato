#!/usr/bin/env bash
# Builds the host aidl (ADR-0045 decision 1) from build/sources/aidl with tools/aidl/CMakeLists.txt into
# build/aidl/aidl, with Ninja and 6 jobs; Ninja rebuilds only what changed. Needs Bison 3 from Homebrew
# (macOS has 2.3) and the sources of tools/fetch-sources.py --set framework and --set art (libbase,
# fmtlib, liblog's headers). tools/aidl-check.sh checks the result.
# Usage: tools/build-aidl.sh
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S tools/aidl -B build/aidl -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/aidl -j 6
echo "build-aidl: build/aidl/aidl"
