#!/usr/bin/env bash
# Runs the test of the Context calls and classes that apps' SDKs use at their start on the Mac
# (framework/tests/java_app_run.py; the compatibility plan's Unity test APK probe): builds apk-run and apk-convert for macOS
# into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s)
# and contexts-test (seconds) every time, so it never tests an old binary; then starts contexts-test through
# app_process in a session on the M4 plan's display, as tools/launch-mode-check.sh does, and compares its log with
# Android 15's (fixtures/expected/contexts-test.logcat, from tools/android-reference.sh --app contexts-test): the main
# looper; package contexts for the app's own package, "android" and a missing one; the device protected storage
# context with its own folder, preferences and moves; the configuration, display and split contexts; ConditionVariable;
# a Messenger to a Handler of the main thread through its binder.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/contexts-check.sh
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
tools/build-java-test-app.sh contexts-test >/dev/null

python3 framework/tests/java_app_run.py contexts-test io.github.kisame76.minato.test.contexts \
  io.github.kisame76.minato.test.contexts.ContextsApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/contexts-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/contexts-test/contexts-test.apk \
  fixtures/expected/contexts-test.logcat 1290x2796@3,en-US
