#!/usr/bin/env bash
# Runs the platform action bar's options menu test (action-bar-menu-test; the compatibility plan's gap 5, step 4) on the
# Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS into build/native/macos-app,
# app_process with libandroid_runtime.so (incremental), framework.jar and the app every time, then starts the app through
# app_process on the M4 plan's display and plays the keys that the AVD's adb shell input keyevent plays in
# tools/android-reference.sh: MENU (the overflow opens), DPAD_DOWN and ENTER (the popup's first item is chosen), and,
# after the app's invalidateOptionsMenu, MENU twice (the overflow opens and closes). The app's log is compared with
# Android 15's (fixtures/expected/action-bar-menu-test.logcat, from tools/android-reference.sh).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/action-bar-menu-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null

app=action-bar-menu-test
tools/build-java-test-app.sh "$app" >/dev/null
python3 framework/tests/java_app_run.py \
  --lifecycle start,key:82,key:20,key:66,key:82,key:82 \
  "$app" io.github.kisame76.minato.test.actionbarmenu android.app.Application \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" "build/$app-check" \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  "build/$app/$app.apk" "fixtures/expected/$app.logcat" 1290x2796@3,en-US
