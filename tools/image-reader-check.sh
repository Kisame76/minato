#!/usr/bin/env bash
# Runs ADR-0058 e1)'s test of ImageReader (the graphics breadth's G3a) on the Mac (framework/tests/java_app_run.py):
# builds graphics-run (apk-run with graphics/host's providers) and apk-convert for macOS into
# build/native/macos-checks, and app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and
# image-reader-test (seconds) every time, so it never tests an old binary; then compares image-reader-test's log with
# Android 15's (fixtures/expected/image-reader-test.logcat, from tools/android-reference.sh --app image-reader-test):
# readers of RGBA_8888, RGB_565 and PRIVATE fed by Canvas and GL producers through their surfaces, on the front libs
# (tools/build-front-libs.sh) and the Mac's ANGLE (MINATO_ANGLE).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-framework-res.sh,
# tools/build-front-libs.sh and tools/build-angle.sh first.
# Usage: tools/image-reader-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi
ANGLE="$PWD/build/angle/angle/out/macos/libGLESv2.dylib"
if [[ ! -f "$ANGLE" ]]; then echo "skipped: $ANGLE is not built (tools/build-angle.sh)"; exit 0; fi
NATIVE=build/native/macos-checks
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target graphics-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh image-reader-test >/dev/null
MINATO_ANGLE="$ANGLE" python3 framework/tests/java_app_run.py image-reader-test \
  io.github.kisame76.minato.test.imagereader \
  io.github.kisame76.minato.test.imagereader.ImageReaderApplication \
  build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" build/image-reader-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/image-reader-test/image-reader-test.apk \
  fixtures/expected/image-reader-test.logcat
