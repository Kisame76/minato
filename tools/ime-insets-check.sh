#!/usr/bin/env bash
# Runs ADR-0068 step 2d's test of the keyboard's insets on the Mac (framework/tests/java_app_run.py), for the two apps
# ime-insets-test (target 35) and ime-insets-test-34 (target 34): it builds apk-run and apk-convert for macOS into
# build/native/macos-app, app_process with libandroid_runtime.so (incremental), framework.jar and the apps every time,
# then starts each app through app_process in a session on the M4 plan's display, the iPhone 15 Pro Max's 1290 x 2796 px
# at scale 3 in en-US. The host plays the keyboard that the guest's IME requests announce (apk-run): a show makes the
# IME inset of its fixed keyboard height, a hide or stop takes it away. The activities of both apps (adjustResize,
# adjustPan and unspecified, one after the other) log whether the ime inset arrives, whether the content is resized
# (target 34, adjustResize only) and whether the field is clear of the keyboard, which is compared with
# fixtures/expected/ime-insets-test{,-34}.logcat, Android 15's with the fixture input method
# (tools/android-reference.sh).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/ime-insets-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."
# The display: MINATO_DISPLAY, by default the M4 plan's (tools/check-display.sh).
source tools/check-display.sh

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null

for app in ime-insets-test ime-insets-test-34; do
  tools/build-java-test-app.sh "$app" >/dev/null
  python3 framework/tests/java_app_run.py \
    --lifecycle start \
    "$app" io.github.kisame76.minato.test.imeinsets android.app.Application \
    build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
    "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" "build/$app-check" \
    build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
    "build/$app/$app.apk" "$(expected_at_display "$app" logcat)" "$CHECK_DISPLAY,en-US"
done
