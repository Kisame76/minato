#!/usr/bin/env bash
# Runs the mouse test (mouse-test; ADR-0068 decision 5, the mouse step) on the Mac (framework/tests/java_app_run.py):
# builds apk-run and apk-convert for macOS into build/native/macos-app, app_process with libandroid_runtime.so
# (incremental), framework.jar and the app every time, then starts the app through app_process in a session on the
# M4 plan's display,
# the iPhone 15 Pro Max's 1290 x 2796 px at scale 3 in en-US. The host plays the mouse that apk-run's mouse: steps
# describe: the device record of a mouse, then its moves, buttons and wheels as apkrt_input_post_mouse's reports, as
# the App's GCMouse and pointer side posts them; the mouse then goes. The log is compared with
# fixtures/expected/mouse-test.logcat, Android 15's with a virtual mouse of `uinput` (tools/android-reference.sh).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/mouse-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh mouse-test >/dev/null

# The reference's uinput.json, report by report: two moves, a click with a drag, a right and a middle click, a chord of
# two buttons, the back and forward buttons, and the wheels.
steps="start,mouse:add,mouse:move:630:1220:30:20,mouse:move:640:1220:10:0"
steps="$steps,mouse:press:1,mouse:move:650:1230:10:10,mouse:release:0"
steps="$steps,mouse:press:2,mouse:release:0,mouse:press:4,mouse:release:0"
steps="$steps,mouse:press:1,mouse:press:3,mouse:release:1,mouse:release:0"
steps="$steps,mouse:press:8,mouse:release:0,mouse:press:16,mouse:release:0"
steps="$steps,mouse:scroll:1:0,mouse:scroll:-2:0,mouse:scroll:0:1,mouse:scroll:0:-1,mouse:scroll:1:1"
steps="$steps,mouse:remove"
python3 framework/tests/java_app_run.py \
  --lifecycle "$steps" \
  mouse-test io.github.kisame76.minato.test.mouse android.app.Application \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/mouse-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/mouse-test/mouse-test.apk fixtures/expected/mouse-test.logcat 1290x2796@3,en-US

# mouse-native-test, a NativeActivity, on the Java path (ADR-0069 step 3): the same mouse's motion events and keys reach
# its input queue as Android 15's do (fixtures/expected/mouse-native-test.logcat). Its window must be drawn, as
# native-activity-check's. The app ends at its 46th motion event, the hover exit the mouse's removal makes.
tools/build-fixtures.sh >/dev/null
python3 framework/tests/java_app_run.py --drawn --lifecycle "$steps" \
  mouse-native-test io.github.kisame76.minato.test.mousenative android.app.Application \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/mouse-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/fixtures/apks/mouse-native-test.apk fixtures/expected/mouse-native-test.logcat 1290x2796@3,en-US
