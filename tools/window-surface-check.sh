#!/usr/bin/env bash
# Runs ADR-0043 c2's test of Surface's natives on the Mac (framework/tests/java_app_run.py): builds graphics-run
# (apk-run with graphics/host's providers) and apk-convert for macOS into build/native/macos-checks, and
# app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and window-surface-test (seconds)
# every time, so it never tests an old binary; then runs window-surface-test on graphics-run's window 1 of 64 x 48
# pixels (MINATO_WINDOW) with the Mac's ANGLE (MINATO_ANGLE), the Metal hook reading back the first 4 drawables
# presented (MINATO_METAL_HOOK=present:4) and the window host each buffer drawn by the CPU that the window shows as
# its sublayer's contents (MINATO_WINDOW_WATCH=contents), and compares the app's lines and the hosts' with
# fixtures/expected/window-surface-test.txt: a Surface from WindowSurface, EGL14's window surface on it, lockCanvas and
# unlockCanvasAndPost with and without a dirty rectangle, setBuffersGeometry, and ANativeWindow_fromSurface and
# ANativeWindow_toSurface from the app's JNI library.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-framework-res.sh,
# tools/build-front-libs.sh and tools/build-angle.sh first.
# Usage: tools/window-surface-check.sh
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
tools/build-java-test-app.sh window-surface-test >/dev/null
MINATO_ANGLE="$ANGLE" MINATO_WINDOW=64x48 MINATO_METAL_HOOK=present:4 MINATO_WINDOW_WATCH=contents \
  python3 framework/tests/java_app_run.py \
  window-surface-test io.github.kisame76.minato.test.windowsurface \
  io.github.kisame76.minato.test.windowsurface.WindowSurfaceApplication build/art build/bionic/lib \
  build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/graphics/tests/graphics-run" build/window-surface-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/window-surface-test/window-surface-test.apk fixtures/expected/window-surface-test.txt
