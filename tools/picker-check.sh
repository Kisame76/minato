#!/usr/bin/env bash
# Runs the test of ADR-0057 step a, picker-test, on the Mac (framework/tests/java_app_run.py):
# builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and picker-test (seconds) every time, so it never tests an old
# binary; then starts picker-test through app_process and compares its log with Android 15's on
# apk-runner-35 (fixtures/expected/picker-test.logcat, from tools/android-reference.sh --app picker-test): an app that
# handles none of the file picker intents resolves each, starts it for a result and logs the lifecycle around the
# picker, which the harness (apk-run with a session) cancels as a user does; Android 15's DocumentsUI stops the app,
# its photo picker only pauses it.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/picker-check.sh
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
tools/build-java-test-app.sh picker-test >/dev/null

# What the host got for each picker (apk-run writes a request's text on one line, its tabs and newlines as \t and \n):
# the fields of each kind, and a title with an emoji as UTF-8.
python3 framework/tests/java_app_run.py --lifecycle start \
  --in-order 'apk-run: pick 1: 1\t1\t0\t*/*\napplication/pdf\nimage/*\t' \
  --in-order 'apk-run: pick 2: 2\t0\t0\ttext/plain\t' \
  --in-order 'apk-run: pick 3: 3\t0\t0\ttext/plain\tnote 😀 ä.txt' \
  --in-order 'apk-run: pick 4: 4\t0\t0\t\t' \
  --in-order 'apk-run: pick 5: 5\t0\t5\t\t' \
  picker-test io.github.kisame76.minato.test.picker \
  android.app.Application build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/picker-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/picker-test/picker-test.apk fixtures/expected/picker-test.logcat \
  1290x2796@3,en-US
