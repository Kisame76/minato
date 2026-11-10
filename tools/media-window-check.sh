#!/usr/bin/env bash
# Runs ADR-0060 e2c3's test of a decoder rendering into a window on the Mac (framework/tests/java_app_run.py): builds
# apk-run, apk-convert and graphics-run for macOS into build/native/macos-checks, and app_process with
# libandroid_runtime.so (incremental), framework.jar and the app every time; then media-window-test against Android
# 15's log (fixtures/expected/media-window-test.logcat, from tools/android-reference.sh --app media-window-test):
# MediaCodec and AMediaCodec rendering into a SurfaceView's window, their frame rendered callbacks, setOutputSurface
# to a reader and back, the other producers a window the codec draws into refuses, and a window that goes away. The
# SurfaceView's window is graphics-run's layer of its own (MINATO_SESSION_LAYERS=1); the decoder runs on graphics/host's
# VideoToolbox, EGL on ANGLE and the swapchain on MoltenVK.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-framework-res.sh,
# tools/build-angle.sh and tools/fetch-moltenvk.sh first.
# Usage: tools/media-window-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi
ANGLE="$PWD/build/angle/angle/out/macos/libGLESv2.dylib"
if [[ ! -f "$ANGLE" ]]; then echo "skipped: $ANGLE is not built (tools/build-angle.sh)"; exit 0; fi
MOLTENVK="$PWD/build/moltenvk/macos/libMoltenVK.dylib"
if [[ ! -f "$MOLTENVK" ]]; then echo "skipped: $MOLTENVK is not there (tools/fetch-moltenvk.sh)"; exit 0; fi

NATIVE=build/native/macos-checks
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert graphics-run >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh media-window-test >/dev/null
app=media-window-test
MINATO_ANGLE="$ANGLE" MINATO_MOLTENVK="$MOLTENVK" MINATO_SESSION_LAYERS=1 python3 framework/tests/java_app_run.py \
  "$app" io.github.kisame76.minato.test.mediawindow android.app.Application build/art build/bionic/lib \
  build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/graphics/tests/graphics-run" "build/$app-check" \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  "build/$app/$app.apk" fixtures/expected/media-window-test.logcat 1290x2796@3,en-US
