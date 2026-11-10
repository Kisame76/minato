#!/usr/bin/env bash
# Runs the test of bundle C, part 1, of the service inventory (services-j-test) on the Mac (framework/tests/java_app_run.py):
# the IPsec, VPN management and connectivity diagnostics managers, each asked for by name and by class, with every
# zero-argument getter and predicate of its class called. It builds apk-run and apk-convert for macOS into
# build/native/macos-app, and app_process, framework.jar and the test every time, then compares the app's log with
# Android 15's on apk-runner-35-bare (fixtures/expected/services-j-test.logcat, from tools/android-reference.sh --app
# services-j-test).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/services-j-check.sh
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
tools/build-java-test-app.sh services-j-test >/dev/null

python3 framework/tests/java_app_run.py --lifecycle start services-j-test \
  io.github.kisame76.minato.test.servicesj io.github.kisame76.minato.test.servicesj.ServicesJApplication build/art \
  build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/services-j-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/services-j-test/services-j-test.apk fixtures/expected/services-j-test.logcat 1290x2796@3,en-US
