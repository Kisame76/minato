#!/usr/bin/env bash
# Runs the compatibility plan's gap 7 test on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert
# for macOS into build/native/macos-app, and app_process with libandroid_runtime.so and libsqlite.so (incremental),
# framework.jar (about 10 s) and sqlite-test (seconds) every time, so it never tests an old binary; then starts
# sqlite-test through app_process in a session on the M4 plan's display and compares its log with Android 15's
# (fixtures/expected/sqlite-test.logcat, from tools/android-reference.sh --app sqlite-test): an app's own SQLite
# database through android.database.sqlite on the main thread, from SQLiteOpenHelper's create to deleteDatabase.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/sqlite-check.sh
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
tools/build-java-test-app.sh sqlite-test >/dev/null

python3 framework/tests/java_app_run.py sqlite-test io.github.kisame76.minato.test.sqlite android.app.Application \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/sqlite-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/sqlite-test/sqlite-test.apk fixtures/expected/sqlite-test.logcat 1290x2796@3,en-US
