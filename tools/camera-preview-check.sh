#!/usr/bin/env bash
# Runs ADR-0073 step e's test of the legacy android.hardware.Camera's preview on the Mac
# (framework/tests/java_app_run.py) on graphics/host's model cameras, with the camera grant held (MINATO_GRANT_CAMERA):
# builds apk-run, apk-convert and graphics-run for macOS into build/native/macos-grafik, and app_process with
# libandroid_runtime.so (incremental), framework.jar and the app every time, so it never tests an old binary; then
# camera-preview-test against Android 15's log (fixtures/expected/camera-preview-test.logcat, from
# tools/android-reference.sh --app camera-preview-test): the preview's frames into a SurfaceTexture and its
# callbacks, which every Android 15 camera gives alike.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/camera-preview-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi
# The test's GL thread takes the frames into its SurfaceTexture: the Mac's ANGLE (MINATO_ANGLE).
ANGLE="$PWD/build/angle/angle/out/macos/libGLESv2.dylib"
if [[ ! -f "$ANGLE" ]]; then echo "skipped: $ANGLE is not built (tools/build-angle.sh)"; exit 0; fi

NATIVE=build/native/macos-grafik
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert graphics-run >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh camera-preview-test >/dev/null
app=camera-preview-test
MINATO_GRANT_CAMERA=granted MINATO_ANGLE="$ANGLE" python3 framework/tests/java_app_run.py "$app" \
  io.github.kisame76.minato.test.camerapreview \
  io.github.kisame76.minato.test.camerapreview.PreviewApplication build/art build/bionic/lib \
  build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/graphics/tests/graphics-run" "build/$app-check" \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  "build/$app/$app.apk" fixtures/expected/$app.logcat
