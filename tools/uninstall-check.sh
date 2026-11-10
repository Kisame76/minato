#!/usr/bin/env bash
# Checks what an app gets when it asks to uninstall an app (uninstall-test, with REQUEST_DELETE_PACKAGES, and
# uninstall-noperm-test, without it): ACTION_UNINSTALL_PACKAGE and ACTION_DELETE for an absent package and for the
# app itself, and PackageInstaller.uninstall with its status callback. Runs on the Mac (framework/tests/java_app_run.py)
# and compares with fixtures/expected/<test>.logcat, the log of Android 15. The Mac has no host to ask, so a request
# for the app itself is declined as a user's Cancel (RESULT_CANCELED); the device line says how often the framework
# asked the host. Takes about 4 minutes.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/uninstall-check.sh
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

for test in uninstall-test uninstall-noperm-test; do
  package=io.github.kisame76.minato.test.${test%-test}
  package=${package/uninstall-noperm/uninstallnoperm}
  tools/build-java-test-app.sh "$test" >/dev/null
  output=$(python3 framework/tests/java_app_run.py "$test" "$package" "$package.UninstallApplication" build/art \
    build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
    "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" "build/$test-check" \
    build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
    "build/$test/$test.apk" "fixtures/expected/$test.logcat" 1290x2796@3,en-US) || { echo "$output"; exit 1; }
  echo "$output"
done
