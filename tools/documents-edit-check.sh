#!/usr/bin/env bash
# Runs the write-back test of ADR-0057, documents-edit-test, on the Mac (framework/tests/java_app_run.py):
# builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and the test app (seconds) every time, so it never tests an old binary;
# then starts the app through app_process and compares its log with Android 15's on apk-runner-35
# (fixtures/expected/documents-edit-test.logcat, from tools/android-reference.sh --app documents-edit-test): the app
# opens a document, replaces its content through the URI and reads it back. On Android the URI is the file, so the
# user's file holds the new text; here the document is a clone and the host (apk-run) writes it back to the user's
# file at the session's end, which this check reads: a copy of known.txt in a scratch folder, so that the fixture stays.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/documents-edit-check.sh
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
tools/build-java-test-app.sh documents-edit-test >/dev/null

# The user's file: a copy, which the host writes back to.
SOURCE=$(mktemp -d "${TMPDIR:-/tmp}/documents-edit.XXXXXX")
trap 'rm -rf "$SOURCE"' EXIT
cp fixtures/apps/documents-edit-test/files/known.txt "$SOURCE/known.txt"
python3 framework/tests/java_app_run.py --lifecycle start \
  --pick "$SOURCE/known.txt" \
  documents-edit-test io.github.kisame76.minato.test.documentsedit \
  android.app.Application build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/documents-edit-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/documents-edit-test/documents-edit-test.apk fixtures/expected/documents-edit-test.logcat \
  1290x2796@3,en-US

if [[ "$(cat "$SOURCE/known.txt")" != "edited by app" ]]; then
  echo "documents-edit-test: the user's file was not written back: $(cat "$SOURCE/known.txt")"
  exit 1
fi
echo "documents-edit-test: the user's file holds the app's text"
