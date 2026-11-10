#!/usr/bin/env bash
# Runs ADR-0041 a)'s test of GLES from Java on the Mac (framework/tests/java_app_run.py): builds graphics-run (apk-run
# with graphics/host's providers) and apk-convert for macOS into build/native/macos-checks, and app_process with
# libandroid_runtime.so (incremental), framework.jar (about 10 s) and gles-java-test (seconds) every time, so it never
# tests an old binary; then compares gles-java-test's log with Android 15's (fixtures/expected/gles-java-test.logcat,
# from tools/android-reference.sh --app gles-java-test): EGL10 as Godot's GLSurfaceView uses it, EGL14 with GLES20 and
# GLES30, and GL10 on an ES 1.1 context, on pbuffers, through the front libs (tools/build-front-libs.sh) on the Mac's
# ANGLE (MINATO_ANGLE); and the app's JNI library, which links the front libs and opens libvulkan by name, as Godot's
# does, with the Mac's MoltenVK behind it (MINATO_MOLTENVK), as the app has both.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-framework-res.sh,
# tools/build-front-libs.sh, tools/build-angle.sh and tools/fetch-moltenvk.sh first.
# Usage: tools/gles-java-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."
# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi
ANGLE="$PWD/build/angle/angle/out/macos/libGLESv2.dylib"
if [[ ! -f "$ANGLE" ]]; then echo "skipped: $ANGLE is not built (tools/build-angle.sh)"; exit 0; fi
MOLTENVK="$PWD/build/moltenvk/macos/libMoltenVK.dylib"
if [[ ! -f "$MOLTENVK" ]]; then echo "skipped: $MOLTENVK is not fetched (tools/fetch-moltenvk.sh)"; exit 0; fi
NATIVE=build/native/macos-checks
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target graphics-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh gles-java-test >/dev/null
MINATO_ANGLE="$ANGLE" MINATO_MOLTENVK="$MOLTENVK" python3 framework/tests/java_app_run.py gles-java-test \
  io.github.kisame76.minato.test.glesjava io.github.kisame76.minato.test.glesjava.GlesJavaApplication build/art \
  build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" build/gles-java-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/gles-java-test/gles-java-test.apk \
  fixtures/expected/gles-java-test.logcat
