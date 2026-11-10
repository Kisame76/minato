#!/usr/bin/env bash
# Runs ADR-0073 step e's test of the legacy android.hardware.Camera on the Mac (framework/tests/java_app_run.py) on
# graphics/host's model cameras, with the camera grant held (MINATO_GRANT_CAMERA): builds apk-run, apk-convert and
# graphics-run for macOS into build/native/macos-checks, and app_process with libandroid_runtime.so (incremental),
# framework.jar and the app every time, so it never tests an old binary; then camera-legacy-test against Android 15's
# log (fixtures/expected/camera-legacy-test.logcat, from tools/android-reference.sh --app camera-legacy-test): the
# cameras, the first one's parameters and the answers around them, which every Android 15 camera gives alike.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/camera-legacy-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-checks
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert graphics-run >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh camera-legacy-test >/dev/null
app=camera-legacy-test
MINATO_GRANT_CAMERA=granted python3 framework/tests/java_app_run.py "$app" io.github.kisame76.minato.test.cameralegacy \
  io.github.kisame76.minato.test.cameralegacy.LegacyApplication build/art build/bionic/lib \
  build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/graphics/tests/graphics-run" "build/$app-check" \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  "build/$app/$app.apk" fixtures/expected/$app.logcat
