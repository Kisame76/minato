#!/usr/bin/env bash
# Runs the test of android.media.audiofx.Visualizer on the Mac (the audio effects plan,
# framework/tests/java_app_run.py):
# visualizer-test, whose app has no RECORD_AUDIO grant, and its twin visualizer-test-granted, which the runtime holds a
# grant for (MINATO_GRANT_MICROPHONE). It builds apk-run and apk-convert for macOS into build/native/macos-app (or
# $NATIVE), and app_process with libandroid_runtime.so, framework.jar and the two apps every time, then compares each
# log with Android 15's on apk-runner-35 (fixtures/expected/visualizer-test.logcat and visualizer-test-granted.logcat,
# from tools/android-reference.sh --app): the capture sizes and modes with their answers, the waveform, the FFT, the
# peak and RMS of silence, the periodic capture's callbacks, and the access after the release. It passes a display
# configuration, which gives the app a session and a frame clock; without one the main looper dispatches no post from
# another thread (framework/tests/java_app_run.py).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/visualizer-check.sh
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
tools/build-java-test-app.sh visualizer-test >/dev/null
tools/build-java-test-app.sh visualizer-test-granted >/dev/null

run() {  # run <app> <package> <expected>
  python3 framework/tests/java_app_run.py "$1" "$2" io.github.kisame76.minato.test.visualizer.VisualizerApplication \
    build/art "$BIONIC/lib" "$BIONIC/system" build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
    "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" "build/visualizer-check/$1" \
    build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
    "build/$1/$1.apk" "$3" 1290x2796@3,en-US
}

run visualizer-test io.github.kisame76.minato.test.visualizer fixtures/expected/visualizer-test.logcat
MINATO_GRANT_MICROPHONE=granted run visualizer-test-granted io.github.kisame76.minato.test.visualizergranted \
  fixtures/expected/visualizer-test-granted.logcat
echo "visualizer-check ok"
