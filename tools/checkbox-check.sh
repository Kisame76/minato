#!/usr/bin/env bash
# Runs ADR-0043 step e1)'s test of CompoundButton and CheckBox on the Mac (framework/tests/java_app_run.py): builds
# apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and checkbox-test (seconds) every time, so it never tests an old binary;
# then starts checkbox-test through app_process. Without a window it inflates a layout's CheckBoxes and makes others
# in code, and logs their state, button drawable, place and size, listeners, saved state and pixels drawn on the CPU;
# its log must be Android 15's (fixtures/expected/checkbox-test.logcat, from tools/android-reference.sh --app
# checkbox-test).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-aapt2.sh and
# tools/build-framework-res.sh first.
# Usage: tools/checkbox-check.sh
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
tools/build-java-test-app.sh checkbox-test >/dev/null

python3 framework/tests/java_app_run.py checkbox-test io.github.kisame76.minato.test.checkbox \
  io.github.kisame76.minato.test.checkbox.CheckBoxApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/checkbox-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/checkbox-test/checkbox-test.apk \
  fixtures/expected/checkbox-test.logcat
