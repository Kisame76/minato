#!/usr/bin/env bash
# Runs the test of ADR-0057 step b, documents-test, on the Mac (framework/tests/java_app_run.py):
# builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and documents-test (seconds) every time, so it never tests an old
# binary; then starts documents-test through app_process and compares its log with Android 15's on apk-runner-35
# (fixtures/expected/documents-test.logcat, from tools/android-reference.sh --app documents-test): an app asks the
# documents provider for a document it holds no grant for, then opens a document, two and one with GET_CONTENT, whose
# picks the harness (apk-run with a session) answers with known.txt and data.bin as a host does: clones in Download and
# the manifest; the app logs the grant, the provider's row, the type, the bytes and the persisted grants it sees.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/documents-check.sh
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
tools/build-java-test-app.sh documents-test >/dev/null

# The files the user picks, the ones tools/android-reference.sh puts into Download on the AVD.
FILES=fixtures/apps/documents-test/files
python3 framework/tests/java_app_run.py --lifecycle start \
  --pick "$FILES/known.txt" --pick "$FILES/data.bin" \
  documents-test io.github.kisame76.minato.test.documents \
  android.app.Application build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/documents-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/documents-test/documents-test.apk fixtures/expected/documents-test.logcat \
  1290x2796@3,en-US
