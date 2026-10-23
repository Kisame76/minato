#!/usr/bin/env bash
# Runs the restart test of ADR-0057 step b, documents-restart-test, on the Mac (framework/tests/java_app_run.py):
# builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and the test app (seconds) every time, so it never tests an old binary;
# then starts the app twice over the same folders (--restart): the first start opens a document and keeps its grant,
# opens two and keeps nothing more; the second finds the kept grant, not the other, and the clone nobody keeps gone.
# The expectation is Minato's own (fixtures/expected/documents-restart-test.txt): nothing on Android deletes a file.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/documents-restart-check.sh
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
tools/build-java-test-app.sh documents-restart-test >/dev/null

# The files the user picks, the ones tools/android-reference.sh puts into Download on the AVD.
FILES=fixtures/apps/documents-test/files
python3 framework/tests/java_app_run.py --lifecycle start --restart \
  --pick "$FILES/known.txt" --pick "$FILES/data.bin" \
  documents-restart-test io.github.kisame76.minato.test.documentsrestart \
  android.app.Application build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/documents-restart-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/documents-restart-test/documents-restart-test.apk fixtures/expected/documents-restart-test.txt \
  1290x2796@3,en-US
