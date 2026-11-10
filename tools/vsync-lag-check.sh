#!/usr/bin/env bash
# Runs ADR-0041 a) decision 6's test of a frame clock that posts late, on the Mac (framework/tests/java_app_run.py):
# builds graphics-run (apk-run with graphics/host's providers and the tests' control) and apk-convert for macOS into
# build/native/macos-checks, and app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and
# vsync-lag-test (seconds) every time, so it never tests an old binary; then runs vsync-lag-test under graphics-run
# without a session, so that no frame clock runs and the app's JNI library posts each tick through the tests' control,
# and compares its lines with fixtures/expected/vsync-lag-test.txt: Choreographer's frame time after a late frame, and
# a tick from before the next frame's request, which that frame does not take.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/vsync-lag-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."
# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi
NATIVE=build/native/macos-checks
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target graphics-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh vsync-lag-test >/dev/null
python3 framework/tests/java_app_run.py vsync-lag-test io.github.kisame76.minato.test.vsynclag \
  io.github.kisame76.minato.test.vsynclag.VsyncLagApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" build/vsync-lag-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/vsync-lag-test/vsync-lag-test.apk \
  fixtures/expected/vsync-lag-test.txt
