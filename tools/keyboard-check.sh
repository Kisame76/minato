# Runs ADR-0068 step 7c2's test of a hardware keyboard on the Mac (framework/tests/java_app_run.py): builds apk-run and
# apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental),
# framework.jar (about 10 s) and keyboard-test (seconds) every time, so it never tests an old binary; then starts
# keyboard-test through app_process in a session on the M4 plan's display, the iPhone 15 Pro Max's 1290 x 2796 px at
# scale 3 in en-US. The host adds a keyboard to the input devices (apk-run's device:add), types h, i and DEL on it
# (hwkey: steps) and removes it (device:remove); the log is compared with fixtures/expected/keyboard-test.logcat, which
# is Minato's own: an Android device has keyboards of its own, with their own maps, so there is no AVD reference, and
# the keyboard's map is Generic.kcm's, AOSP's file parsed by AOSP's parser.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/keyboard-check.sh
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
tools/build-java-test-app.sh keyboard-test >/dev/null

python3 framework/tests/java_app_run.py --lifecycle start,device:add,hwkey:36,hwkey:37,hwkey:67,device:remove \
  keyboard-test io.github.kisame76.minato.test.keyboard io.github.kisame76.minato.test.keyboard.KeyboardApplication \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/keyboard-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/keyboard-test/keyboard-test.apk fixtures/expected/keyboard-test.logcat 1290x2796@3,en-US
