#!/usr/bin/env bash
# Runs ADR-0041 a)'s test on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS into
# build/native/macos-checks, and app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and
# vsync-test (seconds) every time, so it never tests an old binary; then compares vsync-test's log with Android 15's
# (fixtures/expected/vsync-test.logcat, from tools/android-reference.sh --app vsync-test): Java's Choreographer, whose
# DisplayEventReceiver natives sit on libandroid's AChoreographer, runs frame callbacks, delayed and removed ones,
# FrameData's timelines, a frame after the main thread was blocked, and another Looper's instance. The app runs in a
# session of the M4 plan's display, whose frame clock apk-run runs at 120 Hz.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/vsync-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."
# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi
NATIVE=build/native/macos-checks
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh vsync-test >/dev/null
python3 framework/tests/java_app_run.py vsync-test io.github.kisame76.minato.test.vsync \
  io.github.kisame76.minato.test.vsync.VsyncApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/vsync-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/vsync-test/vsync-test.apk fixtures/expected/vsync-test.logcat \
  1290x2796@3,en-US
