#!/usr/bin/env bash
# Runs ADR-0060 e2b's test of MediaCodec on the Mac (framework/tests/java_app_run.py): builds apk-run, apk-convert
# and graphics-run for macOS into build/native/macos-grafik, and app_process with libandroid_runtime.so
# (incremental), framework.jar and the app every time, so it never tests an old binary; then media-codec-test against
# Android 15's log (fixtures/expected/media-codec-test.logcat, from tools/android-reference.sh --app
# media-codec-test): H.264 decoded in ByteBuffer mode from Minato's own streams, synchronously, asynchronously and
# across a flush, each frame by its time, flags and its samples' CRC32s, the refusals, and the NDK's AMediaCodec
# through the app's JNI library. The decoder runs on graphics/host's VideoToolbox through graphics-run.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/media-codec-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-grafik
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert graphics-run >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh media-codec-test >/dev/null
app=media-codec-test
python3 framework/tests/java_app_run.py "$app" io.github.kisame76.minato.test.mediacodec \
  io.github.kisame76.minato.test.mediacodec.MediaCodecApplication build/art build/bionic/lib \
  build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/graphics/tests/graphics-run" "build/$app-check" \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  "build/$app/$app.apk" fixtures/expected/media-codec-test.logcat
