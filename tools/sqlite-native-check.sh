#!/usr/bin/env bash
# Runs the guest's libsqlite.so test on the Mac (framework/tests/sqlite_native_run.py):
# builds apk-run and apk-convert for macOS into build/native/macos-app and the framework's guest code, libsqlite.so and
# sqlite-native-test among it, then lets sqlite-native-test open the library in the guest and compares its lines with
# fixtures/expected/sqlite-native-test.txt, Android 15's: the same program's on apk-runner-35, where it opens Android's
# own /system/lib64/libsqlite.so.
# Needs tools/build-bionic.sh, tools/build-art.sh (ICU's libraries with libandroidicu) and tools/build-hwui.sh first.
# Usage: tools/sqlite-native-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# The framework's guest code links libandroid_runtime with libhwui (ADR-0043 test 2): a shared build, which this
# check does not make.
if [[ ! -e build/hwui/lib/libhwui.so ]]; then
  echo "skipped: build/hwui/lib/libhwui.so is not built (tools/build-hwui.sh)"
  exit 0
fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh | tail -1

python3 framework/tests/sqlite_native_run.py "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" \
  build/bionic/lib build/bionic/system build/art build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  build/sqlite-native-check build/framework-native/sqlite-native-test build/framework-native/libsqlite.so \
  fixtures/expected/sqlite-native-test.txt
