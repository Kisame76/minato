#!/usr/bin/env bash
# Runs the test of second windows on the Mac (framework/tests/java_app_run.py; ADR-0041 gap 4 (b)): builds graphics-run
# (apk-run with graphics/host's providers) and apk-convert for macOS into build/native/macos-app, and app_process with
# libandroid_runtime.so (incremental), framework.jar (about 10 s) and second-windows-test (seconds) every time, so it
# never tests an old binary; then starts second-windows-test through app_process, whose host answers each window
# request and taps the touches part's windows a second apart, where their ready lines in Android 15's log name the
# places, and compares its log with Android 15's on
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
# The display: MINATO_DISPLAY, by default the M4 plan's (tools/check-display.sh).
source tools/check-display.sh

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

EXPECTED=$(expected_at_display second-windows-test logcat)
# The floating window sits between the system bars (check-display.sh's MINATO_BAR_INSETS), as do the dialogs and popups
# of the taps, whose places Android 15's log gives. After the last tap the step barinsets, APKRT_EVENT_BAR_INSETS as the
# host posts it at a turn, makes the status bar 48 px higher than the environment says: the floating activity that
# follows sits 24 px lower, so the event wins over the environment.
IFS=, read -r BAR_TOP BAR_BOTTOM <<<"$MINATO_BAR_INSETS"
TURN_TOP=$((BAR_TOP + 48))
taps=$(sed -n 's/^I\/second-windows-test: ready: .*, tap at \([0-9]*\),\([0-9]*\)$/tap:\1:\2/p' "$EXPECTED" |
  paste -sd , -)
if [[ $(tr -cd , <<<"$taps" | wc -c) -ne 4 ]]; then
  echo "FAIL: $EXPECTED names no five tap places: run MINATO_DISPLAY=$CHECK_DISPLAY tools/android-reference.sh" \
    "--app second-windows-test"
  exit 1
fi

MINATO_SESSION_LAYERS=1 MINATO_WINDOW_WATCH=hidden python3 framework/tests/java_app_run.py \
  --in-order "I/second-windows-test: activity onPause" \
  --in-order "window host: window 10, type 1: at 0, 0, $CHECK_WIDTH x $CHECK_HEIGHT, alpha 1.00, dim 0.00, not opaque" \
  --in-order "window host: window 11, type 1001: at 0, 0, 300 x 300" \
  --in-order "window host: from the bottom: the window, window 11, window 10, dim, window 12" \
  --in-order "window host: from the bottom: the window, dim, window 13, window 11, window 10" \
  --in-order "I/second-windows-test: setTranslucent(false): true" --in-order "window host: window 13 goes" \
  --in-order "I/second-windows-test: convertToTranslucent: true" \
  --in-order "window host: from the bottom: the window, dim, window 14, window 11, window 10" \
  --in-order "window host: window 10 goes" \
  --in-order "window host: window 15, type 1: at $(((CHECK_WIDTH - 295) / 2)),\
 $(((TURN_TOP + CHECK_HEIGHT - BAR_BOTTOM - 201) / 2)), 295 x 201, alpha 1.00, dim 0.60, not opaque" \
  --in-order "window host: from the bottom: the window, dim, window 15" \
  --env "MINATO_BAR_INSETS=$MINATO_BAR_INSETS" \
  --lifecycle "start,$taps,barinsets:$TURN_TOP:$BAR_BOTTOM" \
  second-windows-test io.github.kisame76.minato.test.secondwindows \
  android.app.Application build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" build/second-windows-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/second-windows-test/second-windows-test.apk "$EXPECTED" \
  "$CHECK_DISPLAY,en-US"
