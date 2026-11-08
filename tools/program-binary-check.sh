#!/usr/bin/env bash
# Runs program-binary-test on the Mac (framework/tests/java_app_run.py --restart): a game engine's shader cache, its
# GL programs' binaries saved by the first start and loaded by the second, a new process over the first one's
# folders, then binaries a driver must refuse without a crash (a format not listed, one cut short, one with its middle
# overwritten, an empty one, one with bytes after its end). Builds graphics-run and apk-convert for macOS into
# build/native/macos-grafik, and app_process with libandroid_runtime.so (incremental), framework.jar and the app every
# time, so it never tests an old binary; then starts the NativeActivity twice in a session on the M4 plan's display,
# the iPhone 15 Pro Max's 1290 x 2796 px at scale 3 in en-US, with ANGLE, and compares the two starts' lines with
# fixtures/expected/program-binary-test.logcat, the Android 15 emulator's (tools/android-reference.sh --app
# program-binary-test). MINATO_ANGLE_BACKEND=vulkan runs it on ANGLE's Vulkan backend, the ES 3.1 path, with
# MINATO_MOLTENVK (default build/moltenvk/macos/libMoltenVK.dylib).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-framework-res.sh and
# tools/build-angle.sh first.
# Usage: tools/program-binary-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi
ANGLE="$PWD/build/angle/angle/out/macos/libGLESv2.dylib"
if [[ ! -f "$ANGLE" ]]; then echo "skipped: $ANGLE is not built (tools/build-angle.sh)"; exit 0; fi
if [[ "${MINATO_ANGLE_BACKEND:-}" == vulkan ]]; then
  export MINATO_MOLTENVK="${MINATO_MOLTENVK:-$PWD/build/moltenvk/macos/libMoltenVK.dylib}"
fi

NATIVE=build/native/macos-grafik
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target graphics-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh program-binary-test >/dev/null

MINATO_ANGLE="$ANGLE" MINATO_SESSION_LAYERS=1 python3 framework/tests/java_app_run.py --restart \
  program-binary-test io.github.kisame76.minato.test.programbinary \
  io.github.kisame76.minato.test.programbinary.ProgramBinaryApplication \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/graphics/tests/graphics-run" build/program-binary-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/program-binary-test/program-binary-test.apk fixtures/expected/program-binary-test.logcat 1290x2796@3,en-US
