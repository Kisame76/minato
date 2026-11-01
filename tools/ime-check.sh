#!/usr/bin/env bash
# Runs ADR-0068 step 2b's test of the soft keyboard's bridge on the Mac (framework/tests/java_app_run.py): builds
# apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and ime-test (seconds) every time, so it never tests an old binary; then
# starts ime-test through app_process in a session on the M4 plan's display, the iPhone 15 Pro Max's 1290 x 2796 px at
# scale 3 in en-US. The host plays the keyboard that the guest's IME requests announce (apk-run's text: steps): a tap
# on the field, then h and he as composing text, hello as committed text, a commit for the connection before (the
# guest drops it), a delete of one character, the Go action and the DEL key. The log is compared with
# fixtures/expected/ime-test.logcat, which is the Android 15 emulator's with the fixture input method making the same
# calls (tools/android-reference.sh), and the host must have heard of the editor, of the show at the focus, and of the
# restart with the show that a tap makes (the lines of apk-run's stderr).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/ime-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."
# The display: MINATO_DISPLAY, by default the M4 plan's (tools/check-display.sh).
source tools/check-display.sh

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh ime-test >/dev/null

steps=start,tap:645:1400,text:1:1:0:h,text:1:1:0:he,text:0:1:0:hello,stext:0:1:0:XX
steps+=,text:3:1:0:,text:6:2:0:,text:7:67:0:
python3 framework/tests/java_app_run.py \
  --in-order 'apk-run: ime 1: start\t1\t268435458\t0\t0\t0\t-1\t0\t0\thint' \
  --in-order 'apk-run: ime 1: show' --in-order 'apk-run: ime 1: stop' --in-order 'apk-run: ime 2: start' \
  --in-order 'apk-run: ime 2: show' --in-order 'apk-run: ime 2: hide' \
  --lifecycle "$steps" \
  ime-test io.github.kisame76.minato.test.ime android.app.Application \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/ime-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/ime-test/ime-test.apk "$(expected_at_display ime-test logcat)" "$CHECK_DISPLAY,en-US"
