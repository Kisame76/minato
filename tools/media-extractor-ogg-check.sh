#!/usr/bin/env bash
# Runs ADR-0060 e2's test of MediaExtractor on Ogg Opus on the Mac (framework/tests/java_app_run.py): builds apk-run,
# apk-convert and graphics-run for macOS into build/native/macos-checks, and app_process with libandroid_runtime.so
# (incremental), framework.jar and the app every time, so it never tests an old binary; then media-extractor-ogg-test
# against Android 15's log (fixtures/expected/media-extractor-ogg-test.logcat, from tools/android-reference.sh --app
# media-extractor-ogg-test): the Ogg Opus files' track formats, metrics, samples and seeks through their descriptors
# and through MediaDataSources of their size, of an unknown size and cut short, and their samples decoded by
# MediaCodec, whole and across a seek. libmediandk's Ogg Opus reader (runtime/android/ogg_extractor.cpp) reads them.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/media-extractor-ogg-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-checks
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert graphics-run >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh media-extractor-ogg-test >/dev/null
app=media-extractor-ogg-test
python3 framework/tests/java_app_run.py "$app" io.github.kisame76.minato.test.mediaextractorogg \
  io.github.kisame76.minato.test.mediaextractorogg.ExtractorOggApplication build/art build/bionic/lib \
  build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/graphics/tests/graphics-run" "build/$app-check" \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  "build/$app/$app.apk" fixtures/expected/$app.logcat
