#!/usr/bin/env bash
# Checks what an app that wants to install apps sees first (install-sources-test): canRequestPackageInstalls is false,
# and the settings page for unknown app sources of the app's own package resolves. Runs on the Mac
# (framework/tests/java_app_run.py) and compares with fixtures/expected/install-sources-test.logcat, the log of Android 15.
# It builds apk-run and apk-convert into build/native/macos-app, app_process, framework.jar and the test every time.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/install-sources-check.sh
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
tools/build-java-test-app.sh install-sources-test >/dev/null

python3 framework/tests/java_app_run.py --lifecycle start install-sources-test \
  io.github.kisame76.minato.test.installsources io.github.kisame76.minato.test.installsources.SourcesApplication build/art \
  build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/install-sources-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/install-sources-test/install-sources-test.apk fixtures/expected/install-sources-test.logcat 1290x2796@3,en-US
