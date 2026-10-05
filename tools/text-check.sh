#!/usr/bin/env bash
# Runs ADR-0043 step d)'s test on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS
# into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and
# text-test (seconds) every time, so it never tests an old binary; then starts text-test through app_process in a
# session on the M4 plan's display, the iPhone 15 Pro Max's 1290 x 2796 px at scale 3 in en-US, and compares its log
# with Android 15's on that display without an input method (fixtures/expected/text-test.logcat, from
# tools/android-reference.sh --app text-test): an EditText made as Godot makes its own, beside a TextView, its
# defaults, Godot's setup with a length filter and DigitsKeyListener, each TextWatcher call, both views after the
# global layout, typing through an InputConnection as an input method types, and the input method manager's answers.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/text-check.sh
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
tools/build-java-test-app.sh text-test >/dev/null

python3 framework/tests/java_app_run.py text-test io.github.kisame76.minato.test.text android.app.Application \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/text-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/text-test/text-test.apk fixtures/expected/text-test.logcat 1290x2796@3,en-US
