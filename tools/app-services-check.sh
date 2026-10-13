#!/usr/bin/env bash
# Runs the test of the app's own started services on the Mac (framework/tests/java_app_run.py; ADR-0045 step SV2):
# builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and app-services-test (seconds) every time, so it never tests an old
# binary; then starts app-services-test through app_process and compares its log with Android 15's on apk-runner-35
# (fixtures/expected/app-services-test.logcat, from tools/android-reference.sh --app app-services-test): startService,
# startForegroundService, stopService, stopSelf and stopSelfResult, startForeground and stopForeground, each service
# made through the app's AppComponentFactory with its callbacks on the main thread.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/app-services-check.sh
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
tools/build-java-test-app.sh app-services-test >/dev/null

python3 framework/tests/java_app_run.py app-services-test io.github.kisame76.minato.test.appservices \
  io.github.kisame76.minato.test.appservices.AppServicesApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/app-services-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/app-services-test/app-services-test.apk \
  fixtures/expected/app-services-test.logcat 1290x2796@3,en-US
