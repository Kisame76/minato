#!/usr/bin/env bash
# Runs ADR-0039 step c2e's compat test on the Mac (framework/tests/java_app_run.py) for both of its builds:
# compat-test, for targetSdk 28, and compat-test-35, the same sources for 35. Builds apk-run and apk-convert for macOS
# into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and
# the apps (seconds) every time, so it never tests an old binary; then starts each app through app_process and
# compares its log with Android 15's (fixtures/expected/<app>.logcat, from tools/android-reference.sh --app <app>):
# String.split, UUID.fromString, SharedPreferences' clear(), SparseIntArray past its end and a zip entry that leaves
# its folder, each on its side of the app's target SDK. Then the line each logs with its device tag must be the
# platform compat service's refusal, Android 15's answer to an app (PermissionEnforcer's message for the permissions of
# IPlatformCompat.aidl), which the AVD cannot show: its hidden API policy stops the app's reflection first.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/compat-check.sh
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

permissions="android.permission.LOG_COMPAT_CHANGE, android.permission.READ_COMPAT_CHANGE_CONFIG"
refusal="platform_compat: java.lang.SecurityException: Access denied, requires: allOf={$permissions}"
for app in compat-test compat-test-35; do
  tools/build-java-test-app.sh "$app" >/dev/null
  package=$(sed -n 's/.*package="\([^"]*\)".*/\1/p' "fixtures/apps/$app/AndroidManifest.xml" | head -n 1)
  if ! output=$(python3 framework/tests/java_app_run.py "$app" "$package" \
      io.github.kisame76.minato.test.compat.CompatApplication build/art build/bionic/lib build/bionic/system \
      build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
      "$NATIVE/runtime/apk-run" "build/$app-check" build/framework-native/app_process64 build/framework/framework.jar \
      build/framework-res/framework-res "build/$app/$app.apk" "fixtures/expected/$app.logcat"); then
    echo "$output"
    exit 1
  fi
  echo "$output"
  if [[ "$output" == skipped:* ]]; then
    exit 0
  fi
  if ! grep -qxF "  device: I/$app-device: $refusal" <<<"$output"; then
    echo "FAIL: $app's device line is not Android's refusal: I/$app-device: $refusal"
    exit 1
  fi
done
