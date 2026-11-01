#!/usr/bin/env bash
# Runs the test of an app's meeting no Google Play services and no Play Store on the Mac
# (framework/tests/java_app_run.py; the compatibility plan's open gap): builds apk-run and apk-convert for macOS into
# build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar and gms-absent-test
# every time, then starts gms-absent-test through app_process and compares its log with Android 15's on apk-runner-35,
# which has no Play (fixtures/expected/gms-absent-test.logcat, from tools/android-reference.sh --app gms-absent-test):
# the package lookups, binds, starts, links, content providers and package contexts of com.google.android.gms and
# com.android.vending.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/gms-absent-check.sh
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
tools/build-java-test-app.sh gms-absent-test >/dev/null

python3 framework/tests/java_app_run.py gms-absent-test io.github.kisame76.minato.test.gmsabsent \
  io.github.kisame76.minato.test.gmsabsent.GmsAbsentApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/gms-absent-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/gms-absent-test/gms-absent-test.apk \
  fixtures/expected/gms-absent-test.logcat
