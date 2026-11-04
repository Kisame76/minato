#!/usr/bin/env bash
# Runs sdk-extensions-test (android.os.ext.SdkExtensions) on the Mac (framework/tests/java_app_run.py): builds apk-run
# and apk-convert for macOS into build/native/macos-app, app_process with libandroid_runtime.so (incremental),
# framework.jar and the app every time, then starts the app's activity through app_process and compares its log with
# Android 15's (fixtures/expected/sdk-extensions-test.logcat, from tools/android-reference.sh): the version of each
# extension, the map of all of them, and the errors for a number below 30. Needs tools/build-art.sh,
# tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/sdk-extensions-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null

app=sdk-extensions-test
tools/build-java-test-app.sh "$app" >/dev/null
python3 framework/tests/java_app_run.py \
  --lifecycle start "$app" io.github.kisame76.minato.test.sdkextensions android.app.Application \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" "build/$app-check" \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  "build/$app/$app.apk" "fixtures/expected/$app.logcat" 1290x2796@3,en-US
