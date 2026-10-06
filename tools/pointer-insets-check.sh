#!/usr/bin/env bash
# Runs ADR-0042 b's test of gaps 6 and 9's last classes, PointerIcon and WindowInsetsAnimation, on the Mac
# (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS into build/native/macos-app, and
# app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and pointer-insets-test (seconds)
# every time, so it never tests an old binary; then starts pointer-insets-test through app_process. Without a window it
# asks PointerIcon for system and custom icons, loads its own <pointer-icon>, and drives a WindowInsetsAnimation, its
# Bounds and a Callback, as Godot's views do; its log must be Android 15's
# (fixtures/expected/pointer-insets-test.logcat, from tools/android-reference.sh --app pointer-insets-test).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-aapt2.sh and
# tools/build-framework-res.sh first.
# Usage: tools/pointer-insets-check.sh
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
tools/build-java-test-app.sh pointer-insets-test >/dev/null

python3 framework/tests/java_app_run.py pointer-insets-test io.github.kisame76.minato.test.pointerinsets \
  io.github.kisame76.minato.test.pointerinsets.PointerInsetsApplication build/art build/bionic/lib \
  build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/pointer-insets-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/pointer-insets-test/pointer-insets-test.apk fixtures/expected/pointer-insets-test.logcat
