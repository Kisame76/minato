#!/usr/bin/env bash
# Runs ADR-0069 step 3's test of the IMM's fallback input connection on the Mac (framework/tests/java_app_run.py), as
# tools/ime-check.sh does for ime-test: ime-fallback-test's focused view makes no input connection, and the host, which
# plays the keyboard that apk-run's text: steps stand for, commits h, H, hi and é, deletes a character, sends DEL and
# ENTER as keys and the Go action. The view must hear the key events of native-jni-ime-test's reference on the
# Android 15 emulator (fixtures/expected/native-jni-ime-test.logcat, its lines for the same calls), which
# tools/android-reference.sh --app ime-fallback-test confirms for this app's own fixture input method.
# The keys sent as keys carry 0x6 (FLAG_SOFT_KEYBOARD | FLAG_KEEP_TOUCH_MODE), as a real keyboard's do and as this
# app's fixture input method makes them. The host must have been told to start a keyboard with no editor
# (inputType 0, selection -1, fieldId 0).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/ime-fallback-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh ime-fallback-test >/dev/null

steps=start,text:0:1:0:h,text:0:1:0:H,text:0:1:0:hi,text:0:1:0:é,text:3:1:0:
steps+=,text:7:67:0:,text:7:66:0:,text:6:2:0:
python3 framework/tests/java_app_run.py \
  --in-order 'apk-run: ime 1: start\t0\t0\t0\t-1\t-1\t0\t0\t0\t' \
  --in-order 'apk-run: ime 1: show' \
  --lifecycle "$steps" \
  ime-fallback-test io.github.kisame76.minato.test.imefallback android.app.Application \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/ime-fallback-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/ime-fallback-test/ime-fallback-test.apk fixtures/expected/ime-fallback-test.logcat 1290x2796@3,en-US
