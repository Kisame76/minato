#!/bin/bash
# What an activity hears when it asks for another orientation (setRequestedOrientation), on the Mac: the app asks for
# landscape and then for portrait by itself, apk-run turns its window as the App turns the iPhone's (it answers
# APKRT_REQUEST_ORIENTATION), and the log must be what Android 15 logs (fixtures/expected/orientation-request-
# test.logcat, from tools/android-reference.sh --app orientation-request-test).
# Usage: tools/orientation-request-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null

app=orientation-request-test
tools/build-java-test-app.sh "$app" >/dev/null
# The app asks for the orientations by itself, half a second after its focus; the pause keeps the session open.
python3 framework/tests/java_app_run.py \
  --lifecycle start,pause:20000 --until "config 4 done" \
  "$app" io.github.kisame76.minato.test.orientationrequest android.app.Application \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" "build/$app-check" \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  "build/$app/$app.apk" "fixtures/expected/$app.logcat" 1290x2796@3,en-US
