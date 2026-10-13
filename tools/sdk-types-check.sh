#!/usr/bin/env bash
# Runs the test of the plain framework types that SDKs and libraries reach, sdk-types-test, on the Mac
# (framework/tests/java_app_run.py; framework breadth, batch B4): builds apk-run and apk-convert for macOS into
# build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and
# sdk-types-test (seconds) every time, so it never tests an old binary; then starts sdk-types-test through app_process and
# compares its log with Android 15's on apk-runner-35 (fixtures/expected/sdk-types-test.logcat, from
# tools/android-reference.sh --app sdk-types-test): android.util's mutable numbers and printers, an AtomicFile's round
# trip, the Rfc822 token, the keystore exceptions, the provider constants, GeomagneticField's numbers and the
# classes that only have to load.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/sdk-types-check.sh
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
tools/build-java-test-app.sh sdk-types-test >/dev/null

python3 framework/tests/java_app_run.py sdk-types-test io.github.kisame76.minato.test.sdktypes \
  io.github.kisame76.minato.test.sdktypes.SdkTypesApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/sdk-types-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/sdk-types-test/sdk-types-test.apk fixtures/expected/sdk-types-test.logcat
