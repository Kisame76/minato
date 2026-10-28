#!/usr/bin/env bash
# Runs ADR-0060 e2d part 2a's test of the NDK's AMediaExtractor on the Mac (framework/tests/java_app_run.py): builds
# apk-run, apk-convert and graphics-run for macOS into build/native/macos-grafik, the guest's libmediandk.so with
# Minato's mp4 parser (tools/build-bionic.sh, incremental), app_process with libandroid_runtime.so, framework.jar and
# the app every time, so it never tests an old binary; then media-extractor-test against Android 15's log
# (fixtures/expected/media-extractor-test.logcat, from tools/android-reference.sh --app media-extractor-test): both
# test mp4 files from an fd with an offset, a path and an AMediaDataSource, their tracks' formats, every sample, the
# seeks in each mode, and the refusals.
# Needs tools/build-art.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/media-extractor-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-grafik
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert graphics-run >/dev/null
tools/build-bionic.sh >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh media-extractor-test >/dev/null
app=media-extractor-test
python3 framework/tests/java_app_run.py "$app" io.github.kisame76.minato.test.mediaextractor \
  io.github.kisame76.minato.test.mediaextractor.MediaExtractorApplication build/art build/bionic/lib \
  build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/graphics/tests/graphics-run" "build/$app-check" \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  "build/$app/$app.apk" fixtures/expected/media-extractor-test.logcat
