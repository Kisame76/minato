#!/usr/bin/env bash
# Runs ADR-0043 test 2's first test on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for
# macOS into build/native/macos-checks, and app_process with libandroid_runtime.so (incremental), framework.jar (about
# 10 s) and graphics-start-test (seconds) every time, so it never tests an old binary; then starts graphics-start-test
# through app_process, which registers hwui's graphics natives, and compares its lines with
# fixtures/expected/graphics-start-test.txt. build/framework-native and build/framework are shared builds, as App's
# checks rebuild them: a run in the shared tree is announced first.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/graphics-start-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-checks
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh graphics-start-test >/dev/null

python3 framework/tests/java_app_run.py graphics-start-test io.github.kisame76.minato.test.graphicsstart \
  io.github.kisame76.minato.test.graphicsstart.GraphicsStartApplication build/art build/bionic/lib \
  build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/graphics-start-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/graphics-start-test/graphics-start-test.apk fixtures/expected/graphics-start-test.txt
