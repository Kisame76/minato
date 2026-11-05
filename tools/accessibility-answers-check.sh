#!/usr/bin/env bash
# Runs the test of the accessibility data classes (the compatibility plan's gap 1: AccessibilityNodeInfo with its
# actions, collection and range infos, AccessibilityWindowInfo and AccessibilityServiceInfo) on the Mac
# (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS into build/native/macos-app, and
# app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and accessibility-answers-test
# (seconds) every time, so it never tests an old binary; then starts the app's own Application through app_process and
# compares its log with Android 15's (fixtures/expected/accessibility-answers-test.logcat, from
# tools/android-reference.sh --app accessibility-answers-test): node infos filled and read, copied and sent through a
# parcel, a view's own node info, window infos and service infos.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/accessibility-answers-check.sh
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
tools/build-java-test-app.sh accessibility-answers-test >/dev/null

python3 framework/tests/java_app_run.py --lifecycle start accessibility-answers-test \
  io.github.kisame76.minato.test.accessibility \
  io.github.kisame76.minato.test.accessibility.AccessibilityApplication build/art \
  build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/accessibility-answers-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/accessibility-answers-test/accessibility-answers-test.apk \
  fixtures/expected/accessibility-answers-test.logcat 1290x2796@3,en-US
