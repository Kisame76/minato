#!/usr/bin/env bash
# Runs ADR-0058 e1)'s test of ImageWriter (the graphics breadth's G3b) on the Mac (framework/tests/java_app_run.py):
# builds graphics-run (apk-run with graphics/host's providers) and apk-convert for macOS into
# build/native/macos-grafik, and app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and
# image-writer-test (seconds) every time, so it never tests an old binary; then compares image-writer-test's log with
# Android 15's (fixtures/expected/image-writer-test.logcat, from tools/android-reference.sh --app image-writer-test):
# writers on ImageReaders and a SurfaceTexture, on the front libs (tools/build-front-libs.sh) and the Mac's ANGLE
# (MINATO_ANGLE).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-framework-res.sh,
# tools/build-front-libs.sh and tools/build-angle.sh first.
# Usage: tools/image-writer-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi
ANGLE="$PWD/build/angle/angle/out/macos/libGLESv2.dylib"
if [[ ! -f "$ANGLE" ]]; then echo "skipped: $ANGLE is not built (tools/build-angle.sh)"; exit 0; fi
NATIVE=build/native/macos-grafik
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target graphics-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh image-writer-test >/dev/null
MINATO_ANGLE="$ANGLE" python3 framework/tests/java_app_run.py image-writer-test \
  io.github.kisame76.minato.test.imagewriter \
  io.github.kisame76.minato.test.imagewriter.ImageWriterApplication \
  build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" build/image-writer-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/image-writer-test/image-writer-test.apk \
  fixtures/expected/image-writer-test.logcat
