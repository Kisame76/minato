#!/usr/bin/env bash
# Runs hang-test on the Mac through graphics-run: its main thread sleeps 60 s after its first frame, and the app's ANR
# watchdog (com.android.internal.minato.AnrWatchdog) must log Android's "ANR in <package>" under ActivityManager and
# send the process SIGQUIT, whose dump by ART's signal catcher, every thread's stack with the main thread's at
# HangActivity, the palette logs whole, line by line, before the app wakes; the app's own lines are Android 15's
# (fixtures/expected/hang-test.logcat). The run ends 2 s after "awake".
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/anr-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-checks
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert graphics-run >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh hang-test >/dev/null

OUT=build/anr-check
rm -rf "$OUT/home"
mkdir -p "$OUT/home"
status=0
CFFIXED_USER_HOME="$PWD/$OUT/home" python3 framework/tests/java_app_run.py --until awake \
  --in-order "E/ActivityManager: ANR in io.github.kisame76.minato.test.hang" \
  --in-order "I/libartpalette-system: ----- pid" --in-order 'I/libartpalette-system: "main"' \
  --in-order "I/libartpalette-system:   at io.github.kisame76.minato.test.hang.HangActivity" \
  --in-order 'I/libartpalette-system: "Signal Catcher"' --in-order "I/libartpalette-system: ----- end" \
  hang-test io.github.kisame76.minato.test.hang \
  android.app.Application build/art build/bionic/lib build/bionic/system build/hwui/lib \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" "$OUT" build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/hang-test/hang-test.apk \
  fixtures/expected/hang-test.logcat 1290x2796@3,en-US || status=$?
rm -rf "$OUT/home"
exit $status
