#!/usr/bin/env bash
# Runs ADR-0043 step c4's test (surface-test, ADR-0041 test 1 without touches) on the Mac
# (framework/tests/java_app_run.py): builds graphics-run (apk-run with graphics/host's providers) and apk-convert for
# macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about
# 10 s) and surface-test (seconds) every time, so it never tests an old binary; then starts surface-test through
# app_process in a session on the M4 plan's display, the iPhone 15 Pro Max's 1290 x 2796 px at scale 3 in en-US, and
# compares its log with Android 15's on that display (fixtures/expected/surface-test.logcat, from
# tools/android-reference.sh --app surface-test): a full-screen GLSurfaceView with ES 3.0 through the front libs on the
# Mac's ANGLE (MINATO_ANGLE), its holder's callbacks with their format and size, Choreographer's frames, its bounds,
# and the renderer's callbacks with three fixed frames read back with glReadPixels.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-framework-res.sh,
# tools/build-front-libs.sh and tools/build-angle.sh first.
# Usage: tools/surface-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."
# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi
ANGLE="$PWD/build/angle/angle/out/macos/libGLESv2.dylib"
if [[ ! -f "$ANGLE" ]]; then echo "skipped: $ANGLE is not built (tools/build-angle.sh)"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target graphics-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh surface-test >/dev/null

MINATO_ANGLE="$ANGLE" python3 framework/tests/java_app_run.py surface-test io.github.kisame76.minato.test.surface \
  android.app.Application build/art build/bionic/lib build/bionic/system build/hwui/lib \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" build/surface-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/surface-test/surface-test.apk \
  fixtures/expected/surface-test.logcat 1290x2796@3,en-US
