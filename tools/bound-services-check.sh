#!/usr/bin/env bash
# Runs the test of the app's own bound services on the Mac (framework/tests/java_app_run.py; ADR-0045 step SV3):
# builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and bound-services-test (seconds) every time, so it never tests an old
# binary; then starts bound-services-test through app_process in a session on the M4 plan's display, as
# app-services-check, and compares its log with Android 15's on apk-runner-35
# (fixtures/expected/bound-services-test.logcat, from tools/android-reference.sh --app bound-services-test):
# bindService and unbindService with their ServiceConnections, onBind, onUnbind and onRebind.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/bound-services-check.sh
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
tools/build-java-test-app.sh bound-services-test >/dev/null

python3 framework/tests/java_app_run.py bound-services-test io.github.kisame76.minato.test.boundservices \
  io.github.kisame76.minato.test.boundservices.BoundServicesApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/bound-services-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/bound-services-test/bound-services-test.apk \
  fixtures/expected/bound-services-test.logcat 1290x2796@3,en-US
