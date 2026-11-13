#!/usr/bin/env bash
# Runs the test of SoundPool (soundpool-test) on the Mac
# (framework/tests/java_app_run.py): it builds apk-run and apk-convert for macOS into build/native/macos-app, app_process with libandroid_runtime.so
# (incremental), framework.jar and the app every time, then starts the app through app_process and compares its log
# with Android 15's (fixtures/expected/soundpool-test.logcat, from tools/android-reference.sh --app soundpool-test):
# the sound IDs, the load statuses, the stream IDs play returns, the stealing in a pool of two and unload's answers.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/soundpool-check.sh
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
tools/build-java-test-app.sh soundpool-test >/dev/null

python3 framework/tests/java_app_run.py soundpool-test io.github.kisame76.minato.test.soundpool \
  io.github.kisame76.minato.test.soundpool.SoundPoolApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/soundpool-check build/framework-native/app_process64 \
  build/framework/framework.jar \
  build/framework-res/framework-res build/soundpool-test/soundpool-test.apk \
  fixtures/expected/soundpool-test.logcat
