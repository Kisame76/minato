#!/usr/bin/env bash
# Runs surfaceview-producers-test on the Mac (framework/tests/java_app_run.py) with ANGLE and a session: a
# SurfaceView's EGL window surfaces across a software frame, a destroy while current, setFormat, setFixedSize and a
# hide and show, against Android 15's log (fixtures/expected/surfaceview-producers-test.logcat, from
# tools/android-reference.sh --app surfaceview-producers-test): a new surface of the view (a format change, a hide and
# show) takes a producer again after the software frame of the one before, a new size of the same surface does not.
# Builds graphics-run and apk-convert for macOS into build/native/macos-grafik, and libandroid_runtime.so, framework.jar
# and the app every time. Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh,
# tools/build-framework-res.sh and tools/build-angle.sh first.
# Usage: tools/surfaceview-producers-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi
ANGLE="$PWD/build/angle/angle/out/macos/libGLESv2.dylib"
if [[ ! -f "$ANGLE" ]]; then echo "skipped: $ANGLE is not built (tools/build-angle.sh)"; exit 0; fi

NATIVE=build/native/macos-grafik
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target graphics-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh surfaceview-producers-test >/dev/null

MINATO_ANGLE="$ANGLE" MINATO_SESSION_LAYERS=1 python3 framework/tests/java_app_run.py \
  surfaceview-producers-test io.github.kisame76.minato.test.surfaceviewproducers \
  android.app.Application build/art build/bionic/lib build/bionic/system build/hwui/lib \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" build/surfaceview-producers-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/surfaceview-producers-test/surfaceview-producers-test.apk \
  fixtures/expected/surfaceview-producers-test.logcat 1290x2796@3,en-US
