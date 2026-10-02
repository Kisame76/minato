#!/usr/bin/env bash
# Runs ADR-0039's test 1 on the Mac (framework/tests/app_process_run.py): builds apk-run and apk-convert for macOS
# into build/native/macos-app, and app_process (incremental), framework.jar (about 10 s) and app-start-test (seconds)
# every time, so it never tests an old binary; then starts app-start-test through app_process and compares its lines
# with Android 15's.
# Usage: tools/app-process-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-app-start-test.sh >/dev/null

python3 framework/tests/app_process_run.py build/art build/bionic/lib build/bionic/system \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/app-process-check build/framework-native/app_process64 \
  build/framework/framework.jar build/app-start-test/app-start-test.apk fixtures/expected/app-start-test.logcat
