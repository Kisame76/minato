#!/usr/bin/env bash
# Runs the system's back test (ADR-0041 decision 10) on the Mac (framework/tests/java_app_run.py), for back-test (the
# legacy back) and back-test-callback (opted in to OnBackInvokedCallback): it builds apk-run and apk-convert for macOS
# into build/native/macos-app, app_process with libandroid_runtime.so (incremental), framework.jar and the apps every
# time, then starts each app through app_process in a session on the M4 plan's display, the iPhone 15 Pro Max's
# 1290 x 2796 px at scale 3 in en-US. The host sends the system's back without a gesture (apk-run's back step):
# three for back-test, which pop Second's fragment, finish Second and reach Root, whose back the host must hear as
# BACK_AT_ROOT ("apk-run: back at root"), where Android moves the task to the back and Minato opens its menu, and four
# for back-test-callback, whose third goes to Root's own callback and its fourth to Root's default one. Each app's log
# is compared with fixtures/expected/<app>.logcat up to "done", Android 15's for `input keyevent KEYCODE_BACK`
# (tools/android-reference.sh).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/back-check.sh [app...]   (both apps by default)
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null

apps=("$@")
[[ ${#apps[@]} -gt 0 ]] || apps=(back-test back-test-callback)
for app in "${apps[@]}"; do
  tools/build-java-test-app.sh "$app" >/dev/null
  steps=start,back,back,back
  [[ "$app" != back-test-callback ]] || steps+=,back
  python3 framework/tests/java_app_run.py \
    --in-order 'I/'"$app"': done' --in-order 'apk-run: back at root' \
    --lifecycle "$steps" \
    "$app" io.github.kisame76.minato.test.back android.app.Application \
    build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
    "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" "build/$app-check" \
    build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
    "build/$app/$app.apk" "fixtures/expected/$app.logcat" 1290x2796@3,en-US
done
