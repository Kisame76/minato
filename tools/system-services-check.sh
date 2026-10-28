#!/usr/bin/env bash
# Runs gap 1's batch 3 (the compatibility plan: system services that answered null) on the Mac
# (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS into build/native/macos-app, and
# app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and system-services-test (seconds)
# every time, so it never tests an old binary; then starts the app's activity through app_process and compares its log
# with Android 15's on apk-runner-35 (fixtures/expected/system-services-test.logcat, from tools/android-reference.sh
# --app system-services-test): the vibrator and its manager, a vibration and a view's haptic feedback; what differs by
# device is shown only. Then its twin system-services-test-unpermitted, the same sources without the VIBRATE
# permission, whose vibration and cancel the vibrator service refuses (fixtures/expected/<app>.logcat each).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/system-services-check.sh
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
for app in system-services-test system-services-test-unpermitted; do
  tools/build-java-test-app.sh "$app" >/dev/null
  package=$(sed -n 's/.*package="\([^"]*\)".*/\1/p' "fixtures/apps/$app/AndroidManifest.xml" | head -n 1)
  python3 framework/tests/java_app_run.py --lifecycle start "$app" "$package" \
    android.app.Application build/art build/bionic/lib build/bionic/system \
    build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
    "$NATIVE/runtime/apk-run" "build/system-services-check/$app" build/framework-native/app_process64 \
    build/framework/framework.jar build/framework-res/framework-res \
    "build/$app/$app.apk" "fixtures/expected/$app.logcat" 1290x2796@3,en-US || failed=1
done
exit "$failed"
