#!/usr/bin/env bash
# Runs the test of android.media.audiofx on the Mac (the audio effects plan; framework/tests/java_app_run.py):
# audiofx-test, whose Equalizer, Dynamic Bass Boost, Virtualizer, Loudness Enhancer and reverbs run on AOSP's Java over
# the local effect service (framework/native/android_runtime/android_media_AudioEffect.cpp). It builds apk-run and
# apk-convert for macOS into build/native/macos-app (or $NATIVE), and app_process with libandroid_runtime.so,
# framework.jar and the app every time, then compares its log with Android 15's on apk-runner-35
# (fixtures/expected/audiofx-test.logcat, from tools/android-reference.sh --app audiofx-test): the effects listed, the
# parameters with their ranges and defaults, control between instances, and the access after the release.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/audiofx-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=${NATIVE:-build/native/macos-app}
BIONIC=${BIONIC:-build/bionic}
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh audiofx-test >/dev/null

python3 framework/tests/java_app_run.py audiofx-test io.github.kisame76.minato.test.audiofx \
  io.github.kisame76.minato.test.audiofx.AudioFxApplication build/art "$BIONIC/lib" "$BIONIC/system" \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/audiofx-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/audiofx-test/audiofx-test.apk fixtures/expected/audiofx-test.logcat
