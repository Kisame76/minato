#!/usr/bin/env bash
# Checks what an app gets from a PackageInstaller session that holds a split set (install-set-test): the set is taken
# like one APK, the user is asked (STATUS_PENDING_USER_ACTION with the confirm intent). Runs on the Mac
# (framework/tests/java_app_run.py) and compares with fixtures/expected/install-set-test.logcat, the log of Android 15.
# The set is splits-test's, which tools/build-splits-test.sh builds (it runs first when the set is missing).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/install-set-check.sh
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
[[ -f build/splits-test/set/base.apk ]] || tools/build-splits-test.sh >/dev/null
tools/build-java-test-app.sh install-set-test >/dev/null

output=$(python3 framework/tests/java_app_run.py --lifecycle start install-set-test \
  io.github.kisame76.minato.test.installset io.github.kisame76.minato.test.installset.SetApplication build/art \
  build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/install-set-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/install-set-test/install-set-test.apk fixtures/expected/install-set-test.logcat 1290x2796@3,en-US) || { echo "$output"; exit 1; }
echo "$output"
