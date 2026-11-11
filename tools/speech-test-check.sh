#!/usr/bin/env bash
# Runs the speech recognition test on the Mac (group 3 of the framework breadth, framework/tests/java_app_run.py):
# builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and speech-test (seconds) every time, so it never tests an old binary; then
# starts speech-test through app_process, whose Application asks SpeechRecognizer what a phone without a recognition
# service answers, starts listening and logs what the listener hears, in an app without RECORD_AUDIO, and compares its
# log with Android 15's (fixtures/expected/speech-test.logcat, from tools/android-reference.sh --app speech-test).
# Minato has no recognition: the honest limit, answered as the phone does. Needs tools/build-art.sh,
# tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/speech-test-check.sh
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
tools/build-java-test-app.sh speech-test >/dev/null

python3 framework/tests/java_app_run.py speech-test io.github.kisame76.minato.test.speech \
  io.github.kisame76.minato.test.speech.SpeechApplication build/art "$BIONIC/lib" "$BIONIC/system" \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/speech-test-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/speech-test/speech-test.apk \
  fixtures/expected/speech-test.logcat 1290x2796@3,en-US
