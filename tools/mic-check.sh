#!/usr/bin/env bash
# Runs the test of microphone input on the Mac (ADR-0064; framework/tests/java_app_run.py): mic-test, whose app has no
# RECORD_AUDIO grant, and its twin mic-test-granted, which the runtime holds a grant for (MINATO_GRANT_MICROPHONE) and
# whose microphone hears a 440 Hz tone (MINATO_AUDIO_INPUT_FILE, a WAV file made here). It builds apk-run and
# apk-convert for macOS into build/native/macos-app (or $NATIVE), and app_process with libandroid_runtime.so,
# framework.jar and the two apps every time, then compares each log with Android 15's on apk-runner-35
# (fixtures/expected/mic-test.logcat and mic-test-granted.logcat, from tools/android-reference.sh --app): the permission
# state, the minimum sizes, and the recorder's checks, states and reads. The tone is checked on top: the granted
# app's device line must say about 440 Hz.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/mic-check.sh
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
tools/build-java-test-app.sh mic-test >/dev/null
tools/build-java-test-app.sh mic-test-granted >/dev/null

mkdir -p build/mic-check
python3 -I - build/mic-check/tone.wav <<'PY'
import math, struct, sys, wave
with wave.open(sys.argv[1], "wb") as out:
    out.setnchannels(1)
    out.setsampwidth(2)
    out.setframerate(16000)
    tone = (int(12000 * math.sin(2 * math.pi * 440 * n / 16000)) for n in range(16000))
    out.writeframes(b"".join(struct.pack("<h", sample) for sample in tone))
PY

run() {  # run <app> <package> <class> <expected>
  python3 framework/tests/java_app_run.py "$1" "$2" "$3" build/art "$BIONIC/lib" "$BIONIC/system" \
    build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
    "$NATIVE/runtime/apk-run" "build/mic-check/$1" build/framework-native/app_process64 build/framework/framework.jar \
    build/framework-res/framework-res "build/$1/$1.apk" "$4"
}

run mic-test io.github.kisame76.minato.test.mic io.github.kisame76.minato.test.mic.MicApplication \
  fixtures/expected/mic-test.logcat
MINATO_GRANT_MICROPHONE=granted MINATO_AUDIO_INPUT_FILE=build/mic-check/tone.wav \
  run mic-test-granted io.github.kisame76.minato.test.micgranted io.github.kisame76.minato.test.mic.MicApplication \
  fixtures/expected/mic-test-granted.logcat | tee build/mic-check/granted.out
grep -h "what the microphone heard" build/mic-check/granted.out | grep -q "about 440 Hz" \
  || { echo "mic-test-granted: the tone did not come through"; exit 1; }
echo "mic-check ok"
