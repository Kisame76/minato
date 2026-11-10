#!/usr/bin/env bash
# Runs ADR-0060 e2f's test of MediaCodec's AAC decoder on the Mac (framework/tests/java_app_run.py): builds apk-run,
# apk-convert and graphics-run for macOS into build/native/macos-checks, and app_process with libandroid_runtime.so
# (incremental), framework.jar and the app every time, so it never tests an old binary; then media-codec-audio-test
# against Android 15's log (fixtures/expected/media-codec-audio-test.logcat, from tools/android-reference.sh --app
# media-codec-audio-test): MediaCodecList's AAC decoders, baseline.mp4's AAC track through MediaExtractor decoded
# synchronously, across a seek and a flush, asynchronously and by name, its output formats, buffers, times and
# samples (count, RMS, zero crossings), and the refusals. The decoder runs on the runtime's AudioToolbox
# (APKRT_NR_AUDIO_CODEC).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/media-codec-audio-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-checks
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert graphics-run >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh media-codec-audio-test >/dev/null
app=media-codec-audio-test
python3 framework/tests/java_app_run.py "$app" io.github.kisame76.minato.test.mediacodecaudio \
  io.github.kisame76.minato.test.mediacodecaudio.AudioCodecApplication build/art build/bionic/lib \
  build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/graphics/tests/graphics-run" "build/$app-check" \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  "build/$app/$app.apk" fixtures/expected/$app.logcat
