#!/usr/bin/env bash
# Runs the keyboard-hide test (ime-hide-test and its twin ime-hide-test-cb; ADR-0068, the keyboard reopen bug) on the
# Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS into build/native/macos-app,
# app_process with libandroid_runtime.so (incremental), framework.jar and the apps every time, then starts each app
# through app_process in a session on the M4 plan's display. The host plays what it does for the keyboard (apk-run's
# model: the inset follows a show and a hide; the step imehide is the user's hide, as the keyboard bar's key makes it):
# a tap on the first field, the user's hide, a tap on the first field again, the hide, and the same with the second.
# Each log is compared with Android 15's (fixtures/expected/ime-hide-test.logcat and ime-hide-test-cb.logcat, from
# tools/android-reference.sh): an app that has not opted in sees BACK in onKeyPreIme, one that has sees none, and both
# see the inset go.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/ime-hide-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null

steps=start,tap:645:250,imehide,tap:645:250,imehide,tap:645:550,imehide,tap:645:550
for app in ime-hide-test ime-hide-test-cb; do
  # The opted-in twin never sees BACK in onKeyPreIme, so its keyboard-less BACKs of the reference script (the second one
  # onward) end its activity on Android: the Mac plays only the first hide for it.
  [[ "$app" == ime-hide-test-cb ]] && steps=start,tap:645:250,imehide,tap:645:250
  tools/build-java-test-app.sh "$app" >/dev/null
  python3 framework/tests/java_app_run.py \
    --lifecycle "$steps" \
    "$app" io.github.kisame76.minato.test.imehide android.app.Application \
    build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
    "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" "build/$app-check" \
    build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
    "build/$app/$app.apk" "fixtures/expected/$app.logcat" 1290x2796@3,en-US
done
