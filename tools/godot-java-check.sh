#!/usr/bin/env bash
# Runs M4's test 2 on the Mac (ADR-0042 b), framework/tests/godot_java_run.py): both Godot APKs of tools/build-godot.sh
# through the Java path, each started as the launcher starts it, in a session of the M4 plan's display under
# graphics-run with the session's layers, so that the SurfaceView of Godot's render view has a layer of its own:
# godot-bench-vulkan draws through VK_KHR_android_surface on the Mac's MoltenVK (MINATO_MOLTENVK), godot-bench-gles
# through its GLSurfaceView's EGL on the Mac's ANGLE (MINATO_ANGLE). Each one's "godot-bench: " lines must be Android
# 15's (fixtures/expected/godot-bench-*.logcat, from tools/android-reference.sh --app), its run must have frames and
# no failures, and it must end its process itself afterwards, as on Android. It builds graphics-run and apk-convert
# for macOS into build/native/macos-grafik, and app_process with libandroid_runtime.so (incremental) and
# framework.jar (about 10 s) every time, so it never tests an old binary.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-framework-res.sh,
# tools/build-front-libs.sh, tools/build-angle.sh, tools/fetch-moltenvk.sh and tools/build-godot.sh first.
# Usage: tools/godot-java-check.sh
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
NATIVE=build/native/macos-grafik
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target graphics-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
failed=0
for flavor in vulkan gles; do
  MINATO_ANGLE="$ANGLE" MINATO_MOLTENVK="$MOLTENVK" MINATO_SESSION_LAYERS=1 python3 framework/tests/godot_java_run.py \
    "godot-bench-$flavor" "io.github.kisame76.minato.godotbench.$flavor" build/art build/bionic/lib \
    build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
    "$NATIVE/importer/converter/apk-convert" "$NATIVE/graphics/tests/graphics-run" "build/godot-java-check/$flavor" \
    build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
    "build/godot/godot-bench-$flavor.apk" "fixtures/expected/godot-bench-$flavor.logcat" || failed=1
done
exit "$failed"
