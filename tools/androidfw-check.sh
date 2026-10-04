#!/usr/bin/env bash
# Runs ADR-0039 b3.2's test on the Mac (framework/tests/androidfw_run.py): builds apk-run and apk-convert for macOS
# into build/native/macos-app and the framework's guest code if needed, links tools/aapt2/check/res against
# framework-res with the host aapt2 into build/androidfw-check/app.apk, then lets androidfw-test read framework-res
# (folder and APK) and the app in the guest and compares its lines with fixtures/expected/androidfw-test.txt.
# Needs tools/build-aapt2.sh, tools/build-framework-res.sh, tools/build-bionic.sh and tools/build-hwui.sh first.
# Usage: tools/androidfw-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# The framework's guest code links libandroid_runtime with libhwui (ADR-0043 test 2): a shared build, which this
# check does not make.
if [[ ! -e build/hwui/lib/libhwui.so ]]; then
  echo "skipped: build/hwui/lib/libhwui.so is not built (tools/build-hwui.sh)"
  exit 0
fi

NATIVE=build/native/macos-app
OUT=build/androidfw-check
AAPT2=build/aapt2/aapt2
FRAMEWORK_RES=build/framework-res
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh | tail -1

mkdir -p "$OUT"
"$AAPT2" compile --dir tools/aapt2/check/res -o "$OUT/compiled.zip"
"$AAPT2" link -o "$OUT/app.apk" -I "$FRAMEWORK_RES/framework-res.apk" \
  -A tools/aapt2/check/assets --manifest tools/aapt2/check/AndroidManifest.xml "$OUT/compiled.zip"

python3 framework/tests/androidfw_run.py "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" \
  build/bionic/lib build/bionic/system "$OUT" build/framework-native/androidfw-test \
  "$FRAMEWORK_RES/framework-res" "$FRAMEWORK_RES/framework-res.apk" "$OUT/app.apk" \
  fixtures/expected/androidfw-test.txt
