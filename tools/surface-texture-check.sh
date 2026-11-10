#!/usr/bin/env bash
# Runs ADR-0058 c)'s test of SurfaceTexture on the Mac (framework/tests/java_app_run.py): builds graphics-run (apk-run
# with graphics/host's providers) and apk-convert for macOS into build/native/macos-checks, and app_process with
# libandroid_runtime.so (incremental), framework.jar (about 10 s) and surface-texture-test (seconds) every time, so it
# never tests an old binary; then compares surface-texture-test's log with Android 15's
# (fixtures/expected/surface-texture-test.logcat, from tools/android-reference.sh --app surface-texture-test): a GL
# thread's external texture fed by a Canvas producer through Surface(SurfaceTexture), onFrameAvailable on the creator's
# Looper, updateTexImage, the transform, the timestamp and the pixels drawn through samplerExternalOES, on the front
# libs (tools/build-front-libs.sh) and the Mac's ANGLE (MINATO_ANGLE).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-framework-res.sh,
# tools/build-front-libs.sh and tools/build-angle.sh first.
# Usage: tools/surface-texture-check.sh
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
tools/build-java-test-app.sh surface-texture-test >/dev/null
MINATO_ANGLE="$ANGLE" python3 framework/tests/java_app_run.py surface-texture-test \
  io.github.kisame76.minato.test.surfacetexture \
  io.github.kisame76.minato.test.surfacetexture.SurfaceTextureApplication \
  build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" build/surface-texture-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/surface-texture-test/surface-texture-test.apk \
  fixtures/expected/surface-texture-test.logcat
