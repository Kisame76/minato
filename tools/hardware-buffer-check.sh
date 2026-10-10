#!/usr/bin/env bash
# Runs ADR-0058 a)'s test of android.hardware.HardwareBuffer on the Mac (framework/tests/java_app_run.py): builds
# apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar and the app every time, so it never tests an old binary; then hardware-buffer-test
# against Android 15's log (fixtures/expected/hardware-buffer-test.logcat, from tools/android-reference.sh --app
# hardware-buffer-test): HardwareBuffer's isSupported, create, getters, refusals, a Parcel round trip and close, and
# libandroid's AHardwareBuffer_fromHardwareBuffer and toHardwareBuffer through the app's JNI library. Without a host
# of graphics/host's, apk-run's runtime has no hardware buffers: the check runs with graphics-run's, from
# build/native/macos (cmake --build build/native/macos --target graphics-run).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/hardware-buffer-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert graphics-run >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh hardware-buffer-test >/dev/null
app=hardware-buffer-test
python3 framework/tests/java_app_run.py "$app" io.github.kisame76.minato.test.hardwarebuffer \
  io.github.kisame76.minato.test.hardwarebuffer.HardwareBufferApplication build/art build/bionic/lib \
  build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/graphics/tests/graphics-run" "build/$app-check" \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  "build/$app/$app.apk" fixtures/expected/hardware-buffer-test.logcat
