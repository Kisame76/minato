#!/usr/bin/env bash
# Runs the test of the windows at the background and the return, windows-return-test, on the Mac
# (framework/tests/java_app_run.py; ADR-0041 gap 4 (b5a)): builds graphics-run (apk-run with graphics/host's providers)
# and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental),
# framework.jar (about 10 s) and windows-return-test (seconds) every time, so it never tests an old binary; then starts
# windows-return-test through app_process in a session on the M4 plan's display, whose host plays the start, three taps
# in a corner that no window answers (a second each, so that the windows are up), the background and the return, and
# compares its log with Android 15's on apk-runner-35-iphone-gesture (fixtures/expected/windows-return-test.logcat,
# from tools/android-reference.sh --app windows-return-test, HOME after "ready"): a dialog with a popup on its button
# and a translucent activity with a SurfaceView above them, their lifecycle, focus and SurfaceView callbacks around
# HOME and the return. The windows have graphics-run's layers in decision 1's order (MINATO_SESSION_LAYERS=1,
# MINATO_WINDOW_WATCH=hidden, gap 4 (c)), whose lines it checks among the app's in order: each requested window gone
# at the background and made again at the return (decision 6).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/windows-return-check.sh
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
tools/build-java-test-app.sh windows-return-test >/dev/null

MINATO_SESSION_LAYERS=1 MINATO_WINDOW_WATCH=hidden python3 framework/tests/java_app_run.py \
  --lifecycle start,tap:1:1,tap:1:1,tap:1:1,background,foreground \
  --in-order "I/windows-return-test: ready" --in-order "window host: window 4 goes" \
  --in-order "window host: window 5, type 2: at 32, 1105, 1225 x 585, alpha 1.00, dim 0.60, not opaque" \
  --in-order "window host: window 6, type 1000: at 981, 1630, 146 x 81" \
  --in-order "window host: window 7, type 1: at 0, 0, 1290 x 2796" \
  --in-order "window host: window 8, type 1001: at 0, 0, 300 x 300" \
  --in-order "window host: from the bottom: the window, dim, window 5, window 6, window 8, window 7" \
  --in-order "I/windows-return-test: after the return" \
  windows-return-test io.github.kisame76.minato.test.windowsreturn \
  android.app.Application build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" build/windows-return-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/windows-return-test/windows-return-test.apk fixtures/expected/windows-return-test.logcat \
  1290x2796@3,en-US
