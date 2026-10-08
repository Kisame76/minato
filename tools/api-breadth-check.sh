#!/usr/bin/env bash
# Runs the framework breadth's test on the Mac (the compatibility plan's entry 22, framework/tests/java_app_run.py):
# builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and api-breadth-test (seconds) every time, so it never tests an old
# binary; then starts api-breadth-test through app_process, whose Application loads each public type of its
# assets/types.txt by name, without and then with its initialization, and compares its log with Android 15's
# (fixtures/expected/api-breadth-test.logcat, from tools/android-reference.sh --app api-breadth-test): every type of
# the batches that took it loads.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/api-breadth-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh api-breadth-test >/dev/null

python3 framework/tests/java_app_run.py api-breadth-test io.github.kisame76.minato.test.apibreadth \
  io.github.kisame76.minato.test.apibreadth.BreadthApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/api-breadth-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/api-breadth-test/api-breadth-test.apk \
  fixtures/expected/api-breadth-test.logcat
