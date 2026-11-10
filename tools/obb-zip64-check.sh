#!/usr/bin/env bash
# Runs obb-zip64-test on the Mac (framework/tests/java_app_run.py): an app opens a main OBB that is a Zip64 archive
# (a stored entry of 4 GiB and 64 KiB and a small one behind it, beyond 2^32) with java.util.zip.ZipFile and reads both.
# The OBB is sparse, made by tools/make-sparse-zip64-obb.py into the check's work folder and removed afterwards; its
# log must be Android 15's (fixtures/expected/obb-zip64-test.logcat, from tools/android-reference.sh --app
# obb-zip64-test, where the file is pushed in full).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-aapt2.sh and
# tools/build-framework-res.sh first.
# Usage: tools/obb-zip64-check.sh
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
tools/build-java-test-app.sh obb-zip64-test >/dev/null

obb=$(tools/make-sparse-zip64-obb.py build/obb-zip64-obb)
trap 'rm -rf build/obb-zip64-obb' EXIT
python3 framework/tests/java_app_run.py --obb "$obb" \
  obb-zip64-test io.github.kisame76.minato.test.obbzip64 io.github.kisame76.minato.test.obbzip64.ZipApplication \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/obb-zip64-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/obb-zip64-test/obb-zip64-test.apk fixtures/expected/obb-zip64-test.logcat
