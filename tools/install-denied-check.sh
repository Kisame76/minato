#!/usr/bin/env bash
# Checks what an app gets when it hands an APK to the installer without the right to install apps (install-denied-test):
# the installer asks for the right first (the framework's settings stand-in asks the host once), and with no answer the
# app gets RESULT_CANCELED. Runs on the Mac (framework/tests/java_app_run.py) and compares with
# fixtures/expected/install-denied-test.logcat; the device line "asked the host for the right: 1" is required besides.
# There is no Android reference run: the stock installer's dialog needs a tap. Takes about 2 minutes.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/install-denied-check.sh
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
tools/build-java-test-app.sh install-denied-test >/dev/null

output=$(python3 framework/tests/java_app_run.py install-denied-test \
  io.github.kisame76.minato.test.installdenied io.github.kisame76.minato.test.installdenied.DeniedApplication build/art \
  build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/install-denied-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/install-denied-test/install-denied-test.apk fixtures/expected/install-denied-test.logcat 1290x2796@3,en-US) || { echo "$output"; exit 1; }
echo "$output"
grep -q 'asked the host for the right: 1' <<<"$output" || { echo "FAIL: the host was not asked for the right exactly once"; exit 1; }
