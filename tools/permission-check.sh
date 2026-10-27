#!/usr/bin/env bash
# Runs the test of Activity.requestPermissions (ADR-0063) on the Mac (framework/tests/java_app_run.py): builds
# apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and the app (seconds) every time, so it never tests an old binary;
# then starts permission-request-test through app_process in a session on the M4 plan's display and compares its
# log with Android 15's on apk-runner-35 (fixtures/expected/permission-request-test.logcat, from
# tools/android-reference.sh --app permission-request-test): a request for a normal permission, for one the app does
# not declare, for a mix, for an unknown one and for none, each answered without a dialog as the permission
# controller answers.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/permission-check.sh
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
tools/build-java-test-app.sh permission-request-test >/dev/null

python3 framework/tests/java_app_run.py --lifecycle start \
  permission-request-test io.github.kisame76.minato.test.permreq \
  android.app.Application build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/permission-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/permission-request-test/permission-request-test.apk fixtures/expected/permission-request-test.logcat \
  1290x2796@3,en-US
