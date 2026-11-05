#!/usr/bin/env bash
# Runs the free-space test of the app's storage folders on the Mac (framework/tests/java_app_run.py): builds apk-run
# and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental),
# framework.jar (about 10 s) and storage-space-test (seconds) every time, so it never tests an old binary; then starts
# storage-space-test through app_process and compares its normalized relation lines with Android 15's on apk-runner-35
# (fixtures/expected/storage-space-test.logcat, from tools/android-reference.sh --app storage-space-test): for every
# folder a resource extractor checks, StatFs, java.io.File, StorageManager.getAllocatableBytes, StorageStatsManager and
# the raw statvfs all answer as on Android (a stub returning 0 allocatable bytes made Unity 6 report "Not enough storage
# space", ADR-0045). The byte counts differ by device, so they log under the -device tag and are only shown.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/storage-space-check.sh
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
tools/build-java-test-app.sh storage-space-test >/dev/null

python3 framework/tests/java_app_run.py storage-space-test io.github.kisame76.minato.test.storagespace \
  io.github.kisame76.minato.test.storagespace.StorageSpaceApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/storage-space-check build/framework-native/app_process64 \
  build/framework/framework.jar \
  build/framework-res/framework-res build/storage-space-test/storage-space-test.apk \
  fixtures/expected/storage-space-test.logcat
