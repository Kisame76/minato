#!/usr/bin/env bash
# Runs the selection mode test (selection-mode-test; the text selection's floating action mode) on the Mac
# (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS into build/native/macos-app, app_process
# with libandroid_runtime.so (incremental), framework.jar and the app every time, then starts the app through
# app_process on the M4 plan's display and long-presses the word the app names in its "ready" line, a second after the
# start, as `input swipe` does on the AVD: the selection's action mode starts, which the app's custom callback and its
# activity log. The log is compared with Android 15's (fixtures/expected/selection-mode-test.logcat, from
# tools/android-reference.sh). Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and
# tools/build-framework-res.sh first. Usage: tools/selection-mode-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null

app=selection-mode-test
tools/build-java-test-app.sh "$app" >/dev/null
point=$(sed -n 's/^I\/selection-mode-test: ready: long press at \([0-9]*\),\([0-9]*\)$/\1:\2/p' \
  "fixtures/expected/$app.logcat")
python3 framework/tests/java_app_run.py \
  --lifecycle "start,swipe:$point:$point:1000" \
  "$app" io.github.kisame76.minato.test.selectionmode android.app.Application \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" "build/$app-check" \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  "build/$app/$app.apk" "fixtures/expected/$app.logcat" 1290x2796@3,en-US
