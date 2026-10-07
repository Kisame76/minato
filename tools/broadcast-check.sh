#!/usr/bin/env bash
# Runs ADR-0045 decision 5's test on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for
# macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about
# 10 s) and broadcast-test (seconds) every time, so it never tests an old binary; then starts broadcast-test through
# app_process in a session on the M4 plan's display and compares its log with Android 15's
# (fixtures/expected/broadcast-test.logcat, from tools/android-reference.sh --app broadcast-test): IntentFilter's
# matching and Base64 (step d1), then the app's own receivers and broadcasts (step d2): Android 14's export flags, an
# unordered broadcast, ordered ones by priority with their results, goAsync and an abort, one nobody receives, a
# receiver on its own handler, an unregistered one, one unregistered right after an ordered broadcast to it is sent,
# and one that an activity leaks at its destroy.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/broadcast-check.sh
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
tools/build-java-test-app.sh broadcast-test >/dev/null

python3 framework/tests/java_app_run.py broadcast-test io.github.kisame76.minato.test.broadcast android.app.Application \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/broadcast-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/broadcast-test/broadcast-test.apk fixtures/expected/broadcast-test.logcat 1290x2796@3,en-US
