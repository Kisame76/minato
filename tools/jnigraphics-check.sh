#!/usr/bin/env bash
# Runs ADR-0041 a5's test on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS into
# build/native/macos-grafik, and app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and
# jnigraphics-test (seconds) every time, so it never tests an old binary; then starts jnigraphics-test through
# app_process. Its JNI library links the NDK's libjnigraphics, which hwui's set holds (tools/build-hwui.sh), reads two
# Java bitmaps through AndroidBitmap_* and decodes an asset through AImageDecoder; its log must be Android 15's
# (fixtures/expected/jnigraphics-test.logcat, from tools/android-reference.sh --app jnigraphics-test).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/jnigraphics-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-grafik
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh jnigraphics-test >/dev/null

python3 framework/tests/java_app_run.py jnigraphics-test io.github.kisame76.minato.test.jnigraphics \
  io.github.kisame76.minato.test.jnigraphics.JnigraphicsApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/jnigraphics-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/jnigraphics-test/jnigraphics-test.apk \
  fixtures/expected/jnigraphics-test.logcat
