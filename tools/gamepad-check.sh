#!/usr/bin/env bash
# Runs the controllers' test (gamepad-test; ADR-0068 decision 5) on the Mac (framework/tests/java_app_run.py): builds
# apk-run and apk-convert for macOS into build/native/macos-app, app_process with libandroid_runtime.so (incremental),
# framework.jar and the app every time, then starts the app through app_process in a session on the M4 plan's display,
# the iPhone 15 Pro Max's 1290 x 2796 px at scale 3 in en-US. The host plays the controller that apk-run's pad: steps
# describe: the device record of an Xbox Wireless Controller (045e:02e0), each button's down and up as key records of
# source 0x501, and the sticks, triggers and hat as axes records of source 0x01000010, as the App's GCController side
# posts them; the pad then goes. The log is compared with fixtures/expected/gamepad-test.logcat, Android 15's with a
# virtual controller of `uinput` (tools/android-reference.sh).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/gamepad-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh gamepad-test >/dev/null

steps="start,pad:add"
for code in 96 97 99 100 102 103 108 109 110 106 107; do
  steps="$steps,pad:key:$code:1,pad:key:$code:0"
done
for axes in 0=0.5 1=-1.0 11=0.25 14=-0.5 17=0.5 18=1.0 15=-1.0 16=1.0 "0=1.0;1=-0.5"; do
  steps="$steps,pad:axes:2:$axes,pad:axes:2:"
done
steps="$steps,pad:remove"
python3 framework/tests/java_app_run.py \
  --lifecycle "$steps" \
  gamepad-test io.github.kisame76.minato.test.gamepad android.app.Application \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/gamepad-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/gamepad-test/gamepad-test.apk fixtures/expected/gamepad-test.logcat 1290x2796@3,en-US
