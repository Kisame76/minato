#!/usr/bin/env bash
# Runs the test of framework breadth batch F4d, f4d-cells-test, on the Mac (framework/tests/java_app_run.py):
# builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and f4d-cells-test (seconds) every time, so it never tests an old
# binary; then starts f4d-cells-test through app_process and compares its log with Android 15's on apk-runner-35
# (fixtures/expected/f4d-cells-test.logcat, from tools/android-reference.sh --app f4d-cells-test): the telephony cell
# family, with cells of every radio technology made through CellInfo's CREATOR from parcels in AOSP's layout and read
# back, the answer of getAllCellInfo to an app without the location permission, and the small data types around them.
# Its twin f4d-cells-test-fine (the same sources, requesting the fine location permission, which the host grants with
# --grant-location fine) gets getAllCellInfo's list instead of the SecurityException (ADR-0063 decision 9).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/f4d-cells-check.sh
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
failed=0
# <app> <package> <the grant for the host, none for no --grant-location>
for twin in "f4d-cells-test io.github.kisame76.minato.test.cells none" \
  "f4d-cells-test-fine io.github.kisame76.minato.test.cells.fine fine"; do
  read -r app package grant <<<"$twin"
  tools/build-java-test-app.sh "$app" >/dev/null
  grant_args=()
  if [[ "$grant" != none ]]; then grant_args=(--grant-location "$grant"); fi
  python3 framework/tests/java_app_run.py ${grant_args[@]+"${grant_args[@]}"} \
    "$app" "$package" \
    android.app.Application build/art build/bionic/lib build/bionic/system \
    build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
    "$NATIVE/runtime/apk-run" "build/f4d-cells-check-$app" build/framework-native/app_process64 \
    build/framework/framework.jar build/framework-res/framework-res \
    "build/$app/$app.apk" "fixtures/expected/$app.logcat" 1290x2796@3,en-US || failed=1
done
exit "$failed"
