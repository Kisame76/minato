#!/usr/bin/env bash
# Runs the WorkManager test (workmanager-test: androidx.work 2.12.0 with its closure, built by
# tools/build-workmanager-test.sh from the libraries of tools/fetch-workmanager-libraries.py) on the Mac
# (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS into build/native/macos-app,
# app_process with libandroid_runtime.so (incremental), framework.jar and the app every time, then starts the app
# through app_process in a session on the M4 plan's display, the iPhone 15 Pro Max's 1290 x 2796 px at scale 3 in
# en-US, with the host online over Wi-Fi (apk-run's network:3 step; the Mac harness has no path monitor of its own).
# Its log must be fixtures/expected/workmanager-test.logcat, Android 15's (tools/android-reference.sh). Without
# the libraries, which tools/fetch-workmanager-libraries.py fetches only on request, it is skipped.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-aapt2.sh and
# tools/build-framework-res.sh first.
# Usage: tools/workmanager-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi
if ! libraries=$(/usr/bin/python3 tools/fetch-workmanager-libraries.py 2>&1); then
  echo "skipped: workmanager-test: no WorkManager libraries (tools/fetch-workmanager-libraries.py --fetch, on request):"
  echo "$libraries" | sed 's/^/  /'
  exit 0
fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-workmanager-test.sh >/dev/null
python3 framework/tests/java_app_run.py --restart --lifecycle start,network:3 \
  workmanager-test io.github.kisame76.minato.test.workmanager android.app.Application \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/workmanager-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/workmanager-test/workmanager-test.apk fixtures/expected/workmanager-test.logcat 1290x2796@3,en-US
