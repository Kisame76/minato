#!/usr/bin/env bash
# Builds, once, what the Mac checks (tools/*-check.sh) each rebuild before their run: the harness build/native/macos-app
# (apk-run, apk-convert, graphics-run), the framework's native code and framework.jar, and writes the stamps that let
# MINATO_PREPARED=1 skip them while the tree is unchanged (tools/prepared-stamp.py; build-framework.sh and
# build-framework-native.sh read their own stamps, tools/lane-bin/cmake the harness's). Run it again after the tree
# changes: with MINATO_PREPARED=1 it builds only what changed (tools/check-lanes.sh calls it that way first). The
# checks themselves are untouched: without MINATO_PREPARED they build as before.
# Usage: tools/check-prepare.sh
set -euo pipefail
cd "$(dirname "$0")/.."

NATIVE=build/native/macos-app
NATIVE_INPUTS=(CMakeLists.txt CMakePresets.json runtime importer graphics third_party/sources.json)
if ! python3 tools/prepared-stamp.py check native-macos-app "$NATIVE/runtime/apk-run" "${NATIVE_INPUTS[@]}"; then
  digest=$(python3 tools/prepared-stamp.py compute "${NATIVE_INPUTS[@]}")
  PATH="${PATH//$PWD\/tools\/lane-bin:/}"   # the real cmake, not the lanes' shim
  cmake --preset macos -B "$NATIVE" >/dev/null
  cmake --build "$NATIVE" --target apk-run apk-convert graphics-run >/dev/null
  python3 tools/prepared-stamp.py write native-macos-app "$digest"
fi
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
# make-apk is built by swift build under a lock; built here once, the lanes do not queue on it
tools/make-apk.sh --help >/dev/null 2>&1 || true
echo "check-prepare: prepared; run checks with MINATO_PREPARED=1 (tools/check-lanes.sh does)"
