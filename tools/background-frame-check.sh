#!/usr/bin/env bash
# Runs background-frame-test on the Mac (framework/tests/java_app_run.py): a Choreographer.postFrameCallback loop, and
# a layout requested at onPause, so ViewRootImpl schedules a traversal and its sync barrier as the app goes to the
# background. Builds graphics-run and apk-convert for macOS into build/native/macos-app, and app_process
# (incremental), framework.jar and the app every time, so it never tests an old binary; then starts it in a session on
# the M4 plan's display, whose host plays the start, the background, two seconds there and the return, and compares
# its log with Android 15's (fixtures/expected/background-frame-test.logcat, from tools/android-reference.sh --app
# background-frame-test): onStop within a second of onPause, the frame loop ticking in the first second after onStop,
# and frames again after the return. apk-run holds its frame clock in the background and answers the guest's vsync
# requests there (--background-vsync, which java_app_run.py passes), as the App does for a session on the Java path.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/background-frame-check.sh
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
tools/build-java-test-app.sh background-frame-test >/dev/null

MINATO_SESSION_LAYERS=1 MINATO_WINDOW_WATCH=hidden python3 framework/tests/java_app_run.py \
  --lifecycle start,background,pause:2000,foreground \
  --in-order "I/background-frame-test: ready" --in-order "I/background-frame-test: onPause" \
  --in-order "I/background-frame-test: onStop" --in-order "I/background-frame-test: done" \
  background-frame-test io.github.kisame76.minato.test.backgroundframe \
  android.app.Application build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" build/background-frame-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/background-frame-test/background-frame-test.apk fixtures/expected/background-frame-test.logcat \
  1290x2796@3,en-US
