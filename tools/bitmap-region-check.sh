#!/usr/bin/env bash
# Checks the calls of a tiled image viewer on BitmapRegionDecoder (bitmap-region-test): a PNG and a JPEG opened from
# bytes, a stream, a path and a descriptor, regions of four quadrants at three sample sizes, a 16-bit config, a region
# partly and one wholly outside. Runs on the Mac (framework/tests/java_app_run.py) and compares with
# fixtures/expected/bitmap-region-test.logcat, the log of Android 15.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/bitmap-region-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh bitmap-region-test >/dev/null

python3 framework/tests/java_app_run.py --lifecycle start bitmap-region-test \
  io.github.kisame76.minato.test.bitmapregion io.github.kisame76.minato.test.bitmapregion.RegionApplication build/art \
  build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/bitmap-region-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/bitmap-region-test/bitmap-region-test.apk fixtures/expected/bitmap-region-test.logcat 1290x2796@3,en-US
