#!/usr/bin/env bash
# Runs the test of second windows on the Mac (framework/tests/java_app_run.py; ADR-0041 gap 4 (b)): builds graphics-run
# (apk-run with graphics/host's providers) and apk-convert for macOS into build/native/macos-app, and app_process with
# libandroid_runtime.so (incremental), framework.jar (about 10 s) and second-windows-test (seconds) every time, so it
# never tests an old binary; then starts second-windows-test through app_process, whose host answers each window
# request and taps the touches part's windows a second apart, and compares its log with Android 15's on
# apk-runner-35-iphone-gesture (fixtures/expected/second-windows-test.logcat, from tools/android-reference.sh --app
# second-windows-test): an AlertDialog and PopupWindows in windows of their own, their frames, callbacks, touches, keys
# and focus, what a bad token meets, and a translucent and a floating activity above the launcher activity (gap 4
# (b3)). The windows have graphics-run's layers in decision 1's order (MINATO_SESSION_LAYERS=1,
# MINATO_WINDOW_WATCH=hidden, gap 4 (c)), whose lines the b3 part checks among the app's in this order: the
# translucent activity's own window as a requested one of type 1, its SurfaceView's as a MEDIA sub-window below it, its
# dialog above it, the dialog of the activity below in that one's group below it, gone with it at setTranslucent(false)
# and back at convertToTranslucent, and the floating activity's window at its frame above the dim.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/second-windows-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target graphics-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh second-windows-test >/dev/null

MINATO_SESSION_LAYERS=1 MINATO_WINDOW_WATCH=hidden python3 framework/tests/java_app_run.py \
  --in-order "I/second-windows-test: activity onPause" \
  --in-order "window host: window 10, type 1: at 0, 0, 1290 x 2796, alpha 1.00, dim 0.00, not opaque" \
  --in-order "window host: window 11, type 1001: at 0, 0, 300 x 300" \
  --in-order "window host: from the bottom: the window, window 11, window 10, dim, window 12" \
  --in-order "window host: from the bottom: the window, dim, window 13, window 11, window 10" \
  --in-order "I/second-windows-test: setTranslucent(false): true" --in-order "window host: window 13 goes" \
  --in-order "I/second-windows-test: convertToTranslucent: true" \
  --in-order "window host: from the bottom: the window, dim, window 14, window 11, window 10" \
  --in-order "window host: window 10 goes" \
  --in-order "window host: window 15, type 1: at 497, 1297, 295 x 201, alpha 1.00, dim 0.60, not opaque" \
  --in-order "window host: from the bottom: the window, dim, window 15" \
  --lifecycle start,tap:1077:1549,tap:645:700,tap:645:1398,tap:695:1573,tap:645:1398 second-windows-test io.github.kisame76.minato.test.secondwindows \
  android.app.Application build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" build/second-windows-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/second-windows-test/second-windows-test.apk fixtures/expected/second-windows-test.logcat 1290x2796@3,en-US
