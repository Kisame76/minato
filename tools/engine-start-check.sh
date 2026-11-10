#!/usr/bin/env bash
# Runs engine-start-test on the Mac (framework/tests/java_app_run.py): what a game engine's player asks of EGL, GLES
# and the Java display at its start, past the lookups that gles-procs-test checks (contexts asked from ES 3.2 down,
# a context shared on a second thread, frame timestamps, a frame pacer's fence wait, display
# timing and a class of the APK through the activity's class loader). Builds graphics-run and apk-convert for macOS
# into build/native/macos-checks, and app_process with libandroid_runtime.so (incremental), framework.jar and the app
# every time, so it never tests an old binary; then starts the NativeActivity through app_process in a session on the
# M4 plan's display, the iPhone 15 Pro Max's 1290 x 2796 px at scale 3 in en-US, with ANGLE, and compares its lines with
# fixtures/expected/engine-start-test.logcat, the Android 15 emulator's (tools/android-reference.sh --app
# engine-start-test), whose lines hold on every Android device (the values go to the -device tag).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-framework-res.sh and
# tools/build-angle.sh first.
# Usage: tools/engine-start-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi
ANGLE="$PWD/build/angle/angle/out/macos/libGLESv2.dylib"
if [[ ! -f "$ANGLE" ]]; then echo "skipped: $ANGLE is not built (tools/build-angle.sh)"; exit 0; fi

NATIVE=build/native/macos-checks
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target graphics-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh engine-start-test >/dev/null

MINATO_ANGLE="$ANGLE" MINATO_SESSION_LAYERS=1 python3 framework/tests/java_app_run.py \
  engine-start-test io.github.kisame76.minato.test.enginestart android.app.Application \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/graphics/tests/graphics-run" build/engine-start-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/engine-start-test/engine-start-test.apk fixtures/expected/engine-start-test.logcat 1290x2796@3,en-US
