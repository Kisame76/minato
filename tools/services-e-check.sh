#!/usr/bin/env bash
# Runs the test of six classes that apps ask for at start on the Mac
# (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS into build/native/macos-app, and
# app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and services-e-test (seconds)
# every time, so it never tests an old binary; then starts the app's own Application through app_process and compares
# its log with Android 15's on apk-runner-35-bare (fixtures/expected/services-e-test.logcat, from
# tools/android-reference.sh --app services-e-test): the fingerprint manager with its callback and crypto object, the
# grammatical inflection and print managers, the share action provider, the quick settings tile service and the DRM
# manager client, each with Android's answer or exception.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/services-e-check.sh
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
tools/build-java-test-app.sh services-e-test >/dev/null

python3 framework/tests/java_app_run.py --lifecycle start services-e-test \
  io.github.kisame76.minato.test.servicese io.github.kisame76.minato.test.servicese.ServicesEApplication build/art \
  build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/services-e-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/services-e-test/services-e-test.apk fixtures/expected/services-e-test.logcat 1290x2796@3,en-US
