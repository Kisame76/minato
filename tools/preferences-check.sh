#!/usr/bin/env bash
# Runs the test of android.preference, preferences-test, on the Mac (framework/tests/java_app_run.py; framework breadth,
# step (c)): builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with
# libandroid_runtime.so (incremental), framework.jar (about 10 s) and preferences-test (seconds) every time, so it never
# tests an old binary; then starts preferences-test through app_process and compares its log with Android 15's on
# apk-runner-35 (fixtures/expected/preferences-test.logcat, from tools/android-reference.sh --app preferences-test): the
# defaults an XML tree of every preference type writes into the default SharedPreferences, and the types' own state.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/preferences-check.sh
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
tools/build-java-test-app.sh preferences-test >/dev/null

python3 framework/tests/java_app_run.py preferences-test io.github.kisame76.minato.test.preferences \
  io.github.kisame76.minato.test.preferences.PreferencesApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/preferences-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/preferences-test/preferences-test.apk fixtures/expected/preferences-test.logcat
