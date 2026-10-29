#!/usr/bin/env bash
# Runs ADR-0069 step 1's test on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS
# into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s)
# and native-jni-ime-test (seconds) every time, so it never tests an old binary; then starts native-jni-ime-test, a
# NativeActivity, through app_process in a session on the M4 plan's display, the iPhone 15 Pro Max's 1290 x 2796 px at
# scale 3 in en-US, as the Java path of ADR-0069 starts every NativeActivity app. The app opens the soft keyboard
# through JNI from its activity object (fixtures/java/native-jni-ime-test); the host plays the keyboard as apk-run's
# text: steps, the calls the Android reference's fixture input method makes (commits of h, H, hi and é, a delete of
# one character, the DEL and ENTER keys, a commit of a new line and the Go action), then B and DEL from its hardware
# keyboard. The log is compared with fixtures/expected/native-jni-ime-test.logcat, the Android 15 emulator's
# (tools/android-reference.sh --app native-jni-ime-test), up to the keyboard's show since ADR-0069 step 2: the keys
# reach the app through the input queue of step 3. Its window must be drawn: only the app draws it, by the CPU on the
# ANativeWindow of the surface that NativeActivity takes, as the decor view draws nothing there.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/native-activity-check.sh
# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh native-jni-ime-test >/dev/null

steps=start,text:0:1:0:h,text:0:1:0:H,text:0:1:0:hi,text:0:1:0:é,text:3:1:0:,text:7:67:0:,text:7:66:0:
steps+=,text:0:1:0:$'\n',text:6:2:0:,hwkey:30,hwkey:67
# ADR-0069 step 2: up to the keyboard's show.
until='showSoftInput(decorView, SHOW_FORCED) again: 1'
python3 framework/tests/java_app_run.py --drawn --lifecycle "$steps" --until "$until" \
  native-jni-ime-test io.github.kisame76.minato.test.nativejniime android.app.Application \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/native-activity-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/native-jni-ime-test/native-jni-ime-test.apk fixtures/expected/native-jni-ime-test.logcat 1290x2796@3,en-US
