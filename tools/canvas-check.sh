#!/usr/bin/env bash
# Runs ADR-0043 test 2's canvas-test on the Mac against Android 15 (framework/tests/java_app_run.py): builds apk-run and
# apk-convert for macOS into build/native/macos-checks, and app_process with libandroid_runtime.so (incremental),
# framework.jar (about 10 s) and canvas-test (seconds) every time, so it never tests an old binary; then starts
# canvas-test through app_process, compares its log with fixtures/expected/canvas-test.logcat and the pixels it wrote
# with fixtures/expected/canvas-test.rgba.gz (graphics/tests/canvas_compare.py), both from tools/android-reference.sh
# --app canvas-test. build/framework-native and build/framework are shared builds, as App's checks rebuild them: a
# run in the shared tree is announced first.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-aapt2.sh and
# tools/build-framework-res.sh first.
# Usage: tools/canvas-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

APP=canvas-test
PACKAGE=io.github.kisame76.minato.test.canvas
for expected in fixtures/expected/$APP.logcat fixtures/expected/$APP.rgba.gz; do
  if [[ ! -f "$expected" ]]; then
    echo "FAIL: no $expected: run tools/android-reference.sh --app $APP"
    exit 1
  fi
done

NATIVE=build/native/macos-checks
WORK=build/canvas-check
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh $APP >/dev/null

python3 framework/tests/java_app_run.py $APP $PACKAGE $PACKAGE.CanvasApplication build/art build/bionic/lib \
  build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" "$WORK" build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/$APP/$APP.apk fixtures/expected/$APP.logcat
# The app wrote its pixels into its cache folder, /data/user/0/<package>/cache, which the harness keeps in $WORK/run.
python3 graphics/tests/canvas_compare.py fixtures/expected/$APP.rgba.gz "$WORK/run/data/user/0/$PACKAGE/cache/$APP.rgba" \
  480 480
