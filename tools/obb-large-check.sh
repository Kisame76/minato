#!/usr/bin/env bash
# Runs obb-large-test on the Mac (framework/tests/java_app_run.py): an app reads a main OBB of 2 GiB and 128 MiB at
# offsets beyond 2^31 (RandomAccessFile, FileInputStream.skip, a positional FileChannel read, mappings across and above
# 2 GiB). The OBB is sparse, made by tools/make-sparse-obb.py into the check's work folder and removed afterwards; its
# log must be Android 15's (fixtures/expected/obb-large-test.logcat, from tools/android-reference.sh --app
# obb-large-test, where the file is pushed in full).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-aapt2.sh and
# tools/build-framework-res.sh first.
# Usage: tools/obb-large-check.sh
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
tools/build-java-test-app.sh obb-large-test >/dev/null

obb=$(tools/make-sparse-obb.py build/obb-large-obb)
trap 'rm -rf build/obb-large-obb' EXIT
python3 framework/tests/java_app_run.py --obb "$obb" \
  obb-large-test io.github.kisame76.minato.test.obblarge io.github.kisame76.minato.test.obblarge.LargeApplication \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/obb-large-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/obb-large-test/obb-large-test.apk fixtures/expected/obb-large-test.logcat
