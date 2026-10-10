#!/usr/bin/env bash
# Runs the test of android.media.AudioTrack on the Mac (framework/tests/java_app_run.py; the Unity test APK probe's FMOD output
# thread): builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with
# libandroid_runtime.so (incremental), framework.jar (about 10 s) and audiotrack-test (seconds) every time, so it never
# tests an old binary; then starts audiotrack-test through app_process and compares its log with Android 15's on
# apk-runner-35 (fixtures/expected/audiotrack-test.logcat, from tools/android-reference.sh --app audiotrack-test):
# minimum buffer sizes and their refusals, tracks from the legacy constructors and the Builder with their checks and
# states, streaming writes that fill the buffer, return short, block or fail, a pause that interrupts a blocked write,
# flush, stop and release, and a static clip. Its tracks play silence on the Mac's output.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/audiotrack-check.sh
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
tools/build-java-test-app.sh audiotrack-test >/dev/null

python3 framework/tests/java_app_run.py audiotrack-test io.github.kisame76.minato.test.audiotrack \
  io.github.kisame76.minato.test.audiotrack.AudioTrackApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/audiotrack-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/audiotrack-test/audiotrack-test.apk fixtures/expected/audiotrack-test.logcat
