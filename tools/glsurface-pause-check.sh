#!/usr/bin/env bash
# Runs glsurface-pause-test on the Mac (framework/tests/java_app_run.py): a GLSurfaceView with ES 2.0 that draws
# continuously, as libGDX's does, through the background and the return. Builds graphics-run (apk-run with
# graphics/host's providers) and apk-convert for macOS into build/native/macos-checks, and app_process with
# libandroid_runtime.so (incremental), framework.jar and the app every time, so it never tests an old binary; then
# starts it on the Mac's ANGLE in a session on the M4 plan's display, whose host plays the start, the background and the
# return, and compares its log with Android 15's (fixtures/expected/glsurface-pause-test.logcat, from
# tools/android-reference.sh --app glsurface-pause-test, HOME a second after "ready"): the activity's, the holder's and
# the renderer's callbacks, GLSurfaceView.onPause returning while its GL thread lets the surface go, and frames after
# the return.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-framework-res.sh,
# tools/build-front-libs.sh and tools/build-angle.sh first.
# Usage: tools/glsurface-pause-check.sh
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
tools/build-java-test-app.sh glsurface-pause-test >/dev/null

MINATO_ANGLE="$ANGLE" MINATO_SESSION_LAYERS=1 MINATO_WINDOW_WATCH=hidden python3 framework/tests/java_app_run.py \
  --lifecycle start,background,foreground \
  glsurface-pause-test io.github.kisame76.minato.test.glsurfacepause \
  android.app.Application build/art build/bionic/lib build/bionic/system build/hwui/lib \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" build/glsurface-pause-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/glsurface-pause-test/glsurface-pause-test.apk fixtures/expected/glsurface-pause-test.logcat 1290x2796@3,en-US
