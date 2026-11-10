#!/usr/bin/env bash
# Runs ADR-0073 step a1's test of camera2's metadata on the Mac (framework/tests/java_app_run.py):
# CameraMetadataNative's natives over libcamera_metadata, through the public CameraCharacteristics keys and AOSP's
# marshalers (set, grow, copy, update, move, remove, tags, a Parcel's round trip, the dump). Builds apk-run,
# apk-convert and graphics-run for macOS into build/native/macos-checks, and app_process with libandroid_runtime.so
# (incremental), framework.jar and the app every time, so it never tests an old binary; then compares the app's lines
# with fixtures/expected/camera-metadata-test.logcat. The class is hidden API, which Android 15 refuses an app, so
# there is no emulator reference: each expected line is what AOSP's Java and libcamera_metadata make of the values
# the test sets. Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and
# tools/build-framework-res.sh first.
# Usage: tools/camera-metadata-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-checks
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert graphics-run >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh camera-metadata-test >/dev/null
app=camera-metadata-test
python3 framework/tests/java_app_run.py "$app" io.github.kisame76.minato.test.camerametadata \
  io.github.kisame76.minato.test.camerametadata.MetadataApplication build/art build/bionic/lib \
  build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/graphics/tests/graphics-run" "build/$app-check" \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  "build/$app/$app.apk" fixtures/expected/$app.logcat
