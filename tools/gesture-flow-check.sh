#!/usr/bin/env bash
# Runs ADR-0039 step b3.5b2-1b's test on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for
# macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about
# 10 s) and gesture-flow-test (seconds) every time, so it never tests an old binary; then starts gesture-flow-test
# through app_process in a session on the M4 plan's display, as tools/touch-check.sh does, and compares its log with
# Android 15's (fixtures/expected/gesture-flow-test.logcat, from tools/android-reference.sh --app gesture-flow-test):
# apk-run's lifecycle plays a 3 s swipe down the window's middle, whose DOWN starts Second, so that the finger is still
# down after First stops, and then a tap in Second's window. First's gesture ends with a CANCEL, and Second's first
# event is its own tap's DOWN.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/gesture-flow-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh gesture-flow-test >/dev/null

python3 framework/tests/java_app_run.py --lifecycle start,swipe:645:1000:645:2200:3000,tap:645:1398 \
  gesture-flow-test io.github.kisame76.minato.test.gestureflow \
  io.github.kisame76.minato.test.gestureflow.GestureApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/gesture-flow-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/gesture-flow-test/gesture-flow-test.apk \
  fixtures/expected/gesture-flow-test.logcat 1290x2796@3,en-US
