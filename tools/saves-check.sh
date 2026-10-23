#!/usr/bin/env bash
# Runs the test of ADR-0057 step d, saves-test, on the Mac (framework/tests/java_app_run.py):
# builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and saves-test (seconds) every time, so it never tests an old binary;
# then starts saves-test through app_process and compares its log with Android 15's on apk-runner-35
# (fixtures/expected/saves-test.logcat, from tools/android-reference.sh --app saves-test): an app saves note.txt with
# ACTION_CREATE_DOCUMENT and opens the folder treetest with ACTION_OPEN_DOCUMENT_TREE, which the harness (apk-run with
# a session) answers as a user who chooses Download and treetest (a.txt, b.txt) there, as a host does; the app logs the
# results' grants, the provider's rows and calls (create, rename, delete) and the bytes it writes and reads.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/saves-check.sh
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
tools/build-java-test-app.sh saves-test >/dev/null

# The folder the user opens, the one tools/android-reference.sh puts into Download on the AVD.
FILES=fixtures/apps/saves-test/files
python3 framework/tests/java_app_run.py --lifecycle start \
  --pick-save --pick-tree "$FILES/treetest" \
  saves-test io.github.kisame76.minato.test.saves \
  android.app.Application build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/saves-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/saves-test/saves-test.apk fixtures/expected/saves-test.logcat \
  1290x2796@3,en-US
