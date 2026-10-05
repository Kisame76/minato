#!/usr/bin/env bash
# Runs ADR-0043 d2's tests of android.os.Parcel on the Mac (framework/tests/java_app_run.py): builds apk-run and
# apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental),
# framework.jar (about 10 s) and the two apps (seconds) every time, so it never tests an old binary; then
#   - parcel-test against Android 15's log (fixtures/expected/parcel-test.logcat, from tools/android-reference.sh --app
#     parcel-test): per case the bytes marshall() gives and what reading them back gives, for primitives, strings,
#     arrays, blobs, the interface token, exceptions, a Bundle and styled text, appendFrom, reads past the end and over
#     truncated or garbage bytes, and binders and file descriptors with their ownership;
#   - parcel-forge-test against Minato's own answers (fixtures/expected/parcel-forge-test.txt), for the cases where
#     Android's libbinder crashes or reaches past its buffer.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/parcel-check.sh
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
# <app> <package> <application class> <expected>
check() {
  local app=$1 package=$2 application=$3 expected=$4
  tools/build-java-test-app.sh "$app" >/dev/null
  python3 framework/tests/java_app_run.py "$app" "$package" "$application" build/art build/bionic/lib \
    build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
    "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" "build/$app-check" \
    build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
    "build/$app/$app.apk" "$expected" || failed=1
}
check parcel-test io.github.kisame76.minato.test.parcel io.github.kisame76.minato.test.parcel.ParcelApplication \
  fixtures/expected/parcel-test.logcat
check parcel-forge-test io.github.kisame76.minato.test.parcelforge \
  io.github.kisame76.minato.test.parcelforge.ParcelForgeApplication fixtures/expected/parcel-forge-test.txt
exit "$failed"
