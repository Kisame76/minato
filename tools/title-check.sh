#!/usr/bin/env bash
# Runs the test of window titles, title-test, on the Mac (framework/tests/java_app_run.py): builds apk-run and
# apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental),
# framework.jar (about 10 s) and title-test (seconds) every time, so it never tests an old binary; then starts
# title-test through app_process in a session on the M4 plan's display and compares its log with Android 15's on
# apk-runner-35-iphone-gesture (fixtures/expected/title-test.logcat, from tools/android-reference.sh --app title-test):
# a dialog with setTitle, one without, one with FEATURE_NO_TITLE, a dialog-themed activity's title from its label and
# from setTitle, an activity on Theme.Light, whose title bar has no action bar, and one on Theme.Material.Light, whose
# title is in its action bar; each window's decor size, its title view and where the content starts.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/title-check.sh
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
tools/build-java-test-app.sh title-test >/dev/null

python3 framework/tests/java_app_run.py --lifecycle start \
  title-test io.github.kisame76.minato.test.title \
  android.app.Application build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/title-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/title-test/title-test.apk fixtures/expected/title-test.logcat \
  1290x2796@3,en-US
