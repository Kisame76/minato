#!/usr/bin/env bash
# Runs the test of the framework breadth's batch F, batch-f-test, on the Mac (framework/tests/java_app_run.py):
# android.text.format.Formatter, android.widget.SearchView, android.net.Proxy, android.telephony.PhoneNumberUtils,
# android.widget.VideoView and android.net.LocalSocket with LocalServerSocket, each with the answers an app reads. It
# builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar and batch-f-test every time, so it never tests an old binary; then starts batch-f-test
# through app_process and compares its log with Android 15's on apk-runner-35 (fixtures/expected/batch-f-test.logcat,
# from tools/android-reference.sh --app batch-f-test).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/batch-f-check.sh
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
tools/build-java-test-app.sh batch-f-test >/dev/null

python3 framework/tests/java_app_run.py batch-f-test io.github.kisame76.minato.test.batchf \
  io.github.kisame76.minato.test.batchf.BatchFApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/batch-f-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/batch-f-test/batch-f-test.apk fixtures/expected/batch-f-test.logcat
