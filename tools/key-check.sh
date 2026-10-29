#!/usr/bin/env bash
# Runs ADR-0045 step 7c's test of typed keys on the Mac (framework/tests/java_app_run.py): builds apk-run and
# apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental),
# framework.jar (about 10 s) and key-test (seconds) every time, so it never tests an old binary; then starts key-test
# through app_process in a session on the M4 plan's display, the iPhone 15 Pro Max's 1290 x 2796 px at scale 3 in
# en-US, and compares its log with Android 15's on that display without an input method
# (fixtures/expected/key-test.logcat, from tools/android-reference.sh --app key-test):
#   - the virtual keyboard's key character map: what its keys type, a dead key, numbers, labels, matches, the events
#     that type a string and a parcel round trip (7c1);
#   - KEYCODE_1, KEYCODE_A and KEYCODE_DEL injected into the window, into an EditText with a DigitsKeyListener, which
#     takes the digit, refuses the letter and deletes, and the touch mode, which the digit leaves (7c1);
#   - KEYCODE_2, KEYCODE_B and KEYCODE_DEL sent by the host through the session's input channel, which type and
#     refuse the same way (7c2).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/key-check.sh
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
tools/build-java-test-app.sh key-test >/dev/null

python3 framework/tests/java_app_run.py --lifecycle start,key:9,key:30,key:67 key-test \
  io.github.kisame76.minato.test.key \
  io.github.kisame76.minato.test.key.KeyApplication build/art build/bionic/lib build/bionic/system build/hwui/lib \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/key-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/key-test/key-test.apk fixtures/expected/key-test.logcat 1290x2796@3,en-US
