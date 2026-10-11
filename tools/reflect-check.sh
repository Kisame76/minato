#!/usr/bin/env bash
# Runs the test of reflection over the framework's classes, reflect-test, on the Mac (framework/tests/java_app_run.py;
# retest 8's batch B3): builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with
# libandroid_runtime.so (incremental), framework.jar (about 10 s) and reflect-test (seconds) every time, so it never tests
# an old binary; then starts reflect-test through app_process and compares its log with Android 15's on apk-runner-35
# (fixtures/expected/reflect-test.logcat, from tools/android-reference.sh --app reflect-test): whether getDeclaredMethods,
# getDeclaredFields and getDeclaredConstructors of the classes that libraries reflect over succeed.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/reflect-check.sh
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
tools/build-java-test-app.sh reflect-test >/dev/null

python3 framework/tests/java_app_run.py reflect-test io.github.kisame76.minato.test.reflect \
  io.github.kisame76.minato.test.reflect.ReflectApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/reflect-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/reflect-test/reflect-test.apk fixtures/expected/reflect-test.logcat
