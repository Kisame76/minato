#!/usr/bin/env bash
# Runs ADR-0039 step c2d's test on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS
# into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s)
# and app-calls-test (seconds) every time, so it never tests an old binary; then compares app-calls-test's log with
# Android 15's (fixtures/expected/app-calls-test.logcat, from tools/android-reference.sh --app app-calls-test): calls
# an app makes into the platform without a window, which failed while the classes behind them were compile-only
# stubs. Step c2d1: Log.wtf, which logs at ERROR and lets the app run on.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/app-calls-check.sh
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
tools/build-java-test-app.sh app-calls-test >/dev/null

python3 framework/tests/java_app_run.py app-calls-test io.github.kisame76.minato.test.appcalls \
  io.github.kisame76.minato.test.appcalls.AppCallsApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/app-calls-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/app-calls-test/app-calls-test.apk fixtures/expected/app-calls-test.logcat
