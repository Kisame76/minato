#!/usr/bin/env bash
# Runs ADR-0045's test of the settings Godot's start reads on the Mac (framework/tests/java_app_run.py):
# builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and settings-test (seconds) every time, so it never tests an old binary;
# then starts settings-test through app_process and compares its log with Android 15's on apk-runner-35, wiped
# (fixtures/expected/settings-test.logcat, from tools/android-reference.sh --app settings-test): the app's
# ContentResolver, ViewConfiguration's four settings, values apps read, refused reads and writes, the provider's
# queries, a content observer and text to speech without an engine. The app starts with ActivityThread's
# --android-id, as the host gives it, and its device line must show that value.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/settings-check.sh
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
tools/build-java-test-app.sh settings-test >/dev/null

ANDROID_ID=0123456789abcdef
out=$(python3 framework/tests/java_app_run.py --android-id "$ANDROID_ID" settings-test \
  io.github.kisame76.minato.test.settings \
  io.github.kisame76.minato.test.settings.SettingsApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/settings-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/settings-test/settings-test.apk fixtures/expected/settings-test.logcat) || {
  echo "$out"; exit 1; }
echo "$out"
if [[ "$out" != *skipped:* && "$out" != *"device: I/settings-test-device: android_id $ANDROID_ID"* ]]; then
  echo "FAIL: android_id is not --android-id's $ANDROID_ID"
  exit 1
fi
