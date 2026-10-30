#!/usr/bin/env bash
# Runs the test of SDL 2.30's Java audio driver sequence (sdl-audiotrack-test; the audio speed of an SDL2 game) on the
# Mac
# (framework/tests/java_app_run.py), as tools/sdl-audiotrack-check.sh does for sdl-audiotrack-test: it builds apk-run
# and
# apk-convert for macOS into build/native/macos-app, app_process with libandroid_runtime.so (incremental), framework.jar
# and the app every time, then starts the app through app_process and compares its log with Android 15's
# (fixtures/expected/sdl-audiotrack-test.logcat, from tools/android-reference.sh --app sdl-audiotrack-test):
# SDLAudioManager's
# audioOpen for 48000, 44100 and 22050 Hz, 8-bit, 16-bit and float, mono and stereo, the values read back, and the write
# loop that advances by the count a write returns and sleeps a millisecond on 0, paced by the track at its rate.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/sdl-audiotrack-check.sh
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
tools/build-java-test-app.sh sdl-audiotrack-test >/dev/null

python3 framework/tests/java_app_run.py sdl-audiotrack-test io.github.kisame76.minato.test.sdlaudiotrack \
  io.github.kisame76.minato.test.sdlaudiotrack.SdlAudioTrackApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/sdl-audiotrack-check build/framework-native/app_process64 \
  build/framework/framework.jar \
  build/framework-res/framework-res build/sdl-audiotrack-test/sdl-audiotrack-test.apk \
  fixtures/expected/sdl-audiotrack-test.logcat
